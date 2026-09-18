#include "scalar_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "format_internal.h"

namespace sniffer::internal {
namespace {

arrow::Status InvalidScalar(const FieldSpec& field, const std::string& detail) {
  return arrow::Status::Invalid("[sniffer.scalar] field ", field.field_id, ": ", detail);
}

template <typename ScalarType>
int ComparePrimitive(const arrow::Scalar& left, const arrow::Scalar& right) {
  const auto left_value = static_cast<const ScalarType&>(left).value;
  const auto right_value = static_cast<const ScalarType&>(right).value;
  if (left_value < right_value) {
    return -1;
  }
  if (right_value < left_value) {
    return 1;
  }
  return 0;
}

int CompareBinary(const arrow::Scalar& left, const arrow::Scalar& right) {
  const auto left_view = static_cast<const arrow::BaseBinaryScalar&>(left).view();
  const auto right_view = static_cast<const arrow::BaseBinaryScalar&>(right).view();
  const auto* left_begin = reinterpret_cast<const uint8_t*>(left_view.data());
  const auto* right_begin = reinterpret_cast<const uint8_t*>(right_view.data());
  const std::span<const uint8_t> left_bytes(left_begin, left_view.size());
  const std::span<const uint8_t> right_bytes(right_begin, right_view.size());
  if (std::lexicographical_compare(left_bytes.begin(), left_bytes.end(), right_bytes.begin(),
                                   right_bytes.end())) {
    return -1;
  }
  if (std::lexicographical_compare(right_bytes.begin(), right_bytes.end(), left_bytes.begin(),
                                   left_bytes.end())) {
    return 1;
  }
  return 0;
}

uint64_t Mix64(uint64_t value) {
  value ^= value >> 30U;
  value *= 0xBF58476D1CE4E5B9ULL;
  value ^= value >> 27U;
  value *= 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

void HashByte(uint8_t byte, uint64_t* hash) {
  *hash ^= byte;
  *hash *= 1099511628211ULL;
}

template <typename Unsigned, typename EmitByte>
void EmitLittleEndian(Unsigned value, EmitByte&& emit_byte) {
  for (size_t index = 0; index < sizeof(Unsigned); ++index) {
    emit_byte(static_cast<uint8_t>(value >> (index * 8U)));
  }
}

arrow::Result<uint64_t> HashBytes(const FieldSpec& field, std::span<const uint8_t> bytes,
                                  uint64_t seed) {
  uint64_t hash = 1469598103934665603ULL ^ seed;
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  HashByte(static_cast<uint8_t>(physical_type), &hash);
  for (const uint8_t byte : bytes) {
    HashByte(byte, &hash);
  }
  return Mix64(hash);
}

template <typename ArrayType, typename Unsigned>
Unsigned ArrayBits(const arrow::Array& untyped, int64_t row) {
  const auto value = static_cast<const ArrayType&>(untyped).Value(row);
  if constexpr (std::is_same_v<decltype(value), Unsigned>) {
    return value;
  } else {
    return std::bit_cast<Unsigned>(value);
  }
}

template <typename EmitByte>
arrow::Status EmitArrayValueBytesBound(const FieldSpec& field, const arrow::Array& array,
                                       int64_t row, PhysicalTypeId physical_type,
                                       EmitByte&& emit_byte) {
  emit_byte(static_cast<uint8_t>(physical_type));
  switch (field.type->id()) {
    case arrow::Type::BOOL:
      emit_byte(static_cast<const arrow::BooleanArray&>(array).Value(row) ? 1U : 0U);
      break;
    case arrow::Type::INT8:
      EmitLittleEndian(ArrayBits<arrow::Int8Array, uint8_t>(array, row), emit_byte);
      break;
    case arrow::Type::INT16:
      EmitLittleEndian(ArrayBits<arrow::Int16Array, uint16_t>(array, row), emit_byte);
      break;
    case arrow::Type::INT32:
      EmitLittleEndian(ArrayBits<arrow::Int32Array, uint32_t>(array, row), emit_byte);
      break;
    case arrow::Type::INT64:
      EmitLittleEndian(ArrayBits<arrow::Int64Array, uint64_t>(array, row), emit_byte);
      break;
    case arrow::Type::UINT8:
      EmitLittleEndian(ArrayBits<arrow::UInt8Array, uint8_t>(array, row), emit_byte);
      break;
    case arrow::Type::UINT16:
      EmitLittleEndian(ArrayBits<arrow::UInt16Array, uint16_t>(array, row), emit_byte);
      break;
    case arrow::Type::UINT32:
      EmitLittleEndian(ArrayBits<arrow::UInt32Array, uint32_t>(array, row), emit_byte);
      break;
    case arrow::Type::UINT64:
      EmitLittleEndian(ArrayBits<arrow::UInt64Array, uint64_t>(array, row), emit_byte);
      break;
    case arrow::Type::FLOAT: {
      const float value = static_cast<const arrow::FloatArray&>(array).Value(row);
      EmitLittleEndian(value == 0.0F ? 0U : std::bit_cast<uint32_t>(value), emit_byte);
      break;
    }
    case arrow::Type::DOUBLE: {
      const double value = static_cast<const arrow::DoubleArray&>(array).Value(row);
      EmitLittleEndian(value == 0.0 ? 0ULL : std::bit_cast<uint64_t>(value), emit_byte);
      break;
    }
    case arrow::Type::TIMESTAMP:
      EmitLittleEndian(ArrayBits<arrow::TimestampArray, uint64_t>(array, row), emit_byte);
      break;
    case arrow::Type::STRING: {
      const auto value = static_cast<const arrow::StringArray&>(array).GetView(row);
      for (const char byte : value) {
        emit_byte(static_cast<uint8_t>(byte));
      }
      break;
    }
    case arrow::Type::BINARY: {
      const auto value = static_cast<const arrow::BinaryArray&>(array).GetView(row);
      for (const char byte : value) {
        emit_byte(static_cast<uint8_t>(byte));
      }
      break;
    }
    default:
      return InvalidScalar(field, "unsupported array type");
  }
  return arrow::Status::OK();
}

template <typename EmitByte>
arrow::Status EmitArrayValueBytes(const FieldSpec& field, const arrow::Array& array, int64_t row,
                                  EmitByte&& emit_byte) {
  if (!array.type()->Equals(field.type)) {
    return InvalidScalar(field, "array type does not match field");
  }
  if (row < 0 || row >= array.length() || array.IsNull(row)) {
    return InvalidScalar(field, "cannot hash missing array value");
  }
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  return EmitArrayValueBytesBound(field, array, row, physical_type,
                                  std::forward<EmitByte>(emit_byte));
}

}  // namespace

arrow::Result<ArrayValuePairHasher> ArrayValuePairHasher::Bind(const FieldSpec& field,
                                                               const arrow::Array& array) {
  if (!array.type()->Equals(field.type)) {
    return InvalidScalar(field, "array type does not match field");
  }
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  return ArrayValuePairHasher(&field, &array, static_cast<uint8_t>(physical_type));
}

arrow::Result<std::pair<uint64_t, uint64_t>> ArrayValuePairHasher::Hash(
    int64_t row, uint64_t first_seed, uint64_t second_seed) const {
  if (row < 0 || row >= array_->length() || array_->IsNull(row)) {
    return InvalidScalar(*field_, "cannot hash missing array value");
  }
  uint64_t first = 1469598103934665603ULL ^ first_seed;
  uint64_t second = 1469598103934665603ULL ^ second_seed;
  ARROW_RETURN_NOT_OK(EmitArrayValueBytesBound(
      *field_, *array_, row, static_cast<PhysicalTypeId>(physical_type_), [&](uint8_t byte) {
        HashByte(byte, &first);
        HashByte(byte, &second);
      }));
  return std::pair<uint64_t, uint64_t>{Mix64(first), Mix64(second)};
}

bool ScalarHasNaN(const arrow::Scalar& scalar) {
  if (!scalar.is_valid) {
    return false;
  }
  if (scalar.type->id() == arrow::Type::FLOAT) {
    return std::isnan(static_cast<const arrow::FloatScalar&>(scalar).value);
  }
  if (scalar.type->id() == arrow::Type::DOUBLE) {
    return std::isnan(static_cast<const arrow::DoubleScalar&>(scalar).value);
  }
  return false;
}

arrow::Result<int> CompareScalars(const arrow::Scalar& left, const arrow::Scalar& right) {
  if (!left.is_valid || !right.is_valid) {
    return arrow::Status::Invalid("[sniffer.scalar] null scalar is not orderable");
  }
  if (!left.type->Equals(right.type)) {
    return arrow::Status::Invalid("[sniffer.scalar] cannot compare different Arrow types");
  }
  if (ScalarHasNaN(left) || ScalarHasNaN(right)) {
    return arrow::Status::Invalid("[sniffer.scalar] NaN scalar is not orderable");
  }
  switch (left.type->id()) {
    case arrow::Type::BOOL:
      return ComparePrimitive<arrow::BooleanScalar>(left, right);
    case arrow::Type::INT8:
      return ComparePrimitive<arrow::Int8Scalar>(left, right);
    case arrow::Type::INT16:
      return ComparePrimitive<arrow::Int16Scalar>(left, right);
    case arrow::Type::INT32:
      return ComparePrimitive<arrow::Int32Scalar>(left, right);
    case arrow::Type::INT64:
      return ComparePrimitive<arrow::Int64Scalar>(left, right);
    case arrow::Type::UINT8:
      return ComparePrimitive<arrow::UInt8Scalar>(left, right);
    case arrow::Type::UINT16:
      return ComparePrimitive<arrow::UInt16Scalar>(left, right);
    case arrow::Type::UINT32:
      return ComparePrimitive<arrow::UInt32Scalar>(left, right);
    case arrow::Type::UINT64:
      return ComparePrimitive<arrow::UInt64Scalar>(left, right);
    case arrow::Type::FLOAT:
      return ComparePrimitive<arrow::FloatScalar>(left, right);
    case arrow::Type::DOUBLE:
      return ComparePrimitive<arrow::DoubleScalar>(left, right);
    case arrow::Type::TIMESTAMP:
      return ComparePrimitive<arrow::TimestampScalar>(left, right);
    case arrow::Type::STRING:
    case arrow::Type::BINARY:
      return CompareBinary(left, right);
    default:
      return arrow::Status::NotImplemented("[sniffer.scalar] unsupported comparison type");
  }
}

arrow::Result<std::vector<uint8_t>> SerializeScalar(const FieldSpec& field,
                                                    const arrow::Scalar& scalar) {
  if (!scalar.is_valid) {
    return InvalidScalar(field, "cannot serialize null scalar");
  }
  if (!scalar.type->Equals(field.type)) {
    return InvalidScalar(field, "scalar type does not match field");
  }
  ARROW_RETURN_NOT_OK(scalar.ValidateFull());

  ByteWriter writer;
  switch (field.type->id()) {
    case arrow::Type::BOOL:
      writer.WriteU8(static_cast<const arrow::BooleanScalar&>(scalar).value ? 1U : 0U);
      break;
    case arrow::Type::INT8:
      writer.WriteU8(std::bit_cast<uint8_t>(static_cast<const arrow::Int8Scalar&>(scalar).value));
      break;
    case arrow::Type::INT16:
      writer.WriteU16(
          std::bit_cast<uint16_t>(static_cast<const arrow::Int16Scalar&>(scalar).value));
      break;
    case arrow::Type::INT32:
      writer.WriteU32(
          std::bit_cast<uint32_t>(static_cast<const arrow::Int32Scalar&>(scalar).value));
      break;
    case arrow::Type::INT64:
      writer.WriteU64(
          std::bit_cast<uint64_t>(static_cast<const arrow::Int64Scalar&>(scalar).value));
      break;
    case arrow::Type::UINT8:
      writer.WriteU8(static_cast<const arrow::UInt8Scalar&>(scalar).value);
      break;
    case arrow::Type::UINT16:
      writer.WriteU16(static_cast<const arrow::UInt16Scalar&>(scalar).value);
      break;
    case arrow::Type::UINT32:
      writer.WriteU32(static_cast<const arrow::UInt32Scalar&>(scalar).value);
      break;
    case arrow::Type::UINT64:
      writer.WriteU64(static_cast<const arrow::UInt64Scalar&>(scalar).value);
      break;
    case arrow::Type::FLOAT:
      writer.WriteU32(
          std::bit_cast<uint32_t>(static_cast<const arrow::FloatScalar&>(scalar).value));
      break;
    case arrow::Type::DOUBLE:
      writer.WriteU64(
          std::bit_cast<uint64_t>(static_cast<const arrow::DoubleScalar&>(scalar).value));
      break;
    case arrow::Type::TIMESTAMP:
      writer.WriteU64(
          std::bit_cast<uint64_t>(static_cast<const arrow::TimestampScalar&>(scalar).value));
      break;
    case arrow::Type::STRING:
    case arrow::Type::BINARY: {
      const auto value = static_cast<const arrow::BaseBinaryScalar&>(scalar).view();
      writer.WriteBytes(
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
      break;
    }
    default:
      return InvalidScalar(field, "unsupported scalar type");
  }
  return std::move(writer).Finish();
}

arrow::Result<std::shared_ptr<arrow::Scalar>> ParseScalar(const FieldSpec& field,
                                                          std::span<const uint8_t> bytes) {
  ByteReader reader(bytes);
  std::shared_ptr<arrow::Scalar> scalar;
  switch (field.type->id()) {
    case arrow::Type::BOOL: {
      ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader.ReadU8());
      if (value > 1U) {
        return InvalidScalar(field, "invalid Boolean value");
      }
      scalar = std::make_shared<arrow::BooleanScalar>(value != 0);
      break;
    }
    case arrow::Type::INT8: {
      ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader.ReadU8());
      scalar = std::make_shared<arrow::Int8Scalar>(std::bit_cast<int8_t>(value));
      break;
    }
    case arrow::Type::INT16: {
      ARROW_ASSIGN_OR_RAISE(const uint16_t value, reader.ReadU16());
      scalar = std::make_shared<arrow::Int16Scalar>(std::bit_cast<int16_t>(value));
      break;
    }
    case arrow::Type::INT32: {
      ARROW_ASSIGN_OR_RAISE(const uint32_t value, reader.ReadU32());
      scalar = std::make_shared<arrow::Int32Scalar>(std::bit_cast<int32_t>(value));
      break;
    }
    case arrow::Type::INT64: {
      ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader.ReadU64());
      scalar = std::make_shared<arrow::Int64Scalar>(std::bit_cast<int64_t>(value));
      break;
    }
    case arrow::Type::UINT8: {
      ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader.ReadU8());
      scalar = std::make_shared<arrow::UInt8Scalar>(value);
      break;
    }
    case arrow::Type::UINT16: {
      ARROW_ASSIGN_OR_RAISE(const uint16_t value, reader.ReadU16());
      scalar = std::make_shared<arrow::UInt16Scalar>(value);
      break;
    }
    case arrow::Type::UINT32: {
      ARROW_ASSIGN_OR_RAISE(const uint32_t value, reader.ReadU32());
      scalar = std::make_shared<arrow::UInt32Scalar>(value);
      break;
    }
    case arrow::Type::UINT64: {
      ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader.ReadU64());
      scalar = std::make_shared<arrow::UInt64Scalar>(value);
      break;
    }
    case arrow::Type::FLOAT: {
      ARROW_ASSIGN_OR_RAISE(const uint32_t value, reader.ReadU32());
      scalar = std::make_shared<arrow::FloatScalar>(std::bit_cast<float>(value));
      break;
    }
    case arrow::Type::DOUBLE: {
      ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader.ReadU64());
      scalar = std::make_shared<arrow::DoubleScalar>(std::bit_cast<double>(value));
      break;
    }
    case arrow::Type::TIMESTAMP: {
      ARROW_ASSIGN_OR_RAISE(const uint64_t value, reader.ReadU64());
      scalar = std::make_shared<arrow::TimestampScalar>(std::bit_cast<int64_t>(value), field.type);
      break;
    }
    case arrow::Type::STRING:
      scalar = std::make_shared<arrow::StringScalar>(
          bytes.empty() ? std::string()
                        : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      reader = ByteReader({});
      break;
    case arrow::Type::BINARY:
      scalar = std::make_shared<arrow::BinaryScalar>(
          bytes.empty() ? std::string()
                        : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      reader = ByteReader({});
      break;
    default:
      return InvalidScalar(field, "unsupported scalar type");
  }
  if (field.type->id() != arrow::Type::STRING && field.type->id() != arrow::Type::BINARY &&
      reader.remaining() != 0) {
    return InvalidScalar(field, "wrong scalar byte length");
  }
  ARROW_RETURN_NOT_OK(scalar->ValidateFull());
  return scalar;
}

arrow::Result<uint64_t> HashScalar(const FieldSpec& field, const arrow::Scalar& scalar,
                                   uint64_t seed) {
  ARROW_ASSIGN_OR_RAISE(auto bytes, SerializeScalar(field, scalar));
  if (field.type->id() == arrow::Type::FLOAT &&
      static_cast<const arrow::FloatScalar&>(scalar).value == 0.0F) {
    bytes.assign(4, 0);
  } else if (field.type->id() == arrow::Type::DOUBLE &&
             static_cast<const arrow::DoubleScalar&>(scalar).value == 0.0) {
    bytes.assign(8, 0);
  }

  return HashBytes(field, bytes, seed);
}

arrow::Result<uint64_t> HashArrayValue(const FieldSpec& field, const arrow::Array& array,
                                       int64_t row, uint64_t seed) {
  uint64_t hash = 1469598103934665603ULL ^ seed;
  ARROW_RETURN_NOT_OK(
      EmitArrayValueBytes(field, array, row, [&hash](uint8_t byte) { HashByte(byte, &hash); }));
  return Mix64(hash);
}

arrow::Result<std::pair<uint64_t, uint64_t>> HashArrayValuePair(const FieldSpec& field,
                                                                const arrow::Array& array,
                                                                int64_t row, uint64_t first_seed,
                                                                uint64_t second_seed) {
  uint64_t first = 1469598103934665603ULL ^ first_seed;
  uint64_t second = 1469598103934665603ULL ^ second_seed;
  ARROW_RETURN_NOT_OK(EmitArrayValueBytes(field, array, row, [&](uint8_t byte) {
    HashByte(byte, &first);
    HashByte(byte, &second);
  }));
  return std::pair<uint64_t, uint64_t>{Mix64(first), Mix64(second)};
}

}  // namespace sniffer::internal
