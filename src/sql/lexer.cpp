#include "strata/sql/lexer.hpp"

#include <cctype>
#include <charconv>
#include <unordered_map>

namespace strata::sql {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

const std::unordered_map<std::string_view, TokenType>& keyword_table() {
    static const std::unordered_map<std::string_view, TokenType> table = {
        {"select", TokenType::Select},
        {"from", TokenType::From},
        {"where", TokenType::Where},
        {"insert", TokenType::Insert},
        {"into", TokenType::Into},
        {"values", TokenType::Values},
        {"update", TokenType::Update},
        {"set", TokenType::Set},
        {"delete", TokenType::Delete},
        {"create", TokenType::Create},
        {"table", TokenType::Table},
        {"drop", TokenType::Drop},
        {"order", TokenType::Order},
        {"by", TokenType::By},
        {"asc", TokenType::Asc},
        {"desc", TokenType::Desc},
        {"limit", TokenType::Limit},
        {"and", TokenType::And},
        {"or", TokenType::Or},
        {"not", TokenType::Not},
        {"null", TokenType::Null},
        {"is", TokenType::Is},
        {"primary", TokenType::Primary},
        {"key", TokenType::Key},
        {"if", TokenType::If},
        {"exists", TokenType::Exists},
        {"integer", TokenType::KwInteger},
        {"int", TokenType::KwInteger},
        {"real", TokenType::KwReal},
        {"float", TokenType::KwReal},
        {"double", TokenType::KwReal},
        {"text", TokenType::KwText},
        {"varchar", TokenType::KwText},
        {"string", TokenType::KwText},
        {"begin", TokenType::Begin},
        {"commit", TokenType::Commit},
        {"rollback", TokenType::Rollback},
        {"transaction", TokenType::Transaction},
        {"index", TokenType::Index},
        {"on", TokenType::On},
    };
    return table;
}

} // namespace

const char* token_type_name(TokenType type) {
    switch (type) {
    case TokenType::EndOfInput:
        return "end of input";
    case TokenType::Identifier:
        return "identifier";
    case TokenType::IntegerLiteral:
        return "integer";
    case TokenType::RealLiteral:
        return "real number";
    case TokenType::StringLiteral:
        return "string";
    case TokenType::Select:
        return "SELECT";
    case TokenType::From:
        return "FROM";
    case TokenType::Where:
        return "WHERE";
    case TokenType::Insert:
        return "INSERT";
    case TokenType::Into:
        return "INTO";
    case TokenType::Values:
        return "VALUES";
    case TokenType::Update:
        return "UPDATE";
    case TokenType::Set:
        return "SET";
    case TokenType::Delete:
        return "DELETE";
    case TokenType::Create:
        return "CREATE";
    case TokenType::Table:
        return "TABLE";
    case TokenType::Drop:
        return "DROP";
    case TokenType::Order:
        return "ORDER";
    case TokenType::By:
        return "BY";
    case TokenType::Asc:
        return "ASC";
    case TokenType::Desc:
        return "DESC";
    case TokenType::Limit:
        return "LIMIT";
    case TokenType::And:
        return "AND";
    case TokenType::Or:
        return "OR";
    case TokenType::Not:
        return "NOT";
    case TokenType::Null:
        return "NULL";
    case TokenType::Is:
        return "IS";
    case TokenType::Primary:
        return "PRIMARY";
    case TokenType::Key:
        return "KEY";
    case TokenType::If:
        return "IF";
    case TokenType::Exists:
        return "EXISTS";
    case TokenType::KwInteger:
        return "INTEGER";
    case TokenType::KwReal:
        return "REAL";
    case TokenType::KwText:
        return "TEXT";
    case TokenType::Begin:
        return "BEGIN";
    case TokenType::Commit:
        return "COMMIT";
    case TokenType::Rollback:
        return "ROLLBACK";
    case TokenType::Transaction:
        return "TRANSACTION";
    case TokenType::Index:
        return "INDEX";
    case TokenType::On:
        return "ON";
    case TokenType::LParen:
        return "'('";
    case TokenType::RParen:
        return "')'";
    case TokenType::Comma:
        return "','";
    case TokenType::Semicolon:
        return "';'";
    case TokenType::Star:
        return "'*'";
    case TokenType::Equal:
        return "'='";
    case TokenType::NotEqual:
        return "'<>'";
    case TokenType::Less:
        return "'<'";
    case TokenType::LessEqual:
        return "'<='";
    case TokenType::Greater:
        return "'>'";
    case TokenType::GreaterEqual:
        return "'>='";
    case TokenType::Plus:
        return "'+'";
    case TokenType::Minus:
        return "'-'";
    case TokenType::Slash:
        return "'/'";
    }
    return "?";
}

TokenType keyword_or_identifier(std::string_view lowered) {
    const auto& table = keyword_table();
    const auto it = table.find(lowered);
    return it == table.end() ? TokenType::Identifier : it->second;
}

char Lexer::peek(std::size_t ahead) const {
    const std::size_t at = pos_ + ahead;
    return at < input_.size() ? input_[at] : '\0';
}

char Lexer::advance() {
    const char c = input_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

void Lexer::skip_space_and_comments() {
    for (;;) {
        while (!at_end() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
        // -- to end of line
        if (peek() == '-' && peek(1) == '-') {
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }
        // /* ... */
        if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            while (!at_end() && !(peek() == '*' && peek(1) == '/')) {
                advance();
            }
            if (!at_end()) {
                advance();
                advance();
            }
            continue;
        }
        return;
    }
}

bool Lexer::read_number(Token* token, ParseError* error) {
    const std::size_t start = pos_;
    token->loc = here();

    while (is_digit(peek())) {
        advance();
    }

    bool is_real = false;
    if (peek() == '.' && is_digit(peek(1))) {
        is_real = true;
        advance();
        while (is_digit(peek())) {
            advance();
        }
    }
    if (peek() == 'e' || peek() == 'E') {
        const char sign = peek(1);
        const bool signed_exponent = sign == '+' || sign == '-';
        if (is_digit(signed_exponent ? peek(2) : sign)) {
            is_real = true;
            advance();
            if (signed_exponent) {
                advance();
            }
            while (is_digit(peek())) {
                advance();
            }
        }
    }

    // A number running straight into a letter is a typo, not two tokens.
    if (is_ident_start(peek())) {
        *error = ParseError{"unexpected character in numeric literal", here()};
        return false;
    }

    const std::string_view text = input_.substr(start, pos_ - start);
    token->text = std::string(text);

    if (is_real) {
        token->type = TokenType::RealLiteral;
        token->real = std::strtod(token->text.c_str(), nullptr);
        return true;
    }

    token->type = TokenType::IntegerLiteral;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), token->integer);
    if (ec != std::errc{}) {
        *error = ParseError{"integer literal does not fit in 64 bits", token->loc};
        return false;
    }
    return true;
}

bool Lexer::read_string(Token* token, ParseError* error) {
    token->loc = here();
    const char quote = advance(); // consume the opening quote

    std::string body;
    for (;;) {
        if (at_end()) {
            *error = ParseError{"unterminated string literal", token->loc};
            return false;
        }
        const char c = advance();
        if (c == quote) {
            // '' inside a string is an escaped quote, as in standard SQL.
            if (peek() == quote) {
                body.push_back(advance());
                continue;
            }
            break;
        }
        body.push_back(c);
    }

    token->type = TokenType::StringLiteral;
    token->text = std::move(body);
    return true;
}

void Lexer::read_word(Token* token) {
    token->loc = here();
    const std::size_t start = pos_;
    while (is_ident_char(peek())) {
        advance();
    }
    std::string word(input_.substr(start, pos_ - start));
    for (char& c : word) {
        c = lower(c);
    }
    token->type = keyword_or_identifier(word);
    token->text = std::move(word);
}

bool Lexer::tokenise(std::vector<Token>* out, ParseError* error) {
    out->clear();

    // A UTF-8 byte-order mark. Editors and shells on Windows prepend one
    // routinely, and reporting it as a stray character makes the first
    // statement of every piped script fail for a reason nobody can see.
    if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xEF &&
        static_cast<unsigned char>(input_[1]) == 0xBB &&
        static_cast<unsigned char>(input_[2]) == 0xBF) {
        pos_ = 3;
    }

    for (;;) {
        skip_space_and_comments();
        if (at_end()) {
            Token end;
            end.type = TokenType::EndOfInput;
            end.loc = here();
            out->push_back(std::move(end));
            return true;
        }

        const char c = peek();
        Token token;

        if (is_digit(c)) {
            if (!read_number(&token, error)) {
                return false;
            }
            out->push_back(std::move(token));
            continue;
        }
        if (c == '\'' || c == '"') {
            if (!read_string(&token, error)) {
                return false;
            }
            out->push_back(std::move(token));
            continue;
        }
        if (is_ident_start(c)) {
            read_word(&token);
            out->push_back(std::move(token));
            continue;
        }

        token.loc = here();
        switch (c) {
        case '(':
            advance();
            token.type = TokenType::LParen;
            break;
        case ')':
            advance();
            token.type = TokenType::RParen;
            break;
        case ',':
            advance();
            token.type = TokenType::Comma;
            break;
        case ';':
            advance();
            token.type = TokenType::Semicolon;
            break;
        case '*':
            advance();
            token.type = TokenType::Star;
            break;
        case '+':
            advance();
            token.type = TokenType::Plus;
            break;
        case '-':
            advance();
            token.type = TokenType::Minus;
            break;
        case '/':
            advance();
            token.type = TokenType::Slash;
            break;
        case '=':
            advance();
            token.type = TokenType::Equal;
            break;
        case '<':
            advance();
            if (peek() == '=') {
                advance();
                token.type = TokenType::LessEqual;
            } else if (peek() == '>') {
                advance();
                token.type = TokenType::NotEqual;
            } else {
                token.type = TokenType::Less;
            }
            break;
        case '>':
            advance();
            if (peek() == '=') {
                advance();
                token.type = TokenType::GreaterEqual;
            } else {
                token.type = TokenType::Greater;
            }
            break;
        case '!':
            advance();
            if (peek() == '=') {
                advance();
                token.type = TokenType::NotEqual;
            } else {
                *error = ParseError{"expected '=' after '!'", token.loc};
                return false;
            }
            break;
        default:
            *error = ParseError{std::string("unexpected character '") + c + "'", token.loc};
            return false;
        }
        out->push_back(std::move(token));
    }
}

} // namespace strata::sql
