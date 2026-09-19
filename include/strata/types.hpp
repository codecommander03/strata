#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace strata {

/// Pages are addressed by index into the database file. Page 0 is always the
/// meta page, so 0 doubles as "no page" everywhere else.
using PageId = std::uint32_t;

/// Monotonic sequence number for WAL frames. Every page carries the lsn of the
/// last write that touched it, which is what makes recovery idempotent.
using Lsn = std::uint64_t;

inline constexpr PageId kNoPage = 0;
inline constexpr PageId kMetaPage = 0;

/// 4 KiB. Matches the usual filesystem block and SQLite's default, so a page
/// write is a single block write on every platform that matters.
inline constexpr std::uint32_t kPageSize = 4096;

/// Bytes on disk are little-endian regardless of host. Every field goes through
/// the encode/decode helpers in page.hpp rather than being memcpy'd as a struct,
/// so a file written on one machine is readable on another.
inline constexpr std::string_view kMagic = "STRATA01";

enum class Code : std::uint8_t {
    Ok = 0,
    NotFound,
    Corruption,
    IoError,
    InvalidArgument,
    Full,
    /// Two transactions wrote the same key and this one lost the race. Under
    /// snapshot isolation the first committer wins; the loser must retry.
    Conflict,
};

/// Errors are returned, never thrown. The engine runs inside a WASM build in
/// stage 5 where exceptions cost binary size for no benefit, and a storage
/// engine's failures are expected control flow rather than exceptional.
class Status {
public:
    Status() = default;

    static Status ok() { return Status(); }
    static Status not_found(std::string what) { return Status(Code::NotFound, std::move(what)); }
    static Status corruption(std::string what) { return Status(Code::Corruption, std::move(what)); }
    static Status io_error(std::string what) { return Status(Code::IoError, std::move(what)); }
    static Status invalid_argument(std::string what) {
        return Status(Code::InvalidArgument, std::move(what));
    }
    static Status full(std::string what) { return Status(Code::Full, std::move(what)); }
    static Status conflict(std::string what) { return Status(Code::Conflict, std::move(what)); }

    bool ok_status() const { return code_ == Code::Ok; }
    explicit operator bool() const { return ok_status(); }

    Code code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string to_string() const {
        if (ok_status()) {
            return "Ok";
        }
        return std::string(name_of(code_)) + ": " + message_;
    }

private:
    Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

    static std::string_view name_of(Code code) {
        switch (code) {
        case Code::Ok:
            return "Ok";
        case Code::NotFound:
            return "NotFound";
        case Code::Corruption:
            return "Corruption";
        case Code::IoError:
            return "IoError";
        case Code::InvalidArgument:
            return "InvalidArgument";
        case Code::Full:
            return "Full";
        case Code::Conflict:
            return "Conflict";
        }
        return "Unknown";
    }

    Code code_ = Code::Ok;
    std::string message_;
};

using Bytes = std::span<const std::byte>;
using MutableBytes = std::span<std::byte>;

/// Views a string's bytes without copying. Keys and values are opaque byte
/// strings to the storage engine; meaning is imposed by the layer above.
inline Bytes as_bytes(std::string_view s) {
    return Bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

inline std::string_view as_string_view(Bytes b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

} // namespace strata
