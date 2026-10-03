#include "lockstep/journal/record_codec.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace lockstep::journal {

namespace {

using app::JournalError;

// ---- Encoding --------------------------------------------------------------

class PayloadWriter {
public:
    explicit PayloadWriter(std::vector<std::byte>& out) noexcept : out_{out} {}

    template <std::integral T>
    void put(T value) {
        std::array<std::byte, sizeof(T)> bytes{};
        store_le(value, std::span{bytes});
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    void put_bool(bool value) { put(static_cast<std::uint8_t>(value ? 1 : 0)); }

private:
    std::vector<std::byte>& out_;
};

// Explicit byte values (record_codec.hpp), independent of the enumerators'
// C++ values, so reordering an enum cannot change the format.
std::uint8_t side_byte(domain::Side side) noexcept {
    switch (side) {
        case domain::Side::Buy:
            return 0;
        case domain::Side::Sell:
            return 1;
    }
    std::unreachable();
}

std::uint8_t order_type_byte(domain::OrderType type) noexcept {
    switch (type) {
        case domain::OrderType::Limit:
            return 0;
        case domain::OrderType::Market:
            return 1;
    }
    std::unreachable();
}

std::uint8_t time_in_force_byte(domain::TimeInForce tif) noexcept {
    switch (tif) {
        case domain::TimeInForce::Gtc:
            return 0;
        case domain::TimeInForce::Ioc:
            return 1;
    }
    std::unreachable();
}

void encode_fields(const domain::NewOrder& c, PayloadWriter& w) {
    w.put(c.trader.value());
    w.put(c.client_order_id.value());
    w.put(c.instrument.value());
    w.put(side_byte(c.side));
    w.put(order_type_byte(c.type));
    w.put(time_in_force_byte(c.time_in_force));
    w.put(c.price.value());
    w.put(c.quantity.value());
}

void encode_fields(const domain::CancelOrder& c, PayloadWriter& w) {
    w.put(c.trader.value());
    w.put(c.instrument.value());
    w.put(c.order_id.value());
}

void encode_fields(const domain::ModifyOrder& c, PayloadWriter& w) {
    w.put(c.trader.value());
    w.put(c.instrument.value());
    w.put(c.order_id.value());
    w.put(c.new_price.value());
    w.put(c.new_quantity.value());
}

void encode_fields(const domain::BlockTrader& c, PayloadWriter& w) {
    w.put(c.command_id.value());
    w.put(c.trader.value());
}

void encode_fields(const domain::UnblockTrader& c, PayloadWriter& w) {
    w.put(c.command_id.value());
    w.put(c.trader.value());
}

void encode_fields(const domain::KillSwitch& c, PayloadWriter& w) {
    w.put(c.command_id.value());
    w.put_bool(c.engaged);
}

void encode_fields(const domain::RiskLinkStatus& c, PayloadWriter& w) {
    w.put_bool(c.connected);
}

// ---- Decoding --------------------------------------------------------------

/// Bounds-checked cursor over a payload. Every read returns nullopt instead of
/// reading past the end, which is what makes decode_payload total.
class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

    template <std::integral T>
    [[nodiscard]] std::optional<T> get() noexcept {
        if (bytes_.size() < sizeof(T)) {
            return std::nullopt;
        }
        const T value = load_le<T>(bytes_.first<sizeof(T)>());
        bytes_ = bytes_.subspan(sizeof(T));
        return value;
    }

    [[nodiscard]] std::optional<bool> get_bool() noexcept {
        const auto byte = get<std::uint8_t>();
        if (!byte || *byte > 1) {
            return std::nullopt;
        }
        return *byte == 1;
    }

    [[nodiscard]] bool at_end() const noexcept { return bytes_.empty(); }

private:
    std::span<const std::byte> bytes_;
};

std::optional<domain::Side> side_from(std::optional<std::uint8_t> byte) noexcept {
    if (!byte) {
        return std::nullopt;
    }
    switch (*byte) {
        case 0:
            return domain::Side::Buy;
        case 1:
            return domain::Side::Sell;
        default:  // arbitrary input byte, not an enum: other values are corruption
            return std::nullopt;
    }
}

std::optional<domain::OrderType> order_type_from(std::optional<std::uint8_t> byte) noexcept {
    if (!byte) {
        return std::nullopt;
    }
    switch (*byte) {
        case 0:
            return domain::OrderType::Limit;
        case 1:
            return domain::OrderType::Market;
        default:  // arbitrary input byte, not an enum: other values are corruption
            return std::nullopt;
    }
}

std::optional<domain::TimeInForce> time_in_force_from(std::optional<std::uint8_t> byte) noexcept {
    if (!byte) {
        return std::nullopt;
    }
    switch (*byte) {
        case 0:
            return domain::TimeInForce::Gtc;
        case 1:
            return domain::TimeInForce::Ioc;
        default:  // arbitrary input byte, not an enum: other values are corruption
            return std::nullopt;
    }
}

// Each decoder reads its fields in the order encode_fields wrote them. The
// reads are sequenced statements, not function arguments, whose evaluation
// order C++ leaves unspecified.

std::optional<domain::Command> decode_new_order(PayloadReader& r) noexcept {
    const auto trader = r.get<std::uint64_t>();
    const auto client_order_id = r.get<std::uint64_t>();
    const auto instrument = r.get<std::uint32_t>();
    const auto side = side_from(r.get<std::uint8_t>());
    const auto type = order_type_from(r.get<std::uint8_t>());
    const auto tif = time_in_force_from(r.get<std::uint8_t>());
    const auto price = r.get<std::int64_t>();
    const auto quantity = r.get<std::uint64_t>();
    if (!trader || !client_order_id || !instrument || !side || !type || !tif || !price ||
        !quantity) {
        return std::nullopt;
    }
    return domain::NewOrder{.trader = domain::TraderId{*trader},
                            .client_order_id = domain::ClientOrderId{*client_order_id},
                            .instrument = domain::InstrumentId{*instrument},
                            .side = *side,
                            .type = *type,
                            .time_in_force = *tif,
                            .price = domain::Price{*price},
                            .quantity = domain::Quantity{*quantity}};
}

std::optional<domain::Command> decode_cancel_order(PayloadReader& r) noexcept {
    const auto trader = r.get<std::uint64_t>();
    const auto instrument = r.get<std::uint32_t>();
    const auto order_id = r.get<std::uint64_t>();
    if (!trader || !instrument || !order_id) {
        return std::nullopt;
    }
    return domain::CancelOrder{.trader = domain::TraderId{*trader},
                               .instrument = domain::InstrumentId{*instrument},
                               .order_id = domain::OrderId{*order_id}};
}

std::optional<domain::Command> decode_modify_order(PayloadReader& r) noexcept {
    const auto trader = r.get<std::uint64_t>();
    const auto instrument = r.get<std::uint32_t>();
    const auto order_id = r.get<std::uint64_t>();
    const auto new_price = r.get<std::int64_t>();
    const auto new_quantity = r.get<std::uint64_t>();
    if (!trader || !instrument || !order_id || !new_price || !new_quantity) {
        return std::nullopt;
    }
    return domain::ModifyOrder{.trader = domain::TraderId{*trader},
                               .instrument = domain::InstrumentId{*instrument},
                               .order_id = domain::OrderId{*order_id},
                               .new_price = domain::Price{*new_price},
                               .new_quantity = domain::Quantity{*new_quantity}};
}

template <typename TraderCommand>
std::optional<domain::Command> decode_trader_command(PayloadReader& r) noexcept {
    const auto command_id = r.get<std::uint64_t>();
    const auto trader = r.get<std::uint64_t>();
    if (!command_id || !trader) {
        return std::nullopt;
    }
    return TraderCommand{.command_id = domain::RiskCommandId{*command_id},
                         .trader = domain::TraderId{*trader}};
}

std::optional<domain::Command> decode_kill_switch(PayloadReader& r) noexcept {
    const auto command_id = r.get<std::uint64_t>();
    const auto engaged = r.get_bool();
    if (!command_id || !engaged) {
        return std::nullopt;
    }
    return domain::KillSwitch{.command_id = domain::RiskCommandId{*command_id},
                              .engaged = *engaged};
}

std::optional<domain::Command> decode_risk_link_status(PayloadReader& r) noexcept {
    const auto connected = r.get_bool();
    if (!connected) {
        return std::nullopt;
    }
    return domain::RiskLinkStatus{.connected = *connected};
}

std::optional<domain::Command> decode_command(std::uint8_t tag, PayloadReader& r) noexcept {
    // Switch over the raw byte, not CommandTag: the tag comes from disk and may
    // be any value, and casting an arbitrary byte to the enum first would hide
    // that. The cases still name the CommandTag values (ADR-0012).
    switch (tag) {
        case std::to_underlying(domain::CommandTag::NewOrder):
            return decode_new_order(r);
        case std::to_underlying(domain::CommandTag::CancelOrder):
            return decode_cancel_order(r);
        case std::to_underlying(domain::CommandTag::ModifyOrder):
            return decode_modify_order(r);
        case std::to_underlying(domain::CommandTag::BlockTrader):
            return decode_trader_command<domain::BlockTrader>(r);
        case std::to_underlying(domain::CommandTag::UnblockTrader):
            return decode_trader_command<domain::UnblockTrader>(r);
        case std::to_underlying(domain::CommandTag::KillSwitch):
            return decode_kill_switch(r);
        case std::to_underlying(domain::CommandTag::RiskLinkStatus):
            return decode_risk_link_status(r);
        default:  // unknown tag: corruption, or a newer format (caught by version first)
            return std::nullopt;
    }
}

}  // namespace

void encode_payload(const domain::SequencedCommand& command, std::vector<std::byte>& out) {
    PayloadWriter writer{out};
    writer.put(command.sequence.value());
    writer.put(command.timestamp.value());
    std::visit(
        [&writer](const auto& alternative) {
            writer.put(std::to_underlying(alternative.tag));
            encode_fields(alternative, writer);
        },
        command.command);
}

std::expected<domain::SequencedCommand, app::JournalError> decode_payload(
    std::span<const std::byte> payload) noexcept {
    PayloadReader reader{payload};
    const auto sequence = reader.get<std::uint64_t>();
    const auto timestamp = reader.get<std::int64_t>();
    const auto tag = reader.get<std::uint8_t>();
    if (!sequence || !timestamp || !tag) {
        return std::unexpected(JournalError::Corrupt);
    }
    auto command = decode_command(*tag, reader);
    if (!command || !reader.at_end()) {
        return std::unexpected(JournalError::Corrupt);
    }
    return domain::SequencedCommand{.sequence = domain::SequenceNumber{*sequence},
                                    .timestamp = domain::Timestamp{*timestamp},
                                    .command = std::move(*command)};
}

}  // namespace lockstep::journal
