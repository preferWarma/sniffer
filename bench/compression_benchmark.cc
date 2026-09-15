#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/compression.h>
#include <arrow/util/config.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

struct Options {
  int64_t rows = 100000;
  int iterations = 5;
  uint32_t row_group_rows = 4096;
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
    if (parse("--row-group=", &options.row_group_rows)) {
      continue;
    }
    if (argument == "--output-format=json") {
      options.output_json = true;
      continue;
    }
  }
  options.rows = std::max<int64_t>(1, options.rows);
  options.iterations = std::max(1, options.iterations);
  options.row_group_rows = std::max<uint32_t>(1, options.row_group_rows);
  return options;
}

struct Scenario {
  std::string name;
  sniffer::TableSchema schema;
  std::shared_ptr<arrow::RecordBatch> batch;
};

template <typename Generator>
arrow::Result<Scenario> MakeInt64Scenario(std::string name, int64_t rows, bool nullable,
                                          Generator&& generator) {
  sniffer::TableSchema schema{1, {{1, "value", arrow::int64(), nullable, nullptr}}};
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
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
  std::shared_ptr<arrow::Array> values;
  ARROW_RETURN_NOT_OK(builder.Finish(&values));
  auto batch = arrow::RecordBatch::Make(std::move(arrow_schema), rows, {std::move(values)});
  ARROW_RETURN_NOT_OK(batch->ValidateFull());
  return Scenario{std::move(name), std::move(schema), std::move(batch)};
}

template <typename Generator>
arrow::Result<Scenario> MakeStringScenario(std::string name, int64_t rows, bool nullable,
                                           Generator&& generator) {
  sniffer::TableSchema schema{1, {{1, "value", arrow::utf8(), nullable, nullptr}}};
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
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
  std::shared_ptr<arrow::Array> values;
  ARROW_RETURN_NOT_OK(builder.Finish(&values));
  auto batch = arrow::RecordBatch::Make(std::move(arrow_schema), rows, {std::move(values)});
  ARROW_RETURN_NOT_OK(batch->ValidateFull());
  return Scenario{std::move(name), std::move(schema), std::move(batch)};
}

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::string HighCardinalityString(int64_t row) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string result(24, ' ');
  uint64_t state = Mix(static_cast<uint64_t>(row));
  for (size_t index = 0; index < result.size(); ++index) {
    if (index % 8U == 0) {
      state = Mix(state + index);
    }
    result[index] = alphabet[static_cast<size_t>((state >> ((index % 8U) * 8U)) & 63U)];
  }
  return result;
}

arrow::Result<std::vector<Scenario>> MakeScenarios(int64_t rows) {
  std::vector<Scenario> scenarios;
  scenarios.reserve(6);
  ARROW_ASSIGN_OR_RAISE(auto ascending,
                        MakeInt64Scenario("ascending_int64", rows, false,
                                          [](int64_t row) { return std::optional<int64_t>(row); }));
  scenarios.push_back(std::move(ascending));
  ARROW_ASSIGN_OR_RAISE(auto narrow,
                        MakeInt64Scenario("narrow_int64", rows, false, [](int64_t row) {
                          return std::optional<int64_t>(1000000 + row % 128);
                        }));
  scenarios.push_back(std::move(narrow));
  const int64_t run_length = std::max<int64_t>(1, rows / 128);
  ARROW_ASSIGN_OR_RAISE(auto rle,
                        MakeInt64Scenario("long_rle_int64", rows, false, [run_length](int64_t row) {
                          return std::optional<int64_t>(row / run_length);
                        }));
  scenarios.push_back(std::move(rle));
  ARROW_ASSIGN_OR_RAISE(
      auto nullable, MakeInt64Scenario("nullable_skewed_int64", rows, true, [](int64_t row) {
        return row % 5 == 0 ? std::optional<int64_t>() : std::optional<int64_t>(row % 8);
      }));
  scenarios.push_back(std::move(nullable));
  ARROW_ASSIGN_OR_RAISE(auto low_cardinality,
                        MakeStringScenario("low_cardinality_string", rows, false, [](int64_t row) {
                          return std::optional<std::string>("group-" + std::to_string(row % 32));
                        }));
  scenarios.push_back(std::move(low_cardinality));
  ARROW_ASSIGN_OR_RAISE(auto high_cardinality,
                        MakeStringScenario("high_cardinality_string", rows, false, [](int64_t row) {
                          return std::optional<std::string>(HighCardinalityString(row));
                        }));
  scenarios.push_back(std::move(high_cardinality));
  return scenarios;
}

arrow::Result<uint64_t> FileSize(const std::filesystem::path& path) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error) {
    return arrow::Status::IOError("failed to determine file size: ", error.message());
  }
  return size;
}

arrow::Status ValidateBatches(const arrow::RecordBatch& expected,
                              const std::vector<std::shared_ptr<arrow::RecordBatch>>& actual) {
  int64_t offset = 0;
  for (const auto& batch : actual) {
    if (!batch->Equals(*expected.Slice(offset, batch->num_rows()))) {
      return arrow::Status::Invalid("compression benchmark round-trip mismatch");
    }
    offset += batch->num_rows();
  }
  if (offset != expected.num_rows()) {
    return arrow::Status::Invalid("compression benchmark row count mismatch");
  }
  return arrow::Status::OK();
}

struct CompressionMeasurement {
  double encode_write_milliseconds = 0;
  double decode_read_milliseconds = 0;
  uint64_t file_bytes = 0;
  sniffer::benchmark::SampleStats encode_write_stats;
  sniffer::benchmark::SampleStats decode_read_stats;
};

struct ScenarioMeasurement {
  std::string name;
  uint64_t logical_bytes = 0;
  CompressionMeasurement sniffer;
  CompressionMeasurement arrow_ipc;
  CompressionMeasurement arrow_ipc_zstd;
};

arrow::Result<CompressionMeasurement> MeasureSnifferOnce(const std::filesystem::path& path,
                                                         const Scenario& scenario,
                                                         const Options& options) {
  const auto encode_start = std::chrono::steady_clock::now();
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = options.row_group_rows;
  ARROW_ASSIGN_OR_RAISE(auto writer,
                        sniffer::SegmentWriter::Open(path.string(), scenario.schema, policy));
  ARROW_RETURN_NOT_OK(writer->Append(scenario.batch));
  ARROW_RETURN_NOT_OK(writer->Finish());
  const auto encode_end = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(const uint64_t file_bytes, FileSize(path));

  const auto decode_start = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(auto reader, sniffer::SegmentReader::Open(path.string()));
  ARROW_ASSIGN_OR_RAISE(auto batches, reader->ReadAll());
  const auto decode_end = std::chrono::steady_clock::now();
  ARROW_RETURN_NOT_OK(ValidateBatches(*scenario.batch, batches));
  CompressionMeasurement measurement;
  measurement.encode_write_milliseconds =
      std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
  measurement.decode_read_milliseconds =
      std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
  measurement.file_bytes = file_bytes;
  return measurement;
}

arrow::Result<CompressionMeasurement> MeasureArrowIpcOnce(const std::filesystem::path& path,
                                                          const Scenario& scenario,
                                                          const Options& options,
                                                          arrow::Compression::type compression) {
  const auto encode_start = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(auto output, arrow::io::FileOutputStream::Open(path.string()));
  auto write_options = arrow::ipc::IpcWriteOptions::Defaults();
  write_options.use_threads = false;
  if (compression != arrow::Compression::UNCOMPRESSED) {
    ARROW_ASSIGN_OR_RAISE(auto codec, arrow::util::Codec::Create(compression));
    write_options.codec = std::move(codec);
  }
  ARROW_ASSIGN_OR_RAISE(
      auto writer, arrow::ipc::MakeFileWriter(output, scenario.batch->schema(), write_options));
  for (int64_t offset = 0; offset < scenario.batch->num_rows(); offset += options.row_group_rows) {
    const int64_t length =
        std::min<int64_t>(options.row_group_rows, scenario.batch->num_rows() - offset);
    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*scenario.batch->Slice(offset, length)));
  }
  ARROW_RETURN_NOT_OK(writer->Close());
  ARROW_RETURN_NOT_OK(output->Close());
  const auto encode_end = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(const uint64_t file_bytes, FileSize(path));

  const auto decode_start = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(path.string()));
  auto read_options = arrow::ipc::IpcReadOptions::Defaults();
  read_options.use_threads = false;
  ARROW_ASSIGN_OR_RAISE(auto reader, arrow::ipc::RecordBatchFileReader::Open(input, read_options));
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  batches.reserve(static_cast<size_t>(reader->num_record_batches()));
  for (int index = 0; index < reader->num_record_batches(); ++index) {
    ARROW_ASSIGN_OR_RAISE(auto batch, reader->ReadRecordBatch(index));
    batches.push_back(std::move(batch));
  }
  ARROW_RETURN_NOT_OK(input->Close());
  const auto decode_end = std::chrono::steady_clock::now();
  ARROW_RETURN_NOT_OK(ValidateBatches(*scenario.batch, batches));
  CompressionMeasurement measurement;
  measurement.encode_write_milliseconds =
      std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
  measurement.decode_read_milliseconds =
      std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
  measurement.file_bytes = file_bytes;
  return measurement;
}

template <typename Runner>
arrow::Result<CompressionMeasurement> MedianMeasurement(int iterations, Runner&& runner) {
  std::vector<double> encode_write_times;
  std::vector<double> decode_read_times;
  encode_write_times.reserve(static_cast<size_t>(iterations));
  decode_read_times.reserve(static_cast<size_t>(iterations));
  uint64_t file_bytes = 0;
  for (int iteration = 0; iteration < iterations; ++iteration) {
    ARROW_ASSIGN_OR_RAISE(auto measurement, runner());
    if (iteration != 0 && measurement.file_bytes != file_bytes) {
      return arrow::Status::Invalid("compression benchmark produced a non-deterministic file size");
    }
    file_bytes = measurement.file_bytes;
    encode_write_times.push_back(measurement.encode_write_milliseconds);
    decode_read_times.push_back(measurement.decode_read_milliseconds);
  }
  CompressionMeasurement result;
  result.encode_write_stats = sniffer::benchmark::SummarizeSamples(std::move(encode_write_times));
  result.decode_read_stats = sniffer::benchmark::SummarizeSamples(std::move(decode_read_times));
  result.encode_write_milliseconds = result.encode_write_stats.p50;
  result.decode_read_milliseconds = result.decode_read_stats.p50;
  result.file_bytes = file_bytes;
  return result;
}

double LogicalMiBPerSecond(uint64_t logical_bytes, double milliseconds) {
  constexpr double kBytesPerMiB = 1024.0 * 1024.0;
  return static_cast<double>(logical_bytes) / kBytesPerMiB / (milliseconds / 1000.0);
}

void AccumulateSamples(const sniffer::benchmark::SampleStats& source,
                       std::vector<double>* destination) {
  if (destination->empty()) {
    destination->resize(source.samples.size(), 0);
  }
  for (size_t index = 0; index < source.samples.size(); ++index) {
    (*destination)[index] += source.samples[index];
  }
}

void FinalizeAggregate(std::vector<double> encode_write_samples,
                       std::vector<double> decode_read_samples,
                       CompressionMeasurement* measurement) {
  measurement->encode_write_stats =
      sniffer::benchmark::SummarizeSamples(std::move(encode_write_samples));
  measurement->decode_read_stats =
      sniffer::benchmark::SummarizeSamples(std::move(decode_read_samples));
  measurement->encode_write_milliseconds = measurement->encode_write_stats.p50;
  measurement->decode_read_milliseconds = measurement->decode_read_stats.p50;
}

void PrintResult(std::string_view scenario, std::string_view format, int64_t rows,
                 uint64_t logical_bytes, const CompressionMeasurement& measurement) {
  std::cout << "scenario=" << scenario << " format=" << format << " rows=" << rows
            << " logical_bytes=" << logical_bytes << " file_bytes=" << measurement.file_bytes
            << " compression_ratio="
            << static_cast<double>(logical_bytes) / static_cast<double>(measurement.file_bytes)
            << " storage_ratio="
            << static_cast<double>(measurement.file_bytes) / static_cast<double>(logical_bytes)
            << " bytes_per_value="
            << static_cast<double>(measurement.file_bytes) / static_cast<double>(rows)
            << " encode_write_ms=" << measurement.encode_write_milliseconds
            << " encode_write_mib_per_second="
            << LogicalMiBPerSecond(logical_bytes, measurement.encode_write_milliseconds)
            << " decode_read_ms=" << measurement.decode_read_milliseconds
            << " decode_read_mib_per_second="
            << LogicalMiBPerSecond(logical_bytes, measurement.decode_read_milliseconds);
  sniffer::benchmark::PrintSampleStats(std::cout, "encode_write", measurement.encode_write_stats);
  sniffer::benchmark::PrintSampleStats(std::cout, "decode_read", measurement.decode_read_stats);
  std::cout << '\n';
}

void PrintJsonMeasurement(std::string_view format, int64_t rows, uint64_t logical_bytes,
                          const CompressionMeasurement& measurement) {
  std::cout << "{\"format\":";
  sniffer::benchmark::PrintJsonString(std::cout, format);
  std::cout << ",\"rows\":" << rows << ",\"logical_bytes\":" << logical_bytes
            << ",\"file_bytes\":" << measurement.file_bytes << ",\"compression_ratio\":"
            << static_cast<double>(logical_bytes) / static_cast<double>(measurement.file_bytes)
            << ",\"storage_ratio\":"
            << static_cast<double>(measurement.file_bytes) / static_cast<double>(logical_bytes)
            << ",\"bytes_per_value\":"
            << static_cast<double>(measurement.file_bytes) / static_cast<double>(rows)
            << ",\"encode_write_ms\":" << measurement.encode_write_milliseconds
            << ",\"encode_write_mib_per_second\":"
            << LogicalMiBPerSecond(logical_bytes, measurement.encode_write_milliseconds)
            << ",\"decode_read_ms\":" << measurement.decode_read_milliseconds
            << ",\"decode_read_mib_per_second\":"
            << LogicalMiBPerSecond(logical_bytes, measurement.decode_read_milliseconds)
            << ",\"encode_write_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.encode_write_stats);
  std::cout << ",\"decode_read_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.decode_read_stats);
  std::cout << '}';
}

void PrintJsonScenario(const ScenarioMeasurement& scenario, int64_t rows) {
  std::cout << "    {\"scenario\":";
  sniffer::benchmark::PrintJsonString(std::cout, scenario.name);
  std::cout << ",\"formats\":[";
  PrintJsonMeasurement("sniffer", rows, scenario.logical_bytes, scenario.sniffer);
  std::cout << ',';
  PrintJsonMeasurement("arrow_ipc", rows, scenario.logical_bytes, scenario.arrow_ipc);
  std::cout << ',';
  PrintJsonMeasurement("arrow_ipc_zstd", rows, scenario.logical_bytes, scenario.arrow_ipc_zstd);
  std::cout << "]}";
}

void PrintJsonReport(int argc, char** argv, const Options& options, std::string_view build_mode,
                     const std::vector<ScenarioMeasurement>& scenarios, int64_t total_rows,
                     uint64_t total_logical, const CompressionMeasurement& total_sniffer,
                     const CompressionMeasurement& total_ipc,
                     const CompressionMeasurement& total_ipc_zstd) {
  std::cout << "{\n  \"benchmark\":\"compression\",\n  \"source_revision\":";
  sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_SOURCE_REVISION);
  std::cout << ",\n  \"compiler\":";
  sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_COMPILER);
  std::cout << ",\n  \"arrow_version\":";
  sniffer::benchmark::PrintJsonString(std::cout, ARROW_VERSION_STRING);
  std::cout << ",\n  \"build_mode\":";
  sniffer::benchmark::PrintJsonString(std::cout, build_mode);
  std::cout << ",\n  \"command_arguments\":";
  sniffer::benchmark::PrintJsonArguments(std::cout, argc, argv);
  std::cout << ",\n  \"configuration\":{\"rows_per_scenario\":" << options.rows
            << ",\"row_group_rows\":" << options.row_group_rows
            << ",\"iterations\":" << options.iterations
            << ",\"execution\":\"single_thread\",\"timing\":"
               "\"encode_write_and_decode_read\",\"hardware_threads\":"
            << std::thread::hardware_concurrency() << "},\n  \"scenarios\":[\n";
  for (size_t index = 0; index < scenarios.size(); ++index) {
    if (index != 0) {
      std::cout << ",\n";
    }
    PrintJsonScenario(scenarios[index], options.rows);
  }
  std::cout << "\n  ],\n  \"aggregate\":{\"formats\":[";
  PrintJsonMeasurement("sniffer", total_rows, total_logical, total_sniffer);
  std::cout << ',';
  PrintJsonMeasurement("arrow_ipc", total_rows, total_logical, total_ipc);
  std::cout << ',';
  PrintJsonMeasurement("arrow_ipc_zstd", total_rows, total_logical, total_ipc_zstd);
  std::cout << "]},\n  \"comparisons\":{\"sniffer_size_ratio_vs_arrow_ipc\":"
            << static_cast<double>(total_sniffer.file_bytes) /
                   static_cast<double>(total_ipc.file_bytes)
            << ",\"sniffer_size_ratio_vs_arrow_ipc_zstd\":"
            << static_cast<double>(total_sniffer.file_bytes) /
                   static_cast<double>(total_ipc_zstd.file_bytes)
            << "}\n}\n";
}

arrow::Result<int> RunBenchmark(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  ARROW_ASSIGN_OR_RAISE(auto scenarios, MakeScenarios(options.rows));
  const auto temporary = std::filesystem::temp_directory_path();
  uint64_t total_logical = 0;
  CompressionMeasurement total_sniffer;
  CompressionMeasurement total_ipc;
  CompressionMeasurement total_ipc_zstd;
  std::vector<double> total_sniffer_encode_samples;
  std::vector<double> total_sniffer_decode_samples;
  std::vector<double> total_ipc_encode_samples;
  std::vector<double> total_ipc_decode_samples;
  std::vector<double> total_ipc_zstd_encode_samples;
  std::vector<double> total_ipc_zstd_decode_samples;
  std::vector<ScenarioMeasurement> results;
  results.reserve(scenarios.size());
#ifdef NDEBUG
  constexpr std::string_view kBuildMode = "release";
#else
  constexpr std::string_view kBuildMode = "debug";
#endif
  if (!options.output_json) {
    std::cout << "benchmark=compression row_group_rows=" << options.row_group_rows
              << " iterations=" << options.iterations
              << " execution=single_thread build_mode=" << kBuildMode
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " timing=encode_write_and_decode_read scenarios=" << scenarios.size()
              << " source_revision=" << SNIFFER_BENCHMARK_SOURCE_REVISION << " compiler=\""
              << SNIFFER_BENCHMARK_COMPILER << "\""
              << " arrow_version=" << ARROW_VERSION_STRING << '\n';
  }
  for (const auto& scenario : scenarios) {
    const auto sniffer_path = temporary / ("sniffer_compression_" + scenario.name + ".seg");
    const auto ipc_path = temporary / ("sniffer_compression_" + scenario.name + ".arrow");
    const auto ipc_zstd_path = temporary / ("sniffer_compression_" + scenario.name + ".zstd.arrow");
    const int64_t logical_size = arrow::util::TotalBufferSize(*scenario.batch);
    if (logical_size <= 0) {
      return arrow::Status::Invalid("compression benchmark logical size must be positive");
    }
    const uint64_t logical_bytes = static_cast<uint64_t>(logical_size);
    ARROW_ASSIGN_OR_RAISE(const auto sniffer, MedianMeasurement(options.iterations, [&]() {
                            return MeasureSnifferOnce(sniffer_path, scenario, options);
                          }));
    ARROW_ASSIGN_OR_RAISE(const auto ipc, MedianMeasurement(options.iterations, [&]() {
                            return MeasureArrowIpcOnce(ipc_path, scenario, options,
                                                       arrow::Compression::UNCOMPRESSED);
                          }));
    ARROW_ASSIGN_OR_RAISE(const auto ipc_zstd, MedianMeasurement(options.iterations, [&]() {
                            return MeasureArrowIpcOnce(ipc_zstd_path, scenario, options,
                                                       arrow::Compression::ZSTD);
                          }));
    if (!options.output_json) {
      PrintResult(scenario.name, "sniffer", options.rows, logical_bytes, sniffer);
      PrintResult(scenario.name, "arrow_ipc", options.rows, logical_bytes, ipc);
      PrintResult(scenario.name, "arrow_ipc_zstd", options.rows, logical_bytes, ipc_zstd);
    }
    results.push_back({scenario.name, logical_bytes, sniffer, ipc, ipc_zstd});
    total_logical += logical_bytes;
    total_sniffer.file_bytes += sniffer.file_bytes;
    AccumulateSamples(sniffer.encode_write_stats, &total_sniffer_encode_samples);
    AccumulateSamples(sniffer.decode_read_stats, &total_sniffer_decode_samples);
    total_ipc.file_bytes += ipc.file_bytes;
    AccumulateSamples(ipc.encode_write_stats, &total_ipc_encode_samples);
    AccumulateSamples(ipc.decode_read_stats, &total_ipc_decode_samples);
    total_ipc_zstd.file_bytes += ipc_zstd.file_bytes;
    AccumulateSamples(ipc_zstd.encode_write_stats, &total_ipc_zstd_encode_samples);
    AccumulateSamples(ipc_zstd.decode_read_stats, &total_ipc_zstd_decode_samples);

    std::error_code ignored;
    std::filesystem::remove(sniffer_path, ignored);
    std::filesystem::remove(ipc_path, ignored);
    std::filesystem::remove(ipc_zstd_path, ignored);
  }
  FinalizeAggregate(std::move(total_sniffer_encode_samples),
                    std::move(total_sniffer_decode_samples), &total_sniffer);
  FinalizeAggregate(std::move(total_ipc_encode_samples), std::move(total_ipc_decode_samples),
                    &total_ipc);
  FinalizeAggregate(std::move(total_ipc_zstd_encode_samples),
                    std::move(total_ipc_zstd_decode_samples), &total_ipc_zstd);
  const int64_t total_rows = options.rows * static_cast<int64_t>(scenarios.size());
  if (options.output_json) {
    PrintJsonReport(argc, argv, options, kBuildMode, results, total_rows, total_logical,
                    total_sniffer, total_ipc, total_ipc_zstd);
  } else {
    PrintResult("all", "sniffer", total_rows, total_logical, total_sniffer);
    PrintResult("all", "arrow_ipc", total_rows, total_logical, total_ipc);
    PrintResult("all", "arrow_ipc_zstd", total_rows, total_logical, total_ipc_zstd);
    std::cout << "scenario=all sniffer_size_ratio_vs_arrow_ipc="
              << static_cast<double>(total_sniffer.file_bytes) /
                     static_cast<double>(total_ipc.file_bytes)
              << " sniffer_size_ratio_vs_arrow_ipc_zstd="
              << static_cast<double>(total_sniffer.file_bytes) /
                     static_cast<double>(total_ipc_zstd.file_bytes)
              << '\n';
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
