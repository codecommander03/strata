#include "strata/sql/executor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace strata::sql {
namespace {

/// Tri-state for SQL's logic: a boolean that can also be unknown.
enum class Tri { False, True, Unknown };

Tri truth_of(const Value& value) {
    if (value.is_null()) {
        return Tri::Unknown;
    }
    return value.is_true() ? Tri::True : Tri::False;
}

Value from_tri(Tri t) {
    switch (t) {
    case Tri::False:
        return Value(std::int64_t{0});
    case Tri::True:
        return Value(std::int64_t{1});
    default:
        return Value::null();
    }
}

bool parse_number(const std::string& text, Value* out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long as_int = std::strtoll(text.c_str(), &end, 10);
    if (errno == 0 && end != nullptr && *end == '\0') {
        *out = Value(static_cast<std::int64_t>(as_int));
        return true;
    }
    errno = 0;
    end = nullptr;
    const double as_real = std::strtod(text.c_str(), &end);
    if (errno == 0 && end != nullptr && *end == '\0' && end != text.c_str()) {
        *out = Value(as_real);
        return true;
    }
    return false;
}

std::string default_column_name(const Expr& expr) {
    if (const auto* column = std::get_if<ColumnRef>(&expr.node)) {
        return column->name;
    }
    return to_string(expr);
}

// --- operators --------------------------------------------------------------

/// Reads every live row of one table.
///
/// The rows are materialised up front by `Transaction::scan_prefix` rather than
/// streamed from a cursor, because a tree cursor is invalidated by any write
/// and UPDATE and DELETE write while they scan. See decision 015.
class SeqScan : public Operator {
public:
    SeqScan(std::vector<Row> rows, std::vector<RowId> ids, std::vector<std::string> columns,
            std::string table)
        : rows_(std::move(rows)), ids_(std::move(ids)), columns_(std::move(columns)),
          table_(std::move(table)) {}

    Status next(Row* out, bool* has_row) override {
        if (at_ >= rows_.size()) {
            *has_row = false;
            return Status::ok();
        }
        *out = rows_[at_++];
        *has_row = true;
        return Status::ok();
    }

    const std::vector<std::string>& columns() const override { return columns_; }

    std::string describe(int indent) const override {
        return std::string(indent * 2, ' ') + "SeqScan " + table_ + "  (" +
               std::to_string(rows_.size()) + " rows)";
    }

    const std::vector<RowId>& ids() const { return ids_; }

private:
    std::vector<Row> rows_;
    std::vector<RowId> ids_;
    std::vector<std::string> columns_;
    std::string table_;
    std::size_t at_ = 0;
};

class Filter : public Operator {
public:
    Filter(OperatorPtr child, const Expr* predicate, const TableDef* table)
        : child_(std::move(child)), predicate_(predicate), table_(table) {}

    Status next(Row* out, bool* has_row) override {
        for (;;) {
            Row row;
            bool got = false;
            if (Status s = child_->next(&row, &got); !s) {
                return s;
            }
            if (!got) {
                *has_row = false;
                return Status::ok();
            }
            EvalContext context{table_, &row};
            Value verdict;
            if (Status s = evaluate(*predicate_, context, &verdict); !s) {
                return s;
            }
            // Null is not true, so a row whose predicate is unknown is dropped.
            if (verdict.is_true()) {
                *out = std::move(row);
                *has_row = true;
                return Status::ok();
            }
        }
    }

    const std::vector<std::string>& columns() const override { return child_->columns(); }

    std::string describe(int indent) const override {
        return std::string(indent * 2, ' ') + "Filter " + to_string(*predicate_) + "\n" +
               child_->describe(indent + 1);
    }

private:
    OperatorPtr child_;
    const Expr* predicate_;
    const TableDef* table_;
};

class Project : public Operator {
public:
    Project(OperatorPtr child, const std::vector<SelectItem>* items, const TableDef* table,
            std::vector<std::string> names)
        : child_(std::move(child)), items_(items), table_(table), names_(std::move(names)) {}

    Status next(Row* out, bool* has_row) override {
        Row row;
        bool got = false;
        if (Status s = child_->next(&row, &got); !s) {
            return s;
        }
        if (!got) {
            *has_row = false;
            return Status::ok();
        }

        EvalContext context{table_, &row};
        Row projected;
        projected.reserve(items_->size());
        for (const SelectItem& item : *items_) {
            Value value;
            if (Status s = evaluate(*item.expr, context, &value); !s) {
                return s;
            }
            projected.push_back(std::move(value));
        }
        *out = std::move(projected);
        *has_row = true;
        return Status::ok();
    }

    const std::vector<std::string>& columns() const override { return names_; }

    std::string describe(int indent) const override {
        std::string list;
        for (std::size_t i = 0; i < names_.size(); ++i) {
            list += (i == 0 ? "" : ", ") + names_[i];
        }
        return std::string(indent * 2, ' ') + "Project " + list + "\n" +
               child_->describe(indent + 1);
    }

private:
    OperatorPtr child_;
    const std::vector<SelectItem>* items_;
    const TableDef* table_;
    std::vector<std::string> names_;
};

/// Materialises its input. ORDER BY is the one place a plan must see every row
/// before it can emit the first.
class Sort : public Operator {
public:
    Sort(OperatorPtr child, const std::vector<OrderTerm>* terms, const TableDef* table)
        : child_(std::move(child)), terms_(terms), table_(table) {}

    Status next(Row* out, bool* has_row) override {
        if (!loaded_) {
            if (Status s = load(); !s) {
                return s;
            }
        }
        if (at_ >= sorted_.size()) {
            *has_row = false;
            return Status::ok();
        }
        *out = sorted_[at_++];
        *has_row = true;
        return Status::ok();
    }

    const std::vector<std::string>& columns() const override { return child_->columns(); }

    std::string describe(int indent) const override {
        std::string list;
        for (std::size_t i = 0; i < terms_->size(); ++i) {
            list += (i == 0 ? "" : ", ") + to_string(*(*terms_)[i].expr) +
                    ((*terms_)[i].descending ? " DESC" : "");
        }
        return std::string(indent * 2, ' ') + "Sort " + list + "\n" + child_->describe(indent + 1);
    }

private:
    Status load() {
        std::vector<std::pair<Row, Row>> keyed; // (sort keys, row)
        for (;;) {
            Row row;
            bool got = false;
            if (Status s = child_->next(&row, &got); !s) {
                return s;
            }
            if (!got) {
                break;
            }
            EvalContext context{table_, &row};
            Row keys;
            keys.reserve(terms_->size());
            for (const OrderTerm& term : *terms_) {
                Value value;
                if (Status s = evaluate(*term.expr, context, &value); !s) {
                    return s;
                }
                keys.push_back(std::move(value));
            }
            keyed.emplace_back(std::move(keys), std::move(row));
        }

        const std::vector<OrderTerm>& terms = *terms_;
        // Stable, so that equal keys keep the order the scan produced them in
        // and a result is reproducible rather than merely correct.
        std::stable_sort(keyed.begin(), keyed.end(), [&terms](const auto& a, const auto& b) {
            for (std::size_t i = 0; i < terms.size(); ++i) {
                const int c = compare_values(a.first[i], b.first[i]);
                if (c != 0) {
                    return terms[i].descending ? c > 0 : c < 0;
                }
            }
            return false;
        });

        sorted_.reserve(keyed.size());
        for (auto& [keys, row] : keyed) {
            (void)keys;
            sorted_.push_back(std::move(row));
        }
        loaded_ = true;
        return Status::ok();
    }

    OperatorPtr child_;
    const std::vector<OrderTerm>* terms_;
    const TableDef* table_;
    std::vector<Row> sorted_;
    std::size_t at_ = 0;
    bool loaded_ = false;
};

class Limit : public Operator {
public:
    Limit(OperatorPtr child, std::int64_t limit) : child_(std::move(child)), limit_(limit) {}

    Status next(Row* out, bool* has_row) override {
        if (emitted_ >= limit_) {
            // Stop pulling. This is the point of a pull-based plan: the scan
            // underneath never produces the rows nobody asked for.
            *has_row = false;
            return Status::ok();
        }
        if (Status s = child_->next(out, has_row); !s) {
            return s;
        }
        if (*has_row) {
            ++emitted_;
        }
        return Status::ok();
    }

    const std::vector<std::string>& columns() const override { return child_->columns(); }

    std::string describe(int indent) const override {
        return std::string(indent * 2, ' ') + "Limit " + std::to_string(limit_) + "\n" +
               child_->describe(indent + 1);
    }

private:
    OperatorPtr child_;
    std::int64_t limit_;
    std::int64_t emitted_ = 0;
};

/// A single row of no columns, so `SELECT 1 + 1` has something to project from.
class SingleRow : public Operator {
public:
    Status next(Row* out, bool* has_row) override {
        if (done_) {
            *has_row = false;
            return Status::ok();
        }
        done_ = true;
        out->clear();
        *has_row = true;
        return Status::ok();
    }

    const std::vector<std::string>& columns() const override { return columns_; }

    std::string describe(int indent) const override {
        return std::string(indent * 2, ' ') + "SingleRow";
    }

private:
    std::vector<std::string> columns_;
    bool done_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Value coerce(const Value& value, Type target) {
    if (value.is_null()) {
        return value;
    }
    switch (target) {
    case Type::Integer:
        if (value.type() == Type::Integer) {
            return value;
        }
        if (value.type() == Type::Real) {
            const double d = value.real();
            const auto as_int = static_cast<std::int64_t>(d);
            // Only narrow when nothing is lost; 1.5 stays 1.5.
            return static_cast<double>(as_int) == d ? Value(as_int) : value;
        }
        {
            Value parsed;
            if (parse_number(value.text(), &parsed)) {
                return coerce(parsed, Type::Integer);
            }
        }
        return value;
    case Type::Real:
        if (value.type() == Type::Real) {
            return value;
        }
        if (value.type() == Type::Integer) {
            return Value(static_cast<double>(value.integer()));
        }
        {
            Value parsed;
            if (parse_number(value.text(), &parsed)) {
                return Value(parsed.as_double());
            }
        }
        return value;
    case Type::Text:
        return value.type() == Type::Text ? value : Value(value.to_string());
    case Type::Null:
        return value;
    }
    return value;
}

Status evaluate(const Expr& expr, const EvalContext& context, Value* out) {
    if (const auto* literal = std::get_if<Literal>(&expr.node)) {
        *out = literal->value;
        return Status::ok();
    }

    if (const auto* column = std::get_if<ColumnRef>(&expr.node)) {
        if (context.table == nullptr || context.row == nullptr) {
            return Status::invalid_argument("no such column: " + column->name);
        }
        const int index = context.table->column_index(column->name);
        if (index < 0) {
            return Status::invalid_argument("no such column: " + column->name);
        }
        // A row written before a column existed is short, not corrupt.
        *out = static_cast<std::size_t>(index) < context.row->size() ? (*context.row)[index]
                                                                     : Value::null();
        return Status::ok();
    }

    if (const auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
        Value operand;
        if (Status s = evaluate(*unary->operand, context, &operand); !s) {
            return s;
        }
        switch (unary->op) {
        case UnaryOperator::IsNull:
            *out = Value(static_cast<std::int64_t>(operand.is_null() ? 1 : 0));
            return Status::ok();
        case UnaryOperator::IsNotNull:
            *out = Value(static_cast<std::int64_t>(operand.is_null() ? 0 : 1));
            return Status::ok();
        case UnaryOperator::Not:
            switch (truth_of(operand)) {
            case Tri::True:
                *out = from_tri(Tri::False);
                break;
            case Tri::False:
                *out = from_tri(Tri::True);
                break;
            default:
                *out = Value::null();
            }
            return Status::ok();
        case UnaryOperator::Negate:
            if (operand.is_null()) {
                *out = Value::null();
            } else if (operand.type() == Type::Integer) {
                *out = Value(-operand.integer());
            } else if (operand.type() == Type::Real) {
                *out = Value(-operand.real());
            } else {
                *out = Value::null();
            }
            return Status::ok();
        }
    }

    const auto& binary = std::get<BinaryExpr>(expr.node);

    // AND and OR short-circuit differently from the rest: `false AND null` is
    // false, not null, so the left operand alone can settle it.
    if (binary.op == BinaryOperator::And || binary.op == BinaryOperator::Or) {
        Value left;
        if (Status s = evaluate(*binary.left, context, &left); !s) {
            return s;
        }
        const Tri a = truth_of(left);
        if (binary.op == BinaryOperator::And && a == Tri::False) {
            *out = from_tri(Tri::False);
            return Status::ok();
        }
        if (binary.op == BinaryOperator::Or && a == Tri::True) {
            *out = from_tri(Tri::True);
            return Status::ok();
        }

        Value right;
        if (Status s = evaluate(*binary.right, context, &right); !s) {
            return s;
        }
        const Tri b = truth_of(right);
        if (binary.op == BinaryOperator::And) {
            *out = from_tri(b == Tri::False                      ? Tri::False
                            : (a == Tri::True && b == Tri::True) ? Tri::True
                                                                 : Tri::Unknown);
        } else {
            *out = from_tri(b == Tri::True                         ? Tri::True
                            : (a == Tri::False && b == Tri::False) ? Tri::False
                                                                   : Tri::Unknown);
        }
        return Status::ok();
    }

    Value left;
    Value right;
    if (Status s = evaluate(*binary.left, context, &left); !s) {
        return s;
    }
    if (Status s = evaluate(*binary.right, context, &right); !s) {
        return s;
    }

    // Everything else is null-propagating.
    if (left.is_null() || right.is_null()) {
        *out = Value::null();
        return Status::ok();
    }

    switch (binary.op) {
    case BinaryOperator::Equal:
    case BinaryOperator::NotEqual:
    case BinaryOperator::Less:
    case BinaryOperator::LessEqual:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEqual: {
        const int c = compare_values(left, right);
        bool result = false;
        switch (binary.op) {
        case BinaryOperator::Equal:
            result = c == 0;
            break;
        case BinaryOperator::NotEqual:
            result = c != 0;
            break;
        case BinaryOperator::Less:
            result = c < 0;
            break;
        case BinaryOperator::LessEqual:
            result = c <= 0;
            break;
        case BinaryOperator::Greater:
            result = c > 0;
            break;
        default:
            result = c >= 0;
            break;
        }
        *out = Value(static_cast<std::int64_t>(result ? 1 : 0));
        return Status::ok();
    }
    default:
        break;
    }

    // Arithmetic on non-numbers yields null rather than an error, matching
    // SQLite. An error here would make one bad row fail a whole query.
    if (!left.is_numeric() || !right.is_numeric()) {
        *out = Value::null();
        return Status::ok();
    }

    const bool integral = left.type() == Type::Integer && right.type() == Type::Integer;
    switch (binary.op) {
    case BinaryOperator::Add:
        *out = integral ? Value(left.integer() + right.integer())
                        : Value(left.as_double() + right.as_double());
        return Status::ok();
    case BinaryOperator::Subtract:
        *out = integral ? Value(left.integer() - right.integer())
                        : Value(left.as_double() - right.as_double());
        return Status::ok();
    case BinaryOperator::Multiply:
        *out = integral ? Value(left.integer() * right.integer())
                        : Value(left.as_double() * right.as_double());
        return Status::ok();
    default:
        // Division by zero is null, not a crash and not an error.
        if (right.as_double() == 0.0) {
            *out = Value::null();
            return Status::ok();
        }
        *out = integral ? Value(left.integer() / right.integer())
                        : Value(left.as_double() / right.as_double());
        return Status::ok();
    }
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

namespace {

/// Checks every column reference against the schema before the plan runs.
///
/// Without this, `SELECT b FROM t` on an empty table succeeds: nothing ever
/// evaluates the expression, so nothing ever notices that `b` does not exist.
/// A query that is wrong should be wrong whether or not there is data.
Status validate_columns(const Expr& expr, const TableDef* table) {
    if (const auto* column = std::get_if<ColumnRef>(&expr.node)) {
        if (table == nullptr || table->column_index(column->name) < 0) {
            return Status::invalid_argument("no such column: " + column->name);
        }
        return Status::ok();
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
        return validate_columns(*unary->operand, table);
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
        if (Status s = validate_columns(*binary->left, table); !s) {
            return s;
        }
        return validate_columns(*binary->right, table);
    }
    return Status::ok(); // a literal refers to nothing
}

/// Loads every live row of a table, with its row id.
Status load_table(Transaction& txn, const TableDef& def, std::vector<Row>* rows,
                  std::vector<RowId>* ids) {
    std::vector<std::pair<std::string, std::string>> raw;
    if (Status s = txn.scan_prefix(table_range_start(def.id), &raw); !s) {
        return s;
    }
    rows->clear();
    ids->clear();
    rows->reserve(raw.size());
    ids->reserve(raw.size());

    for (const auto& [key, value] : raw) {
        TableId table = 0;
        RowId id = 0;
        if (!decode_row_key(key, &table, &id) || table != def.id) {
            continue;
        }
        Row row;
        if (!decode_row(as_bytes(value), &row)) {
            return Status::corruption("malformed row in table " + def.name);
        }
        rows->push_back(std::move(row));
        ids->push_back(id);
    }
    return Status::ok();
}

} // namespace

Status Executor::execute(const Statement& statement, ResultSet* out) {
    *out = ResultSet{};
    if (const auto* create = std::get_if<CreateTable>(&statement)) {
        return execute_create(*create, out);
    }
    if (const auto* drop = std::get_if<DropTable>(&statement)) {
        return execute_drop(*drop, out);
    }
    if (const auto* insert = std::get_if<Insert>(&statement)) {
        return execute_insert(*insert, out);
    }
    if (const auto* select = std::get_if<Select>(&statement)) {
        return execute_select(*select, out);
    }
    if (const auto* update = std::get_if<Update>(&statement)) {
        return execute_update(*update, out);
    }
    if (const auto* del = std::get_if<Delete>(&statement)) {
        return execute_delete(*del, out);
    }
    // Transaction control is handled by the caller, which owns the transaction.
    return Status::invalid_argument("BEGIN/COMMIT/ROLLBACK must be handled by the session");
}

Status Executor::execute_create(const CreateTable& statement, ResultSet* out) {
    Catalog catalog(*txn_);
    TableDef def;
    if (Status s = catalog.create(statement, &def); !s) {
        return s;
    }
    out->rows_affected = 0;
    return Status::ok();
}

Status Executor::execute_drop(const DropTable& statement, ResultSet* out) {
    Catalog catalog(*txn_);
    if (Status s = catalog.drop(statement); !s) {
        return s;
    }
    out->rows_affected = 0;
    return Status::ok();
}

Status Executor::execute_insert(const Insert& statement, ResultSet* out) {
    Catalog catalog(*txn_);
    TableDef def;
    if (Status s = catalog.lookup(statement.table, &def); !s) {
        return s.code() == Code::NotFound
                   ? Status::invalid_argument("no such table: " + statement.table)
                   : s;
    }

    // Which column each supplied value belongs to.
    std::vector<int> targets;
    if (statement.columns.empty()) {
        for (std::size_t i = 0; i < def.columns.size(); ++i) {
            targets.push_back(static_cast<int>(i));
        }
    } else {
        for (const std::string& name : statement.columns) {
            const int index = def.column_index(name);
            if (index < 0) {
                return Status::invalid_argument("no such column: " + name);
            }
            targets.push_back(index);
        }
    }

    for (const std::vector<ExprPtr>& source : statement.rows) {
        if (source.size() != targets.size()) {
            return Status::invalid_argument("row has " + std::to_string(source.size()) +
                                            " values but " + std::to_string(targets.size()) +
                                            " columns are being written");
        }

        Row row(def.columns.size(), Value::null());
        for (std::size_t i = 0; i < source.size(); ++i) {
            Value value;
            if (Status s = evaluate(*source[i], EvalContext{}, &value); !s) {
                return s;
            }
            row[static_cast<std::size_t>(targets[i])] =
                coerce(value, def.columns[static_cast<std::size_t>(targets[i])].type);
        }

        for (std::size_t i = 0; i < def.columns.size(); ++i) {
            if (def.columns[i].not_null && row[i].is_null()) {
                return Status::invalid_argument("column '" + def.columns[i].name +
                                                "' must not be null");
            }
        }

        const std::string key = row_key(def.id, def.next_row_id++);
        const std::string encoded = encode_row(row);
        if (Status s = txn_->put(as_bytes(key), as_bytes(encoded)); !s) {
            return s;
        }
        ++out->rows_affected;
    }

    // next_row_id moved, so the catalog entry has to move with it.
    return catalog.save(def);
}

Status Executor::execute_select(const Select& statement, ResultSet* out) {
    out->is_query = true;

    TableDef def;
    const TableDef* table = nullptr;
    OperatorPtr root;

    if (statement.table.empty()) {
        root = std::make_unique<SingleRow>();
    } else {
        Catalog catalog(*txn_);
        if (Status s = catalog.lookup(statement.table, &def); !s) {
            return s.code() == Code::NotFound
                       ? Status::invalid_argument("no such table: " + statement.table)
                       : s;
        }
        table = &def;

        std::vector<Row> rows;
        std::vector<RowId> ids;
        if (Status s = load_table(*txn_, def, &rows, &ids); !s) {
            return s;
        }
        std::vector<std::string> names;
        for (const ColumnDef& column : def.columns) {
            names.push_back(column.name);
        }
        root =
            std::make_unique<SeqScan>(std::move(rows), std::move(ids), std::move(names), def.name);
    }

    // Resolve every column reference before running anything, so that a query
    // naming a column that does not exist fails on an empty table too.
    for (const SelectItem& item : statement.items) {
        if (Status s = validate_columns(*item.expr, table); !s) {
            return s;
        }
    }
    if (statement.where != nullptr) {
        if (Status s = validate_columns(*statement.where, table); !s) {
            return s;
        }
    }
    for (const OrderTerm& term : statement.order_by) {
        if (Status s = validate_columns(*term.expr, table); !s) {
            return s;
        }
    }

    if (statement.where != nullptr) {
        root = std::make_unique<Filter>(std::move(root), statement.where.get(), table);
    }
    if (!statement.order_by.empty()) {
        root = std::make_unique<Sort>(std::move(root), &statement.order_by, table);
    }

    // SELECT * keeps the scan's own columns; anything else projects.
    if (!statement.star) {
        std::vector<std::string> names;
        for (const SelectItem& item : statement.items) {
            names.push_back(item.alias.empty() ? default_column_name(*item.expr) : item.alias);
        }
        root =
            std::make_unique<Project>(std::move(root), &statement.items, table, std::move(names));
    }
    if (statement.limit.has_value()) {
        root = std::make_unique<Limit>(std::move(root), *statement.limit);
    }

    out->columns = root->columns();
    out->plan = root->describe(0);
    for (;;) {
        Row row;
        bool got = false;
        if (Status s = root->next(&row, &got); !s) {
            return s;
        }
        if (!got) {
            break;
        }
        out->rows.push_back(std::move(row));
    }
    out->rows_affected = static_cast<std::int64_t>(out->rows.size());
    return Status::ok();
}

Status Executor::execute_update(const Update& statement, ResultSet* out) {
    Catalog catalog(*txn_);
    TableDef def;
    if (Status s = catalog.lookup(statement.table, &def); !s) {
        return s.code() == Code::NotFound
                   ? Status::invalid_argument("no such table: " + statement.table)
                   : s;
    }

    std::vector<int> targets;
    for (const Assignment& assignment : statement.assignments) {
        const int index = def.column_index(assignment.column);
        if (index < 0) {
            return Status::invalid_argument("no such column: " + assignment.column);
        }
        targets.push_back(index);
    }

    for (const Assignment& assignment : statement.assignments) {
        if (Status s = validate_columns(*assignment.value, &def); !s) {
            return s;
        }
    }
    if (statement.where != nullptr) {
        if (Status s = validate_columns(*statement.where, &def); !s) {
            return s;
        }
    }

    std::vector<Row> rows;
    std::vector<RowId> ids;
    if (Status s = load_table(*txn_, def, &rows, &ids); !s) {
        return s;
    }

    for (std::size_t r = 0; r < rows.size(); ++r) {
        EvalContext context{&def, &rows[r]};

        if (statement.where != nullptr) {
            Value verdict;
            if (Status s = evaluate(*statement.where, context, &verdict); !s) {
                return s;
            }
            if (!verdict.is_true()) {
                continue;
            }
        }

        // Every assignment reads the row as it was before this statement, so
        // `SET a = b, b = a` swaps rather than copying one into both.
        Row updated = rows[r];
        for (std::size_t i = 0; i < targets.size(); ++i) {
            Value value;
            if (Status s = evaluate(*statement.assignments[i].value, context, &value); !s) {
                return s;
            }
            const auto index = static_cast<std::size_t>(targets[i]);
            updated[index] = coerce(value, def.columns[index].type);
        }
        for (std::size_t i = 0; i < def.columns.size(); ++i) {
            if (def.columns[i].not_null && updated[i].is_null()) {
                return Status::invalid_argument("column '" + def.columns[i].name +
                                                "' must not be null");
            }
        }

        const std::string key = row_key(def.id, ids[r]);
        const std::string encoded = encode_row(updated);
        if (Status s = txn_->put(as_bytes(key), as_bytes(encoded)); !s) {
            return s;
        }
        ++out->rows_affected;
    }
    return Status::ok();
}

Status Executor::execute_delete(const Delete& statement, ResultSet* out) {
    Catalog catalog(*txn_);
    TableDef def;
    if (Status s = catalog.lookup(statement.table, &def); !s) {
        return s.code() == Code::NotFound
                   ? Status::invalid_argument("no such table: " + statement.table)
                   : s;
    }

    if (statement.where != nullptr) {
        if (Status s = validate_columns(*statement.where, &def); !s) {
            return s;
        }
    }

    std::vector<Row> rows;
    std::vector<RowId> ids;
    if (Status s = load_table(*txn_, def, &rows, &ids); !s) {
        return s;
    }

    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (statement.where != nullptr) {
            EvalContext context{&def, &rows[r]};
            Value verdict;
            if (Status s = evaluate(*statement.where, context, &verdict); !s) {
                return s;
            }
            if (!verdict.is_true()) {
                continue;
            }
        }
        const std::string key = row_key(def.id, ids[r]);
        if (Status s = txn_->remove(as_bytes(key)); !s) {
            return s;
        }
        ++out->rows_affected;
    }
    return Status::ok();
}

} // namespace strata::sql
