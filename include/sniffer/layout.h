#pragma once

#include <arrow/status.h>

#include <cstdint>
#include <vector>

namespace sniffer {

struct TableSchema;

enum class EncodingKind : uint16_t {
  kPlain = 0,
  kDictionary = 1,
  kRle = 2,
  kForBitpack = 3,
};

struct FieldEncoding {
  uint32_t field_id = 0;
  EncodingKind encoding = EncodingKind::kPlain;
};

struct LayoutPolicy {
  uint32_t target_row_group_rows = 64 * 1024;
  uint32_t encoding_sample_rows = 1024;
  std::vector<uint32_t> sort_key_field_ids;
  std::vector<uint32_t> statistics_field_ids;
  std::vector<uint32_t> bloom_field_ids;
  std::vector<FieldEncoding> field_encodings;

  [[nodiscard]] arrow::Status ValidatePhaseOne() const;
  [[nodiscard]] arrow::Status Validate(const TableSchema& schema) const;
};

}  // namespace sniffer
