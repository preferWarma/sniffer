#pragma once

#include <arrow/api.h>
#include <arrow/util/iterator.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sniffer/io_plan.h"
#include "sniffer/schema.h"

namespace sniffer {

struct ScanExecutionOptions {
  uint32_t worker_count = 1;
  uint32_t max_in_flight_row_groups = 4;
  // Budget for conservative Row Group scheduling estimates, not a process RSS cap.
  uint64_t max_buffered_bytes = 64U * 1024U * 1024U;
};

struct ReaderMetrics {
  uint64_t file_handles_opened = 0;
  uint64_t envelope_io_nanoseconds = 0;
  uint64_t metadata_parse_nanoseconds = 0;
  uint64_t directory_validation_nanoseconds = 0;
  uint64_t index_io_nanoseconds = 0;
  uint64_t index_checksum_nanoseconds = 0;
  uint64_t index_parse_nanoseconds = 0;
  uint64_t file_checksum_nanoseconds = 0;
};

class SegmentReader {
 public:
  [[nodiscard]] static arrow::Result<std::unique_ptr<SegmentReader>> Open(
      std::string path, std::shared_ptr<ReaderMetrics> metrics = nullptr);

  SegmentReader(const SegmentReader&) = delete;
  SegmentReader& operator=(const SegmentReader&) = delete;
  SegmentReader(SegmentReader&&) = delete;
  SegmentReader& operator=(SegmentReader&&) = delete;
  ~SegmentReader();

  [[nodiscard]] const TableSchema& schema() const;
  [[nodiscard]] uint64_t num_row_groups() const;
  // ReadAll and independent Scan calls may run concurrently on the same Reader.
  // Do not call Next() concurrently on one iterator or share one ScanMetrics
  // object between concurrent scans. The caller must not mutate IOPlan scalars
  // while scans that reference them are active.
  [[nodiscard]] arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> ReadAll() const;
  // Each Scan iterator owns its execution state and outlives Reader destruction.
  [[nodiscard]] arrow::Result<arrow::RecordBatchIterator> Scan(
      IOPlan plan, std::shared_ptr<ScanMetrics> metrics = nullptr) const;
  // Opt-in parallel execution; plans with limit retain serial early-stop semantics.
  [[nodiscard]] arrow::Result<arrow::RecordBatchIterator> Scan(
      IOPlan plan, ScanExecutionOptions options,
      std::shared_ptr<ScanMetrics> metrics = nullptr) const;
  [[nodiscard]] arrow::Status VerifyFileChecksum() const;

 private:
  class Impl;
  explicit SegmentReader(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace sniffer
