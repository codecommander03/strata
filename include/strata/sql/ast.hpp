#pragma once

#include "strata/sql/token.hpp"
#include "strata/sql/value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace strata::sql {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class BinaryOperator : std::uint8_t {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
    Add,
    Subtract,
    Multiply,
    Divide,
};

enum class UnaryOperator : std::uint8_t {
    Not,
    Negate,
    IsNull,
    IsNotNull,
};

const char* binary_operator_name(BinaryOperator op);
const char* unary_operator_name(UnaryOperator op);

struct Literal {
    Value value;
};

struct ColumnRef {
    std::string name; ///< already lower-cased by the lexer
};

struct UnaryExpr {
    UnaryOperator op;
    ExprPtr operand;
};

struct BinaryExpr {
    BinaryOperator op;
    ExprPtr left;
    ExprPtr right;
};

struct Expr {
    std::variant<Literal, ColumnRef, UnaryExpr, BinaryExpr> node;
    SourceLocation loc;
};

ExprPtr make_literal(Value value, SourceLocation loc);
ExprPtr make_column(std::string name, SourceLocation loc);
ExprPtr make_unary(UnaryOperator op, ExprPtr operand, SourceLocation loc);
ExprPtr make_binary(BinaryOperator op, ExprPtr left, ExprPtr right, SourceLocation loc);

/// Renders an expression back to a parenthesised string. The parser tests
/// compare against this rather than poking at the tree, so a test reads as the
/// shape it expects: `(a = 1) AND (b < 2)`.
std::string to_string(const Expr& expr);

// --- statements -------------------------------------------------------------

struct ColumnDef {
    std::string name;
    Type type = Type::Text;
    bool primary_key = false;
    bool not_null = false;
};

struct CreateTable {
    std::string table;
    std::vector<ColumnDef> columns;
    bool if_not_exists = false;
};

struct DropTable {
    std::string table;
    bool if_exists = false;
};

struct CreateIndex {
    std::string name;
    std::string table;
    std::string column;
    bool if_not_exists = false;
};

struct DropIndex {
    std::string name;
    bool if_exists = false;
};

struct Insert {
    std::string table;
    std::vector<std::string> columns; ///< empty means "every column, in order"
    std::vector<std::vector<ExprPtr>> rows;
};

struct SelectItem {
    ExprPtr expr;
    std::string alias; ///< empty when none was given
};

struct OrderTerm {
    ExprPtr expr;
    bool descending = false;
};

struct Select {
    bool star = false;
    std::vector<SelectItem> items;
    std::string table; ///< empty for a SELECT with no FROM
    ExprPtr where;
    std::vector<OrderTerm> order_by;
    std::optional<std::int64_t> limit;
};

struct Assignment {
    std::string column;
    ExprPtr value;
};

struct Update {
    std::string table;
    std::vector<Assignment> assignments;
    ExprPtr where;
};

struct Delete {
    std::string table;
    ExprPtr where;
};

/// BEGIN / COMMIT / ROLLBACK. The transaction machinery already exists from
/// stage 2; this only gives it a spelling.
enum class TransactionControl : std::uint8_t { Begin, Commit, Rollback };

struct TransactionStatement {
    TransactionControl control = TransactionControl::Begin;
};

using Statement = std::variant<CreateTable, DropTable, CreateIndex, DropIndex, Insert, Select,
                               Update, Delete, TransactionStatement>;

/// A short description of what a statement is, for errors and tests.
std::string statement_kind(const Statement& statement);

} // namespace strata::sql
