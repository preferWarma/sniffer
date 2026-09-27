#include <arrow/api.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "parquet_benchmark_util.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr int64_t kRows = 500000;
constexpr uint32_t kRowGroupRows = 8192;

struct Fixture {
  std::filesystem::path path;
  uint64_t file_bytes = 0;
  std::filesystem::path parquet_path;
  uint64_t parquet_file_bytes = 0;
  std::filesystem::path parquet_zstd_path;
  uint64_t parquet_zstd_file_bytes = 0;
  std::unique_ptr<sniffer::SegmentReader> reader;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(parquet_path, ignored);
    std::filesystem::remove(parquet_zstd_path, ignored);
  }
};

arrow::Result<std::shared_ptr<Fixture>> MakeFixture() {
  auto fixture = std::make_shared<Fixture>();
  fixture->path =
      std::filesystem::temp_directory_path() /
      ("sniffer_concurrent_scans_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".seg");
  fixture->parquet_path = fixture->path;
  fixture->parquet_path.replace_extension(".parquet");
  fixture->parquet_zstd_path = fixture->path;
  fixture->parquet_zstd_path.replace_extension(".zstd.parquet");
  sniffer::TableSchema schema{
      1, {{1, "id", arrow::int64(), false, nullptr}, {2, "value", arrow::int64(), false, nullptr}}};
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
  arrow::Int64Builder ids;
  arrow::Int64Builder values;
  ARROW_RETURN_NOT_OK(ids.Reserve(kRows));
  ARROW_RETURN_NOT_OK(values.Reserve(kRows));
  for (int64_t row = 0; row < kRows; ++row) {
    ARROW_RETURN_NOT_OK(ids.Append(row));
    ARROW_RETURN_NOT_OK(values.Append(row * 17 + 3));
  }
  std::shared_ptr<arrow::Array> id_array;
  std::shared_ptr<arrow::Array> value_array;
  ARROW_RETURN_NOT_OK(ids.Finish(&id_array));
  ARROW_RETURN_NOT_OK(values.Finish(&value_array));
  auto batch = arrow::RecordBatch::Make(std::move(arrow_schema), kRows,
                                        {std::move(id_array), std::move(value_array)});
  sniffer::LayoutPolicy layout;
  layout.target_row_group_rows = kRowGroupRows;
  layout.sort_key_field_ids = {1};
  layout.statistics_field_ids = {1};
  ARROW_ASSIGN_OR_RAISE(auto writer,
                        sniffer::SegmentWriter::Open(fixture->path.string(), schema, layout));
  ARROW_RETURN_NOT_OK(writer->Append(batch));
  ARROW_RETURN_NOT_OK(writer->Finish());
  std::error_code file_error;
  fixture->file_bytes = std::filesystem::file_size(fixture->path, file_error);
  if (file_error) {
    return arrow::Status::IOError("cannot measure concurrent benchmark file: ",
                                  file_error.message());
  }
  ARROW_RETURN_NOT_OK(sniffer_bench::WriteParquet(fixture->parquet_path, *batch, kRowGroupRows,
                                                  parquet::Compression::UNCOMPRESSED));
  ARROW_RETURN_NOT_OK(sniffer_bench::WriteParquet(fixture->parquet_zstd_path, *batch, kRowGroupRows,
                                                  parquet::Compression::ZSTD));
  fixture->parquet_file_bytes = std::filesystem::file_size(fixture->parquet_path, file_error);
  if (file_error) {
    return arrow::Status::IOError("cannot measure Parquet benchmark file: ", file_error.message());
  }
  fixture->parquet_zstd_file_bytes =
      std::filesystem::file_size(fixture->parquet_zstd_path, file_error);
  if (file_error) {
    return arrow::Status::IOError("cannot measure Parquet ZSTD benchmark file: ",
                                  file_error.message());
  }
  ARROW_ASSIGN_OR_RAISE(fixture->reader, sniffer::SegmentReader::Open(fixture->path.string()));
  return fixture;
}

const arrow::Result<std::shared_ptr<Fixture>>& SharedFixture();

arrow::Result<int64_t> ScanParquet(parquet::arrow::FileReader* reader, bool verify_values) {
  const auto metadata = reader->parquet_reader()->metadata();
  std::vector<int> selected_row_groups;
  selected_row_groups.reserve(static_cast<size_t>(reader->num_row_groups()));
  for (int index = 0; index < reader->num_row_groups(); ++index) {
    const auto statistics = metadata->RowGroup(index)->ColumnChunk(0)->statistics();
    if (statistics && statistics->HasMinMax() &&
        statistics->physical_type() == parquet::Type::INT64 &&
        static_cast<const parquet::Int64Statistics&>(*statistics).max() < kRows / 2) {
      continue;
    }
    selected_row_groups.push_back(index);
  }
  ARROW_ASSIGN_OR_RAISE(auto stream,
                        reader->GetRecordBatchReader(selected_row_groups, std::vector<int>{0, 1}));
  int64_t selected_rows = 0;
  int64_t expected_id = kRows / 2;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto batch, stream->Next());
    if (!batch) {
      break;
    }
    const auto& ids = static_cast<const arrow::Int64Array&>(*batch->column(0));
    // This fixture's id column is sorted and non-null. Slice the boundary batch
    // without copying; other selected batches are already fully qualifying.
    const int64_t* begin = ids.raw_values();
    const int64_t* end = begin + batch->num_rows();
    const int64_t skip = std::lower_bound(begin, end, kRows / 2) - begin;
    auto selected = batch->Slice(skip);
    if (verify_values) {
      const auto& selected_ids = static_cast<const arrow::Int64Array&>(*selected->column(0));
      const auto& selected_values = static_cast<const arrow::Int64Array&>(*selected->column(1));
      for (int64_t row = 0; row < selected->num_rows(); ++row, ++expected_id) {
        if (selected_ids.IsNull(row) || selected_values.IsNull(row) ||
            selected_ids.Value(row) != expected_id ||
            selected_values.Value(row) != expected_id * 17 + 3) {
          return arrow::Status::Invalid("Parquet scan differs from generated reference values");
        }
      }
    }
    selected_rows += selected->num_rows();
    benchmark::DoNotOptimize(selected->column(1)->data().get());
  }
  if (verify_values && expected_id != kRows) {
    return arrow::Status::Invalid("Parquet scan omitted generated reference rows");
  }
  return selected_rows;
}

void ParquetConcurrentScan(benchmark::State& state, bool zstd) {
  const auto& fixture_result = SharedFixture();
  if (!fixture_result.ok()) {
    state.SkipWithError(fixture_result.status().ToString());
    return;
  }
  const auto& fixture = *fixture_result;
  const auto& path = zstd ? fixture->parquet_zstd_path : fixture->parquet_path;
  auto opened = sniffer_bench::OpenParquet(path, kRowGroupRows);
  if (!opened.ok()) {
    state.SkipWithError(opened.status().ToString());
    return;
  }
  auto reader = std::move(opened).ValueUnsafe();
  const auto verified = ScanParquet(reader.get(), true);
  if (!verified.ok() || *verified != kRows / 2) {
    state.SkipWithError(verified.ok() ? "Parquet reference scan row count mismatch"
                                      : verified.status().ToString());
    return;
  }
  for (auto _ : state) {
    (void)_;
    auto result = ScanParquet(reader.get(), false);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    if (*result != kRows / 2) {
      state.SkipWithError("Parquet concurrent scan row count mismatch");
      return;
    }
  }
  state.counters["input_rows"] = static_cast<double>(kRows);
  state.counters["selected_rows"] = static_cast<double>(kRows / 2);
  state.counters["row_group_rows"] = static_cast<double>(kRowGroupRows);
  state.counters["file_bytes"] =
      static_cast<double>(zstd ? fixture->parquet_zstd_file_bytes : fixture->parquet_file_bytes) /
      static_cast<double>(state.threads());
  state.SetItemsProcessed(state.iterations() * kRows * state.threads());
}

const arrow::Result<std::shared_ptr<Fixture>>& SharedFixture() {
  static const auto fixture = MakeFixture();
  return fixture;
}

arrow::Result<int64_t> ScanSniffer(const sniffer::SegmentReader& reader,
                                   const sniffer::IOPlan& plan, bool verify_values,
                                   const std::shared_ptr<sniffer::ScanMetrics>& metrics) {
  ARROW_ASSIGN_OR_RAISE(auto iterator, reader.Scan(plan, metrics));
  int64_t selected_rows = 0;
  int64_t expected_id = kRows / 2;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto batch, iterator.Next());
    if (!batch) {
      break;
    }
    if (verify_values) {
      const auto& ids = static_cast<const arrow::Int64Array&>(*batch->column(0));
      const auto& values = static_cast<const arrow::Int64Array&>(*batch->column(1));
      for (int64_t row = 0; row < batch->num_rows(); ++row, ++expected_id) {
        if (ids.IsNull(row) || values.IsNull(row) || ids.Value(row) != expected_id ||
            values.Value(row) != expected_id * 17 + 3) {
          return arrow::Status::Invalid("Sniffer scan differs from generated reference values");
        }
      }
    }
    selected_rows += batch->num_rows();
    benchmark::DoNotOptimize(batch->column(1)->data().get());
  }
  if (verify_values && expected_id != kRows) {
    return arrow::Status::Invalid("Sniffer scan omitted generated reference rows");
  }
  return selected_rows;
}

void ConcurrentScan(benchmark::State& state, bool share_reader) {
  const auto& fixture_result = SharedFixture();
  if (!fixture_result.ok()) {
    state.SkipWithError(fixture_result.status().ToString());
    return;
  }
  const auto& fixture = *fixture_result;
  std::unique_ptr<sniffer::SegmentReader> independent;
  if (!share_reader) {
    auto opened = sniffer::SegmentReader::Open(fixture->path.string());
    if (!opened.ok()) {
      state.SkipWithError(opened.status().ToString());
      return;
    }
    independent = std::move(opened).ValueUnsafe();
  }
  const auto& reader = share_reader ? *fixture->reader : *independent;
  sniffer::IOPlan plan;
  plan.projection_field_ids = {1, 2};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(kRows / 2)}};
  plan.output_batch_rows = kRowGroupRows;
  const auto verified = ScanSniffer(reader, plan, true, nullptr);
  if (!verified.ok() || *verified != kRows / 2) {
    state.SkipWithError(verified.ok() ? "Sniffer reference scan row count mismatch"
                                      : verified.status().ToString());
    return;
  }
  sniffer::ScanMetrics totals;
  for (auto _ : state) {
    (void)_;
    auto metrics = std::make_shared<sniffer::ScanMetrics>();
    auto result = ScanSniffer(reader, plan, false, metrics);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    if (*result != kRows / 2) {
      state.SkipWithError("concurrent scan row count mismatch");
      return;
    }
    totals.pruning_nanoseconds += metrics->pruning_nanoseconds;
    totals.chunk_io_nanoseconds += metrics->chunk_io_nanoseconds;
    totals.chunk_checksum_nanoseconds += metrics->chunk_checksum_nanoseconds;
    totals.decode_nanoseconds += metrics->decode_nanoseconds;
    totals.predicate_nanoseconds += metrics->predicate_nanoseconds;
    totals.projection_nanoseconds += metrics->projection_nanoseconds;
    totals.batch_materialization_nanoseconds += metrics->batch_materialization_nanoseconds;
    totals.output_concatenation_nanoseconds += metrics->output_concatenation_nanoseconds;
    totals.output_concatenations += metrics->output_concatenations;
  }
  const double metric_divisor =
      static_cast<double>(state.iterations()) * 1e6 * static_cast<double>(state.threads());
  const auto phase = [&](const char* name, uint64_t nanoseconds) {
    state.counters[name] = static_cast<double>(nanoseconds) / metric_divisor;
  };
  phase("prune_ms", totals.pruning_nanoseconds);
  phase("chunk_io_ms", totals.chunk_io_nanoseconds);
  phase("chunk_crc_ms", totals.chunk_checksum_nanoseconds);
  phase("decode_ms", totals.decode_nanoseconds);
  phase("predicate_ms", totals.predicate_nanoseconds);
  phase("projection_ms", totals.projection_nanoseconds);
  phase("batch_ms", totals.batch_materialization_nanoseconds);
  phase("concat_ms", totals.output_concatenation_nanoseconds);
  state.counters["concats"] =
      static_cast<double>(totals.output_concatenations) /
      (static_cast<double>(state.iterations()) * static_cast<double>(state.threads()));
  state.counters["input_rows"] = static_cast<double>(kRows);
  state.counters["selected_rows"] = static_cast<double>(kRows / 2);
  state.counters["row_group_rows"] = static_cast<double>(kRowGroupRows);
  state.counters["file_bytes"] =
      static_cast<double>(fixture->file_bytes) / static_cast<double>(state.threads());
  // Google Benchmark aggregates multi-thread counters but keeps ItemsProcessed
  // at the value supplied by a thread; account for all completed queries.
  state.SetItemsProcessed(state.iterations() * kRows * state.threads());
}

BENCHMARK_CAPTURE(ConcurrentScan, SharedReader, true)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ConcurrentScan, IndependentReaders, false)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ParquetConcurrentScan, Parquet, false)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(ParquetConcurrentScan, Parquet_ZSTD, true)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

}  // namespace

int main(int argc, char** argv) {
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
#ifdef NDEBUG
  benchmark::AddCustomContext("build_mode", "release");
#else
  benchmark::AddCustomContext("build_mode", "debug_or_sanitized");
#endif
  benchmark::AddCustomContext("data", "500000 int64 pairs, increasing id and affine value");
  benchmark::AddCustomContext("query", "id >= 250000, project both columns");
  benchmark::AddCustomContext("parquet_config",
                              "UNCOMPRESSED/ZSTD, dictionary defaults, row-group min/max pruning, "
                              "sorted-id boundary slicing, one reader per benchmark thread, "
                              "Arrow reader threads disabled");
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
