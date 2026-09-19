// An interactive SQL shell over the engine.
//
//   strata_shell [path]        opens (or creates) a database; default :temp:
//
// Dot commands:
//   .tables        list tables
//   .schema NAME   show one table's columns
//   .plan ON|OFF   print the query plan above each result
//   .quit
//
// Statements are terminated by a semicolon and may span lines.

#include "strata/sql/catalog.hpp"
#include "strata/sql/session.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;

namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

void print_table(const ResultSet& result) {
    if (result.columns.empty()) {
        return;
    }

    std::vector<std::size_t> widths;
    for (const std::string& name : result.columns) {
        widths.push_back(name.size());
    }
    std::vector<std::vector<std::string>> cells;
    for (const Row& row : result.rows) {
        std::vector<std::string> line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            std::string text = row[i].is_null() ? "NULL" : row[i].to_string();
            if (i < widths.size()) {
                widths[i] = std::max(widths[i], text.size());
            }
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }

    auto rule = [&]() {
        std::string out = "+";
        for (const std::size_t w : widths) {
            out += std::string(w + 2, '-') + "+";
        }
        return out;
    };

    std::cout << rule() << "\n|";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        std::cout << " " << result.columns[i]
                  << std::string(widths[i] - result.columns[i].size(), ' ') << " |";
    }
    std::cout << "\n" << rule() << "\n";

    for (const auto& line : cells) {
        std::cout << "|";
        for (std::size_t i = 0; i < line.size() && i < widths.size(); ++i) {
            std::cout << " " << line[i] << std::string(widths[i] - line[i].size(), ' ') << " |";
        }
        std::cout << "\n";
    }
    std::cout << rule() << "\n";
    std::cout << result.rows.size() << (result.rows.size() == 1 ? " row\n" : " rows\n");
}

bool dot_command(const std::string& line, Database& db, Session& session, bool* show_plan) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;

    if (command == ".quit" || command == ".exit") {
        return false;
    }
    if (command == ".plan") {
        std::string arg;
        stream >> arg;
        std::transform(arg.begin(), arg.end(), arg.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        *show_plan = arg == "on";
        std::cout << "plan " << (*show_plan ? "on" : "off") << "\n";
        return true;
    }
    if (command == ".tables") {
        auto txn = db.begin();
        std::vector<std::pair<std::string, std::string>> entries;
        const Status s = txn->scan_prefix(std::string(1, kCatalogPrefix), &entries);
        if (!s) {
            std::cout << "error: " << s.to_string() << "\n";
            return true;
        }
        for (const auto& [key, value] : entries) {
            if (key.size() <= 1) {
                continue; // the id sequence record
            }
            TableDef def;
            if (decode_table_def(as_bytes(value), &def)) {
                std::cout << "  " << def.name << " (" << def.columns.size() << " columns)\n";
            }
        }
        txn->abort();
        return true;
    }
    if (command == ".schema") {
        std::string name;
        stream >> name;
        auto txn = db.begin();
        Catalog catalog(*txn);
        TableDef def;
        if (Status s = catalog.lookup(name, &def); !s) {
            std::cout << "no such table: " << name << "\n";
        } else {
            std::cout << "CREATE TABLE " << def.name << " (\n";
            for (std::size_t i = 0; i < def.columns.size(); ++i) {
                std::cout << "  " << def.columns[i].name << " " << type_name(def.columns[i].type)
                          << (def.columns[i].primary_key ? " PRIMARY KEY" : "")
                          << (def.columns[i].not_null ? " NOT NULL" : "")
                          << (i + 1 < def.columns.size() ? "," : "") << "\n";
            }
            std::cout << ")\n";
        }
        txn->abort();
        return true;
    }

    std::cout << "unknown command: " << command << "\n";
    (void)session;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : std::string();
    bool temporary = path.empty();
    if (temporary) {
        path = (std::filesystem::temp_directory_path() / "strata_shell.db").string();
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
    }

    Database db;
    if (Status s = db.open(path); !s) {
        std::cerr << "cannot open " << path << ": " << s.to_string() << "\n";
        return 1;
    }
    Session session(db);
    bool show_plan = false;

    std::cout << "strata — " << (temporary ? "temporary database" : path) << "\n"
              << "enter SQL terminated by ';', or .quit\n\n";

    std::string buffer;
    for (;;) {
        std::cout << (buffer.empty() ? "strata> " : "   ...> ") << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        const std::string trimmed = trim(line);
        if (buffer.empty() && !trimmed.empty() && trimmed[0] == '.') {
            if (!dot_command(trimmed, db, session, &show_plan)) {
                break;
            }
            continue;
        }

        buffer += line + "\n";
        if (trim(buffer).empty() || trim(buffer).back() != ';') {
            continue; // keep reading
        }

        ResultSet result;
        const Status s = session.run(buffer, &result);
        buffer.clear();

        if (!s) {
            std::cout << "error: " << s.to_string() << "\n";
            continue;
        }
        if (show_plan && !result.plan.empty()) {
            std::cout << result.plan << "\n";
        }
        if (result.is_query) {
            print_table(result);
        } else {
            std::cout << "ok";
            if (result.rows_affected > 0) {
                std::cout << " (" << result.rows_affected << " rows)";
            }
            std::cout << "\n";
        }
    }

    std::cout << "\n";
    return 0;
}
