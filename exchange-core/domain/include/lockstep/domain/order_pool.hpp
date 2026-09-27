#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// An order resting on the book.
struct RestingOrder {
    OrderId id;
    TraderId trader;
    ClientOrderId client_order_id;
    Side side{Side::Buy};
    Price price;
    Quantity remaining;

    friend constexpr bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

/// Slot storage for the resting orders of one book, with an intrusive
/// doubly-linked list through each slot so a price level is a FIFO of slots.
///
/// Slots are addressed by index rather than pointer, so growing the backing
/// vector never invalidates a link. Freed slots are chained through `next`
/// and reused before the vector grows, and the vector never shrinks: once the
/// book has seen its peak depth, resting and cancelling do not allocate.
///
/// Single-writer like its book (ADR-0003); no synchronisation.
class OrderPool {
public:
    using Index = std::uint32_t;
    static constexpr Index npos = std::numeric_limits<Index>::max();

    struct Node {
        RestingOrder order;
        Index prev{npos};
        Index next{npos};
    };

    /// Stores `order` in a free slot (reusing one if available) with no links.
    [[nodiscard]] Index acquire(const RestingOrder& order) {
        if (free_head_ != npos) {
            const Index index = free_head_;
            free_head_ = nodes_[index].next;
            nodes_[index] = Node{.order = order};
            return index;
        }
        assert(nodes_.size() < npos && "order pool exhausted its index space");
        nodes_.push_back(Node{.order = order});
        return static_cast<Index>(nodes_.size() - 1);
    }

    /// Returns a slot to the free list. The caller must have unlinked it.
    void release(Index index) noexcept {
        nodes_[index].prev = npos;
        nodes_[index].next = free_head_;
        free_head_ = index;
    }

    template <typename Self>
    [[nodiscard]] auto& node(this Self& self, Index index) noexcept {
        return self.nodes_[index];
    }

    /// Number of slots ever allocated (live + free). Does not grow while the
    /// number of live orders stays at or below a previous peak.
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

private:
    std::vector<Node> nodes_;
    Index free_head_{npos};
};

}  // namespace lockstep::domain
