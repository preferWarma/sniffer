#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "sniffer/schema.h"

namespace sniffer::internal {

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
