// Acceptance criterion 3: replaying the same journal twice gives the same
// digest; changing a single command in a copy of the journal changes it.
// Uses a real on-disk journal from the shared generator's workload, so the
// mutation is a realistic single-byte change to a committed record, not a
// synthetic one (that is covered at the field level by digest_test.cpp).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/digest.hpp"
#include "lockstep/app/replay.hpp"
#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"

#include "app/test_support.hpp"
#include "generator.hpp"
#include <unistd.h>

namespace lockstep {
namespace {

using namespace domain;
namespace fs = std::filesystem;

constexpr std::size_t shard_count = 2;

class DigestStabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-digest-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);

        app::ManualClock clock;
        test::RecordingSubscriber subscriber;
        std::vector<app::CommandReply> live_replies;
        router_ = std::make_unique<app::Router>(
            app::Router::round_robin(test::instruments(), shard_count));
        auto journal_factory = [this](ShardId shard) -> std::unique_ptr<app::Journal> {
            const auto instruments = router_->instruments_of(shard);
            const ShardConfig shard_config{.shard = shard,
                                           .instruments = {instruments.begin(), instruments.end()}};
            return journal::FileJournalWriter::create(
                dir_,
                journal::FileHeader{.shard = shard,
                                    .shard_count = shard_count,
                                    .config_hash = journal::config_hash(shard_config)},
                journal::SyncPolicy::None);
        };
        app::Engine engine{
            app::EngineConfig{.instruments = test::instruments(), .shard_count = shard_count},
            std::move(journal_factory), clock};
        test::run_workload(engine, subscriber, live_replies, /*producer_count=*/2,
                           /*commands_per_producer=*/300);
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] fs::path shard_path(ShardId shard) const {
        return dir_ / journal::FileJournalWriter::file_name(shard);
    }

    [[nodiscard]] std::uint64_t digest_of(ShardId shard) const {
        return digest_of_file(shard_path(shard), shard);
    }

    [[nodiscard]] std::uint64_t digest_of_file(const fs::path& path, ShardId shard) const {
        const auto instruments = router_->instruments_of(shard);
        const ShardConfig shard_config{.shard = shard,
                                       .instruments = {instruments.begin(), instruments.end()}};
        const journal::ReaderExpectations expect{.shard = shard,
                                                 .shard_count = shard_count,
                                                 .config_hash = journal::config_hash(shard_config)};
        ShardEngine engine{shard_config};
        app::ReplayOutput output;
        domain::EventBuffer buffer;
        for (auto&& record : journal::read_journal(path, expect)) {
            if (!record.has_value()) {
                ADD_FAILURE() << app::to_string(record.error());
                return 0;
            }
            buffer.clear();
            const CommandResult result = engine.apply(*record, buffer);
            for (const Event& event : buffer.events()) {
                output.events.push_back(
                    app::PublishedEvent{shard, record->sequence, record->timestamp, event});
            }
            output.replies.push_back(
                app::CommandReply{shard, record->sequence, record->timestamp, result});
        }
        std::vector<BookSnapshot> books;
        for (const InstrumentSpec& spec : instruments) {
            if (const OrderBook* book = engine.book(spec.id)) {
                books.push_back(book->snapshot());
            }
        }
        return app::digest(output, books);
    }

    [[nodiscard]] const fs::path& dir_for_test() const { return dir_; }

    std::unique_ptr<app::Router> router_;

private:
    fs::path dir_;
};

TEST_F(DigestStabilityTest, ReplayingTheSameJournalTwiceGivesTheSameDigest) {
    const ShardId shard{1};  // instrument 2 (the loadgen's default) lives here
    const std::uint64_t first = digest_of(shard);
    const std::uint64_t second = digest_of(shard);
    EXPECT_EQ(first, second);
}

TEST_F(DigestStabilityTest, ChangingOneCommandInACopyChangesTheDigest) {
    const ShardId shard{1};
    const auto original_path = shard_path(shard);
    const auto baseline = digest_of(shard);

    // A copy: the original journal (and therefore `baseline`, computed
    // above) must stay untouched by the mutation below.
    const auto mutated_path = dir_for_test() / "mutated-shard-1.jnl";
    fs::copy_file(original_path, mutated_path);

    // Walk records until a NewOrder is found - not just the first record,
    // which the richer generator (task 010) may have made a risk broadcast
    // or anything else - and flip the last byte of its payload, which is
    // part of `quantity` (record_codec.hpp's payload layout): any byte value
    // is a valid Quantity, so this always stays decodable, unlike flipping
    // (say) a bool field's byte, which can turn into neither 0 nor 1 and
    // make the record Corrupt instead of a different command - flaky,
    // since whether the chosen record happens to be a NewOrder, and which
    // field its last byte lands in, depended on the live run. Then patches
    // the record's CRC32C so it still decodes cleanly to a *different*
    // command, rather than becoming Corrupt (that path is already covered
    // by RestartTest.CorruptJournalIsRefusedAndFileIsByteIdentical). This is
    // what "changing a single command" means for the digest: same shape,
    // different content, still a valid journal.
    {
        std::fstream file{mutated_path, std::ios::binary | std::ios::in | std::ios::out};
        std::size_t offset = journal::file_header_size;
        for (;;) {
            std::array<char, journal::record_header_size> header_bytes{};
            file.seekg(static_cast<std::streamoff>(offset));
            ASSERT_TRUE(
                file.read(header_bytes.data(), static_cast<std::streamsize>(header_bytes.size())))
                << "ran out of records without finding a NewOrder";
            std::array<std::byte, journal::record_header_size> header_as_bytes{};
            std::ranges::transform(header_bytes, header_as_bytes.begin(),
                                   [](char c) { return static_cast<std::byte>(c); });
            const journal::RecordHeader header = journal::decode_record_header(header_as_bytes);
            ASSERT_GT(header.payload_size, 0U);
            const auto payload_offset = offset + journal::record_header_size;

            std::vector<std::byte> payload(header.payload_size);
            file.seekg(static_cast<std::streamoff>(payload_offset));
            // std::fstream::read only takes char*; std::byte* -> char* is a
            // standard-layout reinterpret, not a type pun.
            file.read(reinterpret_cast<char*>(payload.data()),  // NOLINT(*-reinterpret-cast)
                      static_cast<std::streamsize>(payload.size()));

            // Payload layout (record_codec.hpp): sequence u64 | timestamp
            // i64 | tag u8 | fields... - the tag sits right after the two
            // 8-byte fields.
            constexpr std::size_t tag_offset = 16;
            ASSERT_GT(payload.size(), tag_offset);
            const auto tag = std::to_integer<std::uint8_t>(payload[tag_offset]);
            if (tag == std::to_underlying(CommandTag::NewOrder)) {
                payload.back() =
                    static_cast<std::byte>(~std::to_integer<unsigned char>(payload.back()));
                file.seekp(static_cast<std::streamoff>(payload_offset + payload.size() - 1));
                const char mutated_byte = static_cast<char>(payload.back());
                file.write(&mutated_byte, 1);

                const journal::RecordHeader patched{.payload_size = header.payload_size,
                                                    .crc32c = journal::crc32c(payload)};
                const auto encoded_header = journal::encode(patched);
                std::array<char, journal::record_header_size> encoded_chars{};
                std::ranges::transform(encoded_header, encoded_chars.begin(),
                                       [](std::byte b) { return static_cast<char>(b); });
                file.seekp(static_cast<std::streamoff>(offset));
                file.write(encoded_chars.data(),
                           static_cast<std::streamsize>(encoded_chars.size()));
                break;
            }
            offset = payload_offset + header.payload_size;
        }
    }

    const auto mutated_digest = digest_of_file(mutated_path, shard);
    EXPECT_NE(mutated_digest, baseline);
    // The original must stay exactly as digest_of(shard) above read it.
    EXPECT_EQ(digest_of(shard), baseline);
}

}  // namespace
}  // namespace lockstep
