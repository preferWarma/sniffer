#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/compression.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr int64_t kDefaultRows = 100000;
constexpr uint32_t kDefaultRowGroupRows = 4096;

enum class Format { kSniffer, kArrowIpc, kArrowIpcZstd };

struct Scenario {
  std::string name;
  sniffer::TableSchema schema;
  std::shared_ptr<arrow::RecordBatch> batch;
};

struct CompressionMeasurement {
  double encode_write_milliseconds = 0;
  double decode_read_milliseconds = 0;
  uint64_t file_bytes = 0;
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

arrow::Result<CompressionMeasurement> MeasureSnifferOnce(const std::filesystem::path& path,
                                                         const Scenario& scenario,
                                                         uint32_t row_group_rows) {
  const auto encode_start = std::chrono::steady_clock::now();
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = row_group_rows;
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
  return CompressionMeasurement{
      std::chrono::duration<double, std::milli>(encode_end - encode_start).count(),
      std::chrono::duration<double, std::milli>(decode_end - decode_start).count(), file_bytes};
}

arrow::Result<CompressionMeasurement> MeasureArrowIpcOnce(const std::filesystem::path& path,
                                                          const Scenario& scenario,
                                                          uint32_t row_group_rows,
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
  for (int64_t offset = 0; offset < scenario.batch->num_rows(); offset += row_group_rows) {
    const int64_t length = std::min<int64_t>(row_group_rows, scenario.batch->num_rows() - offset);
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
  return CompressionMeasurement{
      std::chrono::duration<double, std::milli>(encode_end - encode_start).count(),
      std::chrono::duration<double, std::milli>(decode_end - decode_start).count(), file_bytes};
}

std::string_view FormatName(Format format) {
  switch (format) {
    case Format::kSniffer:
      return "Sniffer";
    case Format::kArrowIpc:
      return "ArrowIPC";
    case Format::kArrowIpcZstd:
      return "ArrowIPC_ZSTD";
  }
  return "Unknown";
}

void RunCompressionBenchmark(benchmark::State& state, const Scenario& scenario, Format format,
                             uint32_t row_group_rows) {
  const int64_t logical_size = arrow::util::TotalBufferSize(*scenario.batch);
  if (logical_size <= 0) {
    state.SkipWithError("logical size must be positive");
    return;
  }
  const auto path =
      std::filesystem::temp_directory_path() /
      ("sniffer_compression_" + scenario.name + "_" + std::string(FormatName(format)) + ".tmp");
  double encode_total = 0;
  double decode_total = 0;
  uint64_t file_bytes = 0;
  for (auto _ : state) {
    (void)_;
    arrow::Result<CompressionMeasurement> result = arrow::Status::Invalid("unknown format");
    if (format == Format::kSniffer) {
      result = MeasureSnifferOnce(path, scenario, row_group_rows);
    } else {
      result = MeasureArrowIpcOnce(path, scenario, row_group_rows,
                                   format == Format::kArrowIpc ? arrow::Compression::UNCOMPRESSED
                                                               : arrow::Compression::ZSTD);
    }
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      break;
    }
    if (file_bytes != 0 && file_bytes != result->file_bytes) {
      state.SkipWithError("non-deterministic benchmark file size");
      break;
    }
    file_bytes = result->file_bytes;
    encode_total += result->encode_write_milliseconds;
    decode_total += result->decode_read_milliseconds;
    state.SetIterationTime((result->encode_write_milliseconds + result->decode_read_milliseconds) /
                           1000.0);
    benchmark::DoNotOptimize(result->file_bytes);
  }

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (state.iterations() == 0 || file_bytes == 0) {
    return;
  }
  const double iterations = static_cast<double>(state.iterations());
  state.counters["encode_write_ms"] = encode_total / iterations;
  state.counters["decode_read_ms"] = decode_total / iterations;
  state.counters["logical_bytes"] = static_cast<double>(logical_size);
  state.counters["file_bytes"] = static_cast<double>(file_bytes);
  state.counters["compression_ratio"] =
      static_cast<double>(logical_size) / static_cast<double>(file_bytes);
  state.counters["storage_ratio"] =
      static_cast<double>(file_bytes) / static_cast<double>(logical_size);
  state.counters["bytes_per_value"] =
      static_cast<double>(file_bytes) / static_cast<double>(scenario.batch->num_rows());
  state.SetBytesProcessed(state.iterations() * logical_size);
  state.SetItemsProcessed(state.iterations() * scenario.batch->num_rows());
}

arrow::Status RegisterBenchmarks() {
  ARROW_ASSIGN_OR_RAISE(auto scenarios, MakeScenarios(kDefaultRows));
  for (const auto& scenario : scenarios) {
    for (const auto format : {Format::kSniffer, Format::kArrowIpc, Format::kArrowIpcZstd}) {
      const std::string name =
          "Compression/" + scenario.name + "/" + std::string(FormatName(format));
      benchmark::RegisterBenchmark(name.c_str(),
                                   [scenario, format](benchmark::State& state) {
                                     RunCompressionBenchmark(state, scenario, format,
                                                             kDefaultRowGroupRows);
                                   })
          ->UseManualTime()
          ->Unit(benchmark::kMillisecond);
    }
  }
  return arrow::Status::OK();
}

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
  benchmark::AddCustomContext("row_group_rows", std::to_string(kDefaultRowGroupRows));
  benchmark::AddCustomContext("logical_size", "Arrow TotalBufferSize of input RecordBatch");
}

}  // namespace

int main(int argc, char** argv) {
  const auto status = RegisterBenchmarks();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }
  AddBenchmarkContext();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
