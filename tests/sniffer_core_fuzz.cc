#include <arrow/api.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "sniffer/io_plan.h"
#include "sniffer/segment_reader.h"

namespace {

void ExerciseInput(const uint8_t* data, size_t size) {
  static uint64_t counter = 0;
  const auto path = std::filesystem::temp_directory_path() /
                    ("sniffer_core_fuzz_" + std::to_string(counter++) + ".seg");
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      return;
    }
    if (size != 0) {
      stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
  }

  auto reader = sniffer::SegmentReader::Open(path.string());
  if (reader.ok()) {
    (void)reader.ValueUnsafe()->VerifyFileChecksum();
    (void)reader.ValueUnsafe()->ReadAll();
    sniffer::IOPlan plan;
    plan.output_batch_rows = 17;
    plan.limit = 31;
    auto scan = reader.ValueUnsafe()->Scan(std::move(plan));
    if (scan.ok()) {
      while (true) {
        auto batch = scan.ValueUnsafe().Next();
        if (!batch.ok() || !batch.ValueUnsafe()) {
          break;
        }
      }
    }
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ExerciseInput(data, size);
  return 0;
}

#ifdef SNIFFER_FUZZ_STANDALONE
int main() {
  std::vector<uint8_t> input((std::istreambuf_iterator<char>(std::cin)),
                             std::istreambuf_iterator<char>());
  if (!input.empty()) {
    ExerciseInput(input.data(), input.size());
    return 0;
  }

  std::mt19937_64 random(0xF00DBAADULL);
  const std::array<size_t, 12> sizes = {0, 1, 7, 31, 32, 39, 64, 127, 256, 1024, 4096, 16384};
  for (const size_t size : sizes) {
    std::vector<uint8_t> corpus(size);
    for (auto& byte : corpus) {
      byte = static_cast<uint8_t>(random());
    }
    ExerciseInput(corpus.data(), corpus.size());
  }
  std::cout << "fuzz smoke corpus cases=" << sizes.size() << '\n';
  return 0;
}
#endif
