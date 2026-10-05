#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include <eventree/cd_event.hpp>
#include <eventree/layout.hpp>
#include <eventree/packed_layout.hpp>

namespace eventree {

// Time high -> time low -> the CD events sharing that timestamp.
// The timestamp in µs is (time_high << 6) | time_low, whatever the wire format.
template <Layout L = PackedLayout>
class EventTree {
 public:
  static constexpr unsigned time_low_bits = 6;

  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = CdEvent;
    using difference_type = std::ptrdiff_t;
    using pointer = const CdEvent*;
    using reference = CdEvent;

    const_iterator() = default;

    CdEvent operator*() const {
      const CdPixel p = tree_->layout_.at(event_);
      const std::int64_t t = (std::int64_t{tree_->highs_[high_].time_high} << time_low_bits) |
                             tree_->lows_[low_].time_low;
      return {p.x, p.y, p.polarity, t};
    }

    const_iterator& operator++() {
      ++event_;
      if (event_ < tree_->layout_.size() && low_ + 1 < tree_->lows_.size() &&
          event_ == tree_->lows_[low_ + 1].first_event) {
        ++low_;
        if (high_ + 1 < tree_->highs_.size() && low_ == tree_->highs_[high_ + 1].first_low) ++high_;
      }
      return *this;
    }

    const_iterator operator++(int) {
      auto previous = *this;
      ++*this;
      return previous;
    }

    friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.event_ == b.event_; }

   private:
    friend class EventTree;
    const_iterator(const EventTree* tree, std::size_t event) : tree_(tree), event_(event) {}

    const EventTree* tree_ = nullptr;
    std::size_t high_ = 0;
    std::size_t low_ = 0;
    std::size_t event_ = 0;
  };

  // Appends a CD event. Timestamps must be non-decreasing.
  void append(std::uint32_t time_high, std::uint8_t time_low, CdPixel pixel) {
    const bool new_high = highs_.empty() || highs_.back().time_high != time_high;
    assert(highs_.empty() || time_high >= highs_.back().time_high);
    assert(new_high || time_low >= lows_.back().time_low);
    if (new_high) highs_.push_back({time_high, static_cast<std::uint32_t>(lows_.size())});
    if (new_high || lows_.back().time_low != time_low)
      lows_.push_back({time_low, static_cast<std::uint32_t>(layout_.size())});
    layout_.append(pixel);
  }

  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, layout_.size()); }

 private:
  struct HighNode {
    std::uint32_t time_high;
    std::uint32_t first_low;
  };
  struct LowNode {
    std::uint8_t time_low;
    std::uint32_t first_event;
  };

  std::vector<HighNode> highs_;
  std::vector<LowNode> lows_;
  L layout_;
};

}  // namespace eventree
