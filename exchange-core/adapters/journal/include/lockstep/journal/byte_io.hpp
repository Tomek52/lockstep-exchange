#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

namespace lockstep::journal {

// Little-endian integer (de)serialisation for the journal format (ADR-0012).
//
// `if consteval` picks the implementation:
//  * during constant evaluation we assemble bytes with shifts - std::memcpy is
//    not constexpr - which lets format tests be static_asserts;
//  * at run time we use std::memcpy (+ byteswap on big-endian hosts), which
//    every compiler lowers to a single unaligned load/store even at -O0, where
//    the shift loop would stay a loop.

template <std::integral T>
constexpr void store_le(T value, std::span<std::byte, sizeof(T)> out) noexcept {
    using U = std::make_unsigned_t<T>;
    auto bits = static_cast<U>(value);
    if consteval {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out[i] = static_cast<std::byte>(static_cast<unsigned char>(bits >> (8U * i)));
        }
    } else {
        if constexpr (std::endian::native == std::endian::big) {
            bits = std::byteswap(bits);
        }
        std::memcpy(out.data(), &bits, sizeof(T));
    }
}

template <std::integral T>
[[nodiscard]] constexpr T load_le(std::span<const std::byte, sizeof(T)> in) noexcept {
    using U = std::make_unsigned_t<T>;
    U bits{};
    if consteval {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bits = static_cast<U>(
                bits | (static_cast<U>(std::to_integer<unsigned char>(in[i])) << (8U * i)));
        }
    } else {
        std::memcpy(&bits, in.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            bits = std::byteswap(bits);
        }
    }
    return static_cast<T>(bits);
}

}  // namespace lockstep::journal
