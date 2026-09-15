#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sniffer::benchmark {

struct SampleStats {
  std::vector<double> samples;
  double minimum = 0;
  double p50 = 0;
  double p95 = 0;
  double coefficient_of_variation = 0;
};

inline SampleStats SummarizeSamples(std::vector<double> samples) {
  SampleStats result;
  result.samples = std::move(samples);
  if (result.samples.empty()) {
    return result;
  }

  auto sorted = result.samples;
  std::sort(sorted.begin(), sorted.end());
  result.minimum = sorted.front();
  const size_t midpoint = sorted.size() / 2U;
  result.p50 = sorted.size() % 2U == 0U ? (sorted[midpoint - 1U] + sorted[midpoint]) / 2.0
                                        : sorted[midpoint];
  const size_t p95_rank = (sorted.size() * 95U + 99U) / 100U;
  result.p95 = sorted[p95_rank - 1U];

  const double mean = std::accumulate(result.samples.begin(), result.samples.end(), 0.0) /
                      static_cast<double>(result.samples.size());
  double squared_deviation_sum = 0;
  for (const double sample : result.samples) {
    const double deviation = sample - mean;
    squared_deviation_sum += deviation * deviation;
  }
  if (mean != 0) {
    result.coefficient_of_variation =
        std::sqrt(squared_deviation_sum / static_cast<double>(result.samples.size())) / mean;
  }
  return result;
}

inline void PrintSampleStats(std::ostream& output, std::string_view prefix,
                             const SampleStats& stats) {
  output << ' ' << prefix << "_min_ms=" << stats.minimum << ' ' << prefix << "_p50_ms=" << stats.p50
         << ' ' << prefix << "_p95_ms=" << stats.p95 << ' ' << prefix
         << "_cv=" << stats.coefficient_of_variation << ' ' << prefix << "_samples_ms=";
  for (size_t index = 0; index < stats.samples.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << stats.samples[index];
  }
}

inline void PrintJsonString(std::ostream& output, std::string_view value) {
  output << '"';
  for (const char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << character;
        break;
    }
  }
  output << '"';
}

inline void PrintJsonSampleStats(std::ostream& output, const SampleStats& stats) {
  output << "{\"min_ms\":" << stats.minimum << ",\"p50_ms\":" << stats.p50
         << ",\"p95_ms\":" << stats.p95 << ",\"cv\":" << stats.coefficient_of_variation
         << ",\"samples_ms\":[";
  for (size_t index = 0; index < stats.samples.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << stats.samples[index];
  }
  output << "]}";
}

inline void PrintJsonArguments(std::ostream& output, int argc, char* const* argv) {
  output << '[';
  for (int index = 0; index < argc; ++index) {
    if (index != 0) {
      output << ',';
    }
    PrintJsonString(output, argv[index]);
  }
  output << ']';
}

}  // namespace sniffer::benchmark
