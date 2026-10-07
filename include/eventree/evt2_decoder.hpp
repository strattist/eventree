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
// Non-CD words are skipped and counted. Timestamps must not go backwards (checked by assertion in debug builds
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
    std::size_t i = 0;
    for (; i + sizeof(std::uint32_t) <= bytes.size(); i += sizeof(std::uint32_t)) {
      std::uint32_t word;
      std::memcpy(&word, bytes.data() + i, sizeof word);
      decode_word(word, tree);
    }
    pending_size_ = bytes.size() - i;
    if (pending_size_ != 0) std::memcpy(pending_, bytes.data() + i, pending_size_);
  }

  const Evt2Statistics& statistics() const { return statistics_; }

  // Forgets the stream's state; call before decoding an unrelated stream with a decoder already used.
  void reset() { *this = Evt2Decoder{}; }

 private:
  template <Layout L>
  void decode_word(std::uint32_t word, EventTree<L>& tree) {
    switch (word >> 28) {
      case cd_off:
      case cd_on:
        statistics_.cd_events += 1;
        tree.append(time_high_, static_cast<std::uint8_t>((word >> 22) & 0x3F),
                    {static_cast<std::uint16_t>((word >> 11) & 0x7FF), static_cast<std::uint16_t>(word & 0x7FF),
                     static_cast<std::uint8_t>(word >> 28)});
        break;
      case time_high:
        {
          // The wire value is 28 bits and wraps; unroll the wraps into the tree's 32-bit time high.
          statistics_.time_high_words += 1;
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

  std::uint32_t time_high_ = 0;
  std::uint32_t last_wire_high_ = 0;
  std::uint32_t loops_ = 0;
  Evt2Statistics statistics_;
  std::byte pending_[sizeof(std::uint32_t)]{};
  std::size_t pending_size_ = 0;
};

}  // namespace eventree
