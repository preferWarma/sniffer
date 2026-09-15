#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sniffer {

inline constexpr char kFieldIdMetadataKey[] = "sniffer.field_id";
inline constexpr char kSchemaVersionMetadataKey[] = "sniffer.schema_version";

struct FieldSpec {
  uint32_t field_id = 0;
  std::string name;
  std::shared_ptr<arrow::DataType> type;
  bool nullable = true;
  std::shared_ptr<arrow::Scalar> default_value;
};

struct TableSchema {
  uint32_t schema_version = 0;
  std::vector<FieldSpec> fields;

  [[nodiscard]] arrow::Status Validate() const;
  [[nodiscard]] arrow::Status ValidateBatch(const arrow::RecordBatch& batch) const;
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Schema>> ToArrowSchema() const;
};

[[nodiscard]] bool IsSupportedPhaseOneType(const arrow::DataType& type);

}  // namespace sniffer
