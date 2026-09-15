#include <arrow/api.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

void RequireOk(const arrow::Status& status, const std::string& context) {
  if (!status.ok()) {
    throw TestFailure(context + ": " + status.ToString());
  }
}

template <typename T>
T ValueOrThrow(arrow::Result<T> result, const std::string& context) {
  if (!result.ok()) {
    throw TestFailure(context + ": " + result.status().ToString());
  }
  return std::move(result).ValueUnsafe();
}

class TempFile {
 public:
  explicit TempFile(std::string suffix) {
    static uint64_t counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("sniffer_core_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + "_" +
             std::move(suffix));
  }

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> BuildArray(const std::vector<std::optional<Value>>& values) {
  Builder builder;
  for (const auto& value : values) {
    if (value.has_value()) {
      RequireOk(builder.Append(*value), "append test value");
    } else {
      RequireOk(builder.AppendNull(), "append test null");
    }
  }
  std::shared_ptr<arrow::Array> array;
  RequireOk(builder.Finish(&array), "finish test array");
  return array;
}

std::shared_ptr<arrow::Array> BuildTimestampArray(
    const std::shared_ptr<arrow::TimestampType>& type,
    const std::vector<std::optional<int64_t>>& values) {
  arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
  for (const auto& value : values) {
    if (value.has_value()) {
      RequireOk(builder.Append(*value), "append timestamp");
    } else {
      RequireOk(builder.AppendNull(), "append timestamp null");
    }
  }
  std::shared_ptr<arrow::Array> array;
  RequireOk(builder.Finish(&array), "finish timestamp array");
  return array;
}

std::shared_ptr<arrow::Array> BuildStringArray(
    const std::vector<std::optional<std::string>>& values) {
  arrow::StringBuilder builder;
  for (const auto& value : values) {
    if (value.has_value()) {
      RequireOk(builder.Append(*value), "append string");
    } else {
      RequireOk(builder.AppendNull(), "append string null");
    }
  }
  std::shared_ptr<arrow::Array> array;
  RequireOk(builder.Finish(&array), "finish string array");
  return array;
}

std::shared_ptr<arrow::Array> BuildBinaryArray(
    const std::vector<std::optional<std::vector<uint8_t>>>& values) {
  arrow::BinaryBuilder builder;
  for (const auto& value : values) {
    if (value.has_value()) {
      RequireOk(builder.Append(value->data(), static_cast<int32_t>(value->size())),
                "append binary");
    } else {
      RequireOk(builder.AppendNull(), "append binary null");
    }
  }
  std::shared_ptr<arrow::Array> array;
  RequireOk(builder.Finish(&array), "finish binary array");
  return array;
}

struct TestData {
  sniffer::TableSchema table_schema;
  std::shared_ptr<arrow::RecordBatch> batch;
};

TestData MakeAllTypesBatch() {
  const auto timestamp_type = std::static_pointer_cast<arrow::TimestampType>(
      arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"));
  std::vector<sniffer::FieldSpec> fields = {
      {10, "flag", arrow::boolean(), true, nullptr},
      {20, "i8", arrow::int8(), true, nullptr},
      {30, "i16", arrow::int16(), true, nullptr},
      {40, "i32", arrow::int32(), true, nullptr},
      {50, "i64", arrow::int64(), true, nullptr},
      {60, "u8", arrow::uint8(), true, nullptr},
      {70, "u16", arrow::uint16(), true, nullptr},
      {80, "u32", arrow::uint32(), true, nullptr},
      {90, "u64", arrow::uint64(), true, nullptr},
      {100, "f32", arrow::float32(), true, nullptr},
      {110, "f64", arrow::float64(), true, nullptr},
      {120, "event_time", timestamp_type, true, nullptr},
      {130, "text", arrow::utf8(), true, nullptr},
      {140, "bytes", arrow::binary(), true, nullptr},
  };
  sniffer::TableSchema table_schema{7, std::move(fields)};
  auto arrow_schema = ValueOrThrow(table_schema.ToArrowSchema(), "create Arrow schema");

  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.push_back(
      BuildArray<arrow::BooleanBuilder, bool>({true, std::nullopt, false, true, std::nullopt}));
  columns.push_back(
      BuildArray<arrow::Int8Builder, int8_t>({std::numeric_limits<int8_t>::min(), -1, std::nullopt,
                                              0, std::numeric_limits<int8_t>::max()}));
  columns.push_back(BuildArray<arrow::Int16Builder, int16_t>(
      {std::numeric_limits<int16_t>::min(), -2, std::nullopt, 2,
       std::numeric_limits<int16_t>::max()}));
  columns.push_back(BuildArray<arrow::Int32Builder, int32_t>(
      {std::numeric_limits<int32_t>::min(), -3, std::nullopt, 3,
       std::numeric_limits<int32_t>::max()}));
  columns.push_back(BuildArray<arrow::Int64Builder, int64_t>(
      {std::numeric_limits<int64_t>::min(), -4, std::nullopt, 4,
       std::numeric_limits<int64_t>::max()}));
  columns.push_back(BuildArray<arrow::UInt8Builder, uint8_t>(
      {0, 1, std::nullopt, 2, std::numeric_limits<uint8_t>::max()}));
  columns.push_back(BuildArray<arrow::UInt16Builder, uint16_t>(
      {0, 1, std::nullopt, 2, std::numeric_limits<uint16_t>::max()}));
  columns.push_back(BuildArray<arrow::UInt32Builder, uint32_t>(
      {0, 1, std::nullopt, 2, std::numeric_limits<uint32_t>::max()}));
  columns.push_back(BuildArray<arrow::UInt64Builder, uint64_t>(
      {0, 1, std::nullopt, 2, std::numeric_limits<uint64_t>::max()}));
  columns.push_back(BuildArray<arrow::FloatBuilder, float>(
      {-0.0F, 1.5F, std::nullopt, -2.25F, std::numeric_limits<float>::max()}));
  columns.push_back(BuildArray<arrow::DoubleBuilder, double>(
      {-0.0, 1.5, std::nullopt, -2.25, std::numeric_limits<double>::lowest()}));
  columns.push_back(
      BuildTimestampArray(timestamp_type, {std::numeric_limits<int64_t>::min(), 0, std::nullopt, 1,
                                           std::numeric_limits<int64_t>::max()}));
  columns.push_back(BuildStringArray({"", "hello", std::nullopt, "世界", "repeated"}));
  columns.push_back(
      BuildBinaryArray({std::vector<uint8_t>{}, std::vector<uint8_t>{0, 1, 0xFF}, std::nullopt,
                        std::vector<uint8_t>{'a', 0, 'b'}, std::vector<uint8_t>{7, 7, 7}}));

  auto batch = arrow::RecordBatch::Make(arrow_schema, 5, std::move(columns));
  RequireOk(batch->ValidateFull(), "validate all-types batch");
  return {std::move(table_schema), std::move(batch)};
}

void WriteSegment(const std::filesystem::path& path, const sniffer::TableSchema& schema,
                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                  uint32_t row_group_rows = 64 * 1024) {
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = row_group_rows;
  auto writer =
      ValueOrThrow(sniffer::SegmentWriter::Open(path.string(), schema, policy), "open writer");
  for (const auto& batch : batches) {
    RequireOk(writer->Append(batch), "append batch");
  }
  RequireOk(writer->Finish(), "finish segment");
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  Expect(stream.is_open(), "open test file for reading");
  const auto length = stream.tellg();
  Expect(length >= 0, "determine test file length");
  stream.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    Expect(static_cast<bool>(stream), "read test file");
  }
  return bytes;
}

void WriteFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  Expect(stream.is_open(), "open test file for writing");
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  Expect(static_cast<bool>(stream), "write test file");
}

uint32_t Crc32c(std::span<const uint8_t> bytes, uint32_t previous = 0) {
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

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (uint32_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

void WriteU16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  for (uint32_t index = 0; index < 4; ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void WriteU64(std::vector<uint8_t>* bytes, size_t offset, uint64_t value) {
  for (uint32_t index = 0; index < 8; ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void RefreshFooterChecksums(std::vector<uint8_t>* bytes) {
  constexpr size_t kTrailerSize = 40;
  const size_t trailer_offset = bytes->size() - kTrailerSize;
  const uint64_t footer_offset = ReadU64(*bytes, trailer_offset + 8);
  const uint64_t footer_length = ReadU64(*bytes, trailer_offset + 16);
  const auto footer = std::span<const uint8_t>(bytes->data() + static_cast<size_t>(footer_offset),
                                               static_cast<size_t>(footer_length));
  WriteU32(bytes, trailer_offset + 24, Crc32c(footer));
  WriteU32(bytes, trailer_offset + 28,
           Crc32c(std::span<const uint8_t>(bytes->data(), trailer_offset)));
  WriteU32(bytes, trailer_offset + 32,
           Crc32c(std::span<const uint8_t>(bytes->data() + trailer_offset, 32)));
}

void TestRoundTripAllTypesAndMultipleRowGroups() {
  const auto data = MakeAllTypesBatch();
  TempFile file("all_types.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch}, 2);

  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open reader");
  Expect(reader->num_row_groups() == 3, "expected three row groups");
  Expect(reader->schema().schema_version == 7, "schema version round-trip");
  Expect(reader->schema().fields.size() == data.table_schema.fields.size(),
         "field count round-trip");
  for (size_t index = 0; index < data.table_schema.fields.size(); ++index) {
    const auto& expected = data.table_schema.fields[index];
    const auto& actual = reader->schema().fields[index];
    Expect(actual.field_id == expected.field_id, "field ID round-trip");
    Expect(actual.name == expected.name, "field name round-trip");
    Expect(actual.nullable == expected.nullable, "nullable round-trip");
    Expect(actual.type->Equals(expected.type), "field type round-trip");
  }
  auto batches = ValueOrThrow(reader->ReadAll(), "read all row groups");
  Expect(batches.size() == 3, "read three batches");
  int64_t offset = 0;
  for (const auto& batch : batches) {
    Expect(batch->Equals(*data.batch->Slice(offset, batch->num_rows())),
           "row-group batch equals input slice");
    for (int column = 0; column < batch->num_columns(); ++column) {
      const auto metadata = batch->schema()->field(column)->metadata();
      Expect(metadata != nullptr, "field ID metadata exists");
      auto field_id = metadata->Get(sniffer::kFieldIdMetadataKey);
      Expect(field_id.ok(), "field ID metadata lookup succeeds");
      Expect(field_id.ValueUnsafe() ==
                 std::to_string(data.table_schema.fields[static_cast<size_t>(column)].field_id),
             "field ID metadata value round-trip");
    }
    offset += batch->num_rows();
  }
  RequireOk(reader->VerifyFileChecksum(), "verify whole-file checksum");
}

void TestEmptyBatchRoundTrip() {
  sniffer::TableSchema schema{1, {{1, "value", arrow::int32(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "empty schema");
  auto values = BuildArray<arrow::Int32Builder, int32_t>({});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 0, {values});
  TempFile file("empty.seg");
  WriteSegment(file.path(), schema, {batch}, 2);

  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open empty reader");
  Expect(reader->num_row_groups() == 0, "empty segment has no physical row group");
  auto batches = ValueOrThrow(reader->ReadAll(), "read empty segment");
  Expect(batches.size() == 1 && batches[0]->num_rows() == 0,
         "empty segment yields one schema-bearing empty batch");
  Expect(batches[0]->schema()->field(0)->type()->Equals(arrow::int32()),
         "empty batch schema is retained");
}

void TestDeterministicOutput() {
  const auto data = MakeAllTypesBatch();
  TempFile first("deterministic_a.seg");
  TempFile second("deterministic_b.seg");
  WriteSegment(first.path(), data.table_schema, {data.batch}, 3);
  WriteSegment(second.path(), data.table_schema, {data.batch}, 3);
  Expect(ReadFile(first.path()) == ReadFile(second.path()),
         "same input and policy produce identical files");
}

void TestDeterministicRandomizedRoundTrip() {
  sniffer::TableSchema schema{11,
                              {{101, "number", arrow::int64(), true, nullptr},
                               {102, "label", arrow::utf8(), true, nullptr},
                               {103, "payload", arrow::binary(), true, nullptr},
                               {104, "all_null", arrow::float64(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "random schema");

  arrow::Int64Builder number_builder;
  arrow::StringBuilder label_builder;
  arrow::BinaryBuilder payload_builder;
  arrow::DoubleBuilder all_null_builder;
  std::mt19937_64 random(0x5A17D00DULL);
  constexpr int64_t kRows = 257;
  for (int64_t row = 0; row < kRows; ++row) {
    if (row % 5 == 0) {
      RequireOk(number_builder.AppendNull(), "append random number null");
      RequireOk(label_builder.AppendNull(), "append random label null");
      RequireOk(payload_builder.AppendNull(), "append random payload null");
    } else {
      const int64_t number =
          row == 1 ? std::numeric_limits<int64_t>::min() : std::bit_cast<int64_t>(random());
      RequireOk(number_builder.Append(number), "append random number");

      std::string label;
      if (row % 7 == 0) {
        label = "repeated-value";
      } else {
        const size_t length = static_cast<size_t>(random() % 33U);
        label.reserve(length);
        for (size_t index = 0; index < length; ++index) {
          label.push_back(static_cast<char>('a' + random() % 26U));
        }
      }
      RequireOk(label_builder.Append(label), "append random label");

      const size_t length = static_cast<size_t>(random() % 33U);
      std::vector<uint8_t> payload(length);
      for (auto& byte : payload) {
        byte = static_cast<uint8_t>(random());
      }
      RequireOk(payload_builder.Append(payload.data(), static_cast<int32_t>(payload.size())),
                "append random payload");
    }
    RequireOk(all_null_builder.AppendNull(), "append all-null value");
  }

  std::vector<std::shared_ptr<arrow::Array>> columns(4);
  RequireOk(number_builder.Finish(&columns[0]), "finish random numbers");
  RequireOk(label_builder.Finish(&columns[1]), "finish random labels");
  RequireOk(payload_builder.Finish(&columns[2]), "finish random payloads");
  RequireOk(all_null_builder.Finish(&columns[3]), "finish all-null column");
  auto batch = arrow::RecordBatch::Make(arrow_schema, kRows, std::move(columns));
  RequireOk(batch->ValidateFull(), "validate randomized batch");

  TempFile file("randomized.seg");
  WriteSegment(file.path(), schema, {batch->Slice(0, 101), batch->Slice(101)}, 37);
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open randomized reader");
  auto batches = ValueOrThrow(reader->ReadAll(), "read randomized segment");
  int64_t offset = 0;
  for (const auto& output : batches) {
    Expect(output->Equals(*batch->Slice(offset, output->num_rows())),
           "randomized output equals input slice");
    offset += output->num_rows();
  }
  Expect(offset == kRows, "randomized round-trip preserves every row");
}

void TestHeaderCorruptionFailsOpen() {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_header.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes[0] ^= 0x01U;
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok(), "bad header magic must fail Open");
}

void TestUnsupportedVersionFailsOpen() {
  const auto data = MakeAllTypesBatch();
  TempFile file("unsupported_version.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  WriteU16(&bytes, 8, 2);
  WriteU32(&bytes, 28, Crc32c(std::span<const uint8_t>(bytes.data(), 28)));
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok() && reader.status().IsNotImplemented(),
         "unsupported format major version must fail explicitly");
}

void TestFooterCorruptionFailsOpen() {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_footer.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  bytes[footer_offset] ^= 0x01U;
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok(), "bad footer checksum must fail Open");
}

void TestChunkCorruptionFailsLazyReadAndFileVerify() {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_chunk.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes[32 + 24] ^= 0x01U;
  WriteFile(file.path(), bytes);

  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()),
                             "metadata-only open after chunk corruption");
  Expect(!reader->ReadAll().ok(), "chunk corruption must fail when chunk is read");
  Expect(!reader->VerifyFileChecksum().ok(), "chunk corruption must fail whole-file verification");
}

void TestInvalidChunkOffsetFailsOpen() {
  sniffer::TableSchema schema{1, {{77, "x", arrow::int32(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "single schema");
  auto array = BuildArray<arrow::Int32Builder, int32_t>({1, std::nullopt, 3});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 3, {array});
  TempFile file("bad_offset.seg");
  WriteSegment(file.path(), schema, {batch});
  auto bytes = ReadFile(file.path());

  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  constexpr size_t kFieldDescriptor = 16;
  const size_t field_name_length = 1;
  constexpr size_t kEncodingDescriptor = 13;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry = footer_offset + kPrefix + kFieldDescriptor + field_name_length +
                             kEncodingDescriptor + kRowGroupHeader;
  WriteU64(&bytes, chunk_entry + 24, std::numeric_limits<uint64_t>::max());
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok(), "overflowing chunk offset must fail Open");
}

void TestUnknownEncodingFailsOpen() {
  sniffer::TableSchema schema{1, {{77, "x", arrow::int32(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "single schema");
  auto array = BuildArray<arrow::Int32Builder, int32_t>({1, 2, 3});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 3, {array});
  TempFile file("unknown_encoding.seg");
  WriteSegment(file.path(), schema, {batch});
  auto bytes = ReadFile(file.path());

  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  constexpr size_t kFieldDescriptor = 16;
  const size_t field_name_length = 1;
  constexpr size_t kEncodingDescriptor = 13;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry = footer_offset + kPrefix + kFieldDescriptor + field_name_length +
                             kEncodingDescriptor + kRowGroupHeader;
  WriteU16(&bytes, chunk_entry + 6, 999);
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok() && reader.status().IsNotImplemented(),
         "unknown encoding must fail explicitly");
}

void TestUnknownPhysicalTypeFailsOpen() {
  sniffer::TableSchema schema{1, {{77, "x", arrow::int32(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "single schema");
  auto array = BuildArray<arrow::Int32Builder, int32_t>({1, 2, 3});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 3, {array});
  TempFile file("unknown_physical_type.seg");
  WriteSegment(file.path(), schema, {batch});
  auto bytes = ReadFile(file.path());

  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  WriteU16(&bytes, footer_offset + kPrefix + 4, 999);
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok() && reader.status().IsNotImplemented(),
         "unknown physical type must fail explicitly");
}

void TestTruncationFailsOpen() {
  const auto data = MakeAllTypesBatch();
  TempFile file("truncated.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes.resize(bytes.size() - 7);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  Expect(!reader.ok(), "truncated metadata must fail Open");
}

void TestSchemaAndWriterStateValidation() {
  sniffer::TableSchema duplicate{
      1, {{1, "a", arrow::int32(), true, nullptr}, {1, "b", arrow::int64(), true, nullptr}}};
  Expect(!duplicate.Validate().ok(), "duplicate field IDs must fail");

  sniffer::TableSchema nested{1, {{1, "nested", arrow::list(arrow::int32()), true, nullptr}}};
  Expect(nested.Validate().IsNotImplemented(), "nested type must fail explicitly in phase one");

  sniffer::TableSchema schema{1, {{1, "x", arrow::int32(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "writer schema");
  auto array = BuildArray<arrow::Int32Builder, int32_t>({1});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 1, {array});
  TempFile file("writer_state.seg");
  auto writer =
      ValueOrThrow(sniffer::SegmentWriter::Open(file.path().string(), schema), "open state writer");
  RequireOk(writer->Append(batch), "append state batch");
  RequireOk(writer->Finish(), "finish state writer");
  Expect(!writer->Append(batch).ok(), "Append after Finish must fail");
  Expect(!writer->Finish().ok(), "second Finish must fail");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"round_trip_all_types_and_multiple_row_groups", TestRoundTripAllTypesAndMultipleRowGroups},
      {"empty_batch_round_trip", TestEmptyBatchRoundTrip},
      {"deterministic_output", TestDeterministicOutput},
      {"deterministic_randomized_round_trip", TestDeterministicRandomizedRoundTrip},
      {"header_corruption", TestHeaderCorruptionFailsOpen},
      {"unsupported_version", TestUnsupportedVersionFailsOpen},
      {"footer_corruption", TestFooterCorruptionFailsOpen},
      {"chunk_corruption", TestChunkCorruptionFailsLazyReadAndFileVerify},
      {"invalid_chunk_offset", TestInvalidChunkOffsetFailsOpen},
      {"unknown_encoding", TestUnknownEncodingFailsOpen},
      {"unknown_physical_type", TestUnknownPhysicalTypeFailsOpen},
      {"truncation", TestTruncationFailsOpen},
      {"schema_and_writer_state", TestSchemaAndWriterStateValidation},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}
