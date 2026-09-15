#include "index_internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>

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

arrow::Status BloomInsert(const FieldSpec& field, const arrow::Scalar& value, BloomMeta* bloom) {
  ARROW_ASSIGN_OR_RAISE(const uint64_t first, HashScalar(field, value, 0x243F6A8885A308D3ULL));
  ARROW_ASSIGN_OR_RAISE(const uint64_t raw_second, HashScalar(field, value, 0x13198A2E03707344ULL));
  const uint64_t second = raw_second | 1U;
  for (uint32_t probe = 0; probe < bloom->hash_count; ++probe) {
    const uint64_t bit = (first + static_cast<uint64_t>(probe) * second) & (bloom->bit_count - 1U);
    bloom->bits[static_cast<size_t>(bit / 8U)] |=
        static_cast<uint8_t>(1U << static_cast<uint32_t>(bit % 8U));
  }
  return arrow::Status::OK();
}

}  // namespace

arrow::Status ValidateAndUpdateSortOrder(
    const TableSchema& schema, const LayoutPolicy& layout, const arrow::RecordBatch& batch,
    std::vector<std::shared_ptr<arrow::Scalar>>* previous_key) {
  if (layout.sort_key_field_ids.empty() || batch.num_rows() == 0) {
    return arrow::Status::OK();
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
                                                const arrow::RecordBatch& batch) {
  RowGroupIndex result;
  result.statistics.reserve(layout.statistics_field_ids.size());
  result.blooms.reserve(layout.bloom_field_ids.size());
  result.sort_keys.reserve(layout.sort_key_field_ids.size());

  for (const uint32_t field_id : layout.statistics_field_ids) {
    ARROW_ASSIGN_OR_RAISE(const size_t field_index, FieldIndex(schema, field_id));
    const auto& array = batch.column(static_cast<int>(field_index));
    StatisticsMeta statistics;
    statistics.field_id = field_id;
    statistics.null_count = static_cast<uint64_t>(array->null_count());
    bool saw_nan = false;
    for (int64_t row = 0; row < array->length(); ++row) {
      if (array->IsNull(row)) {
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(auto value, array->GetScalar(row));
      if (ScalarHasNaN(*value)) {
        saw_nan = true;
        continue;
      }
      if (!statistics.min) {
        statistics.min = value;
        statistics.max = std::move(value);
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(const int min_order, CompareScalars(*value, *statistics.min));
      ARROW_ASSIGN_OR_RAISE(const int max_order, CompareScalars(*value, *statistics.max));
      if (min_order < 0) {
        statistics.min = value;
      }
      if (max_order > 0) {
        statistics.max = std::move(value);
      }
    }
    if (saw_nan) {
      statistics.min.reset();
      statistics.max.reset();
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
      ARROW_ASSIGN_OR_RAISE(auto value, array->GetScalar(row));
      if (!ScalarHasNaN(*value)) {
        ARROW_RETURN_NOT_OK(BloomInsert(field, *value, &bloom));
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
