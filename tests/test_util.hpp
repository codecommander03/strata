#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace strata::test {

/// A database path in a unique temporary directory, removed on destruction
/// along with its log. Tests that want to reopen a database do so through the
/// same TempDb so the path survives across Pager instances.
class TempDb {
public:
    explicit TempDb(const std::string& name) {
        static int counter = 0;
        dir_ = std::filesystem::temp_directory_path() /
               ("strata_test_" + name + "_" + std::to_string(++counter));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "test.db").string();
    }

    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;

    const std::string& path() const { return path_; }
    std::string wal_path() const { return path_ + "-wal"; }

private:
    std::filesystem::path dir_;
    std::string path_;
};

} // namespace strata::test
