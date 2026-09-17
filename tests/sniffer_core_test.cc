#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codec_internal.h"
#include "index_internal.h"
#include "scalar_internal.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

class ArrowAssertionFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void RequireOk(const arrow::Status& status, const std::string& context) {
  if (!status.ok()) {
    ADD_FAILURE() << context << ": " << status.ToString();
    throw ArrowAssertionFailure(context + ": " + status.ToString());
  }
}

template <typename T>
T ValueOrThrow(arrow::Result<T> result, const std::string& context) {
  if (!result.ok()) {
    ADD_FAILURE() << context << ": " << result.status().ToString();
    throw ArrowAssertionFailure(context + ": " + result.status().ToString());
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

arrow::Result<std::vector<uint8_t>> ReferenceEncodePlainVariable(const arrow::Array& array) {
  const uint64_t rows = static_cast<uint64_t>(array.length());
  std::vector<uint8_t> validity;
  if (array.null_count() != 0) {
    validity.resize(static_cast<size_t>(rows / 8U + (rows % 8U != 0)), 0);
    for (int64_t row = 0; row < array.length(); ++row) {
      if (array.IsValid(row)) {
        validity[static_cast<size_t>(row / 8)] |=
            static_cast<uint8_t>(1U << static_cast<uint32_t>(row % 8));
      }
    }
  }
  const auto& binary = static_cast<const arrow::BinaryArray&>(array);
  sniffer::internal::ByteWriter offsets;
  sniffer::internal::ByteWriter values;
  offsets.WriteU64(0);
  uint64_t current_offset = 0;
  for (int64_t row = 0; row < binary.length(); ++row) {
    if (binary.IsValid(row)) {
      const std::string_view value = binary.GetView(row);
      ARROW_ASSIGN_OR_RAISE(
          current_offset,
          sniffer::internal::CheckedAdd(current_offset, static_cast<uint64_t>(value.size())));
      values.WriteBytes(
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
    }
    offsets.WriteU64(current_offset);
  }
  sniffer::internal::ByteWriter payload;
  payload.WriteU64(static_cast<uint64_t>(validity.size()));
  payload.WriteU64(static_cast<uint64_t>(offsets.data().size()));
  payload.WriteU64(static_cast<uint64_t>(values.data().size()));
  payload.WriteBytes(validity);
  payload.WriteBytes(offsets.data());
  payload.WriteBytes(values.data());
  return std::move(payload).Finish();
}

arrow::Result<std::vector<uint8_t>> ReferenceEncodeRle(const sniffer::FieldSpec& field,
                                                       const arrow::Array& array) {
  ARROW_ASSIGN_OR_RAISE(const auto physical_type, sniffer::internal::PhysicalTypeFor(*field.type));
  const uint32_t width = sniffer::internal::FixedWidthBytes(physical_type);
  struct Run {
    uint64_t count = 0;
    bool valid = false;
    std::vector<uint8_t> value;
  };
  std::vector<Run> runs;
  for (int64_t row = 0; row < array.length(); ++row) {
    const bool valid = array.IsValid(row);
    std::vector<uint8_t> value(width, 0);
    if (valid) {
      ARROW_ASSIGN_OR_RAISE(auto scalar, array.GetScalar(row));
      ARROW_ASSIGN_OR_RAISE(value, sniffer::internal::SerializeScalar(field, *scalar));
    }
    if (!runs.empty() && runs.back().valid == valid && runs.back().value == value) {
      ++runs.back().count;
    } else {
      runs.push_back({1, valid, std::move(value)});
    }
  }
  sniffer::internal::ByteWriter payload;
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

uint32_t ReferenceDictionaryIndexWidth(uint64_t dictionary_count) {
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

void ReferenceWriteWidth(sniffer::internal::ByteWriter* writer, uint64_t value, uint32_t width) {
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

arrow::Result<std::vector<uint8_t>> ReferenceEncodeDictionary(const sniffer::FieldSpec& field,
                                                              const arrow::Array& array) {
  const uint64_t rows = static_cast<uint64_t>(array.length());
  std::vector<uint8_t> validity;
  if (array.null_count() != 0) {
    validity.resize(static_cast<size_t>(rows / 8U + (rows % 8U != 0)), 0);
    for (int64_t row = 0; row < array.length(); ++row) {
      if (array.IsValid(row)) {
        validity[static_cast<size_t>(row / 8)] |=
            static_cast<uint8_t>(1U << static_cast<uint32_t>(row % 8));
      }
    }
  }
  std::unordered_map<std::string, uint64_t> lookup;
  std::vector<std::vector<uint8_t>> dictionary;
  std::vector<uint64_t> indices(static_cast<size_t>(array.length()), 0);
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(auto scalar, array.GetScalar(row));
    ARROW_ASSIGN_OR_RAISE(auto bytes, sniffer::internal::SerializeScalar(field, *scalar));
    const std::string key(bytes.begin(), bytes.end());
    const auto [entry, inserted] = lookup.emplace(key, dictionary.size());
    if (inserted) {
      dictionary.push_back(std::move(bytes));
    }
    indices[static_cast<size_t>(row)] = entry->second;
  }

  const uint32_t index_width = ReferenceDictionaryIndexWidth(dictionary.size());
  sniffer::internal::ByteWriter dictionary_offsets;
  sniffer::internal::ByteWriter dictionary_values;
  const bool variable =
      field.type->id() == arrow::Type::STRING || field.type->id() == arrow::Type::BINARY;
  if (variable) {
    uint64_t offset = 0;
    dictionary_offsets.WriteU64(0);
    for (const auto& value : dictionary) {
      ARROW_ASSIGN_OR_RAISE(
          offset, sniffer::internal::CheckedAdd(offset, static_cast<uint64_t>(value.size())));
      dictionary_values.WriteBytes(value);
      dictionary_offsets.WriteU64(offset);
    }
  } else {
    for (const auto& value : dictionary) {
      dictionary_values.WriteBytes(value);
    }
  }
  sniffer::internal::ByteWriter encoded_indices;
  for (const uint64_t index : indices) {
    ReferenceWriteWidth(&encoded_indices, index, index_width);
  }
  sniffer::internal::ByteWriter payload;
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

arrow::Result<uint64_t> ReferenceIntegralBits(const arrow::Scalar& scalar) {
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
      return arrow::Status::Invalid("reference FOR value is not integral");
  }
}

arrow::Result<std::vector<uint8_t>> ReferenceEncodeFor(const sniffer::FieldSpec& field,
                                                       const arrow::Array& array) {
  const uint64_t rows = static_cast<uint64_t>(array.length());
  std::vector<uint8_t> validity;
  if (array.null_count() != 0) {
    validity.resize(static_cast<size_t>(rows / 8U + (rows % 8U != 0)), 0);
  }
  std::shared_ptr<arrow::Scalar> base;
  std::vector<std::shared_ptr<arrow::Scalar>> values(static_cast<size_t>(array.length()));
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    if (!validity.empty()) {
      validity[static_cast<size_t>(row / 8)] |=
          static_cast<uint8_t>(1U << static_cast<uint32_t>(row % 8));
    }
    ARROW_ASSIGN_OR_RAISE(auto value, array.GetScalar(row));
    values[static_cast<size_t>(row)] = value;
    if (!base) {
      base = std::move(value);
    } else {
      ARROW_ASSIGN_OR_RAISE(const int order, sniffer::internal::CompareScalars(*value, *base));
      if (order < 0) {
        base = std::move(value);
      }
    }
  }
  if (!base) {
    return arrow::Status::Invalid("reference FOR test requires a non-null value");
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t base_bits, ReferenceIntegralBits(*base));
  std::vector<uint64_t> deltas(values.size(), 0);
  uint64_t maximum_delta = 0;
  for (size_t row = 0; row < values.size(); ++row) {
    if (values[row]) {
      ARROW_ASSIGN_OR_RAISE(const uint64_t bits, ReferenceIntegralBits(*values[row]));
      deltas[row] = bits - base_bits;
      maximum_delta = std::max(maximum_delta, deltas[row]);
    }
  }
  const uint8_t bit_width = static_cast<uint8_t>(std::bit_width(maximum_delta));
  ARROW_ASSIGN_OR_RAISE(const uint64_t total_bits,
                        sniffer::internal::CheckedMultiply(rows, bit_width));
  ARROW_ASSIGN_OR_RAISE(const uint64_t padded_bits,
                        sniffer::internal::CheckedAdd(total_bits, uint64_t{7}));
  std::vector<uint8_t> packed(static_cast<size_t>(padded_bits / 8U), 0);
  for (uint64_t row = 0; row < deltas.size(); ++row) {
    for (uint32_t bit = 0; bit < bit_width; ++bit) {
      if (((deltas[static_cast<size_t>(row)] >> bit) & 1U) != 0) {
        const uint64_t position = row * bit_width + bit;
        packed[static_cast<size_t>(position / 8U)] |=
            static_cast<uint8_t>(1U << static_cast<uint32_t>(position % 8U));
      }
    }
  }
  ARROW_ASSIGN_OR_RAISE(const auto base_bytes, sniffer::internal::SerializeScalar(field, *base));
  sniffer::internal::ByteWriter payload;
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

TEST(SnifferCoreTest, TypedArrayHashMatchesScalarReference) {
  const auto data = MakeAllTypesBatch();
  constexpr std::array<uint64_t, 2> kSeeds = {0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL};
  for (size_t column = 0; column < data.table_schema.fields.size(); ++column) {
    const auto& field = data.table_schema.fields[column];
    const auto& array = *data.batch->column(static_cast<int>(column));
    for (int64_t row = 0; row < array.length(); ++row) {
      if (array.IsNull(row)) {
        continue;
      }
      const auto scalar = ValueOrThrow(array.GetScalar(row), "get scalar hash reference");
      std::array<uint64_t, 2> expected_hashes{};
      for (size_t seed_index = 0; seed_index < kSeeds.size(); ++seed_index) {
        const uint64_t seed = kSeeds[seed_index];
        const auto expected = ValueOrThrow(sniffer::internal::HashScalar(field, *scalar, seed),
                                           "hash scalar reference");
        expected_hashes[seed_index] = expected;
        const auto actual = ValueOrThrow(sniffer::internal::HashArrayValue(field, array, row, seed),
                                         "hash typed array value");
        EXPECT_TRUE(actual == expected) << "typed array hash matches scalar byte format";
      }
      const auto pair = ValueOrThrow(
          sniffer::internal::HashArrayValuePair(field, array, row, kSeeds[0], kSeeds[1]),
          "hash typed array value pair");
      EXPECT_TRUE(pair.first == expected_hashes[0] && pair.second == expected_hashes[1])
          << "paired typed array hash matches independent scalar hashes";
    }
  }
}

TEST(SnifferCoreTest, SortedPrimaryStatisticsFastPathMatchesGenericIndex) {
  const sniffer::TableSchema schema{
      1, {{1, "id", arrow::int64(), false, nullptr}, {2, "value", arrow::int32(), true, nullptr}}};
  const auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "create sorted index schema");
  const auto ids = BuildArray<arrow::Int64Builder, int64_t>({-5, -5, 0, 7, 9, 9});
  const auto values =
      BuildArray<arrow::Int32Builder, int32_t>({4, std::nullopt, -3, 8, std::nullopt, 2});
  const auto batch = arrow::RecordBatch::Make(arrow_schema, ids->length(), {ids, values});
  sniffer::LayoutPolicy layout;
  layout.sort_key_field_ids = {1};
  layout.statistics_field_ids = {1, 2};
  layout.bloom_field_ids = {2};

  std::vector<std::shared_ptr<arrow::Scalar>> previous_key;
  RequireOk(sniffer::internal::ValidateAndUpdateSortOrder(schema, layout, *batch, &previous_key),
            "validate sorted index input");
  const auto generic =
      ValueOrThrow(sniffer::internal::BuildRowGroupIndex(schema, layout, *batch, false),
                   "build generic row-group index");
  const auto optimized =
      ValueOrThrow(sniffer::internal::BuildRowGroupIndex(schema, layout, *batch, true),
                   "build sorted-primary row-group index");
  const auto generic_bytes = ValueOrThrow(sniffer::internal::SerializeIndexBlock(schema, generic),
                                          "serialize generic row-group index");
  const auto optimized_bytes =
      ValueOrThrow(sniffer::internal::SerializeIndexBlock(schema, optimized),
                   "serialize sorted-primary row-group index");
  EXPECT_TRUE(optimized_bytes == generic_bytes)
      << "sorted-primary statistics fast path preserves index bytes";

  const sniffer::TableSchema floating_schema{2, {{3, "key", arrow::float64(), false, nullptr}}};
  const auto floating_arrow_schema =
      ValueOrThrow(floating_schema.ToArrowSchema(), "create floating sorted index schema");
  const auto keys = BuildArray<arrow::DoubleBuilder, double>({-0.0, 0.0, 1.0});
  const auto floating_batch =
      arrow::RecordBatch::Make(floating_arrow_schema, keys->length(), {keys});
  sniffer::LayoutPolicy floating_layout;
  floating_layout.sort_key_field_ids = {3};
  floating_layout.statistics_field_ids = {3};
  previous_key.clear();
  RequireOk(sniffer::internal::ValidateAndUpdateSortOrder(floating_schema, floating_layout,
                                                          *floating_batch, &previous_key),
            "validate floating sorted index input");
  const auto floating_generic =
      ValueOrThrow(sniffer::internal::BuildRowGroupIndex(floating_schema, floating_layout,
                                                         *floating_batch, false),
                   "build generic floating row-group index");
  const auto floating_optimized =
      ValueOrThrow(sniffer::internal::BuildRowGroupIndex(floating_schema, floating_layout,
                                                         *floating_batch, true),
                   "build floating fallback row-group index");
  EXPECT_TRUE(
      ValueOrThrow(sniffer::internal::SerializeIndexBlock(floating_schema, floating_optimized),
                   "serialize floating fallback index") ==
      ValueOrThrow(sniffer::internal::SerializeIndexBlock(floating_schema, floating_generic),
                   "serialize generic floating index"))
      << "floating sort-key statistics preserve signed-zero index bytes";
}

TEST(SnifferCoreTest, TypedStatisticsMatchScalarReference) {
  const auto expect_matches = [](const sniffer::TableSchema& schema,
                                 const std::shared_ptr<arrow::RecordBatch>& batch) {
    sniffer::LayoutPolicy layout;
    for (const auto& field : schema.fields) {
      layout.statistics_field_ids.push_back(field.field_id);
    }
    const auto actual = ValueOrThrow(sniffer::internal::BuildRowGroupIndex(schema, layout, *batch),
                                     "build typed statistics index");
    sniffer::internal::RowGroupIndex expected;
    for (size_t column = 0; column < schema.fields.size(); ++column) {
      const auto& field = schema.fields[column];
      const auto& array = *batch->column(static_cast<int>(column));
      sniffer::internal::StatisticsMeta statistics;
      statistics.field_id = field.field_id;
      statistics.null_count = static_cast<uint64_t>(array.null_count());
      for (int64_t row = 0; row < array.length(); ++row) {
        if (array.IsNull(row)) {
          continue;
        }
        const auto value = ValueOrThrow(array.GetScalar(row), "get statistics reference scalar");
        if (sniffer::internal::ScalarHasNaN(*value)) {
          statistics.min.reset();
          statistics.max.reset();
          break;
        }
        if (!statistics.min) {
          statistics.min = value;
          statistics.max = value;
          continue;
        }
        if (ValueOrThrow(sniffer::internal::CompareScalars(*value, *statistics.min),
                         "compare statistics reference minimum") < 0) {
          statistics.min = value;
        }
        if (ValueOrThrow(sniffer::internal::CompareScalars(*value, *statistics.max),
                         "compare statistics reference maximum") > 0) {
          statistics.max = value;
        }
      }
      expected.statistics.push_back(std::move(statistics));
    }
    const auto actual_bytes = ValueOrThrow(sniffer::internal::SerializeIndexBlock(schema, actual),
                                           "serialize typed statistics index");
    const auto expected_bytes =
        ValueOrThrow(sniffer::internal::SerializeIndexBlock(schema, expected),
                     "serialize scalar-reference statistics index");
    EXPECT_TRUE(actual_bytes == expected_bytes)
        << "typed statistics preserve scalar-reference index bytes";
  };

  const auto all_types = MakeAllTypesBatch();
  expect_matches(all_types.table_schema, all_types.batch);

  const sniffer::TableSchema nan_schema{1, {{1, "value", arrow::float64(), true, nullptr}}};
  const auto nan_arrow_schema = ValueOrThrow(nan_schema.ToArrowSchema(), "create NaN schema");
  const auto nan_values = BuildArray<arrow::DoubleBuilder, double>(
      {1.0, std::numeric_limits<double>::quiet_NaN(), std::nullopt, -2.0});
  expect_matches(nan_schema,
                 arrow::RecordBatch::Make(nan_arrow_schema, nan_values->length(), {nan_values}));
}

TEST(SnifferCoreTest, PlainVariableBulkCopyMatchesReference) {
  const auto strings = BuildStringArray({"discard", "alpha", "beta", "gamma", "tail"});
  const auto sliced = strings->Slice(1, 3);
  const sniffer::FieldSpec string_field{1, "text", arrow::utf8(), false, nullptr};
  const auto expected =
      ValueOrThrow(ReferenceEncodePlainVariable(*sliced), "reference sliced Plain string");
  const auto actual = ValueOrThrow(sniffer::internal::EncodePlain(string_field, *sliced),
                                   "bulk sliced Plain string");
  EXPECT_TRUE(actual == expected) << "bulk Plain string bytes match row-wise reference";

  const auto binary = BuildBinaryArray(
      {std::vector<uint8_t>{0, 1}, std::nullopt,
       std::vector<uint8_t>{static_cast<uint8_t>('a'), 0, static_cast<uint8_t>('b')},
       std::vector<uint8_t>{}, std::vector<uint8_t>{0xFF}});
  const sniffer::FieldSpec binary_field{2, "bytes", arrow::binary(), true, nullptr};
  const auto nullable_expected =
      ValueOrThrow(ReferenceEncodePlainVariable(*binary), "reference nullable Plain binary");
  const auto nullable_actual =
      ValueOrThrow(sniffer::internal::EncodePlain(binary_field, *binary), "nullable Plain binary");
  EXPECT_TRUE(nullable_actual == nullable_expected)
      << "nullable Plain binary bytes match row-wise reference";
}

TEST(SnifferCoreTest, PlainSelectedTypedDecodeMatchesReference) {
  const auto data = MakeAllTypesBatch();
  const std::vector<uint64_t> selection = {0, 2, 4};
  for (size_t column = 0; column < data.table_schema.fields.size(); ++column) {
    const auto& field = data.table_schema.fields[column];
    const auto& source = data.batch->column(static_cast<int>(column));
    const auto payload = ValueOrThrow(sniffer::internal::EncodePlain(field, *source),
                                      "encode selected Plain reference payload");
    sniffer::internal::ColumnChunkMeta chunk;
    chunk.field_id = field.field_id;
    chunk.physical_type =
        ValueOrThrow(sniffer::internal::PhysicalTypeFor(*field.type), "selected physical type");
    chunk.encoding_id = sniffer::internal::kPlainEncodingId;
    chunk.row_count = static_cast<uint64_t>(source->length());
    chunk.null_count = static_cast<uint64_t>(source->null_count());
    chunk.length = static_cast<uint64_t>(payload.size());

    const auto actual =
        ValueOrThrow(sniffer::internal::DecodePlainSelected(field, chunk, payload, selection),
                     "decode selected typed Plain values");
    auto expected_builder =
        ValueOrThrow(arrow::MakeBuilder(field.type), "make selected Plain reference builder");
    for (const uint64_t row : selection) {
      const auto value = ValueOrThrow(source->GetScalar(static_cast<int64_t>(row)),
                                      "get selected Plain reference scalar");
      RequireOk(expected_builder->AppendScalar(*value), "append selected Plain reference scalar");
    }
    std::shared_ptr<arrow::Array> expected;
    RequireOk(expected_builder->Finish(&expected), "finish selected Plain reference");
    EXPECT_TRUE(actual->Equals(expected))
        << "typed selected Plain decode matches scalar reference for " + field.name;

    const std::vector<uint64_t> duplicate = {1, 1};
    EXPECT_TRUE(!sniffer::internal::DecodePlainSelected(field, chunk, payload, duplicate).ok())
        << "selected Plain decode rejects duplicate rows";
  }

  const auto& bool_field = data.table_schema.fields.front();
  const auto& bool_source = data.batch->column(0);
  auto corrupted = ValueOrThrow(sniffer::internal::EncodePlain(bool_field, *bool_source),
                                "encode selected Plain bool corruption payload");
  sniffer::internal::ByteReader header(corrupted);
  const uint64_t validity_length =
      ValueOrThrow(header.ReadU64(), "read selected Plain bool validity length");
  const uint64_t offsets_length =
      ValueOrThrow(header.ReadU64(), "read selected Plain bool offsets length");
  static_cast<void>(ValueOrThrow(header.ReadU64(), "read selected Plain bool values length"));
  const size_t values_offset = static_cast<size_t>(24U + validity_length + offsets_length);
  corrupted.at(values_offset) = 2;
  sniffer::internal::ColumnChunkMeta bool_chunk;
  bool_chunk.field_id = bool_field.field_id;
  bool_chunk.physical_type = sniffer::internal::PhysicalTypeId::kBool;
  bool_chunk.encoding_id = sniffer::internal::kPlainEncodingId;
  bool_chunk.row_count = static_cast<uint64_t>(bool_source->length());
  bool_chunk.null_count = static_cast<uint64_t>(bool_source->null_count());
  bool_chunk.length = static_cast<uint64_t>(corrupted.size());
  const std::vector<uint64_t> first_row = {0};
  EXPECT_TRUE(
      !sniffer::internal::DecodePlainSelected(bool_field, bool_chunk, corrupted, first_row).ok())
      << "selected Plain decode rejects non-canonical boolean values";
}

TEST(SnifferCoreTest, TypedRleMatchesScalarReference) {
  const auto data = MakeAllTypesBatch();
  for (size_t index = 0; index < 9; ++index) {
    const auto& field = data.table_schema.fields[index];
    const auto& array = *data.batch->column(static_cast<int>(index));
    const auto expected = ValueOrThrow(ReferenceEncodeRle(field, array), "reference RLE encode");
    const auto actual = ValueOrThrow(
        sniffer::internal::EncodeNonPlain(sniffer::internal::kRleEncodingId, field, array),
        "typed RLE encode");
    EXPECT_TRUE(actual == expected) << "typed RLE bytes match scalar reference for " + field.name;

    sniffer::internal::ColumnChunkMeta chunk;
    chunk.field_id = field.field_id;
    chunk.physical_type =
        ValueOrThrow(sniffer::internal::PhysicalTypeFor(*field.type), "RLE physical type");
    chunk.encoding_id = sniffer::internal::kRleEncodingId;
    chunk.row_count = static_cast<uint64_t>(array.length());
    chunk.null_count = static_cast<uint64_t>(array.null_count());
    chunk.length = static_cast<uint64_t>(actual.size());
    const auto decoded = ValueOrThrow(sniffer::internal::DecodeNonPlain(field, chunk, actual),
                                      "typed RLE full decode");
    EXPECT_TRUE(decoded->Equals(array)) << "typed RLE full decode matches source for " + field.name;

    const std::vector<uint64_t> selection = {0, 2, 4};
    const auto selected =
        ValueOrThrow(sniffer::internal::DecodeNonPlain(field, chunk, actual, &selection),
                     "typed RLE selected decode");
    auto expected_builder =
        ValueOrThrow(arrow::MakeBuilder(field.type), "make RLE selection reference");
    for (const uint64_t row : selection) {
      const auto value =
          ValueOrThrow(array.GetScalar(static_cast<int64_t>(row)), "RLE reference scalar");
      RequireOk(expected_builder->AppendScalar(*value), "append RLE reference scalar");
    }
    std::shared_ptr<arrow::Array> selected_expected;
    RequireOk(expected_builder->Finish(&selected_expected), "finish RLE reference");
    EXPECT_TRUE(selected->Equals(selected_expected))
        << "typed RLE selected decode matches scalar reference for " + field.name;
  }

  sniffer::FieldSpec repeated_field{1, "repeated", arrow::int32(), true, nullptr};
  const auto repeated = BuildArray<arrow::Int32Builder, int32_t>(
      {-7, -7, -7, std::nullopt, std::nullopt, 42, 42, -1, -1, -1});
  const auto expected =
      ValueOrThrow(ReferenceEncodeRle(repeated_field, *repeated), "reference repeated RLE encode");
  const auto actual =
      ValueOrThrow(sniffer::internal::EncodeNonPlain(sniffer::internal::kRleEncodingId,
                                                     repeated_field, *repeated),
                   "typed repeated RLE encode");
  EXPECT_TRUE(actual == expected) << "typed RLE coalescing bytes match scalar reference";
}

TEST(SnifferCoreTest, TypedForMatchesScalarReference) {
  const auto data = MakeAllTypesBatch();
  constexpr std::array<size_t, 9> kColumns = {1, 2, 3, 4, 5, 6, 7, 8, 11};
  for (const size_t index : kColumns) {
    const auto& field = data.table_schema.fields[index];
    const auto& array = *data.batch->column(static_cast<int>(index));
    const auto expected = ValueOrThrow(ReferenceEncodeFor(field, array), "reference FOR encode");
    const auto actual =
        ValueOrThrow(sniffer::internal::EncodeNonPlain(
                         static_cast<uint16_t>(sniffer::EncodingKind::kForBitpack), field, array),
                     "typed FOR encode");
    EXPECT_TRUE(actual == expected) << "typed FOR bytes match scalar reference";
  }
}

TEST(SnifferCoreTest, ForBitpackAllBitWidthsMatchReference) {
  const sniffer::FieldSpec field{1, "value", arrow::uint64(), true, nullptr};
  for (uint32_t bit_width = 0; bit_width <= 64; ++bit_width) {
    const uint64_t maximum = bit_width == 64
                                 ? std::numeric_limits<uint64_t>::max()
                                 : (bit_width == 0 ? 0 : (uint64_t{1} << bit_width) - 1U);
    const auto array = BuildArray<arrow::UInt64Builder, uint64_t>(
        {uint64_t{0}, maximum, std::nullopt, maximum / 3U, maximum});
    const auto expected =
        ValueOrThrow(ReferenceEncodeFor(field, *array), "reference FOR bit-width encode");
    const auto actual = ValueOrThrow(
        sniffer::internal::EncodeNonPlain(sniffer::internal::kForBitpackEncodingId, field, *array),
        "typed FOR bit-width encode");
    EXPECT_TRUE(actual == expected) << "FOR payload differs at bit width " << bit_width;

    sniffer::internal::ColumnChunkMeta chunk;
    chunk.field_id = field.field_id;
    chunk.physical_type = sniffer::internal::PhysicalTypeId::kUInt64;
    chunk.encoding_id = sniffer::internal::kForBitpackEncodingId;
    chunk.row_count = static_cast<uint64_t>(array->length());
    chunk.null_count = static_cast<uint64_t>(array->null_count());
    chunk.length = static_cast<uint64_t>(actual.size());
    const auto decoded = ValueOrThrow(sniffer::internal::DecodeNonPlain(field, chunk, actual),
                                      "decode FOR bit-width payload");
    EXPECT_TRUE(decoded->Equals(array)) << "FOR round-trip differs at bit width " << bit_width;
  }
}

TEST(SnifferCoreTest, TypedDictionaryMatchesScalarReference) {
  const auto data = MakeAllTypesBatch();
  const std::array<size_t, 10> field_indexes = {1, 2, 3, 4, 5, 6, 7, 8, 12, 13};
  for (const size_t index : field_indexes) {
    const auto& field = data.table_schema.fields[index];
    const auto& array = *data.batch->column(static_cast<int>(index));
    const auto expected =
        ValueOrThrow(ReferenceEncodeDictionary(field, array), "reference Dictionary encode");
    const auto actual = ValueOrThrow(
        sniffer::internal::EncodeNonPlain(sniffer::internal::kDictionaryEncodingId, field, array),
        "typed Dictionary encode");
    EXPECT_TRUE(actual == expected)
        << "typed Dictionary bytes match scalar reference for " + field.name;

    sniffer::internal::ColumnChunkMeta chunk;
    chunk.field_id = field.field_id;
    chunk.physical_type =
        ValueOrThrow(sniffer::internal::PhysicalTypeFor(*field.type), "Dictionary physical type");
    chunk.encoding_id = sniffer::internal::kDictionaryEncodingId;
    chunk.row_count = static_cast<uint64_t>(array.length());
    chunk.null_count = static_cast<uint64_t>(array.null_count());
    chunk.length = static_cast<uint64_t>(actual.size());
    const auto decoded = ValueOrThrow(sniffer::internal::DecodeNonPlain(field, chunk, actual),
                                      "typed Dictionary full decode");
    EXPECT_TRUE(decoded->Equals(array))
        << "typed Dictionary full decode matches source for " + field.name;

    const std::vector<uint64_t> selection = {0, 2, 4};
    const auto selected =
        ValueOrThrow(sniffer::internal::DecodeNonPlain(field, chunk, actual, &selection),
                     "typed Dictionary selected decode");
    auto expected_builder =
        ValueOrThrow(arrow::MakeBuilder(field.type), "make Dictionary selection reference");
    for (const uint64_t row : selection) {
      const auto value =
          ValueOrThrow(array.GetScalar(static_cast<int64_t>(row)), "Dictionary reference scalar");
      RequireOk(expected_builder->AppendScalar(*value), "append Dictionary reference scalar");
    }
    std::shared_ptr<arrow::Array> selected_expected;
    RequireOk(expected_builder->Finish(&selected_expected), "finish Dictionary reference");
    EXPECT_TRUE(selected->Equals(selected_expected))
        << "typed Dictionary selected decode matches scalar reference for " + field.name;
  }
}

TEST(SnifferCoreTest, NonPlainSelectionValidation) {
  const sniffer::FieldSpec field{1, "value", arrow::int64(), true, nullptr};
  const auto array =
      BuildArray<arrow::Int64Builder, int64_t>({7, 7, std::nullopt, 9, 10, 10, 10, 12});
  constexpr std::array<uint16_t, 3> kEncodings = {sniffer::internal::kDictionaryEncodingId,
                                                  sniffer::internal::kRleEncodingId,
                                                  sniffer::internal::kForBitpackEncodingId};
  for (const uint16_t encoding : kEncodings) {
    const auto payload = ValueOrThrow(sniffer::internal::EncodeNonPlain(encoding, field, *array),
                                      "encode selection validation payload");
    sniffer::internal::ColumnChunkMeta chunk;
    chunk.field_id = field.field_id;
    chunk.physical_type = sniffer::internal::PhysicalTypeId::kInt64;
    chunk.encoding_id = encoding;
    chunk.row_count = static_cast<uint64_t>(array->length());
    chunk.null_count = static_cast<uint64_t>(array->null_count());
    chunk.length = static_cast<uint64_t>(payload.size());

    const std::vector<uint64_t> duplicate = {1, 1};
    const std::vector<uint64_t> descending = {3, 2};
    const std::vector<uint64_t> out_of_bounds = {0, chunk.row_count};
    EXPECT_TRUE(!sniffer::internal::DecodeNonPlain(field, chunk, payload, &duplicate).ok())
        << "non-Plain decode rejects duplicate selection rows";
    EXPECT_TRUE(!sniffer::internal::DecodeNonPlain(field, chunk, payload, &descending).ok())
        << "non-Plain decode rejects descending selection rows";
    EXPECT_TRUE(!sniffer::internal::DecodeNonPlain(field, chunk, payload, &out_of_bounds).ok())
        << "non-Plain decode rejects out-of-bounds selection rows";
  }
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

void WriteSegmentWithPolicy(const std::filesystem::path& path, const sniffer::TableSchema& schema,
                            const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                            const sniffer::LayoutPolicy& policy) {
  auto writer =
      ValueOrThrow(sniffer::SegmentWriter::Open(path.string(), schema, policy), "open writer");
  for (const auto& batch : batches) {
    RequireOk(writer->Append(batch), "append batch");
  }
  RequireOk(writer->Finish(), "finish segment");
}

std::vector<std::shared_ptr<arrow::RecordBatch>> CollectScan(arrow::RecordBatchIterator iterator) {
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  while (true) {
    auto batch = ValueOrThrow(iterator.Next(), "advance scan iterator");
    if (!batch) {
      break;
    }
    batches.push_back(std::move(batch));
  }
  return batches;
}

std::vector<int64_t> CollectInt64Column(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches, int column) {
  std::vector<int64_t> values;
  for (const auto& batch : batches) {
    const auto& array = static_cast<const arrow::Int64Array&>(*batch->column(column));
    for (int64_t row = 0; row < array.length(); ++row) {
      EXPECT_TRUE(array.IsValid(row)) << "collected int64 value is non-null";
      values.push_back(array.Value(row));
    }
  }
  return values;
}

TestData MakeScanBatch() {
  sniffer::TableSchema schema{21,
                              {{1, "key", arrow::int64(), false, nullptr},
                               {2, "category", arrow::utf8(), true, nullptr},
                               {3, "score", arrow::int32(), true, nullptr},
                               {4, "payload", arrow::binary(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "scan schema");
  arrow::Int64Builder key_builder;
  arrow::StringBuilder category_builder;
  arrow::Int32Builder score_builder;
  arrow::BinaryBuilder payload_builder;
  for (int64_t row = 0; row < 30; ++row) {
    RequireOk(key_builder.Append(row), "append scan key");
    if (row % 6 == 0) {
      RequireOk(category_builder.AppendNull(), "append category null");
    } else {
      RequireOk(category_builder.Append(row % 2 == 0 ? "even" : "odd"), "append category");
    }
    if (row % 7 == 0) {
      RequireOk(score_builder.AppendNull(), "append score null");
    } else {
      RequireOk(score_builder.Append(static_cast<int32_t>(row * 10)), "append score");
    }
    const std::string payload = "p" + std::to_string(row);
    RequireOk(payload_builder.Append(reinterpret_cast<const uint8_t*>(payload.data()),
                                     static_cast<int32_t>(payload.size())),
              "append payload");
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(4);
  RequireOk(key_builder.Finish(&columns[0]), "finish scan keys");
  RequireOk(category_builder.Finish(&columns[1]), "finish categories");
  RequireOk(score_builder.Finish(&columns[2]), "finish scores");
  RequireOk(payload_builder.Finish(&columns[3]), "finish payloads");
  auto batch = arrow::RecordBatch::Make(arrow_schema, 30, std::move(columns));
  RequireOk(batch->ValidateFull(), "validate scan batch");
  return {std::move(schema), std::move(batch)};
}

sniffer::LayoutPolicy ScanLayout() {
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 5;
  policy.sort_key_field_ids = {1};
  policy.statistics_field_ids = {1, 2, 3};
  policy.bloom_field_ids = {2};
  return policy;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  EXPECT_TRUE(stream.is_open()) << "open test file for reading";
  const auto length = stream.tellg();
  EXPECT_TRUE(length >= 0) << "determine test file length";
  stream.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    EXPECT_TRUE(static_cast<bool>(stream)) << "read test file";
  }
  return bytes;
}

void WriteFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(stream.is_open()) << "open test file for writing";
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  EXPECT_TRUE(static_cast<bool>(stream)) << "write test file";
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

TEST(SnifferCoreTest, ProductionCrc32cMatchesBitwiseReference) {
  std::vector<uint8_t> bytes(8193);
  for (size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<uint8_t>((index * 131U + index / 7U) & 0xFFU);
  }
  constexpr std::array<size_t, 15> kLengths = {0,  1,  2,  3,  7,   8,    9,   15,
                                               16, 17, 31, 32, 255, 4096, 8193};
  for (const size_t length : kLengths) {
    const auto input = std::span<const uint8_t>(bytes.data(), length);
    EXPECT_EQ(sniffer::internal::Crc32c(input), Crc32c(input))
        << "CRC32C differs at length " << length;

    const size_t split = length / 3U;
    uint32_t production = sniffer::internal::Crc32c(input.first(split));
    production = sniffer::internal::Crc32c(input.subspan(split), production);
    uint32_t reference = Crc32c(input.first(split));
    reference = Crc32c(input.subspan(split), reference);
    EXPECT_EQ(production, reference) << "incremental CRC32C differs at length " << length;
  }
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (uint32_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8U);
}

uint16_t FirstChunkEncoding(const std::filesystem::path& path, size_t schema_descriptor_bytes) {
  const auto bytes = ReadFile(path);
  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  constexpr size_t kEncodingDescriptors = 61;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry =
      footer_offset + kPrefix + schema_descriptor_bytes + kEncodingDescriptors + kRowGroupHeader;
  return ReadU16(bytes, chunk_entry + 6);
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

TEST(SnifferCoreTest, RoundTripAllTypesAndMultipleRowGroups) {
  const auto data = MakeAllTypesBatch();
  TempFile file("all_types.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch}, 2);

  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open reader");
  EXPECT_TRUE(reader->num_row_groups() == 3) << "expected three row groups";
  EXPECT_TRUE(reader->schema().schema_version == 7) << "schema version round-trip";
  EXPECT_TRUE(reader->schema().fields.size() == data.table_schema.fields.size())
      << "field count round-trip";
  for (size_t index = 0; index < data.table_schema.fields.size(); ++index) {
    const auto& expected = data.table_schema.fields[index];
    const auto& actual = reader->schema().fields[index];
    EXPECT_TRUE(actual.field_id == expected.field_id) << "field ID round-trip";
    EXPECT_TRUE(actual.name == expected.name) << "field name round-trip";
    EXPECT_TRUE(actual.nullable == expected.nullable) << "nullable round-trip";
    EXPECT_TRUE(actual.type->Equals(expected.type)) << "field type round-trip";
  }
  auto batches = ValueOrThrow(reader->ReadAll(), "read all row groups");
  EXPECT_TRUE(batches.size() == 3) << "read three batches";
  int64_t offset = 0;
  for (const auto& batch : batches) {
    EXPECT_TRUE(batch->Equals(*data.batch->Slice(offset, batch->num_rows())))
        << "row-group batch equals input slice";
    for (int column = 0; column < batch->num_columns(); ++column) {
      const auto metadata = batch->schema()->field(column)->metadata();
      EXPECT_TRUE(metadata != nullptr) << "field ID metadata exists";
      auto field_id = metadata->Get(sniffer::kFieldIdMetadataKey);
      EXPECT_TRUE(field_id.ok()) << "field ID metadata lookup succeeds";
      EXPECT_TRUE(field_id.ValueUnsafe() ==
                  std::to_string(data.table_schema.fields[static_cast<size_t>(column)].field_id))
          << "field ID metadata value round-trip";
    }
    offset += batch->num_rows();
  }
  RequireOk(reader->VerifyFileChecksum(), "verify whole-file checksum");
}

TEST(SnifferCoreTest, EmptyBatchRoundTrip) {
  sniffer::TableSchema schema{1, {{1, "value", arrow::int32(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "empty schema");
  auto values = BuildArray<arrow::Int32Builder, int32_t>({});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 0, {values});
  TempFile file("empty.seg");
  WriteSegment(file.path(), schema, {batch}, 2);

  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open empty reader");
  EXPECT_TRUE(reader->num_row_groups() == 0) << "empty segment has no physical row group";
  auto batches = ValueOrThrow(reader->ReadAll(), "read empty segment");
  EXPECT_TRUE(batches.size() == 1 && batches[0]->num_rows() == 0)
      << "empty segment yields one schema-bearing empty batch";
  EXPECT_TRUE(batches[0]->schema()->field(0)->type()->Equals(arrow::int32()))
      << "empty batch schema is retained";
}

TEST(SnifferCoreTest, DeterministicOutput) {
  const auto data = MakeAllTypesBatch();
  TempFile first("deterministic_a.seg");
  TempFile second("deterministic_b.seg");
  WriteSegment(first.path(), data.table_schema, {data.batch}, 3);
  WriteSegment(second.path(), data.table_schema, {data.batch}, 3);
  EXPECT_TRUE(ReadFile(first.path()) == ReadFile(second.path()))
      << "same input and policy produce identical files";
}

TEST(SnifferCoreTest, DeterministicRandomizedRoundTrip) {
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
    EXPECT_TRUE(output->Equals(*batch->Slice(offset, output->num_rows())))
        << "randomized output equals input slice";
    offset += output->num_rows();
  }
  EXPECT_TRUE(offset == kRows) << "randomized round-trip preserves every row";
}

TEST(SnifferCoreTest, HeaderCorruptionFailsOpen) {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_header.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes[0] ^= 0x01U;
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok()) << "bad header magic must fail Open";
}

TEST(SnifferCoreTest, UnsupportedVersionFailsOpen) {
  const auto data = MakeAllTypesBatch();
  TempFile file("unsupported_version.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  WriteU16(&bytes, 8, 2);
  WriteU32(&bytes, 28, Crc32c(std::span<const uint8_t>(bytes.data(), 28)));
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok() && reader.status().IsNotImplemented())
      << "unsupported format major version must fail explicitly";
}

TEST(SnifferCoreTest, FooterCorruptionFailsOpen) {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_footer.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  bytes[footer_offset] ^= 0x01U;
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok()) << "bad footer checksum must fail Open";
}

TEST(SnifferCoreTest, ChunkCorruptionFailsLazyReadAndFileVerify) {
  const auto data = MakeAllTypesBatch();
  TempFile file("bad_chunk.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes[32 + 24] ^= 0x01U;
  WriteFile(file.path(), bytes);

  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()),
                             "metadata-only open after chunk corruption");
  EXPECT_TRUE(!reader->ReadAll().ok()) << "chunk corruption must fail when chunk is read";
  EXPECT_TRUE(!reader->VerifyFileChecksum().ok())
      << "chunk corruption must fail whole-file verification";
}

TEST(SnifferCoreTest, InvalidChunkOffsetFailsOpen) {
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
  constexpr size_t kEncodingDescriptor = 61;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry = footer_offset + kPrefix + kFieldDescriptor + field_name_length +
                             kEncodingDescriptor + kRowGroupHeader;
  WriteU64(&bytes, chunk_entry + 24, std::numeric_limits<uint64_t>::max());
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok()) << "overflowing chunk offset must fail Open";
}

TEST(SnifferCoreTest, UnknownEncodingFailsOpen) {
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
  constexpr size_t kEncodingDescriptor = 61;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry = footer_offset + kPrefix + kFieldDescriptor + field_name_length +
                             kEncodingDescriptor + kRowGroupHeader;
  WriteU16(&bytes, chunk_entry + 6, 999);
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok() && reader.status().IsNotImplemented())
      << "unknown encoding must fail explicitly";
}

TEST(SnifferCoreTest, UnknownPhysicalTypeFailsOpen) {
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
  EXPECT_TRUE(!reader.ok() && reader.status().IsNotImplemented())
      << "unknown physical type must fail explicitly";
}

TEST(SnifferCoreTest, TruncationFailsOpen) {
  const auto data = MakeAllTypesBatch();
  TempFile file("truncated.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch});
  auto bytes = ReadFile(file.path());
  bytes.resize(bytes.size() - 7);
  WriteFile(file.path(), bytes);
  auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok()) << "truncated metadata must fail Open";
}

TEST(SnifferCoreTest, SchemaAndWriterStateValidation) {
  sniffer::TableSchema duplicate{
      1, {{1, "a", arrow::int32(), true, nullptr}, {1, "b", arrow::int64(), true, nullptr}}};
  EXPECT_TRUE(!duplicate.Validate().ok()) << "duplicate field IDs must fail";

  sniffer::TableSchema nested{1, {{1, "nested", arrow::list(arrow::int32()), true, nullptr}}};
  EXPECT_TRUE(nested.Validate().IsNotImplemented())
      << "nested type must fail explicitly in phase one";

  sniffer::TableSchema schema{1, {{1, "x", arrow::int32(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "writer schema");
  auto array = BuildArray<arrow::Int32Builder, int32_t>({1});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 1, {array});
  TempFile file("writer_state.seg");
  auto writer =
      ValueOrThrow(sniffer::SegmentWriter::Open(file.path().string(), schema), "open state writer");
  RequireOk(writer->Append(batch), "append state batch");
  RequireOk(writer->Finish(), "finish state writer");
  EXPECT_TRUE(!writer->Append(batch).ok()) << "Append after Finish must fail";
  EXPECT_TRUE(!writer->Finish().ok()) << "second Finish must fail";
}

TEST(SnifferCoreTest, IOPlanProjectionPredicatesLimitAndBatching) {
  const auto data = MakeScanBatch();
  TempFile file("scan_projection.seg");
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, ScanLayout());
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open scan file");

  sniffer::IOPlan plan;
  plan.projection_field_ids = {4, 1};
  plan.conjunctive_predicates = {
      {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("even")},
      {3, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int32Scalar>(100)}};
  plan.limit = 4;
  plan.output_batch_rows = 2;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto iterator = ValueOrThrow(reader->Scan(plan, metrics), "create projected scan");
  const auto batches = CollectScan(std::move(iterator));
  EXPECT_TRUE(batches.size() == 2) << "limit output is split into two batches";
  EXPECT_TRUE(CollectInt64Column(batches, 1) == std::vector<int64_t>({10, 16, 20, 22}))
      << "AND predicates and limit preserve logical row order";
  std::vector<std::string> payloads;
  for (const auto& batch : batches) {
    EXPECT_TRUE(batch->schema()->field(0)->name() == "payload" &&
                batch->schema()->field(1)->name() == "key")
        << "projection order is caller-defined";
    const auto& payload = static_cast<const arrow::BinaryArray&>(*batch->column(0));
    for (int64_t row = 0; row < payload.length(); ++row) {
      payloads.emplace_back(payload.GetView(row));
    }
  }
  EXPECT_TRUE(payloads == std::vector<std::string>({"p10", "p16", "p20", "p22"}))
      << "projection-only binary values are decoded for selected rows";

  arrow::BinaryBuilder reference_payload_builder;
  arrow::Int64Builder reference_key_builder;
  const auto full_batches = ValueOrThrow(reader->ReadAll(), "full decode for reference filter");
  uint64_t reference_rows = 0;
  for (const auto& full_batch : full_batches) {
    const auto& keys = static_cast<const arrow::Int64Array&>(*full_batch->column(0));
    const auto& categories = static_cast<const arrow::StringArray&>(*full_batch->column(1));
    const auto& scores = static_cast<const arrow::Int32Array&>(*full_batch->column(2));
    const auto& payload = static_cast<const arrow::BinaryArray&>(*full_batch->column(3));
    for (int64_t row = 0; row < full_batch->num_rows() && reference_rows < 4; ++row) {
      if (categories.IsValid(row) && categories.GetView(row) == "even" && scores.IsValid(row) &&
          scores.Value(row) >= 100) {
        RequireOk(reference_payload_builder.Append(payload.GetView(row)),
                  "append Arrow reference payload");
        RequireOk(reference_key_builder.Append(keys.Value(row)), "append Arrow reference key");
        ++reference_rows;
      }
    }
  }
  std::shared_ptr<arrow::Array> expected_payload;
  std::shared_ptr<arrow::Array> expected_key;
  RequireOk(reference_payload_builder.Finish(&expected_payload), "finish Arrow reference payload");
  RequireOk(reference_key_builder.Finish(&expected_key), "finish Arrow reference key");
  auto actual = ValueOrThrow(arrow::ConcatenateRecordBatches(batches), "concatenate scan result");
  auto expected = arrow::RecordBatch::Make(actual->schema(), 4, {expected_payload, expected_key});
  EXPECT_TRUE(actual->Equals(*expected))
      << "IOPlan result equals full decode plus Arrow reference filtering";
  EXPECT_TRUE(metrics->row_groups_considered == 5 && metrics->row_groups_pruned == 2)
      << "statistics prune early row groups before the limit is reached";
  EXPECT_TRUE(metrics->predicate_chunks_decoded == 6 && metrics->projection_chunks_decoded == 6 &&
              metrics->column_chunks_read == 12)
      << "predicate and projection chunks follow separate decode paths";
}

TEST(SnifferCoreTest, PredicateExecutionOrderIsDeterministic) {
  const auto data = MakeScanBatch();
  TempFile file("scan_resolved_plan.seg");
  WriteSegment(file.path(), data.table_schema, {data.batch}, 30);
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open resolved-plan file");

  sniffer::IOPlan plan;
  plan.projection_field_ids = {4, 1};
  plan.conjunctive_predicates = {
      {3, sniffer::Predicate::Op::kIsNotNull, nullptr},
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(8)},
      {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("even")},
      {1, sniffer::Predicate::Op::kLt, std::make_shared<arrow::Int64Scalar>(15)}};
  plan.output_batch_rows = 2;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  const auto batches = CollectScan(
      ValueOrThrow(reader->Scan(plan, metrics), "scan with deterministic predicate order"));

  EXPECT_TRUE(CollectInt64Column(batches, 1) == std::vector<int64_t>({8, 10}))
      << "compiled predicates retain field alignment and null comparison semantics";
  EXPECT_TRUE(!batches.empty() && batches.front()->schema()->field(0)->name() == "payload" &&
              batches.front()->schema()->field(1)->name() == "key")
      << "pre-resolved projection indices retain caller order";
  const auto repeated =
      CollectScan(ValueOrThrow(reader->Scan(plan, metrics), "repeat deterministic predicate scan"));
  EXPECT_TRUE(CollectInt64Column(repeated, 1) == std::vector<int64_t>({8, 10}))
      << "predicate planning is deterministic across scans";
}

TEST(SnifferCoreTest, BloomPrunesWithoutChunkReads) {
  const auto data = MakeScanBatch();
  TempFile file("scan_bloom.seg");
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, ScanLayout());
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open Bloom file");

  sniffer::IOPlan plan;
  plan.projection_field_ids = {1, 4};
  plan.conjunctive_predicates = {
      {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("absent")}};
  plan.output_batch_rows = 3;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto batches = CollectScan(ValueOrThrow(reader->Scan(plan, metrics), "create Bloom scan"));
  EXPECT_TRUE(batches.empty()) << "absent Bloom value produces no rows";
  EXPECT_TRUE(metrics->row_groups_considered == 6 && metrics->row_groups_pruned == 6)
      << "Bloom prunes every row group";
  EXPECT_TRUE(metrics->column_chunks_read == 0 && metrics->chunk_bytes_read == 0)
      << "Bloom pruning reads no data ColumnChunk";
}

TEST(SnifferCoreTest, SortKeyRangeAndEmptyProjection) {
  const auto data = MakeScanBatch();
  TempFile file("scan_sort_range.seg");
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, ScanLayout());
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open range file");

  sniffer::IOPlan range_plan;
  range_plan.projection_field_ids = {1};
  sniffer::SortKeyRange range;
  range.lower =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(12)};
  range.upper =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(18)};
  range_plan.sort_key_range = std::move(range);
  range_plan.output_batch_rows = 4;
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto batches =
      CollectScan(ValueOrThrow(reader->Scan(range_plan, metrics), "create sort-key range scan"));
  EXPECT_TRUE(CollectInt64Column(batches, 0) == std::vector<int64_t>({12, 13, 14, 15, 16, 17}))
      << "half-open sort-key range filters row boundaries exactly";
  EXPECT_TRUE(batches.size() == 2 && batches[0]->num_rows() == 4 && batches[1]->num_rows() == 2)
      << "sort-key range output obeys batch size across row groups";
  EXPECT_TRUE(metrics->row_groups_pruned == 4 && metrics->column_chunks_read == 2 &&
              metrics->predicate_chunks_decoded == 2 && metrics->projection_chunks_decoded == 0)
      << "sort-key index prunes four groups and reuses decoded key columns";

  sniffer::IOPlan empty_projection;
  empty_projection.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kLt, std::make_shared<arrow::Int64Scalar>(3)}};
  empty_projection.output_batch_rows = 2;
  auto empty_batches =
      CollectScan(ValueOrThrow(reader->Scan(empty_projection), "create empty-projection scan"));
  EXPECT_TRUE(empty_batches.size() == 2 && empty_batches[0]->num_columns() == 0 &&
              empty_batches[0]->num_rows() == 2 && empty_batches[1]->num_rows() == 1)
      << "empty projection retains filtered row counts and batching";
}

TEST(SnifferCoreTest, AllPredicateOperationsAndNulls) {
  const auto data = MakeScanBatch();
  TempFile file("scan_operators.seg");
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, ScanLayout());
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open operators");

  const auto scan_keys = [&reader](sniffer::Predicate predicate) {
    sniffer::IOPlan plan;
    plan.projection_field_ids = {1};
    plan.conjunctive_predicates = {std::move(predicate)};
    plan.output_batch_rows = 7;
    return CollectInt64Column(
        CollectScan(ValueOrThrow(reader->Scan(std::move(plan)), "create operator scan")), 0);
  };
  const auto key_value = [] { return std::make_shared<arrow::Int64Scalar>(10); };
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kEq, key_value()}) ==
              std::vector<int64_t>({10}))
      << "equality predicate";
  std::vector<int64_t> not_equal;
  for (int64_t value = 0; value < 30; ++value) {
    if (value != 10) {
      not_equal.push_back(value);
    }
  }
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kNe, key_value()}) == not_equal)
      << "not-equal predicate";
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kLt, key_value()}) ==
              std::vector<int64_t>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}))
      << "less-than predicate";
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kLe, key_value()}) ==
              std::vector<int64_t>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}))
      << "less-or-equal predicate";
  std::vector<int64_t> greater;
  std::vector<int64_t> greater_equal;
  for (int64_t value = 10; value < 30; ++value) {
    greater_equal.push_back(value);
    if (value > 10) {
      greater.push_back(value);
    }
  }
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kGt, key_value()}) == greater)
      << "greater-than predicate";
  EXPECT_TRUE(scan_keys({1, sniffer::Predicate::Op::kGe, key_value()}) == greater_equal)
      << "greater-or-equal predicate";
  EXPECT_TRUE(scan_keys({3, sniffer::Predicate::Op::kIsNull, nullptr}) ==
              std::vector<int64_t>({0, 7, 14, 21, 28}))
      << "IS NULL predicate";
  std::vector<int64_t> not_null;
  for (int64_t value = 0; value < 30; ++value) {
    if (value % 7 != 0) {
      not_null.push_back(value);
    }
  }
  EXPECT_TRUE(scan_keys({3, sniffer::Predicate::Op::kIsNotNull, nullptr}) == not_null)
      << "IS NOT NULL predicate";
}

TEST(SnifferCoreTest, TypedPredicatesAndProjectionAllTypes) {
  const auto data = MakeAllTypesBatch();
  TempFile file("typed_predicates_all_types.seg");
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 2;
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, policy);
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open typed predicates");

  for (size_t column = 0; column < data.table_schema.fields.size(); ++column) {
    const auto& field = data.table_schema.fields[column];
    const auto& source = data.batch->column(static_cast<int>(column));
    const auto target = ValueOrThrow(source->GetScalar(0), "typed predicate target");
    sniffer::IOPlan equal;
    equal.projection_field_ids = {field.field_id};
    equal.conjunctive_predicates = {{field.field_id, sniffer::Predicate::Op::kEq, target}};
    equal.output_batch_rows = 2;
    const auto batches =
        CollectScan(ValueOrThrow(reader->Scan(equal), "scan typed equality predicate"));
    arrow::ArrayVector actual_chunks;
    for (const auto& batch : batches) {
      actual_chunks.push_back(batch->column(0));
    }
    const auto actual =
        ValueOrThrow(arrow::Concatenate(actual_chunks), "concatenate typed predicate result");

    auto expected_builder = ValueOrThrow(arrow::MakeBuilder(field.type), "make reference builder");
    for (int64_t row = 0; row < source->length(); ++row) {
      if (source->IsNull(row)) {
        continue;
      }
      const auto value = ValueOrThrow(source->GetScalar(row), "get reference predicate value");
      if (ValueOrThrow(sniffer::internal::CompareScalars(*value, *target),
                       "compare reference predicate value") == 0) {
        RequireOk(expected_builder->AppendScalar(*value), "append reference predicate value");
      }
    }
    std::shared_ptr<arrow::Array> expected;
    RequireOk(expected_builder->Finish(&expected), "finish reference predicate result");
    EXPECT_TRUE(actual->Equals(expected))
        << "typed equality predicate and projection match reference";

    sniffer::IOPlan nulls;
    nulls.projection_field_ids = {field.field_id};
    nulls.conjunctive_predicates = {{field.field_id, sniffer::Predicate::Op::kIsNull, nullptr}};
    const auto null_batches =
        CollectScan(ValueOrThrow(reader->Scan(nulls), "scan typed null predicate"));
    EXPECT_TRUE(null_batches.size() == 1 && null_batches[0]->num_rows() == source->null_count() &&
                null_batches[0]->column(0)->null_count() == source->null_count())
        << "typed projection preserves selected null values";
  }

  sniffer::TableSchema nan_schema{1, {{1, "value", arrow::float64(), true, nullptr}}};
  const auto nan_arrow_schema = ValueOrThrow(nan_schema.ToArrowSchema(), "NaN predicate schema");
  const auto nan_values = BuildArray<arrow::DoubleBuilder, double>(
      {std::numeric_limits<double>::quiet_NaN(), 1.0, std::nullopt});
  TempFile nan_file("typed_nan_predicate.seg");
  WriteSegment(nan_file.path(), nan_schema,
               {arrow::RecordBatch::Make(nan_arrow_schema, 3, {nan_values})});
  auto nan_reader =
      ValueOrThrow(sniffer::SegmentReader::Open(nan_file.path().string()), "open NaN predicate");
  sniffer::IOPlan nan_not_equal;
  nan_not_equal.projection_field_ids = {1};
  nan_not_equal.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kNe,
       std::make_shared<arrow::DoubleScalar>(std::numeric_limits<double>::quiet_NaN())}};
  const auto nan_batches =
      CollectScan(ValueOrThrow(nan_reader->Scan(nan_not_equal), "scan NaN not-equal"));
  EXPECT_TRUE(nan_batches.size() == 1 && nan_batches[0]->num_rows() == 2)
      << "typed predicate preserves NaN not-equal semantics";
}

TEST(SnifferCoreTest, OptionalPhaseMetrics) {
  const auto data = MakeScanBatch();
  TempFile file("phase_metrics.seg");
  auto writer_metrics = std::make_shared<sniffer::WriterMetrics>();
  writer_metrics->encoding_nanoseconds = std::numeric_limits<uint64_t>::max();
  auto writer = ValueOrThrow(sniffer::SegmentWriter::Open(file.path().string(), data.table_schema,
                                                          ScanLayout(), writer_metrics),
                             "open metrics writer");
  RequireOk(writer->Append(data.batch), "append metrics batch");
  RequireOk(writer->Finish(), "finish metrics writer");
  EXPECT_TRUE(writer_metrics->encoding_nanoseconds != std::numeric_limits<uint64_t>::max() &&
              writer_metrics->encoding_nanoseconds + writer_metrics->index_nanoseconds +
                      writer_metrics->checksum_nanoseconds +
                      writer_metrics->file_write_nanoseconds >
                  0)
      << "optional writer metrics reset and record phase timings";

  auto reader_metrics = std::make_shared<sniffer::ReaderMetrics>();
  reader_metrics->metadata_parse_nanoseconds = std::numeric_limits<uint64_t>::max();
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string(), reader_metrics),
                             "open metrics reader");
  EXPECT_TRUE(reader_metrics->metadata_parse_nanoseconds != std::numeric_limits<uint64_t>::max() &&
              reader_metrics->envelope_io_nanoseconds + reader_metrics->metadata_parse_nanoseconds +
                      reader_metrics->index_parse_nanoseconds >
                  0 &&
              reader_metrics->file_handles_opened == 1)
      << "optional reader metrics reset and record open timings";

  sniffer::IOPlan plan;
  plan.projection_field_ids = {1};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(10)}};
  auto scan_metrics = std::make_shared<sniffer::ScanMetrics>();
  const auto batches =
      CollectScan(ValueOrThrow(reader->Scan(plan, scan_metrics), "scan with phase metrics"));
  EXPECT_TRUE(!batches.empty() && scan_metrics->predicate_nanoseconds +
                                          scan_metrics->decode_nanoseconds +
                                          scan_metrics->chunk_io_nanoseconds >
                                      0)
      << "scan metrics record execution phase timings";
  const auto all_batches = ValueOrThrow(reader->ReadAll(), "read all with retained file handle");
  RequireOk(reader->VerifyFileChecksum(), "verify with retained file handle");
  EXPECT_TRUE(!all_batches.empty() && reader_metrics->file_handles_opened == 1)
      << "reader reuses one file handle across open, scan, read-all, and checksum verification";
}

TEST(SnifferCoreTest, SequentialFallbackAndPlanValidation) {
  const auto data = MakeScanBatch();
  TempFile file("scan_fallback.seg");
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 5;
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, policy);
  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open fallback");

  sniffer::IOPlan fallback;
  fallback.projection_field_ids = {1};
  fallback.conjunctive_predicates = {
      {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("absent")}};
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto batches = CollectScan(ValueOrThrow(reader->Scan(fallback, metrics), "create fallback scan"));
  EXPECT_TRUE(batches.empty() && metrics->row_groups_pruned == 0 &&
              metrics->predicate_chunks_decoded == 6 && metrics->column_chunks_read == 6)
      << "missing indexes safely degrade to predicate-column sequential scan";

  sniffer::IOPlan wrong_type;
  wrong_type.projection_field_ids = {1};
  wrong_type.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kEq, std::make_shared<arrow::Int32Scalar>(1)}};
  EXPECT_TRUE(!reader->Scan(wrong_type).ok()) << "implicit predicate type conversion is rejected";

  sniffer::IOPlan duplicate_projection;
  duplicate_projection.projection_field_ids = {1, 1};
  EXPECT_TRUE(!reader->Scan(duplicate_projection).ok()) << "duplicate projection field is rejected";

  sniffer::IOPlan bad_batch_size;
  bad_batch_size.output_batch_rows = 0;
  EXPECT_TRUE(!reader->Scan(bad_batch_size).ok()) << "zero output batch size is rejected";

  sniffer::IOPlan zero_limit;
  zero_limit.projection_field_ids = {1};
  zero_limit.limit = 0;
  auto zero_metrics = std::make_shared<sniffer::ScanMetrics>();
  EXPECT_TRUE(CollectScan(ValueOrThrow(reader->Scan(zero_limit, zero_metrics), "scan zero limit"))
                  .empty() &&
              zero_metrics->row_groups_considered == 0)
      << "zero limit reads no row groups";

  sniffer::IOPlan unknown_projection;
  unknown_projection.projection_field_ids = {999};
  EXPECT_TRUE(!reader->Scan(unknown_projection).ok()) << "unknown projection field is rejected";

  sniffer::IOPlan missing_value;
  missing_value.conjunctive_predicates = {{1, sniffer::Predicate::Op::kEq, nullptr}};
  EXPECT_TRUE(!reader->Scan(missing_value).ok()) << "comparison predicate requires a value";

  sniffer::IOPlan null_test_value;
  null_test_value.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kIsNull, std::make_shared<arrow::Int64Scalar>(1)}};
  EXPECT_TRUE(!reader->Scan(null_test_value).ok()) << "null predicate rejects a comparison value";

  sniffer::IOPlan missing_sort_key;
  sniffer::SortKeyRange range;
  range.lower =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(1)};
  missing_sort_key.sort_key_range = std::move(range);
  EXPECT_TRUE(!reader->Scan(missing_sort_key).ok())
      << "sort-key range on an unsorted segment is rejected";
}

TEST(SnifferCoreTest, SortOrderAndIndexCorruptionValidation) {
  sniffer::TableSchema nullable_sort_schema{1, {{1, "key", arrow::int64(), true, nullptr}}};
  sniffer::LayoutPolicy invalid_layout;
  invalid_layout.sort_key_field_ids = {1};
  TempFile invalid_file("nullable_sort.seg");
  EXPECT_TRUE(!sniffer::SegmentWriter::Open(invalid_file.path().string(), nullable_sort_schema,
                                            invalid_layout)
                   .ok())
      << "nullable sort-key configuration is rejected";

  sniffer::TableSchema schema{1, {{1, "key", arrow::int64(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "sort validation schema");
  auto unsorted = BuildArray<arrow::Int64Builder, int64_t>({1, 3, 2});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 3, {unsorted});
  sniffer::LayoutPolicy sorted_layout;
  sorted_layout.target_row_group_rows = 10;
  sorted_layout.sort_key_field_ids = {1};
  TempFile unsorted_file("unsorted.seg");
  auto writer = ValueOrThrow(
      sniffer::SegmentWriter::Open(unsorted_file.path().string(), schema, sorted_layout),
      "open unsorted writer");
  EXPECT_TRUE(!writer->Append(batch).ok())
      << "unsorted input is rejected before row groups are written";

  TempFile cross_append_file("cross_append_unsorted.seg");
  auto cross_writer = ValueOrThrow(
      sniffer::SegmentWriter::Open(cross_append_file.path().string(), schema, sorted_layout),
      "open cross-append sort writer");
  auto first_values = BuildArray<arrow::Int64Builder, int64_t>({1, 3});
  auto second_values = BuildArray<arrow::Int64Builder, int64_t>({2, 4});
  RequireOk(cross_writer->Append(arrow::RecordBatch::Make(arrow_schema, 2, {first_values})),
            "append first sorted batch");
  EXPECT_TRUE(
      !cross_writer->Append(arrow::RecordBatch::Make(arrow_schema, 2, {second_values})).ok())
      << "global sort order is checked across Append calls";

  sniffer::TableSchema corruption_schema{1, {{7, "x", arrow::int32(), false, nullptr}}};
  auto corruption_arrow_schema =
      ValueOrThrow(corruption_schema.ToArrowSchema(), "index corruption schema");
  auto values = BuildArray<arrow::Int32Builder, int32_t>({1, 2, 3});
  auto corruption_batch = arrow::RecordBatch::Make(corruption_arrow_schema, 3, {values});
  sniffer::LayoutPolicy corruption_layout;
  corruption_layout.target_row_group_rows = 10;
  corruption_layout.statistics_field_ids = {7};
  TempFile corruption_file("bad_index.seg");
  WriteSegmentWithPolicy(corruption_file.path(), corruption_schema, {corruption_batch},
                         corruption_layout);
  auto bytes = ReadFile(corruption_file.path());
  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  constexpr size_t kFieldDescriptor = 16;
  constexpr size_t kEncodingDescriptor = 61;
  constexpr size_t kRowGroupHeader = 16;
  const size_t chunk_entry =
      footer_offset + kPrefix + kFieldDescriptor + 1 + kEncodingDescriptor + kRowGroupHeader;
  const uint64_t chunk_offset = ReadU64(bytes, chunk_entry + 24);
  const uint64_t chunk_length = ReadU64(bytes, chunk_entry + 32);
  const size_t index_offset = static_cast<size_t>(chunk_offset + chunk_length);
  bytes[index_offset + 4] ^= 0x01U;
  WriteFile(corruption_file.path(), bytes);
  EXPECT_TRUE(!sniffer::SegmentReader::Open(corruption_file.path().string()).ok())
      << "index checksum corruption fails Open before scanning";
}

TEST(SnifferCoreTest, BloomSignedZeroEquality) {
  sniffer::TableSchema schema{1, {{1, "value", arrow::float64(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "signed-zero schema");
  auto values = BuildArray<arrow::DoubleBuilder, double>({0.0});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 1, {values});
  sniffer::LayoutPolicy policy;
  policy.bloom_field_ids = {1};
  TempFile file("signed_zero.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open signed zero");
  sniffer::IOPlan plan;
  plan.projection_field_ids = {1};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kEq, std::make_shared<arrow::DoubleScalar>(-0.0)}};
  auto batches = CollectScan(ValueOrThrow(reader->Scan(plan), "scan signed zero"));
  EXPECT_TRUE(batches.size() == 1 && batches[0]->num_rows() == 1)
      << "Bloom hashing preserves +0 == -0 predicate semantics";
}

TEST(SnifferCoreTest, LegacyFooterScanFallback) {
  const auto data = MakeScanBatch();
  TempFile file("legacy_footer.seg");
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 7;
  WriteSegmentWithPolicy(file.path(), data.table_schema, {data.batch}, policy);
  auto bytes = ReadFile(file.path());
  constexpr size_t kTrailerSize = 40;
  const size_t trailer_offset = bytes.size() - kTrailerSize;
  const uint64_t footer_offset = ReadU64(bytes, trailer_offset + 8);
  const uint64_t footer_length = ReadU64(bytes, trailer_offset + 16);
  constexpr uint64_t kLayoutBytes = 16;
  const uint64_t tail_length = kLayoutBytes + 24U * 5U;
  EXPECT_TRUE(footer_length > tail_length) << "phase-two footer has a removable index tail";
  const uint64_t legacy_footer_length = footer_length - tail_length;
  bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(footer_offset + legacy_footer_length),
              bytes.begin() + static_cast<std::ptrdiff_t>(footer_offset + footer_length));
  WriteU16(&bytes, static_cast<size_t>(footer_offset), 1);
  const size_t legacy_trailer_offset = bytes.size() - kTrailerSize;
  WriteU64(&bytes, legacy_trailer_offset + 16, legacy_footer_length);
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);

  auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()),
                             "open legacy footer segment");
  sniffer::IOPlan plan;
  plan.projection_field_ids = {1};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(27)}};
  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  auto batches = CollectScan(ValueOrThrow(reader->Scan(plan, metrics), "scan legacy footer"));
  EXPECT_TRUE(CollectInt64Column(batches, 0) == std::vector<int64_t>({27, 28, 29}))
      << "footer v1 remains scan-compatible";
  EXPECT_TRUE(metrics->row_groups_considered == 5 && metrics->row_groups_pruned == 0)
      << "footer v1 safely uses sequential fallback";
}

TEST(SnifferCoreTest, UnknownIndexVersionFailsExplicitly) {
  sniffer::TableSchema schema{1, {{7, "x", arrow::int32(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "unknown index schema");
  auto values = BuildArray<arrow::Int32Builder, int32_t>({1, 2, 3});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 3, {values});
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 10;
  policy.statistics_field_ids = {7};
  TempFile file("unknown_index_version.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  auto bytes = ReadFile(file.path());
  const size_t trailer_offset = bytes.size() - 40;
  const size_t footer_offset = static_cast<size_t>(ReadU64(bytes, trailer_offset + 8));
  constexpr size_t kPrefix = 24;
  constexpr size_t kField = 17;
  constexpr size_t kEncoding = 61;
  constexpr size_t kRowGroup = 16 + 56;
  constexpr size_t kLayoutAndIds = 16 + 4;
  const size_t index_directory =
      footer_offset + kPrefix + kField + kEncoding + kRowGroup + kLayoutAndIds;
  const uint64_t index_offset = ReadU64(bytes, index_directory);
  const uint64_t index_length = ReadU64(bytes, index_directory + 8);
  WriteU16(&bytes, static_cast<size_t>(index_offset), 999);
  const auto index_bytes = std::span<const uint8_t>(
      bytes.data() + static_cast<size_t>(index_offset), static_cast<size_t>(index_length));
  WriteU32(&bytes, index_directory + 16, Crc32c(index_bytes));
  RefreshFooterChecksums(&bytes);
  WriteFile(file.path(), bytes);
  const auto reader = sniffer::SegmentReader::Open(file.path().string());
  EXPECT_TRUE(!reader.ok() && reader.status().IsNotImplemented())
      << "unknown index block version fails as unsupported";
}

TEST(SnifferCoreTest, CompositeSortKeyRange) {
  sniffer::TableSchema schema{1,
                              {{1, "major", arrow::int32(), false, nullptr},
                               {2, "minor", arrow::utf8(), false, nullptr},
                               {3, "value", arrow::int64(), false, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "composite sort schema");
  auto major = BuildArray<arrow::Int32Builder, int32_t>({1, 1, 2, 2, 3});
  auto minor = BuildStringArray({"a", "c", "a", "b", "a"});
  auto value = BuildArray<arrow::Int64Builder, int64_t>({10, 11, 12, 13, 14});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 5, {major, minor, value});
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 2;
  policy.sort_key_field_ids = {1, 2};
  TempFile file("composite_sort.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open composite sort");

  sniffer::IOPlan plan;
  plan.projection_field_ids = {3};
  sniffer::SortKeyRange range;
  range.lower = std::vector<std::shared_ptr<arrow::Scalar>>{
      std::make_shared<arrow::Int32Scalar>(1), std::make_shared<arrow::StringScalar>("b")};
  range.upper = std::vector<std::shared_ptr<arrow::Scalar>>{
      std::make_shared<arrow::Int32Scalar>(2), std::make_shared<arrow::StringScalar>("b")};
  plan.sort_key_range = std::move(range);
  plan.output_batch_rows = 8;
  auto batches = CollectScan(ValueOrThrow(reader->Scan(plan), "scan composite range"));
  EXPECT_TRUE(CollectInt64Column(batches, 0) == std::vector<int64_t>({11, 12}))
      << "composite sort-key range uses lexicographic half-open semantics";
}

TEST(SnifferCoreTest, TypedSortKeyRangesMatchScalarReference) {
  const auto timestamp_type = std::static_pointer_cast<arrow::TimestampType>(
      arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"));
  struct SortCase {
    std::string name;
    std::shared_ptr<arrow::DataType> type;
    std::shared_ptr<arrow::Array> values;
  };
  const std::vector<SortCase> cases = {
      {"bool", arrow::boolean(),
       BuildArray<arrow::BooleanBuilder, bool>({false, false, false, true, true})},
      {"int8", arrow::int8(), BuildArray<arrow::Int8Builder, int8_t>({-3, -1, 0, 2, 4})},
      {"int16", arrow::int16(), BuildArray<arrow::Int16Builder, int16_t>({-3, -1, 0, 2, 4})},
      {"int32", arrow::int32(), BuildArray<arrow::Int32Builder, int32_t>({-3, -1, 0, 2, 4})},
      {"int64", arrow::int64(), BuildArray<arrow::Int64Builder, int64_t>({-3, -1, 0, 2, 4})},
      {"uint8", arrow::uint8(), BuildArray<arrow::UInt8Builder, uint8_t>({0, 1, 2, 3, 4})},
      {"uint16", arrow::uint16(), BuildArray<arrow::UInt16Builder, uint16_t>({0, 1, 2, 3, 4})},
      {"uint32", arrow::uint32(), BuildArray<arrow::UInt32Builder, uint32_t>({0, 1, 2, 3, 4})},
      {"uint64", arrow::uint64(), BuildArray<arrow::UInt64Builder, uint64_t>({0, 1, 2, 3, 4})},
      {"float", arrow::float32(), BuildArray<arrow::FloatBuilder, float>({-3, -1, 0, 2, 4})},
      {"double", arrow::float64(), BuildArray<arrow::DoubleBuilder, double>({-3, -1, 0, 2, 4})},
      {"timestamp", timestamp_type, BuildTimestampArray(timestamp_type, {-3, -1, 0, 2, 4})},
      {"string", arrow::utf8(), BuildStringArray({"", "a", "aa", "b", "z"})},
      {"binary", arrow::binary(),
       BuildBinaryArray({std::vector<uint8_t>{}, std::vector<uint8_t>{0},
                         std::vector<uint8_t>{0, 1}, std::vector<uint8_t>{1},
                         std::vector<uint8_t>{0xFF}})},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    sniffer::TableSchema schema{1, {{1, "key", test_case.type, false, nullptr}}};
    const auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "typed sort-key schema");
    const auto batch =
        arrow::RecordBatch::Make(arrow_schema, test_case.values->length(), {test_case.values});
    sniffer::LayoutPolicy policy;
    policy.target_row_group_rows = 2;
    policy.sort_key_field_ids = {1};
    TempFile file("typed_sort_" + test_case.name + ".seg");
    WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
    const auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()),
                                     "open typed sort-key segment");

    const auto lower = ValueOrThrow(test_case.values->GetScalar(1), "typed lower bound");
    const auto upper = ValueOrThrow(test_case.values->GetScalar(4), "typed upper bound");
    sniffer::IOPlan plan;
    plan.projection_field_ids = {1};
    sniffer::SortKeyRange range;
    range.lower = std::vector<std::shared_ptr<arrow::Scalar>>{lower};
    range.upper = std::vector<std::shared_ptr<arrow::Scalar>>{upper};
    plan.sort_key_range = std::move(range);
    plan.output_batch_rows = 2;
    const auto batches = CollectScan(ValueOrThrow(reader->Scan(plan), "scan typed sort-key range"));
    arrow::ArrayVector actual_chunks;
    for (const auto& output : batches) {
      actual_chunks.push_back(output->column(0));
    }
    const auto actual =
        ValueOrThrow(arrow::Concatenate(actual_chunks), "concatenate typed sort-key result");

    auto expected_builder =
        ValueOrThrow(arrow::MakeBuilder(test_case.type), "make typed sort-key reference builder");
    for (int64_t row = 0; row < test_case.values->length(); ++row) {
      const auto value = ValueOrThrow(test_case.values->GetScalar(row), "typed sort-key value");
      const int lower_order = ValueOrThrow(sniffer::internal::CompareScalars(*value, *lower),
                                           "compare typed lower bound");
      const int upper_order = ValueOrThrow(sniffer::internal::CompareScalars(*value, *upper),
                                           "compare typed upper bound");
      if (lower_order >= 0 && upper_order < 0) {
        RequireOk(expected_builder->AppendScalar(*value), "append typed sort-key reference");
      }
    }
    std::shared_ptr<arrow::Array> expected;
    RequireOk(expected_builder->Finish(&expected), "finish typed sort-key reference");
    EXPECT_TRUE(actual->Equals(expected)) << "typed sort-key range matches scalar reference";
  }
}

TEST(SnifferCoreTest, ForcedDictionaryRoundTripAndScan) {
  sniffer::TableSchema schema{1,
                              {{1, "number", arrow::int64(), true, nullptr},
                               {2, "label", arrow::utf8(), true, nullptr},
                               {3, "bytes", arrow::binary(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "Dictionary schema");
  arrow::Int64Builder number_builder;
  arrow::StringBuilder label_builder;
  arrow::BinaryBuilder bytes_builder;
  for (int64_t row = 0; row < 128; ++row) {
    if (row % 11 == 0) {
      RequireOk(number_builder.AppendNull(), "append Dictionary number null");
      RequireOk(label_builder.AppendNull(), "append Dictionary label null");
      RequireOk(bytes_builder.AppendNull(), "append Dictionary bytes null");
      continue;
    }
    RequireOk(number_builder.Append(row % 5), "append Dictionary number");
    RequireOk(label_builder.Append("label-" + std::to_string(row % 3)), "append Dictionary label");
    const std::vector<uint8_t> bytes = {static_cast<uint8_t>(row % 4), 0,
                                        static_cast<uint8_t>(row % 2)};
    RequireOk(bytes_builder.Append(bytes.data(), static_cast<int32_t>(bytes.size())),
              "append Dictionary bytes");
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(3);
  RequireOk(number_builder.Finish(&columns[0]), "finish Dictionary numbers");
  RequireOk(label_builder.Finish(&columns[1]), "finish Dictionary labels");
  RequireOk(bytes_builder.Finish(&columns[2]), "finish Dictionary bytes");
  auto batch = arrow::RecordBatch::Make(arrow_schema, 128, std::move(columns));
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 37;
  policy.field_encodings = {{1, sniffer::EncodingKind::kDictionary},
                            {2, sniffer::EncodingKind::kDictionary},
                            {3, sniffer::EncodingKind::kDictionary}};
  TempFile file("forced_dictionary.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  EXPECT_TRUE(FirstChunkEncoding(file.path(), 64) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kDictionary))
      << "forced Dictionary encoding ID is persisted";
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open Dictionary segment");
  auto round_trip = ValueOrThrow(reader->ReadAll(), "read Dictionary segment");
  int64_t offset = 0;
  for (const auto& output : round_trip) {
    EXPECT_TRUE(output->Equals(*batch->Slice(offset, output->num_rows())))
        << "Dictionary round-trip equals input";
    offset += output->num_rows();
  }
  EXPECT_TRUE(offset == 128) << "Dictionary round-trip preserves all rows";

  sniffer::IOPlan plan;
  plan.projection_field_ids = {3, 2};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kEq, std::make_shared<arrow::Int64Scalar>(3)}};
  plan.output_batch_rows = 9;
  auto scan = CollectScan(ValueOrThrow(reader->Scan(plan), "scan Dictionary segment"));
  int64_t selected_rows = 0;
  for (const auto& output : scan) {
    selected_rows += output->num_rows();
    EXPECT_TRUE(output->schema()->field(0)->name() == "bytes" &&
                output->schema()->field(1)->name() == "label")
        << "Dictionary selected projection order";
  }
  EXPECT_TRUE(selected_rows == 23) << "Dictionary predicate and selected decode preserve matches";
}

TEST(SnifferCoreTest, ForcedRleRoundTripAndScan) {
  sniffer::TableSchema schema{1,
                              {{1, "group", arrow::int32(), true, nullptr},
                               {2, "flag", arrow::boolean(), true, nullptr},
                               {3, "all_null", arrow::int64(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "RLE schema");
  arrow::Int32Builder group_builder;
  arrow::BooleanBuilder flag_builder;
  arrow::Int64Builder null_builder;
  for (int64_t row = 0; row < 150; ++row) {
    if (row >= 60 && row < 75) {
      RequireOk(group_builder.AppendNull(), "append RLE group null");
      RequireOk(flag_builder.AppendNull(), "append RLE flag null");
    } else {
      RequireOk(group_builder.Append(static_cast<int32_t>(row / 25)), "append RLE group");
      RequireOk(flag_builder.Append((row / 30) % 2 == 0), "append RLE flag");
    }
    RequireOk(null_builder.AppendNull(), "append RLE all-null value");
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(3);
  RequireOk(group_builder.Finish(&columns[0]), "finish RLE groups");
  RequireOk(flag_builder.Finish(&columns[1]), "finish RLE flags");
  RequireOk(null_builder.Finish(&columns[2]), "finish RLE nulls");
  auto batch = arrow::RecordBatch::Make(arrow_schema, 150, std::move(columns));
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 64;
  policy.field_encodings = {{1, sniffer::EncodingKind::kRle},
                            {2, sniffer::EncodingKind::kRle},
                            {3, sniffer::EncodingKind::kRle}};
  TempFile file("forced_rle.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  EXPECT_TRUE(FirstChunkEncoding(file.path(), 65) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kRle))
      << "forced RLE encoding ID is persisted";
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open RLE segment");
  auto round_trip = ValueOrThrow(reader->ReadAll(), "read RLE segment");
  int64_t offset = 0;
  for (const auto& output : round_trip) {
    EXPECT_TRUE(output->Equals(*batch->Slice(offset, output->num_rows())))
        << "RLE round-trip equals input";
    offset += output->num_rows();
  }

  sniffer::IOPlan plan;
  plan.projection_field_ids = {2, 3};
  plan.conjunctive_predicates = {
      {1, sniffer::Predicate::Op::kEq, std::make_shared<arrow::Int32Scalar>(4)}};
  plan.output_batch_rows = 8;
  auto scan = CollectScan(ValueOrThrow(reader->Scan(plan), "scan RLE segment"));
  int64_t selected_rows = 0;
  for (const auto& output : scan) {
    selected_rows += output->num_rows();
    EXPECT_TRUE(output->column(1)->null_count() == output->num_rows())
        << "RLE selected all-null projection remains null";
  }
  EXPECT_TRUE(selected_rows == 25) << "RLE predicate returns the full repeated run";
}

TEST(SnifferCoreTest, ForcedForBitpackRoundTripAndScan) {
  const auto timestamp_type = std::static_pointer_cast<arrow::TimestampType>(
      arrow::timestamp(arrow::TimeUnit::NANO, "UTC"));
  sniffer::TableSchema schema{1,
                              {{1, "i8", arrow::int8(), true, nullptr},
                               {2, "u16", arrow::uint16(), true, nullptr},
                               {3, "i64", arrow::int64(), true, nullptr},
                               {4, "u64", arrow::uint64(), true, nullptr},
                               {5, "time", timestamp_type, true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "FOR schema");
  arrow::Int8Builder i8_builder;
  arrow::UInt16Builder u16_builder;
  arrow::Int64Builder i64_builder;
  arrow::UInt64Builder u64_builder;
  arrow::TimestampBuilder timestamp_builder(timestamp_type, arrow::default_memory_pool());
  constexpr int64_t kRows = 131;
  for (int64_t row = 0; row < kRows; ++row) {
    if (row % 17 == 0) {
      RequireOk(i8_builder.AppendNull(), "append FOR i8 null");
      RequireOk(u16_builder.AppendNull(), "append FOR u16 null");
      RequireOk(i64_builder.AppendNull(), "append FOR i64 null");
      RequireOk(u64_builder.AppendNull(), "append FOR u64 null");
      RequireOk(timestamp_builder.AppendNull(), "append FOR timestamp null");
      continue;
    }
    const int8_t i8 = row == 1     ? std::numeric_limits<int8_t>::min()
                      : row == 130 ? std::numeric_limits<int8_t>::max()
                                   : static_cast<int8_t>(-60 + row % 100);
    const int64_t i64 = row == 1     ? std::numeric_limits<int64_t>::min()
                        : row == 130 ? std::numeric_limits<int64_t>::max()
                                     : row * 13 - 700;
    const uint64_t u64 = row == 1     ? 0
                         : row == 130 ? std::numeric_limits<uint64_t>::max()
                                      : static_cast<uint64_t>(row * 29);
    RequireOk(i8_builder.Append(i8), "append FOR i8");
    RequireOk(u16_builder.Append(static_cast<uint16_t>(60000 + row)), "append FOR u16");
    RequireOk(i64_builder.Append(i64), "append FOR i64");
    RequireOk(u64_builder.Append(u64), "append FOR u64");
    RequireOk(timestamp_builder.Append(-1000000 + row * 7), "append FOR timestamp");
  }
  std::vector<std::shared_ptr<arrow::Array>> columns(5);
  RequireOk(i8_builder.Finish(&columns[0]), "finish FOR i8");
  RequireOk(u16_builder.Finish(&columns[1]), "finish FOR u16");
  RequireOk(i64_builder.Finish(&columns[2]), "finish FOR i64");
  RequireOk(u64_builder.Finish(&columns[3]), "finish FOR u64");
  RequireOk(timestamp_builder.Finish(&columns[4]), "finish FOR timestamp");
  auto batch = arrow::RecordBatch::Make(arrow_schema, kRows, std::move(columns));
  sniffer::LayoutPolicy policy;
  policy.target_row_group_rows = 67;
  for (uint32_t field_id = 1; field_id <= 5; ++field_id) {
    policy.field_encodings.push_back({field_id, sniffer::EncodingKind::kForBitpack});
  }
  TempFile file("forced_for.seg");
  WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
  EXPECT_TRUE(FirstChunkEncoding(file.path(), 106) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kForBitpack))
      << "forced FOR + Bitpack encoding ID is persisted";
  auto reader =
      ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()), "open FOR segment");
  auto round_trip = ValueOrThrow(reader->ReadAll(), "read FOR segment");
  int64_t offset = 0;
  for (const auto& output : round_trip) {
    EXPECT_TRUE(output->Equals(*batch->Slice(offset, output->num_rows())))
        << "FOR round-trip equals input";
    offset += output->num_rows();
  }

  sniffer::IOPlan plan;
  plan.projection_field_ids = {5, 4, 1};
  plan.conjunctive_predicates = {
      {2, sniffer::Predicate::Op::kGe, std::make_shared<arrow::UInt16Scalar>(60080)}};
  plan.output_batch_rows = 11;
  auto scan = CollectScan(ValueOrThrow(reader->Scan(plan), "scan FOR segment"));
  int64_t selected_rows = 0;
  for (const auto& output : scan) {
    selected_rows += output->num_rows();
  }
  EXPECT_TRUE(selected_rows == 48)
      << "FOR predicate and selected projection decode preserve null semantics";
}

TEST(SnifferCoreTest, ForcedEncodingRandomizedProperty) {
  std::mt19937_64 random(0xC0DEC0DEULL);
  std::vector<std::optional<int64_t>> filter_values;
  std::vector<std::optional<int64_t>> projected_values;
  constexpr int64_t kRows = 513;
  filter_values.reserve(kRows);
  projected_values.reserve(kRows);
  for (int64_t row = 0; row < kRows; ++row) {
    if (row % 13 == 0) {
      filter_values.push_back(std::nullopt);
    } else {
      filter_values.push_back(std::bit_cast<int64_t>(random()));
    }
    if (row % 17 == 0) {
      projected_values.push_back(std::nullopt);
    } else if (row % 5 == 0) {
      projected_values.push_back(7);
    } else {
      projected_values.push_back(std::bit_cast<int64_t>(random()));
    }
  }
  sniffer::TableSchema schema{1,
                              {{1, "filter", arrow::int64(), true, nullptr},
                               {2, "projected", arrow::int64(), true, nullptr}}};
  auto arrow_schema = ValueOrThrow(schema.ToArrowSchema(), "codec property schema");
  auto filter = BuildArray<arrow::Int64Builder, int64_t>(filter_values);
  auto projected = BuildArray<arrow::Int64Builder, int64_t>(projected_values);
  auto batch = arrow::RecordBatch::Make(arrow_schema, kRows, {filter, projected});

  for (const auto encoding : {sniffer::EncodingKind::kDictionary, sniffer::EncodingKind::kRle,
                              sniffer::EncodingKind::kForBitpack}) {
    sniffer::LayoutPolicy policy;
    policy.target_row_group_rows = 73;
    policy.field_encodings = {{1, encoding}, {2, encoding}};
    TempFile file("codec_property_" + std::to_string(static_cast<uint16_t>(encoding)) + ".seg");
    WriteSegmentWithPolicy(file.path(), schema, {batch}, policy);
    auto reader = ValueOrThrow(sniffer::SegmentReader::Open(file.path().string()),
                               "open randomized codec segment");
    auto round_trip = ValueOrThrow(reader->ReadAll(), "randomized codec round-trip");
    int64_t offset = 0;
    for (const auto& output : round_trip) {
      EXPECT_TRUE(output->Equals(*batch->Slice(offset, output->num_rows())))
          << "randomized forced codec round-trip";
      offset += output->num_rows();
    }

    sniffer::IOPlan plan;
    plan.projection_field_ids = {2};
    plan.conjunctive_predicates = {
        {1, sniffer::Predicate::Op::kGe, std::make_shared<arrow::Int64Scalar>(0)}};
    plan.output_batch_rows = 31;
    auto scan = CollectScan(ValueOrThrow(reader->Scan(plan), "randomized codec scan"));
    arrow::Int64Builder reference_builder;
    const auto& filter_array = static_cast<const arrow::Int64Array&>(*filter);
    const auto& projected_array = static_cast<const arrow::Int64Array&>(*projected);
    for (int64_t row = 0; row < kRows; ++row) {
      if (filter_array.IsValid(row) && filter_array.Value(row) >= 0) {
        if (projected_array.IsValid(row)) {
          RequireOk(reference_builder.Append(projected_array.Value(row)),
                    "append codec property reference");
        } else {
          RequireOk(reference_builder.AppendNull(), "append codec property reference null");
        }
      }
    }
    std::shared_ptr<arrow::Array> expected;
    RequireOk(reference_builder.Finish(&expected), "finish codec property reference");
    arrow::ArrayVector actual_chunks;
    for (const auto& output : scan) {
      actual_chunks.push_back(output->column(0));
    }
    auto actual = ValueOrThrow(arrow::Concatenate(actual_chunks), "concatenate codec scan");
    EXPECT_TRUE(actual->Equals(expected)) << "randomized codec IOPlan equals reference filtering";
  }
}

TEST(SnifferCoreTest, DeterministicEncodingSelector) {
  sniffer::TableSchema integer_schema{1, {{1, "x", arrow::int64(), false, nullptr}}};
  auto integer_arrow_schema = ValueOrThrow(integer_schema.ToArrowSchema(), "selector int schema");

  auto repeated =
      BuildArray<arrow::Int64Builder, int64_t>(std::vector<std::optional<int64_t>>(100, 7));
  TempFile rle_file("selector_rle.seg");
  WriteSegmentWithPolicy(rle_file.path(), integer_schema,
                         {arrow::RecordBatch::Make(integer_arrow_schema, 100, {repeated})}, {});
  EXPECT_TRUE(FirstChunkEncoding(rle_file.path(), 17) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kRle))
      << "selector chooses RLE for a long run";

  std::vector<std::optional<int64_t>> ascending_values;
  for (int64_t value = 0; value < 100; ++value) {
    ascending_values.push_back(value);
  }
  auto ascending = BuildArray<arrow::Int64Builder, int64_t>(ascending_values);
  TempFile for_file("selector_for.seg");
  WriteSegmentWithPolicy(for_file.path(), integer_schema,
                         {arrow::RecordBatch::Make(integer_arrow_schema, 100, {ascending})}, {});
  EXPECT_TRUE(FirstChunkEncoding(for_file.path(), 17) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kForBitpack))
      << "selector chooses FOR + Bitpack for a narrow ascending range";

  sniffer::TableSchema string_schema{1, {{1, "x", arrow::utf8(), false, nullptr}}};
  auto string_arrow_schema = ValueOrThrow(string_schema.ToArrowSchema(), "selector string schema");
  std::vector<std::optional<std::string>> repeated_strings;
  std::vector<std::optional<std::string>> unique_strings;
  for (int64_t row = 0; row < 100; ++row) {
    repeated_strings.push_back(row % 2 == 0 ? "alpha" : "beta");
    unique_strings.push_back("unique-long-value-" + std::to_string(row));
  }
  TempFile dictionary_file("selector_dictionary.seg");
  WriteSegmentWithPolicy(
      dictionary_file.path(), string_schema,
      {arrow::RecordBatch::Make(string_arrow_schema, 100, {BuildStringArray(repeated_strings)})},
      {});
  EXPECT_TRUE(FirstChunkEncoding(dictionary_file.path(), 17) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kDictionary))
      << "selector chooses Dictionary for low-cardinality strings";

  TempFile plain_file("selector_plain.seg");
  WriteSegmentWithPolicy(
      plain_file.path(), string_schema,
      {arrow::RecordBatch::Make(string_arrow_schema, 100, {BuildStringArray(unique_strings)})}, {});
  EXPECT_TRUE(FirstChunkEncoding(plain_file.path(), 17) ==
              static_cast<uint16_t>(sniffer::EncodingKind::kPlain))
      << "selector retains Plain for high-cardinality strings";
}

}  // namespace
