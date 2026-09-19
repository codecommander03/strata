// The Hermitage suite, transliterated from SQL to this engine's key/value API.
//
// Hermitage (Martin Kleppmann) defines, for each isolation level, the concrete
// anomalies it must prevent and the ones it is permitted to allow. It is the
// external judge for stage 2: these tests were not designed around this
// implementation, and both directions are asserted.
//
// Snapshot isolation must PREVENT: G0, G1a, G1b, G1c, OTV, PMP, P4, G-single.
// Snapshot isolation is PERMITTED to allow: G2-item, G2.
//
// A database that prevented everything would not be a database with strong
// isolation; it would be a database with one lock. So the last two tests assert
// that the anomaly does occur, and they would fail if it stopped occurring —
// at which point the claim in the README would have to change from snapshot
// isolation to something stronger.

#include "strata/db.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace strata;
using strata::test::TempDb;

namespace {

class Hermitage : public ::testing::Test {
protected:
    void SetUp() override {
        temp_ = std::make_unique<TempDb>("hermitage");
        db_ = std::make_unique<Database>();
        ASSERT_TRUE(db_->open(temp_->path()));
        // Hermitage's fixture: two rows, both 10.
        ASSERT_TRUE(db_->put(as_bytes("1"), as_bytes("10")));
        ASSERT_TRUE(db_->put(as_bytes("2"), as_bytes("20")));
    }

    /// Reads a key inside a transaction and returns it as text, or "<none>".
    static std::string read(Transaction& txn, const char* key) {
        std::string value;
        const Status s = txn.get(as_bytes(key), &value);
        return s ? value : std::string("<none>");
    }

    std::unique_ptr<TempDb> temp_;
    std::unique_ptr<Database> db_;
};

// --- must be prevented ------------------------------------------------------

TEST_F(Hermitage, G0_DirtyWriteIsPrevented) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11")));
    ASSERT_TRUE(t2->put(as_bytes("1"), as_bytes("12")));

    ASSERT_TRUE(t1->put(as_bytes("2"), as_bytes("19")));
    ASSERT_TRUE(t1->commit());

    ASSERT_TRUE(t2->put(as_bytes("2"), as_bytes("21")));
    const Status s = t2->commit();
    EXPECT_FALSE(s) << "both writers committed, so one write was silently lost";
    EXPECT_EQ(s.code(), Code::Conflict);

    // The winner's writes stand, whole and unmixed.
    std::string v;
    ASSERT_TRUE(db_->get(as_bytes("1"), &v));
    EXPECT_EQ(v, "11");
    ASSERT_TRUE(db_->get(as_bytes("2"), &v));
    EXPECT_EQ(v, "19");
}

TEST_F(Hermitage, G1a_AbortedReadIsPrevented) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("101")));

    EXPECT_EQ(read(*t2, "1"), "10") << "read a value from an uncommitted transaction";

    t1->abort();

    EXPECT_EQ(read(*t2, "1"), "10");
    ASSERT_TRUE(t2->commit());
}

TEST_F(Hermitage, G1b_IntermediateReadIsPrevented) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("101"))); // intermediate state
    EXPECT_EQ(read(*t2, "1"), "10");

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11"))); // final state
    ASSERT_TRUE(t1->commit());

    // t2's snapshot predates t1's commit, so it sees neither value.
    EXPECT_EQ(read(*t2, "1"), "10") << "observed an intermediate value";
    ASSERT_TRUE(t2->commit());
}

TEST_F(Hermitage, G1c_CircularInformationFlowIsPrevented) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11")));
    ASSERT_TRUE(t2->put(as_bytes("2"), as_bytes("22")));

    EXPECT_EQ(read(*t1, "2"), "20") << "t1 saw t2's uncommitted write";
    EXPECT_EQ(read(*t2, "1"), "10") << "t2 saw t1's uncommitted write";

    ASSERT_TRUE(t1->commit());
    ASSERT_TRUE(t2->commit());
}

TEST_F(Hermitage, OTV_ObservedTransactionVanishesIsPrevented) {
    auto t1 = db_->begin();
    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11")));
    ASSERT_TRUE(t1->put(as_bytes("2"), as_bytes("19")));

    auto t3 = db_->begin(); // snapshot taken before t1 commits
    ASSERT_TRUE(t1->commit());

    // t3 must see all of t1 or none of it — never the half-applied middle.
    const std::string a = read(*t3, "1");
    const std::string b = read(*t3, "2");
    const bool all_old = (a == "10" && b == "20");
    const bool all_new = (a == "11" && b == "19");
    EXPECT_TRUE(all_old || all_new) << "saw 1=" << a << " and 2=" << b;
    EXPECT_TRUE(all_old) << "a snapshot taken before the commit should see the old values";
    ASSERT_TRUE(t3->commit());
}

TEST_F(Hermitage, PMP_PredicateManyPrecedersIsPrevented) {
    auto t1 = db_->begin();

    // "Select where value = 30" — no such row yet.
    std::vector<std::pair<std::string, std::string>> before;
    ASSERT_TRUE(db_->scan(&before));
    std::string probe;
    EXPECT_FALSE(t1->get(as_bytes("3"), &probe));

    auto t2 = db_->begin();
    ASSERT_TRUE(t2->put(as_bytes("3"), as_bytes("30")));
    ASSERT_TRUE(t2->commit());

    // t1 repeats its predicate. The new row must not appear.
    EXPECT_FALSE(t1->get(as_bytes("3"), &probe))
        << "a row inserted after the snapshot became visible to it";
    ASSERT_TRUE(t1->commit());
}

TEST_F(Hermitage, P4_LostUpdateIsPrevented) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    EXPECT_EQ(read(*t1, "1"), "10");
    EXPECT_EQ(read(*t2, "1"), "10");

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11")));
    ASSERT_TRUE(t1->commit());

    ASSERT_TRUE(t2->put(as_bytes("1"), as_bytes("11")));
    const Status s = t2->commit();
    EXPECT_FALSE(s) << "t1's update was overwritten as if it never happened";
    EXPECT_EQ(s.code(), Code::Conflict);
}

TEST_F(Hermitage, GSingle_ReadSkewIsPrevented) {
    auto t1 = db_->begin();
    EXPECT_EQ(read(*t1, "1"), "10");

    auto t2 = db_->begin();
    ASSERT_TRUE(t2->put(as_bytes("1"), as_bytes("12")));
    ASSERT_TRUE(t2->put(as_bytes("2"), as_bytes("18")));
    ASSERT_TRUE(t2->commit());

    // t1 already saw the old 1. It must also see the old 2, or it has observed
    // a state of the database that never existed.
    EXPECT_EQ(read(*t1, "2"), "20") << "read skew: saw 1 before t2 and 2 after it";
    ASSERT_TRUE(t1->commit());
}

// --- permitted, and demonstrated to occur -----------------------------------

TEST_F(Hermitage, G2Item_WriteSkewIsAllowedUnderSnapshotIsolation) {
    // The canonical case: a constraint over two rows that each transaction
    // checks and neither violates alone, but that both together break.
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    EXPECT_EQ(read(*t1, "1"), "10");
    EXPECT_EQ(read(*t1, "2"), "20");
    EXPECT_EQ(read(*t2, "1"), "10");
    EXPECT_EQ(read(*t2, "2"), "20");

    ASSERT_TRUE(t1->put(as_bytes("1"), as_bytes("11")));
    ASSERT_TRUE(t2->put(as_bytes("2"), as_bytes("21")));

    // Disjoint write sets, so first-committer-wins has nothing to catch.
    EXPECT_TRUE(t1->commit());
    EXPECT_TRUE(t2->commit()) << "if this now fails, the isolation level has become "
                                 "stronger than snapshot isolation and the README is wrong";

    std::string v;
    ASSERT_TRUE(db_->get(as_bytes("1"), &v));
    EXPECT_EQ(v, "11");
    ASSERT_TRUE(db_->get(as_bytes("2"), &v));
    EXPECT_EQ(v, "21");
}

TEST_F(Hermitage, G2_AntiDependencyCycleIsAllowedUnderSnapshotIsolation) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();

    // Each reads the other's future write target and finds nothing there.
    std::string probe;
    EXPECT_FALSE(t1->get(as_bytes("3"), &probe));
    EXPECT_FALSE(t2->get(as_bytes("4"), &probe));

    ASSERT_TRUE(t1->put(as_bytes("3"), as_bytes("30")));
    ASSERT_TRUE(t2->put(as_bytes("4"), as_bytes("42")));

    EXPECT_TRUE(t1->commit());
    EXPECT_TRUE(t2->commit()) << "snapshot isolation permits this cycle; serialisable would not";
}

} // namespace
