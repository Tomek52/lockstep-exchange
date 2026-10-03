#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lockstep::journal {

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), the record checksum
// of ADR-0012. Table-driven and constexpr, so the check value is a
// static_assert. Hardware CRC (SSE4.2) is a possible later optimisation: the
// hot path checksums one record of a few dozen bytes per command, which is
// noise next to the write(2) per batch.

namespace detail {

inline constexpr std::uint32_t crc32c_polynomial = 0x82F6'3B78U;

[[nodiscard]] consteval std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ crc32c_polynomial : crc >> 1U;
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> crc32c_table = make_crc32c_table();

}  // namespace detail

/// CRC-32C of `data`. `seed` is the result of a previous call, so a checksum
/// can be computed over several spans: crc32c(b, crc32c(a)) == crc32c(a ++ b).
[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data,
                                             std::uint32_t seed = 0) noexcept {
    std::uint32_t crc = ~seed;
    for (const std::byte byte : data) {
        crc = detail::crc32c_table[(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU] ^
              (crc >> 8U);
    }
    return ~crc;
}

}  // namespace lockstep::journal
