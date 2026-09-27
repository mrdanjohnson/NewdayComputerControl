// Command pipeline per spec 5.1 (submit/dedup), 5.2 (Mode A lifecycle),
// 5.3 (deadlines/windows), and 12.2 (closed command surface).
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_engine.h"
#include "../../lib/maccontrol_core/mc_types.h"
#include <string>
#include <vector>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800; // 2025-01-14T09:20:00Z
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
    void advance_seconds(uint64_t s) {
        epoch += s;
        mono_ms += s * 1000;
    }
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

struct EngineFixture {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger{storage, clock, log, 64};
    mcco::CommandEngine engine{ledger, clock, rng, log};
    EngineFixture() { engine.init(); }
};

mcco::Submission power_sub(mcco::CommandType t, const std::string& hash) {
    mcco::Submission sub;
    sub.type = t;
    sub.parameters_json = "{}";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = hash;
    return sub;
}

TEST(Engine, PowerCommandsHappyPath) {
    struct Case {
        mcco::CommandType type;
        uint32_t deadline_s;
        bool has_window;
    };
    const Case cases[] = {
        {mcco::CommandType::Wake, 120, false},
        {mcco::CommandType::Sleep, 90, true},
        {mcco::CommandType::Restart, 180, true},
        {mcco::CommandType::Shutdown, 120, true},
        {mcco::CommandType::Lock, 15, false},
    };
    for (const Case& tc : cases) {
        EngineFixture fx;
        const uint64_t t0 = fx.clock.epoch;
        auto out = fx.engine.submit(power_sub(tc.type, "h"));
        ASSERT_TRUE(out.ok) << mcco::command_type_to_string(tc.type);
        EXPECT_EQ(out.http_status, 202);
        EXPECT_TRUE(out.dispatch_pending);
        EXPECT_EQ(out.record.state, mcco::CommandState::Accepted);
        EXPECT_EQ(out.record.revision, 1);
        // Deadline projected at accept from the per-type default.
        EXPECT_EQ(out.record.deadline_at, t0 + tc.deadline_s);

        // Dispatch completes some seconds later; the deadline is recomputed
        // authoritatively at dispatch time.
        fx.clock.advance_seconds(10);
        const uint64_t t1 = fx.clock.epoch;
        ASSERT_TRUE(fx.engine.complete_dispatch(out.record.command_id, true));

        const mcco::CommandRecord* rec = fx.engine.get(out.record.command_id);
        ASSERT_NE(rec, nullptr);
        // Mode A: dispatch success terminates unconfirmed/hid_only — the
        // engine NEVER assigns completed without MCA evidence.
        EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed) << mcco::command_type_to_string(tc.type);
        EXPECT_NE(rec->state, mcco::CommandState::Completed);
        EXPECT_EQ(rec->result, "hid_only");
        EXPECT_TRUE(rec->error_code.empty());
        EXPECT_EQ(rec->dispatched_at, t1);
        EXPECT_EQ(rec->deadline_at, t1 + tc.deadline_s);
        EXPECT_EQ(rec->revision, 3); // accepted -> dispatched -> unconfirmed
        EXPECT_EQ(rec->has_window, tc.has_window);
        if (tc.has_window) {
            EXPECT_EQ(rec->window_open_after_s, 3u);
            EXPECT_EQ(rec->window_close_after_s, 60u);
        }
    }
}

TEST(Engine, DispatchFailureTerminatesFailed) {
    EngineFixture fx;
    auto out = fx.engine.submit(power_sub(mcco::CommandType::Lock, "h"));
    ASSERT_TRUE(out.ok);

    ASSERT_TRUE(fx.engine.complete_dispatch(out.record.command_id, false));
    const mcco::CommandRecord* rec = fx.engine.get(out.record.command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "dispatch_error");
    EXPECT_TRUE(rec->result.empty());
}

TEST(Engine, DoubleCompleteDispatchIsRejected) {
    EngineFixture fx;
    auto out = fx.engine.submit(power_sub(mcco::CommandType::Wake, "h"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(fx.engine.complete_dispatch(out.record.command_id, true));

    // Terminal records cannot be completed again.
    EXPECT_FALSE(fx.engine.complete_dispatch(out.record.command_id, true));
    EXPECT_FALSE(fx.engine.complete_dispatch(out.record.command_id, false));

    const mcco::CommandRecord* rec = fx.engine.get(out.record.command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->revision, 3); // no 4th revision appended
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
}

TEST(Engine, CompleteDispatchUnknownIdIsRejected) {
    EngineFixture fx;
    EXPECT_FALSE(fx.engine.complete_dispatch("nope0000", true));
}

TEST(Engine, AgentCommandsRejectedPreLedger) {
    for (mcco::CommandType t : {mcco::CommandType::AppLaunch, mcco::CommandType::AppQuit}) {
        EngineFixture fx;
        mcco::Submission sub;
        sub.type = t;
        sub.parameters_json = R"({"bundle_id":"com.example.app"})";
        sub.requested_by = "apikey:key-01";
        sub.body_hash = "h";
        const size_t before = fx.ledger.command_count();

        auto out = fx.engine.submit(sub);
        EXPECT_FALSE(out.ok);
        EXPECT_EQ(out.error, mcco::ErrCode::AgentNotPaired);
        EXPECT_EQ(out.http_status, 500); // status mapped by HTTP layer, not engine
        EXPECT_EQ(fx.ledger.command_count(), before); // NO ledger record (pre-ledger reject)
        EXPECT_TRUE(out.record.command_id.empty());
    }
}

TEST(Engine, MacroExecuteNotFound) {
    EngineFixture fx;
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"m1"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";

    auto out = fx.engine.submit(sub);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::NotFound);
    EXPECT_EQ(fx.ledger.command_count(), 0);
}

TEST(Engine, BadRequestOnUnknownOrMalformedParameters) {
    EngineFixture fx;
    // Unknown parameter for a power command (power commands take no params).
    mcco::Submission unknown = power_sub(mcco::CommandType::Wake, "h1");
    unknown.parameters_json = R"({"foo":1})";
    auto out = fx.engine.submit(unknown);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::BadRequest);

    mcco::Submission extra = power_sub(mcco::CommandType::Sleep, "h2");
    extra.parameters_json = R"({"foo":1})";
    out = fx.engine.submit(extra);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::BadRequest);

    // Malformed JSON.
    mcco::Submission bad = power_sub(mcco::CommandType::Lock, "h3");
    bad.parameters_json = "{unclosed";
    out = fx.engine.submit(bad);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::BadRequest);

    // Valid JSON but not an object.
    mcco::Submission arr = power_sub(mcco::CommandType::Lock, "h4");
    arr.parameters_json = "[1,2]";
    out = fx.engine.submit(arr);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::BadRequest);

    // Missing required schema field for agent commands is also 400.
    mcco::Submission app;
    app.type = mcco::CommandType::AppLaunch;
    app.parameters_json = "{}";
    app.requested_by = "apikey:key-01";
    app.body_hash = "h5";
    out = fx.engine.submit(app);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::BadRequest);

    EXPECT_EQ(fx.ledger.command_count(), 0); // nothing accepted
}

TEST(Engine, ExplicitIdempotencyKeyReplayAndConflict) {
    EngineFixture fx;
    mcco::Submission sub = power_sub(mcco::CommandType::Wake, "hash-a");
    sub.idempotency_key = "k1";

    auto first = fx.engine.submit(sub);
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.http_status, 202);
    EXPECT_TRUE(first.dispatch_pending);
    EXPECT_EQ(fx.ledger.command_count(), 1);

    // Same key + same body: replay (200, same command_id, no re-dispatch).
    auto replay = fx.engine.submit(sub);
    ASSERT_TRUE(replay.ok);
    EXPECT_EQ(replay.http_status, 200);
    EXPECT_TRUE(replay.replay);
    EXPECT_FALSE(replay.dispatch_pending);
    EXPECT_EQ(replay.record.command_id, first.record.command_id);
    EXPECT_EQ(replay.record.revision, first.record.revision);
    EXPECT_EQ(fx.ledger.command_count(), 1); // no new record

    // Same key + different body: conflict.
    mcco::Submission divergent = sub;
    divergent.body_hash = "hash-b";
    auto conflict = fx.engine.submit(divergent);
    EXPECT_FALSE(conflict.ok);
    EXPECT_EQ(conflict.error, mcco::ErrCode::Conflict);
    EXPECT_EQ(fx.ledger.command_count(), 1);
}

TEST(Engine, DerivedCoalescingWithinSixtySeconds) {
    EngineFixture fx;
    const uint64_t t0 = fx.clock.epoch;

    auto a = fx.engine.submit(power_sub(mcco::CommandType::Sleep, "hash-x"));
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(a.http_status, 202);

    // Same type + same body_hash within 60 s: coalesced replay.
    auto b = fx.engine.submit(power_sub(mcco::CommandType::Sleep, "hash-x"));
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(b.http_status, 202);
    EXPECT_TRUE(b.replay);
    EXPECT_EQ(b.record.command_id, a.record.command_id);
    EXPECT_EQ(fx.ledger.command_count(), 1);

    // Different body_hash: new command.
    auto c = fx.engine.submit(power_sub(mcco::CommandType::Sleep, "hash-y"));
    ASSERT_TRUE(c.ok);
    EXPECT_FALSE(c.replay);
    EXPECT_NE(c.record.command_id, a.record.command_id);
    EXPECT_EQ(fx.ledger.command_count(), 2);

    // Past the 60 s window the same hash is a fresh command again.
    fx.clock.advance_seconds(61);
    auto d = fx.engine.submit(power_sub(mcco::CommandType::Sleep, "hash-x"));
    ASSERT_TRUE(d.ok);
    EXPECT_FALSE(d.replay);
    EXPECT_NE(d.record.command_id, a.record.command_id);
    EXPECT_EQ(d.record.requested_at, t0 + 61);
    EXPECT_EQ(fx.ledger.command_count(), 3);
}

TEST(Engine, BootReconciliationDeadlineOpenFailedRestart) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    const std::string command_id = [&] {
        mcco::Ledger ledger(storage, clock, log, 64);
        mcco::CommandEngine engine(ledger, clock, rng, log);
        engine.init();
        auto out = engine.submit(power_sub(mcco::CommandType::Lock, "h"));
        EXPECT_TRUE(out.ok);
        return out.record.command_id;
    }(); // engine destroyed with the accepted record still non-terminal

    // Reboot before the deadline: esp32_restarted / failed.
    clock.advance_seconds(10); // lock deadline is 15 s: still open
    mcco::Ledger ledger2(storage, clock, log, 64);
    mcco::CommandEngine engine2(ledger2, clock, rng, log);
    ASSERT_TRUE(engine2.init());

    const mcco::CommandRecord* rec = engine2.get(command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "esp32_restarted");
    EXPECT_EQ(rec->revision, 2); // accepted -> failed
}

TEST(Engine, BootReconciliationDeadlineExceededTimesOut) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    const std::string command_id = [&] {
        mcco::Ledger ledger(storage, clock, log, 64);
        mcco::CommandEngine engine(ledger, clock, rng, log);
        engine.init();
        auto out = engine.submit(power_sub(mcco::CommandType::Shutdown, "h"));
        EXPECT_TRUE(out.ok);
        return out.record.command_id;
    }();

    // Reboot after the deadline (shutdown: 120 s) passed.
    clock.advance_seconds(121);
    mcco::Ledger ledger2(storage, clock, log, 64);
    mcco::CommandEngine engine2(ledger2, clock, rng, log);
    ASSERT_TRUE(engine2.init());

    const mcco::CommandRecord* rec = engine2.get(command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(Engine, ReconciliationLeavesTerminalRecordsAlone) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    const std::string done_id = [&] {
        mcco::Ledger ledger(storage, clock, log, 64);
        mcco::CommandEngine engine(ledger, clock, rng, log);
        engine.init();
        auto out = engine.submit(power_sub(mcco::CommandType::Wake, "h"));
        EXPECT_TRUE(out.ok);
        EXPECT_TRUE(engine.complete_dispatch(out.record.command_id, true));
        return out.record.command_id;
    }();

    clock.advance_seconds(3600); // far past any deadline
    mcco::Ledger ledger2(storage, clock, log, 64);
    mcco::CommandEngine engine2(ledger2, clock, rng, log);
    ASSERT_TRUE(engine2.init());

    const mcco::CommandRecord* rec = engine2.get(done_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed); // untouched
    EXPECT_EQ(rec->revision, 3);
}

TEST(Engine, ListFilterByStateTypeAndSince) {
    EngineFixture fx;
    const uint64_t t0 = fx.clock.epoch;

    auto wake = fx.engine.submit(power_sub(mcco::CommandType::Wake, "w"));
    ASSERT_TRUE(wake.ok);
    auto lock1 = fx.engine.submit(power_sub(mcco::CommandType::Lock, "l1"));
    ASSERT_TRUE(lock1.ok);
    ASSERT_TRUE(fx.engine.complete_dispatch(lock1.record.command_id, true));

    fx.clock.advance_seconds(5);
    auto lock2 = fx.engine.submit(power_sub(mcco::CommandType::Lock, "l2"));
    ASSERT_TRUE(lock2.ok);
    ASSERT_EQ(fx.ledger.command_count(), 3);

    // State filter.
    mcco::CommandEngine::ListFilter f;
    f.has_state = true;
    f.state = mcco::CommandState::Accepted;
    auto accepted = fx.engine.list(f);
    ASSERT_EQ(accepted.size(), 2);
    for (const mcco::CommandRecord* r : accepted)
        EXPECT_EQ(r->state, mcco::CommandState::Accepted);

    f.state = mcco::CommandState::Unconfirmed;
    auto unconfirmed = fx.engine.list(f);
    ASSERT_EQ(unconfirmed.size(), 1);
    EXPECT_EQ(unconfirmed[0]->command_id, lock1.record.command_id);

    // Type filter.
    mcco::CommandEngine::ListFilter tf;
    tf.has_type = true;
    tf.type = mcco::CommandType::Lock;
    auto locks = fx.engine.list(tf);
    ASSERT_EQ(locks.size(), 2);
    for (const mcco::CommandRecord* r : locks)
        EXPECT_EQ(r->type, mcco::CommandType::Lock);

    // Since filter.
    mcco::CommandEngine::ListFilter sf;
    sf.has_since = true;
    sf.since = t0 + 1;
    auto recent = fx.engine.list(sf);
    ASSERT_EQ(recent.size(), 1);
    EXPECT_EQ(recent[0]->command_id, lock2.record.command_id);

    // Combined filter.
    mcco::CommandEngine::ListFilter cf;
    cf.has_type = true;
    cf.type = mcco::CommandType::Lock;
    cf.has_state = true;
    cf.state = mcco::CommandState::Accepted;
    auto combined = fx.engine.list(cf);
    ASSERT_EQ(combined.size(), 1);
    EXPECT_EQ(combined[0]->command_id, lock2.record.command_id);

    // Newest-first ordering overall.
    mcco::CommandEngine::ListFilter none;
    auto all = fx.engine.list(none);
    ASSERT_EQ(all.size(), 3);
    EXPECT_EQ(all[0]->command_id, lock2.record.command_id);
    EXPECT_EQ(all[2]->command_id, wake.record.command_id);
}

TEST(Engine, MacroExecuteResolverWiredFound) {
    EngineFixture fx;
    fx.engine.set_macro_resolver([](const std::string& macro_id) {
        mcco::CommandEngine::MacroResolution r;
        if (macro_id == "mac_3F81") {
            r.found = true;
            r.timeout_ms = 10000;
        }
        return r;
    });
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"mac_3F81"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";

    auto out = fx.engine.submit(sub);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.http_status, 202);
    EXPECT_TRUE(out.dispatch_pending);
    EXPECT_EQ(fx.ledger.command_count(), 1);

    // Dispatch completes later; the dispatcher passes the authoritative
    // per-macro deadline (timeout_ms/1000 + 5, spec 10.3.1) as an override.
    fx.clock.advance_seconds(2);
    const uint64_t t1 = fx.clock.epoch;
    ASSERT_TRUE(fx.engine.complete_dispatch(out.record.command_id, true, 15));

    const mcco::CommandRecord* rec = fx.engine.get(out.record.command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(rec->result, "hid_only");
    EXPECT_EQ(rec->dispatched_at, t1);
    EXPECT_EQ(rec->deadline_at, t1 + 15); // override, not the per-type default
}

TEST(Engine, MacroExecuteResolverWiredNotFound) {
    EngineFixture fx;
    fx.engine.set_macro_resolver([](const std::string&) {
        return mcco::CommandEngine::MacroResolution{};
    });
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"mac_0000"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";

    auto out = fx.engine.submit(sub);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::NotFound);
    EXPECT_EQ(fx.ledger.command_count(), 0); // pre-ledger reject
}

TEST(Engine, SubmitBeforeInitReturnsLedgerUnavailable) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);
    mcco::CommandEngine engine(ledger, clock, rng, log);
    auto out = engine.submit(power_sub(mcco::CommandType::Wake, "h"));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::LedgerUnavailable);
}

} // namespace
