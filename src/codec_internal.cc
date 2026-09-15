#include "codec_internal.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
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

arrow::Result<uint64_t> IntegralBits(const arrow::Scalar& scalar) {
  switch (scalar.type->id()) {
    case arrow::Type::INT8:
      return static_cast<uint64_t>(static_cast<const arrow::Int8Scalar&>(scalar).value);
    case arrow::Type::INT16:
      return static_cast<uint64_t>(static_cast<const arrow::Int16Scalar&>(scalar).value);
    case arrow::Type::INT32:
      return static_cast<uint64_t>(static_cast<const arrow::Int32Scalar&>(scalar).value);
    case arrow::Type::INT64:
      return static_cast<uint64_t>(static_cast<const arrow::Int64Scalar&>(scalar).value);
    case arrow::Type::UINT8:
      return static_cast<const arrow::UInt8Scalar&>(scalar).value;
    case arrow::Type::UINT16:
      return static_cast<const arrow::UInt16Scalar&>(scalar).value;
    case arrow::Type::UINT32:
      return static_cast<const arrow::UInt32Scalar&>(scalar).value;
    case arrow::Type::UINT64:
      return static_cast<const arrow::UInt64Scalar&>(scalar).value;
    case arrow::Type::TIMESTAMP:
      return static_cast<uint64_t>(static_cast<const arrow::TimestampScalar&>(scalar).value);
    default:
      return InvalidCodec("FOR value is not integral");
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

arrow::Result<std::shared_ptr<arrow::Scalar>> ScalarFromBits(const FieldSpec& field,
                                                             uint64_t bits) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  ByteWriter writer;
  WriteWidth(&writer, bits, width);
  return ParseScalar(field, writer.data());
}

arrow::Result<std::vector<uint64_t>> RowsToDecode(uint64_t row_count,
                                                  const std::vector<uint64_t>* selection) {
  if (row_count > std::numeric_limits<size_t>::max()) {
    return InvalidCodec("row count exceeds platform limit");
  }
  if (!selection) {
    std::vector<uint64_t> rows(static_cast<size_t>(row_count));
    for (uint64_t row = 0; row < row_count; ++row) {
      rows[static_cast<size_t>(row)] = row;
    }
    return rows;
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
  return *selection;
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
  std::unordered_map<std::string, uint64_t> lookup;
  std::vector<std::vector<uint8_t>> dictionary;
  std::vector<uint64_t> indices(static_cast<size_t>(array.length()), 0);
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(auto scalar, array.GetScalar(row));
    ARROW_ASSIGN_OR_RAISE(auto bytes, SerializeScalar(field, *scalar));
    const std::string key(bytes.begin(), bytes.end());
    const auto [entry, inserted] = lookup.emplace(key, dictionary.size());
    if (inserted) {
      dictionary.push_back(std::move(bytes));
    }
    indices[static_cast<size_t>(row)] = entry->second;
  }

  const uint32_t index_width = IndexWidth(dictionary.size());
  ByteWriter dictionary_offsets;
  ByteWriter dictionary_values;
  if (IsVariable(field.type->id())) {
    uint64_t offset = 0;
    dictionary_offsets.WriteU64(0);
    for (const auto& value : dictionary) {
      ARROW_ASSIGN_OR_RAISE(offset, CheckedAdd(offset, static_cast<uint64_t>(value.size())));
      dictionary_values.WriteBytes(value);
      dictionary_offsets.WriteU64(offset);
    }
  } else {
    for (const auto& value : dictionary) {
      dictionary_values.WriteBytes(value);
    }
  }
  ByteWriter encoded_indices;
  for (const uint64_t index : indices) {
    WriteWidth(&encoded_indices, index, index_width);
  }
  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU64(static_cast<uint64_t>(dictionary.size()));
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
  std::vector<uint8_t> value;
};

arrow::Result<std::vector<uint8_t>> EncodeRle(const FieldSpec& field, const arrow::Array& array) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  std::vector<Run> runs;
  for (int64_t row = 0; row < array.length(); ++row) {
    const bool valid = array.IsValid(row);
    std::vector<uint8_t> value(width, 0);
    if (valid) {
      ARROW_ASSIGN_OR_RAISE(auto scalar, array.GetScalar(row));
      ARROW_ASSIGN_OR_RAISE(value, SerializeScalar(field, *scalar));
    }
    if (!runs.empty() && runs.back().valid == valid && runs.back().value == value) {
      ++runs.back().count;
    } else {
      runs.push_back({1, valid, std::move(value)});
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
    payload.WriteBytes(run.value);
  }
  return std::move(payload).Finish();
}

arrow::Result<std::vector<uint8_t>> EncodeForBitpack(const FieldSpec& field,
                                                     const arrow::Array& array) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
  const uint32_t width = FixedWidthBytes(physical_type);
  const auto validity = EncodeValidity(array);
  std::shared_ptr<arrow::Scalar> base;
  std::vector<std::shared_ptr<arrow::Scalar>> values(static_cast<size_t>(array.length()));
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(auto value, array.GetScalar(row));
    values[static_cast<size_t>(row)] = value;
    if (!base) {
      base = std::move(value);
    } else {
      ARROW_ASSIGN_OR_RAISE(const int order, CompareScalars(*value, *base));
      if (order < 0) {
        base = std::move(value);
      }
    }
  }
  if (!base) {
    ARROW_ASSIGN_OR_RAISE(base, ScalarFromBits(field, 0));
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t base_bits, IntegralBits(*base));
  std::vector<uint64_t> deltas(values.size(), 0);
  uint64_t maximum_delta = 0;
  for (size_t row = 0; row < values.size(); ++row) {
    if (values[row]) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t bits, IntegralBits(*values[row]));
      deltas[row] = bits - base_bits;
      maximum_delta = std::max(maximum_delta, deltas[row]);
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
  ARROW_ASSIGN_OR_RAISE(auto base_bytes, SerializeScalar(field, *base));
  if (base_bytes.size() != width) {
    return InvalidCodec("FOR base width mismatch");
  }
  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU8(bit_width);
  for (int reserved = 0; reserved < 7; ++reserved) {
    payload.WriteU8(0);
  }
  payload.WriteU64(static_cast<uint64_t>(base_bytes.size()));
  payload.WriteU64(static_cast<uint64_t>(packed.size()));
  payload.WriteBytes(base_bytes);
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
  ARROW_ASSIGN_OR_RAISE(auto rows, RowsToDecode(chunk.row_count, selection));
  std::vector<std::shared_ptr<arrow::Scalar>> output;
  output.reserve(rows.size());
  for (const uint64_t row : rows) {
    output.push_back(IsValid(validity, row) ? dictionary[static_cast<size_t>(indices[row])]
                                            : nullptr);
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
  ARROW_ASSIGN_OR_RAISE(auto rows, RowsToDecode(chunk.row_count, selection));
  std::vector<std::shared_ptr<arrow::Scalar>> output;
  output.reserve(rows.size());
  size_t run = 0;
  for (const uint64_t row : rows) {
    while (run < runs.size() && row >= runs[run].end) {
      ++run;
    }
    if (run == runs.size()) {
      return InvalidCodec("RLE row is outside runs");
    }
    output.push_back(runs[run].value);
  }
  return FinishScalars(field, output);
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
  ARROW_ASSIGN_OR_RAISE(auto base, ParseScalar(field, base_bytes));
  ARROW_ASSIGN_OR_RAISE(const uint64_t base_bits, IntegralBits(*base));
  ARROW_ASSIGN_OR_RAISE(const uint64_t maximum_bits, MaximumBits(field));
  const uint64_t maximum_delta = maximum_bits - base_bits;
  ARROW_ASSIGN_OR_RAISE(auto rows, RowsToDecode(chunk.row_count, selection));
  std::vector<std::shared_ptr<arrow::Scalar>> output;
  output.reserve(rows.size());
  for (const uint64_t row : rows) {
    if (!IsValid(validity, row)) {
      output.push_back(nullptr);
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
    ARROW_ASSIGN_OR_RAISE(auto value, ScalarFromBits(field, base_bits + delta));
    output.push_back(std::move(value));
  }
  return FinishScalars(field, output);
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

  std::unordered_set<std::string> distinct;
  uint64_t dictionary_values_size = 0;
  uint64_t runs = 0;
  std::string previous;
  bool previous_valid = false;
  bool first = true;
  std::shared_ptr<arrow::Scalar> minimum;
  std::shared_ptr<arrow::Scalar> maximum;
  for (int64_t row = 0; row < sample_rows; ++row) {
    const bool valid = array.IsValid(row);
    std::string key;
    if (valid) {
      ARROW_ASSIGN_OR_RAISE(auto scalar, array.GetScalar(row));
      ARROW_ASSIGN_OR_RAISE(auto bytes, SerializeScalar(field, *scalar));
      key.assign(bytes.begin(), bytes.end());
      const auto [entry, inserted] = distinct.insert(key);
      if (inserted) {
        dictionary_values_size += static_cast<uint64_t>(entry->size());
      }
      if (EncodingSupports(kForBitpackEncodingId, *field.type)) {
        if (!minimum) {
          minimum = scalar;
          maximum = std::move(scalar);
        } else {
          ARROW_ASSIGN_OR_RAISE(const int min_order, CompareScalars(*scalar, *minimum));
          ARROW_ASSIGN_OR_RAISE(const int max_order, CompareScalars(*scalar, *maximum));
          if (min_order < 0) {
            minimum = scalar;
          }
          if (max_order > 0) {
            maximum = std::move(scalar);
          }
        }
      }
    }
    if (first || valid != previous_valid || (valid && key != previous)) {
      ++runs;
    }
    first = false;
    previous_valid = valid;
    previous = std::move(key);
  }
  const uint64_t validity_size = array.Slice(0, sample_rows)->null_count() == 0
                                     ? 0
                                     : static_cast<uint64_t>(sample_rows) / 8U +
                                           (static_cast<uint64_t>(sample_rows) % 8U != 0);
  if (EncodingSupports(kDictionaryEncodingId, *field.type)) {
    const uint64_t dictionary_count = distinct.size();
    uint64_t dictionary_size = 48U + validity_size + dictionary_values_size +
                               static_cast<uint64_t>(sample_rows) * IndexWidth(dictionary_count);
    if (IsVariable(field.type->id())) {
      dictionary_size += (dictionary_count + 1U) * 8U;
    }
    consider(kDictionaryEncodingId, dictionary_size);
  }
  if (EncodingSupports(kRleEncodingId, *field.type)) {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    consider(kRleEncodingId, 16U + runs * (16U + FixedWidthBytes(physical_type)));
  }
  if (EncodingSupports(kForBitpackEncodingId, *field.type)) {
    ARROW_ASSIGN_OR_RAISE(const auto physical_type, PhysicalTypeFor(*field.type));
    uint8_t bit_width = 0;
    if (minimum && maximum) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t min_bits, IntegralBits(*minimum));
      ARROW_ASSIGN_OR_RAISE(const uint64_t max_bits, IntegralBits(*maximum));
      const uint64_t delta = max_bits - min_bits;
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
