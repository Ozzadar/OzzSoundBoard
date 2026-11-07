#include <array>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "audio/SpscRing.hpp"

using ozzsb::SpscRing;

TEST(SpscRing, RoundsCapacityUpToPowerOfTwo) {
    EXPECT_EQ(SpscRing<int>(5).capacity(), 8u);
    EXPECT_EQ(SpscRing<int>(8).capacity(), 8u);
    EXPECT_EQ(SpscRing<int>(0).capacity(), 2u);
}

TEST(SpscRing, WritesAndReadsInOrderAcrossWrap) {
    SpscRing<int> ring(4);
    std::array<int, 3> out{};
    for (int round = 0; round < 5; ++round) {
        const std::array<int, 3> in{round * 10, round * 10 + 1, round * 10 + 2};
        ASSERT_EQ(ring.write(in), 3u);
        ASSERT_EQ(ring.read(out), 3u);
        EXPECT_EQ(out, in);
    }
    EXPECT_EQ(ring.size(), 0u);
}

TEST(SpscRing, WriteStopsWhenFull) {
    SpscRing<int> ring(4);
    const std::array<int, 6> in{1, 2, 3, 4, 5, 6};
    EXPECT_EQ(ring.write(in), 4u);
    EXPECT_FALSE(ring.push(7));
    int value = 0;
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(ring.push(7));
}

TEST(SpscRing, ReadReturnsOnlyWhatIsQueued) {
    SpscRing<int> ring(8);
    ring.push(1);
    ring.push(2);
    std::array<int, 4> out{};
    EXPECT_EQ(ring.read(out), 2u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
}

TEST(SpscRing, DiscardDropsOldest) {
    SpscRing<int> ring(8);
    const std::array<int, 5> in{1, 2, 3, 4, 5};
    ring.write(in);
    EXPECT_EQ(ring.discard(3), 3u);
    EXPECT_EQ(ring.discard(10), 2u);
    ring.push(9);
    int value = 0;
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 9);
}

TEST(SpscRing, ProducerConsumerThreadsSeeEveryItemOnce) {
    SpscRing<int> ring(64);
    constexpr int kCount = 200000;
    std::thread producer([&] {
        for (int i = 0; i < kCount;) {
            if (ring.push(i)) {
                ++i;
            }
        }
    });
    std::vector<int> received;
    received.reserve(kCount);
    while (static_cast<int>(received.size()) < kCount) {
        int value = 0;
        if (ring.pop(value)) {
            received.push_back(value);
        }
    }
    producer.join();
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[static_cast<std::size_t>(i)], i);
    }
}
