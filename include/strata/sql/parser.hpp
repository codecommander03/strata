#pragma once

#include "strata/sql/ast.hpp"
#include "strata/sql/lexer.hpp"
#include "strata/sql/token.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace strata::sql {

/// Recursive descent, one statement at a time.
///
/// Expressions use precedence climbing rather than a separate function per
/// level: the table in `binary_precedence` is the grammar, so adding an
/// operator is a table entry rather than a new function wedged between two
/// others.
///
/// Every error carries the location of the token that caused it, not the
/// location the parser had reached — those differ, and the difference is the
/// whole of whether an error message is useful.
class Parser {
public:
    explicit Parser(std::string_view sql) : sql_(sql) {}

    /// Parses one or more statements separated by semicolons.
    bool parse(std::vector<Statement>* out, ParseError* error);

    /// Parses exactly one statement and fails if anything follows it.
    bool parse_one(Statement* out, ParseError* error);

    /// Parses a bare expression. Used by the tests and by the playground.
    bool parse_expression(ExprPtr* out, ParseError* error);

private:
    bool prime(ParseError* error);

    const Token& peek(std::size_t ahead = 0) const;
    const Token& previous() const;
    bool check(TokenType type) const { return peek().type == type; }
    bool match(TokenType type);
    bool expect(TokenType type, ParseError* error);
    bool expect_identifier(std::string* out, ParseError* error);
    void unexpected(const char* wanted, ParseError* error) const;

    bool statement(Statement* out, ParseError* error);
    bool create_table(Statement* out, ParseError* error);
    bool drop_table(Statement* out, ParseError* error);
    bool insert(Statement* out, ParseError* error);
    bool select(Statement* out, ParseError* error);
    bool update(Statement* out, ParseError* error);
    bool delete_from(Statement* out, ParseError* error);

    bool expression(ExprPtr* out, ParseError* error) { return binary(0, out, error); }
    bool binary(int min_precedence, ExprPtr* out, ParseError* error);
    bool unary(ExprPtr* out, ParseError* error);
    bool primary(ExprPtr* out, ParseError* error);
    bool postfix(ExprPtr* out, ParseError* error);

    std::string_view sql_;
    std::vector<Token> tokens_;
    std::size_t index_ = 0;
    bool primed_ = false;
};

/// Convenience: parse a single statement out of a string.
std::optional<Statement> parse_statement(std::string_view sql, ParseError* error);

} // namespace strata::sql
