#include "format_internal.h"

#include <arrow/util/key_value_metadata.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace sniffer::internal {
namespace {

arrow::Status Truncated(std::string_view what) {
  return arrow::Status::Invalid("[sniffer.format.truncated] ", what);
}

arrow::Status InvalidFormat(std::string_view what) {
  return arrow::Status::Invalid("[sniffer.format.invalid] ", what);
}

arrow::Result<std::vector<uint8_t>> SerializeTypeParameters(const arrow::DataType& type) {
  ByteWriter writer;
  if (type.id() != arrow::Type::TIMESTAMP) {
    return std::move(writer).Finish();
  }

  const auto& timestamp = static_cast<const arrow::TimestampType&>(type);
  writer.WriteU8(static_cast<uint8_t>(timestamp.unit()));
  writer.WriteU8(0);
  writer.WriteU8(0);
  writer.WriteU8(0);
  if (timestamp.timezone().size() > std::numeric_limits<uint32_t>::max()) {
    return arrow::Status::Invalid("[sniffer.schema.limit] timestamp timezone is too long");
  }
  writer.WriteU32(static_cast<uint32_t>(timestamp.timezone().size()));
  writer.WriteString(timestamp.timezone());
  return std::move(writer).Finish();
}

arrow::Result<arrow::TimeUnit::type> ParseTimeUnit(uint8_t value) {
  switch (value) {
    case arrow::TimeUnit::SECOND:
      return arrow::TimeUnit::SECOND;
    case arrow::TimeUnit::MILLI:
      return arrow::TimeUnit::MILLI;
    case arrow::TimeUnit::MICRO:
      return arrow::TimeUnit::MICRO;
    case arrow::TimeUnit::NANO:
      return arrow::TimeUnit::NANO;
    default:
      return InvalidFormat("unknown timestamp unit");
  }
}

}  // namespace

void ByteWriter::WriteU8(uint8_t value) { data_.push_back(value); }

void ByteWriter::WriteU16(uint16_t value) {
  data_.push_back(static_cast<uint8_t>(value));
  data_.push_back(static_cast<uint8_t>(value >> 8U));
}

void ByteWriter::WriteU32(uint32_t value) {
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    data_.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void ByteWriter::WriteU64(uint64_t value) {
  for (uint32_t shift = 0; shift < 64; shift += 8) {
    data_.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void ByteWriter::WriteBytes(std::span<const uint8_t> bytes) {
  data_.insert(data_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::WriteString(const std::string& value) {
  WriteBytes(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
}

uint64_t ByteReader::remaining() const { return static_cast<uint64_t>(data_.size()) - position_; }

arrow::Result<uint8_t> ByteReader::ReadU8() {
  if (remaining() < 1) {
    return Truncated("reading uint8");
  }
  return data_[static_cast<size_t>(position_++)];
}

arrow::Result<uint16_t> ByteReader::ReadU16() {
  ARROW_ASSIGN_OR_RAISE(auto bytes, ReadBytes(2));
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

arrow::Result<uint32_t> ByteReader::ReadU32() {
  ARROW_ASSIGN_OR_RAISE(auto bytes, ReadBytes(4));
  uint32_t value = 0;
  for (uint32_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

arrow::Result<uint64_t> ByteReader::ReadU64() {
  ARROW_ASSIGN_OR_RAISE(auto bytes, ReadBytes(8));
  uint64_t value = 0;
  for (uint32_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

arrow::Result<std::span<const uint8_t>> ByteReader::ReadBytes(uint64_t length) {
  if (length > remaining()) {
    return Truncated("reading byte range");
  }
  const size_t begin = static_cast<size_t>(position_);
  const size_t count = static_cast<size_t>(length);
  position_ += length;
  return data_.subspan(begin, count);
}

arrow::Result<std::string> ByteReader::ReadString(uint64_t length) {
  ARROW_ASSIGN_OR_RAISE(auto bytes, ReadBytes(length));
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

arrow::Result<uint64_t> CheckedAdd(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return arrow::Status::Invalid("[sniffer.format.overflow] addition");
  }
  return left + right;
}

arrow::Result<uint64_t> CheckedMultiply(uint64_t left, uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    return arrow::Status::Invalid("[sniffer.format.overflow] multiplication");
  }
  return left * right;
}

uint32_t Crc32c(std::span<const uint8_t> bytes, uint32_t previous) {
  uint32_t crc = ~previous;
  for (const uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82F63B78U & mask);
    }
  }
  return ~crc;
}

std::vector<uint8_t> SerializeHeader() {
  ByteWriter writer;
  writer.WriteBytes(kHeaderMagic);
  writer.WriteU16(kFormatMajor);
  writer.WriteU16(kFormatMinor);
  writer.WriteU32(kHeaderSize);
  writer.WriteU32(0);
  writer.WriteU64(0);
  const uint32_t checksum = Crc32c(writer.data());
  writer.WriteU32(checksum);
  return std::move(writer).Finish();
}

arrow::Status ValidateHeader(std::span<const uint8_t> bytes) {
  if (bytes.size() != kHeaderSize) {
    return Truncated("header");
  }
  if (!std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin())) {
    return InvalidFormat("bad header magic");
  }

  ByteReader reader(bytes.subspan(8));
  ARROW_ASSIGN_OR_RAISE(const uint16_t major, reader.ReadU16());
  ARROW_ASSIGN_OR_RAISE(const uint16_t minor, reader.ReadU16());
  if (major != kFormatMajor || minor > kFormatMinor) {
    return arrow::Status::NotImplemented("[sniffer.format.version] unsupported format version ",
                                         major, ".", minor);
  }
  ARROW_ASSIGN_OR_RAISE(const uint32_t header_size, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t flags, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint64_t reserved, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(const uint32_t stored_checksum, reader.ReadU32());
  if (header_size != kHeaderSize || flags != 0 || reserved != 0) {
    return InvalidFormat("unsupported header fields");
  }
  if (Crc32c(bytes.first(kHeaderSize - 4)) != stored_checksum) {
    return arrow::Status::Invalid("[sniffer.format.checksum] header CRC32C mismatch");
  }
  return arrow::Status::OK();
}

std::vector<uint8_t> SerializeTrailer(const FooterTrailer& trailer) {
  ByteWriter writer;
  writer.WriteBytes(kTrailerMagic);
  writer.WriteU64(trailer.footer_offset);
  writer.WriteU64(trailer.footer_length);
  writer.WriteU32(trailer.footer_checksum);
  writer.WriteU32(trailer.file_checksum);
  const uint32_t trailer_checksum = Crc32c(writer.data());
  writer.WriteU32(trailer_checksum);
  writer.WriteU32(0);
  return std::move(writer).Finish();
}

arrow::Result<FooterTrailer> ParseTrailer(std::span<const uint8_t> bytes) {
  if (bytes.size() != kTrailerSize) {
    return Truncated("footer trailer");
  }
  if (!std::equal(kTrailerMagic.begin(), kTrailerMagic.end(), bytes.begin())) {
    return InvalidFormat("bad footer magic");
  }

  ByteReader reader(bytes.subspan(8));
  FooterTrailer trailer;
  ARROW_ASSIGN_OR_RAISE(trailer.footer_offset, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(trailer.footer_length, reader.ReadU64());
  ARROW_ASSIGN_OR_RAISE(trailer.footer_checksum, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(trailer.file_checksum, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t stored_trailer_checksum, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t reserved, reader.ReadU32());
  if (reserved != 0) {
    return InvalidFormat("non-zero footer trailer reserved field");
  }
  if (Crc32c(bytes.first(32)) != stored_trailer_checksum) {
    return arrow::Status::Invalid("[sniffer.format.checksum] footer trailer CRC32C mismatch");
  }
  return trailer;
}

arrow::Result<PhysicalTypeId> PhysicalTypeFor(const arrow::DataType& type) {
  switch (type.id()) {
    case arrow::Type::BOOL:
      return PhysicalTypeId::kBool;
    case arrow::Type::INT8:
      return PhysicalTypeId::kInt8;
    case arrow::Type::INT16:
      return PhysicalTypeId::kInt16;
    case arrow::Type::INT32:
      return PhysicalTypeId::kInt32;
    case arrow::Type::INT64:
      return PhysicalTypeId::kInt64;
    case arrow::Type::UINT8:
      return PhysicalTypeId::kUInt8;
    case arrow::Type::UINT16:
      return PhysicalTypeId::kUInt16;
    case arrow::Type::UINT32:
      return PhysicalTypeId::kUInt32;
    case arrow::Type::UINT64:
      return PhysicalTypeId::kUInt64;
    case arrow::Type::FLOAT:
      return PhysicalTypeId::kFloat32;
    case arrow::Type::DOUBLE:
      return PhysicalTypeId::kFloat64;
    case arrow::Type::TIMESTAMP:
      return PhysicalTypeId::kTimestamp;
    case arrow::Type::STRING:
      return PhysicalTypeId::kString;
    case arrow::Type::BINARY:
      return PhysicalTypeId::kBinary;
    default:
      return arrow::Status::NotImplemented("[sniffer.schema.type] unsupported Arrow type: ",
                                           type.ToString());
  }
}

arrow::Result<std::shared_ptr<arrow::DataType>> ArrowTypeFor(PhysicalTypeId type_id,
                                                             std::span<const uint8_t> parameters) {
  if (type_id != PhysicalTypeId::kTimestamp && !parameters.empty()) {
    return InvalidFormat("unexpected type parameters");
  }
  switch (type_id) {
    case PhysicalTypeId::kBool:
      return arrow::boolean();
    case PhysicalTypeId::kInt8:
      return arrow::int8();
    case PhysicalTypeId::kInt16:
      return arrow::int16();
    case PhysicalTypeId::kInt32:
      return arrow::int32();
    case PhysicalTypeId::kInt64:
      return arrow::int64();
    case PhysicalTypeId::kUInt8:
      return arrow::uint8();
    case PhysicalTypeId::kUInt16:
      return arrow::uint16();
    case PhysicalTypeId::kUInt32:
      return arrow::uint32();
    case PhysicalTypeId::kUInt64:
      return arrow::uint64();
    case PhysicalTypeId::kFloat32:
      return arrow::float32();
    case PhysicalTypeId::kFloat64:
      return arrow::float64();
    case PhysicalTypeId::kString:
      return arrow::utf8();
    case PhysicalTypeId::kBinary:
      return arrow::binary();
    case PhysicalTypeId::kTimestamp: {
      ByteReader reader(parameters);
      ARROW_ASSIGN_OR_RAISE(const uint8_t unit_value, reader.ReadU8());
      ARROW_ASSIGN_OR_RAISE(const uint8_t reserved0, reader.ReadU8());
      ARROW_ASSIGN_OR_RAISE(const uint8_t reserved1, reader.ReadU8());
      ARROW_ASSIGN_OR_RAISE(const uint8_t reserved2, reader.ReadU8());
      if (reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
        return InvalidFormat("non-zero timestamp reserved field");
      }
      ARROW_ASSIGN_OR_RAISE(const uint32_t timezone_length, reader.ReadU32());
      ARROW_ASSIGN_OR_RAISE(auto timezone, reader.ReadString(timezone_length));
      if (reader.remaining() != 0) {
        return InvalidFormat("trailing timestamp parameters");
      }
      ARROW_ASSIGN_OR_RAISE(const auto unit, ParseTimeUnit(unit_value));
      return arrow::timestamp(unit, std::move(timezone));
    }
  }
  return arrow::Status::NotImplemented("[sniffer.schema.type] unknown physical type ID ",
                                       static_cast<uint16_t>(type_id));
}

uint32_t FixedWidthBytes(PhysicalTypeId type_id) {
  switch (type_id) {
    case PhysicalTypeId::kBool:
    case PhysicalTypeId::kInt8:
    case PhysicalTypeId::kUInt8:
      return 1;
    case PhysicalTypeId::kInt16:
    case PhysicalTypeId::kUInt16:
      return 2;
    case PhysicalTypeId::kInt32:
    case PhysicalTypeId::kUInt32:
    case PhysicalTypeId::kFloat32:
      return 4;
    case PhysicalTypeId::kInt64:
    case PhysicalTypeId::kUInt64:
    case PhysicalTypeId::kFloat64:
    case PhysicalTypeId::kTimestamp:
      return 8;
    case PhysicalTypeId::kString:
    case PhysicalTypeId::kBinary:
      return 0;
  }
  return 0;
}

arrow::Result<std::vector<uint8_t>> SerializeFooter(const FooterData& footer) {
  ARROW_RETURN_NOT_OK(footer.schema.Validate());
  if (footer.schema.fields.size() > std::numeric_limits<uint32_t>::max()) {
    return arrow::Status::Invalid("[sniffer.schema.limit] too many fields");
  }

  ByteWriter writer;
  writer.WriteU16(kFooterPayloadVersion);
  writer.WriteU16(0);
  writer.WriteU32(footer.schema.schema_version);
  writer.WriteU32(static_cast<uint32_t>(footer.schema.fields.size()));
  writer.WriteU32(1);
  writer.WriteU64(static_cast<uint64_t>(footer.row_groups.size()));

  for (const auto& field : footer.schema.fields) {
    if (field.name.size() > std::numeric_limits<uint32_t>::max()) {
      return arrow::Status::Invalid("[sniffer.schema.limit] field name is too long");
    }
    ARROW_ASSIGN_OR_RAISE(const auto type_id, PhysicalTypeFor(*field.type));
    ARROW_ASSIGN_OR_RAISE(auto parameters, SerializeTypeParameters(*field.type));
    if (parameters.size() > std::numeric_limits<uint32_t>::max()) {
      return arrow::Status::Invalid("[sniffer.schema.limit] type parameters are too long");
    }
    writer.WriteU32(field.field_id);
    writer.WriteU16(static_cast<uint16_t>(type_id));
    writer.WriteU16(field.nullable ? 1U : 0U);
    writer.WriteU32(static_cast<uint32_t>(field.name.size()));
    writer.WriteU32(static_cast<uint32_t>(parameters.size()));
    writer.WriteString(field.name);
    writer.WriteBytes(parameters);
  }

  writer.WriteU16(kPlainEncodingId);
  writer.WriteU16(kPlainEncodingMajor);
  writer.WriteU16(kPlainEncodingMinor);
  writer.WriteU16(5);
  writer.WriteString("plain");

  for (const auto& row_group : footer.row_groups) {
    if (row_group.chunks.size() > std::numeric_limits<uint32_t>::max()) {
      return arrow::Status::Invalid("[sniffer.format.limit] too many chunks");
    }
    writer.WriteU64(row_group.row_count);
    writer.WriteU32(static_cast<uint32_t>(row_group.chunks.size()));
    writer.WriteU32(0);
    for (const auto& chunk : row_group.chunks) {
      writer.WriteU32(chunk.field_id);
      writer.WriteU16(static_cast<uint16_t>(chunk.physical_type));
      writer.WriteU16(chunk.encoding_id);
      writer.WriteU64(chunk.row_count);
      writer.WriteU64(chunk.null_count);
      writer.WriteU64(chunk.offset);
      writer.WriteU64(chunk.length);
      writer.WriteU64(chunk.uncompressed_length);
      writer.WriteU32(chunk.checksum);
      writer.WriteU32(0);
    }
  }
  return std::move(writer).Finish();
}

arrow::Result<FooterData> ParseFooter(std::span<const uint8_t> bytes) {
  ByteReader reader(bytes);
  ARROW_ASSIGN_OR_RAISE(const uint16_t payload_version, reader.ReadU16());
  ARROW_ASSIGN_OR_RAISE(const uint16_t prefix_reserved, reader.ReadU16());
  if (payload_version != kFooterPayloadVersion || prefix_reserved != 0) {
    return arrow::Status::NotImplemented(
        "[sniffer.format.footer_version] unsupported footer payload");
  }

  FooterData footer;
  ARROW_ASSIGN_OR_RAISE(footer.schema.schema_version, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t field_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t encoding_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint64_t row_group_count, reader.ReadU64());
  if (field_count > reader.remaining() / 16U) {
    return Truncated("field descriptors");
  }

  footer.schema.fields.reserve(field_count);
  for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
    FieldSpec field;
    ARROW_ASSIGN_OR_RAISE(field.field_id, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint16_t raw_type, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t flags, reader.ReadU16());
    if ((flags & ~1U) != 0) {
      return InvalidFormat("unknown field flags");
    }
    field.nullable = (flags & 1U) != 0;
    ARROW_ASSIGN_OR_RAISE(const uint32_t name_length, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint32_t parameters_length, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(field.name, reader.ReadString(name_length));
    ARROW_ASSIGN_OR_RAISE(auto parameters, reader.ReadBytes(parameters_length));
    ARROW_ASSIGN_OR_RAISE(field.type,
                          ArrowTypeFor(static_cast<PhysicalTypeId>(raw_type), parameters));
    footer.schema.fields.push_back(std::move(field));
  }
  ARROW_RETURN_NOT_OK(footer.schema.Validate());

  if (encoding_count > reader.remaining() / 8U) {
    return Truncated("encoding descriptors");
  }
  bool saw_plain = false;
  for (uint32_t encoding_index = 0; encoding_index < encoding_count; ++encoding_index) {
    ARROW_ASSIGN_OR_RAISE(const uint16_t encoding_id, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t major, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t minor, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t name_length, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const auto name, reader.ReadString(name_length));
    if (encoding_id != kPlainEncodingId || major != kPlainEncodingMajor ||
        minor > kPlainEncodingMinor || name != "plain" || saw_plain) {
      return arrow::Status::NotImplemented(
          "[sniffer.format.encoding] unsupported encoding descriptor");
    }
    saw_plain = true;
  }
  if (!saw_plain) {
    return InvalidFormat("missing Plain encoding descriptor");
  }

  if (row_group_count > reader.remaining() / 16U) {
    return Truncated("row group directory");
  }
  if (row_group_count > std::numeric_limits<size_t>::max()) {
    return arrow::Status::Invalid("[sniffer.format.limit] too many row groups");
  }
  footer.row_groups.reserve(static_cast<size_t>(row_group_count));
  for (uint64_t row_group_index = 0; row_group_index < row_group_count; ++row_group_index) {
    RowGroupMeta row_group;
    ARROW_ASSIGN_OR_RAISE(row_group.row_count, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint32_t chunk_count, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint32_t reserved, reader.ReadU32());
    if (reserved != 0) {
      return InvalidFormat("non-zero row group reserved field");
    }
    if (chunk_count > reader.remaining() / 56U) {
      return Truncated("column chunk directory");
    }
    row_group.chunks.reserve(chunk_count);
    for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
      ColumnChunkMeta chunk;
      ARROW_ASSIGN_OR_RAISE(chunk.field_id, reader.ReadU32());
      ARROW_ASSIGN_OR_RAISE(const uint16_t raw_type, reader.ReadU16());
      chunk.physical_type = static_cast<PhysicalTypeId>(raw_type);
      ARROW_ASSIGN_OR_RAISE(chunk.encoding_id, reader.ReadU16());
      ARROW_ASSIGN_OR_RAISE(chunk.row_count, reader.ReadU64());
      ARROW_ASSIGN_OR_RAISE(chunk.null_count, reader.ReadU64());
      ARROW_ASSIGN_OR_RAISE(chunk.offset, reader.ReadU64());
      ARROW_ASSIGN_OR_RAISE(chunk.length, reader.ReadU64());
      ARROW_ASSIGN_OR_RAISE(chunk.uncompressed_length, reader.ReadU64());
      ARROW_ASSIGN_OR_RAISE(chunk.checksum, reader.ReadU32());
      ARROW_ASSIGN_OR_RAISE(const uint32_t chunk_reserved, reader.ReadU32());
      if (chunk_reserved != 0) {
        return InvalidFormat("non-zero column chunk reserved field");
      }
      row_group.chunks.push_back(chunk);
    }
    footer.row_groups.push_back(std::move(row_group));
  }
  if (reader.remaining() != 0) {
    return InvalidFormat("trailing footer bytes");
  }
  return footer;
}

}  // namespace sniffer::internal
