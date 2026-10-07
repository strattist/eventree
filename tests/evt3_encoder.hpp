#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <eventree/cd_event.hpp>

namespace eventree::test {

// Test-only: builds an EVT3 raw stream from CD events sorted by timestamp.
// With `use_vectors`, consecutive events sharing time, row and polarity with increasing x become one vector
// (8-bit if every offset fits, else 12-bit); everything else is a single event. Read back, the events come out
// in the same order as the input.
inline std::vector<std::byte> encode_evt3(const std::vector<CdEvent>& events, bool use_vectors = true) {
  std::vector<std::byte> out;
  auto push = [&out](unsigned type, unsigned content) {
    const auto word = static_cast<std::uint16_t>((type << 12) | (content & 0x0FFF));
    const auto at = out.size();
    out.resize(at + sizeof word);
    std::memcpy(out.data() + at, &word, sizeof word);
  };
  bool have_time = false, have_y = false;
  unsigned high = 0, low = 0, y = 0;
  for (std::size_t i = 0; i < events.size();) {
    const CdEvent& e = events[i];
    const auto h = static_cast<unsigned>((e.t >> 12) & 0xFFF);
    const auto l = static_cast<unsigned>(e.t & 0xFFF);
    if (!have_time || h != high) {
      push(0x8, h);
      push(0x6, l);
    } else if (l != low) {
      push(0x6, l);
    }
    high = h;
    low = l;
    have_time = true;
    if (!have_y || e.y != y) {
      push(0x0, e.y);
      y = e.y;
      have_y = true;
    }
    const unsigned pol = e.polarity ? 1u : 0u;
    std::size_t run = 1;
    if (use_vectors) {
      while (i + run < events.size()) {
        const CdEvent& n = events[i + run];
        if (n.t != e.t || n.y != e.y || n.polarity != e.polarity || n.x <= events[i + run - 1].x ||
            n.x - e.x >= 12)
          break;
        ++run;
      }
    }
    if (run == 1) {
      push(0x2, (pol << 11) | e.x);
    } else {
      unsigned valid = 0;
      for (std::size_t k = 0; k < run; ++k) valid |= 1u << (events[i + k].x - e.x);
      push(0x3, (pol << 11) | e.x);
      if (valid < 0x100) push(0x5, valid);
      else push(0x4, valid);
    }
    i += run;
  }
  return out;
}

}  // namespace eventree::test
