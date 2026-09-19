// Runs sqllogictest files and prints the report.
//
//   strata_slt <file.test> [more.test ...]
//
// Exits non-zero if anything failed, so it works in CI. Unsupported blocks are
// printed but never counted as passes — see decision 017 for why the
// denominator matters more than the ratio.

#include "strata/sql/slt.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: strata_slt <file.test> [more.test ...]\n";
        return 2;
    }

    SltReport total;
    bool any_failure = false;

    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        const std::string db_path =
            (std::filesystem::temp_directory_path() /
             ("strata_slt_" + std::filesystem::path(path).stem().string() + ".db"))
                .string();
        std::filesystem::remove(db_path);
        std::filesystem::remove(db_path + "-wal");

        SltReport report;
        if (Status s = run_slt_file(path, db_path, &report); !s) {
            std::cerr << path << ": " << s.to_string() << "\n";
            return 1;
        }

        std::cout << std::filesystem::path(path).filename().string() << "\n  " << report.summary()
                  << "\n";
        for (const std::string& reason : report.unsupported_reasons) {
            std::cout << "    unsupported: " << reason << "\n";
        }
        for (const std::string& failure : report.failures) {
            std::cout << "    FAIL " << failure << "\n";
            any_failure = true;
        }

        total.statements_run += report.statements_run;
        total.statements_passed += report.statements_passed;
        total.queries_run += report.queries_run;
        total.queries_passed += report.queries_passed;
        total.unsupported += report.unsupported;
    }

    std::cout << "\ntotal\n  " << total.summary() << "\n";
    return any_failure ? 1 : 0;
}
