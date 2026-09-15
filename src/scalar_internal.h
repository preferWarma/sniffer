#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <span>
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

}  // namespace sniffer::internal
