#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <eventree/evt3_decoder.hpp>

#include "evt3_encoder.hpp"

namespace {

using eventree::CdEvent;

std::vector<CdEvent> decode_all(const std::vector<CdEvent>& events, bool use_vectors = true) {
  const auto bytes = eventree::test::encode_evt3(events, use_vectors);
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  decoder.decode(bytes, tree);
  return {tree.begin(), tree.end()};
}

std::vector<std::byte> words_to_bytes(const std::vector<std::uint16_t>& words) {
  std::vector<std::byte> out(words.size() * sizeof(std::uint16_t));
  if (!out.empty()) std::memcpy(out.data(), words.data(), out.size());
  return out;
}

std::vector<CdEvent> decode_words(const std::vector<std::uint16_t>& words, eventree::Evt3Statistics* stats = nullptr) {
  const auto bytes = words_to_bytes(words);
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  decoder.decode(bytes, tree);
  if (stats) *stats = decoder.statistics();
  return {tree.begin(), tree.end()};
}

TEST(Evt3Decode, EmptyStreamYieldsNoEvents) { EXPECT_TRUE(decode_all({}).empty()); }

TEST(Evt3Decode, SingleEventRoundTrips) {
  const std::vector<CdEvent> events{{12, 34, 1, 1000}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt3Decode, EventsAcrossTimeHighAndLowRoundTripInOrder) {
  const std::vector<CdEvent> events{
      {0, 0, 0, 0},         {1, 2, 1, 5},          {1279, 719, 1, 63},      {3, 4, 0, 64},
      {2047, 2047, 1, 130}, {7, 8, 0, 10'000'000}, {9, 10, 1, 10'000'063},  {11, 12, 0, (1LL << 24) - 1},
  };
  EXPECT_EQ(decode_all(events, false), events);
  EXPECT_EQ(decode_all(events, true), events);
}

TEST(Evt3Decode, EventsSharingATimestampAreAllReturnedInStreamOrder) {
  const std::vector<CdEvent> events{{1, 1, 1, 500}, {2, 2, 0, 500}, {3, 3, 1, 500}, {4, 4, 1, 501}, {5, 5, 0, 501}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt3Decode, TimeLoopsAreUnrolledIntoAMonotonicTimestamp) {
  const std::int64_t loop = 1LL << 24;
  const std::vector<CdEvent> events{{1, 1, 1, loop - 1}, {2, 2, 0, loop + 5}, {4, 4, 0, 2 * loop - 1}, {3, 3, 1, 2 * loop + 70}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt3Decode, DecodeAppendsToAnExistingTree) {
  const auto first = eventree::test::encode_evt3({{1, 1, 1, 100}});
  const auto second = eventree::test::encode_evt3({{2, 2, 0, 200}});
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  decoder.decode(first, tree);
  decoder.decode(second, tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}),
            (std::vector<CdEvent>{{1, 1, 1, 100}, {2, 2, 0, 200}}));
}

TEST(Evt3Vectors, EightBitVectorDecodesSetBitsFromTheBase) {
  const std::vector<CdEvent> events{{100, 7, 1, 40}, {102, 7, 1, 40}, {107, 7, 1, 40}};
  const auto bytes = eventree::test::encode_evt3(events);
  EXPECT_EQ(bytes.size(), (2 + 1 + 1 + 1) * sizeof(std::uint16_t));  // time high, time low, y, base, vect 8
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt3Vectors, TwelveBitVectorDecodesSetBitsFromTheBase) {
  const std::vector<CdEvent> events{{200, 9, 0, 40}, {203, 9, 0, 40}, {208, 9, 0, 40}, {211, 9, 0, 40}};
  EXPECT_EQ(decode_all(events), events);
}

TEST(Evt3Vectors, BaseAdvancesPastEachVector) {
  // base 100, vect 12 (bit 0 and 11), vect 12 (bit 0), vect 8 (bit 7): x = 100, 111, 112, 131
  const auto events = decode_words({0x8000, 0x6000, 0x0005, 0x3000 | 100, 0x4801, 0x4001, 0x5080});
  EXPECT_EQ(events, (std::vector<CdEvent>{{100, 5, 0, 0}, {111, 5, 0, 0}, {112, 5, 0, 0}, {131, 5, 0, 0}}));
}

TEST(Evt3Vectors, PolarityComesFromTheBaseWord) {
  EXPECT_EQ(decode_words({0x8000, 0x6000, 0x0001, 0x3800 | 10, 0x5003}),
            (std::vector<CdEvent>{{10, 1, 1, 0}, {11, 1, 1, 0}}));
}

TEST(Evt3Time, TimeLowAndHighCombineIntoTheSharedSplit) {
  // t = 0x123 << 12 | 0xABC = 0x123ABC
  const auto events = decode_words({0x8123, 0x6ABC, 0x0000 | 3, 0x2000 | 4});
  const std::int64_t t = 0x123ABC;
  EXPECT_EQ(events, (std::vector<CdEvent>{{4, 3, 0, t}}));
}

TEST(Evt3Time, TimeHighChangeWithoutTimeLowResetsTimeLow) {
  const auto events = decode_words({0x8001, 0x6005, 0x0000, 0x2001, 0x8002, 0x2002});
  EXPECT_EQ(events, (std::vector<CdEvent>{{1, 0, 0, 4096 + 5}, {2, 0, 0, 8192}}));
}

TEST(Evt3Robustness, StreamSplitAtEveryByteOffsetDecodesLikeTheUnsplitStream) {
  const std::int64_t loop = 1LL << 24;
  const std::vector<CdEvent> events{
      {1, 2, 1, 5},        {3, 2, 1, 5},         {4, 2, 1, 5},          {20, 2, 1, 5},       {5, 6, 0, 5},
      {7, 8, 0, 64},       {7, 8, 0, 4100},      {9, 10, 1, loop - 1},  {11, 12, 0, loop + 3}, {12, 14, 1, 2 * loop - 1}, {13, 14, 1, 2 * loop + 1}};
  const auto bytes = eventree::test::encode_evt3(events);
  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    eventree::EventTree<> tree;
    eventree::Evt3Decoder decoder;
    decoder.decode(std::span{bytes}.first(cut), tree);
    decoder.decode(std::span{bytes}.subspan(cut), tree);
    EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events) << "cut at " << cut;
  }
}

TEST(Evt3Robustness, ByteByByteFeedingDecodesLikeTheUnsplitStream) {
  const std::vector<CdEvent> events{{1, 2, 1, 5}, {2, 2, 1, 5}, {3, 4, 0, 100}, {4, 4, 0, (1LL << 24) - 1}, {5, 6, 1, (1LL << 24) + 7}};
  const auto bytes = eventree::test::encode_evt3(events);
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  for (std::size_t i = 0; i < bytes.size(); ++i) decoder.decode(std::span{bytes}.subspan(i, 1), tree);
  EXPECT_EQ((std::vector<CdEvent>{tree.begin(), tree.end()}), events);
}

TEST(Evt3Robustness, NonCdWordsAreSkippedAndCounted) {
  eventree::Evt3Statistics stats;
  const auto events = decode_words(
      {
          0x8001,           // time high
          0x6003,           // time low
          0x0010,           // Y
          0x2000 | 5,       // CD
          0xA000 | 0x123,   // external trigger
          0xE000 | 0x014,   // other
          0xF000 | 0x001,   // continued 12
          0x7001,           // continued 4
          0xD000,           // IMU
          0x2800 | 6,       // CD on
      },
      &stats);
  EXPECT_EQ(events, (std::vector<CdEvent>{{5, 16, 0, 4096 + 3}, {6, 16, 1, 4096 + 3}}));
  EXPECT_EQ(stats.cd_events, 2u);
  EXPECT_EQ(stats.time_high_words, 1u);
  EXPECT_EQ(stats.skipped_words, 5u);
}

TEST(Evt3Robustness, EventsAfterAnEmYAreSkippedUntilTheNextCdY) {
  eventree::Evt3Statistics stats;
  const auto events = decode_words(
      {0x8000, 0x6001,
       0x1020,            // EM Y
       0x2000 | 1,        // EM X: skipped
       0x3000 | 8, 0x5001,  // EM vector: skipped
       0x0021,            // CD Y
       0x2000 | 2},
      &stats);
  EXPECT_EQ(events, (std::vector<CdEvent>{{2, 0x21, 0, 1}}));
  EXPECT_EQ(stats.skipped_words, 4u);  // EM Y, EM X, EM base, EM vector
}

TEST(Evt3Robustness, StatisticsAccumulateAcrossCallsAndResetClearsThem) {
  const auto bytes = words_to_bytes({0x8000, 0xA000, 0x0000, 0x2000});
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  decoder.decode(bytes, tree);
  decoder.decode(bytes, tree);
  EXPECT_EQ(decoder.statistics().skipped_words, 2u);
  decoder.reset();
  EXPECT_EQ(decoder.statistics().skipped_words, 0u);
  EXPECT_EQ(decoder.statistics().cd_events, 0u);
}

TEST(Evt3Robustness, EmptyAndSubWordInputChangeNothing) {
  eventree::EventTree<> tree;
  eventree::Evt3Decoder decoder;
  decoder.decode({}, tree);
  const std::byte partial[1]{};
  decoder.decode(partial, tree);  // held as a pending byte, nothing decoded
  EXPECT_EQ(tree.size(), 0u);
  EXPECT_EQ(decoder.statistics().skipped_words, 0u);
}

#ifndef NDEBUG
TEST(Evt3RobustnessDeathTest, TimeGoingBackwardsTripsAnAssertion) {
  const auto bytes = words_to_bytes({0x8000, 0x6009, 0x0000, 0x2001, 0x6002, 0x2001});
  EXPECT_DEATH(
      {
        eventree::EventTree<> tree;
        eventree::Evt3Decoder decoder;
        decoder.decode(bytes, tree);
      },
      "");
}
#endif

TEST(Evt3Decode, CdEventsBeforeTheFirstTimeHighAreSkippedLikeTheReferenceDecoder) {
  eventree::Evt3Statistics stats;
  // Y=5, X=7 and a vector with no time known, then time high 1, time low 3, Y=2, X=9 (polarity 1).
  const auto events = decode_words({0x0005, 0x2007, 0x3000, 0x4001, 0x8001, 0x6003, 0x0002, 0x2809}, &stats);
  EXPECT_EQ(events, (std::vector<CdEvent>{{9, 2, 1, 4099}}));
  EXPECT_EQ(stats.cd_events, 1u);
}

}  // namespace
