#pragma once

#include <arrow/api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sniffer/layout.h"
#include "sniffer/schema.h"

namespace sniffer::internal {

inline constexpr uint16_t kFormatMajor = 1;
inline constexpr uint16_t kFormatMinor = 0;
inline constexpr uint32_t kHeaderSize = 32;
inline constexpr uint32_t kTrailerSize = 40;
inline constexpr uint16_t kLegacyFooterPayloadVersion = 1;
inline constexpr uint16_t kFooterPayloadVersion = 2;
inline constexpr uint16_t kIndexBlockVersion = 1;
inline constexpr uint16_t kPlainEncodingId = 0;
inline constexpr uint16_t kPlainEncodingMajor = 1;
inline constexpr uint16_t kPlainEncodingMinor = 0;
inline constexpr uint16_t kDictionaryEncodingId = 1;
inline constexpr uint16_t kRleEncodingId = 2;
inline constexpr uint16_t kForBitpackEncodingId = 3;
inline constexpr uint16_t kEncodingMajor = 1;
inline constexpr uint16_t kEncodingMinor = 0;

inline constexpr std::array<uint8_t, 8> kHeaderMagic = {'S', 'N', 'I', 'F', 'S', 'E', 'G', '1'};
inline constexpr std::array<uint8_t, 8> kTrailerMagic = {'S', 'N', 'I', 'F', 'E', 'N', 'D', '1'};

enum class PhysicalTypeId : uint16_t {
  kBool = 1,
  kInt8 = 2,
  kInt16 = 3,
  kInt32 = 4,
  kInt64 = 5,
  kUInt8 = 6,
  kUInt16 = 7,
  kUInt32 = 8,
  kUInt64 = 9,
  kFloat32 = 10,
  kFloat64 = 11,
  kTimestamp = 12,
  kString = 13,
  kBinary = 14,
};

struct ColumnChunkMeta {
  uint32_t field_id = 0;
  PhysicalTypeId physical_type = PhysicalTypeId::kBool;
  uint16_t encoding_id = kPlainEncodingId;
  uint64_t row_count = 0;
  uint64_t null_count = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t uncompressed_length = 0;
  uint32_t checksum = 0;
};

struct IndexBlockMeta {
  uint64_t offset = 0;
  uint64_t length = 0;
  uint32_t checksum = 0;
};

struct RowGroupMeta {
  uint64_t row_count = 0;
  std::vector<ColumnChunkMeta> chunks;
  IndexBlockMeta index_block;
};

struct FooterData {
  TableSchema schema;
  std::vector<RowGroupMeta> row_groups;
  LayoutPolicy layout_policy;
  std::vector<uint16_t> encoding_ids;
  bool has_phase_two_metadata = true;
};

struct StatisticsMeta {
  uint32_t field_id = 0;
  uint64_t null_count = 0;
  std::shared_ptr<arrow::Scalar> min;
  std::shared_ptr<arrow::Scalar> max;
};

struct BloomMeta {
  uint32_t field_id = 0;
  uint64_t bit_count = 0;
  uint32_t hash_count = 0;
  std::vector<uint8_t> bits;
};

struct SortKeyMeta {
  uint32_t field_id = 0;
  std::shared_ptr<arrow::Scalar> first;
  std::shared_ptr<arrow::Scalar> last;
};

struct RowGroupIndex {
  std::vector<StatisticsMeta> statistics;
  std::vector<BloomMeta> blooms;
  std::vector<SortKeyMeta> sort_keys;
};

struct FooterTrailer {
  uint64_t footer_offset = 0;
  uint64_t footer_length = 0;
  uint32_t footer_checksum = 0;
  uint32_t file_checksum = 0;
};

class ByteWriter {
 public:
  void Reserve(size_t capacity) { data_.reserve(capacity); }
  void WriteU8(uint8_t value);
  void WriteU16(uint16_t value);
  void WriteU32(uint32_t value);
  void WriteU64(uint64_t value);
  void WriteBytes(std::span<const uint8_t> bytes);
  void WriteString(const std::string& value);

  [[nodiscard]] const std::vector<uint8_t>& data() const { return data_; }
  [[nodiscard]] std::vector<uint8_t> Finish() && { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

  [[nodiscard]] arrow::Result<uint8_t> ReadU8();
  [[nodiscard]] arrow::Result<uint16_t> ReadU16();
  [[nodiscard]] arrow::Result<uint32_t> ReadU32();
  [[nodiscard]] arrow::Result<uint64_t> ReadU64();
  [[nodiscard]] arrow::Result<std::span<const uint8_t>> ReadBytes(uint64_t length);
  [[nodiscard]] arrow::Result<std::string> ReadString(uint64_t length);

  [[nodiscard]] uint64_t position() const { return position_; }
  [[nodiscard]] uint64_t remaining() const;

 private:
  std::span<const uint8_t> data_;
  uint64_t position_ = 0;
};

[[nodiscard]] arrow::Result<uint64_t> CheckedAdd(uint64_t left, uint64_t right);
[[nodiscard]] arrow::Result<uint64_t> CheckedMultiply(uint64_t left, uint64_t right);
[[nodiscard]] uint32_t Crc32c(std::span<const uint8_t> bytes, uint32_t previous = 0);

[[nodiscard]] std::vector<uint8_t> SerializeHeader();
[[nodiscard]] arrow::Status ValidateHeader(std::span<const uint8_t> bytes);
[[nodiscard]] std::vector<uint8_t> SerializeTrailer(const FooterTrailer& trailer);
[[nodiscard]] arrow::Result<FooterTrailer> ParseTrailer(std::span<const uint8_t> bytes);

[[nodiscard]] arrow::Result<PhysicalTypeId> PhysicalTypeFor(const arrow::DataType& type);
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::DataType>> ArrowTypeFor(
    PhysicalTypeId type_id, std::span<const uint8_t> parameters);
[[nodiscard]] uint32_t FixedWidthBytes(PhysicalTypeId type_id);

[[nodiscard]] arrow::Result<std::vector<uint8_t>> SerializeFooter(const FooterData& footer);
[[nodiscard]] arrow::Result<FooterData> ParseFooter(std::span<const uint8_t> bytes);
[[nodiscard]] arrow::Result<std::vector<uint8_t>> SerializeIndexBlock(const TableSchema& schema,
                                                                      const RowGroupIndex& index);
[[nodiscard]] arrow::Result<RowGroupIndex> ParseIndexBlock(const TableSchema& schema,
                                                           std::span<const uint8_t> bytes);

}  // namespace sniffer::internal
