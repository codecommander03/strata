#pragma once

#include "strata/page.hpp"
#include "strata/pager.hpp"
#include "strata/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace strata {

/// A B+tree over the Pager.
///
/// Keys appear in internal pages only as separators; every key/value pair lives
/// in a leaf, and leaves are chained left to right so a range scan never touches
/// an internal page after the initial descent.
///
/// The ordering invariant that every descent and every split depends on:
///
///   for an internal page with cells (k0,c0) .. (kn-1,cn-1) and rightmost R,
///   child ci holds keys strictly less than ki, and R holds keys >= kn-1.
///
/// Equal keys therefore go right, which is why a descent uses an upper bound
/// rather than a lower bound.
///
/// Splits propagate bottom-up by return value rather than by re-descending:
/// insert_rec hands its caller an optional Split, and the caller places the
/// separator. That keeps every page on the path fetched exactly once, and means
/// a cascading split up a tall tree is still one pass. See decision 006.
class BTree {
public:
    explicit BTree(Pager& pager) : pager_(pager) {}

    /// Inserts, or replaces the value if the key is already present.
    Status insert(Bytes key, Bytes value);

    /// Copies the value out. NotFound if the key is absent.
    Status get(Bytes key, std::string* out);

    /// Removes the key. NotFound if it is absent. Pages are not merged when
    /// they empty — see decision 004.
    Status remove(Bytes key);

    /// Walks leaves left to right. Invalidated by any insert or remove.
    class Cursor {
    public:
        explicit Cursor(Pager& pager) : pager_(&pager) {}

        bool valid() const { return valid_; }
        Status seek_first();
        Status seek(Bytes key);
        Status next();

        Bytes key() const { return key_; }
        Bytes value() const { return value_; }

    private:
        Status load_current();

        Pager* pager_;
        PageId leaf_ = kNoPage;
        std::uint16_t index_ = 0;
        bool valid_ = false;
        Bytes key_;
        Bytes value_;
    };

    Cursor cursor() { return Cursor(pager_); }

    /// Walks the whole tree and checks every invariant: page types, key order
    /// within a page, the separator bounds between a parent and each child, and
    /// that all leaves sit at the same depth. Used by the tests and by the
    /// crash harness after recovery.
    Status verify_integrity(std::size_t* key_count_out = nullptr);

    /// Levels from root to leaf. One for a tree that is a single leaf.
    Status depth(std::size_t* out);

private:
    /// A page that split, and the separator its parent must adopt.
    struct Split {
        std::string separator;
        PageId right;
    };

    Status insert_rec(PageId node_id, Bytes key, Bytes value, std::optional<Split>* split);

    /// Moves the upper half of a leaf into `right` and relinks the sibling
    /// chain. Both pages are marked dirty.
    Status split_leaf_page(Page& leaf, Page& right);

    /// Moves the cells above the midpoint into `right`, promotes the midpoint
    /// key, and rehomes the midpoint's child as the left page's new rightmost.
    Status split_internal_page(Page& node, Page& right, std::string* median);

    /// Inserts a separator whose left child is `left_child`, repointing
    /// whatever followed it at `right_child`. Full if the page has no room.
    static Status place_separator(Page& node, Bytes separator, PageId left_child,
                                  PageId right_child);

    Status verify_subtree(PageId id, const Bytes* lower, const Bytes* upper, std::size_t depth,
                          std::size_t* leaf_depth, std::size_t* key_count);

    Pager& pager_;
};

} // namespace strata
