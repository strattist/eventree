#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <concepts>
#include <iterator>
#include <vector>

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
  struct HighNode {
    std::uint32_t time_high;
    std::uint32_t first_low;
  };
  struct LowNode {
    std::uint8_t time_low;
    std::uint32_t first_event;
  };

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

  // The CD events sharing one timestamp: a view into the tree, valid until the tree is appended to or reset.
  class TimeNode {
   public:
    class const_iterator {
     public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = CdEvent;
      using difference_type = std::ptrdiff_t;
      using pointer = const CdEvent*;
      using reference = CdEvent;

      const_iterator() = default;

      CdEvent operator*() const {
        const CdPixel p = layout_->at(event_);
        return {p.x, p.y, p.polarity, time_};
      }
      const_iterator& operator++() {
        ++event_;
        return *this;
      }
      const_iterator operator++(int) {
        auto previous = *this;
        ++event_;
        return previous;
      }
      friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.event_ == b.event_; }

     private:
      friend class TimeNode;
      const_iterator(const L* layout, std::int64_t time, std::size_t event)
          : layout_(layout), time_(time), event_(event) {}

      const L* layout_ = nullptr;
      std::int64_t time_ = 0;
      std::size_t event_ = 0;
    };

    // Full normalised timestamp in µs, shared by every event of the node.
    std::int64_t time() const { return (std::int64_t{time_high_} << time_low_bits) | time_low_; }
    std::uint32_t time_high() const { return time_high_; }
    std::uint8_t time_low() const { return time_low_; }
    // Number of CD events in the node.
    std::size_t size() const { return last_ - first_; }

    const_iterator begin() const { return const_iterator(layout_, time(), first_); }
    const_iterator end() const { return const_iterator(layout_, time(), last_); }

   private:
    friend class EventTree;
    const L* layout_ = nullptr;
    std::uint32_t time_high_ = 0;
    std::uint8_t time_low_ = 0;
    std::size_t first_ = 0;
    std::size_t last_ = 0;
  };

  // Forward iterator over the time nodes, in timestamp order.
  class const_node_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = TimeNode;
    using difference_type = std::ptrdiff_t;
    using pointer = const TimeNode*;
    using reference = TimeNode;

    const_node_iterator() = default;

    TimeNode operator*() const { return tree_->node_at(high_, low_); }
    const_node_iterator& operator++() {
      ++low_;
      if (high_ + 1 < tree_->highs_.size() && low_ == tree_->highs_[high_ + 1].first_low) ++high_;
      return *this;
    }
    const_node_iterator operator++(int) {
      auto previous = *this;
      ++*this;
      return previous;
    }
    friend bool operator==(const const_node_iterator& a, const const_node_iterator& b) { return a.low_ == b.low_; }

   private:
    friend class EventTree;
    const_node_iterator(const EventTree* tree, std::size_t high, std::size_t low)
        : tree_(tree), high_(high), low_(low) {}

    const EventTree* tree_ = nullptr;
    std::size_t high_ = 0;
    std::size_t low_ = 0;
  };

  class TimeNodes {
   public:
    const_node_iterator begin() const { return tree_->node_begin(); }
    const_node_iterator end() const { return tree_->node_end(); }
    std::size_t size() const { return tree_->time_node_count(); }

   private:
    friend class EventTree;
    explicit TimeNodes(const EventTree* tree) : tree_(tree) {}
    const EventTree* tree_;
  };

  // Appends a CD event. Timestamps must be non-decreasing.
  void append(std::uint32_t time_high, std::uint8_t time_low, CdPixel pixel) {
    const bool new_high = !have_last_ || last_high_ != time_high;
    assert(!have_last_ || time_high >= last_high_);
    assert(new_high || time_low >= last_low_);
    if (new_high) highs_.push_back({time_high, static_cast<std::uint32_t>(lows_.size())});
    if (new_high || last_low_ != time_low) lows_.push_back({time_low, static_cast<std::uint32_t>(layout_.size())});
    layout_.append(pixel);
    have_last_ = true;
    last_high_ = time_high;
    last_low_ = time_low;
  }

  // Whether the layout offers the optional block interface (`pack`, `block_cursor`, `block_free`, `commit_block`).
  static constexpr bool has_block_path = requires(L& layout, std::size_t n) {
    { L::pack(CdPixel{}) } -> std::same_as<std::uint32_t>;
    { layout.block_cursor() } -> std::same_as<std::uint32_t*>;
    { layout.block_free() } -> std::same_as<std::size_t>;
    layout.commit_block(n);
  };

  // How many events a `BlockWriter` may append now: the room left in the current chunk of the fullest pool.
  // 0 when a pool is full (the next `append` enters a new chunk) or the layout has no block interface.
  std::size_t block_capacity() {
    if constexpr (has_block_path)
      return std::min({highs_.contiguous_free(), lows_.contiguous_free(), layout_.block_free()});
    else
      return 0;
  }

  // Appends up to `block_capacity()` events with no per-event capacity check and no unpredictable branch: the
  // candidate time-high and time-low nodes are always written, and only advance when the event opens a node.
  // Same result as calling `append` for each event. The tree must not be touched until `finish`, which commits
  // what was appended (a writer that is dropped without `finish` appends nothing).
  class BlockWriter {
   public:
    explicit BlockWriter(EventTree& tree)
        : tree_(tree),
          h_(tree.highs_.cursor()),
          l_(tree.lows_.cursor()),
          p_(tree.layout_.block_cursor()),
          low_count_(tree.lows_.size()),
          event_count_(tree.layout_.size()),
          have_last_(tree.have_last_),
          last_high_(tree.last_high_),
          last_low_(tree.last_low_) {}

    void append(std::uint32_t time_high, std::uint8_t time_low, CdPixel pixel) {
      assert(!have_last_ || time_high >= last_high_);
      const bool new_high = !have_last_ || last_high_ != time_high;
      assert(new_high || time_low >= last_low_);
      const bool new_low = new_high || last_low_ != time_low;
      *h_ = {time_high, static_cast<std::uint32_t>(low_count_)};
      *l_ = {time_low, static_cast<std::uint32_t>(event_count_)};
      *p_++ = L::pack(pixel);
      h_ += new_high;
      l_ += new_low;
      low_count_ += new_low;
      ++event_count_;
      have_last_ = true;
      last_high_ = time_high;
      last_low_ = time_low;
    }

    void finish() {
      tree_.highs_.advance(h_ - tree_.highs_.cursor());
      tree_.lows_.advance(l_ - tree_.lows_.cursor());
      tree_.layout_.commit_block(event_count_ - tree_.layout_.size());
      tree_.have_last_ = have_last_;
      tree_.last_high_ = last_high_;
      tree_.last_low_ = last_low_;
    }

   private:
    EventTree& tree_;
    HighNode* h_;
    LowNode* l_;
    std::uint32_t* p_;
    std::size_t low_count_, event_count_;
    bool have_last_;
    std::uint32_t last_high_;
    std::uint8_t last_low_;
  };

  // Number of CD events in the tree.
  std::size_t size() const { return layout_.size(); }

  // Number of time-high nodes and of time nodes (distinct timestamps) in the tree.
  std::size_t time_high_count() const { return highs_.size(); }
  std::size_t time_node_count() const { return lows_.size(); }

  // The time nodes, for iteration per timestamp.
  TimeNodes time_nodes() const { return TimeNodes(this); }

  // The `i`-th time node, `i < time_node_count()`.
  TimeNode time_node(std::size_t i) const {
    assert(i < lows_.size());
    // Last time-high node whose first time-low index is <= i.
    std::size_t lo = 0, hi = highs_.size();
    while (hi - lo > 1) {
      const std::size_t mid = lo + (hi - lo) / 2;
      (highs_[mid].first_low <= i ? lo : hi) = mid;
    }
    return node_at(lo, i);
  }

  // All CD events as a vector, with normalised timestamps.
  std::vector<CdEvent> to_events() const {
    std::vector<CdEvent> events;
    events.reserve(size());
    events.insert(events.end(), begin(), end());
    return events;
  }

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
    have_last_ = false;
  }

  // Bytes held by the tree's pools (allocated chunks, whether filled or not).
  std::size_t memory_use() const { return highs_.memory_use() + lows_.memory_use() + layout_.memory_use(); }

  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, layout_.size()); }

 private:
  const_node_iterator node_begin() const { return const_node_iterator(this, 0, 0); }
  const_node_iterator node_end() const { return const_node_iterator(this, highs_.size(), lows_.size()); }

  TimeNode node_at(std::size_t high, std::size_t low) const {
    TimeNode node;
    node.layout_ = &layout_;
    node.time_high_ = highs_[high].time_high;
    node.time_low_ = lows_[low].time_low;
    node.first_ = lows_[low].first_event;
    node.last_ = low + 1 < lows_.size() ? lows_[low + 1].first_event : layout_.size();
    return node;
  }

  // The last time high and time low appended, remembered so that `append` does not read them back from the pools.
  bool have_last_ = false;
  std::uint32_t last_high_ = 0;
  std::uint8_t last_low_ = 0;
  EventPool<HighNode> highs_;
  EventPool<LowNode> lows_;
  L layout_;
};

}  // namespace eventree
