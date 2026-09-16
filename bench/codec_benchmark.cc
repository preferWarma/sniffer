#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/config.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "benchmark_stats.h"
#include "codec_internal.h"

namespace {

struct Options {
  int64_t rows = 100000;
  int iterations = 7;
  bool output_json = false;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto parse = [&argument](std::string_view prefix, auto* destination) {
      if (argument.starts_with(prefix)) {
        using Value = std::remove_reference_t<decltype(*destination)>;
        *destination = static_cast<Value>(std::stoll(argument.substr(prefix.size())));
        return true;
      }
      return false;
    };
    if (parse("--rows=", &options.rows)) {
      continue;
    }
    if (parse("--iterations=", &options.iterations)) {
      continue;
    }
    if (argument == "--output-format=json") {
      options.output_json = true;
      continue;
    }
  }
  options.rows = std::max<int64_t>(1, options.rows);
  options.iterations = std::max(1, options.iterations);
  return options;
}

struct Scenario {
  std::string name;
  std::string encoding_name;
  sniffer::FieldSpec field;
  std::shared_ptr<arrow::Array> array;
  uint16_t encoding_id = 0;
};

template <typename Generator>
arrow::Result<Scenario> MakeInt64Scenario(std::string name, std::string encoding_name,
                                          uint16_t encoding_id, int64_t rows, bool nullable,
                                          Generator&& generator) {
  arrow::Int64Builder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(rows));
  for (int64_t row = 0; row < rows; ++row) {
    const std::optional<int64_t> value = generator(row);
    if (value) {
      ARROW_RETURN_NOT_OK(builder.Append(*value));
    } else {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> array;
  ARROW_RETURN_NOT_OK(builder.Finish(&array));
  ARROW_RETURN_NOT_OK(array->ValidateFull());
  return Scenario{std::move(name),
                  std::move(encoding_name),
                  {1, "value", arrow::int64(), nullable, nullptr},
                  std::move(array),
                  encoding_id};
}

template <typename Generator>
arrow::Result<Scenario> MakeStringScenario(std::string name, std::string encoding_name,
                                           uint16_t encoding_id, int64_t rows, bool nullable,
                                           Generator&& generator) {
  arrow::StringBuilder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(rows));
  for (int64_t row = 0; row < rows; ++row) {
    const std::optional<std::string> value = generator(row);
    if (value) {
      ARROW_RETURN_NOT_OK(builder.Append(*value));
    } else {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> array;
  ARROW_RETURN_NOT_OK(builder.Finish(&array));
  ARROW_RETURN_NOT_OK(array->ValidateFull());
  return Scenario{std::move(name),
                  std::move(encoding_name),
                  {1, "value", arrow::utf8(), nullable, nullptr},
                  std::move(array),
                  encoding_id};
}

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

arrow::Result<std::vector<Scenario>> MakeScenarios(int64_t rows) {
  std::vector<Scenario> scenarios;
  scenarios.reserve(4);
  ARROW_ASSIGN_OR_RAISE(
      auto plain,
      MakeInt64Scenario("plain_random_int64", "plain", sniffer::internal::kPlainEncodingId, rows,
                        true, [](int64_t row) {
                          if (row % 17 == 0) {
                            return std::optional<int64_t>();
                          }
                          return std::optional<int64_t>(
                              static_cast<int64_t>(Mix(static_cast<uint64_t>(row))));
                        }));
  scenarios.push_back(std::move(plain));
  ARROW_ASSIGN_OR_RAISE(
      auto dictionary,
      MakeStringScenario("dictionary_string_32", "dictionary",
                         sniffer::internal::kDictionaryEncodingId, rows, true, [](int64_t row) {
                           return row % 17 == 0 ? std::optional<std::string>()
                                                : std::optional<std::string>(
                                                      "group-" + std::to_string(row % 32));
                         }));
  scenarios.push_back(std::move(dictionary));
  const int64_t run_length = std::max<int64_t>(1, rows / 128);
  ARROW_ASSIGN_OR_RAISE(
      auto rle, MakeInt64Scenario("rle_128_runs", "rle", sniffer::internal::kRleEncodingId, rows,
                                  false, [run_length](int64_t row) {
                                    return std::optional<int64_t>(row / run_length);
                                  }));
  scenarios.push_back(std::move(rle));
  ARROW_ASSIGN_OR_RAISE(
      auto for_bitpack,
      MakeInt64Scenario("for_bitpack_128_range", "for_bitpack",
                        sniffer::internal::kForBitpackEncodingId, rows, true, [](int64_t row) {
                          return row % 13 == 0 ? std::optional<int64_t>()
                                               : std::optional<int64_t>(1000000 + row % 128);
                        }));
  scenarios.push_back(std::move(for_bitpack));
  return scenarios;
}

struct Measurement {
  uint64_t logical_bytes = 0;
  uint64_t encoded_bytes = 0;
  sniffer::benchmark::SampleStats encode_stats;
  sniffer::benchmark::SampleStats decode_stats;
};

arrow::Result<std::vector<uint8_t>> Encode(const Scenario& scenario) {
  if (scenario.encoding_id == sniffer::internal::kPlainEncodingId) {
    return sniffer::internal::EncodePlain(scenario.field, *scenario.array);
  }
  return sniffer::internal::EncodeNonPlain(scenario.encoding_id, scenario.field, *scenario.array);
}

arrow::Result<std::shared_ptr<arrow::Array>> Decode(const Scenario& scenario,
                                                    const sniffer::internal::ColumnChunkMeta& chunk,
                                                    std::span<const uint8_t> payload) {
  if (scenario.encoding_id == sniffer::internal::kPlainEncodingId) {
    return sniffer::internal::DecodePlain(scenario.field, chunk, payload);
  }
  return sniffer::internal::DecodeNonPlain(scenario.field, chunk, payload);
}

arrow::Result<Measurement> Measure(const Scenario& scenario, int iterations) {
  std::vector<double> encode_times;
  std::vector<double> decode_times;
  encode_times.reserve(static_cast<size_t>(iterations));
  decode_times.reserve(static_cast<size_t>(iterations));
  ARROW_ASSIGN_OR_RAISE(const auto physical_type,
                        sniffer::internal::PhysicalTypeFor(*scenario.field.type));
  sniffer::internal::ColumnChunkMeta chunk;
  chunk.field_id = scenario.field.field_id;
  chunk.physical_type = physical_type;
  chunk.encoding_id = scenario.encoding_id;
  chunk.row_count = static_cast<uint64_t>(scenario.array->length());
  chunk.null_count = static_cast<uint64_t>(scenario.array->null_count());
  uint64_t encoded_bytes = 0;
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto encode_start = std::chrono::steady_clock::now();
    ARROW_ASSIGN_OR_RAISE(auto payload, Encode(scenario));
    const auto encode_end = std::chrono::steady_clock::now();
    if (iteration != 0 && payload.size() != encoded_bytes) {
      return arrow::Status::Invalid("codec benchmark produced a non-deterministic payload size");
    }
    encoded_bytes = static_cast<uint64_t>(payload.size());
    chunk.length = encoded_bytes;

    const auto decode_start = std::chrono::steady_clock::now();
    ARROW_ASSIGN_OR_RAISE(auto decoded, Decode(scenario, chunk, payload));
    const auto decode_end = std::chrono::steady_clock::now();
    if (!decoded->Equals(*scenario.array)) {
      return arrow::Status::Invalid("codec benchmark round-trip mismatch for ", scenario.name);
    }
    encode_times.push_back(
        std::chrono::duration<double, std::milli>(encode_end - encode_start).count());
    decode_times.push_back(
        std::chrono::duration<double, std::milli>(decode_end - decode_start).count());
  }
  const int64_t logical_size = arrow::util::TotalBufferSize(*scenario.array);
  if (logical_size <= 0) {
    return arrow::Status::Invalid("codec benchmark logical size must be positive");
  }
  return Measurement{static_cast<uint64_t>(logical_size), encoded_bytes,
                     sniffer::benchmark::SummarizeSamples(std::move(encode_times)),
                     sniffer::benchmark::SummarizeSamples(std::move(decode_times))};
}

double LogicalMiBPerSecond(uint64_t logical_bytes, double milliseconds) {
  constexpr double kBytesPerMiB = 1024.0 * 1024.0;
  return static_cast<double>(logical_bytes) / kBytesPerMiB / (milliseconds / 1000.0);
}

void PrintTextResult(const Scenario& scenario, const Measurement& measurement) {
  std::cout << "scenario=" << scenario.name << " encoding=" << scenario.encoding_name
            << " encoding_id=" << scenario.encoding_id << " rows=" << scenario.array->length()
            << " logical_bytes=" << measurement.logical_bytes
            << " encoded_bytes=" << measurement.encoded_bytes << " compression_ratio="
            << static_cast<double>(measurement.logical_bytes) /
                   static_cast<double>(measurement.encoded_bytes)
            << " encode_ms=" << measurement.encode_stats.p50 << " encode_mib_per_second="
            << LogicalMiBPerSecond(measurement.logical_bytes, measurement.encode_stats.p50)
            << " decode_ms=" << measurement.decode_stats.p50 << " decode_mib_per_second="
            << LogicalMiBPerSecond(measurement.logical_bytes, measurement.decode_stats.p50);
  sniffer::benchmark::PrintSampleStats(std::cout, "encode", measurement.encode_stats);
  sniffer::benchmark::PrintSampleStats(std::cout, "decode", measurement.decode_stats);
  std::cout << '\n';
}

void PrintJsonResult(const Scenario& scenario, const Measurement& measurement) {
  std::cout << "    {\"scenario\":";
  sniffer::benchmark::PrintJsonString(std::cout, scenario.name);
  std::cout << ",\"encoding\":";
  sniffer::benchmark::PrintJsonString(std::cout, scenario.encoding_name);
  std::cout << ",\"encoding_id\":" << scenario.encoding_id
            << ",\"rows\":" << scenario.array->length()
            << ",\"logical_bytes\":" << measurement.logical_bytes
            << ",\"encoded_bytes\":" << measurement.encoded_bytes << ",\"compression_ratio\":"
            << static_cast<double>(measurement.logical_bytes) /
                   static_cast<double>(measurement.encoded_bytes)
            << ",\"encode_mib_per_second\":"
            << LogicalMiBPerSecond(measurement.logical_bytes, measurement.encode_stats.p50)
            << ",\"decode_mib_per_second\":"
            << LogicalMiBPerSecond(measurement.logical_bytes, measurement.decode_stats.p50)
            << ",\"encode_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.encode_stats);
  std::cout << ",\"decode_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.decode_stats);
  std::cout << '}';
}

arrow::Result<int> RunBenchmark(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  ARROW_ASSIGN_OR_RAISE(auto scenarios, MakeScenarios(options.rows));
  std::vector<Measurement> measurements;
  measurements.reserve(scenarios.size());
  for (const auto& scenario : scenarios) {
    ARROW_ASSIGN_OR_RAISE(auto measurement, Measure(scenario, options.iterations));
    measurements.push_back(std::move(measurement));
  }
#ifdef NDEBUG
  constexpr std::string_view kBuildMode = "release";
#else
  constexpr std::string_view kBuildMode = "debug";
#endif
  if (options.output_json) {
    std::cout << "{\n  \"benchmark\":\"codec\",\n  \"source_revision\":";
    sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_SOURCE_REVISION);
    std::cout << ",\n  \"compiler\":";
    sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_COMPILER);
    std::cout << ",\n  \"arrow_version\":";
    sniffer::benchmark::PrintJsonString(std::cout, ARROW_VERSION_STRING);
    std::cout << ",\n  \"build_mode\":";
    sniffer::benchmark::PrintJsonString(std::cout, kBuildMode);
    std::cout << ",\n  \"command_arguments\":";
    sniffer::benchmark::PrintJsonArguments(std::cout, argc, argv);
    std::cout << ",\n  \"configuration\":{\"rows\":" << options.rows
              << ",\"iterations\":" << options.iterations
              << ",\"execution\":\"single_thread\",\"scope\":"
                 "\"memory_only_no_io_index_or_checksum\",\"hardware_threads\":"
              << std::thread::hardware_concurrency() << "},\n  \"scenarios\":[\n";
    for (size_t index = 0; index < scenarios.size(); ++index) {
      if (index != 0) {
        std::cout << ",\n";
      }
      PrintJsonResult(scenarios[index], measurements[index]);
    }
    std::cout << "\n  ]\n}\n";
  } else {
    std::cout << "benchmark=codec rows=" << options.rows << " iterations=" << options.iterations
              << " execution=single_thread scope=memory_only_no_io_index_or_checksum"
              << " build_mode=" << kBuildMode
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " source_revision=" << SNIFFER_BENCHMARK_SOURCE_REVISION << " compiler=\""
              << SNIFFER_BENCHMARK_COMPILER << "\""
              << " arrow_version=" << ARROW_VERSION_STRING << '\n';
    for (size_t index = 0; index < scenarios.size(); ++index) {
      PrintTextResult(scenarios[index], measurements[index]);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto result = RunBenchmark(argc, argv);
  if (!result.ok()) {
    std::cerr << result.status().ToString() << '\n';
    return 1;
  }
  return *result;
}
