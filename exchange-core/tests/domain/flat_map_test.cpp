#include "lockstep/domain/flat_map.hpp"

#include <functional>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace lockstep::domain {
namespace {

using Map = detail::sorted_vector_map<int, std::string>;

// The fallback must be usable with ranges algorithms and views, like std::flat_map.
static_assert(std::bidirectional_iterator<Map::iterator>);
static_assert(std::bidirectional_iterator<Map::const_iterator>);
static_assert(std::ranges::bidirectional_range<Map>);
static_assert(std::convertible_to<Map::iterator, Map::const_iterator>);

TEST(FlatMapFallback, KeepsKeysSortedByComparator) {
    detail::sorted_vector_map<int, int, std::greater<>> desc;
    for (const int k : {3, 1, 4, 1, 5, 9, 2, 6}) {
        desc.try_emplace(k, k * 10);
    }
    const auto keys = desc | std::views::transform([](const auto& kv) { return kv.first; }) |
                      std::ranges::to<std::vector>();
    EXPECT_EQ(keys, (std::vector<int>{9, 6, 5, 4, 3, 2, 1}));
}

TEST(FlatMapFallback, TryEmplaceDoesNotOverwrite) {
    Map m;
    EXPECT_TRUE(m.try_emplace(1, "a").second);
    const auto [it, inserted] = m.try_emplace(1, "b");
    EXPECT_FALSE(inserted);
    EXPECT_EQ(it->second, "a");
}

TEST(FlatMapFallback, FindEraseAndSubscript) {
    Map m;
    m[2] = "two";
    m[1] = "one";
    m[3] = "three";
    ASSERT_NE(m.find(2), m.end());
    EXPECT_EQ(m.find(2)->second, "two");
    EXPECT_EQ(m.find(7), m.end());

    EXPECT_EQ(m.erase(2), 1U);
    EXPECT_EQ(m.erase(2), 0U);
    EXPECT_FALSE(m.contains(2));
    EXPECT_EQ(m.size(), 2U);

    const auto next = m.erase(m.find(1));
    EXPECT_EQ(next->first, 3);
}

TEST(FlatMapFallback, ConstAccessYieldsConstIterator) {
    Map m;
    m[1] = "x";
    const Map& cm = m;
    static_assert(std::same_as<decltype(cm.find(1)), Map::const_iterator>);
    static_assert(std::same_as<decltype(m.find(1)), Map::iterator>);
    auto&& [key, value] = *cm.begin();
    static_assert(std::same_as<decltype(value), const std::string&>);
    EXPECT_EQ(key, 1);
}

TEST(FlatMapAlias, ReportsWhichImplementationIsActive) {
    // Documents the toolchain in test output; both answers are valid (ADR-0009).
    RecordProperty("uses_std_flat_map", uses_std_flat_map ? "true" : "false");
    flat_map<int, int> m;
    m[1] = 1;
    EXPECT_EQ(m.size(), 1U);
}

}  // namespace
}  // namespace lockstep::domain
