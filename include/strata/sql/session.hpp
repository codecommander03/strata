#pragma once

#include "strata/db.hpp"
#include "strata/sql/executor.hpp"
#include "strata/sql/parser.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace strata::sql {

/// One connection's worth of state: the database, and whichever transaction is
/// currently open.
///
/// Outside an explicit `BEGIN`, every statement runs in a transaction of its
/// own that commits if it succeeds and rolls back if it does not. That is
/// autocommit, and it is why a failed statement cannot leave half its work
/// behind.
class Session {
public:
    explicit Session(Database& db) : db_(&db) {}
    ~Session() { rollback_if_open(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Parses and runs one statement. Parse errors come back as
    /// InvalidArgument carrying the `line:column: message` text.
    Status run(std::string_view sql, ResultSet* out);

    /// Parses and runs every statement in `sql`, returning the last result.
    Status run_script(std::string_view sql, ResultSet* out);

    bool in_explicit_transaction() const { return explicit_ && txn_ != nullptr; }

private:
    Status execute_parsed(const Statement& statement, ResultSet* out);
    Status ensure_transaction();
    void rollback_if_open();

    Database* db_;
    std::unique_ptr<Transaction> txn_;
    bool explicit_ = false;
};

/// Renders a result the way sqllogictest expects: one value per line, with
/// nulls as "NULL" and reals to three decimal places.
std::vector<std::string> format_for_comparison(const ResultSet& result,
                                               const std::string& type_string);

} // namespace strata::sql
