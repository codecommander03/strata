#include "strata/sql/slt.hpp"

#include "strata/sql/session.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace strata::sql {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream stream(s);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

/// Splits a rendered result into rows of `width` values so rowsort can sort
/// whole rows rather than shuffling values between them.
std::vector<std::vector<std::string>> group_rows(const std::vector<std::string>& values,
                                                 std::size_t width) {
    std::vector<std::vector<std::string>> rows;
    if (width == 0) {
        return rows;
    }
    for (std::size_t i = 0; i + width <= values.size(); i += width) {
        rows.emplace_back(values.begin() + static_cast<std::ptrdiff_t>(i),
                          values.begin() + static_cast<std::ptrdiff_t>(i + width));
    }
    return rows;
}

void apply_sort_mode(std::vector<std::string>* values, const std::string& mode, std::size_t width) {
    if (mode == "valuesort") {
        std::sort(values->begin(), values->end());
        return;
    }
    if (mode != "rowsort") {
        return; // nosort, or none given
    }
    // rowsort orders whole rows, which is what makes a query with no ORDER BY
    // comparable without pinning the storage engine's iteration order.
    auto rows = group_rows(*values, width);
    std::sort(rows.begin(), rows.end());
    values->clear();
    for (const auto& row : rows) {
        for (const auto& value : row) {
            values->push_back(value);
        }
    }
}

} // namespace

std::string SltReport::summary() const {
    std::ostringstream out;
    out << "statements " << statements_passed << "/" << statements_run << "   queries "
        << queries_passed << "/" << queries_run << "   total " << total_passed() << "/"
        << total_run();
    if (unsupported > 0) {
        out << "   unsupported " << unsupported;
    }
    return out.str();
}

Status run_slt_script(std::string_view script, const std::string& db_path, SltReport* report) {
    Database db;
    if (Status s = db.open(db_path); !s) {
        return s;
    }
    Session session(db);

    std::istringstream input{std::string(script)};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    std::size_t i = 0;
    std::string pending_unsupported;

    auto fail = [&](std::size_t at, const std::string& what) {
        report->failures.push_back("line " + std::to_string(at + 1) + ": " + what);
    };

    while (i < lines.size()) {
        const std::string raw = lines[i];
        const std::string text = trim(raw);

        if (text.empty()) {
            ++i;
            continue;
        }
        if (text[0] == '#') {
            // `#unsupported: reason` marks the next block as a known gap.
            const std::string body = trim(text.substr(1));
            if (body.rfind("unsupported:", 0) == 0) {
                pending_unsupported = trim(body.substr(std::string("unsupported:").size()));
            }
            ++i;
            continue;
        }
        if (text == "halt") {
            break;
        }

        const std::vector<std::string> words = split_words(text);

        // --- statement ok | statement error ---
        if (words[0] == "statement") {
            const bool expect_ok = words.size() < 2 || words[1] == "ok";
            ++i;
            std::string sql;
            while (i < lines.size() && !trim(lines[i]).empty()) {
                sql += lines[i] + "\n";
                ++i;
            }

            if (!pending_unsupported.empty()) {
                ++report->unsupported;
                report->unsupported_reasons.push_back(pending_unsupported);
                pending_unsupported.clear();
                continue;
            }

            ++report->statements_run;
            ResultSet result;
            const Status s = session.run(sql, &result);
            if (expect_ok == static_cast<bool>(s)) {
                ++report->statements_passed;
            } else if (expect_ok) {
                fail(i, "statement failed: " + s.to_string() + "\n    " + trim(sql));
            } else {
                fail(i, "statement should have failed but did not:\n    " + trim(sql));
            }
            continue;
        }

        // --- query <types> [sort-mode] [label] ---
        if (words[0] == "query") {
            const std::string types = words.size() > 1 ? words[1] : std::string();
            const std::string sort_mode = words.size() > 2 ? words[2] : std::string("nosort");
            ++i;

            std::string sql;
            while (i < lines.size() && trim(lines[i]) != "----") {
                sql += lines[i] + "\n";
                ++i;
            }
            if (i < lines.size()) {
                ++i; // consume the ---- separator
            }

            std::vector<std::string> expected;
            while (i < lines.size() && !trim(lines[i]).empty()) {
                expected.push_back(trim(lines[i]));
                ++i;
            }

            if (!pending_unsupported.empty()) {
                ++report->unsupported;
                report->unsupported_reasons.push_back(pending_unsupported);
                pending_unsupported.clear();
                continue;
            }

            ++report->queries_run;
            ResultSet result;
            if (Status s = session.run(sql, &result); !s) {
                fail(i, "query failed: " + s.to_string() + "\n    " + trim(sql));
                continue;
            }

            std::vector<std::string> actual = format_for_comparison(result, types);
            const std::size_t width = types.empty() ? 1 : types.size();
            apply_sort_mode(&actual, sort_mode, width);

            std::vector<std::string> want = expected;
            apply_sort_mode(&want, sort_mode, width);

            if (actual == want) {
                ++report->queries_passed;
                continue;
            }

            std::ostringstream detail;
            detail << "query gave the wrong answer:\n    " << trim(sql) << "\n    expected: ";
            for (const auto& v : want) {
                detail << "[" << v << "]";
            }
            detail << "\n    actual:   ";
            for (const auto& v : actual) {
                detail << "[" << v << "]";
            }
            fail(i, detail.str());
            continue;
        }

        fail(i, "unrecognised directive: " + text);
        ++i;
    }

    return Status::ok();
}

Status run_slt_file(const std::string& path, const std::string& db_path, SltReport* report) {
    std::ifstream file(path);
    if (!file) {
        return Status::io_error("cannot open " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return run_slt_script(buffer.str(), db_path, report);
}

} // namespace strata::sql
