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
    const std::size_t words = bytes.size() / sizeof(std::uint16_t);
    std::size_t w = 0;
    while (w < words) {
      // A block is bounded by the room left in the tree's current pool chunks, at the worst case of one vector word's
      // events per word; when there is less room (or the layout has no block interface) one word goes through the
      // plain path, which enters the next chunk.
      std::size_t count = 0;
      if constexpr (EventTree<L>::has_block_path) count = std::min(tree.block_capacity() / max_events_per_word, words - w);
      if (count == 0) {
        decode_word(load(bytes.data() + w * sizeof(std::uint16_t)), tree);
        ++w;
        continue;
      }
      if constexpr (EventTree<L>::has_block_path) {
        decode_block(bytes.data() + w * sizeof(std::uint16_t), count, tree);
        w += count;
      }
    }
    const std::size_t i = words * sizeof(std::uint16_t);
    if (i < bytes.size()) {
      pending_ = bytes[i];
      has_pending_ = true;
    }
  }

  const Evt3Statistics& statistics() const { return statistics_; }

  // Forgets the stream's state; call before decoding an unrelated stream with a decoder already used.
  void reset() { *this = Evt3Decoder{}; }

 private:
  static constexpr std::size_t max_events_per_word = 12;  // a 12-bit vector word

  // Decodes `count` words, each possibly emitting up to `max_events_per_word` events (the caller sized the block
  // for that), with the decoder's state in locals so that nothing is reloaded from memory between words.
  template <Layout L>
  void decode_block(const std::byte* data, std::size_t count, EventTree<L>& tree) {
    typename EventTree<L>::BlockWriter writer(tree);
    std::uint16_t y = y_, base_x = base_x_, time_low_now = time_low_, last_wire_high = last_wire_high_;
    std::uint8_t polarity = polarity_;
    bool is_cd = is_cd_, has_time_high = has_time_high_;
    std::uint32_t loops = loops_;
    std::uint64_t cd_events = 0, time_high_words = 0, skipped_words = 0;

    auto emit = [&](std::uint16_t x, std::uint8_t pol) {
      ++cd_events;
      const std::uint32_t high = (loops << 18) | (std::uint32_t{last_wire_high} << 6) | (time_low_now >> 6);
      writer.append(high, static_cast<std::uint8_t>(time_low_now & 0x3F), {x, y, pol});
    };
    auto emit_vector = [&](std::uint16_t valid, unsigned width) {
      for (unsigned bit = 0; bit < width; ++bit)
        if (valid & (1u << bit)) emit(static_cast<std::uint16_t>((base_x + bit) & 0x7FF), polarity);
      base_x = static_cast<std::uint16_t>((base_x + width) & 0x7FF);
    };

    for (std::size_t n = 0; n < count; ++n) {
      const std::uint16_t word = load(data + n * sizeof(std::uint16_t));
      const std::uint16_t content = word & 0x0FFF;
      const bool ready = is_cd && has_time_high;
      switch (word >> 12) {
        case addr_y:
          y = content & 0x7FF;
          is_cd = true;
          break;
        case addr_x:
          if (!ready) { ++skipped_words; break; }
          emit(content & 0x7FF, (content >> 11) & 1u);
          break;
        case vect_base_x:
          if (!ready) { ++skipped_words; break; }
          base_x = content & 0x7FF;
          polarity = (content >> 11) & 1u;
          break;
        case vect_12:
          if (!ready) { ++skipped_words; break; }
          emit_vector(content, 12);
          break;
        case vect_8:
          if (!ready) { ++skipped_words; break; }
          emit_vector(content & 0xFF, 8);
          break;
        case time_low:
          time_low_now = content;
          break;
        case time_high:
          ++time_high_words;
          has_time_high = true;
          if (content < last_wire_high) ++loops;
          if (content != last_wire_high) time_low_now = 0;
          last_wire_high = content;
          break;
        case em_addr_y:
          is_cd = false;
          ++skipped_words;
          break;
        default:
          ++skipped_words;
          break;
      }
    }
    writer.finish();
    y_ = y;
    base_x_ = base_x;
    time_low_ = time_low_now;
    last_wire_high_ = last_wire_high;
    polarity_ = polarity;
    is_cd_ = is_cd;
    has_time_high_ = has_time_high;
    loops_ = loops;
    statistics_.cd_events += cd_events;
    statistics_.time_high_words += time_high_words;
    statistics_.skipped_words += skipped_words;
  }

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
