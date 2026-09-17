#include "index_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "scalar_internal.h"

namespace sniffer::internal {
namespace {

arrow::Result<size_t> FieldIndex(const TableSchema& schema, uint32_t field_id) {
  for (size_t index = 0; index < schema.fields.size(); ++index) {
    if (schema.fields[index].field_id == field_id) {
      return index;
    }
  }
  return arrow::Status::Invalid("[sniffer.layout.index] unknown field ID ", field_id);
}

arrow::Result<int> CompareKeys(const std::vector<std::shared_ptr<arrow::Scalar>>& left,
                               const std::vector<std::shared_ptr<arrow::Scalar>>& right) {
  if (left.size() != right.size()) {
    return arrow::Status::Invalid("[sniffer.layout.sort_key] sort-key arity mismatch");
  }
  for (size_t index = 0; index < left.size(); ++index) {
    ARROW_ASSIGN_OR_RAISE(const int order, CompareScalars(*left[index], *right[index]));
    if (order != 0) {
      return order;
    }
  }
  return 0;
}

template <typename ArrayType>
int ComparePrimitiveRows(const arrow::Array& untyped, int64_t left_index, int64_t right_index) {
  const auto& array = static_cast<const ArrayType&>(untyped);
  const auto left = array.Value(left_index);
  const auto right = array.Value(right_index);
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

int CompareByteViews(std::string_view left, std::string_view right) {
  const size_t common_size = std::min(left.size(), right.size());
  for (size_t index = 0; index < common_size; ++index) {
    const auto left_byte = static_cast<uint8_t>(left[index]);
    const auto right_byte = static_cast<uint8_t>(right[index]);
    if (left_byte < right_byte) {
      return -1;
    }
    if (left_byte > right_byte) {
      return 1;
    }
  }
  if (left.size() < right.size()) {
    return -1;
  }
  if (left.size() > right.size()) {
    return 1;
  }
  return 0;
}

arrow::Result<int> CompareArrayRows(const arrow::Array& array, int64_t left_index,
                                    int64_t right_index) {
  switch (array.type_id()) {
    case arrow::Type::BOOL:
      return ComparePrimitiveRows<arrow::BooleanArray>(array, left_index, right_index);
    case arrow::Type::INT8:
      return ComparePrimitiveRows<arrow::Int8Array>(array, left_index, right_index);
    case arrow::Type::INT16:
      return ComparePrimitiveRows<arrow::Int16Array>(array, left_index, right_index);
    case arrow::Type::INT32:
      return ComparePrimitiveRows<arrow::Int32Array>(array, left_index, right_index);
    case arrow::Type::INT64:
      return ComparePrimitiveRows<arrow::Int64Array>(array, left_index, right_index);
    case arrow::Type::UINT8:
      return ComparePrimitiveRows<arrow::UInt8Array>(array, left_index, right_index);
    case arrow::Type::UINT16:
      return ComparePrimitiveRows<arrow::UInt16Array>(array, left_index, right_index);
    case arrow::Type::UINT32:
      return ComparePrimitiveRows<arrow::UInt32Array>(array, left_index, right_index);
    case arrow::Type::UINT64:
      return ComparePrimitiveRows<arrow::UInt64Array>(array, left_index, right_index);
    case arrow::Type::FLOAT:
      return ComparePrimitiveRows<arrow::FloatArray>(array, left_index, right_index);
    case arrow::Type::DOUBLE:
      return ComparePrimitiveRows<arrow::DoubleArray>(array, left_index, right_index);
    case arrow::Type::TIMESTAMP:
      return ComparePrimitiveRows<arrow::TimestampArray>(array, left_index, right_index);
    case arrow::Type::STRING: {
      const auto& strings = static_cast<const arrow::StringArray&>(array);
      return CompareByteViews(strings.GetView(left_index), strings.GetView(right_index));
    }
    case arrow::Type::BINARY: {
      const auto& binary = static_cast<const arrow::BinaryArray&>(array);
      return CompareByteViews(binary.GetView(left_index), binary.GetView(right_index));
    }
    default:
      return arrow::Status::NotImplemented("[sniffer.layout.sort_key] unsupported type ",
                                           array.type()->ToString());
  }
}

bool ArrayValueHasNaN(const arrow::Array& array, int64_t row) {
  if (array.type_id() == arrow::Type::FLOAT) {
    return std::isnan(static_cast<const arrow::FloatArray&>(array).Value(row));
  }
  if (array.type_id() == arrow::Type::DOUBLE) {
    return std::isnan(static_cast<const arrow::DoubleArray&>(array).Value(row));
  }
  return false;
}

arrow::Status ValidateSingleSortKey(const TableSchema& schema, uint32_t field_id,
                                    const arrow::RecordBatch& batch,
                                    std::vector<std::shared_ptr<arrow::Scalar>>* previous_key) {
  ARROW_ASSIGN_OR_RAISE(const size_t field_index, FieldIndex(schema, field_id));
  const auto& array = *batch.column(static_cast<int>(field_index));
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row) || ArrayValueHasNaN(array, row)) {
      return arrow::Status::Invalid("[sniffer.layout.sort_key] field ", field_id,
                                    " contains null or NaN");
    }
  }

  ARROW_ASSIGN_OR_RAISE(auto first, array.GetScalar(0));
  if (!previous_key->empty()) {
    if (previous_key->size() != 1) {
      return arrow::Status::Invalid("[sniffer.layout.sort_key] sort-key arity mismatch");
    }
    ARROW_ASSIGN_OR_RAISE(const int order, CompareScalars(*previous_key->front(), *first));
    if (order > 0) {
      return arrow::Status::Invalid(
          "[sniffer.layout.sort_key] input is not globally non-decreasing");
    }
  }
  for (int64_t row = 1; row < array.length(); ++row) {
    ARROW_ASSIGN_OR_RAISE(const int order, CompareArrayRows(array, row - 1, row));
    if (order > 0) {
      return arrow::Status::Invalid(
          "[sniffer.layout.sort_key] input is not globally non-decreasing");
    }
  }

  if (array.length() == 1) {
    *previous_key = {std::move(first)};
  } else {
    ARROW_ASSIGN_OR_RAISE(auto last, array.GetScalar(array.length() - 1));
    *previous_key = {std::move(last)};
  }
  return arrow::Status::OK();
}

arrow::Result<std::vector<std::shared_ptr<arrow::Scalar>>> KeyAt(const TableSchema& schema,
                                                                 const LayoutPolicy& layout,
                                                                 const arrow::RecordBatch& batch,
                                                                 int64_t row) {
  std::vector<std::shared_ptr<arrow::Scalar>> key;
  key.reserve(layout.sort_key_field_ids.size());
  for (const uint32_t field_id : layout.sort_key_field_ids) {
    ARROW_ASSIGN_OR_RAISE(const size_t index, FieldIndex(schema, field_id));
    ARROW_ASSIGN_OR_RAISE(auto value, batch.column(static_cast<int>(index))->GetScalar(row));
    if (!value->is_valid || ScalarHasNaN(*value)) {
      return arrow::Status::Invalid("[sniffer.layout.sort_key] field ", field_id,
                                    " contains null or NaN");
    }
    key.push_back(std::move(value));
  }
  return key;
}

uint64_t BloomBitCount(uint64_t row_count) {
  const uint64_t target = row_count > std::numeric_limits<uint64_t>::max() / 10U
                              ? std::numeric_limits<uint64_t>::max()
                              : row_count * 10U;
  uint64_t bits = 256;
  while (bits < target && bits <= std::numeric_limits<uint64_t>::max() / 2U) {
    bits *= 2U;
  }
  return bits;
}

arrow::Status BloomInsertArrayValue(const FieldSpec& field, const arrow::Array& array, int64_t row,
                                    BloomMeta* bloom) {
  ARROW_ASSIGN_OR_RAISE(auto hashes, HashArrayValuePair(field, array, row, 0x243F6A8885A308D3ULL,
                                                        0x13198A2E03707344ULL));
  const auto [first, raw_second] = hashes;
  const uint64_t second = raw_second | 1U;
  for (uint32_t probe = 0; probe < bloom->hash_count; ++probe) {
    const uint64_t bit = (first + static_cast<uint64_t>(probe) * second) & (bloom->bit_count - 1U);
    bloom->bits[static_cast<size_t>(bit / 8U)] |=
        static_cast<uint8_t>(1U << static_cast<uint32_t>(bit % 8U));
  }
  return arrow::Status::OK();
}

arrow::Result<StatisticsMeta> FinishStatistics(StatisticsMeta statistics, const arrow::Array& array,
                                               int64_t min_row, int64_t max_row) {
  if (min_row >= 0) {
    ARROW_ASSIGN_OR_RAISE(statistics.min, array.GetScalar(min_row));
    ARROW_ASSIGN_OR_RAISE(statistics.max, array.GetScalar(max_row));
  }
  return statistics;
}

template <typename ArrayType>
arrow::Result<StatisticsMeta> BuildPrimitiveStatistics(uint32_t field_id,
                                                       const arrow::Array& untyped) {
  const auto& array = static_cast<const ArrayType&>(untyped);
  StatisticsMeta statistics;
  statistics.field_id = field_id;
  statistics.null_count = static_cast<uint64_t>(array.null_count());
  int64_t min_row = -1;
  int64_t max_row = -1;
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    const auto value = array.Value(row);
    if constexpr (std::is_floating_point_v<std::remove_cv_t<decltype(value)>>) {
      if (std::isnan(value)) {
        return statistics;
      }
    }
    if (min_row < 0) {
      min_row = row;
      max_row = row;
      continue;
    }
    if (value < array.Value(min_row)) {
      min_row = row;
    }
    if (value > array.Value(max_row)) {
      max_row = row;
    }
  }
  return FinishStatistics(std::move(statistics), array, min_row, max_row);
}

template <typename ArrayType>
arrow::Result<StatisticsMeta> BuildVariableStatistics(uint32_t field_id,
                                                      const arrow::Array& untyped) {
  const auto& array = static_cast<const ArrayType&>(untyped);
  StatisticsMeta statistics;
  statistics.field_id = field_id;
  statistics.null_count = static_cast<uint64_t>(array.null_count());
  int64_t min_row = -1;
  int64_t max_row = -1;
  for (int64_t row = 0; row < array.length(); ++row) {
    if (array.IsNull(row)) {
      continue;
    }
    if (min_row < 0) {
      min_row = row;
      max_row = row;
      continue;
    }
    if (CompareByteViews(array.GetView(row), array.GetView(min_row)) < 0) {
      min_row = row;
    }
    if (CompareByteViews(array.GetView(row), array.GetView(max_row)) > 0) {
      max_row = row;
    }
  }
  return FinishStatistics(std::move(statistics), array, min_row, max_row);
}

arrow::Result<StatisticsMeta> BuildStatistics(uint32_t field_id, const arrow::Array& array) {
  switch (array.type_id()) {
    case arrow::Type::BOOL:
      return BuildPrimitiveStatistics<arrow::BooleanArray>(field_id, array);
    case arrow::Type::INT8:
      return BuildPrimitiveStatistics<arrow::Int8Array>(field_id, array);
    case arrow::Type::INT16:
      return BuildPrimitiveStatistics<arrow::Int16Array>(field_id, array);
    case arrow::Type::INT32:
      return BuildPrimitiveStatistics<arrow::Int32Array>(field_id, array);
    case arrow::Type::INT64:
      return BuildPrimitiveStatistics<arrow::Int64Array>(field_id, array);
    case arrow::Type::UINT8:
      return BuildPrimitiveStatistics<arrow::UInt8Array>(field_id, array);
    case arrow::Type::UINT16:
      return BuildPrimitiveStatistics<arrow::UInt16Array>(field_id, array);
    case arrow::Type::UINT32:
      return BuildPrimitiveStatistics<arrow::UInt32Array>(field_id, array);
    case arrow::Type::UINT64:
      return BuildPrimitiveStatistics<arrow::UInt64Array>(field_id, array);
    case arrow::Type::FLOAT:
      return BuildPrimitiveStatistics<arrow::FloatArray>(field_id, array);
    case arrow::Type::DOUBLE:
      return BuildPrimitiveStatistics<arrow::DoubleArray>(field_id, array);
    case arrow::Type::TIMESTAMP:
      return BuildPrimitiveStatistics<arrow::TimestampArray>(field_id, array);
    case arrow::Type::STRING:
      return BuildVariableStatistics<arrow::StringArray>(field_id, array);
    case arrow::Type::BINARY:
      return BuildVariableStatistics<arrow::BinaryArray>(field_id, array);
    default:
      return arrow::Status::NotImplemented("[sniffer.layout.statistics] unsupported type ",
                                           array.type()->ToString());
  }
}

bool CanUseSortedPrimaryStatistics(const LayoutPolicy& layout, uint32_t field_id,
                                   const arrow::Array& array, bool sort_order_validated) {
  if (!sort_order_validated || layout.sort_key_field_ids.empty() ||
      layout.sort_key_field_ids.front() != field_id || array.length() == 0 ||
      array.null_count() != 0) {
    return false;
  }
  // CompareArrayRows treats signed zeroes as equal, so endpoint substitution could change the
  // persisted min/max bit pattern for an otherwise equivalent FLOAT/DOUBLE sequence.
  return array.type_id() != arrow::Type::FLOAT && array.type_id() != arrow::Type::DOUBLE;
}

arrow::Result<StatisticsMeta> BuildSortedPrimaryStatistics(uint32_t field_id,
                                                           const arrow::Array& array) {
  StatisticsMeta statistics;
  statistics.field_id = field_id;
  ARROW_ASSIGN_OR_RAISE(statistics.min, array.GetScalar(0));
  ARROW_ASSIGN_OR_RAISE(statistics.max, array.GetScalar(array.length() - 1));
  return statistics;
}

}  // namespace

arrow::Status ValidateAndUpdateSortOrder(
    const TableSchema& schema, const LayoutPolicy& layout, const arrow::RecordBatch& batch,
    std::vector<std::shared_ptr<arrow::Scalar>>* previous_key) {
  if (layout.sort_key_field_ids.empty() || batch.num_rows() == 0) {
    return arrow::Status::OK();
  }
  if (layout.sort_key_field_ids.size() == 1) {
    return ValidateSingleSortKey(schema, layout.sort_key_field_ids.front(), batch, previous_key);
  }
  std::vector<std::shared_ptr<arrow::Scalar>> prior = *previous_key;
  for (int64_t row = 0; row < batch.num_rows(); ++row) {
    ARROW_ASSIGN_OR_RAISE(auto key, KeyAt(schema, layout, batch, row));
    if (!prior.empty()) {
      ARROW_ASSIGN_OR_RAISE(const int order, CompareKeys(prior, key));
      if (order > 0) {
        return arrow::Status::Invalid(
            "[sniffer.layout.sort_key] input is not globally non-decreasing");
      }
    }
    prior = std::move(key);
  }
  *previous_key = std::move(prior);
  return arrow::Status::OK();
}

arrow::Result<RowGroupIndex> BuildRowGroupIndex(const TableSchema& schema,
                                                const LayoutPolicy& layout,
                                                const arrow::RecordBatch& batch,
                                                bool sort_order_validated) {
  RowGroupIndex result;
  result.statistics.reserve(layout.statistics_field_ids.size());
  result.blooms.reserve(layout.bloom_field_ids.size());
  result.sort_keys.reserve(layout.sort_key_field_ids.size());

  for (const uint32_t field_id : layout.statistics_field_ids) {
    ARROW_ASSIGN_OR_RAISE(const size_t field_index, FieldIndex(schema, field_id));
    const auto& array = batch.column(static_cast<int>(field_index));
    StatisticsMeta statistics;
    if (CanUseSortedPrimaryStatistics(layout, field_id, *array, sort_order_validated)) {
      ARROW_ASSIGN_OR_RAISE(statistics, BuildSortedPrimaryStatistics(field_id, *array));
    } else {
      ARROW_ASSIGN_OR_RAISE(statistics, BuildStatistics(field_id, *array));
    }
    result.statistics.push_back(std::move(statistics));
  }

  for (const uint32_t field_id : layout.bloom_field_ids) {
    ARROW_ASSIGN_OR_RAISE(const size_t field_index, FieldIndex(schema, field_id));
    const auto& field = schema.fields[field_index];
    const auto& array = batch.column(static_cast<int>(field_index));
    BloomMeta bloom;
    bloom.field_id = field_id;
    bloom.bit_count = BloomBitCount(static_cast<uint64_t>(array->length()));
    bloom.hash_count = 7;
    if (bloom.bit_count / 8U > std::numeric_limits<size_t>::max()) {
      return arrow::Status::Invalid("[sniffer.format.limit] Bloom filter exceeds platform limit");
    }
    bloom.bits.assign(static_cast<size_t>(bloom.bit_count / 8U), 0);
    for (int64_t row = 0; row < array->length(); ++row) {
      if (array->IsNull(row)) {
        continue;
      }
      if (!ArrayValueHasNaN(*array, row)) {
        ARROW_RETURN_NOT_OK(BloomInsertArrayValue(field, *array, row, &bloom));
      }
    }
    result.blooms.push_back(std::move(bloom));
  }

  if (!layout.sort_key_field_ids.empty()) {
    ARROW_ASSIGN_OR_RAISE(auto first, KeyAt(schema, layout, batch, 0));
    ARROW_ASSIGN_OR_RAISE(auto last, KeyAt(schema, layout, batch, batch.num_rows() - 1));
    for (size_t index = 0; index < layout.sort_key_field_ids.size(); ++index) {
      result.sort_keys.push_back(
          {layout.sort_key_field_ids[index], std::move(first[index]), std::move(last[index])});
    }
  }
  return result;
}

arrow::Result<bool> BloomMayContain(const FieldSpec& field, const BloomMeta& bloom,
                                    const arrow::Scalar& value) {
  if (ScalarHasNaN(value)) {
    return false;
  }
  ARROW_ASSIGN_OR_RAISE(const uint64_t first, HashScalar(field, value, 0x243F6A8885A308D3ULL));
  ARROW_ASSIGN_OR_RAISE(const uint64_t raw_second, HashScalar(field, value, 0x13198A2E03707344ULL));
  const uint64_t second = raw_second | 1U;
  for (uint32_t probe = 0; probe < bloom.hash_count; ++probe) {
    const uint64_t bit = (first + static_cast<uint64_t>(probe) * second) & (bloom.bit_count - 1U);
    const uint8_t mask = static_cast<uint8_t>(1U << static_cast<uint32_t>(bit % 8U));
    if ((bloom.bits[static_cast<size_t>(bit / 8U)] & mask) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace sniffer::internal
