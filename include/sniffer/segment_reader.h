#pragma once

#include <arrow/api.h>
#include <arrow/util/iterator.h>

#include <memory>
#include <string>
#include <vector>

#include "sniffer/io_plan.h"
#include "sniffer/schema.h"

namespace sniffer {

class SegmentReader {
 public:
  [[nodiscard]] static arrow::Result<std::unique_ptr<SegmentReader>> Open(std::string path);

  SegmentReader(const SegmentReader&) = delete;
  SegmentReader& operator=(const SegmentReader&) = delete;
  SegmentReader(SegmentReader&&) = delete;
  SegmentReader& operator=(SegmentReader&&) = delete;
  ~SegmentReader();

  [[nodiscard]] const TableSchema& schema() const;
  [[nodiscard]] uint64_t num_row_groups() const;
  [[nodiscard]] arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> ReadAll() const;
  [[nodiscard]] arrow::Result<arrow::RecordBatchIterator> Scan(
      IOPlan plan, std::shared_ptr<ScanMetrics> metrics = nullptr) const;
  [[nodiscard]] arrow::Status VerifyFileChecksum() const;

 private:
  class Impl;
  explicit SegmentReader(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace sniffer
