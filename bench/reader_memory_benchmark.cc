#include <arrow/api.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

#include "benchmark_build_config.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

constexpr uint32_t kRowGroupRows = 8192;
constexpr uint32_t kOutputBatchRows = 4096;

struct Options {
  std::string generate_path;
  std::string segment_path;
  int64_t rows = 0;
};

Options options;

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

arrow::Status GenerateSegment(const Options& config) {
  std::error_code path_error;
  const bool exists = std::filesystem::exists(config.generate_path, path_error);
  if (path_error) {
    return arrow::Status::IOError("cannot inspect output path: ", path_error.message());
  }
  if (exists) {
    return arrow::Status::Invalid("refusing to overwrite existing benchmark Segment");
  }
  const sniffer::TableSchema schema{
      1,
      {{1, "key", arrow::int64(), false, nullptr}, {2, "value", arrow::int64(), false, nullptr}},
  };
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());
  sniffer::LayoutPolicy layout;
  layout.target_row_group_rows = kRowGroupRows;
  layout.sort_key_field_ids = {1};
  layout.statistics_field_ids = {1};
  ARROW_ASSIGN_OR_RAISE(auto writer,
                        sniffer::SegmentWriter::Open(config.generate_path, schema, layout));
  for (int64_t offset = 0; offset < config.rows; offset += kRowGroupRows) {
    const int64_t count = std::min<int64_t>(kRowGroupRows, config.rows - offset);
    arrow::Int64Builder key_builder;
    arrow::Int64Builder value_builder;
    ARROW_RETURN_NOT_OK(key_builder.Reserve(count));
    ARROW_RETURN_NOT_OK(value_builder.Reserve(count));
    for (int64_t row = 0; row < count; ++row) {
      ARROW_RETURN_NOT_OK(key_builder.Append(offset + row));
      ARROW_RETURN_NOT_OK(value_builder.Append(offset + row));
    }
    std::shared_ptr<arrow::Array> key;
    std::shared_ptr<arrow::Array> value;
    ARROW_RETURN_NOT_OK(key_builder.Finish(&key));
    ARROW_RETURN_NOT_OK(value_builder.Finish(&value));
    auto batch = arrow::RecordBatch::Make(arrow_schema, count, {std::move(key), std::move(value)});
    ARROW_RETURN_NOT_OK(writer->Append(std::move(batch)));
  }
  return writer->Finish();
}

void ReaderOnlyScan(benchmark::State& state) {
  auto maybe_reader = sniffer::SegmentReader::Open(options.segment_path);
  if (!maybe_reader.ok()) {
    state.SkipWithError(maybe_reader.status().ToString());
    return;
  }
  auto reader = std::move(*maybe_reader);
  sniffer::IOPlan plan;
  plan.projection_field_ids = {2};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(options.rows / 2)}};
  plan.output_batch_rows = kOutputBatchRows;
  const uint64_t expected_rows = static_cast<uint64_t>(options.rows - options.rows / 2);
  sniffer::ScanMetrics totals;
  for (auto _ : state) {
    auto metrics = std::make_shared<sniffer::ScanMetrics>();
    auto maybe_iterator = reader->Scan(plan, metrics);
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
      benchmark::DoNotOptimize((*maybe_batch)->column(0)->data().get());
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
  }
  std::error_code file_error;
  const uint64_t file_bytes = std::filesystem::file_size(options.segment_path, file_error);
  if (file_error) {
    state.SkipWithError(file_error.message());
    return;
  }
  state.counters["input_rows"] = static_cast<double>(options.rows);
  state.counters["output_rows"] = static_cast<double>(expected_rows);
  state.counters["file_bytes"] = static_cast<double>(file_bytes);
  state.counters["row_groups"] = static_cast<double>(reader->num_row_groups());
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

BENCHMARK(ReaderOnlyScan)->UseRealTime()->Unit(benchmark::kMillisecond);

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
      if (batch->num_columns() != 1 || batch->column(0)->type_id() != arrow::Type::INT64) {
        return arrow::Status::TypeError("sharded scan expects one int64 projection column");
      }
      const auto& values = static_cast<const arrow::Int64Array&>(*batch->column(0));
      for (int64_t row = 0; row < batch->num_rows(); ++row) {
        if (values.IsNull(row) ||
            values.Value(row) != lower + static_cast<int64_t>(output_rows) + row) {
          return arrow::Status::Invalid("sharded scan value or row-order mismatch");
        }
      }
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
  auto maybe_reader = sniffer::SegmentReader::Open(options.segment_path);
  if (!maybe_reader.ok()) {
    state.SkipWithError(maybe_reader.status().ToString());
    return;
  }
  auto reader = std::move(*maybe_reader);
  const int64_t workers = state.range(0);
  const int64_t first = options.rows / 2;
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
    } else if (argument.starts_with("--segment=")) {
      options.segment_path = argument.substr(std::string_view("--segment=").size());
    } else if (argument.starts_with("--rows=")) {
      const auto digits = argument.substr(std::string_view("--rows=").size());
      const auto [end, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), options.rows);
      if (error != std::errc{} || end != digits.data() + digits.size()) {
        std::cerr << "invalid --rows value\n";
        return 1;
      }
    } else {
      benchmark_args.push_back(argv[index]);
    }
  }
  if (options.rows <= 0 || options.rows > 100000000 ||
      (options.generate_path.empty() == options.segment_path.empty())) {
    std::cerr << "use 1 <= --rows=N <= 100000000 with exactly one of "
                 "--generate=PATH or --segment=PATH\n";
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
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
  benchmark::AddCustomContext("scope", "fresh_process_reader_only_no_writer_or_input_batch");
  benchmark::AddCustomContext("row_group_rows", std::to_string(kRowGroupRows));
  benchmark::AddCustomContext("output_batch_rows", std::to_string(kOutputBatchRows));
  benchmark::AddCustomContext(
      "query",
      "ReaderOnlyScan: key >= rows/2; ShardedSortRangeScan: partition sort-key range "
      "[rows/2,rows); project value");
  int benchmark_argc = static_cast<int>(benchmark_args.size());
  benchmark::Initialize(&benchmark_argc, benchmark_args.data());
  if (benchmark::ReportUnrecognizedArguments(benchmark_argc, benchmark_args.data())) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
