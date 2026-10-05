#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <eventree/cd_event.hpp>

namespace eventree::test {

// Test-only: builds an EVT2 raw stream from CD events sorted by timestamp.
inline std::vector<std::byte> encode_evt2(const std::vector<CdEvent>& events) {
  std::vector<std::byte> out;
  auto push = [&out](std::uint32_t word) {
    const auto at = out.size();
    out.resize(at + sizeof word);
    std::memcpy(out.data() + at, &word, sizeof word);
  };
  bool have_high = false;
  std::uint32_t high = 0;
  for (const CdEvent& e : events) {
    const auto h = static_cast<std::uint32_t>((e.t >> 6) & 0x0FFFFFFF);
    if (!have_high || h != high) {
      push((0x8u << 28) | h);
      high = h;
      have_high = true;
    }
    push(((e.polarity ? 0x1u : 0x0u) << 28) | (static_cast<std::uint32_t>(e.t & 0x3F) << 22) |
         (std::uint32_t{e.x} << 11) | e.y);
  }
  return out;
}

}  // namespace eventree::test
