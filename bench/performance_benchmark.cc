#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/memory_pool.h>
#include <arrow/util/compression.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "sniffer/io_plan.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr int64_t kDefaultRows = 100000;
constexpr int64_t kDefaultRowGroupRows = 4096;

enum class Format { kSniffer, kArrowIpc, kArrowIpcZstd };
enum class Query { kSinglePredicate, kThreePredicates, kSortKeyRange };

struct BenchmarkConfig {
  int64_t rows;
  uint32_t row_group_rows;
  uint32_t selectivity_percent = 50;
  uint32_t projection_columns = 2;
};

struct Measurement {
  double write_milliseconds = 0;
  double scan_milliseconds = 0;
  double total_milliseconds = 0;
  uint64_t file_bytes = 0;
  uint64_t output_rows = 0;
  uint64_t record_groups = 0;
  uint64_t arrow_total_allocated_bytes = 0;
  uint64_t arrow_allocations = 0;
  uint64_t arrow_pool_peak_bytes = 0;
  uint64_t process_peak_rss_bytes = 0;
  sniffer::WriterMetrics writer_metrics;
  sniffer::ReaderMetrics reader_metrics;
  sniffer::ScanMetrics scan_metrics;
};

struct MemorySnapshot {
  int64_t arrow_total_allocated_bytes = 0;
  int64_t arrow_allocations = 0;
  int64_t arrow_pool_peak_bytes = 0;
  uint64_t process_peak_rss_bytes = 0;
};

uint64_t PeakResidentSetBytes() {
#if defined(__APPLE__) || defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#else
  return 0;
#endif
}

MemorySnapshot CaptureMemorySnapshot() {
  const auto* pool = arrow::default_memory_pool();
  return {pool->total_bytes_allocated(), pool->num_allocations(), pool->max_memory(),
          PeakResidentSetBytes()};
}

uint64_t NonNegativeDelta(int64_t before, int64_t after) {
  return after > before ? static_cast<uint64_t>(after - before) : 0;
}

void RecordMemory(const MemorySnapshot& before, const MemorySnapshot& after,
                  Measurement* measurement) {
  measurement->arrow_total_allocated_bytes =
      NonNegativeDelta(before.arrow_total_allocated_bytes, after.arrow_total_allocated_bytes);
  measurement->arrow_allocations =
      NonNegativeDelta(before.arrow_allocations, after.arrow_allocations);
  measurement->arrow_pool_peak_bytes =
      after.arrow_pool_peak_bytes > 0 ? static_cast<uint64_t>(after.arrow_pool_peak_bytes) : 0;
  measurement->process_peak_rss_bytes = after.process_peak_rss_bytes;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> MakeBenchmarkBatch(int64_t rows) {
  auto schema = arrow::schema({arrow::field("id", arrow::int64(), false),
                               arrow::field("group", arrow::utf8(), false),
                               arrow::field("value", arrow::int64(), true)});
  arrow::Int64Builder id_builder;
  arrow::StringBuilder group_builder;
  arrow::Int64Builder value_builder;
  ARROW_RETURN_NOT_OK(id_builder.Reserve(rows));
  ARROW_RETURN_NOT_OK(group_builder.Reserve(rows));
  ARROW_RETURN_NOT_OK(value_builder.Reserve(rows));
  for (int64_t row = 0; row < rows; ++row) {
    ARROW_RETURN_NOT_OK(id_builder.Append(row));
    ARROW_RETURN_NOT_OK(group_builder.Append("group-" + std::to_string(row % 32)));
    if (row % 17 == 0) {
      ARROW_RETURN_NOT_OK(value_builder.AppendNull());
    } else {
      ARROW_RETURN_NOT_OK(value_builder.Append(row * 3));
    }
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(3);
  ARROW_RETURN_NOT_OK(id_builder.Finish(&columns[0]));
  ARROW_RETURN_NOT_OK(group_builder.Finish(&columns[1]));
  ARROW_RETURN_NOT_OK(value_builder.Finish(&columns[2]));
  auto batch = arrow::RecordBatch::Make(std::move(schema), rows, std::move(columns));
  ARROW_RETURN_NOT_OK(batch->ValidateFull());
  return batch;
}

arrow::Result<uint64_t> FileSize(const std::filesystem::path& path) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error) {
    return arrow::Status::IOError("failed to determine file size: ", error.message());
  }
  return size;
}

uint64_t SelectedRows(const BenchmarkConfig& config) {
  return static_cast<uint64_t>(config.rows) * config.selectivity_percent / 100U;
}

int64_t PredicateThreshold(const BenchmarkConfig& config) {
  return config.rows - static_cast<int64_t>(SelectedRows(config));
}

std::vector<uint32_t> ProjectionFieldIds(uint32_t projection_columns) {
  switch (projection_columns) {
    case 1:
      return {1};
    case 2:
      return {1, 3};
    case 3:
      return {1, 2, 3};
    default:
      return {};
  }
}

arrow::Result<Measurement> RunSnifferOnce(const std::filesystem::path& path,
                                          const sniffer::TableSchema& schema,
                                          const sniffer::LayoutPolicy& policy,
                                          const std::shared_ptr<arrow::RecordBatch>& batch,
                                          const BenchmarkConfig& config, Query query) {
  const MemorySnapshot memory_before = CaptureMemorySnapshot();
  const auto start = std::chrono::steady_clock::now();
  auto writer_metrics = std::make_shared<sniffer::WriterMetrics>();
  ARROW_ASSIGN_OR_RAISE(
      auto writer, sniffer::SegmentWriter::Open(path.string(), schema, policy, writer_metrics));
  ARROW_RETURN_NOT_OK(writer->Append(batch));
  ARROW_RETURN_NOT_OK(writer->Finish());
  const auto write_end = std::chrono::steady_clock::now();

  auto reader_metrics = std::make_shared<sniffer::ReaderMetrics>();
  ARROW_ASSIGN_OR_RAISE(auto reader, sniffer::SegmentReader::Open(path.string(), reader_metrics));
  sniffer::IOPlan plan;
  plan.projection_field_ids = ProjectionFieldIds(config.projection_columns);
  if (query == Query::kSortKeyRange) {
    sniffer::SortKeyRange range;
    range.lower = std::vector<std::shared_ptr<arrow::Scalar>>{
        std::make_shared<arrow::Int64Scalar>(batch->num_rows() / 4)};
    range.upper = std::vector<std::shared_ptr<arrow::Scalar>>{
        std::make_shared<arrow::Int64Scalar>(batch->num_rows() * 3 / 4)};
    plan.sort_key_range = std::move(range);
  } else {
    plan.conjunctive_predicates = {
        {1, sniffer::Predicate::Op::kGe,
         std::make_shared<arrow::Int64Scalar>(PredicateThreshold(config))}};
  }
  if (query == Query::kThreePredicates) {
    plan.conjunctive_predicates.push_back(
        {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("group-0")});
    plan.conjunctive_predicates.push_back({3, sniffer::Predicate::Op::kIsNotNull, nullptr});
  }
  plan.output_batch_rows = config.row_group_rows / 2 + 1;
  auto scan_metrics = std::make_shared<sniffer::ScanMetrics>();
  ARROW_ASSIGN_OR_RAISE(auto iterator, reader->Scan(std::move(plan), scan_metrics));
  uint64_t output_rows = 0;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto next, iterator.Next());
    if (!next) {
      break;
    }
    output_rows += static_cast<uint64_t>(next->num_rows());
  }
  const auto end = std::chrono::steady_clock::now();

  Measurement measurement;
  measurement.write_milliseconds =
      std::chrono::duration<double, std::milli>(write_end - start).count();
  measurement.scan_milliseconds =
      std::chrono::duration<double, std::milli>(end - write_end).count();
  measurement.total_milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
  ARROW_ASSIGN_OR_RAISE(measurement.file_bytes, FileSize(path));
  measurement.output_rows = output_rows;
  measurement.record_groups = reader->num_row_groups();
  measurement.writer_metrics = *writer_metrics;
  measurement.reader_metrics = *reader_metrics;
  measurement.scan_metrics = *scan_metrics;
  RecordMemory(memory_before, CaptureMemorySnapshot(), &measurement);
  return measurement;
}

arrow::Result<Measurement> RunArrowIpcOnce(const std::filesystem::path& path,
                                           const std::shared_ptr<arrow::RecordBatch>& batch,
                                           const BenchmarkConfig& config,
                                           arrow::Compression::type compression) {
  const MemorySnapshot memory_before = CaptureMemorySnapshot();
  const auto start = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(auto output, arrow::io::FileOutputStream::Open(path.string()));
  auto write_options = arrow::ipc::IpcWriteOptions::Defaults();
  write_options.use_threads = false;
  if (compression != arrow::Compression::UNCOMPRESSED) {
    ARROW_ASSIGN_OR_RAISE(auto codec, arrow::util::Codec::Create(compression));
    write_options.codec = std::move(codec);
  }
  ARROW_ASSIGN_OR_RAISE(auto writer,
                        arrow::ipc::MakeFileWriter(output, batch->schema(), write_options));
  for (int64_t offset = 0; offset < batch->num_rows(); offset += config.row_group_rows) {
    const int64_t length = std::min<int64_t>(config.row_group_rows, batch->num_rows() - offset);
    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*batch->Slice(offset, length)));
  }
  ARROW_RETURN_NOT_OK(writer->Close());
  ARROW_RETURN_NOT_OK(output->Close());
  const auto write_end = std::chrono::steady_clock::now();

  ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(path.string()));
  auto read_options = arrow::ipc::IpcReadOptions::Defaults();
  read_options.use_threads = false;
  ARROW_ASSIGN_OR_RAISE(auto reader, arrow::ipc::RecordBatchFileReader::Open(input, read_options));
  const int64_t threshold = batch->num_rows() / 2;
  uint64_t output_rows = 0;
  for (int index = 0; index < reader->num_record_batches(); ++index) {
    ARROW_ASSIGN_OR_RAISE(auto current, reader->ReadRecordBatch(index));
    const auto& ids = static_cast<const arrow::Int64Array&>(*current->column(0));
    const auto& values = static_cast<const arrow::Int64Array&>(*current->column(2));
    arrow::Int64Builder filtered_id_builder;
    arrow::Int64Builder filtered_value_builder;
    ARROW_RETURN_NOT_OK(filtered_id_builder.Reserve(current->num_rows()));
    ARROW_RETURN_NOT_OK(filtered_value_builder.Reserve(current->num_rows()));
    for (int64_t row = 0; row < current->num_rows(); ++row) {
      if (ids.Value(row) < threshold) {
        continue;
      }
      ARROW_RETURN_NOT_OK(filtered_id_builder.Append(ids.Value(row)));
      if (values.IsNull(row)) {
        ARROW_RETURN_NOT_OK(filtered_value_builder.AppendNull());
      } else {
        ARROW_RETURN_NOT_OK(filtered_value_builder.Append(values.Value(row)));
      }
    }
    std::shared_ptr<arrow::Array> filtered_id;
    std::shared_ptr<arrow::Array> filtered_value;
    ARROW_RETURN_NOT_OK(filtered_id_builder.Finish(&filtered_id));
    ARROW_RETURN_NOT_OK(filtered_value_builder.Finish(&filtered_value));
    if (filtered_id->length() != filtered_value->length()) {
      return arrow::Status::Invalid("Arrow IPC projected columns have different lengths");
    }
    output_rows += static_cast<uint64_t>(filtered_id->length());
  }
  ARROW_RETURN_NOT_OK(input->Close());
  const auto end = std::chrono::steady_clock::now();

  Measurement measurement;
  measurement.write_milliseconds =
      std::chrono::duration<double, std::milli>(write_end - start).count();
  measurement.scan_milliseconds =
      std::chrono::duration<double, std::milli>(end - write_end).count();
  measurement.total_milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
  ARROW_ASSIGN_OR_RAISE(measurement.file_bytes, FileSize(path));
  measurement.output_rows = output_rows;
  measurement.record_groups = static_cast<uint64_t>(reader->num_record_batches());
  RecordMemory(memory_before, CaptureMemorySnapshot(), &measurement);
  return measurement;
}

void SetAverage(benchmark::State& state, std::string_view name, double total) {
  state.counters[std::string(name)] = total / static_cast<double>(state.iterations());
}

void RunPerformance(benchmark::State& state, Format format, Query query,
                    const BenchmarkConfig& config) {
  auto batch_result = MakeBenchmarkBatch(config.rows);
  if (!batch_result.ok()) {
    state.SkipWithError(batch_result.status().ToString());
    return;
  }
  const auto batch = *batch_result;
  const sniffer::TableSchema schema{1,
                                    {{1, "id", arrow::int64(), false, nullptr},
                                     {2, "group", arrow::utf8(), false, nullptr},
                                     {3, "value", arrow::int64(), true, nullptr}}};
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = config.row_group_rows;
  policy.sort_key_field_ids = {1};
  policy.statistics_field_ids = {1, 3};
  policy.bloom_field_ids = {2};
  const std::string suffix = format == Format::kSniffer
                                 ? "sniffer"
                                 : (format == Format::kArrowIpc ? "arrow" : "arrow_zstd");
  const auto path =
      std::filesystem::temp_directory_path() / ("sniffer_performance_" + suffix + ".tmp");
  Measurement totals;
  for (auto _ : state) {
    (void)_;
    arrow::Result<Measurement> result = arrow::Status::Invalid("uninitialized format");
    if (format == Format::kSniffer) {
      result = RunSnifferOnce(path, schema, policy, batch, config, query);
    } else {
      result = RunArrowIpcOnce(path, batch, config,
                               format == Format::kArrowIpc ? arrow::Compression::UNCOMPRESSED
                                                           : arrow::Compression::ZSTD);
    }
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      break;
    }
    auto value = *result;
    uint64_t expected_rows = SelectedRows(config);
    if (query == Query::kSortKeyRange) {
      expected_rows = static_cast<uint64_t>(config.rows * 3 / 4 - config.rows / 4);
    }
    if (query == Query::kThreePredicates) {
      expected_rows = 0;
      for (int64_t row = PredicateThreshold(config); row < config.rows; ++row) {
        if (row % 32 == 0 && row % 17 != 0) {
          ++expected_rows;
        }
      }
    }
    if (value.output_rows != expected_rows) {
      state.SkipWithError("benchmark result count mismatch");
      break;
    }
    state.SetIterationTime(value.total_milliseconds / 1000.0);
    totals.write_milliseconds += value.write_milliseconds;
    totals.scan_milliseconds += value.scan_milliseconds;
    totals.total_milliseconds += value.total_milliseconds;
    totals.file_bytes += value.file_bytes;
    totals.output_rows += value.output_rows;
    totals.record_groups += value.record_groups;
    totals.arrow_total_allocated_bytes += value.arrow_total_allocated_bytes;
    totals.arrow_allocations += value.arrow_allocations;
    totals.arrow_pool_peak_bytes =
        std::max(totals.arrow_pool_peak_bytes, value.arrow_pool_peak_bytes);
    totals.process_peak_rss_bytes =
        std::max(totals.process_peak_rss_bytes, value.process_peak_rss_bytes);
    totals.writer_metrics.validation_nanoseconds += value.writer_metrics.validation_nanoseconds;
    totals.writer_metrics.index_nanoseconds += value.writer_metrics.index_nanoseconds;
    totals.writer_metrics.encoding_selection_nanoseconds +=
        value.writer_metrics.encoding_selection_nanoseconds;
    totals.writer_metrics.encoding_nanoseconds += value.writer_metrics.encoding_nanoseconds;
    totals.writer_metrics.checksum_nanoseconds += value.writer_metrics.checksum_nanoseconds;
    totals.writer_metrics.file_write_nanoseconds += value.writer_metrics.file_write_nanoseconds;
    totals.writer_metrics.footer_nanoseconds += value.writer_metrics.footer_nanoseconds;
    totals.reader_metrics.envelope_io_nanoseconds += value.reader_metrics.envelope_io_nanoseconds;
    totals.reader_metrics.metadata_parse_nanoseconds +=
        value.reader_metrics.metadata_parse_nanoseconds;
    totals.reader_metrics.directory_validation_nanoseconds +=
        value.reader_metrics.directory_validation_nanoseconds;
    totals.reader_metrics.index_io_nanoseconds += value.reader_metrics.index_io_nanoseconds;
    totals.reader_metrics.index_checksum_nanoseconds +=
        value.reader_metrics.index_checksum_nanoseconds;
    totals.reader_metrics.index_parse_nanoseconds += value.reader_metrics.index_parse_nanoseconds;
    totals.scan_metrics.pruning_nanoseconds += value.scan_metrics.pruning_nanoseconds;
    totals.scan_metrics.chunk_io_nanoseconds += value.scan_metrics.chunk_io_nanoseconds;
    totals.scan_metrics.chunk_checksum_nanoseconds += value.scan_metrics.chunk_checksum_nanoseconds;
    totals.scan_metrics.decode_nanoseconds += value.scan_metrics.decode_nanoseconds;
    totals.scan_metrics.predicate_nanoseconds += value.scan_metrics.predicate_nanoseconds;
    totals.scan_metrics.projection_nanoseconds += value.scan_metrics.projection_nanoseconds;
    totals.scan_metrics.batch_materialization_nanoseconds +=
        value.scan_metrics.batch_materialization_nanoseconds;
    totals.scan_metrics.row_groups_considered += value.scan_metrics.row_groups_considered;
    totals.scan_metrics.row_groups_pruned += value.scan_metrics.row_groups_pruned;
    totals.scan_metrics.column_chunks_read += value.scan_metrics.column_chunks_read;
    totals.scan_metrics.chunk_bytes_read += value.scan_metrics.chunk_bytes_read;
    benchmark::DoNotOptimize(value.output_rows);
  }

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (state.iterations() == 0) {
    return;
  }
  SetAverage(state, "write_ms", totals.write_milliseconds);
  SetAverage(state, "scan_ms", totals.scan_milliseconds);
  SetAverage(state, "end_to_end_ms", totals.total_milliseconds);
  SetAverage(state, "file_bytes", static_cast<double>(totals.file_bytes));
  SetAverage(state, "output_rows", static_cast<double>(totals.output_rows));
  SetAverage(state, "record_groups", static_cast<double>(totals.record_groups));
  SetAverage(state, "arrow_total_allocated_bytes",
             static_cast<double>(totals.arrow_total_allocated_bytes));
  SetAverage(state, "arrow_allocations", static_cast<double>(totals.arrow_allocations));
  SetAverage(
      state, "arrow_bytes_per_input_row",
      static_cast<double>(totals.arrow_total_allocated_bytes) / static_cast<double>(config.rows));
  SetAverage(state, "arrow_allocations_per_input_row",
             static_cast<double>(totals.arrow_allocations) / static_cast<double>(config.rows));
  state.counters["arrow_pool_peak_bytes"] = static_cast<double>(totals.arrow_pool_peak_bytes);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(totals.process_peak_rss_bytes);
  state.counters["selectivity_percent"] = static_cast<double>(config.selectivity_percent);
  state.counters["projection_columns"] = static_cast<double>(config.projection_columns);
  if (format == Format::kSniffer) {
    constexpr double kNanosecondsPerMillisecond = 1'000'000.0;
    const auto phase = [&](std::string_view name, uint64_t nanoseconds) {
      SetAverage(state, name, static_cast<double>(nanoseconds) / kNanosecondsPerMillisecond);
    };
    phase("writer_validation_ms", totals.writer_metrics.validation_nanoseconds);
    phase("writer_index_ms", totals.writer_metrics.index_nanoseconds);
    phase("writer_encoding_selection_ms", totals.writer_metrics.encoding_selection_nanoseconds);
    phase("writer_encoding_ms", totals.writer_metrics.encoding_nanoseconds);
    phase("writer_checksum_ms", totals.writer_metrics.checksum_nanoseconds);
    phase("writer_file_write_ms", totals.writer_metrics.file_write_nanoseconds);
    phase("writer_footer_ms", totals.writer_metrics.footer_nanoseconds);
    phase("reader_envelope_io_ms", totals.reader_metrics.envelope_io_nanoseconds);
    phase("reader_metadata_parse_ms", totals.reader_metrics.metadata_parse_nanoseconds);
    phase("reader_directory_validation_ms", totals.reader_metrics.directory_validation_nanoseconds);
    phase("reader_index_io_ms", totals.reader_metrics.index_io_nanoseconds);
    phase("reader_index_checksum_ms", totals.reader_metrics.index_checksum_nanoseconds);
    phase("reader_index_parse_ms", totals.reader_metrics.index_parse_nanoseconds);
    phase("reader_pruning_ms", totals.scan_metrics.pruning_nanoseconds);
    phase("reader_chunk_io_ms", totals.scan_metrics.chunk_io_nanoseconds);
    phase("reader_chunk_checksum_ms", totals.scan_metrics.chunk_checksum_nanoseconds);
    phase("reader_decode_ms", totals.scan_metrics.decode_nanoseconds);
    phase("reader_predicate_ms", totals.scan_metrics.predicate_nanoseconds);
    phase("reader_projection_ms", totals.scan_metrics.projection_nanoseconds);
    phase("reader_batch_materialization_ms", totals.scan_metrics.batch_materialization_nanoseconds);
    SetAverage(state, "row_groups_considered",
               static_cast<double>(totals.scan_metrics.row_groups_considered));
    SetAverage(state, "row_groups_pruned",
               static_cast<double>(totals.scan_metrics.row_groups_pruned));
    SetAverage(state, "column_chunks_read",
               static_cast<double>(totals.scan_metrics.column_chunks_read));
    SetAverage(state, "chunk_bytes_read",
               static_cast<double>(totals.scan_metrics.chunk_bytes_read));
  }
  state.SetItemsProcessed(state.iterations() * config.rows);
}

void Performance(benchmark::State& state, Format format, Query query) {
  const BenchmarkConfig config{state.range(0), static_cast<uint32_t>(state.range(1))};
  RunPerformance(state, format, query, config);
}

void PerformanceMatrix(benchmark::State& state, Format format, Query query) {
  const BenchmarkConfig config{state.range(0), static_cast<uint32_t>(state.range(1)),
                               static_cast<uint32_t>(state.range(2)),
                               static_cast<uint32_t>(state.range(3))};
  if (config.rows <= 0 || config.row_group_rows == 0 || config.selectivity_percent == 0 ||
      config.selectivity_percent > 100 || config.projection_columns == 0 ||
      config.projection_columns > 3) {
    state.SkipWithError("invalid performance matrix configuration");
    return;
  }
  RunPerformance(state, format, query, config);
}

void ApplyPerformanceMatrix(benchmark::internal::Benchmark* benchmark) {
  constexpr std::array<int64_t, 4> kRowGroupRows = {1024, 8192, 65536, 262144};
  constexpr std::array<int64_t, 4> kSelectivityPercent = {1, 10, 50, 100};
  constexpr std::array<int64_t, 3> kProjectionColumns = {1, 2, 3};
  for (const int64_t row_group_rows : kRowGroupRows) {
    for (const int64_t selectivity_percent : kSelectivityPercent) {
      for (const int64_t projection_columns : kProjectionColumns) {
        benchmark->Args({kDefaultRows, row_group_rows, selectivity_percent, projection_columns});
      }
    }
  }
}

BENCHMARK_CAPTURE(Performance, Sniffer, Format::kSniffer, Query::kSinglePredicate)
    ->Args({kDefaultRows, kDefaultRowGroupRows})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Performance, SnifferThreePredicates, Format::kSniffer, Query::kThreePredicates)
    ->Args({kDefaultRows, kDefaultRowGroupRows})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Performance, SnifferSortKeyRange, Format::kSniffer, Query::kSortKeyRange)
    ->Args({kDefaultRows, kDefaultRowGroupRows})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Performance, ArrowIPC, Format::kArrowIpc, Query::kSinglePredicate)
    ->Args({kDefaultRows, kDefaultRowGroupRows})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Performance, ArrowIPC_ZSTD, Format::kArrowIpcZstd, Query::kSinglePredicate)
    ->Args({kDefaultRows, kDefaultRowGroupRows})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(PerformanceMatrix, Sniffer, Format::kSniffer, Query::kSinglePredicate)
    ->Apply(ApplyPerformanceMatrix)
    ->ArgNames({"rows", "row_group_rows", "selectivity_percent", "projection_columns"})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

void AddBenchmarkContext() {
#ifdef NDEBUG
  benchmark::AddCustomContext("build_mode", "release");
#else
  benchmark::AddCustomContext("build_mode", "debug");
#endif
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
  benchmark::AddCustomContext("distribution", "grouped_id_linear_value_nullable");
  benchmark::AddCustomContext("predicate", "id_ge_half");
  benchmark::AddCustomContext("query_variants", "single_predicate,three_predicates,sort_key_range");
  benchmark::AddCustomContext("projection", "id,value");
  benchmark::AddCustomContext("matrix_dimensions",
                              "row_group_rows,selectivity_percent,projection_columns");
  benchmark::AddCustomContext(
      "memory_metrics", "Arrow allocation deltas per iteration; pool/RSS process high-water marks");
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
