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
  uint64_t chunk_bytes_read = 0;
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
    chunk_bytes_read = metrics->chunk_bytes_read;
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
  state.counters["chunk_bytes_read"] = static_cast<double>(chunk_bytes_read);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(ProcessPeakRssBytes());
  state.counters["arrow_pool_peak_bytes"] =
      static_cast<double>(arrow::default_memory_pool()->max_memory());
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(expected_rows));
}

BENCHMARK(ReaderOnlyScan)->UseRealTime()->Unit(benchmark::kMillisecond);

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
  benchmark::AddCustomContext("query", "key >= rows/2; project value");
  int benchmark_argc = static_cast<int>(benchmark_args.size());
  benchmark::Initialize(&benchmark_argc, benchmark_args.data());
  if (benchmark::ReportUnrecognizedArguments(benchmark_argc, benchmark_args.data())) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
