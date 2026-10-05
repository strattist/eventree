#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <eventree/layout.hpp>

namespace eventree {

// One 32-bit word per CD event: y (11 bits), x (11 bits), polarity (1 bit).
class PackedLayout {
 public:
  void append(CdPixel pixel) {
    words_.push_back((std::uint32_t{pixel.y} << 12) | (std::uint32_t{pixel.x} << 1) |
                     (pixel.polarity & 1u));
  }

  std::size_t size() const { return words_.size(); }

  CdPixel at(std::size_t i) const {
    const std::uint32_t w = words_[i];
    return {static_cast<std::uint16_t>((w >> 1) & 0x7FF), static_cast<std::uint16_t>((w >> 12) & 0x7FF),
            static_cast<std::uint8_t>(w & 1u)};
  }

 private:
  std::vector<std::uint32_t> words_;
};

static_assert(Layout<PackedLayout>);

}  // namespace eventree
