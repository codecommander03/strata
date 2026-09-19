#include "strata/sql/lexer.hpp"
#include "strata/sql/parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace strata::sql;

namespace {

/// Parses and returns the statement, failing the test with the error message
/// if it does not parse.
Statement must_parse(const std::string& sql) {
    ParseError error;
    Parser parser(sql);
    Statement out;
    EXPECT_TRUE(parser.parse_one(&out, &error)) << sql << "\n  -> " << error.to_string();
    return out;
}

/// Parses an expression and renders it back, so a test can assert the shape as
/// a string instead of walking the tree.
std::string shape_of(const std::string& sql) {
    ParseError error;
    Parser parser(sql);
    ExprPtr expr;
    if (!parser.parse_expression(&expr, &error)) {
        return "ERROR " + error.to_string();
    }
    return to_string(*expr);
}

/// Returns "line:column: message" for input that must fail to parse.
std::string error_from(const std::string& sql) {
    ParseError error;
    Parser parser(sql);
    std::vector<Statement> out;
    if (parser.parse(&out, &error)) {
        return "<parsed successfully>";
    }
    return error.to_string();
}

// --- lexer ------------------------------------------------------------------

TEST(Lexer, FoldsKeywordsAndIdentifiersToLowerCase) {
    Lexer lexer("SeLeCt Foo FROM Bar");
    std::vector<Token> tokens;
    ParseError error;
    ASSERT_TRUE(lexer.tokenise(&tokens, &error)) << error.to_string();

    ASSERT_EQ(tokens.size(), 5u); // select, foo, from, bar, eof
    EXPECT_EQ(tokens[0].type, TokenType::Select);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
    EXPECT_EQ(tokens[1].text, "foo");
    EXPECT_EQ(tokens[2].type, TokenType::From);
    EXPECT_EQ(tokens[3].text, "bar");
}

TEST(Lexer, TracksLineAndColumnAcrossNewlines) {
    Lexer lexer("select\n  x\nfrom t");
    std::vector<Token> tokens;
    ParseError error;
    ASSERT_TRUE(lexer.tokenise(&tokens, &error));

    EXPECT_EQ(tokens[0].loc.line, 1);
    EXPECT_EQ(tokens[0].loc.column, 1);
    EXPECT_EQ(tokens[1].loc.line, 2);
    EXPECT_EQ(tokens[1].loc.column, 3);
    EXPECT_EQ(tokens[2].loc.line, 3);
    EXPECT_EQ(tokens[2].loc.column, 1);
}

TEST(Lexer, ReadsNumbersAndStrings) {
    Lexer lexer("42 3.5 1e3 'hello' 'don''t'");
    std::vector<Token> tokens;
    ParseError error;
    ASSERT_TRUE(lexer.tokenise(&tokens, &error)) << error.to_string();

    EXPECT_EQ(tokens[0].type, TokenType::IntegerLiteral);
    EXPECT_EQ(tokens[0].integer, 42);
    EXPECT_EQ(tokens[1].type, TokenType::RealLiteral);
    EXPECT_DOUBLE_EQ(tokens[1].real, 3.5);
    EXPECT_EQ(tokens[2].type, TokenType::RealLiteral);
    EXPECT_DOUBLE_EQ(tokens[2].real, 1000.0);
    EXPECT_EQ(tokens[3].text, "hello");
    EXPECT_EQ(tokens[4].text, "don't") << "'' inside a string is an escaped quote";
}

TEST(Lexer, SkipsBothCommentStyles) {
    Lexer lexer("select -- a line comment\n /* and a\n block one */ x");
    std::vector<Token> tokens;
    ParseError error;
    ASSERT_TRUE(lexer.tokenise(&tokens, &error));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[1].text, "x");
}

TEST(Lexer, SkipsAUtf8ByteOrderMark) {
    // Windows editors and shells prepend one routinely; without this the first
    // statement of every piped script fails on an invisible character.
    const std::string with_bom = "\xEF\xBB\xBFSELECT 1";
    ParseError error;
    Parser parser(with_bom);
    Statement out;
    EXPECT_TRUE(parser.parse_one(&out, &error)) << error.to_string();
}

TEST(Lexer, ReportsAnUnterminatedStringAtItsOpeningQuote) {
    // Pointing at the end of input would be useless in a long statement.
    EXPECT_EQ(error_from("select 'oops"), "1:8: unterminated string literal");
}

TEST(Lexer, ReportsAStrayCharacterWhereItIs) {
    EXPECT_EQ(error_from("select x # y from t"), "1:10: unexpected character '#'");
}

// --- expressions ------------------------------------------------------------

TEST(ParseExpression, AppliesArithmeticPrecedence) {
    EXPECT_EQ(shape_of("1 + 2 * 3"), "(1 + (2 * 3))");
    EXPECT_EQ(shape_of("1 * 2 + 3"), "((1 * 2) + 3)");
    EXPECT_EQ(shape_of("(1 + 2) * 3"), "((1 + 2) * 3)");
}

TEST(ParseExpression, BindsComparisonLooserThanArithmeticAndTighterThanAnd) {
    EXPECT_EQ(shape_of("a + 1 = 2"), "((a + 1) = 2)");
    EXPECT_EQ(shape_of("a = 1 and b = 2"), "((a = 1) AND (b = 2))");
    EXPECT_EQ(shape_of("a = 1 or b = 2 and c = 3"), "((a = 1) OR ((b = 2) AND (c = 3)))");
}

TEST(ParseExpression, IsLeftAssociative) {
    EXPECT_EQ(shape_of("10 - 3 - 2"), "((10 - 3) - 2)") << "right-associative would give 10-(3-2)";
}

TEST(ParseExpression, HandlesUnaryOperators) {
    EXPECT_EQ(shape_of("-5"), "(- 5)");
    EXPECT_EQ(shape_of("x is null"), "(x IS NULL)");
    EXPECT_EQ(shape_of("x is not null"), "(x IS NOT NULL)");
    EXPECT_EQ(shape_of("x is null and y = 1"), "((x IS NULL) AND (y = 1))");
}

TEST(ParseExpression, NotBindsLooserThanComparisonButTighterThanAnd) {
    // The whole comparison is negated, not just its left operand. Getting this
    // backwards silently changes what every WHERE clause means.
    EXPECT_EQ(shape_of("not a = 1"), "(NOT (a = 1))");
    EXPECT_EQ(shape_of("not a > 2"), "(NOT (a > 2))");
    EXPECT_EQ(shape_of("not a + 1 = 2"), "(NOT ((a + 1) = 2))");

    // But AND stops it: `NOT a AND b` is `(NOT a) AND b`.
    EXPECT_EQ(shape_of("not a and b"), "((NOT a) AND b)");
    EXPECT_EQ(shape_of("a = 1 and not b = 2"), "((a = 1) AND (NOT (b = 2)))");
    EXPECT_EQ(shape_of("not not a"), "(NOT (NOT a))");
}

TEST(ParseExpression, UnaryMinusStillBindsTightest) {
    EXPECT_EQ(shape_of("-a + b"), "((- a) + b)") << "not -(a + b)";
    EXPECT_EQ(shape_of("-a * b"), "((- a) * b)");
}

TEST(ParseExpression, KeepsStringAndNullLiteralsDistinct) {
    EXPECT_EQ(shape_of("'null'"), "'null'");
    EXPECT_EQ(shape_of("null"), "NULL");
}

// --- statements -------------------------------------------------------------

TEST(ParseStatement, CreateTable) {
    const Statement statement =
        must_parse("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL)");
    const auto* create = std::get_if<CreateTable>(&statement);
    ASSERT_NE(create, nullptr);

    EXPECT_EQ(create->table, "users");
    EXPECT_FALSE(create->if_not_exists);
    ASSERT_EQ(create->columns.size(), 3u);

    EXPECT_EQ(create->columns[0].name, "id");
    EXPECT_EQ(create->columns[0].type, Type::Integer);
    EXPECT_TRUE(create->columns[0].primary_key);

    EXPECT_EQ(create->columns[1].name, "name");
    EXPECT_EQ(create->columns[1].type, Type::Text);
    EXPECT_TRUE(create->columns[1].not_null);
    EXPECT_FALSE(create->columns[1].primary_key);

    EXPECT_EQ(create->columns[2].type, Type::Real);
}

TEST(ParseStatement, CreateTableIfNotExists) {
    const Statement statement = must_parse("CREATE TABLE IF NOT EXISTS t (a INTEGER)");
    const auto* create = std::get_if<CreateTable>(&statement);
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(create->if_not_exists);
}

TEST(ParseStatement, DropTable) {
    const Statement statement = must_parse("DROP TABLE IF EXISTS users");
    const auto* drop = std::get_if<DropTable>(&statement);
    ASSERT_NE(drop, nullptr);
    EXPECT_EQ(drop->table, "users");
    EXPECT_TRUE(drop->if_exists);
}

TEST(ParseStatement, InsertWithNamedColumnsAndSeveralRows) {
    const Statement statement =
        must_parse("INSERT INTO t (a, b) VALUES (1, 'x'), (2, 'y'), (3, NULL)");
    const auto* insert = std::get_if<Insert>(&statement);
    ASSERT_NE(insert, nullptr);

    EXPECT_EQ(insert->table, "t");
    ASSERT_EQ(insert->columns.size(), 2u);
    EXPECT_EQ(insert->columns[0], "a");
    ASSERT_EQ(insert->rows.size(), 3u);
    EXPECT_EQ(to_string(*insert->rows[0][1]), "'x'");
    EXPECT_EQ(to_string(*insert->rows[2][1]), "NULL");
}

TEST(ParseStatement, InsertWithoutColumnList) {
    const Statement statement = must_parse("INSERT INTO t VALUES (1, 2)");
    const auto* insert = std::get_if<Insert>(&statement);
    ASSERT_NE(insert, nullptr);
    EXPECT_TRUE(insert->columns.empty()) << "empty means every column, in declaration order";
    ASSERT_EQ(insert->rows.size(), 1u);
}

TEST(ParseStatement, SelectStar) {
    const Statement statement = must_parse("SELECT * FROM t");
    const auto* select = std::get_if<Select>(&statement);
    ASSERT_NE(select, nullptr);
    EXPECT_TRUE(select->star);
    EXPECT_EQ(select->table, "t");
    EXPECT_EQ(select->where, nullptr);
}

TEST(ParseStatement, SelectWithEveryClause) {
    const Statement statement =
        must_parse("SELECT a, b + 1 total FROM t WHERE a > 5 AND b IS NOT NULL "
                   "ORDER BY a DESC, b LIMIT 10");
    const auto* select = std::get_if<Select>(&statement);
    ASSERT_NE(select, nullptr);

    EXPECT_FALSE(select->star);
    ASSERT_EQ(select->items.size(), 2u);
    EXPECT_EQ(to_string(*select->items[0].expr), "a");
    EXPECT_EQ(to_string(*select->items[1].expr), "(b + 1)");
    EXPECT_EQ(select->items[1].alias, "total");

    EXPECT_EQ(select->table, "t");
    ASSERT_NE(select->where, nullptr);
    EXPECT_EQ(to_string(*select->where), "((a > 5) AND (b IS NOT NULL))");

    ASSERT_EQ(select->order_by.size(), 2u);
    EXPECT_EQ(to_string(*select->order_by[0].expr), "a");
    EXPECT_TRUE(select->order_by[0].descending);
    EXPECT_FALSE(select->order_by[1].descending) << "ASC is the default";

    ASSERT_TRUE(select->limit.has_value());
    EXPECT_EQ(*select->limit, 10);
}

TEST(ParseStatement, Update) {
    const Statement statement = must_parse("UPDATE t SET a = 1, b = a + 2 WHERE id = 7");
    const auto* update = std::get_if<Update>(&statement);
    ASSERT_NE(update, nullptr);

    EXPECT_EQ(update->table, "t");
    ASSERT_EQ(update->assignments.size(), 2u);
    EXPECT_EQ(update->assignments[0].column, "a");
    EXPECT_EQ(to_string(*update->assignments[1].value), "(a + 2)");
    ASSERT_NE(update->where, nullptr);
    EXPECT_EQ(to_string(*update->where), "(id = 7)");
}

TEST(ParseStatement, DeleteWithAndWithoutWhere) {
    {
        const Statement statement = must_parse("DELETE FROM t WHERE a < 3");
        const auto* del = std::get_if<Delete>(&statement);
        ASSERT_NE(del, nullptr);
        EXPECT_EQ(del->table, "t");
        ASSERT_NE(del->where, nullptr);
    }
    {
        const Statement statement = must_parse("DELETE FROM t");
        const auto* del = std::get_if<Delete>(&statement);
        ASSERT_NE(del, nullptr);
        EXPECT_EQ(del->where, nullptr) << "no WHERE means every row";
    }
}

TEST(ParseStatement, TransactionControl) {
    EXPECT_EQ(statement_kind(must_parse("BEGIN")), "BEGIN");
    EXPECT_EQ(statement_kind(must_parse("BEGIN TRANSACTION")), "BEGIN");
    EXPECT_EQ(statement_kind(must_parse("COMMIT")), "COMMIT");
    EXPECT_EQ(statement_kind(must_parse("ROLLBACK")), "ROLLBACK");
}

TEST(ParseStatement, SeveralStatementsSeparatedBySemicolons) {
    ParseError error;
    Parser parser("CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1); SELECT * FROM t;");
    std::vector<Statement> statements;
    ASSERT_TRUE(parser.parse(&statements, &error)) << error.to_string();

    ASSERT_EQ(statements.size(), 3u);
    EXPECT_EQ(statement_kind(statements[0]), "CREATE TABLE");
    EXPECT_EQ(statement_kind(statements[1]), "INSERT");
    EXPECT_EQ(statement_kind(statements[2]), "SELECT");
}

// --- error locations: the stage 3 gate --------------------------------------

TEST(ParseError, PointsAtTheOffendingTokenNotTheEndOfTheStatement) {
    // Each of these names the column of the thing that is actually wrong.
    EXPECT_EQ(error_from("SELECT * FROM"), "1:14: expected a name, found end of input");
    EXPECT_EQ(error_from("SELECT * FROM 42"), "1:15: expected a name, found integer '42'");
    EXPECT_EQ(error_from("SELECT FROM t"),
              "1:8: expected a value, a column name or '(', found FROM");
    EXPECT_EQ(error_from("INSERT INTO t VALUES 1"), "1:22: expected '(', found integer '1'");
    EXPECT_EQ(error_from("UPDATE t a = 1"), "1:10: expected SET, found identifier 'a'");
    EXPECT_EQ(error_from("DELETE t"), "1:8: expected FROM, found identifier 't'");
    EXPECT_EQ(error_from("SELECT * FROM t WHERE"),
              "1:22: expected a value, a column name or '(', found end of input");
    EXPECT_EQ(error_from("SELECT * FROM t LIMIT x"),
              "1:23: expected an integer after LIMIT, found identifier 'x'");
}

TEST(ParseError, ReportsTheRightLineInAMultiLineStatement) {
    const std::string sql = "SELECT a,\n       b,\n       *bad\nFROM t";
    EXPECT_EQ(error_from(sql), "3:8: expected a value, a column name or '(', found '*'");
}

TEST(ParseError, ReportsAnUnclosedParenthesisAtWhatFollowsIt) {
    EXPECT_EQ(error_from("SELECT (1 + 2 FROM t"), "1:15: expected ')', found FROM");
}

TEST(ParseError, ExplainsAColumnCountMismatchInInsert) {
    EXPECT_EQ(error_from("INSERT INTO t (a, b) VALUES (1)"),
              "1:31: this row has 1 values but 2 columns were named");
    EXPECT_EQ(error_from("INSERT INTO t VALUES (1, 2), (3)"),
              "1:32: this row has 1 values but the first row has 2");
}

TEST(ParseError, RejectsAnUnknownColumnType) {
    EXPECT_EQ(error_from("CREATE TABLE t (a BLOB)"),
              "1:19: expected a column type (INTEGER, REAL or TEXT), found identifier 'blob'");
}

TEST(ParseError, RejectsTrailingJunkAfterAStatement) {
    EXPECT_EQ(error_from("SELECT * FROM t t2"), "1:17: expected ';' or end of input, "
                                                "found identifier 't2'");
}

} // namespace
