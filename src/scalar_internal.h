#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "sniffer/schema.h"

namespace sniffer::internal {

class ArrayValuePairHasher {
 public:
  [[nodiscard]] static arrow::Result<ArrayValuePairHasher> Bind(const FieldSpec& field,
                                                                const arrow::Array& array);
  [[nodiscard]] arrow::Result<std::pair<uint64_t, uint64_t>> Hash(int64_t row, uint64_t first_seed,
                                                                  uint64_t second_seed) const;
  // The caller must establish that row is in bounds and non-null.
  [[nodiscard]] std::pair<uint64_t, uint64_t> HashKnownValid(int64_t row, uint64_t first_seed,
                                                             uint64_t second_seed) const;

 private:
  ArrayValuePairHasher(const FieldSpec* field, const arrow::Array* array, uint8_t physical_type)
      : field_(field), array_(array), physical_type_(physical_type) {}

  const FieldSpec* field_;
  const arrow::Array* array_;
  uint8_t physical_type_;
};

[[nodiscard]] bool ScalarHasNaN(const arrow::Scalar& scalar);
[[nodiscard]] arrow::Result<int> CompareScalars(const arrow::Scalar& left,
                                                const arrow::Scalar& right);
[[nodiscard]] arrow::Result<std::vector<uint8_t>> SerializeScalar(const FieldSpec& field,
                                                                  const arrow::Scalar& scalar);
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Scalar>> ParseScalar(
    const FieldSpec& field, std::span<const uint8_t> bytes);
[[nodiscard]] arrow::Result<uint64_t> HashScalar(const FieldSpec& field,
                                                 const arrow::Scalar& scalar, uint64_t seed);
[[nodiscard]] arrow::Result<uint64_t> HashArrayValue(const FieldSpec& field,
                                                     const arrow::Array& array, int64_t row,
                                                     uint64_t seed);
[[nodiscard]] arrow::Result<std::pair<uint64_t, uint64_t>> HashArrayValuePair(
    const FieldSpec& field, const arrow::Array& array, int64_t row, uint64_t first_seed,
    uint64_t second_seed);

}  // namespace sniffer::internal
