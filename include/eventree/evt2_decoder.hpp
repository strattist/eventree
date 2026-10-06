#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <eventree/event_tree.hpp>

namespace eventree {

// Decodes an EVT2 raw stream (32-bit little-endian words) into an event tree.
// Non-CD words are skipped. `decode` expects whole words.
class Evt2Decoder {
 public:
  template <Layout L>
  void decode(std::span<const std::byte> bytes, EventTree<L>& tree) {
    assert(bytes.size() % sizeof(std::uint32_t) == 0);
    for (std::size_t i = 0; i + sizeof(std::uint32_t) <= bytes.size(); i += sizeof(std::uint32_t)) {
      std::uint32_t word;
      std::memcpy(&word, bytes.data() + i, sizeof word);
      switch (word >> 28) {
        case cd_off:
        case cd_on:
          tree.append(time_high_, static_cast<std::uint8_t>((word >> 22) & 0x3F),
                      {static_cast<std::uint16_t>((word >> 11) & 0x7FF), static_cast<std::uint16_t>(word & 0x7FF),
                       static_cast<std::uint8_t>(word >> 28)});
          break;
        case time_high:
          {
            // The wire value is 28 bits and wraps; unroll the wraps into the tree's 32-bit time high.
            const std::uint32_t value = word & 0x0FFFFFFF;
            if (value < last_wire_high_) loops_ += 1;
            last_wire_high_ = value;
            time_high_ = (loops_ << 28) | value;
          }
          break;
        default:
          break;
      }
    }
  }

 // Forgets the stream's time state; call before decoding an unrelated stream with a decoder already used.
  void reset() { *this = Evt2Decoder{}; }

 private:
  static constexpr std::uint32_t cd_off = 0x0;
  static constexpr std::uint32_t cd_on = 0x1;
  static constexpr std::uint32_t time_high = 0x8;

  std::uint32_t time_high_ = 0;
  std::uint32_t last_wire_high_ = 0;
  std::uint32_t loops_ = 0;
};

}  // namespace eventree
