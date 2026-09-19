#pragma once

#include "strata/btree.hpp"
#include "strata/encoding.hpp"
#include "strata/pager.hpp"
#include "strata/types.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace strata {

/// What a transaction can see.
///
/// Taken when a transaction starts and never changed, which is what makes
/// repeated reads repeatable. A version written by transaction X is visible if
/// X had already finished when this snapshot was taken — which means X is below
/// the boundary and was not one of the transactions still running.
struct Snapshot {
    Xid boundary = kFirstXid; ///< every xid at or above this is in the future
    std::set<Xid> active;     ///< running when the snapshot was taken

    bool sees(Xid writer) const { return writer < boundary && active.find(writer) == active.end(); }
};

class Database;

/// A read/write transaction under snapshot isolation.
///
/// Writes go into a private buffer, not into the tree. Nothing another
/// transaction can reach is touched until commit, which is why a dirty read is
/// not merely prevented here but unrepresentable. See decision 012.
class Transaction {
public:
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = default;

    Xid id() const { return xid_; }
    bool active() const { return state_ == State::Active; }

    /// Reads own writes first, then the newest version the snapshot can see.
    /// NotFound if the key does not exist or was deleted.
    Status get(Bytes key, std::string* out);

    Status put(Bytes key, Bytes value);
    Status remove(Bytes key);

    /// Every live key starting with `prefix`, in key order, as this
    /// transaction sees them — committed versions merged with its own
    /// uncommitted writes.
    ///
    /// This materialises rather than returning a cursor, because a cursor into
    /// the tree is invalidated by any write and callers routinely write while
    /// iterating. See decision 015.
    Status scan_prefix(const std::string& prefix,
                       std::vector<std::pair<std::string, std::string>>* out);

    /// Applies the write set and makes it durable. Fails with Conflict if any
    /// key written here was also written by a transaction that committed after
    /// this one's snapshot — first committer wins.
    Status commit();

    /// Discards the write set. Nothing was ever visible, so there is nothing to
    /// undo.
    void abort();

    /// Keys this transaction has written. Used by the tests.
    std::size_t write_set_size() const { return writes_.size(); }

private:
    friend class Database;

    enum class State { Active, Committed, Aborted };

    struct Write {
        std::string value;
        bool deleted = false;
    };

    Transaction(Database& db, Xid xid, Snapshot snapshot);

    /// Walks the version chain for `key` and returns the newest visible one.
    Status visible_version(Bytes key, bool* deleted, std::string* payload);

    Database* db_;
    Xid xid_;
    Snapshot snapshot_;
    std::map<std::string, Write> writes_;
    State state_ = State::Active;
};

/// The database: a file, a tree over it, and the transaction bookkeeping.
///
/// Single-process and single-threaded. Concurrency here means several
/// transactions open at once in one thread, interleaved by the caller — which
/// is precisely the shape the Hermitage tests are written in.
class Database {
public:
    Database() = default;

    Status open(const std::string& path);
    void close();

    /// Starts a transaction and takes its snapshot.
    std::unique_ptr<Transaction> begin();

    /// Convenience for tests and one-shot use: a transaction of one statement.
    Status put(Bytes key, Bytes value);
    Status get(Bytes key, std::string* out);
    Status remove(Bytes key);

    /// Every live user key visible to a fresh snapshot, in order. Used by tests
    /// and, later, by sequential scan.
    Status scan(std::vector<std::pair<std::string, std::string>>* out);

    Pager& pager() { return pager_; }
    BTree& tree() { return *tree_; }

    std::size_t active_transaction_count() const { return active_.size(); }

private:
    friend class Transaction;

    Snapshot take_snapshot() const;
    void finish(Xid xid);

    Pager pager_;
    std::unique_ptr<BTree> tree_;
    Xid next_xid_ = kFirstXid;
    std::set<Xid> active_;
};

/// Returned when two transactions wrote the same key and this one lost.
Status conflict_status(const std::string& key);

} // namespace strata
