#pragma once

#include "strata/types.hpp"

#include <cstring>
#include <string>

namespace strata {

using Xid = std::uint64_t;

inline constexpr Xid kInvalidXid = 0;
inline constexpr Xid kFirstXid = 1;

/// Flags in a version's header.
inline constexpr std::uint8_t kVersionDeleted = 0x01;

/// How a user key and a version number become one B+tree key.
///
/// All versions of a key must sort adjacently, and within a key the newest
/// version must come first so that a forward cursor finds it without scanning
/// the whole chain. Appending the version straight onto the key does not work:
/// "a" + version would sort after "ab" + version, because the version's first
/// byte is compared against 'b'.
///
/// So the user key is escaped and terminated. A 0x00 byte becomes 0x00 0xFF,
/// and the key ends with 0x00 0x00. The terminator is lower than any escaped
/// zero and lower than any ordinary byte, which is exactly the property that
/// makes a prefix sort before a longer key. The version follows as eight bytes
/// of (max - version) big-endian, so higher version numbers sort first.
inline std::string encode_key(Bytes user_key, Xid version) {
    std::string out;
    out.reserve(user_key.size() + 12);
    for (const std::byte b : user_key) {
        const auto c = std::to_integer<unsigned char>(b);
        out.push_back(static_cast<char>(c));
        if (c == 0x00) {
            out.push_back(static_cast<char>(0xFF));
        }
    }
    out.push_back('\0');
    out.push_back('\0');

    const Xid inverted = ~version; // equivalently UINT64_MAX - version
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<char>((inverted >> (8 * i)) & 0xFF));
    }
    return out;
}

/// Splits an encoded key back into its user key and version. Returns false if
/// the bytes are not a well-formed encoded key.
inline bool decode_key(Bytes encoded, std::string* user_key, Xid* version) {
    const auto* p = reinterpret_cast<const unsigned char*>(encoded.data());
    const std::size_t n = encoded.size();

    std::string key;
    std::size_t i = 0;
    bool terminated = false;
    while (i + 1 < n) {
        if (p[i] == 0x00) {
            if (p[i + 1] == 0x00) {
                i += 2;
                terminated = true;
                break;
            }
            if (p[i + 1] == 0xFF) {
                key.push_back('\0');
                i += 2;
                continue;
            }
            return false;
        }
        key.push_back(static_cast<char>(p[i]));
        ++i;
    }
    if (!terminated || n - i != 8) {
        return false;
    }

    Xid inverted = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        inverted = (inverted << 8) | p[i + k];
    }
    *user_key = std::move(key);
    *version = ~inverted;
    return true;
}

/// The prefix every version of a user key shares — seek here and the newest
/// version is the first thing the cursor lands on.
inline std::string key_prefix(Bytes user_key) {
    std::string out = encode_key(user_key, 0);
    out.resize(out.size() - 8);
    return out;
}

/// The escaped form of a user-key *prefix*, with no terminator and no version.
///
/// Seeking here lands on the first encoded key whose user key starts with this
/// prefix, which is what makes a range scan over a key namespace possible. It
/// is deliberately not `key_prefix`: that one terminates the key, so it would
/// only ever match one exact user key.
inline std::string encode_prefix(Bytes user_key) {
    std::string out;
    out.reserve(user_key.size() + 4);
    for (const std::byte b : user_key) {
        const auto c = std::to_integer<unsigned char>(b);
        out.push_back(static_cast<char>(c));
        if (c == 0x00) {
            out.push_back(static_cast<char>(0xFF));
        }
    }
    return out;
}

/// A stored version: one flag byte, then the payload.
///
/// There is no xmax field. Postgres puts xmin and xmax on one mutable tuple, so
/// a delete rewrites the row it deletes. Here the chain is append-only and a
/// delete appends a tombstone, so "was this version deleted" is answered by
/// finding a newer tombstone rather than by a field on the old version. xmin
/// does not need storing either — it is already in the key. See decision 011.
inline std::string encode_version(bool deleted, Bytes payload) {
    std::string out;
    out.reserve(payload.size() + 1);
    out.push_back(static_cast<char>(deleted ? kVersionDeleted : 0));
    out.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    return out;
}

inline bool decode_version(Bytes stored, bool* deleted, std::string* payload) {
    if (stored.empty()) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(stored.data());
    *deleted = (p[0] & kVersionDeleted) != 0;
    payload->assign(reinterpret_cast<const char*>(p + 1), stored.size() - 1);
    return true;
}

} // namespace strata
