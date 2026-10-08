// Fuzzes the inbound risk trust boundary: arbitrary bytes -> protobuf parse ->
// codec decode of a RiskCommand. Properties checked (beyond "no crash, no UB"
// from ASan/UBSan):
//  * a successful decode never yields an out-of-range domain command variant;
//  * decoding is a pure function of the message (decode twice -> same result).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <variant>

#include "lockstep/codec/order_entry_codec.hpp"

namespace {

void check(bool condition) {
    if (!condition) {
        std::abort();  // libFuzzer reports the input that got here
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    lockstep::v1::RiskCommand command;
    if (!command.ParseFromArray(data, static_cast<int>(size))) {
        return 0;
    }
    const auto first = lockstep::codec::decode(command);
    const auto second = lockstep::codec::decode(command);
    check(first.has_value() == second.has_value());
    if (first) {
        check(*first == *second);
        // A decoded risk command is always one of the three risk alternatives.
        const bool is_risk = std::holds_alternative<lockstep::domain::BlockTrader>(*first) ||
                             std::holds_alternative<lockstep::domain::UnblockTrader>(*first) ||
                             std::holds_alternative<lockstep::domain::KillSwitch>(*first);
        check(is_risk);
    } else {
        check(first.error() == second.error());
    }
    return 0;
}
