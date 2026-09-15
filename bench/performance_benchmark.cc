#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/compression.h>
#include <arrow/util/config.h>

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
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "benchmark_stats.h"
#include "sniffer/io_plan.h"
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

struct Measurement {
  double write_milliseconds = 0;
  double scan_milliseconds = 0;
  double total_milliseconds = 0;
  uint64_t file_bytes = 0;
  uint64_t output_rows = 0;
  uint64_t record_groups = 0;
  sniffer::ScanMetrics metrics;
  sniffer::benchmark::SampleStats write_stats;
  sniffer::benchmark::SampleStats scan_stats;
  sniffer::benchmark::SampleStats total_stats;
};

arrow::Result<uint64_t> FileSize(const std::filesystem::path& path) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error) {
    return arrow::Status::IOError("failed to determine file size: ", error.message());
  }
  return size;
}

arrow::Result<Measurement> RunSnifferOnce(const std::filesystem::path& path,
                                          const sniffer::TableSchema& schema,
                                          const sniffer::LayoutPolicy& policy,
                                          const std::shared_ptr<arrow::RecordBatch>& batch,
                                          const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  ARROW_ASSIGN_OR_RAISE(auto writer, sniffer::SegmentWriter::Open(path.string(), schema, policy));
  ARROW_RETURN_NOT_OK(writer->Append(batch));
  ARROW_RETURN_NOT_OK(writer->Finish());
  const auto write_end = std::chrono::steady_clock::now();

  ARROW_ASSIGN_OR_RAISE(auto reader, sniffer::SegmentReader::Open(path.string()));
  sniffer::IOPlan plan;
  plan.projection_field_ids = {1, 3};
  plan.conjunctive_predicates = {{1, sniffer::Predicate::Op::kGe,
                                  std::make_shared<arrow::Int64Scalar>(batch->num_rows() / 2)}};
  plan.output_batch_rows = options.row_group_rows / 2 + 1;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  ARROW_ASSIGN_OR_RAISE(auto iterator, reader->Scan(std::move(plan), metrics));
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
  measurement.metrics = *metrics;
  return measurement;
}

arrow::Result<Measurement> RunArrowIpcOnce(const std::filesystem::path& path,
                                           const std::shared_ptr<arrow::RecordBatch>& batch,
                                           const Options& options,
                                           arrow::Compression::type compression) {
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
  for (int64_t offset = 0; offset < batch->num_rows(); offset += options.row_group_rows) {
    const int64_t length = std::min<int64_t>(options.row_group_rows, batch->num_rows() - offset);
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
  return measurement;
}

template <typename Runner>
arrow::Result<Measurement> MedianMeasurement(int iterations, Runner&& runner) {
  std::vector<Measurement> measurements;
  measurements.reserve(static_cast<size_t>(iterations));
  for (int iteration = 0; iteration < iterations; ++iteration) {
    ARROW_ASSIGN_OR_RAISE(auto measurement, runner());
    measurements.push_back(std::move(measurement));
  }

  std::vector<double> writes;
  std::vector<double> scans;
  std::vector<double> totals;
  writes.reserve(measurements.size());
  scans.reserve(measurements.size());
  totals.reserve(measurements.size());
  for (const auto& measurement : measurements) {
    writes.push_back(measurement.write_milliseconds);
    scans.push_back(measurement.scan_milliseconds);
    totals.push_back(measurement.total_milliseconds);
  }
  Measurement result = measurements.back();
  result.write_stats = sniffer::benchmark::SummarizeSamples(std::move(writes));
  result.scan_stats = sniffer::benchmark::SummarizeSamples(std::move(scans));
  result.total_stats = sniffer::benchmark::SummarizeSamples(std::move(totals));
  result.write_milliseconds = result.write_stats.p50;
  result.scan_milliseconds = result.scan_stats.p50;
  result.total_milliseconds = result.total_stats.p50;
  return result;
}

double RowsPerSecond(double milliseconds, int64_t input_rows) {
  return milliseconds > 0 ? static_cast<double>(input_rows) / (milliseconds / 1000.0) : 0.0;
}

void PrintMeasurement(std::string_view format, const Measurement& measurement, int64_t input_rows) {
  std::cout << "format=" << format << " write_ms=" << measurement.write_milliseconds
            << " write_rows_per_second="
            << RowsPerSecond(measurement.write_milliseconds, input_rows)
            << " scan_ms=" << measurement.scan_milliseconds
            << " scan_rows_per_second=" << RowsPerSecond(measurement.scan_milliseconds, input_rows)
            << " end_to_end_ms=" << measurement.total_milliseconds << " end_to_end_rows_per_second="
            << RowsPerSecond(measurement.total_milliseconds, input_rows)
            << " file_bytes=" << measurement.file_bytes
            << " output_rows=" << measurement.output_rows
            << " record_groups=" << measurement.record_groups;
  sniffer::benchmark::PrintSampleStats(std::cout, "write", measurement.write_stats);
  sniffer::benchmark::PrintSampleStats(std::cout, "scan", measurement.scan_stats);
  sniffer::benchmark::PrintSampleStats(std::cout, "end_to_end", measurement.total_stats);
  if (format == "sniffer") {
    std::cout << " row_groups_considered=" << measurement.metrics.row_groups_considered
              << " row_groups_pruned=" << measurement.metrics.row_groups_pruned
              << " chunks_read=" << measurement.metrics.column_chunks_read
              << " chunk_bytes_read=" << measurement.metrics.chunk_bytes_read;
  }
  std::cout << '\n';
}

void PrintJsonMeasurement(std::string_view format, const Measurement& measurement,
                          int64_t input_rows) {
  std::cout << "    {\"format\":";
  sniffer::benchmark::PrintJsonString(std::cout, format);
  std::cout << ",\"write_ms\":" << measurement.write_milliseconds << ",\"write_rows_per_second\":"
            << RowsPerSecond(measurement.write_milliseconds, input_rows)
            << ",\"scan_ms\":" << measurement.scan_milliseconds << ",\"scan_rows_per_second\":"
            << RowsPerSecond(measurement.scan_milliseconds, input_rows)
            << ",\"end_to_end_ms\":" << measurement.total_milliseconds
            << ",\"end_to_end_rows_per_second\":"
            << RowsPerSecond(measurement.total_milliseconds, input_rows)
            << ",\"file_bytes\":" << measurement.file_bytes
            << ",\"output_rows\":" << measurement.output_rows
            << ",\"record_groups\":" << measurement.record_groups << ",\"write_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.write_stats);
  std::cout << ",\"scan_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.scan_stats);
  std::cout << ",\"end_to_end_stats\":";
  sniffer::benchmark::PrintJsonSampleStats(std::cout, measurement.total_stats);
  if (format == "sniffer") {
    std::cout << ",\"scan_metrics\":{\"row_groups_considered\":"
              << measurement.metrics.row_groups_considered
              << ",\"row_groups_pruned\":" << measurement.metrics.row_groups_pruned
              << ",\"column_chunks_read\":" << measurement.metrics.column_chunks_read
              << ",\"chunk_bytes_read\":" << measurement.metrics.chunk_bytes_read << '}';
  }
  std::cout << '}';
}

void PrintJsonReport(int argc, char** argv, const Options& options, uint64_t expected_rows,
                     std::string_view build_mode, const Measurement& sniffer,
                     const Measurement& arrow_ipc, const Measurement& arrow_ipc_zstd) {
  std::cout << "{\n  \"benchmark\":\"performance\",\n  \"source_revision\":";
  sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_SOURCE_REVISION);
  std::cout << ",\n  \"compiler\":";
  sniffer::benchmark::PrintJsonString(std::cout, SNIFFER_BENCHMARK_COMPILER);
  std::cout << ",\n  \"arrow_version\":";
  sniffer::benchmark::PrintJsonString(std::cout, ARROW_VERSION_STRING);
  std::cout << ",\n  \"build_mode\":";
  sniffer::benchmark::PrintJsonString(std::cout, build_mode);
  std::cout << ",\n  \"command_arguments\":";
  sniffer::benchmark::PrintJsonArguments(std::cout, argc, argv);
  std::cout << ",\n  \"configuration\":{\"rows\":" << options.rows
            << ",\"row_group_rows\":" << options.row_group_rows
            << ",\"iterations\":" << options.iterations << ",\"selectivity\":"
            << static_cast<double>(expected_rows) / static_cast<double>(options.rows)
            << ",\"distribution\":\"grouped_id_linear_value_nullable\""
               ",\"predicate\":\"id_ge_half\",\"projection\":\"id,value\""
               ",\"execution\":\"single_thread\",\"hardware_threads\":"
            << std::thread::hardware_concurrency() << "},\n  \"measurements\":[\n";
  PrintJsonMeasurement("sniffer", sniffer, options.rows);
  std::cout << ",\n";
  PrintJsonMeasurement("arrow_ipc", arrow_ipc, options.rows);
  std::cout << ",\n";
  PrintJsonMeasurement("arrow_ipc_zstd", arrow_ipc_zstd, options.rows);
  std::cout << "\n  ],\n  \"comparisons\":{\"write_sniffer_speedup_vs_arrow_ipc\":"
            << arrow_ipc.write_milliseconds / sniffer.write_milliseconds
            << ",\"write_sniffer_speedup_vs_arrow_ipc_zstd\":"
            << arrow_ipc_zstd.write_milliseconds / sniffer.write_milliseconds
            << ",\"scan_sniffer_speedup_vs_arrow_ipc\":"
            << arrow_ipc.scan_milliseconds / sniffer.scan_milliseconds
            << ",\"scan_sniffer_speedup_vs_arrow_ipc_zstd\":"
            << arrow_ipc_zstd.scan_milliseconds / sniffer.scan_milliseconds << "}\n}\n";
}

arrow::Result<int> RunBenchmark(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  ARROW_ASSIGN_OR_RAISE(auto arrow_batch, MakeBenchmarkBatch(options.rows));
  sniffer::TableSchema schema{1,
                              {{1, "id", arrow::int64(), false, nullptr},
                               {2, "group", arrow::utf8(), false, nullptr},
                               {3, "value", arrow::int64(), true, nullptr}}};
  const auto temporary = std::filesystem::temp_directory_path();
  const auto sniffer_path = temporary / "sniffer_core_performance.seg";
  const auto ipc_path = temporary / "sniffer_core_performance.arrow";
  const auto ipc_zstd_path = temporary / "sniffer_core_performance.zstd.arrow";
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = options.row_group_rows;
  policy.sort_key_field_ids = {1};
  policy.statistics_field_ids = {1, 3};
  policy.bloom_field_ids = {2};

  ARROW_ASSIGN_OR_RAISE(const auto sniffer, MedianMeasurement(options.iterations, [&] {
                          return RunSnifferOnce(sniffer_path, schema, policy, arrow_batch, options);
                        }));
  ARROW_ASSIGN_OR_RAISE(const auto arrow_ipc, MedianMeasurement(options.iterations, [&] {
                          return RunArrowIpcOnce(ipc_path, arrow_batch, options,
                                                 arrow::Compression::UNCOMPRESSED);
                        }));
  ARROW_ASSIGN_OR_RAISE(const auto arrow_ipc_zstd, MedianMeasurement(options.iterations, [&] {
                          return RunArrowIpcOnce(ipc_zstd_path, arrow_batch, options,
                                                 arrow::Compression::ZSTD);
                        }));
  const uint64_t expected_rows = static_cast<uint64_t>(options.rows - options.rows / 2);
  if (sniffer.output_rows != expected_rows || arrow_ipc.output_rows != expected_rows ||
      arrow_ipc_zstd.output_rows != expected_rows) {
    return arrow::Status::Invalid("benchmark result count mismatch");
  }

#ifdef NDEBUG
  constexpr std::string_view kBuildMode = "release";
#else
  constexpr std::string_view kBuildMode = "debug";
#endif
  if (options.output_json) {
    PrintJsonReport(argc, argv, options, expected_rows, kBuildMode, sniffer, arrow_ipc,
                    arrow_ipc_zstd);
  } else {
    std::cout << "benchmark=performance rows=" << options.rows
              << " row_group_rows=" << options.row_group_rows
              << " iterations=" << options.iterations << " selectivity="
              << static_cast<double>(expected_rows) / static_cast<double>(options.rows)
              << " distribution=grouped_id_linear_value_nullable"
              << " predicate=id_ge_half projection=id,value"
              << " execution=single_thread build_mode=" << kBuildMode
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " source_revision=" << SNIFFER_BENCHMARK_SOURCE_REVISION << " compiler=\""
              << SNIFFER_BENCHMARK_COMPILER << "\""
              << " arrow_version=" << ARROW_VERSION_STRING << '\n';
    PrintMeasurement("sniffer", sniffer, options.rows);
    PrintMeasurement("arrow_ipc", arrow_ipc, options.rows);
    PrintMeasurement("arrow_ipc_zstd", arrow_ipc_zstd, options.rows);
    std::cout << "phase=write sniffer_speedup_vs_arrow_ipc="
              << arrow_ipc.write_milliseconds / sniffer.write_milliseconds
              << " sniffer_speedup_vs_arrow_ipc_zstd="
              << arrow_ipc_zstd.write_milliseconds / sniffer.write_milliseconds << '\n';
    std::cout << "phase=scan sniffer_speedup_vs_arrow_ipc="
              << arrow_ipc.scan_milliseconds / sniffer.scan_milliseconds
              << " sniffer_speedup_vs_arrow_ipc_zstd="
              << arrow_ipc_zstd.scan_milliseconds / sniffer.scan_milliseconds << '\n';
  }

  std::error_code ignored;
  std::filesystem::remove(sniffer_path, ignored);
  std::filesystem::remove(ipc_path, ignored);
  std::filesystem::remove(ipc_zstd_path, ignored);
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
