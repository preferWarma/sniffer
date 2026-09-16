#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "codec_internal.h"

namespace {

constexpr int64_t kDefaultRows = 100000;

struct Scenario {
  std::string name;
  std::string encoding_name;
  sniffer::FieldSpec field;
  std::shared_ptr<arrow::Array> array;
  uint16_t encoding_id = 0;
  std::vector<uint64_t> selection;
  std::shared_ptr<arrow::Array> expected;
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
                  encoding_id,
                  {},
                  nullptr};
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
                  encoding_id,
                  {},
                  nullptr};
}

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

arrow::Result<std::vector<Scenario>> MakeScenarios(int64_t rows) {
  std::vector<Scenario> scenarios;
  scenarios.reserve(6);
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
  Scenario plain_selected = plain;
  plain_selected.name = "plain_selected_int64_50pct";
  plain_selected.selection.reserve(static_cast<size_t>((rows + 1) / 2));
  arrow::Int64Builder selected_builder;
  ARROW_RETURN_NOT_OK(selected_builder.Reserve((rows + 1) / 2));
  const auto& plain_array = static_cast<const arrow::Int64Array&>(*plain.array);
  for (int64_t row = 0; row < rows; row += 2) {
    plain_selected.selection.push_back(static_cast<uint64_t>(row));
    if (plain_array.IsNull(row)) {
      ARROW_RETURN_NOT_OK(selected_builder.AppendNull());
    } else {
      ARROW_RETURN_NOT_OK(selected_builder.Append(plain_array.Value(row)));
    }
  }
  ARROW_RETURN_NOT_OK(selected_builder.Finish(&plain_selected.expected));
  scenarios.push_back(std::move(plain));
  scenarios.push_back(std::move(plain_selected));
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
  Scenario rle_selected = rle;
  rle_selected.name = "rle_selected_int64_1pct";
  rle_selected.selection.reserve(static_cast<size_t>((rows + 99) / 100));
  arrow::Int64Builder rle_selected_builder;
  ARROW_RETURN_NOT_OK(rle_selected_builder.Reserve((rows + 99) / 100));
  const auto& rle_array = static_cast<const arrow::Int64Array&>(*rle.array);
  for (int64_t row = 0; row < rows; row += 100) {
    rle_selected.selection.push_back(static_cast<uint64_t>(row));
    ARROW_RETURN_NOT_OK(rle_selected_builder.Append(rle_array.Value(row)));
  }
  ARROW_RETURN_NOT_OK(rle_selected_builder.Finish(&rle_selected.expected));
  scenarios.push_back(std::move(rle));
  scenarios.push_back(std::move(rle_selected));
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

const arrow::Result<std::vector<Scenario>>& BenchmarkScenarios() {
  static const auto scenarios = MakeScenarios(kDefaultRows);
  return scenarios;
}

arrow::Result<std::vector<uint8_t>> Encode(const Scenario& scenario) {
  if (scenario.encoding_id == sniffer::internal::kPlainEncodingId) {
    return sniffer::internal::EncodePlain(scenario.field, *scenario.array);
  }
  return sniffer::internal::EncodeNonPlain(scenario.encoding_id, scenario.field, *scenario.array);
}

arrow::Result<sniffer::internal::ColumnChunkMeta> MakeChunk(const Scenario& scenario,
                                                            uint64_t payload_size) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type,
                        sniffer::internal::PhysicalTypeFor(*scenario.field.type));
  sniffer::internal::ColumnChunkMeta chunk;
  chunk.field_id = scenario.field.field_id;
  chunk.physical_type = physical_type;
  chunk.encoding_id = scenario.encoding_id;
  chunk.row_count = static_cast<uint64_t>(scenario.array->length());
  chunk.null_count = static_cast<uint64_t>(scenario.array->null_count());
  chunk.length = payload_size;
  return chunk;
}

arrow::Result<std::shared_ptr<arrow::Array>> Decode(const Scenario& scenario,
                                                    const sniffer::internal::ColumnChunkMeta& chunk,
                                                    std::span<const uint8_t> payload) {
  if (scenario.encoding_id == sniffer::internal::kPlainEncodingId) {
    if (!scenario.selection.empty()) {
      return sniffer::internal::DecodePlainSelected(scenario.field, chunk, payload,
                                                    scenario.selection);
    }
    return sniffer::internal::DecodePlain(scenario.field, chunk, payload);
  }
  const auto* selection = scenario.selection.empty() ? nullptr : &scenario.selection;
  return sniffer::internal::DecodeNonPlain(scenario.field, chunk, payload, selection);
}

arrow::Status ValidateRoundTrip(const Scenario& scenario, std::span<const uint8_t> payload) {
  ARROW_ASSIGN_OR_RAISE(auto chunk, MakeChunk(scenario, payload.size()));
  ARROW_ASSIGN_OR_RAISE(auto decoded, Decode(scenario, chunk, payload));
  const auto& expected = scenario.expected ? scenario.expected : scenario.array;
  if (!decoded->Equals(*expected)) {
    return arrow::Status::Invalid("codec benchmark round-trip mismatch for ", scenario.name);
  }
  return arrow::Status::OK();
}

void SetCodecCounters(benchmark::State& state, const Scenario& scenario, int64_t logical_bytes,
                      uint64_t encoded_bytes) {
  state.counters["input_rows"] = static_cast<double>(scenario.array->length());
  state.counters["output_rows"] = static_cast<double>(
      scenario.expected ? scenario.expected->length() : scenario.array->length());
  state.counters["encoding_id"] = static_cast<double>(scenario.encoding_id);
  state.counters["logical_bytes"] = static_cast<double>(logical_bytes);
  state.counters["encoded_bytes"] = static_cast<double>(encoded_bytes);
  state.counters["compression_ratio"] =
      static_cast<double>(logical_bytes) / static_cast<double>(encoded_bytes);
  state.SetBytesProcessed(state.iterations() * logical_bytes);
  state.SetItemsProcessed(state.iterations() * scenario.array->length());
}

void RunEncodeBenchmark(benchmark::State& state, const Scenario& scenario) {
  const int64_t logical_bytes = arrow::util::TotalBufferSize(*scenario.array);
  auto baseline = Encode(scenario);
  if (!baseline.ok()) {
    state.SkipWithError(baseline.status().ToString());
    return;
  }
  const auto validation = ValidateRoundTrip(scenario, *baseline);
  if (!validation.ok()) {
    state.SkipWithError(validation.ToString());
    return;
  }
  const uint64_t encoded_bytes = static_cast<uint64_t>(baseline->size());
  for (auto _ : state) {
    (void)_;
    auto payload = Encode(scenario);
    if (!payload.ok()) {
      state.SkipWithError(payload.status().ToString());
      break;
    }
    if (payload->size() != encoded_bytes) {
      state.SkipWithError("codec produced a non-deterministic payload size");
      break;
    }
    benchmark::DoNotOptimize(payload->data());
    benchmark::ClobberMemory();
  }
  SetCodecCounters(state, scenario, logical_bytes, encoded_bytes);
}

void RunDecodeBenchmark(benchmark::State& state, const Scenario& scenario) {
  const int64_t logical_bytes = arrow::util::TotalBufferSize(*scenario.array);
  auto payload = Encode(scenario);
  if (!payload.ok()) {
    state.SkipWithError(payload.status().ToString());
    return;
  }
  auto chunk = MakeChunk(scenario, payload->size());
  if (!chunk.ok()) {
    state.SkipWithError(chunk.status().ToString());
    return;
  }
  const auto validation = ValidateRoundTrip(scenario, *payload);
  if (!validation.ok()) {
    state.SkipWithError(validation.ToString());
    return;
  }
  for (auto _ : state) {
    (void)_;
    auto decoded = Decode(scenario, *chunk, *payload);
    if (!decoded.ok()) {
      state.SkipWithError(decoded.status().ToString());
      break;
    }
    benchmark::DoNotOptimize(decoded->get());
    benchmark::ClobberMemory();
  }
  SetCodecCounters(state, scenario, logical_bytes, static_cast<uint64_t>(payload->size()));
}

void Codec(benchmark::State& state, size_t scenario_index, bool encode) {
  const auto& scenarios = BenchmarkScenarios();
  if (!scenarios.ok()) {
    state.SkipWithError(scenarios.status().ToString());
    return;
  }
  if (encode) {
    RunEncodeBenchmark(state, scenarios->at(scenario_index));
  } else {
    RunDecodeBenchmark(state, scenarios->at(scenario_index));
  }
}

BENCHMARK_CAPTURE(Codec, plain_random_int64_Encode, 0U, true)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, plain_random_int64_Decode, 0U, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, plain_selected_int64_50pct_Encode, 1U, true)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, plain_selected_int64_50pct_Decode, 1U, false)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, dictionary_string_32_Encode, 2U, true)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, dictionary_string_32_Decode, 2U, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, rle_128_runs_Encode, 3U, true)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, rle_128_runs_Decode, 3U, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, rle_selected_int64_1pct_Decode, 4U, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, for_bitpack_128_range_Encode, 5U, true)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(Codec, for_bitpack_128_range_Decode, 5U, false)->Unit(benchmark::kMicrosecond);

void AddBenchmarkContext() {
#ifdef NDEBUG
  benchmark::AddCustomContext("build_mode", "release");
#else
  benchmark::AddCustomContext("build_mode", "debug");
#endif
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
  benchmark::AddCustomContext("rows", std::to_string(kDefaultRows));
  benchmark::AddCustomContext("scope", "memory_only_no_io_index_or_checksum");
}

}  // namespace

int main(int argc, char** argv) {
  AddBenchmarkContext();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
