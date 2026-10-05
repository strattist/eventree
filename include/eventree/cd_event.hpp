#pragma once

#include <cstdint>

namespace eventree {

// A CD event as read back from a tree: position, polarity and the full
// normalised timestamp in microseconds.
struct CdEvent {
  std::uint16_t x;
  std::uint16_t y;
  std::uint8_t polarity;
  std::int64_t t;

  friend bool operator==(const CdEvent&, const CdEvent&) = default;
};

}  // namespace eventree
