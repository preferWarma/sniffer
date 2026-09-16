#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>

#include "sniffer/layout.h"
#include "sniffer/schema.h"

namespace sniffer {

struct WriterMetrics {
  uint64_t validation_nanoseconds = 0;
  uint64_t index_nanoseconds = 0;
  uint64_t encoding_selection_nanoseconds = 0;
  uint64_t encoding_nanoseconds = 0;
  uint64_t checksum_nanoseconds = 0;
  uint64_t file_write_nanoseconds = 0;
  uint64_t footer_nanoseconds = 0;
};

class SegmentWriter {
 public:
  [[nodiscard]] static arrow::Result<std::unique_ptr<SegmentWriter>> Open(
      std::string path, TableSchema schema, LayoutPolicy layout_policy = {},
      std::shared_ptr<WriterMetrics> metrics = nullptr);

  SegmentWriter(const SegmentWriter&) = delete;
  SegmentWriter& operator=(const SegmentWriter&) = delete;
  SegmentWriter(SegmentWriter&&) = delete;
  SegmentWriter& operator=(SegmentWriter&&) = delete;
  ~SegmentWriter();

  [[nodiscard]] arrow::Status Append(const std::shared_ptr<arrow::RecordBatch>& batch);
  [[nodiscard]] arrow::Status Finish();

 private:
  class Impl;
  explicit SegmentWriter(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace sniffer
