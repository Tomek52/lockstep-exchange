// journal-dump: prints a shard journal, a header line and then one line per
// record, for debugging and for diffing two runs (task 009).
//
//   journal-dump <shard-N.jnl>
//
// Exit status: 0 when the whole journal was read, 1 on the first error (after
// printing every record before it), 2 on a usage error. A torn tail is an
// error here too: the tool reports what is on disk and never repairs it; use
// recover_tail for that.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/format.hpp"

namespace {

using namespace lockstep;

constexpr int exit_journal_error = 1;
constexpr int exit_usage = 2;

[[nodiscard]] constexpr std::string_view name(domain::Side side) noexcept {
    switch (side) {
        case domain::Side::Buy:
            return "Buy";
        case domain::Side::Sell:
            return "Sell";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view name(domain::OrderType type) noexcept {
    switch (type) {
        case domain::OrderType::Limit:
            return "Limit";
        case domain::OrderType::Market:
            return "Market";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view name(domain::TimeInForce tif) noexcept {
    switch (tif) {
        case domain::TimeInForce::Gtc:
            return "Gtc";
        case domain::TimeInForce::Ioc:
            return "Ioc";
    }
    std::unreachable();
}

// One overload per command, each printing the whole command: the dump must
// show every input that replay will feed back (ADR-0004).
void print_command(const domain::NewOrder& c) {
    std::println("NewOrder trader={} instrument={} {} {} {}@{} {} client_order_id={}",
                 c.trader.value(), c.instrument.value(), name(c.side), name(c.type),
                 c.price.value(), c.quantity.value(), name(c.time_in_force),
                 c.client_order_id.value());
}

void print_command(const domain::CancelOrder& c) {
    std::println("CancelOrder trader={} instrument={} order_id={}", c.trader.value(),
                 c.instrument.value(), c.order_id.value());
}

void print_command(const domain::ModifyOrder& c) {
    std::println("ModifyOrder trader={} instrument={} order_id={} price={} quantity={}",
                 c.trader.value(), c.instrument.value(), c.order_id.value(), c.new_price.value(),
                 c.new_quantity.value());
}

void print_command(const domain::BlockTrader& c) {
    std::println("BlockTrader command_id={} trader={}", c.command_id.value(), c.trader.value());
}

void print_command(const domain::UnblockTrader& c) {
    std::println("UnblockTrader command_id={} trader={}", c.command_id.value(), c.trader.value());
}

void print_command(const domain::KillSwitch& c) {
    std::println("KillSwitch command_id={} engaged={}", c.command_id.value(), c.engaged);
}

void print_command(const domain::RiskLinkStatus& c) {
    std::println("RiskLinkStatus connected={}", c.connected);
}

void print_record(const domain::SequencedCommand& record) {
    std::print("seq={} ts={} ", record.sequence.value(), record.timestamp.value());
    std::visit([](const auto& command) { print_command(command); }, record.command);
}

/// Prints the header line if the file has a valid one; otherwise stays silent
/// and lets the reader report why.
void print_header(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::array<char, journal::file_header_size> raw{};
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    std::array<std::byte, journal::file_header_size> bytes{};
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto header = journal::decode_file_header(std::span<const std::byte>{bytes}.first(got));
    if (header) {
        std::println("journal version={} shard={} shard_count={} config_hash={:#018x}",
                     header->version, header->shard.value(), header->shard_count,
                     header->config_hash);
    }
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc != 2) {
        std::println(stderr, "usage: journal-dump <shard journal file>");
        return exit_usage;
    }
    const std::filesystem::path path{
        argv[1]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    print_header(path);
    std::uint64_t records = 0;
    for (const auto& element : journal::read_journal(path)) {
        if (!element) {
            // Records first, then the error, also when both go to one pipe.
            (void)std::fflush(stdout);
            std::println(stderr, "journal-dump: {}: {} after {} record(s)", path.string(),
                         app::to_string(element.error()), records);
            return exit_journal_error;
        }
        print_record(*element);
        ++records;
    }
    return 0;
} catch (const std::exception& e) {
    // Allocation or stream failures; never let them escape main.
    (void)std::fputs("journal-dump: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputs("\n", stderr);
    return exit_journal_error;
} catch (...) {
    return exit_journal_error;
}
