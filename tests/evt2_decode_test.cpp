#include <gtest/gtest.h>

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

}  // namespace
