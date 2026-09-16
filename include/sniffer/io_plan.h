#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace sniffer {

struct Predicate {
  enum class Op { kEq, kNe, kLt, kLe, kGt, kGe, kIsNull, kIsNotNull };

  uint32_t field_id = 0;
  Op op = Op::kEq;
  std::shared_ptr<arrow::Scalar> value;
};

struct SortKeyRange {
  std::optional<std::vector<std::shared_ptr<arrow::Scalar>>> lower;
  std::optional<std::vector<std::shared_ptr<arrow::Scalar>>> upper;
  bool lower_inclusive = true;
  bool upper_inclusive = false;
};

struct IOPlan {
  std::vector<uint32_t> projection_field_ids;
  std::vector<Predicate> conjunctive_predicates;
  std::optional<SortKeyRange> sort_key_range;
  std::optional<uint64_t> limit;
  uint32_t output_batch_rows = 64 * 1024;
};

struct ScanMetrics {
  uint64_t row_groups_considered = 0;
  uint64_t row_groups_pruned = 0;
  uint64_t column_chunks_read = 0;
  uint64_t predicate_chunks_decoded = 0;
  uint64_t projection_chunks_decoded = 0;
  uint64_t chunk_bytes_read = 0;
  uint64_t pruning_nanoseconds = 0;
  uint64_t chunk_io_nanoseconds = 0;
  uint64_t chunk_checksum_nanoseconds = 0;
  uint64_t decode_nanoseconds = 0;
  uint64_t predicate_nanoseconds = 0;
  uint64_t projection_nanoseconds = 0;
  uint64_t batch_materialization_nanoseconds = 0;
};

}  // namespace sniffer
