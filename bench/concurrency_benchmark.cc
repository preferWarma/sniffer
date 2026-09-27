#include <arrow/api.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "benchmark_build_config.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr int64_t kRows = 500000;
constexpr uint32_t kRowGroupRows = 8192;

struct Fixture {
  std::filesystem::path path;
  uint64_t file_bytes = 0;
  std::unique_ptr<sniffer::SegmentReader> reader;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

arrow::Result<std::shared_ptr<Fixture>> MakeFixture() {
  auto fixture = std::make_shared<Fixture>();
  fixture->path =
      std::filesystem::temp_directory_path() /
      ("sniffer_concurrent_scans_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".seg");
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
  ARROW_ASSIGN_OR_RAISE(fixture->reader, sniffer::SegmentReader::Open(fixture->path.string()));
  return fixture;
}

const arrow::Result<std::shared_ptr<Fixture>>& SharedFixture() {
  static const auto fixture = MakeFixture();
  return fixture;
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
  for (auto _ : state) {
    (void)_;
    auto scanned = reader.Scan(plan);
    if (!scanned.ok()) {
      state.SkipWithError(scanned.status().ToString());
      break;
    }
    auto iterator = std::move(scanned).ValueUnsafe();
    int64_t selected_rows = 0;
    while (true) {
      auto next = iterator.Next();
      if (!next.ok()) {
        state.SkipWithError(next.status().ToString());
        return;
      }
      auto batch = std::move(next).ValueUnsafe();
      if (!batch) {
        break;
      }
      selected_rows += batch->num_rows();
      benchmark::DoNotOptimize(batch->column(1)->data().get());
    }
    if (selected_rows != kRows / 2) {
      state.SkipWithError("concurrent scan row count mismatch");
      break;
    }
  }
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
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
