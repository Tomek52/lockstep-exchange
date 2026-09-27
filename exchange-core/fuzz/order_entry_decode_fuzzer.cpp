// Fuzzes the trust boundary: arbitrary bytes -> protobuf parse -> codec decode.
// Properties checked (beyond "no crash, no UB" from ASan/UBSan):
//  * a successful decode never yields an out-of-range domain enum;
//  * decoding is a pure function of the message (decode twice -> same result).
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "lockstep/codec/order_entry_codec.hpp"

namespace {

void check(bool condition) {
    if (!condition) {
        std::abort();  // libFuzzer reports the input that got here
    }
}

bool valid(lockstep::domain::Side side) {
    return side == lockstep::domain::Side::Buy || side == lockstep::domain::Side::Sell;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    lockstep::v1::SubmitOrderRequest request;
    if (!request.ParseFromArray(data, static_cast<int>(size))) {
        return 0;
    }
    const auto first = lockstep::codec::decode(request);
    const auto second = lockstep::codec::decode(request);
    check(first.has_value() == second.has_value());
    if (first) {
        check(*first == *second);
        check(valid(first->side));
        check(first->quantity.value() == request.quantity());
        check(first->price.value() == request.price_ticks());
        if (first->type == lockstep::domain::OrderType::Market) {
            check(first->time_in_force == lockstep::domain::TimeInForce::Ioc);
        }
    } else {
        check(first.error() == second.error());
    }
    return 0;
}
