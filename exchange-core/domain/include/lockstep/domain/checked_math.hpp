#pragma once

#include <concepts>
#include <optional>

namespace lockstep::domain {

// Overflow-checked integer arithmetic for the few places where a product or an
// unbounded sum can occur (level totals, notionals). The builtins are constexpr
// on GCC and Clang, so these work in static_assert-based tests too.

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_add(T lhs, T rhs) noexcept {
    T result{};
    if (__builtin_add_overflow(lhs, rhs, &result)) {
        return std::nullopt;
    }
    return result;
}

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T lhs, T rhs) noexcept {
    T result{};
    if (__builtin_sub_overflow(lhs, rhs, &result)) {
        return std::nullopt;
    }
    return result;
}

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T lhs, T rhs) noexcept {
    T result{};
    if (__builtin_mul_overflow(lhs, rhs, &result)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace lockstep::domain
