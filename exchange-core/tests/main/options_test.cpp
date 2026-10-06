// exchange-core's command-line parser (options.hpp). Task 010 review: an
// option that takes no value must reject one anyway, rather than silently
// ignoring it.
#include "options.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include <gtest/gtest.h>

namespace lockstep::main_app {
namespace {

std::expected<Options, std::string> parse(std::span<const char*> args) {
    // parse_options takes char* const (argv's own type); the literals below
    // are never written through, so the cast is safe for this test only.
    return parse_options(std::span<char* const>(  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        const_cast<char**>(args.data()), args.size()));
}

TEST(ParseOptions, PrintDigestOnExitTakesNoValue) {
    std::array args{"exchange-core", "--print-digest-on-exit"};
    const auto options = parse(args);
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_TRUE(options->print_digest_on_exit);
}

TEST(ParseOptions, PrintDigestOnExitRejectsAValue) {
    std::array args{"exchange-core", "--print-digest-on-exit=anything"};
    const auto options = parse(args);
    ASSERT_FALSE(options.has_value());
    EXPECT_NE(options.error().find("--print-digest-on-exit"), std::string::npos);
}

}  // namespace
}  // namespace lockstep::main_app
