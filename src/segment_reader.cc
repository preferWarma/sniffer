#include "sniffer/segment_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "codec_internal.h"
#include "format_internal.h"
#include "index_internal.h"
#include "scalar_internal.h"

namespace sniffer {
namespace {

arrow::Status IoError(const std::string& operation, const std::string& path) {
  return arrow::Status::IOError("[sniffer.io] ", operation, ": ", path);
}

arrow::Status InvalidFormat(const std::string& detail) {
  return arrow::Status::Invalid("[sniffer.format.invalid] ", detail);
}

arrow::Result<std::vector<uint8_t>> ReadRange(const std::string& path, uint64_t file_size,
                                              uint64_t offset, uint64_t length) {
  ARROW_ASSIGN_OR_RAISE(const uint64_t end, internal::CheckedAdd(offset, length));
  if (end > file_size) {
    return arrow::Status::Invalid("[sniffer.format.bounds] read range exceeds file size");
  }
  if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      length > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
      length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return arrow::Status::Invalid("[sniffer.format.limit] read range exceeds platform limits");
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return IoError("cannot open segment for reading", path);
  }
  stream.seekg(static_cast<std::streamoff>(offset));
  if (!stream) {
    return IoError("cannot seek segment", path);
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (length != 0) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (!stream) {
      return IoError("cannot read segment range", path);
    }
  }
  return bytes;
}

bool IsValid(std::span<const uint8_t> validity, uint64_t index) {
  if (validity.empty()) {
    return true;
  }
  const size_t byte_index = static_cast<size_t>(index / 8U);
  const uint32_t bit_index = static_cast<uint32_t>(index % 8U);
  return (validity[byte_index] & static_cast<uint8_t>(1U << bit_index)) != 0;
}

arrow::Status ValidateValidity(std::span<const uint8_t> validity, uint64_t row_count,
                               uint64_t null_count) {
  const uint64_t expected_length = null_count == 0 ? 0 : row_count / 8U + (row_count % 8U != 0);
  if (validity.size() != expected_length) {
    return InvalidFormat("invalid validity bitmap length");
  }
  uint64_t observed_nulls = 0;
  for (uint64_t index = 0; index < row_count; ++index) {
    if (!IsValid(validity, index)) {
      ++observed_nulls;
    }
  }
  if (observed_nulls != null_count) {
    return InvalidFormat("null count does not match validity bitmap");
  }
  if (!validity.empty() && row_count % 8U != 0) {
    const uint32_t used_bits = static_cast<uint32_t>(row_count % 8U);
    const uint8_t padding_mask = static_cast<uint8_t>(0xFFU << used_bits);
    if ((validity.back() & padding_mask) != 0) {
      return InvalidFormat("validity padding bits must be zero");
    }
  }
  return arrow::Status::OK();
}

template <typename Builder, typename ReadValue>
arrow::Result<std::shared_ptr<arrow::Array>> DecodeFixed(uint64_t row_count,
                                                         std::span<const uint8_t> validity,
                                                         internal::ByteReader* values,
                                                         Builder* builder, ReadValue read_value) {
  if (row_count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return arrow::Status::Invalid("[sniffer.format.limit] row count exceeds Arrow limit");
  }
  ARROW_RETURN_NOT_OK(builder->Reserve(static_cast<int64_t>(row_count)));
  for (uint64_t index = 0; index < row_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(auto value, read_value(values));
    if (IsValid(validity, index)) {
      ARROW_RETURN_NOT_OK(builder->Append(value));
    } else {
      ARROW_RETURN_NOT_OK(builder->AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> result;
  ARROW_RETURN_NOT_OK(builder->Finish(&result));
  return result;
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodeVariable(internal::PhysicalTypeId type_id,
                                                            uint64_t row_count,
                                                            std::span<const uint8_t> validity,
                                                            std::span<const uint8_t> offset_bytes,
                                                            std::span<const uint8_t> value_bytes) {
  ARROW_ASSIGN_OR_RAISE(const uint64_t offset_count, internal::CheckedAdd(row_count, uint64_t{1}));
  ARROW_ASSIGN_OR_RAISE(const uint64_t expected_offset_bytes,
                        internal::CheckedMultiply(offset_count, uint64_t{8}));
  if (offset_bytes.size() != expected_offset_bytes) {
    return InvalidFormat("invalid variable-size offset buffer length");
  }

  internal::ByteReader offset_reader(offset_bytes);
  std::vector<uint64_t> offsets;
  if (offset_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return arrow::Status::Invalid("[sniffer.format.limit] offset count exceeds platform limit");
  }
  offsets.reserve(static_cast<size_t>(offset_count));
  for (uint64_t index = 0; index < offset_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t offset, offset_reader.ReadU64());
    if ((!offsets.empty() && offset < offsets.back()) || offset > value_bytes.size()) {
      return InvalidFormat("variable-size offsets are not monotonic and bounded");
    }
    offsets.push_back(offset);
  }
  if (offsets.front() != 0 || offsets.back() != value_bytes.size()) {
    return InvalidFormat("variable-size offsets are not normalized");
  }
  if (row_count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return arrow::Status::Invalid("[sniffer.format.limit] row count exceeds Arrow limit");
  }

  std::unique_ptr<arrow::BinaryBuilder> binary_builder;
  std::unique_ptr<arrow::StringBuilder> string_builder;
  if (type_id == internal::PhysicalTypeId::kString) {
    string_builder = std::make_unique<arrow::StringBuilder>();
    ARROW_RETURN_NOT_OK(string_builder->Reserve(static_cast<int64_t>(row_count)));
  } else {
    binary_builder = std::make_unique<arrow::BinaryBuilder>();
    ARROW_RETURN_NOT_OK(binary_builder->Reserve(static_cast<int64_t>(row_count)));
  }

  for (uint64_t index = 0; index < row_count; ++index) {
    if (!IsValid(validity, index)) {
      if (string_builder) {
        ARROW_RETURN_NOT_OK(string_builder->AppendNull());
      } else {
        ARROW_RETURN_NOT_OK(binary_builder->AppendNull());
      }
      continue;
    }
    const uint64_t begin = offsets[static_cast<size_t>(index)];
    const uint64_t end = offsets[static_cast<size_t>(index + 1)];
    const uint64_t length = end - begin;
    if (length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return arrow::Status::Invalid(
          "[sniffer.format.limit] Binary/String value exceeds Arrow limit");
    }
    const auto value = value_bytes.subspan(static_cast<size_t>(begin), static_cast<size_t>(length));
    if (string_builder) {
      ARROW_RETURN_NOT_OK(string_builder->Append(reinterpret_cast<const char*>(value.data()),
                                                 static_cast<int32_t>(length)));
    } else {
      ARROW_RETURN_NOT_OK(binary_builder->Append(value.data(), static_cast<int32_t>(length)));
    }
  }

  std::shared_ptr<arrow::Array> result;
  if (string_builder) {
    ARROW_RETURN_NOT_OK(string_builder->Finish(&result));
  } else {
    ARROW_RETURN_NOT_OK(binary_builder->Finish(&result));
  }
  ARROW_RETURN_NOT_OK(result->ValidateFull());
  return result;
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodePlainImpl(const FieldSpec& field,
                                                             const internal::ColumnChunkMeta& chunk,
                                                             std::span<const uint8_t> payload) {
  // Builders deliberately allocate Arrow-owned output buffers: little-endian
  // wire values cannot be exposed as portable zero-copy Arrow buffers.
  internal::ByteReader payload_reader(payload);
  ARROW_ASSIGN_OR_RAISE(const uint64_t validity_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t offsets_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t values_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(auto validity, payload_reader.ReadBytes(validity_length));
  ARROW_ASSIGN_OR_RAISE(auto offsets, payload_reader.ReadBytes(offsets_length));
  ARROW_ASSIGN_OR_RAISE(auto values, payload_reader.ReadBytes(values_length));
  if (payload_reader.remaining() != 0) {
    return InvalidFormat("trailing Plain payload bytes");
  }
  ARROW_RETURN_NOT_OK(ValidateValidity(validity, chunk.row_count, chunk.null_count));

  if (chunk.physical_type == internal::PhysicalTypeId::kString ||
      chunk.physical_type == internal::PhysicalTypeId::kBinary) {
    return DecodeVariable(chunk.physical_type, chunk.row_count, validity, offsets, values);
  }
  if (!offsets.empty()) {
    return InvalidFormat("fixed-width Plain payload has offsets");
  }
  ARROW_ASSIGN_OR_RAISE(
      const uint64_t expected_values_length,
      internal::CheckedMultiply(
          chunk.row_count, static_cast<uint64_t>(internal::FixedWidthBytes(chunk.physical_type))));
  if (values.size() != expected_values_length) {
    return InvalidFormat("invalid fixed-width values length");
  }
  internal::ByteReader value_reader(values);

  switch (chunk.physical_type) {
    case internal::PhysicalTypeId::kBool: {
      arrow::BooleanBuilder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<bool> {
                           ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader->ReadU8());
                           if (value > 1) {
                             return InvalidFormat("invalid Boolean Plain value");
                           }
                           return value == 1;
                         });
    }
    case internal::PhysicalTypeId::kInt8: {
      arrow::Int8Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<int8_t> {
                           ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader->ReadU8());
                           return std::bit_cast<int8_t>(value);
                         });
    }
    case internal::PhysicalTypeId::kInt16: {
      arrow::Int16Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<int16_t> {
                           ARROW_ASSIGN_OR_RAISE(const uint16_t value, reader->ReadU16());
                           return std::bit_cast<int16_t>(value);
                         });
    }
    case internal::PhysicalTypeId::kInt32: {
      arrow::Int32Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<int32_t> {
                           ARROW_ASSIGN_OR_RAISE(const uint32_t value, reader->ReadU32());
                           return std::bit_cast<int32_t>(value);
                         });
    }
    case internal::PhysicalTypeId::kInt64: {
      arrow::Int64Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<int64_t> {
                           ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader->ReadU64());
                           return std::bit_cast<int64_t>(value);
                         });
    }
    case internal::PhysicalTypeId::kUInt8: {
      arrow::UInt8Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) { return reader->ReadU8(); });
    }
    case internal::PhysicalTypeId::kUInt16: {
      arrow::UInt16Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) { return reader->ReadU16(); });
    }
    case internal::PhysicalTypeId::kUInt32: {
      arrow::UInt32Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) { return reader->ReadU32(); });
    }
    case internal::PhysicalTypeId::kUInt64: {
      arrow::UInt64Builder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) { return reader->ReadU64(); });
    }
    case internal::PhysicalTypeId::kFloat32: {
      arrow::FloatBuilder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<float> {
                           ARROW_ASSIGN_OR_RAISE(const uint32_t value, reader->ReadU32());
                           return std::bit_cast<float>(value);
                         });
    }
    case internal::PhysicalTypeId::kFloat64: {
      arrow::DoubleBuilder builder;
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<double> {
                           ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader->ReadU64());
                           return std::bit_cast<double>(value);
                         });
    }
    case internal::PhysicalTypeId::kTimestamp: {
      arrow::TimestampBuilder builder(std::static_pointer_cast<arrow::TimestampType>(field.type),
                                      arrow::default_memory_pool());
      return DecodeFixed(chunk.row_count, validity, &value_reader, &builder,
                         [](internal::ByteReader* reader) -> arrow::Result<int64_t> {
                           ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader->ReadU64());
                           return std::bit_cast<int64_t>(value);
                         });
    }
    case internal::PhysicalTypeId::kString:
    case internal::PhysicalTypeId::kBinary:
      break;
  }
  return arrow::Status::NotImplemented("[sniffer.format.type] unknown physical type");
}

arrow::Result<std::shared_ptr<arrow::Array>> SelectArray(
    const std::shared_ptr<arrow::Array>& source, const std::vector<uint64_t>& selection) {
  ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(source->type()));
  if (selection.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return arrow::Status::Invalid("[sniffer.scan.limit] selection exceeds Arrow limit");
  }
  ARROW_RETURN_NOT_OK(builder->Reserve(static_cast<int64_t>(selection.size())));
  for (const uint64_t row : selection) {
    if (row >= static_cast<uint64_t>(source->length())) {
      return InvalidFormat("selection row exceeds source array");
    }
    ARROW_ASSIGN_OR_RAISE(auto scalar, source->GetScalar(static_cast<int64_t>(row)));
    ARROW_RETURN_NOT_OK(builder->AppendScalar(*scalar));
  }
  std::shared_ptr<arrow::Array> result;
  ARROW_RETURN_NOT_OK(builder->Finish(&result));
  return result;
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodePlainSelected(
    const FieldSpec& field, const internal::ColumnChunkMeta& chunk,
    std::span<const uint8_t> payload, const std::vector<uint64_t>& selection) {
  internal::ByteReader payload_reader(payload);
  ARROW_ASSIGN_OR_RAISE(const uint64_t validity_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t offsets_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t values_length, payload_reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(auto validity, payload_reader.ReadBytes(validity_length));
  ARROW_ASSIGN_OR_RAISE(auto offsets, payload_reader.ReadBytes(offsets_length));
  ARROW_ASSIGN_OR_RAISE(auto values, payload_reader.ReadBytes(values_length));
  if (payload_reader.remaining() != 0) {
    return InvalidFormat("trailing Plain payload bytes");
  }
  ARROW_RETURN_NOT_OK(ValidateValidity(validity, chunk.row_count, chunk.null_count));

  std::vector<uint64_t> variable_offsets;
  const bool variable = chunk.physical_type == internal::PhysicalTypeId::kString ||
                        chunk.physical_type == internal::PhysicalTypeId::kBinary;
  const uint32_t width = internal::FixedWidthBytes(chunk.physical_type);
  if (variable) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t offset_count,
                          internal::CheckedAdd(chunk.row_count, uint64_t{1}));
    ARROW_ASSIGN_OR_RAISE(const uint64_t expected_bytes,
                          internal::CheckedMultiply(offset_count, uint64_t{8}));
    if (offsets.size() != expected_bytes || offset_count > std::numeric_limits<size_t>::max()) {
      return InvalidFormat("invalid variable-size offset buffer length");
    }
    internal::ByteReader offset_reader(offsets);
    variable_offsets.reserve(static_cast<size_t>(offset_count));
    for (uint64_t index = 0; index < offset_count; ++index) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t offset, offset_reader.ReadU64());
      if ((!variable_offsets.empty() && offset < variable_offsets.back()) ||
          offset > values.size()) {
        return InvalidFormat("variable-size offsets are not monotonic and bounded");
      }
      variable_offsets.push_back(offset);
    }
    if (variable_offsets.front() != 0 || variable_offsets.back() != values.size()) {
      return InvalidFormat("variable-size offsets are not normalized");
    }
  } else {
    if (!offsets.empty()) {
      return InvalidFormat("fixed-width Plain payload has offsets");
    }
    ARROW_ASSIGN_OR_RAISE(const uint64_t expected_values,
                          internal::CheckedMultiply(chunk.row_count, static_cast<uint64_t>(width)));
    if (values.size() != expected_values) {
      return InvalidFormat("invalid fixed-width values length");
    }
  }

  ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(field.type));
  if (selection.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return arrow::Status::Invalid("[sniffer.scan.limit] selection exceeds Arrow limit");
  }
  ARROW_RETURN_NOT_OK(builder->Reserve(static_cast<int64_t>(selection.size())));
  uint64_t previous = 0;
  bool first_row = true;
  for (const uint64_t row : selection) {
    if (row >= chunk.row_count || (!first_row && row <= previous)) {
      return InvalidFormat("selection vector must be strictly increasing and bounded");
    }
    first_row = false;
    previous = row;
    if (!IsValid(validity, row)) {
      ARROW_RETURN_NOT_OK(builder->AppendNull());
      continue;
    }
    std::span<const uint8_t> scalar_bytes;
    if (variable) {
      const uint64_t begin = variable_offsets[static_cast<size_t>(row)];
      const uint64_t end = variable_offsets[static_cast<size_t>(row + 1U)];
      scalar_bytes = values.subspan(static_cast<size_t>(begin), static_cast<size_t>(end - begin));
    } else {
      ARROW_ASSIGN_OR_RAISE(const uint64_t begin,
                            internal::CheckedMultiply(row, static_cast<uint64_t>(width)));
      scalar_bytes = values.subspan(static_cast<size_t>(begin), width);
    }
    ARROW_ASSIGN_OR_RAISE(auto scalar, internal::ParseScalar(field, scalar_bytes));
    ARROW_RETURN_NOT_OK(builder->AppendScalar(*scalar));
  }
  std::shared_ptr<arrow::Array> result;
  ARROW_RETURN_NOT_OK(builder->Finish(&result));
  ARROW_RETURN_NOT_OK(result->ValidateFull());
  return result;
}

arrow::Result<size_t> FindFieldIndex(const TableSchema& schema, uint32_t field_id) {
  for (size_t index = 0; index < schema.fields.size(); ++index) {
    if (schema.fields[index].field_id == field_id) {
      return index;
    }
  }
  return arrow::Status::Invalid("[sniffer.plan.field] unknown field ID ", field_id);
}

arrow::Result<int> CompareKeyVectors(const std::vector<std::shared_ptr<arrow::Scalar>>& left,
                                     const std::vector<std::shared_ptr<arrow::Scalar>>& right) {
  if (left.size() != right.size()) {
    return arrow::Status::Invalid("[sniffer.plan.sort_key] key arity mismatch");
  }
  for (size_t index = 0; index < left.size(); ++index) {
    ARROW_ASSIGN_OR_RAISE(const int order, internal::CompareScalars(*left[index], *right[index]));
    if (order != 0) {
      return order;
    }
  }
  return 0;
}

arrow::Status ValidatePlan(const internal::FooterData& footer, const IOPlan& plan) {
  if (plan.output_batch_rows == 0) {
    return arrow::Status::Invalid("[sniffer.plan.batch] output_batch_rows must be positive");
  }
  std::unordered_set<uint32_t> projection_ids;
  for (const uint32_t field_id : plan.projection_field_ids) {
    ARROW_ASSIGN_OR_RAISE(const size_t ignored, FindFieldIndex(footer.schema, field_id));
    static_cast<void>(ignored);
    if (!projection_ids.insert(field_id).second) {
      return arrow::Status::Invalid("[sniffer.plan.projection] duplicate field ID ", field_id);
    }
  }
  for (const auto& predicate : plan.conjunctive_predicates) {
    ARROW_ASSIGN_OR_RAISE(const size_t field_index,
                          FindFieldIndex(footer.schema, predicate.field_id));
    const bool null_test =
        predicate.op == Predicate::Op::kIsNull || predicate.op == Predicate::Op::kIsNotNull;
    if (null_test) {
      if (predicate.value) {
        return arrow::Status::Invalid("[sniffer.plan.predicate] null test must not carry a value");
      }
    } else if (!predicate.value || !predicate.value->is_valid ||
               !predicate.value->type->Equals(footer.schema.fields[field_index].type)) {
      return arrow::Status::Invalid(
          "[sniffer.plan.predicate] comparison value must be non-null and exactly typed");
    }
  }
  if (!plan.sort_key_range) {
    return arrow::Status::OK();
  }
  const auto& sort_fields = footer.layout_policy.sort_key_field_ids;
  if (sort_fields.empty()) {
    return arrow::Status::Invalid(
        "[sniffer.plan.sort_key] range requires a configured segment sort key");
  }
  const auto validate_bound =
      [&footer,
       &sort_fields](const std::optional<std::vector<std::shared_ptr<arrow::Scalar>>>& bound)
      -> arrow::Status {
    if (!bound) {
      return arrow::Status::OK();
    }
    if (bound->size() != sort_fields.size()) {
      return arrow::Status::Invalid("[sniffer.plan.sort_key] bound arity mismatch");
    }
    for (size_t index = 0; index < bound->size(); ++index) {
      ARROW_ASSIGN_OR_RAISE(const size_t field_index,
                            FindFieldIndex(footer.schema, sort_fields[index]));
      const auto& value = (*bound)[index];
      if (!value || !value->is_valid || internal::ScalarHasNaN(*value) ||
          !value->type->Equals(footer.schema.fields[field_index].type)) {
        return arrow::Status::Invalid(
            "[sniffer.plan.sort_key] bound values must be non-null, non-NaN, and exactly typed");
      }
    }
    return arrow::Status::OK();
  };
  ARROW_RETURN_NOT_OK(validate_bound(plan.sort_key_range->lower));
  ARROW_RETURN_NOT_OK(validate_bound(plan.sort_key_range->upper));
  if (plan.sort_key_range->lower && plan.sort_key_range->upper) {
    ARROW_ASSIGN_OR_RAISE(const int order, CompareKeyVectors(*plan.sort_key_range->lower,
                                                             *plan.sort_key_range->upper));
    if (order > 0) {
      return arrow::Status::Invalid("[sniffer.plan.sort_key] lower bound exceeds upper bound");
    }
  }
  return arrow::Status::OK();
}

bool EvaluateOrderedResult(int order, Predicate::Op op) {
  switch (op) {
    case Predicate::Op::kEq:
      return order == 0;
    case Predicate::Op::kNe:
      return order != 0;
    case Predicate::Op::kLt:
      return order < 0;
    case Predicate::Op::kLe:
      return order <= 0;
    case Predicate::Op::kGt:
      return order > 0;
    case Predicate::Op::kGe:
      return order >= 0;
    case Predicate::Op::kIsNull:
    case Predicate::Op::kIsNotNull:
      break;
  }
  return false;
}

arrow::Result<bool> EvaluatePredicate(const arrow::Array& array, uint64_t row,
                                      const Predicate& predicate) {
  const bool is_null = array.IsNull(static_cast<int64_t>(row));
  if (predicate.op == Predicate::Op::kIsNull) {
    return is_null;
  }
  if (predicate.op == Predicate::Op::kIsNotNull) {
    return !is_null;
  }
  if (is_null) {
    return false;
  }
  ARROW_ASSIGN_OR_RAISE(auto value, array.GetScalar(static_cast<int64_t>(row)));
  if (internal::ScalarHasNaN(*value) || internal::ScalarHasNaN(*predicate.value)) {
    return predicate.op == Predicate::Op::kNe;
  }
  ARROW_ASSIGN_OR_RAISE(const int order, internal::CompareScalars(*value, *predicate.value));
  return EvaluateOrderedResult(order, predicate.op);
}

const internal::StatisticsMeta* FindStatistics(const internal::RowGroupIndex& index,
                                               uint32_t field_id) {
  const auto found = std::find_if(index.statistics.begin(), index.statistics.end(),
                                  [field_id](const internal::StatisticsMeta& candidate) {
                                    return candidate.field_id == field_id;
                                  });
  return found == index.statistics.end() ? nullptr : &*found;
}

const internal::BloomMeta* FindBloom(const internal::RowGroupIndex& index, uint32_t field_id) {
  const auto found = std::find_if(
      index.blooms.begin(), index.blooms.end(),
      [field_id](const internal::BloomMeta& candidate) { return candidate.field_id == field_id; });
  return found == index.blooms.end() ? nullptr : &*found;
}

arrow::Result<bool> PredicatePrunes(const TableSchema& schema,
                                    const internal::RowGroupMeta& row_group,
                                    const internal::RowGroupIndex& index,
                                    const Predicate& predicate) {
  const auto* statistics = FindStatistics(index, predicate.field_id);
  if (statistics) {
    if (predicate.op == Predicate::Op::kIsNull) {
      return statistics->null_count == 0;
    }
    if (predicate.op == Predicate::Op::kIsNotNull) {
      return statistics->null_count == row_group.row_count;
    }
    if (statistics->null_count == row_group.row_count) {
      return true;
    }
    if (internal::ScalarHasNaN(*predicate.value)) {
      return predicate.op != Predicate::Op::kNe;
    }
    if (statistics->min && statistics->max) {
      ARROW_ASSIGN_OR_RAISE(const int min_order,
                            internal::CompareScalars(*statistics->min, *predicate.value));
      ARROW_ASSIGN_OR_RAISE(const int max_order,
                            internal::CompareScalars(*statistics->max, *predicate.value));
      switch (predicate.op) {
        case Predicate::Op::kEq:
          if (min_order > 0 || max_order < 0) {
            return true;
          }
          break;
        case Predicate::Op::kNe:
          if (min_order == 0 && max_order == 0) {
            return true;
          }
          break;
        case Predicate::Op::kLt:
          if (min_order >= 0) {
            return true;
          }
          break;
        case Predicate::Op::kLe:
          if (min_order > 0) {
            return true;
          }
          break;
        case Predicate::Op::kGt:
          if (max_order <= 0) {
            return true;
          }
          break;
        case Predicate::Op::kGe:
          if (max_order < 0) {
            return true;
          }
          break;
        case Predicate::Op::kIsNull:
        case Predicate::Op::kIsNotNull:
          break;
      }
    }
  }
  if (predicate.op == Predicate::Op::kEq) {
    const auto* bloom = FindBloom(index, predicate.field_id);
    if (bloom) {
      ARROW_ASSIGN_OR_RAISE(const size_t field_index, FindFieldIndex(schema, predicate.field_id));
      ARROW_ASSIGN_OR_RAISE(
          const bool may_contain,
          internal::BloomMayContain(schema.fields[field_index], *bloom, *predicate.value));
      if (!may_contain) {
        return true;
      }
    }
  }
  return false;
}

arrow::Result<bool> SortRangePrunes(const internal::FooterData& footer,
                                    const internal::RowGroupIndex& index,
                                    const SortKeyRange& range) {
  if (index.sort_keys.size() != footer.layout_policy.sort_key_field_ids.size()) {
    return false;
  }
  std::vector<std::shared_ptr<arrow::Scalar>> first;
  std::vector<std::shared_ptr<arrow::Scalar>> last;
  first.reserve(index.sort_keys.size());
  last.reserve(index.sort_keys.size());
  for (size_t position = 0; position < index.sort_keys.size(); ++position) {
    if (index.sort_keys[position].field_id != footer.layout_policy.sort_key_field_ids[position]) {
      return false;
    }
    first.push_back(index.sort_keys[position].first);
    last.push_back(index.sort_keys[position].last);
  }
  if (range.lower) {
    ARROW_ASSIGN_OR_RAISE(const int order, CompareKeyVectors(last, *range.lower));
    if (order < 0 || (order == 0 && !range.lower_inclusive)) {
      return true;
    }
  }
  if (range.upper) {
    ARROW_ASSIGN_OR_RAISE(const int order, CompareKeyVectors(first, *range.upper));
    if (order > 0 || (order == 0 && !range.upper_inclusive)) {
      return true;
    }
  }
  return false;
}

}  // namespace

namespace internal {

arrow::Result<std::shared_ptr<arrow::Array>> DecodePlain(const FieldSpec& field,
                                                         const ColumnChunkMeta& chunk,
                                                         std::span<const uint8_t> payload) {
  return DecodePlainImpl(field, chunk, payload);
}

}  // namespace internal

class ScanState {
 public:
  ScanState(std::string path, uint64_t file_size, internal::FooterData footer,
            std::vector<internal::RowGroupIndex> indexes, IOPlan plan,
            std::shared_ptr<ScanMetrics> metrics, std::shared_ptr<arrow::Schema> output_schema)
      : path_(std::move(path)),
        file_size_(file_size),
        footer_(std::move(footer)),
        indexes_(std::move(indexes)),
        plan_(std::move(plan)),
        metrics_(std::move(metrics)),
        output_schema_(std::move(output_schema)) {}

  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Next() {
    if (plan_.limit && produced_ >= *plan_.limit) {
      return std::shared_ptr<arrow::RecordBatch>();
    }
    uint64_t target_rows = plan_.output_batch_rows;
    if (plan_.limit) {
      target_rows = std::min<uint64_t>(target_rows, *plan_.limit - produced_);
    }
    while (buffered_rows_ < target_rows && next_row_group_ < footer_.row_groups.size()) {
      uint64_t remaining_limit = std::numeric_limits<uint64_t>::max();
      if (plan_.limit) {
        remaining_limit = *plan_.limit - produced_ - buffered_rows_;
      }
      ARROW_ASSIGN_OR_RAISE(auto batch, ReadNextMatchingRowGroup(remaining_limit));
      if (batch) {
        buffered_rows_ += static_cast<uint64_t>(batch->num_rows());
        buffered_.push_back(std::move(batch));
      }
    }
    if (buffered_rows_ == 0) {
      return std::shared_ptr<arrow::RecordBatch>();
    }

    const uint64_t emit_rows = std::min<uint64_t>(target_rows, buffered_rows_);
    uint64_t remaining = emit_rows;
    arrow::RecordBatchVector pieces;
    while (remaining != 0) {
      const auto& front = buffered_.front();
      const int64_t available = front->num_rows() - buffered_front_offset_;
      const int64_t take =
          static_cast<int64_t>(std::min<uint64_t>(static_cast<uint64_t>(available), remaining));
      pieces.push_back(front->Slice(buffered_front_offset_, take));
      buffered_front_offset_ += take;
      remaining -= static_cast<uint64_t>(take);
      buffered_rows_ -= static_cast<uint64_t>(take);
      if (buffered_front_offset_ == front->num_rows()) {
        buffered_.pop_front();
        buffered_front_offset_ = 0;
      }
    }
    produced_ += emit_rows;
    if (pieces.size() == 1) {
      return pieces.front();
    }
    return arrow::ConcatenateRecordBatches(pieces);
  }

 private:
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadNextMatchingRowGroup(
      uint64_t remaining_limit) {
    while (next_row_group_ < footer_.row_groups.size() && remaining_limit != 0) {
      const size_t row_group_index = next_row_group_++;
      const auto& row_group = footer_.row_groups[row_group_index];
      const auto& index = indexes_[row_group_index];
      ++metrics_->row_groups_considered;
      ARROW_ASSIGN_OR_RAISE(const bool pruned, IsPruned(row_group, index));
      if (pruned) {
        ++metrics_->row_groups_pruned;
        continue;
      }

      std::unordered_map<size_t, std::shared_ptr<arrow::Array>> filter_columns;
      for (const auto& predicate : plan_.conjunctive_predicates) {
        ARROW_ASSIGN_OR_RAISE(const size_t field_index,
                              FindFieldIndex(footer_.schema, predicate.field_id));
        if (!filter_columns.contains(field_index)) {
          ARROW_ASSIGN_OR_RAISE(auto array, DecodePredicateChunk(row_group, field_index));
          filter_columns.emplace(field_index, std::move(array));
        }
      }
      if (plan_.sort_key_range) {
        for (const uint32_t field_id : footer_.layout_policy.sort_key_field_ids) {
          ARROW_ASSIGN_OR_RAISE(const size_t field_index, FindFieldIndex(footer_.schema, field_id));
          if (!filter_columns.contains(field_index)) {
            ARROW_ASSIGN_OR_RAISE(auto array, DecodePredicateChunk(row_group, field_index));
            filter_columns.emplace(field_index, std::move(array));
          }
        }
      }

      std::vector<uint64_t> selection;
      selection.reserve(
          static_cast<size_t>(std::min<uint64_t>(row_group.row_count, remaining_limit)));
      for (uint64_t row = 0;
           row < row_group.row_count && static_cast<uint64_t>(selection.size()) < remaining_limit;
           ++row) {
        ARROW_ASSIGN_OR_RAISE(const bool matches, RowMatches(row, filter_columns));
        if (matches) {
          selection.push_back(row);
        }
      }
      if (selection.empty()) {
        continue;
      }

      std::vector<std::shared_ptr<arrow::Array>> projected_columns;
      projected_columns.reserve(plan_.projection_field_ids.size());
      for (const uint32_t field_id : plan_.projection_field_ids) {
        ARROW_ASSIGN_OR_RAISE(const size_t field_index, FindFieldIndex(footer_.schema, field_id));
        const auto decoded = filter_columns.find(field_index);
        if (decoded != filter_columns.end()) {
          ARROW_ASSIGN_OR_RAISE(auto selected, SelectArray(decoded->second, selection));
          projected_columns.push_back(std::move(selected));
        } else {
          ARROW_ASSIGN_OR_RAISE(auto selected,
                                DecodeProjectionChunk(row_group, field_index, selection));
          projected_columns.push_back(std::move(selected));
        }
      }
      auto batch = arrow::RecordBatch::Make(output_schema_, static_cast<int64_t>(selection.size()),
                                            std::move(projected_columns));
      ARROW_RETURN_NOT_OK(batch->ValidateFull());
      return batch;
    }
    return std::shared_ptr<arrow::RecordBatch>();
  }
  arrow::Result<std::vector<uint8_t>> ReadChunk(const internal::ColumnChunkMeta& chunk) {
    ARROW_ASSIGN_OR_RAISE(auto payload, ReadRange(path_, file_size_, chunk.offset, chunk.length));
    ++metrics_->column_chunks_read;
    metrics_->chunk_bytes_read += chunk.length;
    if (internal::Crc32c(payload) != chunk.checksum) {
      return arrow::Status::Invalid(
          "[sniffer.format.checksum] ColumnChunk CRC32C mismatch for field ", chunk.field_id);
    }
    return payload;
  }

  arrow::Result<std::shared_ptr<arrow::Array>> DecodePredicateChunk(
      const internal::RowGroupMeta& row_group, size_t field_index) {
    const auto& chunk = row_group.chunks[field_index];
    ARROW_ASSIGN_OR_RAISE(auto payload, ReadChunk(chunk));
    std::shared_ptr<arrow::Array> array;
    if (chunk.encoding_id == internal::kPlainEncodingId) {
      ARROW_ASSIGN_OR_RAISE(
          array, internal::DecodePlain(footer_.schema.fields[field_index], chunk, payload));
    } else {
      ARROW_ASSIGN_OR_RAISE(
          array, internal::DecodeNonPlain(footer_.schema.fields[field_index], chunk, payload));
    }
    ++metrics_->predicate_chunks_decoded;
    return array;
  }

  arrow::Result<std::shared_ptr<arrow::Array>> DecodeProjectionChunk(
      const internal::RowGroupMeta& row_group, size_t field_index,
      const std::vector<uint64_t>& selection) {
    const auto& chunk = row_group.chunks[field_index];
    ARROW_ASSIGN_OR_RAISE(auto payload, ReadChunk(chunk));
    std::shared_ptr<arrow::Array> array;
    if (chunk.encoding_id == internal::kPlainEncodingId) {
      ARROW_ASSIGN_OR_RAISE(array, DecodePlainSelected(footer_.schema.fields[field_index], chunk,
                                                       payload, selection));
    } else {
      ARROW_ASSIGN_OR_RAISE(array, internal::DecodeNonPlain(footer_.schema.fields[field_index],
                                                            chunk, payload, &selection));
    }
    ++metrics_->projection_chunks_decoded;
    return array;
  }

  arrow::Result<bool> IsPruned(const internal::RowGroupMeta& row_group,
                               const internal::RowGroupIndex& index) const {
    for (const auto& predicate : plan_.conjunctive_predicates) {
      ARROW_ASSIGN_OR_RAISE(const bool prunes,
                            PredicatePrunes(footer_.schema, row_group, index, predicate));
      if (prunes) {
        return true;
      }
    }
    if (plan_.sort_key_range) {
      return SortRangePrunes(footer_, index, *plan_.sort_key_range);
    }
    return false;
  }

  arrow::Result<bool> RowMatches(
      uint64_t row,
      const std::unordered_map<size_t, std::shared_ptr<arrow::Array>>& columns) const {
    for (const auto& predicate : plan_.conjunctive_predicates) {
      ARROW_ASSIGN_OR_RAISE(const size_t field_index,
                            FindFieldIndex(footer_.schema, predicate.field_id));
      ARROW_ASSIGN_OR_RAISE(const bool matches,
                            EvaluatePredicate(*columns.at(field_index), row, predicate));
      if (!matches) {
        return false;
      }
    }
    if (!plan_.sort_key_range) {
      return true;
    }
    std::vector<std::shared_ptr<arrow::Scalar>> key;
    key.reserve(footer_.layout_policy.sort_key_field_ids.size());
    for (const uint32_t field_id : footer_.layout_policy.sort_key_field_ids) {
      ARROW_ASSIGN_OR_RAISE(const size_t field_index, FindFieldIndex(footer_.schema, field_id));
      ARROW_ASSIGN_OR_RAISE(auto value,
                            columns.at(field_index)->GetScalar(static_cast<int64_t>(row)));
      key.push_back(std::move(value));
    }
    const auto& range = *plan_.sort_key_range;
    if (range.lower) {
      ARROW_ASSIGN_OR_RAISE(const int order, CompareKeyVectors(key, *range.lower));
      if (order < 0 || (order == 0 && !range.lower_inclusive)) {
        return false;
      }
    }
    if (range.upper) {
      ARROW_ASSIGN_OR_RAISE(const int order, CompareKeyVectors(key, *range.upper));
      if (order > 0 || (order == 0 && !range.upper_inclusive)) {
        return false;
      }
    }
    return true;
  }

  std::string path_;
  uint64_t file_size_;
  internal::FooterData footer_;
  std::vector<internal::RowGroupIndex> indexes_;
  IOPlan plan_;
  std::shared_ptr<ScanMetrics> metrics_;
  std::shared_ptr<arrow::Schema> output_schema_;
  size_t next_row_group_ = 0;
  std::deque<std::shared_ptr<arrow::RecordBatch>> buffered_;
  uint64_t buffered_rows_ = 0;
  int64_t buffered_front_offset_ = 0;
  uint64_t produced_ = 0;
};

class SegmentReader::Impl {
 public:
  Impl(std::string path, uint64_t file_size, internal::FooterTrailer trailer,
       internal::FooterData footer, std::vector<internal::RowGroupIndex> indexes)
      : path_(std::move(path)),
        file_size_(file_size),
        trailer_(trailer),
        footer_(std::move(footer)),
        indexes_(std::move(indexes)) {}

  arrow::Status ValidateDirectory() const {
    uint64_t previous_end = internal::kHeaderSize;
    for (const auto& row_group : footer_.row_groups) {
      if (row_group.chunks.size() != footer_.schema.fields.size()) {
        return InvalidFormat("row group chunk count does not match schema");
      }
      for (size_t index = 0; index < row_group.chunks.size(); ++index) {
        const auto& field = footer_.schema.fields[index];
        const auto& chunk = row_group.chunks[index];
        ARROW_ASSIGN_OR_RAISE(const auto expected_type, internal::PhysicalTypeFor(*field.type));
        if (chunk.field_id != field.field_id || chunk.physical_type != expected_type) {
          return InvalidFormat("column chunk identity does not match schema");
        }
        if (std::find(footer_.encoding_ids.begin(), footer_.encoding_ids.end(),
                      chunk.encoding_id) == footer_.encoding_ids.end() ||
            !internal::EncodingSupports(chunk.encoding_id, *field.type)) {
          return arrow::Status::NotImplemented("[sniffer.format.encoding] unsupported encoding ID ",
                                               chunk.encoding_id);
        }
        if (chunk.row_count != row_group.row_count || chunk.null_count > chunk.row_count) {
          return InvalidFormat("invalid column chunk row/null count");
        }
        if (!field.nullable && chunk.null_count != 0) {
          return InvalidFormat("non-nullable field contains nulls");
        }
        const uint64_t minimum_length = chunk.encoding_id == internal::kPlainEncodingId        ? 24U
                                        : chunk.encoding_id == internal::kDictionaryEncodingId ? 48U
                                        : chunk.encoding_id == internal::kRleEncodingId        ? 16U
                                                                                        : 32U;
        if (chunk.length < minimum_length || chunk.uncompressed_length < 24U ||
            (chunk.encoding_id == internal::kPlainEncodingId &&
             chunk.uncompressed_length != chunk.length)) {
          return InvalidFormat("invalid encoded column chunk length");
        }
        ARROW_ASSIGN_OR_RAISE(const uint64_t chunk_end,
                              internal::CheckedAdd(chunk.offset, chunk.length));
        if (chunk.offset < internal::kHeaderSize || chunk.offset < previous_end ||
            chunk_end > trailer_.footer_offset) {
          return arrow::Status::Invalid("[sniffer.format.bounds] invalid column chunk range");
        }
        previous_end = chunk_end;
      }
      if (footer_.has_phase_two_metadata) {
        if (row_group.index_block.length < 20U) {
          return InvalidFormat("row group index block is too small");
        }
        ARROW_ASSIGN_OR_RAISE(
            const uint64_t index_end,
            internal::CheckedAdd(row_group.index_block.offset, row_group.index_block.length));
        if (row_group.index_block.offset < previous_end || index_end > trailer_.footer_offset) {
          return arrow::Status::Invalid("[sniffer.format.bounds] invalid index block range");
        }
        previous_end = index_end;
      } else if (row_group.index_block.offset != 0 || row_group.index_block.length != 0 ||
                 row_group.index_block.checksum != 0) {
        return InvalidFormat("legacy footer contains index metadata");
      }
    }
    if (previous_end > trailer_.footer_offset) {
      return arrow::Status::Invalid("[sniffer.format.bounds] data overlaps footer");
    }
    return arrow::Status::OK();
  }

  arrow::Status LoadIndexes() {
    indexes_.clear();
    indexes_.reserve(footer_.row_groups.size());
    if (!footer_.has_phase_two_metadata) {
      indexes_.resize(footer_.row_groups.size());
      return arrow::Status::OK();
    }
    std::vector<std::shared_ptr<arrow::Scalar>> previous_last_key;
    for (const auto& row_group : footer_.row_groups) {
      ARROW_ASSIGN_OR_RAISE(auto bytes, ReadRange(path_, file_size_, row_group.index_block.offset,
                                                  row_group.index_block.length));
      if (internal::Crc32c(bytes) != row_group.index_block.checksum) {
        return arrow::Status::Invalid("[sniffer.format.checksum] index block CRC32C mismatch");
      }
      ARROW_ASSIGN_OR_RAISE(auto index, internal::ParseIndexBlock(footer_.schema, bytes));
      if (index.statistics.size() != footer_.layout_policy.statistics_field_ids.size() ||
          index.blooms.size() != footer_.layout_policy.bloom_field_ids.size() ||
          index.sort_keys.size() != footer_.layout_policy.sort_key_field_ids.size()) {
        return InvalidFormat("index block does not match layout policy");
      }
      for (size_t position = 0; position < index.statistics.size(); ++position) {
        if (index.statistics[position].field_id !=
                footer_.layout_policy.statistics_field_ids[position] ||
            index.statistics[position].null_count > row_group.row_count) {
          return InvalidFormat("statistics index does not match row group");
        }
      }
      for (size_t position = 0; position < index.blooms.size(); ++position) {
        if (index.blooms[position].field_id != footer_.layout_policy.bloom_field_ids[position]) {
          return InvalidFormat("Bloom index does not match layout policy");
        }
      }
      std::vector<std::shared_ptr<arrow::Scalar>> first_key;
      std::vector<std::shared_ptr<arrow::Scalar>> last_key;
      for (size_t position = 0; position < index.sort_keys.size(); ++position) {
        if (index.sort_keys[position].field_id !=
            footer_.layout_policy.sort_key_field_ids[position]) {
          return InvalidFormat("sort-key index does not match layout policy");
        }
        first_key.push_back(index.sort_keys[position].first);
        last_key.push_back(index.sort_keys[position].last);
      }
      if (!first_key.empty()) {
        ARROW_ASSIGN_OR_RAISE(const int local_order, CompareKeyVectors(first_key, last_key));
        if (local_order > 0) {
          return InvalidFormat("sort-key row-group boundaries are reversed");
        }
        if (!previous_last_key.empty()) {
          ARROW_ASSIGN_OR_RAISE(const int global_order,
                                CompareKeyVectors(previous_last_key, first_key));
          if (global_order > 0) {
            return InvalidFormat("sort-key row groups are not globally ordered");
          }
        }
        previous_last_key = std::move(last_key);
      }
      indexes_.push_back(std::move(index));
    }
    return arrow::Status::OK();
  }

  arrow::Result<arrow::RecordBatchIterator> Scan(IOPlan plan,
                                                 std::shared_ptr<ScanMetrics> metrics) const {
    ARROW_RETURN_NOT_OK(ValidatePlan(footer_, plan));
    if (!metrics) {
      metrics = std::make_shared<ScanMetrics>();
    } else {
      *metrics = {};
    }
    ARROW_ASSIGN_OR_RAISE(auto full_schema, footer_.schema.ToArrowSchema());
    std::vector<std::shared_ptr<arrow::Field>> projected_fields;
    projected_fields.reserve(plan.projection_field_ids.size());
    for (const uint32_t field_id : plan.projection_field_ids) {
      ARROW_ASSIGN_OR_RAISE(const size_t field_index, FindFieldIndex(footer_.schema, field_id));
      projected_fields.push_back(full_schema->field(static_cast<int>(field_index)));
    }
    auto output_schema = arrow::schema(std::move(projected_fields), full_schema->metadata());
    auto state = std::make_shared<ScanState>(path_, file_size_, footer_, indexes_, std::move(plan),
                                             std::move(metrics), std::move(output_schema));
    return arrow::MakeFunctionIterator(
        [state]() -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> { return state->Next(); });
  }

  arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> ReadAll() const {
    ARROW_ASSIGN_OR_RAISE(auto arrow_schema, footer_.schema.ToArrowSchema());
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    batches.reserve(std::max<size_t>(1, footer_.row_groups.size()));
    for (const auto& row_group : footer_.row_groups) {
      std::vector<std::shared_ptr<arrow::Array>> columns;
      columns.reserve(row_group.chunks.size());
      for (size_t index = 0; index < row_group.chunks.size(); ++index) {
        const auto& chunk = row_group.chunks[index];
        ARROW_ASSIGN_OR_RAISE(auto payload,
                              ReadRange(path_, file_size_, chunk.offset, chunk.length));
        if (internal::Crc32c(payload) != chunk.checksum) {
          return arrow::Status::Invalid(
              "[sniffer.format.checksum] ColumnChunk CRC32C mismatch for field ", chunk.field_id);
        }
        std::shared_ptr<arrow::Array> array;
        if (chunk.encoding_id == internal::kPlainEncodingId) {
          ARROW_ASSIGN_OR_RAISE(
              array, internal::DecodePlain(footer_.schema.fields[index], chunk, payload));
        } else {
          ARROW_ASSIGN_OR_RAISE(
              array, internal::DecodeNonPlain(footer_.schema.fields[index], chunk, payload));
        }
        columns.push_back(std::move(array));
      }
      auto batch = arrow::RecordBatch::Make(arrow_schema, static_cast<int64_t>(row_group.row_count),
                                            std::move(columns));
      ARROW_RETURN_NOT_OK(batch->ValidateFull());
      batches.push_back(std::move(batch));
    }

    if (batches.empty()) {
      std::vector<std::shared_ptr<arrow::Array>> columns;
      columns.reserve(footer_.schema.fields.size());
      for (const auto& field : footer_.schema.fields) {
        ARROW_ASSIGN_OR_RAISE(auto array, arrow::MakeArrayOfNull(field.type, 0));
        columns.push_back(std::move(array));
      }
      batches.push_back(arrow::RecordBatch::Make(arrow_schema, 0, std::move(columns)));
    }
    return batches;
  }

  arrow::Status VerifyFileChecksum() const {
    std::ifstream stream(path_, std::ios::binary);
    if (!stream.is_open()) {
      return IoError("cannot open segment for checksum", path_);
    }
    uint64_t remaining = trailer_.footer_offset + trailer_.footer_length;
    uint32_t checksum = 0;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (remaining != 0) {
      const size_t to_read =
          static_cast<size_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer.size())));
      stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(to_read));
      if (!stream) {
        return IoError("cannot read segment for checksum", path_);
      }
      checksum = internal::Crc32c(std::span<const uint8_t>(buffer.data(), to_read), checksum);
      remaining -= static_cast<uint64_t>(to_read);
    }
    if (checksum != trailer_.file_checksum) {
      return arrow::Status::Invalid("[sniffer.format.checksum] file CRC32C mismatch");
    }
    return arrow::Status::OK();
  }

  const TableSchema& schema() const { return footer_.schema; }
  uint64_t num_row_groups() const { return static_cast<uint64_t>(footer_.row_groups.size()); }

 private:
  std::string path_;
  uint64_t file_size_;
  internal::FooterTrailer trailer_;
  internal::FooterData footer_;
  std::vector<internal::RowGroupIndex> indexes_;
};

arrow::Result<std::unique_ptr<SegmentReader>> SegmentReader::Open(std::string path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream.is_open()) {
    return IoError("cannot open segment for reading", path);
  }
  const std::streampos end_position = stream.tellg();
  if (end_position < 0) {
    return IoError("cannot determine segment size", path);
  }
  const uint64_t file_size = static_cast<uint64_t>(end_position);
  if (file_size < internal::kHeaderSize + internal::kTrailerSize) {
    return arrow::Status::Invalid("[sniffer.format.truncated] segment is smaller than envelope");
  }
  stream.close();

  ARROW_ASSIGN_OR_RAISE(auto header, ReadRange(path, file_size, 0, internal::kHeaderSize));
  ARROW_RETURN_NOT_OK(internal::ValidateHeader(header));
  ARROW_ASSIGN_OR_RAISE(
      auto trailer_bytes,
      ReadRange(path, file_size, file_size - internal::kTrailerSize, internal::kTrailerSize));
  ARROW_ASSIGN_OR_RAISE(auto trailer, internal::ParseTrailer(trailer_bytes));
  ARROW_ASSIGN_OR_RAISE(const uint64_t footer_end,
                        internal::CheckedAdd(trailer.footer_offset, trailer.footer_length));
  if (trailer.footer_offset < internal::kHeaderSize ||
      footer_end != file_size - internal::kTrailerSize) {
    return arrow::Status::Invalid("[sniffer.format.bounds] invalid footer range");
  }
  ARROW_ASSIGN_OR_RAISE(auto footer_bytes,
                        ReadRange(path, file_size, trailer.footer_offset, trailer.footer_length));
  if (internal::Crc32c(footer_bytes) != trailer.footer_checksum) {
    return arrow::Status::Invalid("[sniffer.format.checksum] footer CRC32C mismatch");
  }
  ARROW_ASSIGN_OR_RAISE(auto footer, internal::ParseFooter(footer_bytes));
  auto impl = std::make_unique<Impl>(std::move(path), file_size, trailer, std::move(footer),
                                     std::vector<internal::RowGroupIndex>{});
  ARROW_RETURN_NOT_OK(impl->ValidateDirectory());
  ARROW_RETURN_NOT_OK(impl->LoadIndexes());
  return std::unique_ptr<SegmentReader>(new SegmentReader(std::move(impl)));
}

SegmentReader::SegmentReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SegmentReader::~SegmentReader() = default;

const TableSchema& SegmentReader::schema() const { return impl_->schema(); }

uint64_t SegmentReader::num_row_groups() const { return impl_->num_row_groups(); }

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> SegmentReader::ReadAll() const {
  return impl_->ReadAll();
}

arrow::Result<arrow::RecordBatchIterator> SegmentReader::Scan(
    IOPlan plan, std::shared_ptr<ScanMetrics> metrics) const {
  return impl_->Scan(std::move(plan), std::move(metrics));
}

arrow::Status SegmentReader::VerifyFileChecksum() const { return impl_->VerifyFileChecksum(); }

}  // namespace sniffer
