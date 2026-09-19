#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace strata::sql {

enum class Type : std::uint8_t {
    Null = 0,
    Integer,
    Real,
    Text,
};

inline const char* type_name(Type t) {
    switch (t) {
    case Type::Null:
        return "NULL";
    case Type::Integer:
        return "INTEGER";
    case Type::Real:
        return "REAL";
    case Type::Text:
        return "TEXT";
    }
    return "?";
}

/// A SQL value. Null is a distinct state rather than a sentinel, because SQL's
/// three-valued logic needs to tell "unknown" apart from zero and from "".
class Value {
public:
    Value() = default; // null
    explicit Value(std::int64_t v) : data_(v) {}
    explicit Value(double v) : data_(v) {}
    explicit Value(std::string v) : data_(std::move(v)) {}

    static Value null() { return Value(); }

    Type type() const {
        switch (data_.index()) {
        case 0:
            return Type::Null;
        case 1:
            return Type::Integer;
        case 2:
            return Type::Real;
        default:
            return Type::Text;
        }
    }

    bool is_null() const { return data_.index() == 0; }
    std::int64_t integer() const { return std::get<std::int64_t>(data_); }
    double real() const { return std::get<double>(data_); }
    const std::string& text() const { return std::get<std::string>(data_); }

    /// Numeric value of an INTEGER or REAL, for arithmetic and comparison
    /// across the two. Undefined for other types; callers check first.
    double as_double() const {
        return data_.index() == 1 ? static_cast<double>(std::get<std::int64_t>(data_))
                                  : std::get<double>(data_);
    }

    bool is_numeric() const { return data_.index() == 1 || data_.index() == 2; }

    /// Truthiness for WHERE. Null is not true — which is not the same as being
    /// false, but WHERE keeps only rows that are definitely true.
    bool is_true() const {
        switch (data_.index()) {
        case 1:
            return std::get<std::int64_t>(data_) != 0;
        case 2:
            return std::get<double>(data_) != 0.0;
        default:
            return false;
        }
    }

    std::string to_string() const;

    bool operator==(const Value& other) const { return data_ == other.data_; }

private:
    std::variant<std::monostate, std::int64_t, double, std::string> data_;
};

/// SQL ordering across types, used by comparison operators and ORDER BY.
/// Nulls sort first, then numbers, then text — SQLite's ordering, chosen so
/// sqllogictest's expected output matches.
int compare_values(const Value& a, const Value& b);

} // namespace strata::sql
