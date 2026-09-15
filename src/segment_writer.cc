#include "sniffer/segment_writer.h"

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
#include "index_internal.h"

namespace sniffer {
namespace {

using internal::ByteWriter;

arrow::Status IoError(const std::string& operation, const std::string& path) {
  return arrow::Status::IOError("[sniffer.io] ", operation, ": ", path);
}

void WriteIntegralValue(ByteWriter* writer, uint64_t value, uint32_t width) {
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

template <typename ArrayType>
void EncodeIntegerValues(const ArrayType& array, uint32_t width, ByteWriter* values) {
  for (int64_t index = 0; index < array.length(); ++index) {
    const uint64_t value = array.IsNull(index) ? 0 : static_cast<uint64_t>(array.Value(index));
    WriteIntegralValue(values, value, width);
  }
}

std::vector<uint8_t> EncodeValidity(const arrow::Array& array) {
  if (array.null_count() == 0) {
    return {};
  }
  const uint64_t row_count = static_cast<uint64_t>(array.length());
  const auto byte_count = static_cast<size_t>(row_count / 8U + (row_count % 8U != 0));
  std::vector<uint8_t> validity(byte_count, 0);
  for (int64_t index = 0; index < array.length(); ++index) {
    if (array.IsValid(index)) {
      const size_t byte_index = static_cast<size_t>(index / 8);
      const uint32_t bit_index = static_cast<uint32_t>(index % 8);
      validity[byte_index] |= static_cast<uint8_t>(1U << bit_index);
    }
  }
  return validity;
}

arrow::Result<std::vector<uint8_t>> EncodePlain(const FieldSpec& field,
                                                const std::shared_ptr<arrow::Array>& array) {
  // Plain serialization deliberately copies values so persisted bytes have an
  // explicit endian and deterministic contents for Arrow null slots.
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, internal::PhysicalTypeFor(*field.type));
  auto validity = EncodeValidity(*array);
  ByteWriter offsets;
  ByteWriter values;

  switch (physical_type) {
    case internal::PhysicalTypeId::kBool: {
      const auto& typed = static_cast<const arrow::BooleanArray&>(*array);
      for (int64_t index = 0; index < typed.length(); ++index) {
        values.WriteU8(typed.IsValid(index) && typed.Value(index) ? uint8_t{1} : uint8_t{0});
      }
      break;
    }
    case internal::PhysicalTypeId::kInt8:
      EncodeIntegerValues(static_cast<const arrow::Int8Array&>(*array), 1, &values);
      break;
    case internal::PhysicalTypeId::kInt16:
      EncodeIntegerValues(static_cast<const arrow::Int16Array&>(*array), 2, &values);
      break;
    case internal::PhysicalTypeId::kInt32:
      EncodeIntegerValues(static_cast<const arrow::Int32Array&>(*array), 4, &values);
      break;
    case internal::PhysicalTypeId::kInt64:
      EncodeIntegerValues(static_cast<const arrow::Int64Array&>(*array), 8, &values);
      break;
    case internal::PhysicalTypeId::kUInt8:
      EncodeIntegerValues(static_cast<const arrow::UInt8Array&>(*array), 1, &values);
      break;
    case internal::PhysicalTypeId::kUInt16:
      EncodeIntegerValues(static_cast<const arrow::UInt16Array&>(*array), 2, &values);
      break;
    case internal::PhysicalTypeId::kUInt32:
      EncodeIntegerValues(static_cast<const arrow::UInt32Array&>(*array), 4, &values);
      break;
    case internal::PhysicalTypeId::kUInt64:
      EncodeIntegerValues(static_cast<const arrow::UInt64Array&>(*array), 8, &values);
      break;
    case internal::PhysicalTypeId::kFloat32: {
      const auto& typed = static_cast<const arrow::FloatArray&>(*array);
      for (int64_t index = 0; index < typed.length(); ++index) {
        const float value = typed.IsNull(index) ? 0.0F : typed.Value(index);
        values.WriteU32(std::bit_cast<uint32_t>(value));
      }
      break;
    }
    case internal::PhysicalTypeId::kFloat64: {
      const auto& typed = static_cast<const arrow::DoubleArray&>(*array);
      for (int64_t index = 0; index < typed.length(); ++index) {
        const double value = typed.IsNull(index) ? 0.0 : typed.Value(index);
        values.WriteU64(std::bit_cast<uint64_t>(value));
      }
      break;
    }
    case internal::PhysicalTypeId::kTimestamp:
      EncodeIntegerValues(static_cast<const arrow::TimestampArray&>(*array), 8, &values);
      break;
    case internal::PhysicalTypeId::kString:
    case internal::PhysicalTypeId::kBinary: {
      offsets.WriteU64(0);
      uint64_t current_offset = 0;
      const auto& typed = static_cast<const arrow::BinaryArray&>(*array);
      for (int64_t index = 0; index < typed.length(); ++index) {
        if (typed.IsValid(index)) {
          const std::string_view value = typed.GetView(index);
          ARROW_ASSIGN_OR_RAISE(
              current_offset,
              internal::CheckedAdd(current_offset, static_cast<uint64_t>(value.size())));
          values.WriteBytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()),
                                                     value.size()));
        }
        offsets.WriteU64(current_offset);
      }
      break;
    }
  }

  ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU64(static_cast<uint64_t>(offsets.data().size()));
  payload.WriteU64(static_cast<uint64_t>(values.data().size()));
  payload.WriteBytes(validity);
  payload.WriteBytes(offsets.data());
  payload.WriteBytes(values.data());
  return std::move(payload).Finish();
}

}  // namespace

class SegmentWriter::Impl {
 public:
  Impl(std::string path, TableSchema schema, LayoutPolicy layout_policy)
      : path_(std::move(path)),
        schema_(std::move(schema)),
        layout_policy_(std::move(layout_policy)),
        stream_(path_, std::ios::binary | std::ios::trunc) {}

  arrow::Status Initialize() {
    if (!stream_.is_open()) {
      return IoError("cannot open segment for writing", path_);
    }
    const auto header = internal::SerializeHeader();
    return WriteTracked(header);
  }

  arrow::Status Append(const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (finished_) {
      return arrow::Status::Invalid("[sniffer.writer.state] Append called after Finish");
    }
    if (!batch) {
      return arrow::Status::Invalid("[sniffer.writer.input] null RecordBatch");
    }
    ARROW_RETURN_NOT_OK(batch->ValidateFull());
    ARROW_RETURN_NOT_OK(schema_.ValidateBatch(*batch));
    ARROW_RETURN_NOT_OK(
        internal::ValidateAndUpdateSortOrder(schema_, layout_policy_, *batch, &previous_sort_key_));
    int64_t offset = 0;
    while (offset < batch->num_rows()) {
      const int64_t remaining = batch->num_rows() - offset;
      const int64_t row_count =
          std::min<int64_t>(remaining, static_cast<int64_t>(layout_policy_.target_row_group_rows));
      ARROW_RETURN_NOT_OK(WriteRowGroup(batch->Slice(offset, row_count)));
      offset += row_count;
    }
    return arrow::Status::OK();
  }

  arrow::Status Finish() {
    if (finished_) {
      return arrow::Status::Invalid("[sniffer.writer.state] Finish called more than once");
    }
    internal::FooterData footer;
    footer.schema = schema_;
    footer.row_groups = row_groups_;
    footer.layout_policy = layout_policy_;
    ARROW_ASSIGN_OR_RAISE(auto footer_bytes, internal::SerializeFooter(footer));
    const uint64_t footer_offset = position_;
    const uint32_t footer_checksum = internal::Crc32c(footer_bytes);
    ARROW_RETURN_NOT_OK(WriteTracked(footer_bytes));

    internal::FooterTrailer trailer;
    trailer.footer_offset = footer_offset;
    trailer.footer_length = static_cast<uint64_t>(footer_bytes.size());
    trailer.footer_checksum = footer_checksum;
    trailer.file_checksum = file_checksum_;
    const auto trailer_bytes = internal::SerializeTrailer(trailer);
    ARROW_RETURN_NOT_OK(WriteUntracked(trailer_bytes));
    stream_.flush();
    if (!stream_) {
      return IoError("cannot flush segment", path_);
    }
    stream_.close();
    finished_ = true;
    return arrow::Status::OK();
  }

 private:
  arrow::Status WriteRowGroup(const std::shared_ptr<arrow::RecordBatch>& batch) {
    ARROW_ASSIGN_OR_RAISE(auto indexes,
                          internal::BuildRowGroupIndex(schema_, layout_policy_, *batch));
    internal::RowGroupMeta row_group;
    row_group.row_count = static_cast<uint64_t>(batch->num_rows());
    row_group.chunks.reserve(schema_.fields.size());
    for (int column_index = 0; column_index < batch->num_columns(); ++column_index) {
      const auto& field = schema_.fields[static_cast<size_t>(column_index)];
      const auto& array = batch->column(column_index);
      ARROW_ASSIGN_OR_RAISE(auto payload, EncodePlain(field, array));
      ARROW_ASSIGN_OR_RAISE(const auto physical_type, internal::PhysicalTypeFor(*field.type));

      internal::ColumnChunkMeta chunk;
      chunk.field_id = field.field_id;
      chunk.physical_type = physical_type;
      chunk.encoding_id = internal::kPlainEncodingId;
      chunk.row_count = static_cast<uint64_t>(array->length());
      chunk.null_count = static_cast<uint64_t>(array->null_count());
      chunk.offset = position_;
      chunk.length = static_cast<uint64_t>(payload.size());
      chunk.uncompressed_length = chunk.length;
      chunk.checksum = internal::Crc32c(payload);
      ARROW_RETURN_NOT_OK(WriteTracked(payload));
      row_group.chunks.push_back(chunk);
    }
    ARROW_ASSIGN_OR_RAISE(auto index_bytes, internal::SerializeIndexBlock(schema_, indexes));
    row_group.index_block.offset = position_;
    row_group.index_block.length = static_cast<uint64_t>(index_bytes.size());
    row_group.index_block.checksum = internal::Crc32c(index_bytes);
    ARROW_RETURN_NOT_OK(WriteTracked(index_bytes));
    row_groups_.push_back(std::move(row_group));
    return arrow::Status::OK();
  }

  arrow::Status WriteTracked(std::span<const uint8_t> bytes) {
    ARROW_RETURN_NOT_OK(WriteUntracked(bytes));
    file_checksum_ = internal::Crc32c(bytes, file_checksum_);
    return arrow::Status::OK();
  }

  arrow::Status WriteUntracked(std::span<const uint8_t> bytes) {
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
      return arrow::Status::Invalid("[sniffer.format.limit] write exceeds streamsize");
    }
    stream_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!stream_) {
      return IoError("cannot write segment", path_);
    }
    ARROW_ASSIGN_OR_RAISE(position_,
                          internal::CheckedAdd(position_, static_cast<uint64_t>(bytes.size())));
    return arrow::Status::OK();
  }

  std::string path_;
  TableSchema schema_;
  LayoutPolicy layout_policy_;
  std::ofstream stream_;
  uint64_t position_ = 0;
  uint32_t file_checksum_ = 0;
  std::vector<internal::RowGroupMeta> row_groups_;
  std::vector<std::shared_ptr<arrow::Scalar>> previous_sort_key_;
  bool finished_ = false;
};

arrow::Result<std::unique_ptr<SegmentWriter>> SegmentWriter::Open(std::string path,
                                                                  TableSchema schema,
                                                                  LayoutPolicy layout_policy) {
  ARROW_RETURN_NOT_OK(schema.Validate());
  ARROW_RETURN_NOT_OK(layout_policy.Validate(schema));
  auto impl = std::make_unique<Impl>(std::move(path), std::move(schema), std::move(layout_policy));
  ARROW_RETURN_NOT_OK(impl->Initialize());
  return std::unique_ptr<SegmentWriter>(new SegmentWriter(std::move(impl)));
}

SegmentWriter::SegmentWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SegmentWriter::~SegmentWriter() = default;

arrow::Status SegmentWriter::Append(const std::shared_ptr<arrow::RecordBatch>& batch) {
  return impl_->Append(batch);
}

arrow::Status SegmentWriter::Finish() { return impl_->Finish(); }

}  // namespace sniffer
