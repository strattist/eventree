#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <eventree/event_tree.hpp>

namespace eventree {

// What a decoder has seen so far, in 32-bit words.
struct Evt2Statistics {
  std::uint64_t cd_events = 0;
  std::uint64_t time_high_words = 0;
  std::uint64_t skipped_words = 0;  // non-CD words other than time high (triggers, monitoring, IMU, padding...)
};

// Decodes an EVT2 raw stream (32-bit little-endian words) into an event tree.
// Chunks may end anywhere, even inside a word: the trailing bytes are held until the next call.
// Non-CD words, and CD words before the first time high word, are skipped and counted. Timestamps must not go backwards (checked by assertion in debug builds
// only, by the tree).
class Evt2Decoder {
 public:
  template <Layout L>
  void decode(std::span<const std::byte> bytes, EventTree<L>& tree) {
    // Complete a word left over from the previous chunk first.
    if (pending_size_ != 0) {
      const std::size_t take = std::min(sizeof(std::uint32_t) - pending_size_, bytes.size());
      std::memcpy(pending_ + pending_size_, bytes.data(), take);
      pending_size_ += take;
      bytes = bytes.subspan(take);
      if (pending_size_ < sizeof(std::uint32_t)) return;
      std::uint32_t word;
      std::memcpy(&word, pending_, sizeof word);
      pending_size_ = 0;
      decode_word(word, tree);
    }
    const std::size_t words = bytes.size() / sizeof(std::uint32_t);
    std::size_t w = 0;
    while (w < words) {
      // Blocks are bounded by the room left in the tree's current pool chunks; when a pool is full (or the layout has
      // no block interface) one word goes through the plain path, which enters the next chunk.
      const std::size_t capacity = tree.block_capacity();
      if (capacity == 0) {
        std::uint32_t word;
        std::memcpy(&word, bytes.data() + w * sizeof word, sizeof word);
        decode_word(word, tree);
        ++w;
        continue;
      }
      if constexpr (EventTree<L>::has_block_path) {
        const std::size_t count = std::min(capacity, words - w);
        decode_block(bytes.data() + w * sizeof(std::uint32_t), count, tree);
        w += count;
      }
    }
    const std::size_t i = words * sizeof(std::uint32_t);
    pending_size_ = bytes.size() - i;
    if (pending_size_ != 0) std::memcpy(pending_, bytes.data() + i, pending_size_);
  }

  const Evt2Statistics& statistics() const { return statistics_; }

  // Forgets the stream's state; call before decoding an unrelated stream with a decoder already used.
  void reset() { *this = Evt2Decoder{}; }

 private:
  // Decodes `count` words (at most the tree's block capacity) with the decoder's state in locals, so that nothing is
  // reloaded from memory between words.
  template <Layout L>
  void decode_block(const std::byte* data, std::size_t count, EventTree<L>& tree) {
    typename EventTree<L>::BlockWriter writer(tree);
    bool have_time_high = have_time_high_;
    std::uint32_t time_high_now = time_high_, last_wire_high = last_wire_high_, loops = loops_;
    std::uint64_t cd_events = 0, time_high_words = 0, skipped_words = 0;
    for (std::size_t n = 0; n < count; ++n) {
      std::uint32_t word;
      std::memcpy(&word, data + n * sizeof word, sizeof word);
      const std::uint32_t type = word >> 28;
      if (type <= cd_on) {
        if (!have_time_high) [[unlikely]] {
          ++skipped_words;
          continue;
        }
        ++cd_events;
        writer.append(time_high_now, static_cast<std::uint8_t>((word >> 22) & 0x3F),
                      {static_cast<std::uint16_t>((word >> 11) & 0x7FF), static_cast<std::uint16_t>(word & 0x7FF),
                       static_cast<std::uint8_t>(type)});
      } else if (type == time_high) {
        ++time_high_words;
        have_time_high = true;
        const std::uint32_t value = word & 0x0FFFFFFF;
        if (value < last_wire_high) ++loops;
        last_wire_high = value;
        time_high_now = (loops << 28) | value;
      } else {
        ++skipped_words;
      }
    }
    writer.finish();
    have_time_high_ = have_time_high;
    time_high_ = time_high_now;
    last_wire_high_ = last_wire_high;
    loops_ = loops;
    statistics_.cd_events += cd_events;
    statistics_.time_high_words += time_high_words;
    statistics_.skipped_words += skipped_words;
  }

  template <Layout L>
  void decode_word(std::uint32_t word, EventTree<L>& tree) {
    switch (word >> 28) {
      case cd_off:
      case cd_on:
        // Without a time high the timestamp is unknown: skip, as the reference decoder does.
        if (!have_time_high_) {
          statistics_.skipped_words += 1;
          break;
        }
        statistics_.cd_events += 1;
        tree.append(time_high_, static_cast<std::uint8_t>((word >> 22) & 0x3F),
                    {static_cast<std::uint16_t>((word >> 11) & 0x7FF), static_cast<std::uint16_t>(word & 0x7FF),
                     static_cast<std::uint8_t>(word >> 28)});
        break;
      case time_high:
        {
          // The wire value is 28 bits and wraps; unroll the wraps into the tree's 32-bit time high.
          statistics_.time_high_words += 1;
          have_time_high_ = true;
          const std::uint32_t value = word & 0x0FFFFFFF;
          if (value < last_wire_high_) loops_ += 1;
          last_wire_high_ = value;
          time_high_ = (loops_ << 28) | value;
        }
        break;
      default:
        statistics_.skipped_words += 1;
        break;
    }
  }

  static constexpr std::uint32_t cd_off = 0x0;
  static constexpr std::uint32_t cd_on = 0x1;
  static constexpr std::uint32_t time_high = 0x8;

  bool have_time_high_ = false;
  std::uint32_t time_high_ = 0;
  std::uint32_t last_wire_high_ = 0;
  std::uint32_t loops_ = 0;
  Evt2Statistics statistics_;
  std::byte pending_[sizeof(std::uint32_t)]{};
  std::size_t pending_size_ = 0;
};

}  // namespace eventree
