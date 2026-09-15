#include "sniffer/schema.h"

#include <arrow/util/key_value_metadata.h>

#include <algorithm>
#include <charconv>
#include <string_view>
#include <unordered_set>

#include "sniffer/layout.h"

namespace sniffer {

bool IsSupportedPhaseOneType(const arrow::DataType& type) {
  switch (type.id()) {
    case arrow::Type::BOOL:
    case arrow::Type::INT8:
    case arrow::Type::INT16:
    case arrow::Type::INT32:
    case arrow::Type::INT64:
    case arrow::Type::UINT8:
    case arrow::Type::UINT16:
    case arrow::Type::UINT32:
    case arrow::Type::UINT64:
    case arrow::Type::FLOAT:
    case arrow::Type::DOUBLE:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::STRING:
    case arrow::Type::BINARY:
      return true;
    default:
      return false;
  }
}

arrow::Status TableSchema::Validate() const {
  std::unordered_set<uint32_t> field_ids;
  for (const auto& field : fields) {
    if (!field.type) {
      return arrow::Status::Invalid("[sniffer.schema.type] field has no Arrow type: ", field.name);
    }
    if (!field_ids.insert(field.field_id).second) {
      return arrow::Status::Invalid("[sniffer.schema.field_id] duplicate field ID ",
                                    field.field_id);
    }
    if (!IsSupportedPhaseOneType(*field.type)) {
      return arrow::Status::NotImplemented(
          "[sniffer.schema.type] unsupported Arrow type for field ", field.field_id, ": ",
          field.type->ToString());
    }
    if (field.default_value) {
      return arrow::Status::NotImplemented(
          "[sniffer.schema.default] default values are not supported in format 1.0");
    }
  }
  return arrow::Status::OK();
}

arrow::Status TableSchema::ValidateBatch(const arrow::RecordBatch& batch) const {
  ARROW_RETURN_NOT_OK(Validate());
  if (batch.num_columns() != static_cast<int>(fields.size())) {
    return arrow::Status::Invalid("[sniffer.schema.mismatch] expected ", fields.size(),
                                  " columns, got ", batch.num_columns());
  }
  for (int index = 0; index < batch.num_columns(); ++index) {
    const auto& expected = fields[static_cast<size_t>(index)];
    const auto& actual = batch.schema()->field(index);
    if (expected.name != actual->name() || !expected.type->Equals(actual->type()) ||
        expected.nullable != actual->nullable()) {
      return arrow::Status::Invalid("[sniffer.schema.mismatch] column ", index,
                                    " does not match TableSchema field ID ", expected.field_id);
    }
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Schema>> TableSchema::ToArrowSchema() const {
  ARROW_RETURN_NOT_OK(Validate());
  std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
  arrow_fields.reserve(fields.size());
  for (const auto& field : fields) {
    auto metadata =
        arrow::key_value_metadata({kFieldIdMetadataKey}, {std::to_string(field.field_id)});
    arrow_fields.push_back(arrow::field(field.name, field.type, field.nullable, metadata));
  }
  auto schema_metadata =
      arrow::key_value_metadata({kSchemaVersionMetadataKey}, {std::to_string(schema_version)});
  return arrow::schema(std::move(arrow_fields), std::move(schema_metadata));
}

arrow::Status LayoutPolicy::ValidatePhaseOne() const {
  if (target_row_group_rows == 0) {
    return arrow::Status::Invalid(
        "[sniffer.layout.row_group] target_row_group_rows must be positive");
  }
  if (!sort_key_field_ids.empty() || !statistics_field_ids.empty() || !bloom_field_ids.empty()) {
    return arrow::Status::NotImplemented("[sniffer.layout.index] indexes are a phase-two feature");
  }
  return arrow::Status::OK();
}

arrow::Status LayoutPolicy::Validate(const TableSchema& schema) const {
  ARROW_RETURN_NOT_OK(schema.Validate());
  if (target_row_group_rows == 0) {
    return arrow::Status::Invalid(
        "[sniffer.layout.row_group] target_row_group_rows must be positive");
  }
  if (encoding_sample_rows == 0) {
    return arrow::Status::Invalid(
        "[sniffer.layout.encoding] encoding_sample_rows must be positive");
  }

  const auto validate_ids = [&schema](const std::vector<uint32_t>& ids,
                                      std::string_view kind) -> arrow::Status {
    std::unordered_set<uint32_t> seen;
    for (const uint32_t id : ids) {
      if (!seen.insert(id).second) {
        return arrow::Status::Invalid("[sniffer.layout.index] duplicate ", kind, " field ID ", id);
      }
      const auto field =
          std::find_if(schema.fields.begin(), schema.fields.end(),
                       [id](const FieldSpec& candidate) { return candidate.field_id == id; });
      if (field == schema.fields.end()) {
        return arrow::Status::Invalid("[sniffer.layout.index] unknown ", kind, " field ID ", id);
      }
      if (kind == "sort-key" && field->nullable) {
        return arrow::Status::Invalid("[sniffer.layout.sort_key] sort-key field ", id,
                                      " must be non-nullable");
      }
    }
    return arrow::Status::OK();
  };

  ARROW_RETURN_NOT_OK(validate_ids(sort_key_field_ids, "sort-key"));
  ARROW_RETURN_NOT_OK(validate_ids(statistics_field_ids, "statistics"));
  ARROW_RETURN_NOT_OK(validate_ids(bloom_field_ids, "Bloom"));

  std::unordered_set<uint32_t> encoded_fields;
  for (const auto& override : field_encodings) {
    if (!encoded_fields.insert(override.field_id).second) {
      return arrow::Status::Invalid("[sniffer.layout.encoding] duplicate field override ",
                                    override.field_id);
    }
    const auto field = std::find_if(schema.fields.begin(), schema.fields.end(),
                                    [&override](const FieldSpec& candidate) {
                                      return candidate.field_id == override.field_id;
                                    });
    if (field == schema.fields.end()) {
      return arrow::Status::Invalid("[sniffer.layout.encoding] unknown field ID ",
                                    override.field_id);
    }
    const auto type = field->type->id();
    const bool integer = type == arrow::Type::INT8 || type == arrow::Type::INT16 ||
                         type == arrow::Type::INT32 || type == arrow::Type::INT64 ||
                         type == arrow::Type::UINT8 || type == arrow::Type::UINT16 ||
                         type == arrow::Type::UINT32 || type == arrow::Type::UINT64;
    const bool supported =
        override.encoding == EncodingKind::kPlain ||
        (override.encoding == EncodingKind::kDictionary &&
         (integer || type == arrow::Type::STRING || type == arrow::Type::BINARY)) ||
        (override.encoding == EncodingKind::kRle && (integer || type == arrow::Type::BOOL)) ||
        (override.encoding == EncodingKind::kForBitpack &&
         (integer || type == arrow::Type::TIMESTAMP));
    if (!supported) {
      return arrow::Status::Invalid("[sniffer.layout.encoding] encoding ",
                                    static_cast<uint16_t>(override.encoding),
                                    " does not support field ", override.field_id);
    }
  }
  return arrow::Status::OK();
}

}  // namespace sniffer
