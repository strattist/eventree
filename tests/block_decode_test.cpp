// The decoders' block path (EventTree::BlockWriter) against the plain per-event path, for EVT2 and EVT3, around the
// pool chunk boundaries: what must not change is what the tree returns when read back.
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <eventree/evt2_decoder.hpp>
#include <eventree/evt3_decoder.hpp>

#include "evt2_encoder.hpp"
#include "evt3_encoder.hpp"

namespace {

using eventree::CdEvent;

// A layout without the block interface, so trees using it take the plain `append` path.
class VectorLayout {
 public:
  explicit VectorLayout(std::size_t) {}
  void append(eventree::CdPixel p) { pixels_.push_back(p); }
  void reserve(std::size_t n) { pixels_.reserve(n); }
  void reset() { pixels_.clear(); }
  std::size_t memory_use() const { return pixels_.capacity() * sizeof(eventree::CdPixel); }
  std::size_t size() const { return pixels_.size(); }
  eventree::CdPixel at(std::size_t i) const { return pixels_[i]; }

 private:
  std::vector<eventree::CdPixel> pixels_;
};

static_assert(eventree::EventTree<>::has_block_path);
static_assert(!eventree::EventTree<VectorLayout>::has_block_path);

template <class Decoder, class Tree>
void decode_in_pieces(const std::vector<std::byte>& bytes, std::size_t piece, Tree& tree) {
  Decoder decoder;
  for (std::size_t at = 0; at < bytes.size(); at += piece)
    decoder.decode(std::span{bytes}.subspan(at, std::min(piece, bytes.size() - at)), tree);
}

enum class Wire { evt2, evt3 };

std::vector<std::byte> encode(Wire wire, const std::vector<CdEvent>& events) {
  return wire == Wire::evt2 ? eventree::test::encode_evt2(events) : eventree::test::encode_evt3(events);
}

template <class Tree>
void decode(Wire wire, const std::vector<std::byte>& bytes, std::size_t piece, Tree& tree) {
  if (wire == Wire::evt2)
    decode_in_pieces<eventree::Evt2Decoder>(bytes, piece, tree);
  else
    decode_in_pieces<eventree::Evt3Decoder>(bytes, piece, tree);
}

// Events with `per_timestamp` events per timestamp, `gap` µs between timestamps, and runs of consecutive x in a row
// (so EVT3 uses vector words, which emit several events from one word).
std::vector<CdEvent> make_events(int count, int per_timestamp, std::int64_t gap) {
  std::vector<CdEvent> events;
  for (int i = 0; i < count; ++i) {
    const int slot = i % per_timestamp;
    events.push_back({static_cast<std::uint16_t>(10 + slot), static_cast<std::uint16_t>((i / per_timestamp) % 700),
                      static_cast<std::uint8_t>((i / per_timestamp) & 1), (i / per_timestamp) * gap});
  }
  return events;
}

std::size_t expected_nodes(const std::vector<CdEvent>& events) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < events.size(); ++i)
    if (i == 0 || events[i].t != events[i - 1].t) ++n;
  return n;
}

class BlockDecode : public ::testing::TestWithParam<Wire> {};

TEST_P(BlockDecode, MatchesTheInputForPoolChunksAndInputPiecesAroundTheirBoundaries) {
  const Wire wire = GetParam();
  // gap 1: timestamps share a time high (the highs pool barely grows); gap 1<<20: every timestamp is a new time high.
  for (const std::int64_t gap : {std::int64_t{1}, std::int64_t{7}, std::int64_t{1} << 20}) {
    for (const int per_timestamp : {1, 3, 12, 13, 40}) {
      const auto events = make_events(700, per_timestamp, gap);
      const auto bytes = encode(wire, events);
      for (const std::size_t chunk : {8u, 16u, 64u, 128u, 100u}) {
        for (const std::size_t piece : {1u, 2u, 3u, 7u, 62u, 64u, 66u, 4096u, 1u << 20}) {
          eventree::EventTree<> tree(chunk);
          decode(wire, bytes, piece, tree);
          ASSERT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events)
              << "gap " << gap << " per_timestamp " << per_timestamp << " chunk " << chunk << " piece " << piece;
          ASSERT_EQ(tree.time_node_count(), expected_nodes(events))
              << "gap " << gap << " per_timestamp " << per_timestamp << " chunk " << chunk << " piece " << piece;
        }
      }
    }
  }
}

TEST_P(BlockDecode, BlockPathAndPlainPathBuildTheSameTree) {
  const Wire wire = GetParam();
  const auto events = make_events(3000, 5, 3);
  const auto bytes = encode(wire, events);
  for (const std::size_t chunk : {8u, 64u, 1024u}) {
    eventree::EventTree<> packed(chunk);
    eventree::EventTree<VectorLayout> plain(chunk);
    decode(wire, bytes, 1u << 20, packed);
    decode(wire, bytes, 1u << 20, plain);
    EXPECT_EQ((std::vector<CdEvent>{packed.begin(), packed.end()}), (std::vector<CdEvent>{plain.begin(), plain.end()}));
    EXPECT_EQ(packed.time_high_count(), plain.time_high_count());
    EXPECT_EQ(packed.time_node_count(), plain.time_node_count());
  }
}

TEST_P(BlockDecode, ReserveThenDecodeAllocatesNothingAndResetThenDecodeAgainIsIdentical) {
  const Wire wire = GetParam();
  const auto events = make_events(2000, 3, 2);
  const auto bytes = encode(wire, events);
  eventree::EventTree<> tree(64);
  tree.reserve(events.size());
  const auto reserved = tree.memory_use();
  decode(wire, bytes, 4096, tree);
  EXPECT_EQ(tree.memory_use(), reserved);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
  tree.reset();
  EXPECT_EQ(tree.size(), 0u);
  decode(wire, bytes, 100, tree);
  EXPECT_EQ(tree.memory_use(), reserved);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
}

TEST_P(BlockDecode, ResetThenSameFirstTimestampOpensANewNodeInTheBlockPath) {
  const Wire wire = GetParam();
  const std::vector<CdEvent> events{{1, 1, 1, 100}, {2, 2, 0, 100}};
  eventree::EventTree<> tree(64);
  decode(wire, encode(wire, events), 1u << 20, tree);
  tree.reset();
  decode(wire, encode(wire, events), 1u << 20, tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
  EXPECT_EQ(tree.time_high_count(), 1u);
  EXPECT_EQ(tree.time_node_count(), 1u);
}

TEST_P(BlockDecode, TimeHighWrapsAreUnrolledAcrossBlocks) {
  const Wire wire = GetParam();
  const std::int64_t loop = wire == Wire::evt2 ? (1LL << 34) : (1LL << 24);
  std::vector<CdEvent> events;
  for (int i = 0; i < 300; ++i)
    events.push_back({static_cast<std::uint16_t>(i), 1, static_cast<std::uint8_t>(i & 1),
                      loop * (i / 100 + 1) - 250 + (i % 100) * 5});  // each group of 100 crosses a wrap
  for (const std::size_t chunk : {8u, 64u}) {
    eventree::EventTree<> tree(chunk);
    decode(wire, encode(wire, events), 61, tree);
    EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events) << "chunk " << chunk;
  }
}

INSTANTIATE_TEST_SUITE_P(Formats, BlockDecode, ::testing::Values(Wire::evt2, Wire::evt3),
                         [](const auto& info) { return std::string(info.param == Wire::evt2 ? "Evt2" : "Evt3"); });

TEST(BlockDecodeEvt3, VectorWordsStraddlingPoolChunkEndsKeepEveryEvent) {
  // Rows of 30 consecutive pixels at one timestamp: 12- and 8-bit vector words emitting several events at once,
  // against pool chunks smaller and larger than a vector word's worst case.
  std::vector<CdEvent> events;
  for (int row = 0; row < 40; ++row)
    for (int x = 0; x < 30; ++x) events.push_back({static_cast<std::uint16_t>(100 + x), static_cast<std::uint16_t>(row), 1, row * 10});
  const auto bytes = eventree::test::encode_evt3(events);
  for (const std::size_t chunk : {8u, 11u, 12u, 13u, 16u, 24u, 30u, 64u}) {
    eventree::EventTree<> tree(chunk);
    eventree::Evt3Decoder decoder;
    decoder.decode(bytes, tree);
    EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events) << "chunk " << chunk;
    EXPECT_EQ(decoder.statistics().cd_events, events.size());
  }
}

TEST(BlockDecodeEvt3, ACustomLayoutWithoutTheBlockInterfaceStillDecodes) {
  const auto events = make_events(500, 4, 3);
  eventree::EventTree<VectorLayout> tree(16);
  eventree::Evt3Decoder decoder;
  decoder.decode(eventree::test::encode_evt3(events), tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
}

}  // namespace
