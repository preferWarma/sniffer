#include <arrow/api.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

#include "benchmark_build_config.h"
#include "parquet_benchmark_util.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr uint32_t kDefaultRowGroupRows = 8192;
constexpr uint32_t kOutputBatchRows = 4096;

struct Options {
  std::string generate_path;
  std::string generate_parquet_path;
  std::string generate_parquet_zstd_path;
  std::string segment_path;
  std::string parquet_path;
  std::string parquet_zstd_path;
  int64_t rows = 0;
  uint32_t unprojected_binary_bytes = 0;
  uint32_t projected_binary_bytes = 0;
  uint32_t projected_columns = 1;
  uint32_t selectivity_percent = 50;
  uint32_t row_group_rows = kDefaultRowGroupRows;
  uint64_t buffered_budget_bytes = 64U * 1024U * 1024U;
  bool cache_bypass = false;
};

Options options;

int64_t QueryLowerBound() {
  return options.rows * static_cast<int64_t>(100U - options.selectivity_percent) / 100;
}

uint64_t ExpectedRowGroups() {
  return (static_cast<uint64_t>(options.rows) + options.row_group_rows - 1U) /
         options.row_group_rows;
}

uint64_t ProcessPeakRssBytes() {
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

std::string BinaryValue(int64_t row, uint32_t width) {
  std::string value(width, 'x');
  const auto row_id = std::to_string(row);
  value.replace(0, row_id.size(), row_id);
  return value;
}

std::string ProjectedBinaryValue(int64_t row, uint32_t width, uint32_t projection = 0) {
  std::string value(width, '\0');
  uint64_t state =
      static_cast<uint64_t>(row) ^ (static_cast<uint64_t>(projection) * 0xD6E8FEB86659FD93ULL);
  for (uint32_t index = 0; index < width; ++index) {
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t sample = state;
    sample = (sample ^ (sample >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    sample = (sample ^ (sample >> 27U)) * 0x94D049BB133111EBULL;
    sample ^= sample >> 31U;
    value[index] = static_cast<char>(sample & 0xFFU);
  }
  return value;
}

arrow::Status VerifyGeneratedValues(const arrow::Array& values, int64_t* expected_row,
                                    uint32_t projected_binary_bytes, uint32_t projection = 0) {
  if (projected_binary_bytes > 0) {
    if (values.type_id() != arrow::Type::BINARY) {
      return arrow::Status::Invalid("generated benchmark expected binary values");
    }
    const auto& binary = static_cast<const arrow::BinaryArray&>(values);
    for (int64_t row = 0; row < binary.length(); ++row, ++*expected_row) {
      if (*expected_row % 17 == 0) {
        if (!binary.IsNull(row)) {
          return arrow::Status::Invalid("generated benchmark expected null binary value");
        }
      } else if (binary.IsNull(row) ||
                 binary.GetView(row) !=
                     ProjectedBinaryValue(*expected_row, projected_binary_bytes, projection)) {
        return arrow::Status::Invalid("generated benchmark binary value mismatch");
      }
    }
    return arrow::Status::OK();
  }
  if (values.type_id() != arrow::Type::INT64) {
    return arrow::Status::Invalid("generated benchmark expected int64 values");
  }
  const auto& integers = static_cast<const arrow::Int64Array&>(values);
  for (int64_t row = 0; row < integers.length(); ++row, ++*expected_row) {
    if (integers.IsNull(row) || integers.Value(row) != *expected_row) {
      return arrow::Status::Invalid("generated benchmark int64 value mismatch");
    }
  }
  return arrow::Status::OK();
}

int64_t ExtraValue(int64_t row, uint32_t projection_index) {
  return row * static_cast<int64_t>(projection_index + 1U) + projection_index;
}

arrow::Status VerifyGeneratedProjection(const arrow::RecordBatch& batch, int64_t skip,
                                        int first_column, int64_t* expected_row) {
  if (batch.num_columns() != first_column + static_cast<int>(options.projected_columns)) {
    return arrow::Status::Invalid("generated benchmark projection column count mismatch");
  }
  const int64_t first_row = *expected_row;
  ARROW_RETURN_NOT_OK(VerifyGeneratedValues(*batch.column(first_column)->Slice(skip), expected_row,
                                            options.projected_binary_bytes));
  for (uint32_t projection = 1; projection < options.projected_columns; ++projection) {
    const auto values = batch.column(first_column + static_cast<int>(projection))->Slice(skip);
    if (options.projected_binary_bytes > 0) {
      int64_t expected = first_row;
      ARROW_RETURN_NOT_OK(
          VerifyGeneratedValues(*values, &expected, options.projected_binary_bytes, projection));
      continue;
    }
    if (values->type_id() != arrow::Type::INT64) {
      return arrow::Status::Invalid("generated benchmark expected int64 extra column");
    }
    const auto& typed = static_cast<const arrow::Int64Array&>(*values);
    for (int64_t row = 0; row < typed.length(); ++row) {
      if (typed.IsNull(row) || typed.Value(row) != ExtraValue(first_row + row, projection)) {
        return arrow::Status::Invalid("generated benchmark extra column value mismatch");
      }
    }
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> MakeGeneratedBatch(
    const std::shared_ptr<arrow::Schema>& arrow_schema, int64_t offset, int64_t count,
    uint32_t unprojected_binary_bytes, uint32_t projected_binary_bytes,
    uint32_t projected_columns) {
  arrow::Int64Builder key_builder;
  ARROW_RETURN_NOT_OK(key_builder.Reserve(count));
  for (int64_t row = 0; row < count; ++row) {
    ARROW_RETURN_NOT_OK(key_builder.Append(offset + row));
  }
  std::shared_ptr<arrow::Array> key;
  std::shared_ptr<arrow::Array> value;
  ARROW_RETURN_NOT_OK(key_builder.Finish(&key));
  if (projected_binary_bytes > 0) {
    arrow::BinaryBuilder value_builder;
    for (int64_t row = 0; row < count; ++row) {
      const int64_t id = offset + row;
      if (id % 17 == 0) {
        ARROW_RETURN_NOT_OK(value_builder.AppendNull());
      } else {
        ARROW_RETURN_NOT_OK(value_builder.Append(ProjectedBinaryValue(id, projected_binary_bytes)));
      }
    }
    ARROW_RETURN_NOT_OK(value_builder.Finish(&value));
  } else {
    arrow::Int64Builder value_builder;
    ARROW_RETURN_NOT_OK(value_builder.Reserve(count));
    for (int64_t row = 0; row < count; ++row) {
      ARROW_RETURN_NOT_OK(value_builder.Append(offset + row));
    }
    ARROW_RETURN_NOT_OK(value_builder.Finish(&value));
  }
  std::vector<std::shared_ptr<arrow::Array>> columns = {std::move(key), std::move(value)};
  for (uint32_t projection = 1; projection < projected_columns; ++projection) {
    if (projected_binary_bytes > 0) {
      arrow::BinaryBuilder extra_builder;
      for (int64_t row = 0; row < count; ++row) {
        const int64_t id = offset + row;
        if (id % 17 == 0) {
          ARROW_RETURN_NOT_OK(extra_builder.AppendNull());
        } else {
          ARROW_RETURN_NOT_OK(
              extra_builder.Append(ProjectedBinaryValue(id, projected_binary_bytes, projection)));
        }
      }
      std::shared_ptr<arrow::Array> extra;
      ARROW_RETURN_NOT_OK(extra_builder.Finish(&extra));
      columns.push_back(std::move(extra));
      continue;
    }
    arrow::Int64Builder extra_builder;
    ARROW_RETURN_NOT_OK(extra_builder.Reserve(count));
    for (int64_t row = 0; row < count; ++row) {
      ARROW_RETURN_NOT_OK(extra_builder.Append(ExtraValue(offset + row, projection)));
    }
    std::shared_ptr<arrow::Array> extra;
    ARROW_RETURN_NOT_OK(extra_builder.Finish(&extra));
    columns.push_back(std::move(extra));
  }
  if (unprojected_binary_bytes > 0) {
    arrow::BinaryBuilder unused_builder;
    for (int64_t row = 0; row < count; ++row) {
      ARROW_RETURN_NOT_OK(
          unused_builder.Append(BinaryValue(offset + row, unprojected_binary_bytes)));
    }
    std::shared_ptr<arrow::Array> unused;
    ARROW_RETURN_NOT_OK(unused_builder.Finish(&unused));
    columns.push_back(std::move(unused));
  }
  return arrow::RecordBatch::Make(arrow_schema, count, std::move(columns));
}

arrow::Status CheckNewPath(const std::string& path) {
  std::error_code path_error;
  const bool exists = std::filesystem::exists(path, path_error);
  if (path_error) {
    return arrow::Status::IOError("cannot inspect output path: ", path_error.message());
  }
  if (exists) {
    return arrow::Status::Invalid("refusing to overwrite benchmark output: ", path);
  }
  return arrow::Status::OK();
}

arrow::Status GenerateSegment(const Options& config) {
  ARROW_RETURN_NOT_OK(CheckNewPath(config.generate_path));
  sniffer::TableSchema schema{
      1,
      {{1, "key", arrow::int64(), false, nullptr},
       {2, "value", config.projected_binary_bytes > 0 ? arrow::binary() : arrow::int64(),
        config.projected_binary_bytes > 0, nullptr}},
  };
  for (uint32_t projection = 1; projection < config.projected_columns; ++projection) {
    schema.fields.push_back({projection + 2U, "value_" + std::to_string(projection),
                             config.projected_binary_bytes > 0 ? arrow::binary() : arrow::int64(),
                             config.projected_binary_bytes > 0, nullptr});
  }
  if (config.unprojected_binary_bytes > 0) {
    schema.fields.push_back({3, "unused_payload", arrow::binary(), false, nullptr});
  }
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
  sniffer::LayoutPolicy layout;
  layout.target_row_group_rows = config.row_group_rows;
  layout.sort_key_field_ids = {1};
  layout.statistics_field_ids = {1};
  ARROW_ASSIGN_OR_RAISE(auto writer,
                        sniffer::SegmentWriter::Open(config.generate_path, schema, layout));
  for (int64_t offset = 0; offset < config.rows; offset += config.row_group_rows) {
    const int64_t count = std::min<int64_t>(config.row_group_rows, config.rows - offset);
    ARROW_ASSIGN_OR_RAISE(
        auto batch, MakeGeneratedBatch(arrow_schema, offset, count, config.unprojected_binary_bytes,
                                       config.projected_binary_bytes, config.projected_columns));
    ARROW_RETURN_NOT_OK(writer->Append(std::move(batch)));
  }
  return writer->Finish();
}

arrow::Status GenerateParquet(const Options& config, const std::string& path,
                              parquet::Compression::type compression) {
  ARROW_RETURN_NOT_OK(CheckNewPath(path));
  sniffer::TableSchema schema{
      1,
      {{1, "key", arrow::int64(), false, nullptr},
       {2, "value", config.projected_binary_bytes > 0 ? arrow::binary() : arrow::int64(),
        config.projected_binary_bytes > 0, nullptr}},
  };
  for (uint32_t projection = 1; projection < config.projected_columns; ++projection) {
    schema.fields.push_back({projection + 2U, "value_" + std::to_string(projection),
                             config.projected_binary_bytes > 0 ? arrow::binary() : arrow::int64(),
                             config.projected_binary_bytes > 0, nullptr});
  }
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
  ARROW_ASSIGN_OR_RAISE(auto output, arrow::io::FileOutputStream::Open(path));
  parquet::WriterProperties::Builder properties;
  properties.compression(compression);
  properties.max_row_group_length(config.row_group_rows);
  parquet::ArrowWriterProperties::Builder arrow_properties;
  arrow_properties.set_use_threads(false);
  arrow_properties.store_schema();
  ARROW_ASSIGN_OR_RAISE(auto writer, parquet::arrow::FileWriter::Open(
                                         *arrow_schema, arrow::default_memory_pool(), output,
                                         properties.build(), arrow_properties.build()));
  for (int64_t offset = 0; offset < config.rows; offset += config.row_group_rows) {
    const int64_t count = std::min<int64_t>(config.row_group_rows, config.rows - offset);
    ARROW_ASSIGN_OR_RAISE(
        auto batch, MakeGeneratedBatch(arrow_schema, offset, count, 0,
                                       config.projected_binary_bytes, config.projected_columns));
    ARROW_RETURN_NOT_OK(writer->NewBufferedRowGroup());
    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
  }
  ARROW_RETURN_NOT_OK(writer->Close());
  return output->Close();
}

void ReaderOnlyScanImpl(benchmark::State& state, uint32_t workers) {
  auto maybe_reader = sniffer::SegmentReader::OpenWithOptions(
      options.segment_path, sniffer::ReaderOpenOptions{.bypass_os_cache = options.cache_bypass});
  if (!maybe_reader.ok()) {
    state.SkipWithError(maybe_reader.status().ToString());
    return;
  }
  auto reader = std::move(*maybe_reader);
  if (reader->num_row_groups() != ExpectedRowGroups()) {
    state.SkipWithError("Segment Row Group count differs from --row-group-rows");
    return;
  }
  sniffer::IOPlan plan;
  for (uint32_t projection = 0; projection < options.projected_columns; ++projection) {
    plan.projection_field_ids.push_back(projection + 2U);
  }
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(QueryLowerBound())}};
  plan.output_batch_rows = kOutputBatchRows;
  const uint64_t expected_rows = static_cast<uint64_t>(options.rows - QueryLowerBound());
  {
    sniffer::ScanExecutionOptions checked_execution;
    checked_execution.worker_count = workers;
    checked_execution.max_in_flight_row_groups = std::max<uint32_t>(workers, 2U);
    checked_execution.max_buffered_bytes = options.buffered_budget_bytes;
    auto checked = reader->Scan(plan, checked_execution);
    if (!checked.ok()) {
      state.SkipWithError(checked.status().ToString());
      return;
    }
    auto iterator = std::move(checked).ValueUnsafe();
    int64_t expected_value = QueryLowerBound();
    for (;;) {
      auto next = iterator.Next();
      if (!next.ok()) {
        state.SkipWithError(next.status().ToString());
        return;
      }
      auto batch = std::move(next).ValueUnsafe();
      if (!batch) {
        break;
      }
      const auto status = VerifyGeneratedProjection(*batch, 0, 0, &expected_value);
      if (!status.ok()) {
        state.SkipWithError(status.ToString());
        return;
      }
    }
    if (expected_value != options.rows) {
      state.SkipWithError("Sniffer scan omitted generated reference rows");
      return;
    }
  }
  sniffer::ScanMetrics totals;
  uint64_t peak_in_flight = 0;
  uint64_t peak_reserved_bytes = 0;
  uint64_t workers_started = 0;
  for (auto _ : state) {
    auto metrics = std::make_shared<sniffer::ScanMetrics>();
    sniffer::ScanExecutionOptions execution;
    execution.worker_count = workers;
    execution.max_in_flight_row_groups = std::max<uint32_t>(workers, 2U);
    execution.max_buffered_bytes = options.buffered_budget_bytes;
    auto maybe_iterator = reader->Scan(plan, execution, metrics);
    if (!maybe_iterator.ok()) {
      state.SkipWithError(maybe_iterator.status().ToString());
      return;
    }
    auto iterator = std::move(*maybe_iterator);
    uint64_t output_rows = 0;
    while (true) {
      auto maybe_batch = iterator.Next();
      if (!maybe_batch.ok()) {
        state.SkipWithError(maybe_batch.status().ToString());
        return;
      }
      if (!*maybe_batch) {
        break;
      }
      output_rows += static_cast<uint64_t>((*maybe_batch)->num_rows());
      for (const auto& column : (*maybe_batch)->columns()) {
        benchmark::DoNotOptimize(column->data().get());
      }
    }
    if (output_rows != expected_rows) {
      state.SkipWithError("reader-only scan row count mismatch");
      return;
    }
    totals.row_groups_pruned += metrics->row_groups_pruned;
    totals.column_chunks_read += metrics->column_chunks_read;
    totals.chunk_bytes_read += metrics->chunk_bytes_read;
    totals.pruning_nanoseconds += metrics->pruning_nanoseconds;
    totals.chunk_io_nanoseconds += metrics->chunk_io_nanoseconds;
    totals.chunk_checksum_nanoseconds += metrics->chunk_checksum_nanoseconds;
    totals.decode_nanoseconds += metrics->decode_nanoseconds;
    totals.predicate_nanoseconds += metrics->predicate_nanoseconds;
    totals.projection_nanoseconds += metrics->projection_nanoseconds;
    totals.batch_materialization_nanoseconds += metrics->batch_materialization_nanoseconds;
    peak_in_flight = std::max(peak_in_flight, metrics->parallel_peak_in_flight_row_groups);
    peak_reserved_bytes = std::max(peak_reserved_bytes, metrics->parallel_peak_reserved_bytes);
    workers_started = std::max(workers_started, metrics->parallel_workers_started);
  }
  std::error_code file_error;
  const uint64_t file_bytes = std::filesystem::file_size(options.segment_path, file_error);
  if (file_error) {
    state.SkipWithError(file_error.message());
    return;
  }
  state.counters["input_rows"] = static_cast<double>(options.rows);
  state.counters["output_rows"] = static_cast<double>(expected_rows);
  state.counters["selectivity_percent"] = static_cast<double>(options.selectivity_percent);
  state.counters["file_bytes"] = static_cast<double>(file_bytes);
  state.counters["row_groups"] = static_cast<double>(reader->num_row_groups());
  state.counters["workers"] = static_cast<double>(workers);
  state.counters["parallel_workers_started"] = static_cast<double>(workers_started);
  state.counters["buffered_budget_bytes"] = static_cast<double>(options.buffered_budget_bytes);
  state.counters["parallel_peak_in_flight"] = static_cast<double>(peak_in_flight);
  state.counters["parallel_peak_reserved_bytes"] = static_cast<double>(peak_reserved_bytes);
  const auto per_scan = [&state](const char* name, uint64_t count) {
    state.counters[name] = static_cast<double>(count) / static_cast<double>(state.iterations());
  };
  per_scan("row_groups_pruned", totals.row_groups_pruned);
  per_scan("column_chunks_read", totals.column_chunks_read);
  per_scan("chunk_bytes_read", totals.chunk_bytes_read);
  per_scan("pruning_ns", totals.pruning_nanoseconds);
  per_scan("chunk_io_ns", totals.chunk_io_nanoseconds);
  per_scan("chunk_checksum_ns", totals.chunk_checksum_nanoseconds);
  per_scan("decode_ns", totals.decode_nanoseconds);
  per_scan("predicate_ns", totals.predicate_nanoseconds);
  per_scan("projection_ns", totals.projection_nanoseconds);
  per_scan("batch_materialization_ns", totals.batch_materialization_nanoseconds);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(ProcessPeakRssBytes());
  state.counters["arrow_pool_peak_bytes"] =
      static_cast<double>(arrow::default_memory_pool()->max_memory());
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(expected_rows));
}

void ReaderOnlyScan(benchmark::State& state) { ReaderOnlyScanImpl(state, 1); }

void BoundedParallelReaderScan(benchmark::State& state) {
  ReaderOnlyScanImpl(state, static_cast<uint32_t>(state.range(0)));
}

BENCHMARK(ReaderOnlyScan)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BoundedParallelReaderScan)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

struct ParquetMeasurement {
  uint64_t output_rows = 0;
  uint64_t row_groups_pruned = 0;
  uint64_t candidate_column_bytes = 0;
  uint64_t candidate_column_chunks = 0;
};

arrow::Result<ParquetMeasurement> ScanParquetOnce(parquet::arrow::FileReader* reader, int64_t rows,
                                                  bool verify_values) {
  const int64_t lower = QueryLowerBound();
  const auto metadata = reader->parquet_reader()->metadata();
  std::vector<int> full_groups;
  std::optional<int> boundary_group;
  ParquetMeasurement measured;
  for (int group = 0; group < reader->num_row_groups(); ++group) {
    const auto row_group = metadata->RowGroup(group);
    const auto statistics = row_group->ColumnChunk(0)->statistics();
    if (!statistics || !statistics->HasMinMax() ||
        statistics->physical_type() != parquet::Type::INT64) {
      return arrow::Status::Invalid("Parquet benchmark requires int64 key statistics");
    }
    const auto& key_stats = static_cast<const parquet::Int64Statistics&>(*statistics);
    if (key_stats.max() < lower) {
      ++measured.row_groups_pruned;
      continue;
    }
    for (uint32_t projection = 0; projection < options.projected_columns; ++projection) {
      const int64_t value_bytes =
          row_group->ColumnChunk(static_cast<int>(projection + 1U))->total_compressed_size();
      if (value_bytes < 0) {
        return arrow::Status::Invalid("Parquet benchmark has invalid column byte length");
      }
      measured.candidate_column_bytes += static_cast<uint64_t>(value_bytes);
      ++measured.candidate_column_chunks;
    }
    if (key_stats.min() >= lower) {
      full_groups.push_back(group);
    } else {
      if (boundary_group || !full_groups.empty()) {
        return arrow::Status::Invalid("Parquet benchmark expects one leading boundary group");
      }
      boundary_group = group;
      const int64_t key_bytes = row_group->ColumnChunk(0)->total_compressed_size();
      if (key_bytes < 0) {
        return arrow::Status::Invalid("Parquet benchmark has invalid key byte length");
      }
      measured.candidate_column_bytes += static_cast<uint64_t>(key_bytes);
      ++measured.candidate_column_chunks;
    }
  }

  std::vector<int> full_columns;
  std::vector<int> boundary_columns = {0};
  for (uint32_t projection = 0; projection < options.projected_columns; ++projection) {
    full_columns.push_back(static_cast<int>(projection + 1U));
    boundary_columns.push_back(static_cast<int>(projection + 1U));
  }
  int64_t expected_value = lower;
  const auto consume = [&](const std::vector<int>& groups, const std::vector<int>& columns,
                           bool filter_boundary) -> arrow::Status {
    if (groups.empty()) {
      return arrow::Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(auto stream, reader->GetRecordBatchReader(groups, columns));
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(auto batch, stream->Next());
      if (!batch) {
        break;
      }
      int64_t skip = 0;
      if (filter_boundary) {
        const auto& keys = static_cast<const arrow::Int64Array&>(*batch->column(0));
        skip = std::lower_bound(keys.raw_values(), keys.raw_values() + keys.length(), lower) -
               keys.raw_values();
      }
      if (verify_values) {
        ARROW_RETURN_NOT_OK(
            VerifyGeneratedProjection(*batch, skip, filter_boundary ? 1 : 0, &expected_value));
      }
      measured.output_rows += static_cast<uint64_t>(batch->num_rows() - skip);
      for (int column = filter_boundary ? 1 : 0; column < batch->num_columns(); ++column) {
        benchmark::DoNotOptimize(batch->column(column)->data().get());
      }
    }
    return arrow::Status::OK();
  };
  if (boundary_group) {
    ARROW_RETURN_NOT_OK(consume({*boundary_group}, boundary_columns, true));
  }
  ARROW_RETURN_NOT_OK(consume(full_groups, full_columns, false));
  if (measured.output_rows != static_cast<uint64_t>(rows - lower) ||
      (verify_values && expected_value != rows)) {
    return arrow::Status::Invalid("Parquet benchmark output row count mismatch");
  }
  return measured;
}

void ParquetReaderOnlyScan(benchmark::State& state, bool zstd) {
  const auto& path = zstd ? options.parquet_zstd_path : options.parquet_path;
  if (path.empty()) {
    state.SkipWithError(zstd ? "--parquet-zstd=PATH is required" : "--parquet=PATH is required");
    return;
  }
  auto opened = sniffer_bench::OpenParquet(path, kOutputBatchRows, options.cache_bypass);
  if (!opened.ok()) {
    state.SkipWithError(opened.status().ToString());
    return;
  }
  auto reader = std::move(opened).ValueUnsafe();
  if (static_cast<uint64_t>(reader->num_row_groups()) != ExpectedRowGroups()) {
    state.SkipWithError("Parquet Row Group count differs from --row-group-rows");
    return;
  }
  const auto verified = ScanParquetOnce(reader.get(), options.rows, true);
  if (!verified.ok()) {
    state.SkipWithError(verified.status().ToString());
    return;
  }
  ParquetMeasurement last;
  for (auto _ : state) {
    (void)_;
    auto result = ScanParquetOnce(reader.get(), options.rows, false);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    last = *result;
  }
  std::error_code file_error;
  const uint64_t file_bytes = std::filesystem::file_size(path, file_error);
  if (file_error) {
    state.SkipWithError(file_error.message());
    return;
  }
  state.counters["input_rows"] = static_cast<double>(options.rows);
  state.counters["output_rows"] = static_cast<double>(last.output_rows);
  state.counters["selectivity_percent"] = static_cast<double>(options.selectivity_percent);
  state.counters["file_bytes"] = static_cast<double>(file_bytes);
  state.counters["row_groups"] = static_cast<double>(reader->num_row_groups());
  state.counters["row_groups_pruned"] = static_cast<double>(last.row_groups_pruned);
  state.counters["candidate_column_chunks"] = static_cast<double>(last.candidate_column_chunks);
  state.counters["candidate_column_bytes"] = static_cast<double>(last.candidate_column_bytes);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(ProcessPeakRssBytes());
  state.counters["arrow_pool_peak_bytes"] =
      static_cast<double>(arrow::default_memory_pool()->max_memory());
  state.SetItemsProcessed(state.iterations() * (options.rows - QueryLowerBound()));
}

BENCHMARK_CAPTURE(ParquetReaderOnlyScan, Uncompressed, false)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ParquetReaderOnlyScan, ZSTD, true)->UseRealTime()->Unit(benchmark::kMillisecond);

sniffer::IOPlan ShardPlan(int64_t lower, int64_t upper) {
  sniffer::IOPlan plan;
  plan.projection_field_ids = {2};
  sniffer::SortKeyRange range;
  range.lower =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(lower)};
  range.upper =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(upper)};
  plan.sort_key_range = std::move(range);
  plan.output_batch_rows = kOutputBatchRows;
  return plan;
}

arrow::Result<uint64_t> ScanShard(sniffer::SegmentReader& reader, const sniffer::IOPlan& plan,
                                  int64_t lower, int64_t upper, bool validate_values,
                                  std::shared_ptr<sniffer::ScanMetrics> metrics) {
  ARROW_ASSIGN_OR_RAISE(auto iterator, reader.Scan(plan, std::move(metrics)));
  uint64_t output_rows = 0;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto batch, iterator.Next());
    if (!batch) {
      break;
    }
    if (validate_values) {
      if (batch->num_columns() != 1) {
        return arrow::Status::TypeError("sharded scan expects one projection column");
      }
      int64_t expected_row = lower + static_cast<int64_t>(output_rows);
      ARROW_RETURN_NOT_OK(
          VerifyGeneratedValues(*batch->column(0), &expected_row, options.projected_binary_bytes));
    }
    output_rows += static_cast<uint64_t>(batch->num_rows());
    benchmark::DoNotOptimize(batch->column(0)->data().get());
  }
  if (output_rows != static_cast<uint64_t>(upper - lower)) {
    return arrow::Status::Invalid("sharded scan row count mismatch");
  }
  return output_rows;
}

void ShardedSortRangeScan(benchmark::State& state) {
  auto maybe_reader = sniffer::SegmentReader::OpenWithOptions(
      options.segment_path, sniffer::ReaderOpenOptions{.bypass_os_cache = options.cache_bypass});
  if (!maybe_reader.ok()) {
    state.SkipWithError(maybe_reader.status().ToString());
    return;
  }
  auto reader = std::move(*maybe_reader);
  if (reader->num_row_groups() != ExpectedRowGroups()) {
    state.SkipWithError("Segment Row Group count differs from --row-group-rows");
    return;
  }
  const int64_t workers = state.range(0);
  const int64_t first = QueryLowerBound();
  const int64_t matching_rows = options.rows - first;
  if (workers > matching_rows) {
    state.SkipWithError("worker count exceeds matching row count");
    return;
  }
  std::vector<sniffer::IOPlan> plans;
  std::vector<std::pair<int64_t, int64_t>> bounds;
  plans.reserve(static_cast<size_t>(workers));
  bounds.reserve(static_cast<size_t>(workers));
  for (int64_t worker = 0; worker < workers; ++worker) {
    const int64_t lower = first + matching_rows * worker / workers;
    const int64_t upper = first + matching_rows * (worker + 1) / workers;
    bounds.emplace_back(lower, upper);
    plans.push_back(ShardPlan(lower, upper));
    const auto check = ScanShard(*reader, plans.back(), lower, upper, true, nullptr);
    if (!check.ok()) {
      state.SkipWithError(check.status().ToString());
      return;
    }
  }
  uint64_t chunk_bytes_read = 0;
  for (auto _ : state) {
    std::vector<std::jthread> threads;
    std::vector<arrow::Status> statuses(static_cast<size_t>(workers), arrow::Status::OK());
    std::vector<std::shared_ptr<sniffer::ScanMetrics>> metrics(static_cast<size_t>(workers));
    threads.reserve(static_cast<size_t>(workers));
    for (int64_t worker = 0; worker < workers; ++worker) {
      const size_t slot = static_cast<size_t>(worker);
      metrics[slot] = std::make_shared<sniffer::ScanMetrics>();
      threads.emplace_back([&, slot] {
        const auto [lower, upper] = bounds[slot];
        const auto result = ScanShard(*reader, plans[slot], lower, upper, false, metrics[slot]);
        if (!result.ok()) {
          statuses[slot] = result.status();
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    chunk_bytes_read = 0;
    for (size_t slot = 0; slot < static_cast<size_t>(workers); ++slot) {
      if (!statuses[slot].ok()) {
        state.SkipWithError(statuses[slot].ToString());
        return;
      }
      chunk_bytes_read += metrics[slot]->chunk_bytes_read;
    }
  }
  state.counters["workers"] = static_cast<double>(workers);
  state.counters["input_rows"] = static_cast<double>(options.rows);
  state.counters["output_rows"] = static_cast<double>(matching_rows);
  state.counters["chunk_bytes_read"] = static_cast<double>(chunk_bytes_read);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(ProcessPeakRssBytes());
  state.counters["arrow_pool_peak_bytes"] =
      static_cast<double>(arrow::default_memory_pool()->max_memory());
  state.SetItemsProcessed(state.iterations() * matching_rows);
}

// This shards one logical range query through independent public Scan iterators.
// It is an experimental scheduling bound, not production in-Reader parallelism.
BENCHMARK(ShardedSortRangeScan)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

}  // namespace

int main(int argc, char** argv) {
  std::vector<char*> benchmark_args = {argv[0]};
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument.starts_with("--generate=")) {
      options.generate_path = argument.substr(std::string_view("--generate=").size());
    } else if (argument.starts_with("--generate-parquet=")) {
      options.generate_parquet_path =
          argument.substr(std::string_view("--generate-parquet=").size());
    } else if (argument.starts_with("--generate-parquet-zstd=")) {
      options.generate_parquet_zstd_path =
          argument.substr(std::string_view("--generate-parquet-zstd=").size());
    } else if (argument.starts_with("--segment=")) {
      options.segment_path = argument.substr(std::string_view("--segment=").size());
    } else if (argument.starts_with("--parquet=")) {
      options.parquet_path = argument.substr(std::string_view("--parquet=").size());
    } else if (argument.starts_with("--parquet-zstd=")) {
      options.parquet_zstd_path = argument.substr(std::string_view("--parquet-zstd=").size());
    } else if (argument.starts_with("--rows=")) {
      const auto digits = argument.substr(std::string_view("--rows=").size());
      const auto [end, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), options.rows);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --rows value\n";
        return 1;
      }
    } else if (argument.starts_with("--unprojected-binary-bytes=")) {
      const auto digits = argument.substr(std::string_view("--unprojected-binary-bytes=").size());
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                options.unprojected_binary_bytes);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --unprojected-binary-bytes value\n";
        return 1;
      }
    } else if (argument.starts_with("--projected-binary-bytes=")) {
      const auto digits = argument.substr(std::string_view("--projected-binary-bytes=").size());
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                options.projected_binary_bytes);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --projected-binary-bytes value\n";
        return 1;
      }
    } else if (argument.starts_with("--projected-columns=")) {
      const auto digits = argument.substr(std::string_view("--projected-columns=").size());
      const auto [end, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), options.projected_columns);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --projected-columns value\n";
        return 1;
      }
    } else if (argument.starts_with("--selectivity-percent=")) {
      const auto digits = argument.substr(std::string_view("--selectivity-percent=").size());
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                options.selectivity_percent);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --selectivity-percent value\n";
        return 1;
      }
    } else if (argument.starts_with("--row-group-rows=")) {
      const auto digits = argument.substr(std::string_view("--row-group-rows=").size());
      const auto [end, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), options.row_group_rows);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --row-group-rows value\n";
        return 1;
      }
    } else if (argument.starts_with("--buffer-budget-bytes=")) {
      const auto digits = argument.substr(std::string_view("--buffer-budget-bytes=").size());
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                options.buffered_budget_bytes);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --buffer-budget-bytes value\n";
        return 1;
      }
    } else if (argument == "--cache-bypass") {
      options.cache_bypass = true;
    } else {
      benchmark_args.push_back(argv[index]);
    }
  }
  const int mode_count = static_cast<int>(!options.generate_path.empty()) +
                         static_cast<int>(!options.generate_parquet_path.empty()) +
                         static_cast<int>(!options.generate_parquet_zstd_path.empty()) +
                         static_cast<int>(!options.segment_path.empty());
  if (options.rows <= 0 || options.rows > 100000000 || mode_count != 1 ||
      options.unprojected_binary_bytes > 4096 ||
      (options.unprojected_binary_bytes > 0 && options.unprojected_binary_bytes < 20) ||
      options.projected_binary_bytes > 4096 ||
      (options.projected_binary_bytes > 0 && options.projected_binary_bytes < 20) ||
      (options.unprojected_binary_bytes > 0 && options.projected_binary_bytes > 0) ||
      options.projected_columns == 0 || options.projected_columns > 15 ||
      options.selectivity_percent == 0 || options.selectivity_percent > 100 ||
      options.row_group_rows == 0 || options.row_group_rows > 262144 ||
      (options.projected_columns > 1 && options.unprojected_binary_bytes > 0) ||
      options.buffered_budget_bytes == 0 ||
      (options.cache_bypass && options.segment_path.empty()) ||
      (options.unprojected_binary_bytes > 0 && options.generate_path.empty())) {
    std::cerr << "use 1 <= --rows=N <= 100000000 with exactly one of "
                 "--generate=PATH, --generate-parquet=PATH, "
                 "--generate-parquet-zstd=PATH or --segment=PATH; optional Segment-only "
                 "--unprojected-binary-bytes=20..4096 (Segment generator only), "
                 "--projected-binary-bytes=20..4096, --projected-columns=1..15; "
                 "--selectivity-percent=1..100, --row-group-rows=1..262144 "
                 "positive --buffer-budget-bytes=N; scan-only --cache-bypass on macOS\n";
    return 1;
  }
  if (!options.generate_path.empty()) {
    const auto status = GenerateSegment(options);
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return 1;
    }
    return 0;
  }
  if (!options.generate_parquet_path.empty()) {
    const auto status =
        GenerateParquet(options, options.generate_parquet_path, parquet::Compression::UNCOMPRESSED);
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return 1;
    }
    return 0;
  }
  if (!options.generate_parquet_zstd_path.empty()) {
    const auto status =
        GenerateParquet(options, options.generate_parquet_zstd_path, parquet::Compression::ZSTD);
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return 1;
    }
    return 0;
  }
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
  benchmark::AddCustomContext("scope", "fresh_process_reader_only_no_writer_or_input_batch");
  benchmark::AddCustomContext("parquet_compression", "UNCOMPRESSED and ZSTD; explicit input paths");
  benchmark::AddCustomContext("row_group_rows", std::to_string(options.row_group_rows));
  benchmark::AddCustomContext("cache_mode", options.cache_bypass ? "macOS_F_NOCACHE" : "default");
  benchmark::AddCustomContext("output_batch_rows", std::to_string(kOutputBatchRows));
  benchmark::AddCustomContext("buffered_budget_bytes",
                              std::to_string(options.buffered_budget_bytes));
  benchmark::AddCustomContext("projected_binary_bytes",
                              std::to_string(options.projected_binary_bytes));
  benchmark::AddCustomContext("projected_columns", std::to_string(options.projected_columns));
  benchmark::AddCustomContext("selectivity_percent", std::to_string(options.selectivity_percent));
  benchmark::AddCustomContext(
      "query",
      "ReaderOnlyScan, BoundedParallelReaderScan and ParquetReaderOnlyScan: "
      "key >= rows * (100 - selectivity_percent) / 100, project value and optional extra columns; "
      "ShardedSortRangeScan: partition sort-key range "
      "[lower,rows); project value");
  int benchmark_argc = static_cast<int>(benchmark_args.size());
  benchmark::Initialize(&benchmark_argc, benchmark_args.data());
  if (benchmark::ReportUnrecognizedArguments(benchmark_argc, benchmark_args.data())) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
