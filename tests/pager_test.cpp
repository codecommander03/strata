#include "strata/pager.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace strata;
using strata::test::TempDb;

namespace {

TEST(Pager, ANewDatabaseHasAMetaPageAndAnEmptyLeafRoot) {
    TempDb db("new");
    Pager pager;
    ASSERT_TRUE(pager.open(db.path())) << "open failed";

    EXPECT_EQ(pager.page_count(), 2u);
    EXPECT_EQ(pager.root(), 1u);

    Page root(nullptr, 0);
    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    EXPECT_EQ(root.type(), PageType::Leaf);
    EXPECT_EQ(root.cell_count(), 0);
}

TEST(Pager, RejectsFetchingPastTheEndOfTheDatabase) {
    TempDb db("bounds");
    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));

    Page p(nullptr, 0);
    const Status s = pager.fetch(9999, &p);
    EXPECT_FALSE(s);
    EXPECT_EQ(s.code(), Code::InvalidArgument);
}

TEST(Pager, AllocateGrowsTheDatabaseAndReturnsAnInitialisedPage) {
    TempDb db("allocate");
    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    const std::uint32_t before = pager.page_count();

    Page fresh(nullptr, 0);
    ASSERT_TRUE(pager.allocate(PageType::Leaf, &fresh));

    EXPECT_EQ(pager.page_count(), before + 1);
    EXPECT_EQ(fresh.id(), before);
    EXPECT_EQ(fresh.type(), PageType::Leaf);
    EXPECT_EQ(fresh.cell_count(), 0);
}

TEST(Pager, CommittedWritesSurviveReopeningWithoutACheckpoint) {
    TempDb db("reopen_wal");
    {
        Pager pager;
        ASSERT_TRUE(pager.open(db.path()));
        Page root(nullptr, 0);
        ASSERT_TRUE(pager.fetch(pager.root(), &root));
        ASSERT_TRUE(root.insert_leaf_cell(0, as_bytes("persisted"), as_bytes("yes")));
        pager.mark_dirty(root.id());
        ASSERT_TRUE(pager.commit());
        // Deliberately no checkpoint: the data exists only in the log.
    }

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    Page root(nullptr, 0);
    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_EQ(root.cell_count(), 1);
    EXPECT_EQ(as_string_view(root.leaf_cell(0).value), "yes");
}

TEST(Pager, CommittedWritesSurviveACheckpoint) {
    TempDb db("reopen_checkpoint");
    {
        Pager pager;
        ASSERT_TRUE(pager.open(db.path()));
        Page root(nullptr, 0);
        ASSERT_TRUE(pager.fetch(pager.root(), &root));
        ASSERT_TRUE(root.insert_leaf_cell(0, as_bytes("durable"), as_bytes("yes")));
        pager.mark_dirty(root.id());
        ASSERT_TRUE(pager.commit());
        ASSERT_TRUE(pager.checkpoint());
    }

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    Page root(nullptr, 0);
    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_EQ(root.cell_count(), 1);
    EXPECT_EQ(as_string_view(root.leaf_cell(0).key), "durable");
}

TEST(Pager, RollbackDiscardsUncommittedChanges) {
    TempDb db("rollback");
    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));

    Page root(nullptr, 0);
    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_TRUE(root.insert_leaf_cell(0, as_bytes("committed"), as_bytes("1")));
    pager.mark_dirty(root.id());
    ASSERT_TRUE(pager.commit());

    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_TRUE(root.insert_leaf_cell(1, as_bytes("discarded"), as_bytes("2")));
    pager.mark_dirty(root.id());
    pager.rollback();

    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_EQ(root.cell_count(), 1);
    EXPECT_EQ(as_string_view(root.leaf_cell(0).key), "committed");
}

TEST(Pager, CheckpointEmptiesTheLogButKeepsTheData) {
    TempDb db("checkpoint_resets");
    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));

    Page root(nullptr, 0);
    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    ASSERT_TRUE(root.insert_leaf_cell(0, as_bytes("k"), as_bytes("v")));
    pager.mark_dirty(root.id());
    ASSERT_TRUE(pager.commit());
    EXPECT_GT(pager.wal().committed_page_count(), 0u);

    ASSERT_TRUE(pager.checkpoint());
    EXPECT_EQ(pager.wal().committed_page_count(), 0u);

    ASSERT_TRUE(pager.fetch(pager.root(), &root));
    EXPECT_EQ(root.cell_count(), 1);
}

} // namespace
