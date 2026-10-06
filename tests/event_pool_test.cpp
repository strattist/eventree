#include <gtest/gtest.h>

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

}  // namespace
