#include "strata/wal.hpp"

#include "strata/page.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace strata;
using strata::test::TempDb;

namespace {

std::array<std::byte, kPageSize> page_filled_with(std::byte value) {
    std::array<std::byte, kPageSize> page{};
    page.fill(value);
    return page;
}

TEST(Wal, AFreshLogHasNothingToReplay) {
    TempDb db("wal_fresh");
    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));
    EXPECT_EQ(wal.committed_page_count(), 0u);
    EXPECT_EQ(wal.db_page_count(), 0u);
}

TEST(Wal, StagedFramesAreInvisibleUntilCommit) {
    TempDb db("wal_staged");
    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));

    const auto page = page_filled_with(std::byte{0xAB});
    wal.stage(5, page.data());

    EXPECT_EQ(wal.staged_frame_count(), 1u);
    EXPECT_FALSE(wal.contains(5));

    ASSERT_TRUE(wal.commit(10));
    EXPECT_TRUE(wal.contains(5));
    EXPECT_EQ(wal.db_page_count(), 10u);
}

TEST(Wal, RollbackDropsStagedFramesWithoutWritingThem) {
    TempDb db("wal_rollback");
    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));

    const auto page = page_filled_with(std::byte{0x01});
    wal.stage(3, page.data());
    wal.rollback();

    ASSERT_TRUE(wal.commit(4));
    EXPECT_FALSE(wal.contains(3));
}

TEST(Wal, LastWriteOfAPageWithinATransactionWins) {
    TempDb db("wal_dedup");
    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));

    wal.stage(2, page_filled_with(std::byte{0x11}).data());
    wal.stage(2, page_filled_with(std::byte{0x22}).data());
    EXPECT_EQ(wal.staged_frame_count(), 1u);

    ASSERT_TRUE(wal.commit(3));

    std::vector<std::byte> out(kPageSize);
    ASSERT_TRUE(wal.read_page(2, out.data()));
    EXPECT_EQ(out[0], std::byte{0x22});
}

TEST(Wal, ReplayRecoversEveryCommittedFrame) {
    TempDb db("wal_replay");
    {
        Wal wal;
        ASSERT_TRUE(wal.open(db.wal_path()));
        wal.stage(1, page_filled_with(std::byte{0xA1}).data());
        wal.stage(2, page_filled_with(std::byte{0xA2}).data());
        ASSERT_TRUE(wal.commit(3));
    }

    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));
    EXPECT_EQ(wal.committed_page_count(), 2u);
    EXPECT_EQ(wal.db_page_count(), 3u);

    std::vector<std::byte> out(kPageSize);
    ASSERT_TRUE(wal.read_page(2, out.data()));
    EXPECT_EQ(out[0], std::byte{0xA2});
}

TEST(Wal, ATornFrameAndEverythingAfterItIsDiscarded) {
    TempDb db("wal_torn");
    {
        Wal wal;
        ASSERT_TRUE(wal.open(db.wal_path()));
        wal.stage(1, page_filled_with(std::byte{0x10}).data());
        ASSERT_TRUE(wal.commit(2)); // transaction one: durable
        wal.stage(1, page_filled_with(std::byte{0x20}).data());
        ASSERT_TRUE(wal.commit(2)); // transaction two: durable
    }

    // Simulate a torn write: flip a byte inside the second transaction's frame.
    {
        File f;
        ASSERT_TRUE(f.open(db.wal_path()));
        const std::uint64_t second_frame = Wal::kHeaderSize + Wal::kFrameSize;
        std::byte b{};
        ASSERT_TRUE(f.read_at(second_frame + Wal::kFrameHeaderSize + 4, &b, 1));
        b ^= std::byte{0xFF};
        ASSERT_TRUE(f.write_at(second_frame + Wal::kFrameHeaderSize + 4, &b, 1));
        ASSERT_TRUE(f.sync());
    }

    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));
    // The first transaction survives; the damaged one is gone entirely.
    EXPECT_EQ(wal.committed_page_count(), 1u);
    std::vector<std::byte> out(kPageSize);
    ASSERT_TRUE(wal.read_page(1, out.data()));
    EXPECT_EQ(out[0], std::byte{0x10});
}

TEST(Wal, FramesWrittenWithoutACommitMarkerAreDiscarded) {
    TempDb db("wal_uncommitted_tail");
    {
        Wal wal;
        ASSERT_TRUE(wal.open(db.wal_path()));
        wal.stage(1, page_filled_with(std::byte{0x55}).data());
        ASSERT_TRUE(wal.commit(2));
    }

    // Append a well-formed-looking frame by hand with no commit marker after
    // it. Recovery must stop at the last commit, not at the last valid frame.
    {
        File f;
        ASSERT_TRUE(f.open(db.wal_path()));
        const std::uint64_t end = f.size();
        std::vector<std::byte> junk(Wal::kFrameSize, std::byte{0x00});
        ASSERT_TRUE(f.write_at(end, junk.data(), junk.size()));
        ASSERT_TRUE(f.sync());
    }

    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));
    EXPECT_EQ(wal.committed_page_count(), 1u);
    EXPECT_EQ(wal.db_page_count(), 2u);
}

TEST(Wal, CheckpointMovesFramesIntoTheDatabaseFileAndResetsTheLog) {
    TempDb db("wal_checkpoint");

    File data_file;
    ASSERT_TRUE(data_file.open(db.path()));
    // The database file must be large enough to hold page 1.
    std::vector<std::byte> blank(kPageSize * 2, std::byte{0});
    ASSERT_TRUE(data_file.write_at(0, blank.data(), blank.size()));

    Wal wal;
    ASSERT_TRUE(wal.open(db.wal_path()));
    wal.stage(1, page_filled_with(std::byte{0x77}).data());
    ASSERT_TRUE(wal.commit(2));
    ASSERT_TRUE(wal.checkpoint(data_file));

    EXPECT_EQ(wal.committed_page_count(), 0u);

    std::vector<std::byte> out(kPageSize);
    ASSERT_TRUE(data_file.read_at(kPageSize, out.data(), out.size()));
    EXPECT_EQ(out[0], std::byte{0x77});
}

} // namespace
