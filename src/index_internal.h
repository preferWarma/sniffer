#pragma once

#include <arrow/api.h>

#include <memory>
#include <vector>

#include "column_analysis_internal.h"
#include "format_internal.h"
#include "sniffer/layout.h"
#include "sniffer/schema.h"

namespace sniffer::internal {

[[nodiscard]] arrow::Status ValidateAndUpdateSortOrder(
    const TableSchema& schema, const LayoutPolicy& layout, const arrow::RecordBatch& batch,
    std::vector<std::shared_ptr<arrow::Scalar>>* previous_key);
[[nodiscard]] arrow::Result<RowGroupIndex> BuildRowGroupIndex(const TableSchema& schema,
                                                              const LayoutPolicy& layout,
                                                              const arrow::RecordBatch& batch,
                                                              bool sort_order_validated = false,
                                                              RowGroupAnalysis* analysis = nullptr);
[[nodiscard]] arrow::Result<bool> BloomMayContain(const FieldSpec& field, const BloomMeta& bloom,
                                                  const arrow::Scalar& value);

}  // namespace sniffer::internal
