#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <eventree/evt2_decoder.hpp>

#include "evt2_encoder.hpp"

namespace {

using eventree::CdEvent;

std::vector<CdEvent> decode_all(const std::vector<CdEvent>& events) {
  const auto bytes = eventree::test::encode_evt2(events);
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(bytes, tree);
  return {tree.begin(), tree.end()};
}

TEST(Evt2Decode, EmptyStreamYieldsNoEvents) { EXPECT_TRUE(decode_all({}).empty()); }

TEST(Evt2Decode, SingleEventRoundTrips) {
  const std::vector<CdEvent> events{{12, 34, 1, 1000}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt2Decode, EventsAcrossTimeHighAndLowRoundTripInOrder) {
  const std::vector<CdEvent> events{
      {0, 0, 0, 0},         {1, 2, 1, 5},          {1279, 719, 1, 63},      {3, 4, 0, 64},
      {2047, 2047, 1, 130}, {7, 8, 0, 10'000'000}, {9, 10, 1, 10'000'063},  {11, 12, 0, (1LL << 34) - 1},
  };
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt2Decode, EventsSharingATimestampAreAllReturnedInStreamOrder) {
  const std::vector<CdEvent> events{{1, 1, 1, 500}, {2, 2, 0, 500}, {3, 3, 1, 500}, {4, 4, 1, 501}, {5, 5, 0, 501}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt2Decode, TimeHighLoopsAreUnrolledIntoAMonotonicTimestamp) {
  const std::int64_t loop = 1LL << 34;
  const std::vector<CdEvent> events{{1, 1, 1, loop - 1}, {2, 2, 0, loop + 5}, {4, 4, 0, 2 * loop - 1}, {3, 3, 1, 2 * loop + 70}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt2Decode, DecodeAppendsToAnExistingTree) {
  const auto first = eventree::test::encode_evt2({{1, 1, 1, 100}});
  const auto second = eventree::test::encode_evt2({{2, 2, 0, 200}});
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(first, tree);
  decoder.decode(second, tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}),
            (std::vector<CdEvent>{{1, 1, 1, 100}, {2, 2, 0, 200}}));
}

std::vector<std::byte> words_to_bytes(const std::vector<std::uint32_t>& words) {
  std::vector<std::byte> out(words.size() * sizeof(std::uint32_t));
  if (!out.empty()) std::memcpy(out.data(), words.data(), out.size());
  return out;
}

TEST(Evt2Robustness, StreamSplitAtEveryByteOffsetDecodesLikeTheUnsplitStream) {
  const std::int64_t loop = 1LL << 34;
  const std::vector<CdEvent> events{{1, 2, 1, 5},          {3, 4, 0, 5},          {5, 6, 1, 64},
                                    {7, 8, 0, loop - 1},   {9, 10, 1, loop + 3},  {11, 12, 0, loop + 70}, {13, 14, 1, 2 * loop + 1}};
  const auto bytes = eventree::test::encode_evt2(events);
  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    eventree::EventTree<> tree;
    eventree::Evt2Decoder decoder;
    decoder.decode(std::span{bytes}.first(cut), tree);
    decoder.decode(std::span{bytes}.subspan(cut), tree);
    EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events) << "cut at " << cut;
  }
}

TEST(Evt2Robustness, ByteByByteFeedingDecodesLikeTheUnsplitStream) {
  const std::vector<CdEvent> events{{1, 2, 1, 5}, {3, 4, 0, 100}, {5, 6, 1, (1LL << 34) + 7}};
  const auto bytes = eventree::test::encode_evt2(events);
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  for (std::size_t i = 0; i < bytes.size(); ++i) decoder.decode(std::span{bytes}.subspan(i, 1), tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
}

TEST(Evt2Robustness, NonCdWordsAreSkippedAndCounted) {
  const auto bytes = words_to_bytes({
      0x8u << 28 | 1,                             // time high
      0x0u << 28 | 3u << 22 | 10u << 11 | 20u,    // CD off
      0xAu << 28 | 0x123,                         // external trigger
      0xEu << 28 | 0x1,                           // other
      0xFu << 28,                                 // continued / padding
      0x1u << 28 | 4u << 22 | 11u << 11 | 21u,    // CD on
      0x0u << 28 | 5u << 22 | 12u << 11 | 22u,    // CD off
  });
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(bytes, tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}),
            (std::vector<CdEvent>{{10, 20, 0, 64 + 3}, {11, 21, 1, 64 + 4}, {12, 22, 0, 64 + 5}}));
  const auto& stats = decoder.statistics();
  EXPECT_EQ(stats.cd_events, 3u);
  EXPECT_EQ(stats.time_high_words, 1u);
  EXPECT_EQ(stats.skipped_words, 3u);
}

TEST(Evt2Robustness, StatisticsAccumulateAcrossCallsAndResetClearsThem) {
  const auto bytes = words_to_bytes({0x8u << 28, 0xAu << 28, 0x1u << 28});
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(bytes, tree);
  decoder.decode(bytes, tree);
  EXPECT_EQ(decoder.statistics().skipped_words, 2u);
  decoder.reset();
  EXPECT_EQ(decoder.statistics().skipped_words, 0u);
  EXPECT_EQ(decoder.statistics().cd_events, 0u);
}

TEST(Evt2Robustness, EmptyAndSubWordInputChangeNothing) {
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode({}, tree);
  const std::byte partial[3]{};
  decoder.decode(partial, tree);  // held as a pending partial word, nothing decoded
  EXPECT_EQ(tree.size(), 0u);
  EXPECT_EQ(decoder.statistics().cd_events, 0u);
  EXPECT_EQ(decoder.statistics().skipped_words, 0u);
}

#ifndef NDEBUG
TEST(Evt2RobustnessDeathTest, TimeGoingBackwardsTripsAnAssertion) {
  const auto bytes = words_to_bytes({0x8u << 28 | 5, 0x1u << 28 | 9u << 22, 0x1u << 28 | 2u << 22});
  EXPECT_DEATH(
      {
        eventree::EventTree<> tree;
        eventree::Evt2Decoder decoder;
        decoder.decode(bytes, tree);
      },
      "");
}
#endif

}  // namespace

namespace {

TEST(Evt2Decode, CdEventsBeforeTheFirstTimeHighAreSkippedLikeTheReferenceDecoder) {
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  // Two CD words with no time known, then time high 1, then one CD word (time low 3).
  decoder.decode(words_to_bytes({(0x1u << 28) | (5u << 22), (0x0u << 28), (0x8u << 28) | 1u, (0x1u << 28) | (3u << 22)}),
                 tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), (std::vector<CdEvent>{{0, 0, 1, 67}}));
  EXPECT_EQ(decoder.statistics().cd_events, 1u);
  EXPECT_EQ(decoder.statistics().skipped_words, 2u);
}

}  // namespace
