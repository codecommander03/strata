#pragma once

#include "strata/db.hpp"
#include "strata/sql/ast.hpp"
#include "strata/sql/catalog.hpp"
#include "strata/sql/record.hpp"

#include <memory>
#include <string>
#include <vector>

namespace strata::sql {

/// What a statement produced. A query fills `columns` and `rows`; everything
/// else reports `rows_affected`.
struct ResultSet {
    bool is_query = false;
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::int64_t rows_affected = 0;
    /// The plan that produced it, one line per node. Empty for non-queries.
    std::string plan;
};

/// A row and the schema it belongs to, which is everything an expression needs
/// in order to resolve a column reference.
struct EvalContext {
    const TableDef* table = nullptr;
    const Row* row = nullptr;
};

/// Evaluates an expression under SQL's three-valued logic.
///
/// Null is not false. `NULL = NULL` is null, `NULL AND false` is false, and
/// `NULL AND true` is null — the distinction that makes a WHERE clause keep
/// only rows that are *definitely* true.
Status evaluate(const Expr& expr, const EvalContext& context, Value* out);

/// Adjusts a value to a column's declared type where that can be done without
/// losing information. Follows SQLite's type affinity rather than rejecting
/// the row, because sqllogictest's expected output assumes it.
Value coerce(const Value& value, Type target);

/// One node of a query plan. Volcano-style: pull one row at a time, so a LIMIT
/// stops the scan underneath it rather than filtering a finished result.
class Operator {
public:
    virtual ~Operator() = default;
    virtual Status next(Row* out, bool* has_row) = 0;
    virtual const std::vector<std::string>& columns() const = 0;

    /// One line per node, indented by depth — what the playground shows as the
    /// query plan.
    virtual std::string describe(int indent = 0) const = 0;
};

using OperatorPtr = std::unique_ptr<Operator>;

/// Runs parsed statements against a transaction.
class Executor {
public:
    explicit Executor(Transaction& txn) : txn_(&txn) {}

    Status execute(const Statement& statement, ResultSet* out);

private:
    Status execute_create(const CreateTable& statement, ResultSet* out);
    Status execute_drop(const DropTable& statement, ResultSet* out);
    Status execute_insert(const Insert& statement, ResultSet* out);
    Status execute_select(const Select& statement, ResultSet* out);
    Status execute_update(const Update& statement, ResultSet* out);
    Status execute_delete(const Delete& statement, ResultSet* out);

    Transaction* txn_;
};

} // namespace strata::sql
