#pragma once

#include <cstdint>
#include <vector>

namespace sniffer::internal {

struct EncodingSampleAnalysis {
  uint32_t field_id = 0;
  int64_t sample_rows = 0;
  uint64_t dictionary_count = 0;
  uint64_t runs = 0;
  uint64_t minimum_bits = 0;
  uint64_t maximum_bits = 0;
  bool have_extrema = false;
};

struct RowGroupAnalysis {
  uint32_t encoding_sample_rows = 0;
  std::vector<EncodingSampleAnalysis> encoding_samples;
};

}  // namespace sniffer::internal
