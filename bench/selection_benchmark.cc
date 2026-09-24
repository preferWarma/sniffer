#include <arrow/api.h>
#include <arrow/util/config.h>
#include <benchmark/benchmark.h>

#include <bit>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "benchmark_build_config.h"
#include "codec_internal.h"

namespace {

enum class Representation { kIndices, kBitmap, kRanges };
enum class Distribution { kUniform, kClustered };

struct Measurement {
  uint64_t sum = 0;
  uint64_t selected_rows = 0;
  uint64_t representation_bytes = 0;  // Vector capacity, not allocator overhead.
  uint64_t ranges = 0;
};

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::vector<uint8_t> MakeMask(uint64_t rows, uint64_t selectivity_percent,
                              Distribution distribution) {
  std::vector<uint8_t> mask(static_cast<size_t>(rows), 0);
  const uint64_t clustered_rows = rows * selectivity_percent / 100U;
  for (uint64_t row = 0; row < rows; ++row) {
    const bool selected = distribution == Distribution::kClustered
                              ? row < clustered_rows
                              : Mix(row) % 100U < selectivity_percent;
    mask[static_cast<size_t>(row)] = static_cast<uint8_t>(selected);
  }
  return mask;
}

Measurement BuildAndConsume(Representation representation, std::span<const uint8_t> mask,
                            std::span<const uint64_t> values) {
  Measurement result;
  switch (representation) {
    case Representation::kIndices: {
      std::vector<uint64_t> indices;
      // Match the scan path's row-group-sized reserve when selectivity is unknown.
      indices.reserve(mask.size());
      for (size_t row = 0; row < mask.size(); ++row) {
        if (mask[row] != 0) {
          indices.push_back(static_cast<uint64_t>(row));
        }
      }
      auto* index_data = indices.data();
      benchmark::DoNotOptimize(index_data);
      for (const uint64_t row : indices) {
        result.sum += values[static_cast<size_t>(row)];
      }
      result.selected_rows = static_cast<uint64_t>(indices.size());
      result.representation_bytes = static_cast<uint64_t>(indices.capacity()) * sizeof(uint64_t);
      break;
    }
    case Representation::kBitmap: {
      std::vector<uint64_t> bits(mask.size() / 64U + (mask.size() % 64U != 0), 0);
      for (size_t row = 0; row < mask.size(); ++row) {
        if (mask[row] != 0) {
          bits[row / 64U] |= uint64_t{1} << (row % 64U);
        }
      }
      auto* bitmap_data = bits.data();
      benchmark::DoNotOptimize(bitmap_data);
      for (size_t word = 0; word < bits.size(); ++word) {
        uint64_t remaining = bits[word];
        while (remaining != 0) {
          const size_t bit = static_cast<size_t>(std::countr_zero(remaining));
          result.sum += values[word * 64U + bit];
          ++result.selected_rows;
          remaining &= remaining - 1U;
        }
      }
      result.representation_bytes = static_cast<uint64_t>(bits.capacity()) * sizeof(uint64_t);
      break;
    }
    case Representation::kRanges: {
      std::vector<std::pair<uint64_t, uint64_t>> ranges;
      size_t row = 0;
      while (row < mask.size()) {
        if (mask[row] == 0) {
          ++row;
          continue;
        }
        const size_t begin = row;
        do {
          ++row;
        } while (row < mask.size() && mask[row] != 0);
        ranges.emplace_back(static_cast<uint64_t>(begin), static_cast<uint64_t>(row));
      }
      auto* range_data = ranges.data();
      benchmark::DoNotOptimize(range_data);
      for (const auto& [begin, end] : ranges) {
        for (uint64_t selected = begin; selected < end; ++selected) {
          result.sum += values[static_cast<size_t>(selected)];
          ++result.selected_rows;
        }
      }
      result.representation_bytes =
          static_cast<uint64_t>(ranges.capacity()) * sizeof(std::pair<uint64_t, uint64_t>);
      result.ranges = static_cast<uint64_t>(ranges.size());
      break;
    }
  }
  return result;
}

void Selection(benchmark::State& state, Representation representation) {
  const auto rows = static_cast<uint64_t>(state.range(0));
  const auto selectivity_percent = static_cast<uint64_t>(state.range(1));
  const auto distribution = static_cast<Distribution>(state.range(2));
  const auto mask = MakeMask(rows, selectivity_percent, distribution);
  std::vector<uint64_t> values(static_cast<size_t>(rows));
  uint64_t expected_sum = 0;
  uint64_t expected_rows = 0;
  for (uint64_t row = 0; row < rows; ++row) {
    values[static_cast<size_t>(row)] = Mix(row + rows);
    if (mask[static_cast<size_t>(row)] != 0) {
      expected_sum += values[static_cast<size_t>(row)];
      ++expected_rows;
    }
  }
  const auto reference = BuildAndConsume(representation, mask, values);
  if (reference.sum != expected_sum || reference.selected_rows != expected_rows) {
    state.SkipWithError("selection representation differs from reference");
    return;
  }
  for (auto _ : state) {
    auto result = BuildAndConsume(representation, mask, values);
    benchmark::DoNotOptimize(result.sum);
  }
  state.counters["selected_rows"] = static_cast<double>(reference.selected_rows);
  state.counters["representation_bytes"] = static_cast<double>(reference.representation_bytes);
  state.counters["ranges"] = static_cast<double>(reference.ranges);
  state.counters["selectivity_percent"] = static_cast<double>(selectivity_percent);
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(rows));
}

void ApplySelectionMatrix(benchmark::internal::Benchmark* benchmark_case) {
  for (const int64_t percent : {int64_t{1}, int64_t{10}, int64_t{50}, int64_t{100}}) {
    for (const int64_t distribution : {int64_t{0}, int64_t{1}}) {
      benchmark_case->Args({100000, percent, distribution});
    }
  }
}

BENCHMARK_CAPTURE(Selection, Indices, Representation::kIndices)
    ->Apply(ApplySelectionMatrix)
    ->ArgNames({"rows", "selectivity_percent", "distribution"});
BENCHMARK_CAPTURE(Selection, Bitmap, Representation::kBitmap)
    ->Apply(ApplySelectionMatrix)
    ->ArgNames({"rows", "selectivity_percent", "distribution"});
BENCHMARK_CAPTURE(Selection, Ranges, Representation::kRanges)
    ->Apply(ApplySelectionMatrix)
    ->ArgNames({"rows", "selectivity_percent", "distribution"});

void PlainSelectedDecode(benchmark::State& state, Representation representation) {
  const auto rows = static_cast<uint64_t>(state.range(0));
  const auto percent = static_cast<uint64_t>(state.range(1));
  const auto distribution = static_cast<Distribution>(state.range(2));
  const auto mask = MakeMask(rows, percent, distribution);
  arrow::Int64Builder builder;
  if (!builder.Reserve(static_cast<int64_t>(rows)).ok()) {
    state.SkipWithError("failed to reserve Plain input");
    return;
  }
  std::vector<uint64_t> indices;
  std::vector<uint64_t> words(static_cast<size_t>((rows + 63U) / 64U), 0);
  for (uint64_t row = 0; row < rows; ++row) {
    const auto status =
        row % 17U == 0 ? builder.AppendNull() : builder.Append(static_cast<int64_t>(Mix(row)));
    if (!status.ok()) {
      state.SkipWithError(status.ToString().c_str());
      return;
    }
    if (mask[static_cast<size_t>(row)] != 0) {
      indices.push_back(row);
      words[static_cast<size_t>(row / 64U)] |= uint64_t{1} << (row % 64U);
    }
  }
  std::shared_ptr<arrow::Array> source;
  if (!builder.Finish(&source).ok()) {
    state.SkipWithError("failed to finish Plain input");
    return;
  }
  const sniffer::FieldSpec field{1, "value", arrow::int64(), true, nullptr};
  auto encoded = sniffer::internal::EncodePlain(field, *source);
  auto bitmap_result = sniffer::internal::BitmapSelection::Make(rows, std::move(words));
  if (!encoded.ok() || !bitmap_result.ok()) {
    state.SkipWithError("failed to prepare Plain decode");
    return;
  }
  const auto& payload = *encoded;
  const auto& bitmap = *bitmap_result;
  sniffer::internal::ColumnChunkMeta chunk;
  chunk.field_id = field.field_id;
  chunk.physical_type = sniffer::internal::PhysicalTypeId::kInt64;
  chunk.encoding_id = sniffer::internal::kPlainEncodingId;
  chunk.row_count = rows;
  chunk.null_count = static_cast<uint64_t>(source->null_count());
  chunk.length = payload.size();
  auto reference = sniffer::internal::DecodePlainSelected(field, chunk, payload, indices);
  auto candidate = sniffer::internal::DecodePlainSelectedBitmap(field, chunk, payload, bitmap);
  if (!reference.ok() || !candidate.ok() || !(*reference)->Equals(*candidate)) {
    state.SkipWithError("Plain bitmap decode differs from index decode");
    return;
  }
  for (auto _ : state) {
    auto decoded =
        representation == Representation::kIndices
            ? sniffer::internal::DecodePlainSelected(field, chunk, payload, indices)
            : sniffer::internal::DecodePlainSelectedBitmap(field, chunk, payload, bitmap);
    if (!decoded.ok()) {
      state.SkipWithError(decoded.status().ToString().c_str());
      break;
    }
    benchmark::DoNotOptimize((*decoded).get());
  }
  state.counters["selected_rows"] = static_cast<double>(indices.size());
  state.counters["selection_bytes"] = static_cast<double>(
      representation == Representation::kIndices ? indices.capacity() * sizeof(uint64_t)
                                                 : bitmap.bytes());
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(rows));
}

BENCHMARK_CAPTURE(PlainSelectedDecode, Indices, Representation::kIndices)
    ->Apply(ApplySelectionMatrix)
    ->ArgNames({"rows", "selectivity_percent", "distribution"});
BENCHMARK_CAPTURE(PlainSelectedDecode, Bitmap, Representation::kBitmap)
    ->Apply(ApplySelectionMatrix)
    ->ArgNames({"rows", "selectivity_percent", "distribution"});

}  // namespace

int main(int argc, char** argv) {
  benchmark::AddCustomContext("source_revision", SNIFFER_BENCHMARK_SOURCE_REVISION);
  benchmark::AddCustomContext("compiler", SNIFFER_BENCHMARK_COMPILER);
  benchmark::AddCustomContext("arrow_version", ARROW_VERSION_STRING);
  benchmark::AddCustomContext("distribution", "0=uniform_hash,1=clustered_prefix");
  benchmark::AddCustomContext("timed_work", "build_representation_and_sum_selected_values");
  benchmark::AddCustomContext("plain_decode_timed_work",
                              "prebuilt_selection_to_nullable_int64_Arrow_array");
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
