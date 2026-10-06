#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <eventree/event_pool.hpp>
#include <eventree/cd_event.hpp>
#include <eventree/layout.hpp>
#include <eventree/packed_layout.hpp>

namespace eventree {

// Time high -> time low -> the CD events sharing that timestamp, stored in event pools.
// One tree serves one stream and is single-threaded: no reader (iteration, `size`) may run while a writer
// (`append`, a decoder) is appending, and appending never invalidates data already written but iterators
// are not meant to be held across appends.
// The timestamp in µs is (time_high << 6) | time_low, whatever the wire format.
template <Layout L = PackedLayout>
class EventTree {
 public:
  static constexpr unsigned time_low_bits = 6;

  // `chunk_size` is the number of elements per pool chunk (rounded up to a power of two).
  explicit EventTree(std::size_t chunk_size = EventPool<std::uint32_t>::default_chunk_size)
      : highs_(chunk_size), lows_(chunk_size), layout_(chunk_size) {}

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

  // Number of CD events in the tree.
  std::size_t size() const { return layout_.size(); }

  // Allocates storage up front for `expected_events` CD events, so that appending that many allocates nothing.
  // Sized for one time-low node per event and one time-high node per 16 events at worst typical density;
  // a stream with sparser timestamps may still grow the pool.
  void reserve(std::size_t expected_events) {
    layout_.reserve(expected_events);
    lows_.reserve(expected_events);
    highs_.reserve(expected_events / 16 + 1);
  }

  // Empties the tree but keeps all allocated chunks, so decoding into it again allocates nothing.
  void reset() {
    highs_.reset();
    lows_.reset();
    layout_.reset();
  }

  // Bytes held by the tree's pools (allocated chunks, whether filled or not).
  std::size_t memory_use() const { return highs_.memory_use() + lows_.memory_use() + layout_.memory_use(); }

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

  EventPool<HighNode> highs_;
  EventPool<LowNode> lows_;
  L layout_;
};

}  // namespace eventree
