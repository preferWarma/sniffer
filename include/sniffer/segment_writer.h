#pragma once

#include <arrow/api.h>

#include <memory>
#include <string>

#include "sniffer/layout.h"
#include "sniffer/schema.h"

namespace sniffer {

class SegmentWriter {
 public:
  [[nodiscard]] static arrow::Result<std::unique_ptr<SegmentWriter>> Open(
      std::string path, TableSchema schema, LayoutPolicy layout_policy = {});

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
