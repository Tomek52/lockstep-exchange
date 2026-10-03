#include "lockstep/journal/crc32c.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace lockstep::journal {
namespace {

constexpr auto ascii(std::string_view text) {
    std::array<std::byte, 9> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

constexpr auto check_input = ascii("123456789");

// The standard CRC-32C (Castagnoli) check value, verified by the compiler.
static_assert(crc32c(check_input) == 0xE3069283U);
static_assert(crc32c(std::span<const std::byte>{}) == 0U);

TEST(Crc32c, SeedChainsPartialComputations) {
    const std::span<const std::byte> all{check_input};
    const auto first = crc32c(all.first(4));
    EXPECT_EQ(crc32c(all.subspan(4), first), crc32c(all));
}

TEST(Crc32c, DetectsSingleBitFlip) {
    auto flipped = check_input;
    flipped[3] ^= std::byte{0x10};
    EXPECT_NE(crc32c(flipped), crc32c(check_input));
}

}  // namespace
}  // namespace lockstep::journal
