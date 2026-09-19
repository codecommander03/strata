#pragma once

#include "strata/db.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace strata::sql {

/// Outcome of running one sqllogictest file.
struct SltReport {
    int statements_run = 0;
    int statements_passed = 0;
    int queries_run = 0;
    int queries_passed = 0;

    /// Blocks marked `#unsupported:` in the file. Counted, named, and never
    /// counted as passes — the point of the report is that the denominator is
    /// honest.
    int unsupported = 0;
    std::vector<std::string> unsupported_reasons;

    std::vector<std::string> failures;

    int total_run() const { return statements_run + queries_run; }
    int total_passed() const { return statements_passed + queries_passed; }

    std::string summary() const;
};

/// Runs a sqllogictest script against a fresh database.
///
/// The format is SQLite's: `statement ok`, `statement error`, and
/// `query <types> [sort-mode]` followed by `----` and the expected values, one
/// per line. Comments begin with `#`. Two extensions are used here: a
/// `#unsupported: reason` line marks the following block as a known gap, and
/// `halt` stops the file.
///
/// Type letters are I for integer, R for real, T for text, and they control
/// how a value is rendered before comparison — which is how the official suite
/// avoids arguing about float formatting.
Status run_slt_script(std::string_view script, const std::string& db_path, SltReport* report);

/// Reads a file and runs it.
Status run_slt_file(const std::string& path, const std::string& db_path, SltReport* report);

} // namespace strata::sql
