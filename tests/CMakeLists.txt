include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(eventree_tests skeleton_test.cpp evt2_decode_test.cpp event_pool_test.cpp)
target_link_libraries(eventree_tests PRIVATE eventree::eventree GTest::gtest_main)
target_compile_options(eventree_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(GoogleTest)
gtest_discover_tests(eventree_tests)
