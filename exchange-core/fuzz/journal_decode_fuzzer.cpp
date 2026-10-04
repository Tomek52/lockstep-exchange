// Fuzzes the journal's decoders with arbitrary bytes (ADR-0012). Both are
// documented as total: any input yields a value or a JournalError, never UB.
// Properties checked beyond "no crash, no UB" from ASan/UBSan:
//  * decode_payload is a pure function, and a payload it accepts re-encodes to
//    exactly the same bytes (one canonical encoding per command);
//  * parse_record on a buffer never consumes more than the buffer, and a
//    record it accepts is a well-formed record that decodes the same way again;
//  * a payload wrapped in a correct record header (random bytes almost never
//    carry a valid CRC, so without this the fuzzer would not get past it) is
//    accepted by parse_record exactly when decode_payload accepts it;
//  * walking a buffer record by record terminates.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"

namespace {

using namespace lockstep;

void check(bool condition) {
    if (!condition) {
        std::abort();  // libFuzzer reports the input that got here
    }
}

void fuzz_payload(std::span<const std::byte> bytes) {
    const auto first = journal::decode_payload(bytes);
    const auto second = journal::decode_payload(bytes);
    check(first.has_value() == second.has_value());
    if (!first) {
        check(first.error() == app::JournalError::Corrupt);
        check(first.error() == second.error());
        return;
    }
    check(*first == *second);
    std::vector<std::byte> reencoded;
    journal::encode_payload(*first, reencoded);
    check(std::ranges::equal(reencoded, bytes));
}

void fuzz_framed_payload(std::span<const std::byte> bytes) {
    if (bytes.size() > journal::max_payload_size) {
        return;
    }
    const auto header = journal::encode(
        journal::RecordHeader{.payload_size = static_cast<std::uint32_t>(bytes.size()),
                              .crc32c = journal::crc32c(bytes)});
    std::vector<std::byte> record(header.begin(), header.end());
    record.insert(record.end(), bytes.begin(), bytes.end());

    const auto parsed = journal::parse_record(record);
    const auto decoded = journal::decode_payload(bytes);
    check(parsed.has_value() == decoded.has_value());
    if (parsed) {
        check(parsed->command == *decoded);
        check(parsed->size == record.size());
    } else {
        check(parsed.error() == decoded.error());
    }
}

void fuzz_record_sequence(std::span<const std::byte> bytes) {
    std::span<const std::byte> rest = bytes;
    while (!rest.empty()) {
        const auto record = journal::parse_record(rest);
        if (!record) {
            const auto error = record.error();
            check(error == app::JournalError::Truncated || error == app::JournalError::Corrupt);
            return;
        }
        check(record->size >= journal::record_header_size);
        check(record->size <= rest.size());
        check(record->size - journal::record_header_size <= journal::max_payload_size);

        // The accepted record decodes identically on its own, and parses with
        // no trailing bytes needed.
        const auto again = journal::parse_record(rest.first(record->size));
        check(again.has_value());
        check(again->command == record->command);
        check(again->size == record->size);
        rest = rest.subspan(record->size);  // strictly shrinks: the loop ends
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes = std::as_bytes(std::span{data, size});
    fuzz_payload(bytes);
    fuzz_framed_payload(bytes);
    fuzz_record_sequence(bytes);
    return 0;
}
