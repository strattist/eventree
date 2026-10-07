#include <gtest/gtest.h>

#include <vector>

#include <eventree/evt2_decoder.hpp>

#include "evt2_encoder.hpp"

namespace {

using eventree::CdEvent;

const std::vector<CdEvent> kEvents{{1, 2, 1, 100}, {3, 4, 0, 100}, {5, 6, 1, 100}, {7, 8, 0, 101},
                                   {9, 10, 1, 5000}, {11, 12, 0, 5000}};

eventree::EventTree<> decode(const std::vector<CdEvent>& events) {
  eventree::EventTree<> tree;
  eventree::Evt2Decoder decoder;
  decoder.decode(eventree::test::encode_evt2(events), tree);
  return tree;
}

TEST(ReadingApi, TimeNodesExposeTimestampAndEvents) {
  const auto tree = decode(kEvents);
  std::vector<std::int64_t> times;
  std::vector<CdEvent> flattened;
  for (const auto node : tree.time_nodes()) {
    times.push_back(node.time());
    for (const CdEvent& e : node) {
      EXPECT_EQ(e.t, node.time());
      flattened.push_back(e);
    }
  }
  EXPECT_EQ(times, (std::vector<std::int64_t>{100, 101, 5000}));
  EXPECT_EQ(flattened, kEvents);
}

TEST(ReadingApi, CountsPerNodeAndPerTree) {
  const auto tree = decode(kEvents);
  EXPECT_EQ(tree.size(), 6u);
  std::vector<std::size_t> counts;
  for (const auto node : tree.time_nodes()) counts.push_back(node.size());
  EXPECT_EQ(counts, (std::vector<std::size_t>{3, 1, 2}));
}

TEST(ReadingApi, GettersInspectWithoutIterating) {
  const auto tree = decode(kEvents);
  EXPECT_EQ(tree.time_node_count(), 3u);
  EXPECT_EQ(tree.time_nodes().size(), 3u);
  EXPECT_EQ(tree.time_high_count(), 2u);  // 100,101 -> high 1; 5000 -> high 78
  const auto node = tree.time_node(2);
  EXPECT_EQ(node.time(), 5000);
  EXPECT_EQ(node.time_high(), 5000u >> 6);
  EXPECT_EQ(node.time_low(), 5000u & 63);
  EXPECT_EQ(node.size(), 2u);
  EXPECT_EQ(tree.time_node(0).size(), 3u);
  EXPECT_EQ(tree.time_node(1).time(), 101);
}

TEST(ReadingApi, ConvertsToEventVector) { EXPECT_EQ(decode(kEvents).to_events(), kEvents); }

TEST(ReadingApi, EmptyTreeHasNoNodes) {
  const eventree::EventTree<> tree;
  EXPECT_EQ(tree.time_node_count(), 0u);
  EXPECT_EQ(tree.time_high_count(), 0u);
  EXPECT_TRUE(tree.time_nodes().begin() == tree.time_nodes().end());
  EXPECT_TRUE(tree.to_events().empty());
}

}  // namespace
