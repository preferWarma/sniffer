#pragma once

#include <arrow/status.h>

#include <cstdint>
#include <vector>

namespace sniffer {

struct TableSchema;

struct LayoutPolicy {
  uint32_t target_row_group_rows = 64 * 1024;
  std::vector<uint32_t> sort_key_field_ids;
  std::vector<uint32_t> statistics_field_ids;
  std::vector<uint32_t> bloom_field_ids;

  [[nodiscard]] arrow::Status ValidatePhaseOne() const;
  [[nodiscard]] arrow::Status Validate(const TableSchema& schema) const;
};

}  // namespace sniffer
