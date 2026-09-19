#include "strata/sql/slt.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;
using strata::test::TempDb;

namespace {

/// The corpus lives next to the source, and the test binary runs from the
/// build directory, so the path is resolved from a compile-time define.
std::filesystem::path testdata_dir() { return std::filesystem::path(STRATA_TESTDATA_DIR); }

SltReport run_file(const std::string& name) {
    TempDb db("slt_" + name);
    SltReport report;
    const std::string path = (testdata_dir() / (name + ".test")).string();
    const Status s = run_slt_file(path, db.path(), &report);
    EXPECT_TRUE(s) << "could not run " << path << ": " << s.to_string();
    return report;
}

void expect_clean(const SltReport& report, const char* name) {
    for (const std::string& failure : report.failures) {
        ADD_FAILURE() << name << ": " << failure;
    }
    EXPECT_GT(report.total_run(), 0) << name << ": the file ran nothing";
}

TEST(SqlLogicTest, Select1) {
    const SltReport report = run_file("select1");
    expect_clean(report, "select1");
    EXPECT_EQ(report.total_passed(), report.total_run());
    // The gaps are declared in the file rather than inferred from failures.
    EXPECT_EQ(report.unsupported, 4);
}

TEST(SqlLogicTest, Nulls) {
    const SltReport report = run_file("nulls");
    expect_clean(report, "nulls");
    EXPECT_EQ(report.total_passed(), report.total_run());
}

TEST(SqlLogicTest, Dml) {
    const SltReport report = run_file("dml");
    expect_clean(report, "dml");
    EXPECT_EQ(report.total_passed(), report.total_run());
}

TEST(SqlLogicTest, Transactions) {
    const SltReport report = run_file("transactions");
    expect_clean(report, "transactions");
    EXPECT_EQ(report.total_passed(), report.total_run());
}

/// Every query in this file runs twice, before and after the index exists,
/// with the same expected output. An index that changes an answer is a bug.
TEST(SqlLogicTest, Indexes) {
    const SltReport report = run_file("indexes");
    expect_clean(report, "indexes");
    EXPECT_EQ(report.total_passed(), report.total_run());
    EXPECT_EQ(report.unsupported, 1);
}

/// Prints the combined figure that goes in the README, and fails if the whole
/// corpus is not green.
TEST(SqlLogicTest, WholeCorpus) {
    SltReport total;
    for (const char* name : {"select1", "nulls", "dml", "transactions", "indexes"}) {
        const SltReport report = run_file(name);
        total.statements_run += report.statements_run;
        total.statements_passed += report.statements_passed;
        total.queries_run += report.queries_run;
        total.queries_passed += report.queries_passed;
        total.unsupported += report.unsupported;
        for (const auto& reason : report.unsupported_reasons) {
            total.unsupported_reasons.push_back(reason);
        }
        for (const auto& failure : report.failures) {
            total.failures.push_back(std::string(name) + ": " + failure);
        }
    }

    std::cout << "\n  sqllogictest corpus: " << total.summary() << "\n";
    for (const std::string& reason : total.unsupported_reasons) {
        std::cout << "    unsupported: " << reason << "\n";
    }
    std::cout << std::endl;

    for (const std::string& failure : total.failures) {
        ADD_FAILURE() << failure;
    }
    EXPECT_EQ(total.total_passed(), total.total_run());
}

/// The runner must actually detect a wrong answer, or every pass above is
/// worthless.
TEST(SqlLogicTest, TheRunnerCatchesAWrongAnswer) {
    TempDb db("slt_negative");
    SltReport report;
    const std::string script = R"(statement ok
CREATE TABLE t(a INTEGER)

statement ok
INSERT INTO t VALUES (1)

query I nosort
SELECT a FROM t
----
999
)";
    ASSERT_TRUE(run_slt_script(script, db.path(), &report));
    EXPECT_EQ(report.queries_run, 1);
    EXPECT_EQ(report.queries_passed, 0);
    ASSERT_EQ(report.failures.size(), 1u);
    EXPECT_NE(report.failures[0].find("wrong answer"), std::string::npos);
}

TEST(SqlLogicTest, TheRunnerCatchesAStatementThatShouldHaveFailed) {
    TempDb db("slt_negative2");
    SltReport report;
    const std::string script = R"(statement ok
CREATE TABLE t(a INTEGER)

statement error
INSERT INTO t VALUES (1)
)";
    ASSERT_TRUE(run_slt_script(script, db.path(), &report));
    EXPECT_EQ(report.statements_passed, 1);
    EXPECT_EQ(report.statements_run, 2);
    ASSERT_EQ(report.failures.size(), 1u);
    EXPECT_NE(report.failures[0].find("should have failed"), std::string::npos);
}

TEST(SqlLogicTest, UnsupportedBlocksAreCountedNotSkipped) {
    TempDb db("slt_unsupported");
    SltReport report;
    const std::string script = R"(#unsupported: pretend feature
query I nosort
SELECT nothing()
----
1
)";
    ASSERT_TRUE(run_slt_script(script, db.path(), &report));
    EXPECT_EQ(report.unsupported, 1);
    EXPECT_EQ(report.queries_run, 0) << "an unsupported block must not inflate the denominator";
    EXPECT_EQ(report.total_passed(), 0) << "nor the numerator";
    ASSERT_EQ(report.unsupported_reasons.size(), 1u);
    EXPECT_EQ(report.unsupported_reasons[0], "pretend feature");
}

} // namespace
