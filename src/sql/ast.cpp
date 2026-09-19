#include "strata/sql/ast.hpp"

#include <cmath>
#include <cstdio>

namespace strata::sql {

std::string Value::to_string() const {
    switch (type()) {
    case Type::Null:
        return "NULL";
    case Type::Integer:
        return std::to_string(integer());
    case Type::Real: {
        // Enough digits to round-trip a double, without a trailing ".000000".
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.17g", real());
        return buffer;
    }
    case Type::Text:
        return text();
    }
    return "?";
}

int compare_values(const Value& a, const Value& b) {
    // Null sorts before everything, and two nulls are equal *for ordering*.
    // That is separate from SQL's `null = null` being unknown, which the
    // evaluator handles before it ever reaches here.
    if (a.is_null() || b.is_null()) {
        if (a.is_null() && b.is_null()) {
            return 0;
        }
        return a.is_null() ? -1 : 1;
    }

    if (a.is_numeric() && b.is_numeric()) {
        // Compare integers exactly; a double cannot hold every int64 and
        // converting first would make two distinct large integers equal.
        if (a.type() == Type::Integer && b.type() == Type::Integer) {
            const std::int64_t x = a.integer();
            const std::int64_t y = b.integer();
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        const double x = a.as_double();
        const double y = b.as_double();
        return x < y ? -1 : (x > y ? 1 : 0);
    }

    // Numbers sort before text, as in SQLite.
    if (a.is_numeric() != b.is_numeric()) {
        return a.is_numeric() ? -1 : 1;
    }

    const int c = a.text().compare(b.text());
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

const char* binary_operator_name(BinaryOperator op) {
    switch (op) {
    case BinaryOperator::Equal:
        return "=";
    case BinaryOperator::NotEqual:
        return "<>";
    case BinaryOperator::Less:
        return "<";
    case BinaryOperator::LessEqual:
        return "<=";
    case BinaryOperator::Greater:
        return ">";
    case BinaryOperator::GreaterEqual:
        return ">=";
    case BinaryOperator::And:
        return "AND";
    case BinaryOperator::Or:
        return "OR";
    case BinaryOperator::Add:
        return "+";
    case BinaryOperator::Subtract:
        return "-";
    case BinaryOperator::Multiply:
        return "*";
    case BinaryOperator::Divide:
        return "/";
    }
    return "?";
}

const char* unary_operator_name(UnaryOperator op) {
    switch (op) {
    case UnaryOperator::Not:
        return "NOT";
    case UnaryOperator::Negate:
        return "-";
    case UnaryOperator::IsNull:
        return "IS NULL";
    case UnaryOperator::IsNotNull:
        return "IS NOT NULL";
    }
    return "?";
}

ExprPtr make_literal(Value value, SourceLocation loc) {
    auto expr = std::make_unique<Expr>();
    expr->node = Literal{std::move(value)};
    expr->loc = loc;
    return expr;
}

ExprPtr make_column(std::string name, SourceLocation loc) {
    auto expr = std::make_unique<Expr>();
    expr->node = ColumnRef{std::move(name)};
    expr->loc = loc;
    return expr;
}

ExprPtr make_unary(UnaryOperator op, ExprPtr operand, SourceLocation loc) {
    auto expr = std::make_unique<Expr>();
    expr->node = UnaryExpr{op, std::move(operand)};
    expr->loc = loc;
    return expr;
}

ExprPtr make_binary(BinaryOperator op, ExprPtr left, ExprPtr right, SourceLocation loc) {
    auto expr = std::make_unique<Expr>();
    expr->node = BinaryExpr{op, std::move(left), std::move(right)};
    expr->loc = loc;
    return expr;
}

std::string to_string(const Expr& expr) {
    return std::visit(
        [](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, Literal>) {
                if (node.value.type() == Type::Text) {
                    return "'" + node.value.text() + "'";
                }
                return node.value.to_string();
            } else if constexpr (std::is_same_v<T, ColumnRef>) {
                return node.name;
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                const std::string inner = to_string(*node.operand);
                if (node.op == UnaryOperator::IsNull || node.op == UnaryOperator::IsNotNull) {
                    return "(" + inner + " " + unary_operator_name(node.op) + ")";
                }
                return std::string("(") + unary_operator_name(node.op) + " " + inner + ")";
            } else {
                return "(" + to_string(*node.left) + " " + binary_operator_name(node.op) + " " +
                       to_string(*node.right) + ")";
            }
        },
        expr.node);
}

std::string statement_kind(const Statement& statement) {
    return std::visit(
        [](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, CreateTable>) {
                return "CREATE TABLE";
            } else if constexpr (std::is_same_v<T, DropTable>) {
                return "DROP TABLE";
            } else if constexpr (std::is_same_v<T, CreateIndex>) {
                return "CREATE INDEX";
            } else if constexpr (std::is_same_v<T, DropIndex>) {
                return "DROP INDEX";
            } else if constexpr (std::is_same_v<T, Insert>) {
                return "INSERT";
            } else if constexpr (std::is_same_v<T, Select>) {
                return "SELECT";
            } else if constexpr (std::is_same_v<T, Update>) {
                return "UPDATE";
            } else if constexpr (std::is_same_v<T, Delete>) {
                return "DELETE";
            } else {
                switch (s.control) {
                case TransactionControl::Begin:
                    return "BEGIN";
                case TransactionControl::Commit:
                    return "COMMIT";
                default:
                    return "ROLLBACK";
                }
            }
        },
        statement);
}

} // namespace strata::sql
