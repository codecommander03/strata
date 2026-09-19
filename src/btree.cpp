#include "strata/btree.hpp"

#include <algorithm>
#include <cstring>

namespace strata {
namespace {

/// Copies bytes out of a page before that page is modified underneath them.
std::string copy_of(Bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

/// The child to follow for `key`. Equal keys go right, so this is an upper
/// bound rather than the lower bound the page gives us directly.
std::uint16_t descend_slot(const Page& node, Bytes key) {
    auto [idx, exact] = node.lower_bound(key);
    if (exact) {
        ++idx;
    }
    return idx;
}

PageId child_at(const Page& node, std::uint16_t slot) {
    if (slot >= node.cell_count()) {
        return node.extra();
    }
    return node.internal_cell(slot).child;
}

} // namespace

// ---------------------------------------------------------------------------
// Splitting
// ---------------------------------------------------------------------------

Status BTree::split_leaf_page(Page& leaf, Page& right) {
    const std::uint16_t n = leaf.cell_count();
    if (n < 2) {
        return Status::full("cannot split a leaf holding fewer than two cells");
    }
    const std::uint16_t mid = static_cast<std::uint16_t>(n / 2);

    // Copy first, truncate second: the cells being read still live in the
    // page's content area until the page is reused.
    for (std::uint16_t i = mid; i < n; ++i) {
        const LeafCell cell = leaf.leaf_cell(i);
        if (Status s = right.insert_leaf_cell(right.cell_count(), cell.key, cell.value); !s) {
            return s;
        }
    }
    for (std::uint16_t i = n; i > mid; --i) {
        leaf.remove_cell(static_cast<std::uint16_t>(i - 1));
    }
    // The cells are gone but their bytes are not. Without this the left page
    // comes out of a split with half the cells and none of the space.
    leaf.compact();

    right.set_extra(leaf.extra());
    leaf.set_extra(right.id());
    return Status::ok();
}

Status BTree::split_internal_page(Page& node, Page& right, std::string* median) {
    const std::uint16_t n = node.cell_count();
    if (n < 2) {
        return Status::full("cannot split an internal page holding fewer than two cells");
    }
    const std::uint16_t m = static_cast<std::uint16_t>(n / 2);

    const InternalCell mid_cell = node.internal_cell(m);
    *median = copy_of(mid_cell.key);
    const PageId mid_child = mid_cell.child;

    // Everything above the midpoint moves right, along with the old rightmost.
    for (std::uint16_t i = static_cast<std::uint16_t>(m + 1); i < n; ++i) {
        const InternalCell cell = node.internal_cell(i);
        if (Status s = right.insert_internal_cell(right.cell_count(), cell.key, cell.child); !s) {
            return s;
        }
    }
    right.set_extra(node.extra());

    for (std::uint16_t i = n; i > m; --i) {
        node.remove_cell(static_cast<std::uint16_t>(i - 1));
    }
    node.compact();
    // The midpoint's key went up; its child becomes the left page's rightmost.
    node.set_extra(mid_child);
    return Status::ok();
}

Status BTree::place_separator(Page& node, Bytes separator, PageId left_child, PageId right_child) {
    auto [idx, exact] = node.lower_bound(separator);
    (void)exact;
    if (Status s = node.insert_internal_cell(idx, separator, left_child); !s) {
        return s;
    }
    // Whatever the old slot pointed at now belongs to the right half.
    if (idx + 1 == node.cell_count()) {
        node.set_extra(right_child);
    } else {
        node.set_internal_child(static_cast<std::uint16_t>(idx + 1), right_child);
    }
    return Status::ok();
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

Status BTree::insert_rec(PageId node_id, Bytes key, Bytes value, std::optional<Split>* split) {
    split->reset();

    Page node(nullptr, node_id);
    if (Status s = pager_.fetch(node_id, &node); !s) {
        return s;
    }

    if (node.type() == PageType::Leaf) {
        auto [idx, exact] = node.lower_bound(key);
        if (exact) {
            // Replace: drop the old cell, then insert in its place.
            node.remove_cell(idx);
        }
        if (Status s = node.insert_leaf_cell(idx, key, value); s) {
            pager_.mark_dirty(node_id);
            return Status::ok();
        }

        // Out of contiguous space is not the same as out of space. Reclaim the
        // bytes left by earlier deletes and replacements before deciding the
        // page is genuinely full — splitting a page that had room all along
        // makes the tree taller for nothing.
        node.compact();
        if (Status s = node.insert_leaf_cell(idx, key, value); s) {
            pager_.mark_dirty(node_id);
            return Status::ok();
        }

        Page right(nullptr, kNoPage);
        if (Status s = pager_.allocate(PageType::Leaf, &right); !s) {
            return s;
        }
        // allocate() can rehash the cache; re-fetch so `node` is certainly live.
        if (Status s = pager_.fetch(node_id, &node); !s) {
            return s;
        }
        if (Status s = split_leaf_page(node, right); !s) {
            return s;
        }

        const std::string separator = copy_of(right.key_at(0));
        Page& target = compare_keys(key, as_bytes(separator)) < 0 ? node : right;
        auto [pos, dup] = target.lower_bound(key);
        if (dup) {
            target.remove_cell(pos);
        }
        if (Status s = target.insert_leaf_cell(pos, key, value); !s) {
            return s;
        }

        pager_.mark_dirty(node_id);
        pager_.mark_dirty(right.id());
        *split = Split{separator, right.id()};
        return Status::ok();
    }

    // Internal page: descend, then adopt whatever the child hands back.
    const std::uint16_t slot = descend_slot(node, key);
    const PageId child_id = child_at(node, slot);

    std::optional<Split> child_split;
    if (Status s = insert_rec(child_id, key, value, &child_split); !s) {
        return s;
    }
    if (!child_split.has_value()) {
        return Status::ok();
    }

    if (Status s = pager_.fetch(node_id, &node); !s) {
        return s;
    }

    const Bytes sep = as_bytes(child_split->separator);
    if (Status s = place_separator(node, sep, child_id, child_split->right); s) {
        pager_.mark_dirty(node_id);
        return Status::ok();
    }

    node.compact();
    if (Status s = place_separator(node, sep, child_id, child_split->right); s) {
        pager_.mark_dirty(node_id);
        return Status::ok();
    }

    // No room for the separator: split this page too, then place it in
    // whichever half it now belongs to.
    Page right(nullptr, kNoPage);
    if (Status s = pager_.allocate(PageType::Internal, &right); !s) {
        return s;
    }
    if (Status s = pager_.fetch(node_id, &node); !s) {
        return s;
    }

    std::string median;
    if (Status s = split_internal_page(node, right, &median); !s) {
        return s;
    }

    Page& target = compare_keys(sep, as_bytes(median)) < 0 ? node : right;
    if (Status s = place_separator(target, sep, child_id, child_split->right); !s) {
        return s;
    }

    pager_.mark_dirty(node_id);
    pager_.mark_dirty(right.id());
    *split = Split{median, right.id()};
    return Status::ok();
}

Status BTree::insert(Bytes key, Bytes value) {
    if (key.empty()) {
        return Status::invalid_argument("empty key");
    }
    if (key.size() + value.size() > Page::max_payload()) {
        return Status::invalid_argument("payload exceeds max_payload; overflow pages are not "
                                        "implemented, see FUTURE.md");
    }

    std::optional<Split> split;
    if (Status s = insert_rec(pager_.root(), key, value, &split); !s) {
        return s;
    }
    if (!split.has_value()) {
        return Status::ok();
    }

    // The root split. A new one goes on top; the tree grows by exactly one
    // level, which is the only way it ever gets taller.
    const PageId old_root = pager_.root();
    Page new_root(nullptr, kNoPage);
    if (Status s = pager_.allocate(PageType::Internal, &new_root); !s) {
        return s;
    }
    if (Status s = new_root.insert_internal_cell(0, as_bytes(split->separator), old_root); !s) {
        return s;
    }
    new_root.set_extra(split->right);
    pager_.mark_dirty(new_root.id());
    pager_.set_root(new_root.id());
    return Status::ok();
}

// ---------------------------------------------------------------------------
// Lookup and delete
// ---------------------------------------------------------------------------

Status BTree::get(Bytes key, std::string* out) {
    PageId id = pager_.root();
    for (;;) {
        Page node(nullptr, id);
        if (Status s = pager_.fetch(id, &node); !s) {
            return s;
        }
        if (node.type() == PageType::Leaf) {
            auto [idx, exact] = node.lower_bound(key);
            if (!exact) {
                return Status::not_found("key not present");
            }
            *out = copy_of(node.leaf_cell(idx).value);
            return Status::ok();
        }
        id = child_at(node, descend_slot(node, key));
    }
}

Status BTree::remove(Bytes key) {
    PageId id = pager_.root();
    for (;;) {
        Page node(nullptr, id);
        if (Status s = pager_.fetch(id, &node); !s) {
            return s;
        }
        if (node.type() == PageType::Leaf) {
            auto [idx, exact] = node.lower_bound(key);
            if (!exact) {
                return Status::not_found("key not present");
            }
            node.remove_cell(idx);
            pager_.mark_dirty(id);
            return Status::ok();
        }
        id = child_at(node, descend_slot(node, key));
    }
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

Status BTree::Cursor::load_current() {
    Page leaf(nullptr, leaf_);
    if (Status s = pager_->fetch(leaf_, &leaf); !s) {
        valid_ = false;
        return s;
    }
    // Skip over leaves that have been emptied by deletes.
    while (index_ >= leaf.cell_count()) {
        const PageId next = leaf.extra();
        if (next == kNoPage) {
            valid_ = false;
            return Status::ok();
        }
        leaf_ = next;
        index_ = 0;
        if (Status s = pager_->fetch(leaf_, &leaf); !s) {
            valid_ = false;
            return s;
        }
    }
    const LeafCell cell = leaf.leaf_cell(index_);
    key_ = cell.key;
    value_ = cell.value;
    valid_ = true;
    return Status::ok();
}

Status BTree::Cursor::seek_first() {
    PageId id = pager_->root();
    for (;;) {
        Page node(nullptr, id);
        if (Status s = pager_->fetch(id, &node); !s) {
            return s;
        }
        if (node.type() == PageType::Leaf) {
            break;
        }
        id = node.cell_count() > 0 ? node.internal_cell(0).child : node.extra();
    }
    leaf_ = id;
    index_ = 0;
    return load_current();
}

Status BTree::Cursor::seek(Bytes key) {
    PageId id = pager_->root();
    for (;;) {
        Page node(nullptr, id);
        if (Status s = pager_->fetch(id, &node); !s) {
            return s;
        }
        if (node.type() == PageType::Leaf) {
            auto [idx, exact] = node.lower_bound(key);
            (void)exact;
            leaf_ = id;
            index_ = idx;
            return load_current();
        }
        id = child_at(node, descend_slot(node, key));
    }
}

Status BTree::Cursor::next() {
    if (!valid_) {
        return Status::not_found("cursor is exhausted");
    }
    ++index_;
    return load_current();
}

// ---------------------------------------------------------------------------
// Integrity
// ---------------------------------------------------------------------------

Status BTree::verify_subtree(PageId id, const Bytes* lower, const Bytes* upper, std::size_t depth,
                             std::size_t* leaf_depth, std::size_t* key_count) {
    Page node(nullptr, id);
    if (Status s = pager_.fetch(id, &node); !s) {
        return s;
    }

    const std::uint16_t n = node.cell_count();

    // Keys within a page must be strictly increasing.
    for (std::uint16_t i = 1; i < n; ++i) {
        if (compare_keys(node.key_at(static_cast<std::uint16_t>(i - 1)), node.key_at(i)) >= 0) {
            return Status::corruption("keys out of order on page " + std::to_string(id));
        }
    }
    // Every key must sit inside the range its parent promised.
    for (std::uint16_t i = 0; i < n; ++i) {
        const Bytes k = node.key_at(i);
        if (lower != nullptr && compare_keys(k, *lower) < 0) {
            return Status::corruption("key below lower bound on page " + std::to_string(id));
        }
        if (upper != nullptr && compare_keys(k, *upper) >= 0) {
            return Status::corruption("key at or above upper bound on page " + std::to_string(id));
        }
    }

    if (node.type() == PageType::Leaf) {
        if (*leaf_depth == 0) {
            *leaf_depth = depth;
        } else if (*leaf_depth != depth) {
            return Status::corruption("leaves at differing depths");
        }
        *key_count += n;
        return Status::ok();
    }

    if (node.type() != PageType::Internal) {
        return Status::corruption("unexpected page type on page " + std::to_string(id));
    }
    if (n == 0) {
        return Status::corruption("internal page " + std::to_string(id) + " has no separators");
    }

    // Child i covers [previous separator, separator i).
    for (std::uint16_t i = 0; i < n; ++i) {
        const Bytes sep = node.key_at(i);
        const Bytes prev = i == 0 ? Bytes{} : node.key_at(static_cast<std::uint16_t>(i - 1));
        const Bytes* child_lower = i == 0 ? lower : &prev;
        if (Status s = verify_subtree(node.internal_cell(i).child, child_lower, &sep, depth + 1,
                                      leaf_depth, key_count);
            !s) {
            return s;
        }
    }
    const Bytes last = node.key_at(static_cast<std::uint16_t>(n - 1));
    return verify_subtree(node.extra(), &last, upper, depth + 1, leaf_depth, key_count);
}

Status BTree::verify_integrity(std::size_t* key_count_out) {
    std::size_t leaf_depth = 0;
    std::size_t keys = 0;
    if (Status s = verify_subtree(pager_.root(), nullptr, nullptr, 1, &leaf_depth, &keys); !s) {
        return s;
    }

    // Every key must also be reachable by walking the sibling chain, which is
    // the path a range scan takes. A tree can be correct top-down and still
    // have a broken chain.
    Cursor c(pager_);
    std::size_t walked = 0;
    if (Status s = c.seek_first(); !s) {
        return s;
    }
    std::string previous;
    bool have_previous = false;
    while (c.valid()) {
        if (have_previous && compare_keys(as_bytes(previous), c.key()) >= 0) {
            return Status::corruption("sibling chain yields keys out of order");
        }
        previous = copy_of(c.key());
        have_previous = true;
        ++walked;
        if (Status s = c.next(); !s) {
            return s;
        }
    }
    if (walked != keys) {
        return Status::corruption("sibling chain reaches " + std::to_string(walked) + " of " +
                                  std::to_string(keys) + " keys");
    }

    if (key_count_out != nullptr) {
        *key_count_out = keys;
    }
    return Status::ok();
}

Status BTree::depth(std::size_t* out) {
    std::size_t d = 0;
    PageId id = pager_.root();
    for (;;) {
        Page node(nullptr, id);
        if (Status s = pager_.fetch(id, &node); !s) {
            return s;
        }
        ++d;
        if (node.type() == PageType::Leaf) {
            break;
        }
        id = node.cell_count() > 0 ? node.internal_cell(0).child : node.extra();
    }
    *out = d;
    return Status::ok();
}

} // namespace strata
