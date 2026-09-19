#include "strata/sql/session.hpp"

#include <cstdio>

namespace strata::sql {

Status Session::ensure_transaction() {
    if (txn_ == nullptr) {
        txn_ = db_->begin();
    }
    return Status::ok();
}

void Session::rollback_if_open() {
    if (txn_ != nullptr) {
        txn_->abort();
        txn_.reset();
    }
    explicit_ = false;
}

Status Session::execute_parsed(const Statement& statement, ResultSet* out) {
    if (const auto* control = std::get_if<TransactionStatement>(&statement)) {
        switch (control->control) {
        case TransactionControl::Begin:
            if (explicit_) {
                return Status::invalid_argument("a transaction is already open");
            }
            if (Status s = ensure_transaction(); !s) {
                return s;
            }
            explicit_ = true;
            return Status::ok();
        case TransactionControl::Commit: {
            if (!explicit_ || txn_ == nullptr) {
                return Status::invalid_argument("no transaction is open");
            }
            const Status s = txn_->commit();
            txn_.reset();
            explicit_ = false;
            return s;
        }
        case TransactionControl::Rollback:
            if (!explicit_ || txn_ == nullptr) {
                return Status::invalid_argument("no transaction is open");
            }
            txn_->abort();
            txn_.reset();
            explicit_ = false;
            return Status::ok();
        }
    }

    if (Status s = ensure_transaction(); !s) {
        return s;
    }

    Executor executor(*txn_);
    const Status result = executor.execute(statement, out);

    if (!explicit_) {
        // Autocommit: this statement was its own transaction.
        if (!result) {
            txn_->abort();
            txn_.reset();
            return result;
        }
        const Status committed = txn_->commit();
        txn_.reset();
        return committed;
    }

    // Inside an explicit transaction a failed statement leaves the transaction
    // open, so the caller can choose between rolling back and carrying on.
    return result;
}

Status Session::run(std::string_view sql, ResultSet* out) {
    ParseError error;
    Parser parser(sql);
    Statement statement;
    if (!parser.parse_one(&statement, &error)) {
        return Status::invalid_argument(error.to_string());
    }
    return execute_parsed(statement, out);
}

Status Session::run_script(std::string_view sql, ResultSet* out) {
    ParseError error;
    Parser parser(sql);
    std::vector<Statement> statements;
    if (!parser.parse(&statements, &error)) {
        return Status::invalid_argument(error.to_string());
    }
    for (const Statement& statement : statements) {
        if (Status s = execute_parsed(statement, out); !s) {
            return s;
        }
    }
    return Status::ok();
}

std::vector<std::string> format_for_comparison(const ResultSet& result,
                                               const std::string& type_string) {
    std::vector<std::string> out;
    out.reserve(result.rows.size() * result.columns.size());

    for (const Row& row : result.rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            const Value& value = row[i];
            // The type string says how each column should be rendered: I for
            // integer, R for real, T for text. A value that does not fit its
            // declared letter is rendered by its own type instead of being
            // forced, so a mismatch shows up as a diff rather than silently
            // rounding away.
            const char kind = i < type_string.size() ? type_string[i] : 'T';

            if (value.is_null()) {
                out.emplace_back("NULL");
                continue;
            }
            switch (kind) {
            case 'I':
                if (value.type() == Type::Integer) {
                    out.push_back(std::to_string(value.integer()));
                } else if (value.is_numeric()) {
                    out.push_back(std::to_string(static_cast<std::int64_t>(value.as_double())));
                } else {
                    out.emplace_back(value.text().empty() ? "0" : value.text());
                }
                break;
            case 'R': {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%.3f",
                              value.is_numeric() ? value.as_double() : 0.0);
                out.emplace_back(buffer);
                break;
            }
            default:
                out.push_back(value.type() == Type::Text
                                  ? (value.text().empty() ? std::string("(empty)") : value.text())
                                  : value.to_string());
                break;
            }
        }
    }
    return out;
}

} // namespace strata::sql
