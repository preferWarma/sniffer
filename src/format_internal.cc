#include "format_internal.h"

#include <arrow/util/key_value_metadata.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "scalar_internal.h"

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

arrow::Result<const FieldSpec*> FindField(const TableSchema& schema, uint32_t field_id) {
  const auto field = std::find_if(
      schema.fields.begin(), schema.fields.end(),
      [field_id](const FieldSpec& candidate) { return candidate.field_id == field_id; });
  if (field == schema.fields.end()) {
    return InvalidFormat("index references unknown field ID");
  }
  return &*field;
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
  writer.WriteU32(4);
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
  writer.WriteU16(kDictionaryEncodingId);
  writer.WriteU16(kEncodingMajor);
  writer.WriteU16(kEncodingMinor);
  writer.WriteU16(10);
  writer.WriteString("dictionary");
  writer.WriteU16(kRleEncodingId);
  writer.WriteU16(kEncodingMajor);
  writer.WriteU16(kEncodingMinor);
  writer.WriteU16(3);
  writer.WriteString("rle");
  writer.WriteU16(kForBitpackEncodingId);
  writer.WriteU16(kEncodingMajor);
  writer.WriteU16(kEncodingMinor);
  writer.WriteU16(11);
  writer.WriteString("for_bitpack");

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

  if (footer.layout_policy.sort_key_field_ids.size() > std::numeric_limits<uint32_t>::max() ||
      footer.layout_policy.statistics_field_ids.size() > std::numeric_limits<uint32_t>::max() ||
      footer.layout_policy.bloom_field_ids.size() > std::numeric_limits<uint32_t>::max()) {
    return arrow::Status::Invalid("[sniffer.format.limit] too many configured index fields");
  }
  ARROW_RETURN_NOT_OK(footer.layout_policy.Validate(footer.schema));
  writer.WriteU32(footer.layout_policy.target_row_group_rows);
  writer.WriteU32(static_cast<uint32_t>(footer.layout_policy.sort_key_field_ids.size()));
  writer.WriteU32(static_cast<uint32_t>(footer.layout_policy.statistics_field_ids.size()));
  writer.WriteU32(static_cast<uint32_t>(footer.layout_policy.bloom_field_ids.size()));
  for (const uint32_t field_id : footer.layout_policy.sort_key_field_ids) {
    writer.WriteU32(field_id);
  }
  for (const uint32_t field_id : footer.layout_policy.statistics_field_ids) {
    writer.WriteU32(field_id);
  }
  for (const uint32_t field_id : footer.layout_policy.bloom_field_ids) {
    writer.WriteU32(field_id);
  }
  for (const auto& row_group : footer.row_groups) {
    writer.WriteU64(row_group.index_block.offset);
    writer.WriteU64(row_group.index_block.length);
    writer.WriteU32(row_group.index_block.checksum);
    writer.WriteU32(0);
  }
  return std::move(writer).Finish();
}

arrow::Result<FooterData> ParseFooter(std::span<const uint8_t> bytes) {
  ByteReader reader(bytes);
  ARROW_ASSIGN_OR_RAISE(const uint16_t payload_version, reader.ReadU16());
  ARROW_ASSIGN_OR_RAISE(const uint16_t prefix_reserved, reader.ReadU16());
  if ((payload_version != kLegacyFooterPayloadVersion &&
       payload_version != kFooterPayloadVersion) ||
      prefix_reserved != 0) {
    return arrow::Status::NotImplemented(
        "[sniffer.format.footer_version] unsupported footer payload");
  }

  FooterData footer;
  footer.has_phase_two_metadata = payload_version >= kFooterPayloadVersion;
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
  std::unordered_set<uint16_t> seen_encodings;
  for (uint32_t encoding_index = 0; encoding_index < encoding_count; ++encoding_index) {
    ARROW_ASSIGN_OR_RAISE(const uint16_t encoding_id, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t major, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t minor, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const uint16_t name_length, reader.ReadU16());
    ARROW_ASSIGN_OR_RAISE(const auto name, reader.ReadString(name_length));
    std::string_view expected_name;
    switch (encoding_id) {
      case kPlainEncodingId:
        expected_name = "plain";
        break;
      case kDictionaryEncodingId:
        expected_name = "dictionary";
        break;
      case kRleEncodingId:
        expected_name = "rle";
        break;
      case kForBitpackEncodingId:
        expected_name = "for_bitpack";
        break;
      default:
        return arrow::Status::NotImplemented(
            "[sniffer.format.encoding] unsupported encoding descriptor");
    }
    if (major != kEncodingMajor || minor > kEncodingMinor || name != expected_name ||
        !seen_encodings.insert(encoding_id).second) {
      return arrow::Status::NotImplemented(
          "[sniffer.format.encoding] unsupported encoding descriptor");
    }
    footer.encoding_ids.push_back(encoding_id);
  }
  if (!seen_encodings.contains(kPlainEncodingId)) {
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
  if (payload_version == kLegacyFooterPayloadVersion) {
    footer.has_phase_two_metadata = false;
    if (reader.remaining() != 0) {
      return InvalidFormat("trailing footer bytes");
    }
    return footer;
  }

  ARROW_ASSIGN_OR_RAISE(footer.layout_policy.target_row_group_rows, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t sort_key_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t statistics_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t bloom_count, reader.ReadU32());
  const uint64_t configured_count =
      static_cast<uint64_t>(sort_key_count) + statistics_count + bloom_count;
  if (configured_count > reader.remaining() / 4U) {
    return Truncated("layout policy field IDs");
  }
  footer.layout_policy.sort_key_field_ids.reserve(sort_key_count);
  footer.layout_policy.statistics_field_ids.reserve(statistics_count);
  footer.layout_policy.bloom_field_ids.reserve(bloom_count);
  for (uint32_t index = 0; index < sort_key_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(const uint32_t field_id, reader.ReadU32());
    footer.layout_policy.sort_key_field_ids.push_back(field_id);
  }
  for (uint32_t index = 0; index < statistics_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(const uint32_t field_id, reader.ReadU32());
    footer.layout_policy.statistics_field_ids.push_back(field_id);
  }
  for (uint32_t index = 0; index < bloom_count; ++index) {
    ARROW_ASSIGN_OR_RAISE(const uint32_t field_id, reader.ReadU32());
    footer.layout_policy.bloom_field_ids.push_back(field_id);
  }
  ARROW_RETURN_NOT_OK(footer.layout_policy.Validate(footer.schema));
  if (row_group_count > reader.remaining() / 24U) {
    return Truncated("row group index directory");
  }
  for (auto& row_group : footer.row_groups) {
    ARROW_ASSIGN_OR_RAISE(row_group.index_block.offset, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(row_group.index_block.length, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(row_group.index_block.checksum, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint32_t reserved, reader.ReadU32());
    if (reserved != 0) {
      return InvalidFormat("non-zero index directory reserved field");
    }
  }
  if (reader.remaining() != 0) {
    return InvalidFormat("trailing footer bytes");
  }
  return footer;
}

arrow::Result<std::vector<uint8_t>> SerializeIndexBlock(const TableSchema& schema,
                                                        const RowGroupIndex& index) {
  if (index.statistics.size() > std::numeric_limits<uint32_t>::max() ||
      index.blooms.size() > std::numeric_limits<uint32_t>::max() ||
      index.sort_keys.size() > std::numeric_limits<uint32_t>::max()) {
    return arrow::Status::Invalid("[sniffer.format.limit] too many row-group indexes");
  }
  ByteWriter writer;
  writer.WriteU16(kIndexBlockVersion);
  writer.WriteU16(0);
  writer.WriteU32(static_cast<uint32_t>(index.statistics.size()));
  writer.WriteU32(static_cast<uint32_t>(index.blooms.size()));
  writer.WriteU32(static_cast<uint32_t>(index.sort_keys.size()));
  writer.WriteU32(0);

  for (const auto& statistics : index.statistics) {
    ARROW_ASSIGN_OR_RAISE(const auto* field, FindField(schema, statistics.field_id));
    const bool has_min_max = statistics.min && statistics.max;
    std::vector<uint8_t> min_bytes;
    std::vector<uint8_t> max_bytes;
    if (has_min_max) {
      ARROW_ASSIGN_OR_RAISE(min_bytes, SerializeScalar(*field, *statistics.min));
      ARROW_ASSIGN_OR_RAISE(max_bytes, SerializeScalar(*field, *statistics.max));
    }
    writer.WriteU32(statistics.field_id);
    writer.WriteU32(has_min_max ? 1U : 0U);
    writer.WriteU64(statistics.null_count);
    writer.WriteU64(static_cast<uint64_t>(min_bytes.size()));
    writer.WriteU64(static_cast<uint64_t>(max_bytes.size()));
    writer.WriteBytes(min_bytes);
    writer.WriteBytes(max_bytes);
  }
  for (const auto& bloom : index.blooms) {
    if (bloom.bits.size() > std::numeric_limits<uint64_t>::max()) {
      return arrow::Status::Invalid("[sniffer.format.limit] Bloom filter is too large");
    }
    writer.WriteU32(bloom.field_id);
    writer.WriteU32(bloom.hash_count);
    writer.WriteU64(bloom.bit_count);
    writer.WriteU64(static_cast<uint64_t>(bloom.bits.size()));
    writer.WriteBytes(bloom.bits);
  }
  for (const auto& sort_key : index.sort_keys) {
    ARROW_ASSIGN_OR_RAISE(const auto* field, FindField(schema, sort_key.field_id));
    if (!sort_key.first || !sort_key.last) {
      return InvalidFormat("missing sort-key boundary");
    }
    ARROW_ASSIGN_OR_RAISE(auto first_bytes, SerializeScalar(*field, *sort_key.first));
    ARROW_ASSIGN_OR_RAISE(auto last_bytes, SerializeScalar(*field, *sort_key.last));
    writer.WriteU32(sort_key.field_id);
    writer.WriteU32(0);
    writer.WriteU64(static_cast<uint64_t>(first_bytes.size()));
    writer.WriteU64(static_cast<uint64_t>(last_bytes.size()));
    writer.WriteBytes(first_bytes);
    writer.WriteBytes(last_bytes);
  }
  return std::move(writer).Finish();
}

arrow::Result<RowGroupIndex> ParseIndexBlock(const TableSchema& schema,
                                             std::span<const uint8_t> bytes) {
  ByteReader reader(bytes);
  ARROW_ASSIGN_OR_RAISE(const uint16_t version, reader.ReadU16());
  ARROW_ASSIGN_OR_RAISE(const uint16_t prefix_reserved, reader.ReadU16());
  if (version != kIndexBlockVersion || prefix_reserved != 0) {
    return arrow::Status::NotImplemented("[sniffer.format.index_version] unsupported index block");
  }
  ARROW_ASSIGN_OR_RAISE(const uint32_t statistics_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t bloom_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t sort_key_count, reader.ReadU32());
  ARROW_ASSIGN_OR_RAISE(const uint32_t reserved, reader.ReadU32());
  if (reserved != 0) {
    return InvalidFormat("non-zero index block reserved field");
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t minimum_statistics_bytes,
                        CheckedMultiply(static_cast<uint64_t>(statistics_count), uint64_t{32}));
  ARROW_ASSIGN_OR_RAISE(const uint64_t minimum_bloom_bytes,
                        CheckedMultiply(static_cast<uint64_t>(bloom_count), uint64_t{24}));
  ARROW_ASSIGN_OR_RAISE(const uint64_t minimum_sort_key_bytes,
                        CheckedMultiply(static_cast<uint64_t>(sort_key_count), uint64_t{24}));
  ARROW_ASSIGN_OR_RAISE(const uint64_t minimum_index_bytes,
                        CheckedAdd(minimum_statistics_bytes, minimum_bloom_bytes));
  ARROW_ASSIGN_OR_RAISE(const uint64_t minimum_entry_bytes,
                        CheckedAdd(minimum_index_bytes, minimum_sort_key_bytes));
  if (minimum_entry_bytes > reader.remaining()) {
    return Truncated("index block entries");
  }

  RowGroupIndex index;
  std::unordered_set<uint32_t> statistics_ids;
  std::unordered_set<uint32_t> bloom_ids;
  std::unordered_set<uint32_t> sort_key_ids;
  index.statistics.reserve(statistics_count);
  index.blooms.reserve(bloom_count);
  index.sort_keys.reserve(sort_key_count);
  for (uint32_t entry = 0; entry < statistics_count; ++entry) {
    StatisticsMeta statistics;
    ARROW_ASSIGN_OR_RAISE(statistics.field_id, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint32_t flags, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(statistics.null_count, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint64_t min_length, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint64_t max_length, reader.ReadU64());
    if ((flags & ~1U) != 0 || !statistics_ids.insert(statistics.field_id).second) {
      return InvalidFormat("invalid statistics entry flags or duplicate field");
    }
    ARROW_ASSIGN_OR_RAISE(const auto* field, FindField(schema, statistics.field_id));
    ARROW_ASSIGN_OR_RAISE(auto min_bytes, reader.ReadBytes(min_length));
    ARROW_ASSIGN_OR_RAISE(auto max_bytes, reader.ReadBytes(max_length));
    if ((flags & 1U) != 0) {
      ARROW_ASSIGN_OR_RAISE(statistics.min, ParseScalar(*field, min_bytes));
      ARROW_ASSIGN_OR_RAISE(statistics.max, ParseScalar(*field, max_bytes));
      ARROW_ASSIGN_OR_RAISE(const int order, CompareScalars(*statistics.min, *statistics.max));
      if (order > 0) {
        return InvalidFormat("statistics minimum exceeds maximum");
      }
    } else if (min_length != 0 || max_length != 0) {
      return InvalidFormat("statistics without min/max has scalar bytes");
    }
    index.statistics.push_back(std::move(statistics));
  }
  for (uint32_t entry = 0; entry < bloom_count; ++entry) {
    BloomMeta bloom;
    ARROW_ASSIGN_OR_RAISE(bloom.field_id, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(bloom.hash_count, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(bloom.bit_count, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint64_t byte_count, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const auto* ignored_field, FindField(schema, bloom.field_id));
    static_cast<void>(ignored_field);
    if (!bloom_ids.insert(bloom.field_id).second || bloom.hash_count != 7 || bloom.bit_count == 0 ||
        bloom.bit_count % 8U != 0 || (bloom.bit_count & (bloom.bit_count - 1U)) != 0 ||
        byte_count != bloom.bit_count / 8U || byte_count > std::numeric_limits<size_t>::max()) {
      return InvalidFormat("invalid Bloom filter entry");
    }
    ARROW_ASSIGN_OR_RAISE(auto bit_bytes, reader.ReadBytes(byte_count));
    bloom.bits.assign(bit_bytes.begin(), bit_bytes.end());
    index.blooms.push_back(std::move(bloom));
  }
  for (uint32_t entry = 0; entry < sort_key_count; ++entry) {
    SortKeyMeta sort_key;
    ARROW_ASSIGN_OR_RAISE(sort_key.field_id, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint32_t entry_reserved, reader.ReadU32());
    ARROW_ASSIGN_OR_RAISE(const uint64_t first_length, reader.ReadU64());
    ARROW_ASSIGN_OR_RAISE(const uint64_t last_length, reader.ReadU64());
    if (entry_reserved != 0 || !sort_key_ids.insert(sort_key.field_id).second) {
      return InvalidFormat("invalid sort-key entry");
    }
    ARROW_ASSIGN_OR_RAISE(const auto* field, FindField(schema, sort_key.field_id));
    ARROW_ASSIGN_OR_RAISE(auto first_bytes, reader.ReadBytes(first_length));
    ARROW_ASSIGN_OR_RAISE(auto last_bytes, reader.ReadBytes(last_length));
    ARROW_ASSIGN_OR_RAISE(sort_key.first, ParseScalar(*field, first_bytes));
    ARROW_ASSIGN_OR_RAISE(sort_key.last, ParseScalar(*field, last_bytes));
    index.sort_keys.push_back(std::move(sort_key));
  }
  if (reader.remaining() != 0) {
    return InvalidFormat("trailing index block bytes");
  }
  return index;
}

}  // namespace sniffer::internal
