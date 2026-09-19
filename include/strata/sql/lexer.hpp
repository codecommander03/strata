#pragma once

#include "strata/sql/token.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace strata::sql {

/// What went wrong, and exactly where. The stage 3 gate is that this points at
/// the actual mistake rather than at the end of the statement.
struct ParseError {
    std::string message;
    SourceLocation loc;

    /// Renders as `line:column: message`, the form every compiler uses.
    std::string to_string() const {
        return std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " + message;
    }
};

/// Turns SQL text into tokens.
///
/// Keywords are matched case-insensitively and identifiers are folded to lower
/// case, because SQL is case-insensitive for both and every later comparison
/// would otherwise need to remember that.
class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    /// Tokenises the whole input. Returns false and fills `error` on the first
    /// problem — an unterminated string, a stray character, a malformed number.
    bool tokenise(std::vector<Token>* out, ParseError* error);

private:
    char peek(std::size_t ahead = 0) const;
    char advance();
    bool at_end() const { return pos_ >= input_.size(); }
    void skip_space_and_comments();

    bool read_number(Token* token, ParseError* error);
    bool read_string(Token* token, ParseError* error);
    void read_word(Token* token);

    SourceLocation here() const { return SourceLocation{line_, column_}; }

    std::string_view input_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

/// Looks up a lower-cased word in the keyword table. Returns Identifier when it
/// is not a keyword.
TokenType keyword_or_identifier(std::string_view lowered);

} // namespace strata::sql
