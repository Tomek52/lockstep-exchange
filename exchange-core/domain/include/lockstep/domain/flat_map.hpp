#pragma once

// lockstep::domain::flat_map selects std::flat_map when the standard library
// provides it and falls back to detail::sorted_vector_map otherwise (ADR-0009).
// libstdc++ 14 (our GCC 14 / Clang 19 baseline) has no <flat_map>; libstdc++ 15
// does, and the switch happens automatically via the feature-test macro.
//
// Portability rules for callers (both implementations satisfy them):
//  * dereferencing an iterator yields a proxy pair<const Key&, T&>; bind it with
//    `auto&& [key, value] = *it` or `const auto& [key, value] = *it`, never `auto&`;
//  * use only: begin/end, find, lower_bound, contains, try_emplace, operator[],
//    erase(iterator), erase(key), size, empty, clear.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#if __has_include(<flat_map>)
#include <flat_map>
#endif

namespace lockstep::domain {
namespace detail {

// Names follow the standard library (it stands in for std::flat_map).
// NOLINTBEGIN(readability-identifier-naming)

/// Sorted parallel vectors of keys and values - the same layout std::flat_map
/// uses: key searches touch only the dense key array, which is what makes a
/// flat map beat a node-based std::map for a handful of hot price levels.
template <typename Key, typename T, typename Compare = std::less<Key>>
class sorted_vector_map {
public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<Key, T>;
    using key_compare = Compare;
    using size_type = std::size_t;

    template <bool Const>
    class basic_iterator {
        using map_ptr = std::conditional_t<Const, const sorted_vector_map*, sorted_vector_map*>;
        using mapped_ref = std::conditional_t<Const, const T&, T&>;

    public:
        using iterator_concept = std::bidirectional_iterator_tag;
        using iterator_category = std::input_iterator_tag;  // proxy reference, like std::flat_map
        using value_type = std::pair<Key, T>;
        using difference_type = std::ptrdiff_t;
        using reference = std::pair<const Key&, mapped_ref>;

        struct arrow_proxy {
            reference ref;
            const reference* operator->() const noexcept { return &ref; }
        };
        using pointer = arrow_proxy;

        basic_iterator() = default;
        basic_iterator(map_ptr map, size_type index) noexcept : map_{map}, index_{index} {}

        // iterator -> const_iterator, as for standard containers.
        // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
        operator basic_iterator<true>() const noexcept
            requires(!Const)
        {
            return {map_, index_};
        }

        reference operator*() const noexcept {
            return {map_->keys_[index_], map_->values_[index_]};
        }
        arrow_proxy operator->() const noexcept { return {**this}; }

        basic_iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        basic_iterator operator++(int) noexcept {
            auto copy = *this;
            ++index_;
            return copy;
        }
        basic_iterator& operator--() noexcept {
            --index_;
            return *this;
        }
        basic_iterator operator--(int) noexcept {
            auto copy = *this;
            --index_;
            return copy;
        }

        friend bool operator==(const basic_iterator&, const basic_iterator&) = default;

    private:
        friend class sorted_vector_map;
        map_ptr map_ = nullptr;
        size_type index_ = 0;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    // Deducing this: one definition serves const and non-const objects and
    // returns const_iterator / iterator accordingly.
    template <typename Self>
    [[nodiscard]] auto begin(this Self& self) noexcept {
        return self.make_iterator(0);
    }

    template <typename Self>
    [[nodiscard]] auto end(this Self& self) noexcept {
        return self.make_iterator(self.keys_.size());
    }

    template <typename Self>
    [[nodiscard]] auto lower_bound(this Self& self, const Key& key) {
        return self.make_iterator(self.lower_index(key));
    }

    template <typename Self>
    [[nodiscard]] auto find(this Self& self, const Key& key) {
        const size_type index = self.lower_index(key);
        if (index != self.keys_.size() && !self.comp_(key, self.keys_[index])) {
            return self.make_iterator(index);
        }
        return self.end();
    }

    [[nodiscard]] bool contains(const Key& key) const { return find(key) != end(); }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args) {
        const size_type index = lower_index(key);
        if (index != keys_.size() && !comp_(key, keys_[index])) {
            return {make_iterator(index), false};
        }
        keys_.insert(keys_.begin() + offset(index), key);
        try {
            values_.emplace(values_.begin() + offset(index), std::forward<Args>(args)...);
        } catch (...) {
            keys_.erase(keys_.begin() + offset(index));  // keep keys_/values_ in lockstep
            throw;
        }
        return {make_iterator(index), true};
    }

    T& operator[](const Key& key) { return values_[try_emplace(key).first.index_]; }

    iterator erase(const_iterator pos) {
        keys_.erase(keys_.begin() + offset(pos.index_));
        values_.erase(values_.begin() + offset(pos.index_));
        return make_iterator(pos.index_);
    }

    size_type erase(const Key& key) {
        const auto it = find(key);
        if (it == end()) {
            return 0;
        }
        erase(it);
        return 1;
    }

    [[nodiscard]] size_type size() const noexcept { return keys_.size(); }
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
    [[nodiscard]] key_compare key_comp() const { return comp_; }

    void clear() noexcept {
        keys_.clear();
        values_.clear();
    }

private:
    template <typename Self>
    [[nodiscard]] auto make_iterator(this Self& self, size_type index) noexcept {
        return basic_iterator<std::is_const_v<Self>>{&self, index};
    }

    [[nodiscard]] size_type lower_index(const Key& key) const {
        return static_cast<size_type>(std::ranges::lower_bound(keys_, key, comp_) - keys_.begin());
    }

    [[nodiscard]] static std::ptrdiff_t offset(size_type index) noexcept {
        return static_cast<std::ptrdiff_t>(index);
    }

    std::vector<Key> keys_;
    std::vector<T> values_;
    [[no_unique_address]] Compare comp_{};
};

// NOLINTEND(readability-identifier-naming)

}  // namespace detail

#if defined(__cpp_lib_flat_map) && __cpp_lib_flat_map >= 202207L
template <typename Key, typename T, typename Compare = std::less<Key>>
using flat_map = std::flat_map<Key, T, Compare>;
inline constexpr bool uses_std_flat_map = true;
#else
template <typename Key, typename T, typename Compare = std::less<Key>>
using flat_map = detail::sorted_vector_map<Key, T, Compare>;
inline constexpr bool uses_std_flat_map = false;
#endif

}  // namespace lockstep::domain
