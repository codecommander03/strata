#include "strata/sql/executor.hpp"
#include "strata/sql/session.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;
using strata::test::TempDb;

namespace {

class ExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_ = std::make_unique<TempDb>("executor");
        db_ = std::make_unique<Database>();
        ASSERT_TRUE(db_->open(temp_->path()));
        session_ = std::make_unique<Session>(*db_);
    }

    Status run(const std::string& sql) {
        ResultSet result;
        return session_->run(sql, &result);
    }

    ResultSet query(const std::string& sql) {
        ResultSet result;
        const Status s = session_->run(sql, &result);
        EXPECT_TRUE(s) << sql << "\n  -> " << s.to_string();
        return result;
    }

    /// The first column of every row, as text, for compact assertions.
    std::vector<std::string> column0(const std::string& sql) {
        const ResultSet result = query(sql);
        std::vector<std::string> out;
        for (const Row& row : result.rows) {
            out.push_back(row.empty() ? "<empty>" : row[0].to_string());
        }
        return out;
    }

    std::unique_ptr<TempDb> temp_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Session> session_;
};

TEST_F(ExecutorTest, CreateInsertSelectRoundTrip) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1, 'one'), (2, 'two')"));

    const ResultSet result = query("SELECT * FROM t ORDER BY a");
    ASSERT_EQ(result.columns.size(), 2u);
    EXPECT_EQ(result.columns[0], "a");
    EXPECT_EQ(result.columns[1], "b");
    ASSERT_EQ(result.rows.size(), 2u);
    EXPECT_EQ(result.rows[0][0].integer(), 1);
    EXPECT_EQ(result.rows[0][1].text(), "one");
}

TEST_F(ExecutorTest, InsertReportsRowsAffected) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    ResultSet result;
    ASSERT_TRUE(session_->run("INSERT INTO t VALUES (1), (2), (3)", &result));
    EXPECT_EQ(result.rows_affected, 3);
}

TEST_F(ExecutorTest, RejectsUnknownTablesAndColumns) {
    EXPECT_FALSE(run("SELECT * FROM nope"));
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    EXPECT_FALSE(run("SELECT b FROM t"));
    EXPECT_FALSE(run("INSERT INTO t (b) VALUES (1)"));
    EXPECT_FALSE(run("UPDATE t SET b = 1"));
}

TEST_F(ExecutorTest, DuplicateTableAndColumnNamesAreRejected) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    EXPECT_FALSE(run("CREATE TABLE t(a INTEGER)"));
    EXPECT_TRUE(run("CREATE TABLE IF NOT EXISTS t(a INTEGER)"));
    EXPECT_FALSE(run("CREATE TABLE u(a INTEGER, a TEXT)"));
}

TEST_F(ExecutorTest, UpdateReadsTheRowAsItWasBeforeTheStatement) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b INTEGER)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1, 2)"));

    // A swap only works if both assignments see the pre-statement row.
    ASSERT_TRUE(run("UPDATE t SET a = b, b = a"));

    const ResultSet result = query("SELECT a, b FROM t");
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0].integer(), 2);
    EXPECT_EQ(result.rows[0][1].integer(), 1) << "b was overwritten with the new a";
}

TEST_F(ExecutorTest, DeleteWithoutWhereEmptiesTheTable) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1), (2), (3)"));
    ResultSet result;
    ASSERT_TRUE(session_->run("DELETE FROM t", &result));
    EXPECT_EQ(result.rows_affected, 3);
    EXPECT_TRUE(column0("SELECT a FROM t").empty());
}

TEST_F(ExecutorTest, DropTableRemovesItsRowsSoANewTableCannotInheritThem) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1), (2), (3)"));
    ASSERT_TRUE(run("DROP TABLE t"));
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    EXPECT_TRUE(column0("SELECT a FROM t").empty()) << "the new table inherited the old rows";
}

TEST_F(ExecutorTest, OrderByIsStableForEqualKeys) {
    ASSERT_TRUE(run("CREATE TABLE t(k INTEGER, tag TEXT)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1,'a'),(1,'b'),(1,'c'),(0,'z')"));

    const ResultSet result = query("SELECT tag FROM t ORDER BY k");
    ASSERT_EQ(result.rows.size(), 4u);
    EXPECT_EQ(result.rows[0][0].text(), "z");
    // Insertion order is preserved among the equal keys.
    EXPECT_EQ(result.rows[1][0].text(), "a");
    EXPECT_EQ(result.rows[2][0].text(), "b");
    EXPECT_EQ(result.rows[3][0].text(), "c");
}

TEST_F(ExecutorTest, LimitStopsThePlanRatherThanTruncatingAFinishedResult) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(run("INSERT INTO t VALUES (" + std::to_string(i) + ")"));
    }
    const ResultSet result = query("SELECT a FROM t ORDER BY a LIMIT 3");
    ASSERT_EQ(result.rows.size(), 3u);
    EXPECT_EQ(result.rows[0][0].integer(), 0);
    EXPECT_NE(result.plan.find("Limit 3"), std::string::npos) << result.plan;
    EXPECT_NE(result.plan.find("SeqScan t"), std::string::npos) << result.plan;
}

TEST_F(ExecutorTest, ThePlanIsReportedInExecutionOrder) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b INTEGER)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1,2)"));

    const ResultSet result = query("SELECT a FROM t WHERE b > 1 ORDER BY a LIMIT 5");
    // Outermost first, each child indented under its parent.
    const std::size_t limit = result.plan.find("Limit");
    const std::size_t project = result.plan.find("Project");
    const std::size_t sort = result.plan.find("Sort");
    const std::size_t filter = result.plan.find("Filter");
    const std::size_t scan = result.plan.find("SeqScan");
    EXPECT_LT(limit, project) << result.plan;
    EXPECT_LT(project, sort) << result.plan;
    EXPECT_LT(sort, filter) << result.plan;
    EXPECT_LT(filter, scan) << result.plan;
}

TEST_F(ExecutorTest, SelectWithoutFromEvaluatesOnce) {
    const ResultSet result = query("SELECT 1 + 2 * 3");
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0].integer(), 7);
}

TEST_F(ExecutorTest, AutocommitMakesEachStatementDurableOnItsOwn) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1)"));

    // A fresh session on the same database sees it.
    Session other(*db_);
    ResultSet result;
    ASSERT_TRUE(other.run("SELECT a FROM t", &result));
    ASSERT_EQ(result.rows.size(), 1u);
}

TEST_F(ExecutorTest, AFailedStatementLeavesNothingBehindUnderAutocommit) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT NOT NULL)"));
    // The first row is fine, the second violates NOT NULL. Neither must land.
    EXPECT_FALSE(run("INSERT INTO t VALUES (1, 'ok'), (2, NULL)"));
    EXPECT_TRUE(column0("SELECT a FROM t").empty())
        << "a partially applied INSERT survived its own failure";
}

TEST_F(ExecutorTest, DataSurvivesReopeningTheDatabase) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1, 'kept')"));

    session_.reset();
    db_->close();
    ASSERT_TRUE(db_->open(temp_->path()));
    session_ = std::make_unique<Session>(*db_);

    const ResultSet result = query("SELECT b FROM t");
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0].text(), "kept");
}

TEST_F(ExecutorTest, HandlesAThousandRows) {
    ASSERT_TRUE(run("CREATE TABLE big(a INTEGER, b TEXT)"));
    ResultSet ignored;
    ASSERT_TRUE(session_->run("BEGIN", &ignored));
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(run("INSERT INTO big VALUES (" + std::to_string(i) + ", 'row')"));
    }
    ASSERT_TRUE(session_->run("COMMIT", &ignored));

    const ResultSet all = query("SELECT a FROM big");
    EXPECT_EQ(all.rows.size(), 1000u);

    const ResultSet some = query("SELECT a FROM big WHERE a >= 990 ORDER BY a");
    ASSERT_EQ(some.rows.size(), 10u);
    EXPECT_EQ(some.rows[0][0].integer(), 990);
}

// --- evaluation -------------------------------------------------------------

TEST(Evaluate, CoercionFollowsColumnAffinity) {
    EXPECT_EQ(coerce(Value(std::int64_t{5}), Type::Real).type(), Type::Real);
    EXPECT_EQ(coerce(Value(5.0), Type::Integer).type(), Type::Integer);
    // 1.5 cannot become an integer without losing information, so it does not.
    EXPECT_EQ(coerce(Value(1.5), Type::Integer).type(), Type::Real);
    EXPECT_EQ(coerce(Value(std::string("17")), Type::Integer).integer(), 17);
    EXPECT_EQ(coerce(Value(std::string("abc")), Type::Integer).type(), Type::Text);
    EXPECT_TRUE(coerce(Value::null(), Type::Integer).is_null());
}

TEST(CompareValues, PutsNullsFirstThenNumbersThenText) {
    EXPECT_LT(compare_values(Value::null(), Value(std::int64_t{0})), 0);
    EXPECT_LT(compare_values(Value(std::int64_t{1}), Value(std::string("a"))), 0);
    EXPECT_EQ(compare_values(Value(std::int64_t{2}), Value(2.0)), 0);
    EXPECT_LT(compare_values(Value(std::int64_t{1}), Value(2.5)), 0);
}

TEST(CompareValues, DoesNotLosePrecisionOnLargeIntegers) {
    // Two distinct int64s that collapse to the same double if converted.
    const std::int64_t a = 9007199254740993LL;
    const std::int64_t b = 9007199254740992LL;
    EXPECT_NE(compare_values(Value(a), Value(b)), 0);
}

} // namespace
