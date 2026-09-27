#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <functional>

namespace lockstep::domain {

/// A distinct integer type. `Tag` (an incomplete marker type such as
/// `struct PriceTag`) makes every instantiation unique, so a Price
/// never converts to, compares with, or adds to an OrderId by accident.
///
/// Operations beyond comparison are opt-in via mixins (see Additive) that use
/// C++23 explicit object parameters ("deducing this") instead of CRTP: the mixin
/// needs no template parameter naming the derived type, and the result type of
/// `a + b` is deduced as the most-derived type (Price, not StrongInt<...>).
template <typename Tag, std::integral Rep>
class StrongInt {
public:
    using rep = Rep;

    constexpr StrongInt() noexcept = default;
    constexpr explicit StrongInt(Rep value) noexcept : value_{value} {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr bool operator==(StrongInt, StrongInt) noexcept = default;
    friend constexpr auto operator<=>(StrongInt, StrongInt) noexcept = default;

private:
    Rep value_{};
};

/// Opt-in addition/subtraction for quantities and prices.
///
/// Unchecked by design: inputs are range-validated at the domain boundary
/// (InstrumentSpec limits), which bounds every sum the matching engine forms.
/// Use checked_math.hpp where a product or an unbounded sum can occur.
struct Additive {
    template <typename Self>
    [[nodiscard]] constexpr Self operator+(this Self lhs, Self rhs) noexcept {
        return Self{static_cast<typename Self::rep>(lhs.value() + rhs.value())};
    }

    template <typename Self>
    [[nodiscard]] constexpr Self operator-(this Self lhs, Self rhs) noexcept {
        return Self{static_cast<typename Self::rep>(lhs.value() - rhs.value())};
    }

    template <typename Self>
    constexpr Self& operator+=(this Self& lhs, Self rhs) noexcept {
        lhs = lhs + rhs;
        return lhs;
    }

    template <typename Self>
    constexpr Self& operator-=(this Self& lhs, Self rhs) noexcept {
        lhs = lhs - rhs;
        return lhs;
    }
};

/// Hash functor for any StrongInt-derived type (use as the Hash parameter of
/// unordered containers). Iteration order of hashed containers must never
/// influence domain output; see CLAUDE.md "determinism rules".
struct StrongIntHash {
    template <typename T>
        requires requires(T t) {
            typename T::rep;
            { t.value() } -> std::same_as<typename T::rep>;
        }
    [[nodiscard]] std::size_t operator()(T v) const noexcept {
        return std::hash<typename T::rep>{}(v.value());
    }
};

}  // namespace lockstep::domain
