#include <arrow/api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "sniffer/io_plan.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

struct Options {
  int64_t rows = 100000;
  int iterations = 5;
  uint32_t row_group_rows = 4096;
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
  }
  options.rows = std::max<int64_t>(1, options.rows);
  options.iterations = std::max(1, options.iterations);
  options.row_group_rows = std::max<uint32_t>(1, options.row_group_rows);
  return options;
}

std::shared_ptr<arrow::RecordBatch> MakeBenchmarkBatch(int64_t rows) {
  auto schema = arrow::schema({arrow::field("id", arrow::int64(), false),
                               arrow::field("group", arrow::utf8(), false),
                               arrow::field("value", arrow::int64(), true)});
  arrow::Int64Builder id_builder;
  arrow::StringBuilder group_builder;
  arrow::Int64Builder value_builder;
  for (int64_t row = 0; row < rows; ++row) {
    (void)id_builder.Append(row);
    (void)group_builder.Append("group-" + std::to_string(row % 32));
    if (row % 17 == 0) {
      (void)value_builder.AppendNull();
    } else {
      (void)value_builder.Append(row * 3);
    }
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(3);
  if (!id_builder.Finish(&columns[0]).ok() || !group_builder.Finish(&columns[1]).ok() ||
      !value_builder.Finish(&columns[2]).ok()) {
    return nullptr;
  }
  return arrow::RecordBatch::Make(schema, rows, std::move(columns));
}

struct Measurement {
  double milliseconds = 0;
  uint64_t file_bytes = 0;
  sniffer::ScanMetrics metrics;
  uint64_t output_rows = 0;
};

Measurement RunOnce(const std::filesystem::path& path, const sniffer::TableSchema& schema,
                    const sniffer::LayoutPolicy& policy,
                    const std::shared_ptr<arrow::RecordBatch>& batch, const Options& options) {
  auto start = std::chrono::steady_clock::now();
  auto writer_result = sniffer::SegmentWriter::Open(path.string(), schema, policy);
  if (!writer_result.ok()) {
    return {};
  }
  auto writer = std::move(writer_result).ValueUnsafe();
  if (!writer->Append(batch).ok() || !writer->Finish().ok()) {
    return {};
  }
  auto reader_result = sniffer::SegmentReader::Open(path.string());
  if (!reader_result.ok()) {
    return {};
  }
  auto reader = std::move(reader_result).ValueUnsafe();
  sniffer::IOPlan plan;
  plan.projection_field_ids = {1, 3};
  plan.conjunctive_predicates = {{1, sniffer::Predicate::Op::kGe,
                                  std::make_shared<arrow::Int64Scalar>(batch->num_rows() / 2)}};
  plan.output_batch_rows = options.row_group_rows / 2 + 1;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto iterator_result = reader->Scan(std::move(plan), metrics);
  uint64_t output_rows = 0;
  if (iterator_result.ok()) {
    auto iterator = std::move(iterator_result).ValueUnsafe();
    while (true) {
      auto next = iterator.Next();
      if (!next.ok() || !next.ValueUnsafe()) {
        break;
      }
      output_rows += static_cast<uint64_t>(next.ValueUnsafe()->num_rows());
    }
  }
  auto end = std::chrono::steady_clock::now();
  Measurement measurement;
  measurement.milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
  measurement.file_bytes = std::filesystem::file_size(path);
  measurement.metrics = *metrics;
  measurement.output_rows = output_rows;
  return measurement;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  const auto arrow_batch = MakeBenchmarkBatch(options.rows);
  if (!arrow_batch) {
    std::cerr << "failed to build benchmark batch\n";
    return 1;
  }
  sniffer::TableSchema schema{1,
                              {{1, "id", arrow::int64(), false, nullptr},
                               {2, "group", arrow::utf8(), false, nullptr},
                               {3, "value", arrow::int64(), true, nullptr}}};
  const auto path = std::filesystem::temp_directory_path() / "sniffer_core_benchmark.seg";
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = options.row_group_rows;
  policy.sort_key_field_ids = {1};
  policy.statistics_field_ids = {1, 3};
  policy.bloom_field_ids = {2};

  std::vector<Measurement> measurements;
  measurements.reserve(static_cast<size_t>(options.iterations));
  for (int iteration = 0; iteration < options.iterations; ++iteration) {
    measurements.push_back(RunOnce(path, schema, policy, arrow_batch, options));
  }
  std::sort(measurements.begin(), measurements.end(),
            [](const Measurement& left, const Measurement& right) {
              return left.milliseconds < right.milliseconds;
            });
  const Measurement& median = measurements[measurements.size() / 2];
#ifdef NDEBUG
  constexpr std::string_view kBuildMode = "release";
#else
  constexpr std::string_view kBuildMode = "debug";
#endif
  std::cout << "rows=" << options.rows << " row_group_rows=" << options.row_group_rows
            << " iterations=" << options.iterations
            << " selectivity=" << static_cast<double>(median.output_rows) / options.rows
            << " distribution=grouped_id_linear_value_nullable"
            << " predicate=id_ge_half projection=id,value"
            << " build_mode=" << kBuildMode << " threads=" << std::thread::hardware_concurrency()
            << '\n';
  std::cout << "median_ms=" << median.milliseconds << " file_bytes=" << median.file_bytes
            << " scan_rows_per_second="
            << (median.milliseconds > 0
                    ? static_cast<double>(options.rows) / (median.milliseconds / 1000.0)
                    : 0.0)
            << " output_rows=" << median.output_rows
            << " row_groups_considered=" << median.metrics.row_groups_considered
            << " row_groups_pruned=" << median.metrics.row_groups_pruned
            << " chunks_read=" << median.metrics.column_chunks_read
            << " chunk_bytes_read=" << median.metrics.chunk_bytes_read << '\n';
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return 0;
}
