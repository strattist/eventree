#include <gtest/gtest.h>

#include <eventree/version.hpp>

TEST(Skeleton, LibraryHeadersAreReachable) {
  EXPECT_EQ(eventree::version_major, 0);
}
