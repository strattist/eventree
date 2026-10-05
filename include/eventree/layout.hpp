#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace eventree {

// The spatial part of a CD event, as stored under a time node.
struct CdPixel {
  std::uint16_t x;
  std::uint16_t y;
  std::uint8_t polarity;

  friend bool operator==(const CdPixel&, const CdPixel&) = default;
};

// What a layout must provide: appending a CD event, counting the events it
// holds, and reading event `i` back (events are indexed in append order).
template <class L>
concept Layout = requires(L layout, const L clayout, CdPixel pixel, std::size_t i) {
  layout.append(pixel);
  { clayout.size() } -> std::same_as<std::size_t>;
  { clayout.at(i) } -> std::same_as<CdPixel>;
};

}  // namespace eventree
