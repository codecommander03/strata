#include "strata/db.hpp"

#include <algorithm>

namespace strata {

Status conflict_status(const std::string& key) {
    return Status::conflict("key '" + key +
                            "' was written by a transaction that committed "
                            "after this one's snapshot");
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Status Database::open(const std::string& path) {
    close();
    if (Status s = pager_.open(path); !s) {
        return s;
    }
    tree_ = std::make_unique<BTree>(pager_);
    next_xid_ = std::max<Xid>(pager_.next_xid(), kFirstXid);
    active_.clear();
    return Status::ok();
}

void Database::close() {
    tree_.reset();
    pager_.close();
    active_.clear();
    next_xid_ = kFirstXid;
}

Snapshot Database::take_snapshot() const {
    Snapshot s;
    s.boundary = next_xid_;
    s.active = active_;
    return s;
}

std::unique_ptr<Transaction> Database::begin() {
    // Order matters: the snapshot is taken before this transaction's own id
    // exists, so it never appears in its own active set.
    Snapshot snapshot = take_snapshot();
    const Xid xid = next_xid_++;
    active_.insert(xid);
    return std::unique_ptr<Transaction>(new Transaction(*this, xid, std::move(snapshot)));
}

void Database::finish(Xid xid) { active_.erase(xid); }

Status Database::put(Bytes key, Bytes value) {
    auto txn = begin();
    if (Status s = txn->put(key, value); !s) {
        txn->abort();
        return s;
    }
    return txn->commit();
}

Status Database::get(Bytes key, std::string* out) {
    auto txn = begin();
    const Status s = txn->get(key, out);
    txn->abort();
    return s;
}

Status Database::remove(Bytes key) {
    auto txn = begin();
    if (Status s = txn->remove(key); !s) {
        txn->abort();
        return s;
    }
    return txn->commit();
}

Status Database::scan(std::vector<std::pair<std::string, std::string>>* out) {
    out->clear();
    const Snapshot snapshot = take_snapshot();

    auto cursor = tree_->cursor();
    if (Status s = cursor.seek_first(); !s) {
        return s;
    }

    std::string handled;
    bool have_handled = false;

    while (cursor.valid()) {
        std::string user_key;
        Xid version = 0;
        if (!decode_key(cursor.key(), &user_key, &version)) {
            return Status::corruption("malformed encoded key");
        }

        // Versions of one key are adjacent and newest-first, so once a key has
        // yielded its visible version the rest of its chain is history.
        if (have_handled && user_key == handled) {
            if (Status s = cursor.next(); !s) {
                return s;
            }
            continue;
        }

        if (snapshot.sees(version)) {
            bool deleted = false;
            std::string payload;
            if (!decode_version(cursor.value(), &deleted, &payload)) {
                return Status::corruption("malformed version record");
            }
            if (!deleted) {
                out->emplace_back(user_key, payload);
            }
            handled = user_key;
            have_handled = true;
        }

        if (Status s = cursor.next(); !s) {
            return s;
        }
    }
    return Status::ok();
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Database& db, Xid xid, Snapshot snapshot)
    : db_(&db), xid_(xid), snapshot_(std::move(snapshot)) {}

Transaction::~Transaction() {
    if (state_ == State::Active) {
        abort();
    }
}

Status Transaction::visible_version(Bytes key, bool* deleted, std::string* payload) {
    const std::string wanted(as_string_view(key));
    const std::string prefix = key_prefix(key);

    auto cursor = db_->tree().cursor();
    if (Status s = cursor.seek(as_bytes(prefix)); !s) {
        return s;
    }

    while (cursor.valid()) {
        std::string user_key;
        Xid version = 0;
        if (!decode_key(cursor.key(), &user_key, &version)) {
            return Status::corruption("malformed encoded key");
        }
        if (user_key != wanted) {
            break; // walked off the end of this key's chain
        }
        if (snapshot_.sees(version)) {
            if (!decode_version(cursor.value(), deleted, payload)) {
                return Status::corruption("malformed version record");
            }
            return Status::ok();
        }
        if (Status s = cursor.next(); !s) {
            return s;
        }
    }
    return Status::not_found("no visible version");
}

Status Transaction::get(Bytes key, std::string* out) {
    if (state_ != State::Active) {
        return Status::invalid_argument("transaction is not active");
    }

    // Own writes first: a transaction always sees what it has done itself.
    if (const auto it = writes_.find(std::string(as_string_view(key))); it != writes_.end()) {
        if (it->second.deleted) {
            return Status::not_found("deleted in this transaction");
        }
        *out = it->second.value;
        return Status::ok();
    }

    bool deleted = false;
    std::string payload;
    if (Status s = visible_version(key, &deleted, &payload); !s) {
        return s;
    }
    if (deleted) {
        return Status::not_found("key was deleted");
    }
    *out = std::move(payload);
    return Status::ok();
}

Status Transaction::put(Bytes key, Bytes value) {
    if (state_ != State::Active) {
        return Status::invalid_argument("transaction is not active");
    }
    if (key.empty()) {
        return Status::invalid_argument("empty key");
    }
    writes_[std::string(as_string_view(key))] = Write{std::string(as_string_view(value)), false};
    return Status::ok();
}

Status Transaction::remove(Bytes key) {
    if (state_ != State::Active) {
        return Status::invalid_argument("transaction is not active");
    }

    // Deleting something that is not there is an error, so the visibility check
    // has to happen now rather than at commit.
    std::string existing;
    if (Status s = get(key, &existing); !s) {
        return s;
    }
    writes_[std::string(as_string_view(key))] = Write{std::string(), true};
    return Status::ok();
}

Status Transaction::scan_prefix(const std::string& prefix,
                                std::vector<std::pair<std::string, std::string>>* out) {
    if (state_ != State::Active) {
        return Status::invalid_argument("transaction is not active");
    }
    out->clear();

    // Committed state first, newest visible version of each key.
    std::map<std::string, std::string> live;
    {
        const std::string encoded = encode_prefix(as_bytes(prefix));
        auto cursor = db_->tree().cursor();
        if (Status s = cursor.seek(as_bytes(encoded)); !s) {
            return s;
        }

        std::string handled;
        bool have_handled = false;

        while (cursor.valid()) {
            std::string user_key;
            Xid version = 0;
            if (!decode_key(cursor.key(), &user_key, &version)) {
                return Status::corruption("malformed encoded key");
            }
            if (user_key.compare(0, prefix.size(), prefix) != 0) {
                break; // past the end of the namespace
            }

            if (!have_handled || user_key != handled) {
                if (snapshot_.sees(version)) {
                    bool deleted = false;
                    std::string payload;
                    if (!decode_version(cursor.value(), &deleted, &payload)) {
                        return Status::corruption("malformed version record");
                    }
                    if (!deleted) {
                        live.emplace(user_key, std::move(payload));
                    }
                    handled = user_key;
                    have_handled = true;
                }
            }

            if (Status s = cursor.next(); !s) {
                return s;
            }
        }
    }

    // Then this transaction's own writes, which win over what it can see.
    for (const auto& [key, write] : writes_) {
        if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (write.deleted) {
            live.erase(key);
        } else {
            live[key] = write.value;
        }
    }

    out->assign(live.begin(), live.end());
    return Status::ok();
}

Status Transaction::commit() {
    if (state_ != State::Active) {
        return Status::invalid_argument("transaction is not active");
    }
    if (writes_.empty()) {
        state_ = State::Committed;
        db_->finish(xid_);
        return Status::ok();
    }

    // First committer wins. If the newest version of a key we wrote is one this
    // snapshot cannot see, somebody else got there after we started.
    for (const auto& [key, write] : writes_) {
        (void)write;
        auto cursor = db_->tree().cursor();
        const std::string prefix = key_prefix(as_bytes(key));
        if (Status s = cursor.seek(as_bytes(prefix)); !s) {
            return s;
        }
        if (!cursor.valid()) {
            continue;
        }
        std::string user_key;
        Xid version = 0;
        if (!decode_key(cursor.key(), &user_key, &version)) {
            return Status::corruption("malformed encoded key");
        }
        if (user_key != key) {
            continue; // no versions of this key at all
        }
        if (!snapshot_.sees(version)) {
            const Status s = conflict_status(key);
            abort();
            return s;
        }
    }

    for (const auto& [key, write] : writes_) {
        const std::string encoded = encode_key(as_bytes(key), xid_);
        const std::string record = encode_version(write.deleted, as_bytes(write.value));
        if (Status s = db_->tree().insert(as_bytes(encoded), as_bytes(record)); !s) {
            abort();
            return s;
        }
    }

    // The id counter must be durable with the data it stamped, or a restart
    // could hand the same id out twice.
    db_->pager().set_next_xid(db_->next_xid_);
    if (Status s = db_->pager().commit(); !s) {
        abort();
        return s;
    }

    writes_.clear();
    state_ = State::Committed;
    db_->finish(xid_);
    return Status::ok();
}

void Transaction::abort() {
    if (state_ != State::Active) {
        return;
    }
    // Nothing this transaction wrote ever left its own buffer, so there is
    // nothing in the tree to undo.
    writes_.clear();
    state_ = State::Aborted;
    db_->finish(xid_);
}

} // namespace strata
