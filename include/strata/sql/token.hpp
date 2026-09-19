#pragma once

#include <cstdint>
#include <string>

namespace strata::sql {

enum class TokenType : std::uint8_t {
    EndOfInput,

    // literals and names
    Identifier,
    IntegerLiteral,
    RealLiteral,
    StringLiteral,

    // keywords
    Select,
    From,
    Where,
    Insert,
    Into,
    Values,
    Update,
    Set,
    Delete,
    Create,
    Table,
    Drop,
    Order,
    By,
    Asc,
    Desc,
    Limit,
    And,
    Or,
    Not,
    Null,
    Is,
    Primary,
    Key,
    If,
    Exists,
    KwInteger,
    KwReal,
    KwText,
    Begin,
    Commit,
    Rollback,
    Transaction,

    // punctuation and operators
    LParen,
    RParen,
    Comma,
    Semicolon,
    Star,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    Minus,
    Slash,
};

const char* token_type_name(TokenType type);

/// A position in the input, one-based, so that an error message points at what
/// a person would call line 1 column 1.
struct SourceLocation {
    int line = 1;
    int column = 1;
};

struct Token {
    TokenType type = TokenType::EndOfInput;
    std::string text; ///< the source spelling, or the decoded string body
    std::int64_t integer = 0;
    double real = 0.0;
    SourceLocation loc;
};

} // namespace strata::sql
