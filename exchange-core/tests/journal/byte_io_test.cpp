#include "lockstep/journal/byte_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace lockstep::journal {
namespace {

template <typename T>
constexpr std::array<std::byte, sizeof(T)> bytes_of(T value) {
    std::array<std::byte, sizeof(T)> out{};
    store_le(value, std::span{out});
    return out;
}

template <typename T>
constexpr T round_trip(T value) {
    const auto bytes = bytes_of(value);
    return load_le<T>(std::span{bytes});
}

// Compile-time path.
static_assert(round_trip<std::int64_t>(-2) == -2);
static_assert(round_trip(std::numeric_limits<std::uint64_t>::max()) ==
              std::numeric_limits<std::uint64_t>::max());
static_assert(bytes_of<std::uint32_t>(0x0A0B0C0D)[0] == std::byte{0x0D});

TEST(ByteIo, RuntimePathMatchesCompileTimePath) {
    // Same inputs through the memcpy path (runtime) must give the same bytes
    // the shift path (constant evaluation) produced.
    constexpr auto expected = bytes_of<std::int64_t>(-1234567890123);
    volatile std::int64_t input = -1234567890123;  // force runtime evaluation
    const auto actual = bytes_of<std::int64_t>(input);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(round_trip<std::int64_t>(input), -1234567890123);
}

TEST(ByteIo, SmallTypes) {
    volatile std::uint16_t input = 0xBEEF;
    const auto bytes = bytes_of<std::uint16_t>(input);
    EXPECT_EQ(bytes[0], std::byte{0xEF});
    EXPECT_EQ(bytes[1], std::byte{0xBE});
}

}  // namespace
}  // namespace lockstep::journal
