// Ledger semantics per spec 5.1.1: revision numbering, JSON round-trip,
// FIFO eviction with the eviction guard, boot reload, and persistence
// failure behavior.
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_ledger.h"
#include "../../lib/maccontrol_core/mc_log.h"
#include "../../lib/maccontrol_core/mc_random.h"
#include "../../lib/maccontrol_core/mc_sha256.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800; // 2025-01-14T09:20:00Z
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
    void advance_seconds(uint64_t s) { epoch += s; }
    void advance_ms(uint64_t ms) { mono_ms += ms; }
};

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x42;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

struct MemStorage : mcco::ILedgerStorage {
    std::vector<std::string> lines;
    bool append(const std::string& line) override {
        lines.push_back(line);
        return true;
    }
    bool read_all(const std::function<void(const std::string&)>& cb) override {
        for (auto& l : lines) cb(l);
        return true;
    }
    bool replace_all(const std::vector<std::string>& new_lines) override {
        lines = new_lines;
        return true;
    }
};

// Storage that keeps appended lines even across compaction, so tests can
// inspect the raw append stream (including the eviction-guard revision).
struct NoCompactStorage : mcco::ILedgerStorage {
    std::vector<std::string> lines;
    bool append(const std::string& line) override {
        lines.push_back(line);
        return true;
    }
    bool read_all(const std::function<void(const std::string&)>& cb) override {
        for (auto& l : lines) cb(l);
        return true;
    }
    bool replace_all(const std::vector<std::string>&) override { return true; }
};

// Storage whose append() starts failing once `fail_from` lines exist.
struct FailStorage : mcco::ILedgerStorage {
    std::vector<std::string> lines;
    size_t fail_from = 0;
    bool append(const std::string& line) override {
        if (lines.size() >= fail_from) return false;
        lines.push_back(line);
        return true;
    }
    bool read_all(const std::function<void(const std::string&)>& cb) override {
        for (auto& l : lines) cb(l);
        return true;
    }
    bool replace_all(const std::vector<std::string>& new_lines) override {
        lines = new_lines;
        return true;
    }
};

mcco::CommandRecord make_rec(const std::string& id, mcco::CommandState state) {
    mcco::CommandRecord rec;
    rec.command_id = id;
    rec.type = mcco::CommandType::Lock;
    rec.parameters_json = "{}";
    rec.requested_by = "apikey:key-01";
    rec.requested_at = 1736848800;
    rec.state = state;
    rec.deadline_at = 1736848815;
    return rec;
}

TEST(Ledger, RevisionNumberingIsPerCommand) {
    FakeClock clock;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);

    mcco::CommandRecord a1 = make_rec("cmd-aaaa", mcco::CommandState::Accepted);
    ASSERT_TRUE(ledger.append_revision(a1));
    EXPECT_EQ(a1.revision, 1);

    mcco::CommandRecord b1 = make_rec("cmd-bbbb", mcco::CommandState::Accepted);
    ASSERT_TRUE(ledger.append_revision(b1));
    EXPECT_EQ(b1.revision, 1); // revision counters are per command_id

    mcco::CommandRecord a2 = make_rec("cmd-aaaa", mcco::CommandState::Dispatched);
    ASSERT_TRUE(ledger.append_revision(a2));
    EXPECT_EQ(a2.revision, 2);

    mcco::CommandRecord a3 = make_rec("cmd-aaaa", mcco::CommandState::Unconfirmed);
    ASSERT_TRUE(ledger.append_revision(a3));
    EXPECT_EQ(a3.revision, 3);

    mcco::CommandRecord b2 = make_rec("cmd-bbbb", mcco::CommandState::Failed);
    ASSERT_TRUE(ledger.append_revision(b2));
    EXPECT_EQ(b2.revision, 2);

    // latest() returns the highest revision.
    const mcco::CommandRecord* la = ledger.latest("cmd-aaaa");
    ASSERT_NE(la, nullptr);
    EXPECT_EQ(la->revision, 3);
    EXPECT_EQ(la->state, mcco::CommandState::Unconfirmed);
    const mcco::CommandRecord* lb = ledger.latest("cmd-bbbb");
    ASSERT_NE(lb, nullptr);
    EXPECT_EQ(lb->revision, 2);
    EXPECT_EQ(lb->state, mcco::CommandState::Failed);
    EXPECT_EQ(ledger.command_count(), 2);
}

TEST(Ledger, RecordJsonRoundTripPreservesAllFields) {
    mcco::CommandRecord rec;
    rec.command_id = "1AB234CD";
    rec.idempotency_key = "k1";
    rec.revision = 7;
    rec.type = mcco::CommandType::Sleep;
    rec.parameters_json = R"({"brightness":50})";
    rec.requested_by = "apikey:key-03";
    rec.requested_at = 1736848800;
    rec.mode_at_accept = 'A';
    rec.state = mcco::CommandState::Dispatched;
    rec.dispatched_at = 1736848812;
    rec.deadline_at = 1736848890;
    rec.has_window = true;
    rec.window_open_after_s = 3;
    rec.window_close_after_s = 60;
    rec.evidence = {"ev-1", "ev-2"};
    rec.result = "";      // null in JSON
    rec.error_code = "";  // null in JSON

    const std::string json = mcco::Ledger::record_to_json(rec);
    mcco::CommandRecord back;
    ASSERT_TRUE(mcco::Ledger::record_from_json(json, back));

    EXPECT_EQ(back.command_id, rec.command_id);
    EXPECT_EQ(back.idempotency_key, rec.idempotency_key);
    EXPECT_EQ(back.revision, rec.revision);
    EXPECT_EQ(back.type, rec.type);
    EXPECT_EQ(back.parameters_json, rec.parameters_json);
    EXPECT_EQ(back.requested_by, rec.requested_by);
    EXPECT_EQ(back.requested_at, rec.requested_at);
    EXPECT_EQ(back.mode_at_accept, rec.mode_at_accept);
    EXPECT_EQ(back.state, rec.state);
    EXPECT_EQ(back.dispatched_at, rec.dispatched_at);
    EXPECT_EQ(back.deadline_at, rec.deadline_at);
    EXPECT_TRUE(back.has_window);
    EXPECT_EQ(back.window_open_after_s, 3u);
    EXPECT_EQ(back.window_close_after_s, 60u);
    EXPECT_EQ(back.evidence, rec.evidence);
    EXPECT_TRUE(back.result.empty());
    EXPECT_TRUE(back.error_code.empty());
}

TEST(Ledger, RecordJsonRoundTripPreservesNulls) {
    mcco::CommandRecord rec;
    rec.command_id = "1AB234CD";
    rec.idempotency_key = ""; // null in JSON
    rec.revision = 1;
    rec.type = mcco::CommandType::Wake;
    rec.parameters_json = "{}";
    rec.requested_by = "apikey:key-01";
    rec.requested_at = 0;
    rec.state = mcco::CommandState::Accepted;
    rec.dispatched_at = 0;    // null in JSON
    rec.deadline_at = 1736848920;
    rec.has_window = false;   // null in JSON

    const std::string json = mcco::Ledger::record_to_json(rec);
    mcco::CommandRecord back;
    ASSERT_TRUE(mcco::Ledger::record_from_json(json, back));

    EXPECT_TRUE(back.idempotency_key.empty());
    EXPECT_EQ(back.requested_at, 0u);
    EXPECT_EQ(back.dispatched_at, 0u);
    EXPECT_FALSE(back.has_window);
    EXPECT_TRUE(back.evidence.empty());
    EXPECT_TRUE(back.result.empty());
    EXPECT_TRUE(back.error_code.empty());
    // Timestamps serialize as ISO8601 strings.
    EXPECT_NE(json.find("\"requested_at\":\"1970-01-01T00:00:00Z\""), std::string::npos);
}

TEST(Ledger, FifoEvictionDropsOldestInAcceptanceOrder) {
    FakeClock clock;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);

    for (int i = 0; i < 70; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        mcco::CommandRecord rec = make_rec(id, mcco::CommandState::Completed);
        ASSERT_TRUE(ledger.append_revision(rec)) << id;
    }

    EXPECT_EQ(ledger.command_count(), 64);
    EXPECT_EQ(ledger.capacity(), 64);

    // The oldest 6 (acceptance order) are gone.
    for (int i = 0; i < 6; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        EXPECT_EQ(ledger.latest(id), nullptr) << id;
    }
    // The rest survive, newest-acceptance-first.
    for (int i = 6; i < 70; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        ASSERT_NE(ledger.latest(id), nullptr) << id;
    }
    std::vector<const mcco::CommandRecord*> listed = ledger.list_newest_first();
    ASSERT_EQ(listed.size(), 64);
    EXPECT_EQ(listed.front()->command_id, "cmd-69"); // newest acceptance first
    EXPECT_EQ(listed.back()->command_id, "cmd-06");
}

TEST(Ledger, EvictionGuardTerminatesPendingCommandBeforeRemoval) {
    FakeClock clock;
    NoCompactStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);

    // Oldest entry is a non-terminal (accepted) command.
    mcco::CommandRecord pending = make_rec("cmd-pend", mcco::CommandState::Accepted);
    ASSERT_TRUE(ledger.append_revision(pending));
    for (int i = 0; i < 63; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        mcco::CommandRecord rec = make_rec(id, mcco::CommandState::Completed);
        ASSERT_TRUE(ledger.append_revision(rec));
    }
    ASSERT_EQ(ledger.command_count(), 64);

    // 65th append triggers eviction of the pending command.
    mcco::CommandRecord extra = make_rec("cmd-new", mcco::CommandState::Completed);
    ASSERT_TRUE(ledger.append_revision(extra));

    EXPECT_EQ(ledger.command_count(), 64);
    EXPECT_EQ(ledger.latest("cmd-pend"), nullptr);
    ASSERT_NE(ledger.latest("cmd-00"), nullptr); // FIFO: oldest by acceptance

    // The eviction guard persisted a terminal failed/evicted_pending revision
    // BEFORE the command was removed from the index.
    bool found_terminal = false;
    for (const auto& line : storage.lines) {
        mcco::CommandRecord rec;
        if (!mcco::Ledger::record_from_json(line, rec)) continue;
        if (rec.command_id != "cmd-pend") continue;
        if (rec.state == mcco::CommandState::Failed && rec.error_code == "evicted_pending") {
            found_terminal = true;
            EXPECT_TRUE(mcco::is_terminal(rec.state));
        }
    }
    EXPECT_TRUE(found_terminal) << "eviction guard revision missing from storage stream";
}

TEST(Ledger, BootReloadPreservesCommandsAndStates) {
    FakeClock clock;
    MemStorage storage;
    mcco::NullLog log;
    {
        mcco::Ledger ledger(storage, clock, log, 64);
        mcco::CommandRecord a1 = make_rec("cmd-aaaa", mcco::CommandState::Accepted);
        ASSERT_TRUE(ledger.append_revision(a1));
        mcco::CommandRecord a2 = make_rec("cmd-aaaa", mcco::CommandState::Unconfirmed);
        a2.result = "hid_only";
        ASSERT_TRUE(ledger.append_revision(a2));
        mcco::CommandRecord b1 = make_rec("cmd-bbbb", mcco::CommandState::Accepted);
        ASSERT_TRUE(ledger.append_revision(b1));
        ASSERT_EQ(ledger.command_count(), 2);
    }

    // New ledger over the same storage, as after a reboot.
    mcco::Ledger reloaded(storage, clock, log, 64);
    ASSERT_TRUE(reloaded.load());
    EXPECT_TRUE(reloaded.loaded());
    EXPECT_EQ(reloaded.command_count(), 2);

    const mcco::CommandRecord* la = reloaded.latest("cmd-aaaa");
    ASSERT_NE(la, nullptr);
    EXPECT_EQ(la->revision, 2);
    EXPECT_EQ(la->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(la->result, "hid_only");
    const mcco::CommandRecord* lb = reloaded.latest("cmd-bbbb");
    ASSERT_NE(lb, nullptr);
    EXPECT_EQ(lb->revision, 1);
    EXPECT_EQ(lb->state, mcco::CommandState::Accepted);
}

TEST(Ledger, BootReloadTrimsMirrorToCapacity) {
    // The durable stream is append-only and grows forever; a ledger file
    // written by a higher-capacity device (or a long-lived one whose flash
    // compacts silently failed) must not inflate the RAM mirror past
    // capacity at boot (S3 heap landmine: the 2026-09-29 bad_alloc panic
    // loop). Eviction at load keeps the newest `capacity` commands, same as
    // runtime eviction (spec 5.1.1).
    FakeClock clock;
    MemStorage storage;
    mcco::NullLog log;
    // Writer at the spec-max capacity, no eviction at 100 commands.
    {
        mcco::Ledger writer(storage, clock, log, 256);
        for (int i = 0; i < 100; i++) {
            char id[16];
            snprintf(id, sizeof(id), "cmd-%03d", i);
            mcco::CommandRecord rec = make_rec(id, mcco::CommandState::Accepted);
            ASSERT_TRUE(writer.append_revision(rec));
        }
        ASSERT_EQ(writer.command_count(), 100);
    }
    // Reload at 64 (the shipped minimum): mirror must trim to capacity.
    mcco::Ledger reloaded(storage, clock, log, 64);
    ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.command_count(), 64);
    // Newest survivors present, oldest evicted.
    EXPECT_NE(reloaded.latest("cmd-099"), nullptr);
    EXPECT_NE(reloaded.latest("cmd-036"), nullptr);
    EXPECT_EQ(reloaded.latest("cmd-000"), nullptr);
    EXPECT_EQ(reloaded.latest("cmd-035"), nullptr);
}

TEST(Ledger, BootReloadSkipsCorruptLines) {
    FakeClock clock;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);
    for (int i = 0; i < 3; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        mcco::CommandRecord rec = make_rec(id, mcco::CommandState::Accepted);
        ASSERT_TRUE(ledger.append_revision(rec));
    }
    const size_t good_lines = storage.lines.size();
    ASSERT_EQ(good_lines, 3);

    // Inject garbage mid-file: torn JSON, non-object JSON, and a record with
    // an unknown state string.
    storage.lines.insert(storage.lines.begin() + 1, "{\"command_id\": \"broken\"");
    storage.lines.insert(storage.lines.begin() + 2, "42");
    storage.lines.insert(storage.lines.begin() + 3,
                         R"({"command_id":"cmd-x","revision":1,"type":"lock","requested_by":"","requested_at":"2025-01-14T09:20:00Z","state":"bogus","deadline_at":"2025-01-14T09:20:15Z"})");

    mcco::Ledger reloaded(storage, clock, log, 64);
    ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.command_count(), 3); // corrupt lines skipped, rest intact
    for (int i = 0; i < 3; i++) {
        char id[16];
        snprintf(id, sizeof(id), "cmd-%02d", i);
        ASSERT_NE(reloaded.latest(id), nullptr) << id;
    }
    EXPECT_EQ(reloaded.latest("cmd-x"), nullptr);
}

TEST(Ledger, PersistenceFailureOnNewCommandLeavesNoIndexLeak) {
    FakeClock clock;
    FailStorage storage;
    storage.fail_from = 0; // first append already fails
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);

    mcco::CommandRecord rec = make_rec("cmd-dead", mcco::CommandState::Accepted);
    EXPECT_FALSE(ledger.append_revision(rec));
    EXPECT_EQ(ledger.command_count(), 0);
    EXPECT_EQ(ledger.latest("cmd-dead"), nullptr);
    EXPECT_TRUE(ledger.list_newest_first().empty());
}

TEST(Ledger, PersistenceFailureOnFollowUpRevisionKeepsPriorState) {
    FakeClock clock;
    FailStorage storage;
    storage.fail_from = 1; // first append succeeds, then writes fail
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);

    mcco::CommandRecord r1 = make_rec("cmd-aaaa", mcco::CommandState::Accepted);
    ASSERT_TRUE(ledger.append_revision(r1));
    ASSERT_EQ(ledger.command_count(), 1);

    mcco::CommandRecord r2 = make_rec("cmd-aaaa", mcco::CommandState::Dispatched);
    EXPECT_FALSE(ledger.append_revision(r2)); // caller surfaces 503

    EXPECT_EQ(ledger.command_count(), 1); // index unchanged
    const mcco::CommandRecord* cur = ledger.latest("cmd-aaaa");
    ASSERT_NE(cur, nullptr);
    EXPECT_EQ(cur->revision, 1);
    EXPECT_EQ(cur->state, mcco::CommandState::Accepted);
}

TEST(Ledger, DigestMatchesSha256Hex) {
    // Sanity anchor for storage-level digests used elsewhere in the spec.
    EXPECT_EQ(mcco::Sha256::hex_digest("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

} // namespace
