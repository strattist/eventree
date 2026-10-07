#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include <eventree/evt2_decoder.hpp>

#include "evt2_encoder.hpp"

namespace {

using eventree::CdEvent;

constexpr std::size_t small_chunk = 8;

std::vector<CdEvent> make_events(int count, int per_timestamp = 3) {
  std::vector<CdEvent> events;
  for (int i = 0; i < count; ++i)
    events.push_back({static_cast<std::uint16_t>(i % 1280), static_cast<std::uint16_t>(i % 720),
                      static_cast<std::uint8_t>(i & 1), static_cast<std::int64_t>(i / per_timestamp) * 7});
  return events;
}

std::vector<CdEvent> contents(const eventree::EventTree<>& tree) { return {tree.begin(), tree.end()}; }

TEST(EventPool, IterationStaysCorrectAcrossManyChunks) {
  const auto events = make_events(1000);
  eventree::EventTree<> tree(small_chunk);
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  EXPECT_EQ(contents(tree), events);
  EXPECT_EQ(tree.size(), events.size());
}

TEST(EventPool, GrowthKeepsEarlierDataReadable) {
  eventree::EventTree<> tree(small_chunk);
  eventree::Evt2Decoder decoder;
  const auto events = make_events(200);
  std::vector<CdEvent> so_far;
  for (const auto& e : events) {
    decoder.decode(eventree::test::encode_evt2({e}), tree);
    so_far.push_back(e);
    if (so_far.size() % 37 == 0) {
      EXPECT_EQ(contents(tree), so_far);
    }
  }
}

TEST(EventPool, MemoryUseGrowsInWholeChunksAsEventsAreAdded) {
  eventree::EventTree<> tree(small_chunk);
  EXPECT_EQ(tree.memory_use(), 0u);
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(make_events(1)), tree);
  const auto one = tree.memory_use();
  EXPECT_GT(one, 0u);
  decoder.decode(eventree::test::encode_evt2(make_events(1000)), tree);
  EXPECT_GT(tree.memory_use(), one);
}

TEST(EventPool, ReserveMakesTheFollowingDecodeAllocateNothing) {
  const auto events = make_events(1000);
  const auto bytes = eventree::test::encode_evt2(events);
  eventree::EventTree<> tree(small_chunk);
  tree.reserve(events.size());
  const auto reserved = tree.memory_use();
  EXPECT_GT(reserved, 0u);
  eventree::Evt2Decoder decoder;
  decoder.decode(bytes, tree);
  EXPECT_EQ(tree.memory_use(), reserved);
  EXPECT_EQ(contents(tree), events);
}

TEST(EventPool, ResetKeepsChunksAndASecondDecodeAllocatesNothing) {
  const auto events = make_events(1000);
  const auto bytes = eventree::test::encode_evt2(events);
  eventree::EventTree<> tree(small_chunk);
  {
    eventree::Evt2Decoder decoder;
    decoder.decode(bytes, tree);
  }
  const auto used = tree.memory_use();
  tree.reset();
  EXPECT_EQ(tree.size(), 0u);
  EXPECT_TRUE(contents(tree).empty());
  EXPECT_EQ(tree.memory_use(), used);
  eventree::Evt2Decoder decoder;
  decoder.decode(bytes, tree);
  tree.reset();
  decoder.reset();
  decoder.decode(bytes, tree);
  EXPECT_EQ(tree.memory_use(), used);
  EXPECT_EQ(contents(tree), events);
}

TEST(EventPool, BytesPerEventCanBeComputedFromMemoryUse) {
  const auto events = make_events(100'000, 4);
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  const double bytes_per_event = static_cast<double>(tree.memory_use()) / static_cast<double>(tree.size());
  EXPECT_LT(bytes_per_event, 16.0);
}

// Decodes `bytes` in pieces of `piece` bytes.
void decode_in_pieces(const std::vector<std::byte>& bytes, std::size_t piece, eventree::EventTree<>& tree) {
  eventree::Evt2Decoder decoder;
  for (std::size_t at = 0; at < bytes.size(); at += piece)
    decoder.decode(std::span{bytes}.subspan(at, std::min(piece, bytes.size() - at)), tree);
}

TEST(EventPool, DecodeChunkSizesEqualToAndAroundThePoolChunkSizeDecodeTheSame) {
  const auto events = make_events(500);
  const auto bytes = eventree::test::encode_evt2(events);
  for (const std::size_t piece : {small_chunk * 4 - 1, small_chunk * 4, small_chunk * 4 + 1, small_chunk, small_chunk + 1}) {
    eventree::EventTree<> tree(small_chunk);
    decode_in_pieces(bytes, piece, tree);
    EXPECT_EQ(contents(tree), events) << "piece " << piece;
  }
}

TEST(EventPool, TimestampsRepeatingAcrossAPoolChunkBoundaryStayOneNode) {
  // 3 events per timestamp with 8-event chunks: some timestamps straddle the layout and node chunk boundaries.
  for (const int per_timestamp : {2, 3, 5, 8, 9, 20}) {
    const auto events = make_events(200, per_timestamp);
    eventree::EventTree<> tree(small_chunk);
    eventree::Evt2Decoder decoder;
    decoder.decode(eventree::test::encode_evt2(events), tree);
    EXPECT_EQ(contents(tree), events) << per_timestamp << " per timestamp";
    std::size_t expected_nodes = 0;
    for (std::size_t i = 0; i < events.size(); ++i)
      if (i == 0 || events[i].t != events[i - 1].t) ++expected_nodes;
    EXPECT_EQ(tree.time_node_count(), expected_nodes) << per_timestamp << " per timestamp";
  }
}

TEST(EventPool, ResetThenDecodingAnEarlierStreamIntoTheSameTreeStartsFresh) {
  const std::vector<CdEvent> later{{1, 1, 1, 5000}, {2, 2, 0, 5000}, {3, 3, 1, 9000}};
  const std::vector<CdEvent> earlier{{4, 4, 0, 10}, {5, 5, 1, 10}, {6, 6, 0, 11}};
  eventree::EventTree<> tree(small_chunk);
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(later), tree);
  tree.reset();
  decoder.reset();
  decoder.decode(eventree::test::encode_evt2(earlier), tree);
  EXPECT_EQ(contents(tree), earlier);
  EXPECT_EQ(tree.time_high_count(), 1u);
  EXPECT_EQ(tree.time_node_count(), 2u);
}

TEST(EventPool, ResetThenSameFirstTimestampStillOpensANewNode) {
  const std::vector<CdEvent> events{{1, 1, 1, 100}, {2, 2, 0, 100}};
  eventree::EventTree<> tree(small_chunk);
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  tree.reset();
  decoder.reset();
  decoder.decode(eventree::test::encode_evt2(events), tree);
  EXPECT_EQ(contents(tree), events);
  EXPECT_EQ(tree.time_high_count(), 1u);
  EXPECT_EQ(tree.time_node_count(), 1u);
}

TEST(EventPool, ReserveThenDecodeAcrossTimeHighWrapsAllocatesNothing) {
  const std::int64_t loop = 1LL << 34;
  std::vector<CdEvent> events;
  for (int i = 0; i < 100; ++i)
    events.push_back({static_cast<std::uint16_t>(i), 1, static_cast<std::uint8_t>(i & 1), loop - 20 + i * 3});
  eventree::EventTree<> tree(small_chunk);
  tree.reserve(events.size());
  const auto reserved = tree.memory_use();
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  EXPECT_EQ(tree.memory_use(), reserved);
  EXPECT_EQ(contents(tree), events);
}

// A layout other than the packed one: events kept in a plain vector.
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

TEST(EventPool, ACustomLayoutSatisfyingTheConceptWorksWithTheTree) {
  static_assert(eventree::Layout<VectorLayout>);
  const auto events = make_events(300);
  eventree::EventTree<VectorLayout> tree(small_chunk);
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
  tree.reset();
  EXPECT_EQ(tree.size(), 0u);
}

}  // namespace
