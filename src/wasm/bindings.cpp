// The WebAssembly surface: a handful of C functions that take SQL and return
// JSON.
//
// Deliberately not embind. embind is friendlier but pulls in RTTI and a
// sizeable runtime, and the whole argument for a WASM playground is that the
// page is small enough to be a static file with no server behind it. Three
// functions and a JSON string cost nothing.
//
// Every returned pointer is owned by a static string and is valid until the
// next call. JavaScript copies it immediately, so that is sufficient.

#include "strata/sql/catalog.hpp"
#include "strata/sql/parser.hpp"
#include "strata/sql/session.hpp"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

using namespace strata;
using namespace strata::sql;

namespace {

/// The database lives in Emscripten's in-memory filesystem, so every visitor
/// gets their own copy in their own tab and there is nothing to host.
constexpr const char* kDbPath = "/strata.db";

struct Instance {
    Database db;
    std::unique_ptr<Session> session;
};

Instance* g_instance = nullptr;
std::string g_result;

void escape_json(const std::string& in, std::string* out) {
    for (const char c : in) {
        switch (c) {
        case '"':
            out->append("\\\"");
            break;
        case '\\':
            out->append("\\\\");
            break;
        case '\n':
            out->append("\\n");
            break;
        case '\r':
            out->append("\\r");
            break;
        case '\t':
            out->append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out->append(buffer);
            } else {
                out->push_back(c);
            }
        }
    }
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    escape_json(s, &out);
    out.push_back('"');
    return out;
}

const char* publish(std::string json) {
    g_result = std::move(json);
    return g_result.c_str();
}

const char* error_json(const std::string& message) {
    return publish("{\"ok\":false,\"error\":" + quoted(message) + "}");
}

std::string value_json(const Value& value) {
    if (value.is_null()) {
        return "null";
    }
    if (value.type() == Type::Text) {
        return quoted(value.text());
    }
    return quoted(value.to_string());
}

} // namespace

extern "C" {

/// Creates (or recreates) the database. Safe to call repeatedly — the
/// playground's Reset button does exactly that.
EMSCRIPTEN_KEEPALIVE const char* strata_open() {
    delete g_instance;
    g_instance = nullptr;

    std::remove(kDbPath);
    std::remove((std::string(kDbPath) + "-wal").c_str());

    auto instance = std::make_unique<Instance>();
    if (Status s = instance->db.open(kDbPath); !s) {
        return error_json(s.to_string());
    }
    instance->session = std::make_unique<Session>(instance->db);
    g_instance = instance.release();
    return publish("{\"ok\":true}");
}

/// Runs one statement and returns its result, plan and page counters.
EMSCRIPTEN_KEEPALIVE const char* strata_exec(const char* sql) {
    if (g_instance == nullptr) {
        return error_json("database is not open");
    }

    g_instance->db.pager().reset_counters();

    ResultSet result;
    const Status status = g_instance->session->run(sql, &result);
    if (!status) {
        return error_json(status.to_string());
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"isQuery\":" << (result.is_query ? "true" : "false")
        << ",\"rowsAffected\":" << result.rows_affected << ",\"plan\":" << quoted(result.plan);

    out << ",\"columns\":[";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        out << (i == 0 ? "" : ",") << quoted(result.columns[i]);
    }
    out << "],\"rows\":[";
    for (std::size_t r = 0; r < result.rows.size(); ++r) {
        out << (r == 0 ? "" : ",") << "[";
        for (std::size_t c = 0; c < result.rows[r].size(); ++c) {
            out << (c == 0 ? "" : ",") << value_json(result.rows[r][c]);
        }
        out << "]";
    }
    out << "]";

    Pager& pager = g_instance->db.pager();
    out << ",\"stats\":{\"pageFetches\":" << pager.fetches()
        << ",\"pageLoads\":" << pager.page_loads() << ",\"pageCount\":" << pager.page_count()
        << ",\"cachedPages\":" << pager.cached_page_count()
        << ",\"walPages\":" << pager.wal().committed_page_count() << "}";

    out << "}";
    return publish(out.str());
}

/// Parses without executing, and returns the token stream and the rendered
/// AST. This is what makes the playground a teaching tool rather than a form:
/// you can see what the parser thought you meant.
EMSCRIPTEN_KEEPALIVE const char* strata_parse(const char* sql) {
    ParseError error;
    std::vector<Token> tokens;
    Lexer lexer(sql);
    if (!lexer.tokenise(&tokens, &error)) {
        return error_json(error.to_string());
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"tokens\":[";
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) { // skip EndOfInput
        out << (i == 0 ? "" : ",") << "{\"type\":" << quoted(token_type_name(tokens[i].type))
            << ",\"text\":" << quoted(tokens[i].text) << ",\"line\":" << tokens[i].loc.line
            << ",\"column\":" << tokens[i].loc.column << "}";
    }
    out << "]";

    Parser parser(sql);
    std::vector<Statement> statements;
    if (!parser.parse(&statements, &error)) {
        out << ",\"parsed\":false,\"parseError\":" << quoted(error.to_string())
            << ",\"errorLine\":" << error.loc.line << ",\"errorColumn\":" << error.loc.column
            << "}";
        return publish(out.str());
    }

    out << ",\"parsed\":true,\"statements\":[";
    for (std::size_t i = 0; i < statements.size(); ++i) {
        out << (i == 0 ? "" : ",") << "{\"kind\":" << quoted(statement_kind(statements[i]));
        if (const auto* select = std::get_if<Select>(&statements[i])) {
            if (select->where != nullptr) {
                out << ",\"where\":" << quoted(to_string(*select->where));
            }
        }
        out << "}";
    }
    out << "]}";
    return publish(out.str());
}

/// The tables currently defined, for the schema sidebar.
EMSCRIPTEN_KEEPALIVE const char* strata_schema() {
    if (g_instance == nullptr) {
        return error_json("database is not open");
    }

    auto txn = g_instance->db.begin();
    std::vector<std::pair<std::string, std::string>> entries;
    if (Status s = txn->scan_prefix(std::string(1, kCatalogPrefix), &entries); !s) {
        txn->abort();
        return error_json(s.to_string());
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"tables\":[";
    bool first = true;
    for (const auto& [key, value] : entries) {
        if (key.size() <= 1) {
            continue; // the table-id sequence record
        }
        TableDef def;
        if (!decode_table_def(as_bytes(value), &def)) {
            continue;
        }
        out << (first ? "" : ",") << "{\"name\":" << quoted(def.name) << ",\"columns\":[";
        for (std::size_t i = 0; i < def.columns.size(); ++i) {
            out << (i == 0 ? "" : ",") << "{\"name\":" << quoted(def.columns[i].name)
                << ",\"type\":" << quoted(type_name(def.columns[i].type))
                << ",\"primaryKey\":" << (def.columns[i].primary_key ? "true" : "false")
                << ",\"notNull\":" << (def.columns[i].not_null ? "true" : "false") << "}";
        }
        out << "]}";
        first = false;
    }
    out << "]}";
    txn->abort();
    return publish(out.str());
}

} // extern "C"
