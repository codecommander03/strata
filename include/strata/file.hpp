#pragma once

#include "strata/types.hpp"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace strata {

/// A thin wrapper over a buffered stdio file handle.
///
/// The only reason this exists rather than std::fstream is `sync()`. Crash
/// safety turns entirely on being able to say "these bytes are on the platter
/// before I write the next ones", and the C++ standard library has no portable
/// way to express that. See decision 003.
class File {
public:
    File() = default;
    ~File() { close(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept : fp_(other.fp_), path_(std::move(other.path_)) {
        other.fp_ = nullptr;
    }

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            fp_ = other.fp_;
            path_ = std::move(other.path_);
            other.fp_ = nullptr;
        }
        return *this;
    }

    /// Opens for read and write, creating the file if it does not exist.
    Status open(const std::string& path) {
        close();
        path_ = path;
        fp_ = std::fopen(path.c_str(), "r+b");
        if (fp_ == nullptr) {
            fp_ = std::fopen(path.c_str(), "w+b");
        }
        if (fp_ == nullptr) {
            return Status::io_error("cannot open " + path);
        }
        return Status::ok();
    }

    bool is_open() const { return fp_ != nullptr; }

    void close() {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    Status read_at(std::uint64_t offset, std::byte* out, std::size_t len) {
        if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) {
            return Status::io_error("seek failed");
        }
        const std::size_t got = std::fread(out, 1, len, fp_);
        if (got != len) {
            return Status::io_error("short read");
        }
        return Status::ok();
    }

    Status write_at(std::uint64_t offset, const std::byte* in, std::size_t len) {
        if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) {
            return Status::io_error("seek failed");
        }
        const std::size_t put = std::fwrite(in, 1, len, fp_);
        if (put != len) {
            return Status::io_error("short write");
        }
        return Status::ok();
    }

    /// Flushes the stdio buffer and then asks the OS to put the bytes on stable
    /// storage. Both halves are needed: fflush only moves bytes from our buffer
    /// into the kernel's.
    Status sync() {
        if (std::fflush(fp_) != 0) {
            return Status::io_error("fflush failed");
        }
#ifdef _WIN32
        if (_commit(_fileno(fp_)) != 0) {
            return Status::io_error("_commit failed");
        }
#else
        if (::fsync(fileno(fp_)) != 0) {
            return Status::io_error("fsync failed");
        }
#endif
        return Status::ok();
    }

    std::uint64_t size() {
        const long here = std::ftell(fp_);
        std::fseek(fp_, 0, SEEK_END);
        const long end = std::ftell(fp_);
        std::fseek(fp_, here, SEEK_SET);
        return end < 0 ? 0 : static_cast<std::uint64_t>(end);
    }

    Status truncate_to_empty() {
        close();
        fp_ = std::fopen(path_.c_str(), "w+b");
        if (fp_ == nullptr) {
            return Status::io_error("cannot truncate " + path_);
        }
        return Status::ok();
    }

    const std::string& path() const { return path_; }

private:
    std::FILE* fp_ = nullptr;
    std::string path_;
};

} // namespace strata
