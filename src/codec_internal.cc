#include "codec_internal.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "scalar_internal.h"

namespace sniffer::internal {
namespace {

arrow::Status InvalidCodec(const std::string& detail) {
  return arrow::Status::Invalid("[sniffer.codec.invalid] ", detail);
}

bool IsInteger(arrow::Type::type type) {
  return type == arrow::Type::INT8 || type == arrow::Type::INT16 || type == arrow::Type::INT32 ||
         type == arrow::Type::INT64 || type == arrow::Type::UINT8 || type == arrow::Type::UINT16 ||
         type == arrow::Type::UINT32 || type == arrow::Type::UINT64;
}

bool IsVariable(arrow::Type::type type) {
  return type == arrow::Type::STRING || type == arrow::Type::BINARY;
}

std::vector<uint8_t> EncodeValidity(const arrow::Array& array) {
  if (array.null_count() == 0) {
    return {};
  }
  const uint64_t rows = static_cast<uint64_t>(array.length());
  std::vector<uint8_t> validity(static_cast<size_t>(rows / 8U + (rows % 8U != 0)), 0);
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsValid(row)) {
      validity[static_cast<size_t>(row / 8)] |=
          static_cast<uint8_t>(1U << static_cast<uint32_t>(row % 8));
    }
  }
  return validity;
}

bool IsValid(std::span<const uint8_t> validity, uint64_t row) {
  return validity.empty() || (validity[static_cast<size_t>(row / 8U)] &
                              static_cast<uint8_t>(1U << static_cast<uint32_t>(row % 8U))) != 0;
}

arrow::Status ValidateValidity(std::span<const uint8_t> validity, uint64_t row_count,
                               uint64_t null_count) {
  const uint64_t expected = null_count == 0 ? 0 : row_count / 8U + (row_count % 8U != 0);
  if (validity.size() != expected) {
    return InvalidCodec("invalid validity length");
  }
  uint64_t observed_nulls = 0;
  for (uint64_t row = 0; row < row_count; ++row) {
    observed_nulls += IsValid(validity, row) ? 0U : 1U;
  }
  if (observed_nulls != null_count) {
    return InvalidCodec("validity does not match null count");
  }
  if (!validity.empty() && row_count % 8U != 0) {
    const uint8_t padding = static_cast<uint8_t>(0xFFU << static_cast<uint32_t>(row_count % 8U));
    if ((validity.back() & padding) != 0) {
      return InvalidCodec("non-zero validity padding bits");
    }
  }
  return arrow::Status::OK();
}

uint32_t IndexWidth(uint64_t dictionary_count) {
  if (dictionary_count <= 256U) {
    return 1;
  }
  if (dictionary_count <= 65536U) {
    return 2;
  }
  if (dictionary_count <= std::numeric_limits<uint32_t>::max()) {
    return 4;
  }
  return 8;
}

void WriteWidth(ByteWriter* writer, uint64_t value, uint32_t width) {
  switch (width) {
    case 1:
      writer->WriteU8(static_cast<uint8_t>(value));
      break;
    case 2:
      writer->WriteU16(static_cast<uint16_t>(value));
      break;
    case 4:
      writer->WriteU32(static_cast<uint32_t>(value));
      break;
    case 8:
      writer->WriteU64(value);
      break;
    default:
      break;
  }
}

arrow::Result<uint64_t> ReadWidth(ByteReader* reader, uint32_t width) {
  switch (width) {
    case 1:
      return reader->ReadU8();
    case 2:
      return reader->ReadU16();
    case 4:
      return reader->ReadU32();
    case 8:
      return reader->ReadU64();
    default:
      return InvalidCodec("invalid integer width");
  }
}

arrow::Result<uint64_t> ArrayIntegralBits(const arrow::Array& array, int64_t row) {
  switch (array.type_id()) {
    case arrow::Type::BOOL:
      return static_cast<const arrow::BooleanArray&>(array).Value(row) ? uint64_t{1} : uint64_t{0};
    case arrow::Type::INT8:
      return static_cast<uint64_t>(static_cast<const arrow::Int8Array&>(array).Value(row));
    case arrow::Type::INT16:
      return static_cast<uint64_t>(static_cast<const arrow::Int16Array&>(array).Value(row));
    case arrow::Type::INT32:
      return static_cast<uint64_t>(static_cast<const arrow::Int32Array&>(array).Value(row));
    case arrow::Type::INT64:
      return static_cast<uint64_t>(static_cast<const arrow::Int64Array&>(array).Value(row));
    case arrow::Type::UINT8:
      return static_cast<const arrow::UInt8Array&>(array).Value(row);
    case arrow::Type::UINT16:
      return static_cast<const arrow::UInt16Array&>(array).Value(row);
    case arrow::Type::UINT32:
      return static_cast<const arrow::UInt32Array&>(array).Value(row);
    case arrow::Type::UINT64:
      return static_cast<const arrow::UInt64Array&>(array).Value(row);
    case arrow::Type::TIMESTAMP:
      return static_cast<uint64_t>(static_cast<const arrow::TimestampArray&>(array).Value(row));
    default:
      return InvalidCodec("array value is not integral");
  }
}

template <typename Value>
int CompareValues(Value left, Value right) {
  return left < right ? -1 : (left > right ? 1 : 0);
}

arrow::Result<int> CompareIntegralBits(const arrow::DataType& type, uint64_t left, uint64_t right) {
  switch (type.id()) {
    case arrow::Type::INT8:
      return CompareValues(std::bit_cast<int8_t>(static_cast<uint8_t>(left)),
                           std::bit_cast<int8_t>(static_cast<uint8_t>(right)));
    case arrow::Type::INT16:
      return CompareValues(std::bit_cast<int16_t>(static_cast<uint16_t>(left)),
                           std::bit_cast<int16_t>(static_cast<uint16_t>(right)));
    case arrow::Type::INT32:
      return CompareValues(std::bit_cast<int32_t>(static_cast<uint32_t>(left)),
                           std::bit_cast<int32_t>(static_cast<uint32_t>(right)));
    case arrow::Type::INT64:
    case arrow::Type::TIMESTAMP:
      return CompareValues(std::bit_cast<int64_t>(left), std::bit_cast<int64_t>(right));
    case arrow::Type::UINT8:
      return CompareValues(static_cast<uint8_t>(left), static_cast<uint8_t>(right));
    case arrow::Type::UINT16:
      return CompareValues(static_cast<uint16_t>(left), static_cast<uint16_t>(right));
    case arrow::Type::UINT32:
      return CompareValues(static_cast<uint32_t>(left), static_cast<uint32_t>(right));
    case arrow::Type::UINT64:
      return CompareValues(left, right);
    default:
      return InvalidCodec("value is not integral or timestamp");
  }
}

arrow::Result<uint64_t> MaximumBits(const FieldSpec& field) {
  switch (field.type->id()) {
    case arrow::Type::INT8:
      return static_cast<uint64_t>(std::numeric_limits<int8_t>::max());
    case arrow::Type::INT16:
      return static_cast<uint64_t>(std::numeric_limits<int16_t>::max());
    case arrow::Type::INT32:
      return static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    case arrow::Type::INT64:
    case arrow::Type::TIMESTAMP:
      return static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    case arrow::Type::UINT8:
      return std::numeric_limits<uint8_t>::max();
    case arrow::Type::UINT16:
      return std::numeric_limits<uint16_t>::max();
    case arrow::Type::UINT32:
      return std::numeric_limits<uint32_t>::max();
    case arrow::Type::UINT64:
      return std::numeric_limits<uint64_t>::max();
    default:
      return InvalidCodec("FOR type has no maximum");
  }
}

arrow::Status ValidateRowsToDecode(uint64_t row_count, const std::vector<uint64_t>* selection) {
  if (row_count > std::numeric_limits<size_t>::max()) {
    return InvalidCodec("row count exceeds platform limit");
  }
  if (!selection) {
    return arrow::Status::OK();
  }
  uint64_t previous = 0;
  bool first = true;
  for (const uint64_t row : *selection) {
    if (row >= row_count || (!first && row <= previous)) {
      return InvalidCodec("selection must be strictly increasing and bounded");
    }
    first = false;
    previous = row;
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Array>> FinishScalars(
    const FieldSpec& field, const std::vector<std::shared_ptr<arrow::Scalar>>& values) {
  ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(field.type));
  if (values.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return InvalidCodec("decoded array exceeds Arrow limit");
  }
  ARROW_RETURN_NOT_OK(builder->Reserve(static_cast<int64_t>(values.size())));
  for (const auto& value : values) {
    if (value) {
      ARROW_RETURN_NOT_OK(builder->AppendScalar(*value));
    } else {
      ARROW_RETURN_NOT_OK(builder->AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> result;
  ARROW_RETURN_NOT_OK(builder->Finish(&result));
  ARROW_RETURN_NOT_OK(result->ValidateFull());
  return result;
}

arrow::Result<std::vector<uint8_t>> EncodeDictionary(const FieldSpec& field,
                                                     const arrow::Array& array) {
  const auto validity = EncodeValidity(array);
  std::vector<uint64_t> indices(static_cast<size_t>(array.length()), 0);
  ByteWriter dictionary_offsets;
  ByteWriter dictionary_values;
  uint64_t dictionary_count = 0;
  if (IsVariable(field.type->id())) {
    const auto& binary = static_cast<const arrow::BinaryArray&>(array);
    std::unordered_map<std::string_view, uint64_t> lookup;
    std::vector<std::string_view> dictionary;
    lookup.reserve(static_cast<size_t>(std::min<int64_t>(array.length(), 1024)));
    dictionary.reserve(static_cast<size_t>(std::min<int64_t>(array.length(), 1024)));
    for (int64_t row = 0; row < array.length(); ++row) {
      if (binary.IsNull(row)) {
        continue;
      }
      const std::string_view value = binary.GetView(row);
      const auto [entry, inserted] = lookup.emplace(value, dictionary.size());
      if (inserted) {
        dictionary.push_back(value);
      }
      indices[static_cast<size_t>(row)] = entry->second;
    }
    uint64_t offset = 0;
    dictionary_offsets.WriteU64(0);
    for (const auto& value : dictionary) {
      ARROW_ASSIGN_OR_RAISE(offset, CheckedAdd(offset, static_cast<uint64_t>(value.size())));
      dictionary_values.WriteBytes(
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
      dictionary_offsets.WriteU64(offset);
    }
    dictionary_count = static_cast<uint64_t>(dictionary.size());
  } else {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    const uint32_t width = FixedWidthBytes(physical_type);
    std::unordered_map<uint64_t, uint64_t> lookup;
    std::vector<uint64_t> dictionary;
    lookup.reserve(static_cast<size_t>(std::min<int64_t>(array.length(), 1024)));
    dictionary.reserve(static_cast<size_t>(std::min<int64_t>(array.length(), 1024)));
    for (int64_t row = 0; row < array.length(); ++row) {
      if (array.IsNull(row)) {
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(const uint64_t value, ArrayIntegralBits(array, row));
      const auto [entry, inserted] = lookup.emplace(value, dictionary.size());
      if (inserted) {
        dictionary.push_back(value);
      }
      indices[static_cast<size_t>(row)] = entry->second;
    }
    for (const auto& value : dictionary) {
      WriteWidth(&dictionary_values, value, width);
    }
    dictionary_count = static_cast<uint64_t>(dictionary.size());
  }
  const uint32_t index_width = IndexWidth(dictionary_count);
  ByteWriter encoded_indices;
  for (const uint64_t index : indices) {
    WriteWidth(&encoded_indices, index, index_width);
  }
  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU64(dictionary_count);
  payload.WriteU8(static_cast<uint8_t>(index_width));
  for (int reserved = 0; reserved < 7; ++reserved) {
    payload.WriteU8(0);
  }
  payload.WriteU64(static_cast<uint64_t>(dictionary_offsets.data().size()));
  payload.WriteU64(static_cast<uint64_t>(dictionary_values.data().size()));
  payload.WriteU64(static_cast<uint64_t>(encoded_indices.data().size()));
  payload.WriteBytes(validity);
  payload.WriteBytes(dictionary_offsets.data());
  payload.WriteBytes(dictionary_values.data());
  payload.WriteBytes(encoded_indices.data());
  return std::move(payload).Finish();
}

struct Run {
  uint64_t count = 0;
  bool valid = false;
  uint64_t value = 0;
};

arrow::Result<std::vector<uint8_t>> EncodeRle(const FieldSpec& field, const arrow::Array& array) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  std::vector<Run> runs;
  runs.reserve(static_cast<size_t>(std::min<int64_t>(array.length(), 1024)));
  for (int64_t row = 0; row < array.length(); ++row) {
    const bool valid = array.IsValid(row);
    uint64_t value = 0;
    if (valid) {
      ARROW_ASSIGN_OR_RAISE(value, ArrayIntegralBits(array, row));
    }
    if (!runs.empty() && runs.back().valid == valid && runs.back().value == value) {
      ++runs.back().count;
    } else {
      runs.push_back({1, valid, value});
    }
  }
  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(runs.size()));
  payload.WriteU64(0);
  for (const auto& run : runs) {
    payload.WriteU64(run.count);
    payload.WriteU8(run.valid ? 1U : 0U);
    for (int reserved = 0; reserved < 7; ++reserved) {
      payload.WriteU8(0);
    }
    WriteWidth(&payload, run.value, width);
  }
  return std::move(payload).Finish();
}

arrow::Result<std::vector<uint8_t>> EncodeForBitpack(const FieldSpec& field,
                                                     const arrow::Array& array) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  const auto validity = EncodeValidity(array);
  uint64_t base_bits = 0;
  bool have_base = false;
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(const uint64_t bits, ArrayIntegralBits(array, row));
    if (!have_base) {
      base_bits = bits;
      have_base = true;
    } else {
      ARROW_ASSIGN_OR_RAISE(const int order, CompareIntegralBits(*field.type, bits, base_bits));
      if (order < 0) {
        base_bits = bits;
      }
    }
  }
  std::vector<uint64_t> deltas(static_cast<size_t>(array.length()), 0);
  uint64_t maximum_delta = 0;
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsValid(row)) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t bits, ArrayIntegralBits(array, row));
      const uint64_t delta = bits - base_bits;
      deltas[static_cast<size_t>(row)] = delta;
      maximum_delta = std::max(maximum_delta, delta);
    }
  }
  const uint8_t bit_width = static_cast<uint8_t>(std::bit_width(maximum_delta));
  ARROW_ASSIGN_OR_RAISE(const uint64_t total_bits,
                        CheckedMultiply(static_cast<uint64_t>(array.length()), bit_width));
  ARROW_ASSIGN_OR_RAISE(const uint64_t padded_bits, CheckedAdd(total_bits, uint64_t{7}));
  const uint64_t packed_length = padded_bits / 8U;
  if (packed_length > std::numeric_limits<size_t>::max()) {
    return InvalidCodec("packed FOR payload exceeds platform limit");
  }
  std::vector<uint8_t> packed(static_cast<size_t>(packed_length), 0);
  for (uint64_t row = 0; row < deltas.size(); ++row) {
    const uint64_t delta = deltas[static_cast<size_t>(row)];
    for (uint32_t bit = 0; bit < bit_width; ++bit) {
      if (((delta >> bit) & 1U) != 0) {
        const uint64_t position = row * bit_width + bit;
        packed[static_cast<size_t>(position / 8U)] |=
            static_cast<uint8_t>(1U << static_cast<uint32_t>(position % 8U));
      }
    }
  }
  ByteWriter base_bytes;
  WriteWidth(&base_bytes, base_bits, width);
  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU8(bit_width);
  for (int reserved = 0; reserved < 7; ++reserved) {
    payload.WriteU8(0);
  }
  payload.WriteU64(static_cast<uint64_t>(base_bytes.data().size()));
  payload.WriteU64(static_cast<uint64_t>(packed.size()));
  payload.WriteBytes(base_bytes.data());
  payload.WriteBytes(validity);
  payload.WriteBytes(packed);
  return std::move(payload).Finish();
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodeDictionary(
    const FieldSpec& field, const ColumnChunkMeta& chunk, std::span<const uint8_t> payload,
    const std::vector<uint64_t>* selection) {
  ByteReader reader(payload);
  ARROW_ASSIGN_OR_RAISE(const uint64_t validity_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t dictionary_count, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint8_t index_width_byte, reader.ReadU8());
  for (int reserved = 0; reserved < 7; ++reserved) {
    ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader.ReadU8());
    if (value != 0) {
      return InvalidCodec("non-zero Dictionary reserved byte");
    }
  }
  const uint32_t index_width = index_width_byte;
  if (index_width != IndexWidth(dictionary_count)) {
    return InvalidCodec("non-canonical Dictionary index width");
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t offsets_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t values_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t indices_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(auto validity, reader.ReadBytes(validity_length));
  ARROW_ASSIGN_OR_RAISE(auto offset_bytes, reader.ReadBytes(offsets_length));
  ARROW_ASSIGN_OR_RAISE(auto value_bytes, reader.ReadBytes(values_length));
  ARROW_ASSIGN_OR_RAISE(auto index_bytes, reader.ReadBytes(indices_length));
  if (reader.remaining() != 0) {
    return InvalidCodec("trailing Dictionary bytes");
  }
  ARROW_RETURN_NOT_OK(ValidateValidity(validity, chunk.row_count, chunk.null_count));
  ARROW_ASSIGN_OR_RAISE(const uint64_t expected_indices,
                        CheckedMultiply(chunk.row_count, index_width));
  if (indices_length != expected_indices || dictionary_count > std::numeric_limits<size_t>::max()) {
    return InvalidCodec("invalid Dictionary index buffer");
  }

  std::vector<std::shared_ptr<arrow::Scalar>> dictionary;
  dictionary.reserve(static_cast<size_t>(dictionary_count));
  if (IsVariable(field.type->id())) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t offset_count, CheckedAdd(dictionary_count, uint64_t{1}));
    ARROW_ASSIGN_OR_RAISE(const uint64_t expected_offsets,
                          CheckedMultiply(offset_count, uint64_t{8}));
    if (offsets_length != expected_offsets) {
      return InvalidCodec("invalid Dictionary offsets");
    }
    ByteReader offsets(offset_bytes);
    std::vector<uint64_t> positions;
    positions.reserve(static_cast<size_t>(offset_count));
    for (uint64_t index = 0; index < offset_count; ++index) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t offset, offsets.ReadU64());
      if ((!positions.empty() && offset < positions.back()) || offset > value_bytes.size()) {
        return InvalidCodec("Dictionary offsets are not monotonic and bounded");
      }
      positions.push_back(offset);
    }
    if (positions.front() != 0 || positions.back() != value_bytes.size()) {
      return InvalidCodec("Dictionary offsets are not normalized");
    }
    for (uint64_t index = 0; index < dictionary_count; ++index) {
      const uint64_t begin = positions[static_cast<size_t>(index)];
      const uint64_t end = positions[static_cast<size_t>(index + 1U)];
      ARROW_ASSIGN_OR_RAISE(
          auto scalar, ParseScalar(field, value_bytes.subspan(static_cast<size_t>(begin),
                                                              static_cast<size_t>(end - begin))));
      dictionary.push_back(std::move(scalar));
    }
  } else {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    const uint32_t width = FixedWidthBytes(physical_type);
    ARROW_ASSIGN_OR_RAISE(const uint64_t expected_values, CheckedMultiply(dictionary_count, width));
    if (!offset_bytes.empty() || value_bytes.size() != expected_values) {
      return InvalidCodec("invalid fixed-width Dictionary values");
    }
    for (uint64_t index = 0; index < dictionary_count; ++index) {
      ARROW_ASSIGN_OR_RAISE(
          auto scalar,
          ParseScalar(field, value_bytes.subspan(static_cast<size_t>(index * width), width)));
      dictionary.push_back(std::move(scalar));
    }
  }

  ByteReader indices_reader(index_bytes);
  std::vector<uint64_t> indices;
  indices.reserve(static_cast<size_t>(chunk.row_count));
  for (uint64_t row = 0; row < chunk.row_count; ++row) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t index, ReadWidth(&indices_reader, index_width));
    if (IsValid(validity, row)) {
      if (index >= dictionary_count) {
        return InvalidCodec("Dictionary index out of bounds");
      }
    } else if (index != 0) {
      return InvalidCodec("null Dictionary row has non-zero index");
    }
    indices.push_back(index);
  }
  ARROW_RETURN_NOT_OK(ValidateRowsToDecode(chunk.row_count, selection));
  std::vector<std::shared_ptr<arrow::Scalar>> output;
  output.reserve(selection ? selection->size() : static_cast<size_t>(chunk.row_count));
  const auto append_rows = [&]<typename Rows>(const Rows& rows) {
    for (const uint64_t row : rows) {
      output.push_back(IsValid(validity, row) ? dictionary[static_cast<size_t>(indices[row])]
                                              : nullptr);
    }
  };
  if (selection) {
    append_rows(*selection);
  } else {
    append_rows(std::views::iota(uint64_t{0}, chunk.row_count));
  }
  return FinishScalars(field, output);
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodeRle(const FieldSpec& field,
                                                       const ColumnChunkMeta& chunk,
                                                       std::span<const uint8_t> payload,
                                                       const std::vector<uint64_t>* selection) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  ByteReader reader(payload);
  ARROW_ASSIGN_OR_RAISE(const uint64_t run_count, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t reserved, reader.ReadU64());
  if (reserved != 0 || run_count > reader.remaining() / (16U + width) ||
      run_count > std::numeric_limits<size_t>::max()) {
    return InvalidCodec("invalid RLE header");
  }
  struct DecodedRun {
    uint64_t end = 0;
    std::shared_ptr<arrow::Scalar> value;
  };
  std::vector<DecodedRun> runs;
  runs.reserve(static_cast<size_t>(run_count));
  uint64_t total_rows = 0;
  uint64_t null_rows = 0;
  for (uint64_t index = 0; index < run_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t count, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint8_t valid, reader.ReadU8());
    if (count == 0 || valid > 1) {
      return InvalidCodec("invalid RLE run");
    }
    for (int byte = 0; byte < 7; ++byte) {
      ARROW_ASSIGN_OR_RAISE(const uint8_t reserved_byte, reader.ReadU8());
      if (reserved_byte != 0) {
        return InvalidCodec("non-zero RLE reserved byte");
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto value_bytes, reader.ReadBytes(width));
    std::shared_ptr<arrow::Scalar> value;
    if (valid != 0) {
      ARROW_ASSIGN_OR_RAISE(value, ParseScalar(field, value_bytes));
    } else {
      if (std::any_of(value_bytes.begin(), value_bytes.end(),
                      [](uint8_t value) { return value != 0; })) {
        return InvalidCodec("null RLE run has non-zero value bytes");
      }
      ARROW_ASSIGN_OR_RAISE(null_rows, CheckedAdd(null_rows, count));
    }
    ARROW_ASSIGN_OR_RAISE(total_rows, CheckedAdd(total_rows, count));
    runs.push_back({total_rows, std::move(value)});
  }
  if (reader.remaining() != 0 || total_rows != chunk.row_count || null_rows != chunk.null_count) {
    return InvalidCodec("RLE runs do not match chunk counts");
  }
  ARROW_RETURN_NOT_OK(ValidateRowsToDecode(chunk.row_count, selection));
  std::vector<std::shared_ptr<arrow::Scalar>> output;
  output.reserve(selection ? selection->size() : static_cast<size_t>(chunk.row_count));
  if (selection) {
    size_t run = 0;
    for (const uint64_t row : *selection) {
      while (run < runs.size() && row >= runs[run].end) {
        ++run;
      }
      if (run == runs.size()) {
        return InvalidCodec("RLE row is outside runs");
      }
      output.push_back(runs[run].value);
    }
  } else {
    size_t run = 0;
    for (uint64_t row = 0; row < chunk.row_count; ++row) {
      while (run < runs.size() && row >= runs[run].end) {
        ++run;
      }
      if (run == runs.size()) {
        return InvalidCodec("RLE row is outside runs");
      }
      output.push_back(runs[run].value);
    }
  }
  return FinishScalars(field, output);
}

template <typename Rows, typename Builder, typename Convert>
arrow::Result<std::shared_ptr<arrow::Array>> DecodeForRows(const Rows& rows,
                                                           std::span<const uint8_t> validity,
                                                           std::span<const uint8_t> packed,
                                                           uint8_t bit_width, uint64_t base_bits,
                                                           uint64_t maximum_delta, Builder* builder,
                                                           Convert convert) {
  const size_t output_rows = static_cast<size_t>(std::ranges::size(rows));
  if (output_rows > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return InvalidCodec("decoded FOR array exceeds Arrow limit");
  }
  ARROW_RETURN_NOT_OK(builder->Reserve(static_cast<int64_t>(output_rows)));
  for (const uint64_t row : rows) {
    if (!IsValid(validity, row)) {
      ARROW_RETURN_NOT_OK(builder->AppendNull());
      continue;
    }
    uint64_t delta = 0;
    for (uint32_t bit = 0; bit < bit_width; ++bit) {
      const uint64_t position = row * bit_width + bit;
      if ((packed[static_cast<size_t>(position / 8U)] &
           static_cast<uint8_t>(1U << static_cast<uint32_t>(position % 8U))) != 0) {
        delta |= uint64_t{1} << bit;
      }
    }
    if (delta > maximum_delta) {
      return InvalidCodec("FOR delta exceeds physical type domain");
    }
    ARROW_RETURN_NOT_OK(builder->Append(convert(base_bits + delta)));
  }
  std::shared_ptr<arrow::Array> result;
  ARROW_RETURN_NOT_OK(builder->Finish(&result));
  ARROW_RETURN_NOT_OK(result->ValidateFull());
  return result;
}

template <typename Builder, typename Convert>
arrow::Result<std::shared_ptr<arrow::Array>> DecodeForValues(
    uint64_t row_count, const std::vector<uint64_t>* selection, std::span<const uint8_t> validity,
    std::span<const uint8_t> packed, uint8_t bit_width, uint64_t base_bits, uint64_t maximum_delta,
    Builder* builder, Convert convert) {
  if (selection) {
    return DecodeForRows(*selection, validity, packed, bit_width, base_bits, maximum_delta, builder,
                         convert);
  }
  const auto rows = std::views::iota(uint64_t{0}, row_count);
  return DecodeForRows(rows, validity, packed, bit_width, base_bits, maximum_delta, builder,
                       convert);
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodeForBitpack(
    const FieldSpec& field, const ColumnChunkMeta& chunk, std::span<const uint8_t> payload,
    const std::vector<uint64_t>* selection) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  ByteReader reader(payload);
  ARROW_ASSIGN_OR_RAISE(const uint64_t validity_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint8_t bit_width, reader.ReadU8());
  for (int reserved = 0; reserved < 7; ++reserved) {
    ARROW_ASSIGN_OR_RAISE(const uint8_t value, reader.ReadU8());
    if (value != 0) {
      return InvalidCodec("non-zero FOR reserved byte");
    }
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t base_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint64_t packed_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(auto base_bytes, reader.ReadBytes(base_length));
  ARROW_ASSIGN_OR_RAISE(auto validity, reader.ReadBytes(validity_length));
  ARROW_ASSIGN_OR_RAISE(auto packed, reader.ReadBytes(packed_length));
  if (reader.remaining() != 0 || base_length != width || bit_width > width * 8U) {
    return InvalidCodec("invalid FOR header");
  }
  ARROW_RETURN_NOT_OK(ValidateValidity(validity, chunk.row_count, chunk.null_count));
  ARROW_ASSIGN_OR_RAISE(const uint64_t total_bits, CheckedMultiply(chunk.row_count, bit_width));
  ARROW_ASSIGN_OR_RAISE(const uint64_t padded_bits, CheckedAdd(total_bits, uint64_t{7}));
  if (packed_length != padded_bits / 8U) {
    return InvalidCodec("invalid FOR packed length");
  }
  if (total_bits % 8U != 0 && !packed.empty()) {
    const uint8_t padding = static_cast<uint8_t>(0xFFU << static_cast<uint32_t>(total_bits % 8U));
    if ((packed.back() & padding) != 0) {
      return InvalidCodec("non-zero FOR padding bits");
    }
  }
  ByteReader base_reader(base_bytes);
  ARROW_ASSIGN_OR_RAISE(const uint64_t base_bits, ReadWidth(&base_reader, width));
  ARROW_ASSIGN_OR_RAISE(const uint64_t maximum_bits, MaximumBits(field));
  const uint64_t maximum_delta = maximum_bits - base_bits;
  ARROW_RETURN_NOT_OK(ValidateRowsToDecode(chunk.row_count, selection));
  switch (field.type->id()) {
    case arrow::Type::INT8: {
      arrow::Int8Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder, [](uint64_t bits) {
                               return std::bit_cast<int8_t>(static_cast<uint8_t>(bits));
                             });
    }
    case arrow::Type::INT16: {
      arrow::Int16Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder, [](uint64_t bits) {
                               return std::bit_cast<int16_t>(static_cast<uint16_t>(bits));
                             });
    }
    case arrow::Type::INT32: {
      arrow::Int32Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder, [](uint64_t bits) {
                               return std::bit_cast<int32_t>(static_cast<uint32_t>(bits));
                             });
    }
    case arrow::Type::INT64: {
      arrow::Int64Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder,
                             [](uint64_t bits) { return std::bit_cast<int64_t>(bits); });
    }
    case arrow::Type::UINT8: {
      arrow::UInt8Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder,
                             [](uint64_t bits) { return static_cast<uint8_t>(bits); });
    }
    case arrow::Type::UINT16: {
      arrow::UInt16Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder,
                             [](uint64_t bits) { return static_cast<uint16_t>(bits); });
    }
    case arrow::Type::UINT32: {
      arrow::UInt32Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder,
                             [](uint64_t bits) { return static_cast<uint32_t>(bits); });
    }
    case arrow::Type::UINT64: {
      arrow::UInt64Builder builder;
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder, [](uint64_t bits) { return bits; });
    }
    case arrow::Type::TIMESTAMP: {
      arrow::TimestampBuilder builder(std::static_pointer_cast<arrow::TimestampType>(field.type),
                                      arrow::default_memory_pool());
      return DecodeForValues(chunk.row_count, selection, validity, packed, bit_width, base_bits,
                             maximum_delta, &builder,
                             [](uint64_t bits) { return std::bit_cast<int64_t>(bits); });
    }
    default:
      return InvalidCodec("FOR type is not integral or timestamp");
  }
}

arrow::Result<uint64_t> PlainSampleSize(const FieldSpec& field, const arrow::Array& array,
                                        int64_t sample_rows) {
  if (sample_rows < 0 || sample_rows > array.length()) {
    return InvalidCodec("Plain size sample exceeds array bounds");
  }
  const uint64_t rows = static_cast<uint64_t>(sample_rows);
  uint64_t size = 24;
  if (array.Slice(0, sample_rows)->null_count() != 0) {
    ARROW_ASSIGN_OR_RAISE(size, CheckedAdd(size, rows / 8U + (rows % 8U != 0)));
  }
  if (IsVariable(field.type->id())) {
    ARROW_ASSIGN_OR_RAISE(const uint64_t offset_count, CheckedAdd(rows, uint64_t{1}));
    ARROW_ASSIGN_OR_RAISE(const uint64_t offset_bytes, CheckedMultiply(offset_count, uint64_t{8}));
    ARROW_ASSIGN_OR_RAISE(size, CheckedAdd(size, offset_bytes));
    const auto& binary = static_cast<const arrow::BinaryArray&>(array);
    for (int64_t row = 0; row < sample_rows; ++row) {
      if (binary.IsValid(row)) {
        ARROW_ASSIGN_OR_RAISE(size,
                              CheckedAdd(size, static_cast<uint64_t>(binary.value_length(row))));
      }
    }
  } else {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    ARROW_ASSIGN_OR_RAISE(
        const uint64_t value_bytes,
        CheckedMultiply(rows, static_cast<uint64_t>(FixedWidthBytes(physical_type))));
    ARROW_ASSIGN_OR_RAISE(size, CheckedAdd(size, value_bytes));
  }
  return size;
}

}  // namespace

bool EncodingSupports(uint16_t encoding_id, const arrow::DataType& type) {
  const bool integer = IsInteger(type.id());
  switch (encoding_id) {
    case kPlainEncodingId:
      return true;
    case kDictionaryEncodingId:
      return integer || IsVariable(type.id());
    case kRleEncodingId:
      return integer || type.id() == arrow::Type::BOOL;
    case kForBitpackEncodingId:
      return integer || type.id() == arrow::Type::TIMESTAMP;
    default:
      return false;
  }
}

arrow::Result<uint64_t> PlainEncodedSize(const FieldSpec& field, const arrow::Array& array) {
  return PlainSampleSize(field, array, array.length());
}

arrow::Result<uint16_t> SelectEncoding(const FieldSpec& field, const arrow::Array& array,
                                       const LayoutPolicy& layout) {
  const auto forced = std::find_if(
      layout.field_encodings.begin(), layout.field_encodings.end(),
      [&field](const FieldEncoding& candidate) { return candidate.field_id == field.field_id; });
  if (forced != layout.field_encodings.end()) {
    return static_cast<uint16_t>(forced->encoding);
  }
  const int64_t sample_rows = std::min<int64_t>(array.length(), layout.encoding_sample_rows);
  if (sample_rows == 0) {
    return kPlainEncodingId;
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t plain_size, PlainSampleSize(field, array, sample_rows));
  const uint64_t threshold = plain_size - plain_size / 10U;
  uint16_t best_id = kPlainEncodingId;
  uint64_t best_size = plain_size;
  const auto consider = [&](uint16_t id, uint64_t size) {
    if (size <= threshold &&
        (best_id == kPlainEncodingId || size < best_size || (size == best_size && id < best_id))) {
      best_id = id;
      best_size = size;
    }
  };

  const bool supports_dictionary = EncodingSupports(kDictionaryEncodingId, *field.type);
  const bool supports_rle = EncodingSupports(kRleEncodingId, *field.type);
  const bool supports_for = EncodingSupports(kForBitpackEncodingId, *field.type);
  if (!supports_dictionary && !supports_rle && !supports_for) {
    return best_id;
  }

  uint64_t dictionary_values_size = 0;
  uint64_t dictionary_count = 0;
  uint64_t runs = 0;
  bool previous_valid = false;
  bool first = true;
  uint64_t minimum_bits = 0;
  uint64_t maximum_bits = 0;
  bool have_extrema = false;

  if (IsVariable(field.type->id())) {
    std::unordered_set<std::string_view> distinct;
    std::string_view previous;
    const auto& binary = static_cast<const arrow::BinaryArray&>(array);
    for (int64_t row = 0; row < sample_rows; ++row) {
      const bool valid = binary.IsValid(row);
      const std::string_view value = valid ? binary.GetView(row) : std::string_view{};
      if (valid && distinct.insert(value).second) {
        dictionary_values_size += static_cast<uint64_t>(value.size());
      }
      if (first || valid != previous_valid || (valid && value != previous)) {
        ++runs;
      }
      first = false;
      previous_valid = valid;
      previous = value;
    }
    dictionary_count = static_cast<uint64_t>(distinct.size());
  } else {
    std::unordered_set<uint64_t> distinct;
    uint64_t previous = 0;
    for (int64_t row = 0; row < sample_rows; ++row) {
      const bool valid = array.IsValid(row);
      uint64_t value = 0;
      if (valid) {
        ARROW_ASSIGN_OR_RAISE(value, ArrayIntegralBits(array, row));
        if (supports_dictionary && distinct.insert(value).second) {
          ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
          dictionary_values_size += FixedWidthBytes(physical_type);
        }
        if (supports_for) {
          if (!have_extrema) {
            minimum_bits = value;
            maximum_bits = value;
            have_extrema = true;
          } else {
            ARROW_ASSIGN_OR_RAISE(const int min_order,
                                  CompareIntegralBits(*field.type, value, minimum_bits));
            ARROW_ASSIGN_OR_RAISE(const int max_order,
                                  CompareIntegralBits(*field.type, value, maximum_bits));
            if (min_order < 0) {
              minimum_bits = value;
            }
            if (max_order > 0) {
              maximum_bits = value;
            }
          }
        }
      }
      if (first || valid != previous_valid || (valid && value != previous)) {
        ++runs;
      }
      first = false;
      previous_valid = valid;
      previous = value;
    }
    dictionary_count = static_cast<uint64_t>(distinct.size());
  }
  const uint64_t validity_size = array.Slice(0, sample_rows)->null_count() == 0
                                     ? 0
                                     : static_cast<uint64_t>(sample_rows) / 8U +
                                           (static_cast<uint64_t>(sample_rows) % 8U != 0);
  if (supports_dictionary) {
    uint64_t dictionary_size = 48U + validity_size + dictionary_values_size +
                               static_cast<uint64_t>(sample_rows) * IndexWidth(dictionary_count);
    if (IsVariable(field.type->id())) {
      dictionary_size += (dictionary_count + 1U) * 8U;
    }
    consider(kDictionaryEncodingId, dictionary_size);
  }
  if (supports_rle) {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    consider(kRleEncodingId, 16U + runs * (16U + FixedWidthBytes(physical_type)));
  }
  if (supports_for) {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    uint8_t bit_width = 0;
    if (have_extrema) {
      const uint64_t delta = maximum_bits - minimum_bits;
      bit_width = static_cast<uint8_t>(std::bit_width(delta));
    }
    const uint64_t packed = (static_cast<uint64_t>(sample_rows) * bit_width + 7U) / 8U;
    consider(kForBitpackEncodingId, 32U + FixedWidthBytes(physical_type) + validity_size + packed);
  }
  return best_id;
}

arrow::Result<std::vector<uint8_t>> EncodeNonPlain(uint16_t encoding_id, const FieldSpec& field,
                                                   const arrow::Array& array) {
  if (!EncodingSupports(encoding_id, *field.type)) {
    return arrow::Status::NotImplemented("[sniffer.codec.encoding] unsupported type/encoding");
  }
  switch (encoding_id) {
    case kDictionaryEncodingId:
      return EncodeDictionary(field, array);
    case kRleEncodingId:
      return EncodeRle(field, array);
    case kForBitpackEncodingId:
      return EncodeForBitpack(field, array);
    default:
      return arrow::Status::NotImplemented("[sniffer.codec.encoding] unknown non-Plain encoding");
  }
}

arrow::Result<std::shared_ptr<arrow::Array>> DecodeNonPlain(
    const FieldSpec& field, const ColumnChunkMeta& chunk, std::span<const uint8_t> payload,
    const std::vector<uint64_t>* selection) {
  if (!EncodingSupports(chunk.encoding_id, *field.type)) {
    return arrow::Status::NotImplemented("[sniffer.codec.encoding] unsupported type/encoding");
  }
  switch (chunk.encoding_id) {
    case kDictionaryEncodingId:
      return DecodeDictionary(field, chunk, payload, selection);
    case kRleEncodingId:
      return DecodeRle(field, chunk, payload, selection);
    case kForBitpackEncodingId:
      return DecodeForBitpack(field, chunk, payload, selection);
    default:
      return arrow::Status::NotImplemented("[sniffer.codec.encoding] unknown non-Plain encoding");
  }
}

}  // namespace sniffer::internal
