#pragma once

#include "strata/sql/value.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace strata::sql {

/// Encoding a SQL value so that **byte order equals SQL order**.
///
/// This is the whole correctness burden of an index. If the encoding's
/// lexicographic order ever disagrees with `compare_values`, equality lookups
/// keep working while range scans quietly return the wrong rows — which is the
/// worst shape a bug can take, because nothing looks broken.
///
/// A leading tag byte reproduces the type ordering that `compare_values`
/// defines: nulls first, then numbers, then text.
///
///     0x00  null      (no payload)
///     0x01  numeric   (8 bytes, order-preserving double)
///     0x02  text      (escaped bytes, terminated)
///
/// Integers and reals share one tag so that `2` and `2.0` land in the same
/// place, which is what `compare_values` says about them.
///
/// **The index is not the authority.** An index scan narrows candidates; the
/// original predicate is still applied to every row it produces. That is what
/// makes the double encoding safe despite losing precision above 2^53: a
/// collision costs a wasted row fetch, never a wrong answer. See decision 021.
inline constexpr unsigned char kIndexTagNull = 0x00;
inline constexpr unsigned char kIndexTagNumber = 0x01;
inline constexpr unsigned char kIndexTagText = 0x02;

/// Maps a double onto a uint64 whose unsigned order matches the double's
/// numeric order. IEEE-754 is nearly sorted already: positives compare
/// correctly as integers once the sign bit is set, and negatives compare in
/// reverse, so they need every bit inverted.
inline std::uint64_t order_preserving_bits(double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    if ((bits & 0x8000000000000000ull) != 0) {
        return ~bits; // negative: reverse the run
    }
    return bits | 0x8000000000000000ull; // positive: lift above every negative
}

inline void append_be64(std::string* out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

inline std::uint64_t read_be64_at(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

/// Encodes one value. Text is escaped and terminated for the same reason the
/// MVCC key encoding does it: a fixed-width suffix follows, and without a
/// terminator `"a"` would sort after `"ab"`.
inline std::string encode_index_value(const Value& value) {
    std::string out;
    switch (value.type()) {
    case Type::Null:
        out.push_back(static_cast<char>(kIndexTagNull));
        return out;

    case Type::Integer:
    case Type::Real:
        out.push_back(static_cast<char>(kIndexTagNumber));
        append_be64(&out, order_preserving_bits(value.as_double()));
        return out;

    case Type::Text: {
        out.push_back(static_cast<char>(kIndexTagText));
        for (const char c : value.text()) {
            out.push_back(c);
            if (c == '\0') {
                out.push_back(static_cast<char>(0xFF));
            }
        }
        out.push_back('\0');
        out.push_back('\0');
        return out;
    }
    }
    return out;
}

} // namespace strata::sql
