#include "strata/page.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace strata;

namespace {

class PageTest : public ::testing::Test {
protected:
    std::array<std::byte, kPageSize> buffer_{};
    Page page_{buffer_.data(), 7};
};

TEST_F(PageTest, InitLeavesAnEmptyPageWithAllSpaceFree) {
    page_.init(PageType::Leaf);
    EXPECT_EQ(page_.type(), PageType::Leaf);
    EXPECT_EQ(page_.cell_count(), 0);
    EXPECT_EQ(page_.free_start(), kPageHeaderSize);
    EXPECT_EQ(page_.free_end(), kPageSize);
    EXPECT_EQ(page_.free_space(), kPageSize - kPageHeaderSize);
    EXPECT_EQ(page_.id(), 7u);
}

TEST_F(PageTest, RoundTripsALeafCell) {
    page_.init(PageType::Leaf);
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("alpha"), as_bytes("one")));

    ASSERT_EQ(page_.cell_count(), 1);
    const LeafCell cell = page_.leaf_cell(0);
    EXPECT_EQ(as_string_view(cell.key), "alpha");
    EXPECT_EQ(as_string_view(cell.value), "one");
}

TEST_F(PageTest, RoundTripsAnInternalCell) {
    page_.init(PageType::Internal);
    ASSERT_TRUE(page_.insert_internal_cell(0, as_bytes("mid"), 42));

    const InternalCell cell = page_.internal_cell(0);
    EXPECT_EQ(as_string_view(cell.key), "mid");
    EXPECT_EQ(cell.child, 42u);
}

TEST_F(PageTest, KeepsCellsInSlotOrderRegardlessOfInsertOrder) {
    page_.init(PageType::Leaf);
    // Insert at the front each time: slot order must follow the slot index,
    // not the order the bytes landed in the content area.
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("c"), as_bytes("3")));
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("b"), as_bytes("2")));
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("a"), as_bytes("1")));

    ASSERT_EQ(page_.cell_count(), 3);
    EXPECT_EQ(as_string_view(page_.leaf_cell(0).key), "a");
    EXPECT_EQ(as_string_view(page_.leaf_cell(1).key), "b");
    EXPECT_EQ(as_string_view(page_.leaf_cell(2).key), "c");
}

TEST_F(PageTest, LowerBoundFindsInsertionPointAndReportsExactMatches) {
    page_.init(PageType::Leaf);
    for (const char* k : {"a", "c", "e"}) {
        const auto [idx, exact] = page_.lower_bound(as_bytes(k));
        (void)exact;
        ASSERT_TRUE(page_.insert_leaf_cell(idx, as_bytes(k), as_bytes("v")));
    }

    EXPECT_EQ(page_.lower_bound(as_bytes("a")), std::make_pair(std::uint16_t{0}, true));
    EXPECT_EQ(page_.lower_bound(as_bytes("b")), std::make_pair(std::uint16_t{1}, false));
    EXPECT_EQ(page_.lower_bound(as_bytes("e")), std::make_pair(std::uint16_t{2}, true));
    EXPECT_EQ(page_.lower_bound(as_bytes("z")), std::make_pair(std::uint16_t{3}, false));
}

TEST_F(PageTest, RemoveClosesTheGapInTheSlotArray) {
    page_.init(PageType::Leaf);
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("a"), as_bytes("1")));
    ASSERT_TRUE(page_.insert_leaf_cell(1, as_bytes("b"), as_bytes("2")));
    ASSERT_TRUE(page_.insert_leaf_cell(2, as_bytes("c"), as_bytes("3")));

    page_.remove_cell(1);

    ASSERT_EQ(page_.cell_count(), 2);
    EXPECT_EQ(as_string_view(page_.leaf_cell(0).key), "a");
    EXPECT_EQ(as_string_view(page_.leaf_cell(1).key), "c");
}

TEST_F(PageTest, ReportsFullRatherThanOverflowing) {
    page_.init(PageType::Leaf);
    const std::string big(1000, 'x');

    int inserted = 0;
    for (int i = 0; i < 100; ++i) {
        const std::string key = "k" + std::to_string(i);
        if (!page_.insert_leaf_cell(static_cast<std::uint16_t>(page_.cell_count()), as_bytes(key),
                                    as_bytes(big))) {
            break;
        }
        ++inserted;
    }

    EXPECT_GT(inserted, 0);
    EXPECT_LT(inserted, 100);
    // The page is intact: nothing wrote past its own boundaries.
    EXPECT_LE(page_.free_start(), page_.free_end());
    EXPECT_LE(page_.free_end(), kPageSize);
}

TEST_F(PageTest, RemoveAloneDoesNotGiveTheSpaceBack) {
    // Documents the behaviour that compact() exists to correct. Removing a cell
    // returns its two-byte slot and nothing else, because reclaiming the
    // content would mean moving every cell after it.
    page_.init(PageType::Leaf);
    const std::string filler(500, 'x');
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(page_.insert_leaf_cell(static_cast<std::uint16_t>(i),
                                           as_bytes("k" + std::to_string(i)), as_bytes(filler)));
    }
    const std::uint16_t before = page_.free_space();

    page_.remove_cell(0);
    page_.remove_cell(0);

    EXPECT_EQ(page_.free_space(), before + 4) << "only the slot pointers came back";
}

TEST_F(PageTest, CompactReclaimsSpaceAndKeepsCellsIntact) {
    // The regression test for the split bug: a page that had cells removed
    // reports free space it cannot actually use until it is compacted.
    page_.init(PageType::Leaf);
    const std::string filler(500, 'x');
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(page_.insert_leaf_cell(static_cast<std::uint16_t>(i),
                                           as_bytes("k" + std::to_string(i)), as_bytes(filler)));
    }
    for (int i = 5; i >= 3; --i) {
        page_.remove_cell(static_cast<std::uint16_t>(i));
    }
    const std::uint16_t fragmented = page_.free_space();

    page_.compact();

    EXPECT_GT(page_.free_space(), fragmented) << "compaction reclaimed nothing";
    ASSERT_EQ(page_.cell_count(), 3);
    for (int i = 0; i < 3; ++i) {
        const LeafCell cell = page_.leaf_cell(static_cast<std::uint16_t>(i));
        EXPECT_EQ(as_string_view(cell.key), "k" + std::to_string(i));
        EXPECT_EQ(as_string_view(cell.value), filler);
    }
    // And the reclaimed space is genuinely usable.
    EXPECT_TRUE(page_.insert_leaf_cell(3, as_bytes("k9"), as_bytes(filler)));
}

TEST_F(PageTest, CompactPreservesTheHeaderFields) {
    page_.init(PageType::Internal);
    page_.set_extra(1234);
    page_.set_lsn(99);
    ASSERT_TRUE(page_.insert_internal_cell(0, as_bytes("sep"), 77));

    page_.compact();

    EXPECT_EQ(page_.type(), PageType::Internal);
    EXPECT_EQ(page_.extra(), 1234u);
    EXPECT_EQ(page_.lsn(), 99u);
    ASSERT_EQ(page_.cell_count(), 1);
    EXPECT_EQ(page_.internal_cell(0).child, 77u);
}

TEST_F(PageTest, ChecksumCatchesASingleFlippedByte) {
    page_.init(PageType::Leaf);
    ASSERT_TRUE(page_.insert_leaf_cell(0, as_bytes("key"), as_bytes("value")));
    page_.seal();
    ASSERT_TRUE(page_.verify());

    buffer_[2000] ^= std::byte{0x01};
    EXPECT_FALSE(page_.verify());
}

TEST(CompareKeys, OrdersLexicographicallyThenByLength) {
    EXPECT_LT(compare_keys(as_bytes("a"), as_bytes("b")), 0);
    EXPECT_GT(compare_keys(as_bytes("b"), as_bytes("a")), 0);
    EXPECT_EQ(compare_keys(as_bytes("ab"), as_bytes("ab")), 0);
    EXPECT_LT(compare_keys(as_bytes("ab"), as_bytes("abc")), 0);
    EXPECT_GT(compare_keys(as_bytes("abc"), as_bytes("ab")), 0);
}

TEST(CompareKeys, TreatsKeysAsBytesNotStrings) {
    // A key containing a NUL is not truncated at it.
    const std::string a("a\0b", 3);
    const std::string b("a\0c", 3);
    EXPECT_LT(compare_keys(as_bytes(a), as_bytes(b)), 0);
}

} // namespace
