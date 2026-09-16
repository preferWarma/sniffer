#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <span>
#include <vector>

#include "format_internal.h"
#include "sniffer/layout.h"
#include "sniffer/schema.h"

namespace sniffer::internal {

[[nodiscard]] bool EncodingSupports(uint16_t encoding_id, const arrow::DataType& type);
[[nodiscard]] arrow::Result<uint16_t> SelectEncoding(const FieldSpec& field,
                                                     const arrow::Array& array,
                                                     const LayoutPolicy& layout);
[[nodiscard]] arrow::Result<uint64_t> PlainEncodedSize(const FieldSpec& field,
                                                       const arrow::Array& array);
[[nodiscard]] arrow::Result<std::vector<uint8_t>> EncodePlain(const FieldSpec& field,
                                                              const arrow::Array& array);
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> DecodePlain(
    const FieldSpec& field, const ColumnChunkMeta& chunk, std::span<const uint8_t> payload);
[[nodiscard]] arrow::Result<std::vector<uint8_t>> EncodeNonPlain(uint16_t encoding_id,
                                                                 const FieldSpec& field,
                                                                 const arrow::Array& array);
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> DecodeNonPlain(
    const FieldSpec& field, const ColumnChunkMeta& chunk, std::span<const uint8_t> payload,
    const std::vector<uint64_t>* selection = nullptr);

}  // namespace sniffer::internal
