#include "sniffer/segment_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "format_internal.h"

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

arrow::Result<std::shared_ptr<arrow::Array>> DecodePlain(const FieldSpec& field,
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

}  // namespace

class SegmentReader::Impl {
 public:
  Impl(std::string path, uint64_t file_size, internal::FooterTrailer trailer,
       internal::FooterData footer)
      : path_(std::move(path)),
        file_size_(file_size),
        trailer_(trailer),
        footer_(std::move(footer)) {}

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
        if (chunk.encoding_id != internal::kPlainEncodingId) {
          return arrow::Status::NotImplemented("[sniffer.format.encoding] unsupported encoding ID ",
                                               chunk.encoding_id);
        }
        if (chunk.row_count != row_group.row_count || chunk.null_count > chunk.row_count) {
          return InvalidFormat("invalid column chunk row/null count");
        }
        if (!field.nullable && chunk.null_count != 0) {
          return InvalidFormat("non-nullable field contains nulls");
        }
        if (chunk.length < 24 || chunk.uncompressed_length != chunk.length) {
          return InvalidFormat("invalid Plain column chunk length");
        }
        ARROW_ASSIGN_OR_RAISE(const uint64_t chunk_end,
                              internal::CheckedAdd(chunk.offset, chunk.length));
        if (chunk.offset < internal::kHeaderSize || chunk.offset < previous_end ||
            chunk_end > trailer_.footer_offset) {
          return arrow::Status::Invalid("[sniffer.format.bounds] invalid column chunk range");
        }
        previous_end = chunk_end;
      }
    }
    if (previous_end > trailer_.footer_offset) {
      return arrow::Status::Invalid("[sniffer.format.bounds] data overlaps footer");
    }
    return arrow::Status::OK();
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
        ARROW_ASSIGN_OR_RAISE(auto array,
                              DecodePlain(footer_.schema.fields[index], chunk, payload));
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
  auto impl = std::make_unique<Impl>(std::move(path), file_size, trailer, std::move(footer));
  ARROW_RETURN_NOT_OK(impl->ValidateDirectory());
  return std::unique_ptr<SegmentReader>(new SegmentReader(std::move(impl)));
}

SegmentReader::SegmentReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SegmentReader::~SegmentReader() = default;

const TableSchema& SegmentReader::schema() const { return impl_->schema(); }

uint64_t SegmentReader::num_row_groups() const { return impl_->num_row_groups(); }

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> SegmentReader::ReadAll() const {
  return impl_->ReadAll();
}

arrow::Status SegmentReader::VerifyFileChecksum() const { return impl_->VerifyFileChecksum(); }

}  // namespace sniffer
