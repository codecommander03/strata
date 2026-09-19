#include "strata/sql/parser.hpp"

#include <utility>

namespace strata::sql {
namespace {

/// SQL's precedence ladder, loosest first:
///
///     OR  <  AND  <  NOT  <  comparison  <  + -  <  * /
///
/// NOT sits *below* comparison, which is the part that is easy to get wrong:
/// `NOT a > 2` means `NOT (a > 2)`, not `(NOT a) > 2`. Unary minus is a
/// different matter and binds tightest of all, so `-a + b` is `(-a) + b`.
constexpr int kNotPrecedence = 3;

/// Binding power, higher binds tighter. Zero means "not a binary operator",
/// which is how the precedence-climbing loop knows where an expression ends.
int binary_precedence(TokenType type) {
    switch (type) {
    case TokenType::Or:
        return 1;
    case TokenType::And:
        return 2;
    case TokenType::Equal:
    case TokenType::NotEqual:
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
        return 4;
    case TokenType::Plus:
    case TokenType::Minus:
        return 5;
    case TokenType::Star:
    case TokenType::Slash:
        return 6;
    default:
        return 0;
    }
}

BinaryOperator binary_operator_for(TokenType type) {
    switch (type) {
    case TokenType::Equal:
        return BinaryOperator::Equal;
    case TokenType::NotEqual:
        return BinaryOperator::NotEqual;
    case TokenType::Less:
        return BinaryOperator::Less;
    case TokenType::LessEqual:
        return BinaryOperator::LessEqual;
    case TokenType::Greater:
        return BinaryOperator::Greater;
    case TokenType::GreaterEqual:
        return BinaryOperator::GreaterEqual;
    case TokenType::And:
        return BinaryOperator::And;
    case TokenType::Or:
        return BinaryOperator::Or;
    case TokenType::Plus:
        return BinaryOperator::Add;
    case TokenType::Minus:
        return BinaryOperator::Subtract;
    case TokenType::Star:
        return BinaryOperator::Multiply;
    default:
        return BinaryOperator::Divide;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Token plumbing
// ---------------------------------------------------------------------------

bool Parser::prime(ParseError* error) {
    if (primed_) {
        return true;
    }
    Lexer lexer(sql_);
    if (!lexer.tokenise(&tokens_, error)) {
        return false;
    }
    primed_ = true;
    return true;
}

const Token& Parser::peek(std::size_t ahead) const {
    const std::size_t at = index_ + ahead;
    return at < tokens_.size() ? tokens_[at] : tokens_.back();
}

const Token& Parser::previous() const { return tokens_[index_ == 0 ? 0 : index_ - 1]; }

bool Parser::match(TokenType type) {
    if (!check(type)) {
        return false;
    }
    ++index_;
    return true;
}

void Parser::unexpected(const char* wanted, ParseError* error) const {
    const Token& token = peek();
    std::string found = token_type_name(token.type);
    if (token.type == TokenType::Identifier || token.type == TokenType::StringLiteral ||
        token.type == TokenType::IntegerLiteral || token.type == TokenType::RealLiteral) {
        found += " '" + token.text + "'";
    }
    // The location is the offending token's, not wherever the parser happens
    // to have got to. Those differ, and the difference is the whole of whether
    // the message is useful.
    *error = ParseError{std::string("expected ") + wanted + ", found " + found, token.loc};
}

bool Parser::expect(TokenType type, ParseError* error) {
    if (match(type)) {
        return true;
    }
    unexpected(token_type_name(type), error);
    return false;
}

bool Parser::expect_identifier(std::string* out, ParseError* error) {
    if (!check(TokenType::Identifier)) {
        unexpected("a name", error);
        return false;
    }
    *out = peek().text;
    ++index_;
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool Parser::parse(std::vector<Statement>* out, ParseError* error) {
    if (!prime(error)) {
        return false;
    }
    out->clear();

    while (!check(TokenType::EndOfInput)) {
        if (match(TokenType::Semicolon)) {
            continue; // empty statement, or a trailing separator
        }
        Statement statement_out;
        if (!statement(&statement_out, error)) {
            return false;
        }
        out->push_back(std::move(statement_out));
        if (!check(TokenType::EndOfInput) && !match(TokenType::Semicolon)) {
            unexpected("';' or end of input", error);
            return false;
        }
    }
    return true;
}

bool Parser::parse_one(Statement* out, ParseError* error) {
    std::vector<Statement> all;
    if (!parse(&all, error)) {
        return false;
    }
    if (all.size() != 1) {
        *error = ParseError{"expected exactly one statement, found " + std::to_string(all.size()),
                            SourceLocation{}};
        return false;
    }
    *out = std::move(all.front());
    return true;
}

bool Parser::parse_expression(ExprPtr* out, ParseError* error) {
    if (!prime(error)) {
        return false;
    }
    if (!expression(out, error)) {
        return false;
    }
    if (!check(TokenType::EndOfInput)) {
        unexpected("end of expression", error);
        return false;
    }
    return true;
}

std::optional<Statement> parse_statement(std::string_view sql, ParseError* error) {
    Parser parser(sql);
    Statement out;
    if (!parser.parse_one(&out, error)) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

bool Parser::statement(Statement* out, ParseError* error) {
    if (match(TokenType::Create)) {
        // CREATE is followed by what is being created, so the branch happens
        // here rather than inside create_table.
        return check(TokenType::Index) ? create_index(out, error) : create_table(out, error);
    }
    if (match(TokenType::Drop)) {
        return check(TokenType::Index) ? drop_index(out, error) : drop_table(out, error);
    }
    if (match(TokenType::Insert)) {
        return insert(out, error);
    }
    if (match(TokenType::Select)) {
        return select(out, error);
    }
    if (match(TokenType::Update)) {
        return update(out, error);
    }
    if (match(TokenType::Delete)) {
        return delete_from(out, error);
    }
    if (match(TokenType::Begin)) {
        match(TokenType::Transaction); // optional noise word
        *out = TransactionStatement{TransactionControl::Begin};
        return true;
    }
    if (match(TokenType::Commit)) {
        match(TokenType::Transaction);
        *out = TransactionStatement{TransactionControl::Commit};
        return true;
    }
    if (match(TokenType::Rollback)) {
        match(TokenType::Transaction);
        *out = TransactionStatement{TransactionControl::Rollback};
        return true;
    }
    unexpected("a statement", error);
    return false;
}

bool Parser::create_table(Statement* out, ParseError* error) {
    CreateTable statement;
    if (!expect(TokenType::Table, error)) {
        return false;
    }
    if (check(TokenType::If) && peek(1).type == TokenType::Not &&
        peek(2).type == TokenType::Exists) {
        index_ += 3;
        statement.if_not_exists = true;
    }
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }
    if (!expect(TokenType::LParen, error)) {
        return false;
    }

    do {
        ColumnDef column;
        if (!expect_identifier(&column.name, error)) {
            return false;
        }
        if (match(TokenType::KwInteger)) {
            column.type = Type::Integer;
        } else if (match(TokenType::KwReal)) {
            column.type = Type::Real;
        } else if (match(TokenType::KwText)) {
            column.type = Type::Text;
        } else {
            unexpected("a column type (INTEGER, REAL or TEXT)", error);
            return false;
        }
        for (;;) {
            if (match(TokenType::Primary)) {
                if (!expect(TokenType::Key, error)) {
                    return false;
                }
                column.primary_key = true;
                continue;
            }
            if (check(TokenType::Not) && peek(1).type == TokenType::Null) {
                index_ += 2;
                column.not_null = true;
                continue;
            }
            break;
        }
        statement.columns.push_back(std::move(column));
    } while (match(TokenType::Comma));

    if (!expect(TokenType::RParen, error)) {
        return false;
    }
    if (statement.columns.empty()) {
        *error = ParseError{"a table needs at least one column", previous().loc};
        return false;
    }
    *out = std::move(statement);
    return true;
}

bool Parser::drop_table(Statement* out, ParseError* error) {
    DropTable statement;
    if (!expect(TokenType::Table, error)) {
        return false;
    }
    if (check(TokenType::If) && peek(1).type == TokenType::Exists) {
        index_ += 2;
        statement.if_exists = true;
    }
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }
    *out = std::move(statement);
    return true;
}

bool Parser::create_index(Statement* out, ParseError* error) {
    CreateIndex statement;
    if (!expect(TokenType::Index, error)) {
        return false;
    }
    if (check(TokenType::If) && peek(1).type == TokenType::Not &&
        peek(2).type == TokenType::Exists) {
        index_ += 3;
        statement.if_not_exists = true;
    }
    if (!expect_identifier(&statement.name, error)) {
        return false;
    }
    if (!expect(TokenType::On, error)) {
        return false;
    }
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }
    if (!expect(TokenType::LParen, error)) {
        return false;
    }
    if (!expect_identifier(&statement.column, error)) {
        return false;
    }
    // Composite indexes need a tuple encoding and a planner that understands
    // prefix matching. Reject the syntax rather than silently indexing only
    // the first column.
    if (check(TokenType::Comma)) {
        *error = ParseError{"composite indexes are not supported; name one column", peek().loc};
        return false;
    }
    if (!expect(TokenType::RParen, error)) {
        return false;
    }
    *out = std::move(statement);
    return true;
}

bool Parser::drop_index(Statement* out, ParseError* error) {
    DropIndex statement;
    if (!expect(TokenType::Index, error)) {
        return false;
    }
    if (check(TokenType::If) && peek(1).type == TokenType::Exists) {
        index_ += 2;
        statement.if_exists = true;
    }
    if (!expect_identifier(&statement.name, error)) {
        return false;
    }
    *out = std::move(statement);
    return true;
}

bool Parser::insert(Statement* out, ParseError* error) {
    Insert statement;
    if (!expect(TokenType::Into, error)) {
        return false;
    }
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }

    if (match(TokenType::LParen)) {
        do {
            std::string name;
            if (!expect_identifier(&name, error)) {
                return false;
            }
            statement.columns.push_back(std::move(name));
        } while (match(TokenType::Comma));
        if (!expect(TokenType::RParen, error)) {
            return false;
        }
    }

    if (!expect(TokenType::Values, error)) {
        return false;
    }

    do {
        if (!expect(TokenType::LParen, error)) {
            return false;
        }
        std::vector<ExprPtr> row;
        do {
            ExprPtr value;
            if (!expression(&value, error)) {
                return false;
            }
            row.push_back(std::move(value));
        } while (match(TokenType::Comma));
        if (!expect(TokenType::RParen, error)) {
            return false;
        }

        // Catching this here rather than at execution means the error points
        // at the offending row instead of at the statement as a whole.
        if (!statement.columns.empty() && row.size() != statement.columns.size()) {
            *error =
                ParseError{"this row has " + std::to_string(row.size()) + " values but " +
                               std::to_string(statement.columns.size()) + " columns were named",
                           previous().loc};
            return false;
        }
        if (!statement.rows.empty() && row.size() != statement.rows.front().size()) {
            *error = ParseError{"this row has " + std::to_string(row.size()) +
                                    " values but the first row has " +
                                    std::to_string(statement.rows.front().size()),
                                previous().loc};
            return false;
        }
        statement.rows.push_back(std::move(row));
    } while (match(TokenType::Comma));

    *out = std::move(statement);
    return true;
}

bool Parser::select(Statement* out, ParseError* error) {
    Select statement;

    if (match(TokenType::Star)) {
        statement.star = true;
    } else {
        do {
            SelectItem item;
            if (!expression(&item.expr, error)) {
                return false;
            }
            if (check(TokenType::Identifier)) { // `expr alias`, no AS keyword
                item.alias = peek().text;
                ++index_;
            }
            statement.items.push_back(std::move(item));
        } while (match(TokenType::Comma));
    }

    if (match(TokenType::From)) {
        if (!expect_identifier(&statement.table, error)) {
            return false;
        }
    }

    if (match(TokenType::Where)) {
        if (!expression(&statement.where, error)) {
            return false;
        }
    }

    if (match(TokenType::Order)) {
        if (!expect(TokenType::By, error)) {
            return false;
        }
        do {
            OrderTerm term;
            if (!expression(&term.expr, error)) {
                return false;
            }
            if (match(TokenType::Desc)) {
                term.descending = true;
            } else {
                match(TokenType::Asc);
            }
            statement.order_by.push_back(std::move(term));
        } while (match(TokenType::Comma));
    }

    if (match(TokenType::Limit)) {
        if (!check(TokenType::IntegerLiteral)) {
            unexpected("an integer after LIMIT", error);
            return false;
        }
        statement.limit = peek().integer;
        ++index_;
    }

    *out = std::move(statement);
    return true;
}

bool Parser::update(Statement* out, ParseError* error) {
    Update statement;
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }
    if (!expect(TokenType::Set, error)) {
        return false;
    }

    do {
        Assignment assignment;
        if (!expect_identifier(&assignment.column, error)) {
            return false;
        }
        if (!expect(TokenType::Equal, error)) {
            return false;
        }
        if (!expression(&assignment.value, error)) {
            return false;
        }
        statement.assignments.push_back(std::move(assignment));
    } while (match(TokenType::Comma));

    if (match(TokenType::Where)) {
        if (!expression(&statement.where, error)) {
            return false;
        }
    }
    *out = std::move(statement);
    return true;
}

bool Parser::delete_from(Statement* out, ParseError* error) {
    Delete statement;
    if (!expect(TokenType::From, error)) {
        return false;
    }
    if (!expect_identifier(&statement.table, error)) {
        return false;
    }
    if (match(TokenType::Where)) {
        if (!expression(&statement.where, error)) {
            return false;
        }
    }
    *out = std::move(statement);
    return true;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

bool Parser::binary(int min_precedence, ExprPtr* out, ParseError* error) {
    ExprPtr left;

    // NOT is a prefix operator that sits in the middle of the ladder, so it is
    // handled here rather than in unary(): its operand must swallow every
    // comparison and arithmetic operator, and stop at AND.
    if (min_precedence <= kNotPrecedence && check(TokenType::Not)) {
        const SourceLocation loc = peek().loc;
        ++index_;
        ExprPtr operand;
        if (!binary(kNotPrecedence, &operand, error)) {
            return false;
        }
        left = make_unary(UnaryOperator::Not, std::move(operand), loc);
    } else if (!unary(&left, error)) {
        return false;
    }

    for (;;) {
        const int precedence = binary_precedence(peek().type);
        if (precedence == 0 || precedence < min_precedence) {
            break;
        }
        const Token op_token = peek();
        ++index_;

        ExprPtr right;
        // Left-associative: the right operand binds only operators strictly
        // tighter than this one, so `a - b - c` groups as `(a - b) - c`.
        if (!binary(precedence + 1, &right, error)) {
            return false;
        }
        left = make_binary(binary_operator_for(op_token.type), std::move(left), std::move(right),
                           op_token.loc);
    }

    *out = std::move(left);
    return true;
}

bool Parser::unary(ExprPtr* out, ParseError* error) {
    if (check(TokenType::Minus)) {
        const SourceLocation loc = peek().loc;
        ++index_;
        ExprPtr operand;
        if (!unary(&operand, error)) {
            return false;
        }
        *out = make_unary(UnaryOperator::Negate, std::move(operand), loc);
        return true;
    }
    return postfix(out, error);
}

bool Parser::postfix(ExprPtr* out, ParseError* error) {
    ExprPtr expr;
    if (!primary(&expr, error)) {
        return false;
    }

    // `x IS NULL` and `x IS NOT NULL` bind tighter than any binary operator.
    while (check(TokenType::Is)) {
        const SourceLocation loc = peek().loc;
        ++index_;
        const bool negated = match(TokenType::Not);
        if (!expect(TokenType::Null, error)) {
            return false;
        }
        expr = make_unary(negated ? UnaryOperator::IsNotNull : UnaryOperator::IsNull,
                          std::move(expr), loc);
    }

    *out = std::move(expr);
    return true;
}

bool Parser::primary(ExprPtr* out, ParseError* error) {
    const Token& token = peek();

    switch (token.type) {
    case TokenType::IntegerLiteral:
        ++index_;
        *out = make_literal(Value(token.integer), token.loc);
        return true;
    case TokenType::RealLiteral:
        ++index_;
        *out = make_literal(Value(token.real), token.loc);
        return true;
    case TokenType::StringLiteral:
        ++index_;
        *out = make_literal(Value(token.text), token.loc);
        return true;
    case TokenType::Null:
        ++index_;
        *out = make_literal(Value::null(), token.loc);
        return true;
    case TokenType::Identifier:
        ++index_;
        *out = make_column(token.text, token.loc);
        return true;
    case TokenType::LParen: {
        ++index_;
        ExprPtr inner;
        if (!expression(&inner, error)) {
            return false;
        }
        if (!expect(TokenType::RParen, error)) {
            return false;
        }
        *out = std::move(inner);
        return true;
    }
    default:
        unexpected("a value, a column name or '('", error);
        return false;
    }
}

} // namespace strata::sql
