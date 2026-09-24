#pragma once

#include <arrow/result.h>
#include <arrow/status.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace sniffer::internal {

// Internal, in-memory alternative to the index vector. Bits are stored in
// ascending row order; no selection representation is persisted in a Segment.
class BitmapSelection {
 public:
  static arrow::Result<BitmapSelection> Make(uint64_t row_count, std::vector<uint64_t> words) {
    if (row_count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        row_count / 64U + (row_count % 64U != 0) != words.size()) {
      return arrow::Status::Invalid("[sniffer.scan.selection] invalid bitmap length");
    }
    if (row_count % 64U != 0 && !words.empty() && (words.back() >> (row_count % 64U)) != 0) {
      return arrow::Status::Invalid("[sniffer.scan.selection] bitmap has out-of-range bits");
    }
    uint64_t count = 0;
    for (const uint64_t word : words) {
      count += static_cast<uint64_t>(std::popcount(word));
    }
    return BitmapSelection(row_count, count, std::move(words));
  }

  class Iterator {
   public:
    using value_type = uint64_t;
    using difference_type = std::ptrdiff_t;
    using reference = uint64_t;
    using pointer = void;
    using iterator_category = std::input_iterator_tag;

    Iterator(const BitmapSelection* owner, size_t word_index)
        : owner_(owner), word_index_(word_index) {
      AdvanceWord();
    }

    uint64_t operator*() const {
      return static_cast<uint64_t>(word_index_) * 64U +
             static_cast<uint64_t>(std::countr_zero(remaining_));
    }
    Iterator& operator++() {
      remaining_ &= remaining_ - 1U;
      if (remaining_ == 0) {
        ++word_index_;
        AdvanceWord();
      }
      return *this;
    }
    Iterator operator++(int) {
      Iterator previous = *this;
      ++(*this);
      return previous;
    }
    bool operator==(const Iterator& other) const {
      return owner_ == other.owner_ && word_index_ == other.word_index_ &&
             remaining_ == other.remaining_;
    }
    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    void AdvanceWord() {
      while (word_index_ < owner_->words_.size()) {
        remaining_ = owner_->words_[word_index_];
        if (remaining_ != 0) {
          return;
        }
        ++word_index_;
      }
      remaining_ = 0;
    }
    const BitmapSelection* owner_;
    size_t word_index_;
    uint64_t remaining_ = 0;
  };

  Iterator begin() const { return Iterator(this, 0); }
  Iterator end() const { return Iterator(this, words_.size()); }
  uint64_t size() const { return selected_count_; }
  uint64_t row_count() const { return row_count_; }
  uint64_t bytes() const { return static_cast<uint64_t>(words_.size()) * sizeof(uint64_t); }

 private:
  BitmapSelection(uint64_t row_count, uint64_t selected_count, std::vector<uint64_t> words)
      : row_count_(row_count), selected_count_(selected_count), words_(std::move(words)) {}
  uint64_t row_count_;
  uint64_t selected_count_;
  std::vector<uint64_t> words_;
};

}  // namespace sniffer::internal
