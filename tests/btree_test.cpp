#include "strata/btree.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace strata;
using strata::test::TempDb;

namespace {

/// Zero-padded so lexicographic byte order matches numeric order — otherwise
/// "10" sorts before "9" and every ordering assertion below becomes a lie.
std::string key_for(int i) {
    std::string s = std::to_string(i);
    return std::string(10 - s.size(), '0') + s;
}

class BTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<TempDb>("btree");
        pager_ = std::make_unique<Pager>();
        ASSERT_TRUE(pager_->open(db_->path()));
        tree_ = std::make_unique<BTree>(*pager_);
    }

    std::unique_ptr<TempDb> db_;
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<BTree> tree_;
};

TEST_F(BTreeTest, GetOnAnEmptyTreeReportsNotFound) {
    std::string value;
    const Status s = tree_->get(as_bytes("missing"), &value);
    EXPECT_FALSE(s);
    EXPECT_EQ(s.code(), Code::NotFound);
}

TEST_F(BTreeTest, RoundTripsASingleKey) {
    ASSERT_TRUE(tree_->insert(as_bytes("hello"), as_bytes("world")));
    std::string value;
    ASSERT_TRUE(tree_->get(as_bytes("hello"), &value));
    EXPECT_EQ(value, "world");
}

TEST_F(BTreeTest, InsertingAnExistingKeyReplacesItsValue) {
    ASSERT_TRUE(tree_->insert(as_bytes("k"), as_bytes("first")));
    ASSERT_TRUE(tree_->insert(as_bytes("k"), as_bytes("second")));

    std::string value;
    ASSERT_TRUE(tree_->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "second");

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, 1u);
}

TEST_F(BTreeTest, RejectsAPayloadTooLargeForAPage) {
    const std::string huge(Page::max_payload() + 1, 'x');
    const Status s = tree_->insert(as_bytes("k"), as_bytes(huge));
    EXPECT_FALSE(s);
    EXPECT_EQ(s.code(), Code::InvalidArgument);
}

TEST_F(BTreeTest, RemoveDeletesOnlyTheNamedKey) {
    ASSERT_TRUE(tree_->insert(as_bytes("a"), as_bytes("1")));
    ASSERT_TRUE(tree_->insert(as_bytes("b"), as_bytes("2")));

    ASSERT_TRUE(tree_->remove(as_bytes("a")));

    std::string value;
    EXPECT_FALSE(tree_->get(as_bytes("a"), &value));
    ASSERT_TRUE(tree_->get(as_bytes("b"), &value));
    EXPECT_EQ(value, "2");
    EXPECT_EQ(tree_->remove(as_bytes("a")).code(), Code::NotFound);
}

TEST_F(BTreeTest, GrowsTallerWhenALeafOverflows) {
    std::size_t before = 0;
    ASSERT_TRUE(tree_->depth(&before));
    EXPECT_EQ(before, 1u); // a single leaf

    const std::string filler(200, 'v');
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes(filler))) << "insert " << i;
    }

    std::size_t after = 0;
    ASSERT_TRUE(tree_->depth(&after));
    EXPECT_GT(after, before) << "the tree never split";
    ASSERT_TRUE(tree_->verify_integrity());
}

TEST_F(BTreeTest, HoldsEveryKeyAcrossManySplits) {
    constexpr int kCount = 5000;
    const std::string filler(64, 'v');

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes(filler))) << "insert " << i;
    }

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys)) << "integrity check failed";
    EXPECT_EQ(keys, static_cast<std::size_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        std::string value;
        ASSERT_TRUE(tree_->get(as_bytes(key_for(i)), &value)) << "missing key " << i;
        EXPECT_EQ(value, filler);
    }
}

TEST_F(BTreeTest, HandlesKeysArrivingInRandomOrder) {
    constexpr int kCount = 3000;
    std::vector<int> order(kCount);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(12345));

    for (const int i : order) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes("v" + std::to_string(i))));
    }

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, static_cast<std::size_t>(kCount));

    for (const int i : order) {
        std::string value;
        ASSERT_TRUE(tree_->get(as_bytes(key_for(i)), &value));
        EXPECT_EQ(value, "v" + std::to_string(i));
    }
}

TEST_F(BTreeTest, HandlesKeysArrivingInReverseOrder) {
    // Descending insertion is the pathological case for a naive split point:
    // every new key lands at the front of the leftmost leaf.
    constexpr int kCount = 2000;
    for (int i = kCount - 1; i >= 0; --i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes("v")));
    }
    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, static_cast<std::size_t>(kCount));
}

TEST_F(BTreeTest, CursorWalksEveryKeyInOrder) {
    constexpr int kCount = 1500;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes("v")));
    }

    auto cursor = tree_->cursor();
    ASSERT_TRUE(cursor.seek_first());

    int seen = 0;
    std::string previous;
    while (cursor.valid()) {
        const std::string key(as_string_view(cursor.key()));
        if (!previous.empty()) {
            EXPECT_LT(previous, key) << "cursor went backwards";
        }
        previous = key;
        ++seen;
        ASSERT_TRUE(cursor.next());
    }
    EXPECT_EQ(seen, kCount);
}

TEST_F(BTreeTest, CursorSeekLandsOnTheFirstKeyAtOrAfterTheProbe) {
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i * 2)), as_bytes("v")));
    }

    auto cursor = tree_->cursor();
    ASSERT_TRUE(cursor.seek(as_bytes(key_for(101)))); // absent; 102 is the next
    ASSERT_TRUE(cursor.valid());
    EXPECT_EQ(std::string(as_string_view(cursor.key())), key_for(102));
}

TEST_F(BTreeTest, SurvivesInterleavedInsertsAndDeletes) {
    constexpr int kCount = 2000;
    std::set<int> live;

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes("v")));
        live.insert(i);
        if (i % 3 == 0 && i > 0) {
            const int victim = i / 2;
            if (live.erase(victim) > 0) {
                ASSERT_TRUE(tree_->remove(as_bytes(key_for(victim)))) << "remove " << victim;
            }
        }
    }

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, live.size());

    for (const int i : live) {
        std::string value;
        EXPECT_TRUE(tree_->get(as_bytes(key_for(i)), &value)) << "lost key " << i;
    }
}

TEST_F(BTreeTest, ReusesSpaceFreedByDeletesInsteadOfGrowingTheTree) {
    // Regression: before page compaction existed, a page that had been split or
    // had cells deleted reported free space it could not use, so every
    // subsequent insert into it split again and the tree grew without bound.
    const std::string filler(300, 'v');
    for (int i = 0; i < 400; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes(filler)));
    }
    std::size_t depth_before = 0;
    ASSERT_TRUE(tree_->depth(&depth_before));

    // Churn the same key range many times over. With space being reclaimed the
    // tree stays the same height; without it, it climbs.
    for (int round = 0; round < 15; ++round) {
        for (int i = 0; i < 400; ++i) {
            ASSERT_TRUE(tree_->remove(as_bytes(key_for(i))));
        }
        for (int i = 0; i < 400; ++i) {
            ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes(filler)));
        }
    }

    std::size_t depth_after = 0;
    ASSERT_TRUE(tree_->depth(&depth_after));
    EXPECT_LE(depth_after, depth_before + 1) << "the tree grew from churn alone";

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, 400u);
}

TEST_F(BTreeTest, DataSurvivesCommitAndReopen) {
    constexpr int kCount = 1200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree_->insert(as_bytes(key_for(i)), as_bytes("v" + std::to_string(i))));
    }
    ASSERT_TRUE(pager_->commit());

    tree_.reset();
    pager_->close();
    pager_ = std::make_unique<Pager>();
    ASSERT_TRUE(pager_->open(db_->path()));
    tree_ = std::make_unique<BTree>(*pager_);

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, static_cast<std::size_t>(kCount));

    std::string value;
    ASSERT_TRUE(tree_->get(as_bytes(key_for(999)), &value));
    EXPECT_EQ(value, "v999");
}

TEST_F(BTreeTest, HandlesVariableLengthKeysAndValues) {
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> key_len(1, 60);
    std::uniform_int_distribution<int> val_len(0, 400);

    std::set<std::string> inserted;
    for (int i = 0; i < 1200; ++i) {
        std::string key = key_for(i).substr(0, static_cast<std::size_t>(key_len(rng)));
        key += "#" + std::to_string(i); // keep them distinct after truncation
        const std::string value(static_cast<std::size_t>(val_len(rng)), 'x');
        ASSERT_TRUE(tree_->insert(as_bytes(key), as_bytes(value)));
        inserted.insert(key);
    }

    std::size_t keys = 0;
    ASSERT_TRUE(tree_->verify_integrity(&keys));
    EXPECT_EQ(keys, inserted.size());
}

} // namespace
