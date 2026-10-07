#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <eventree/event_tree.hpp>

namespace eventree {

// What a decoder has seen so far, in 16-bit words.
struct Evt3Statistics {
  std::uint64_t cd_events = 0;
  std::uint64_t time_high_words = 0;
  std::uint64_t skipped_words = 0;  // non-CD words (triggers, monitoring, IMU, continuations, EM events...)
};

// Decodes an EVT3 raw stream (16-bit little-endian words) into an event tree.
// EVT3 is stateful: address, vector base and time words set state that later words build on, so a multi-word event
// is never held back; chunks may end anywhere, even inside a word: the trailing byte is held until the next call.
// Non-CD words, and CD words before the first time high word, are skipped and counted. Timestamps must not go
// backwards (checked by assertion in debug builds only, by the tree).
class Evt3Decoder {
 public:
  template <Layout L>
  void decode(std::span<const std::byte> bytes, EventTree<L>& tree) {
    // Complete a word left over from the previous chunk first.
    if (has_pending_ && !bytes.empty()) {
      const std::byte pair[2] = {pending_, bytes.front()};
      bytes = bytes.subspan(1);
      has_pending_ = false;
      decode_word(load(pair), tree);
    }
    std::size_t i = 0;
    for (; i + sizeof(std::uint16_t) <= bytes.size(); i += sizeof(std::uint16_t)) decode_word(load(bytes.data() + i), tree);
    if (i < bytes.size()) {
      pending_ = bytes[i];
      has_pending_ = true;
    }
  }

  const Evt3Statistics& statistics() const { return statistics_; }

  // Forgets the stream's state; call before decoding an unrelated stream with a decoder already used.
  void reset() { *this = Evt3Decoder{}; }

 private:
  static std::uint16_t load(const std::byte* p) {
    std::uint16_t word;
    std::memcpy(&word, p, sizeof word);
    return word;
  }

  template <Layout L>
  void decode_word(std::uint16_t word, EventTree<L>& tree) {
    const std::uint16_t content = word & 0x0FFF;
    switch (word >> 12) {
      case addr_y:
        y_ = content & 0x7FF;
        is_cd_ = true;
        break;
      case addr_x:
        if (!cd_ready()) return skip();
        emit(content & 0x7FF, (content >> 11) & 1u, tree);
        break;
      case vect_base_x:
        if (!cd_ready()) return skip();
        base_x_ = content & 0x7FF;
        polarity_ = (content >> 11) & 1u;
        break;
      case vect_12:
        if (!cd_ready()) return skip();
        emit_vector(content, 12, tree);
        break;
      case vect_8:
        if (!cd_ready()) return skip();
        emit_vector(content & 0xFF, 8, tree);
        break;
      case time_low:
        time_low_ = content;
        break;
      case time_high:
        {
          // The wire value is 12 bits and wraps; unroll the wraps into the tree's 32-bit time high.
          statistics_.time_high_words += 1;
          has_time_high_ = true;
          if (content < last_wire_high_) loops_ += 1;
          // A new time high invalidates the time low until the time low word that follows it.
          if (content != last_wire_high_) time_low_ = 0;
          last_wire_high_ = content;
        }
        break;
      case em_addr_y:
        is_cd_ = false;
        skip();
        break;
      default:
        skip();
        break;
    }
  }

  // Events whose time is not known yet (before the first time high word) are dropped, as the reference decoder does.
  bool cd_ready() const { return is_cd_ && has_time_high_; }

  void skip() { statistics_.skipped_words += 1; }

  template <Layout L>
  void emit(std::uint16_t x, std::uint8_t polarity, EventTree<L>& tree) {
    statistics_.cd_events += 1;
    const std::uint32_t high = (loops_ << 18) | (std::uint32_t{last_wire_high_} << 6) | (time_low_ >> 6);
    tree.append(high, static_cast<std::uint8_t>(time_low_ & 0x3F), {x, y_, polarity});
  }

  // Emits one event per set bit, then moves the base past the bits the vector covered.
  template <Layout L>
  void emit_vector(std::uint16_t valid, unsigned width, EventTree<L>& tree) {
    for (unsigned bit = 0; bit < width; ++bit)
      if (valid & (1u << bit)) emit(static_cast<std::uint16_t>((base_x_ + bit) & 0x7FF), polarity_, tree);
    base_x_ = static_cast<std::uint16_t>((base_x_ + width) & 0x7FF);
  }

  static constexpr std::uint16_t addr_y = 0x0;
  static constexpr std::uint16_t em_addr_y = 0x1;
  static constexpr std::uint16_t addr_x = 0x2;
  static constexpr std::uint16_t vect_base_x = 0x3;
  static constexpr std::uint16_t vect_12 = 0x4;
  static constexpr std::uint16_t vect_8 = 0x5;
  static constexpr std::uint16_t time_low = 0x6;
  static constexpr std::uint16_t time_high = 0x8;

  std::uint16_t y_ = 0;
  std::uint16_t base_x_ = 0;
  std::uint8_t polarity_ = 0;
  bool is_cd_ = false;  // whether the last Y word was a CD one (events sharing a row follow it)
  bool has_time_high_ = false;
  std::uint16_t time_low_ = 0;
  std::uint16_t last_wire_high_ = 0;
  std::uint32_t loops_ = 0;
  Evt3Statistics statistics_;
  std::byte pending_{};
  bool has_pending_ = false;
};

}  // namespace eventree
