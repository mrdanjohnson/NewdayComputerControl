#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_engine.h"
#include <vector>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800;
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
    void advance_ms(uint64_t ms) { mono_ms += ms; }
    void advance_seconds(uint64_t s) {
        epoch += s;
        mono_ms += s * 1000;
    }
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

struct CapturingLog : mcco::ILog {
    std::vector<std::string> events;
    void write(mcco::LogCategory, mcco::LogLevel, const char* event, const char*,
               const char*, const char*, const char*) override {
        events.push_back(event ? event : "");
    }
    bool has(const char* e) const {
        for (const std::string& x : events)
            if (x == e) return true;
        return false;
    }
};

struct Ctx {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    CapturingLog log;
    mcco::Ledger ledger{storage, clock, log, 64};
    mcco::CommandEngine engine{ledger, clock, rng, log};
    Ctx() { engine.init(); }
};

mcco::Submission power_sub(mcco::CommandType t, const std::string& hash) {
    mcco::Submission sub;
    sub.type = t;
    sub.parameters_json = "{}";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = hash;
    return sub;
}

mcco::Submission app_sub(mcco::CommandType t, const char* bundle) {
    mcco::Submission sub;
    sub.type = t;
    sub.parameters_json = std::string("{\"bundle_id\":\"") + bundle + "\"}";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";
    return sub;
}

mcco::AgentEvent ack(const std::string& cmd, uint64_t seq, const char* action) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_ack";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::CommandAck;
    ev.has_command_id = true;
    ev.command_id = cmd;
    ev.payload_json = std::string("{\"command_id\":\"") + cmd + "\",\"action\":\"" + action + "\"}";
    return ev;
}

mcco::AgentEvent started(const char* bundle, uint64_t seq) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_started";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::ApplicationStarted;
    ev.payload_json = std::string("{\"bundle_id\":\"") + bundle + "\",\"pid\":812}";
    return ev;
}

mcco::AgentEvent exited(const char* bundle, uint64_t seq, const char* reason) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_exited";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::ApplicationExited;
    ev.payload_json = std::string("{\"bundle_id\":\"") + bundle + "\",\"pid\":812,\"reason\":\"" + reason + "\"}";
    return ev;
}

mcco::AgentEvent result_failed(const std::string& cmd, uint64_t seq, const char* code) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_result";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::CommandResult;
    ev.has_command_id = true;
    ev.command_id = cmd;
    ev.payload_json = std::string("{\"command_id\":\"") + cmd + "\",\"outcome\":\"failed\",\"error_code\":\"" + code + "\"}";
    return ev;
}

mcco::AgentEvent hello(const char* boot_id, uint64_t seq, const char* event_id) {
    mcco::AgentEvent ev;
    ev.event_id = event_id;
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::Hello;
    ev.payload_json = std::string("{\"protocol_version\":1,\"agent_version\":\"1.1.2\","
                                  "\"boot_id\":\"") +
                      boot_id + "\",\"hostname\":\"mac-b53478\"}";
    return ev;
}

mcco::AgentEvent state_changed(const char* state, uint64_t seq, const char* event_id) {
    mcco::AgentEvent ev;
    ev.event_id = event_id;
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::SystemStateChanged;
    ev.payload_json = std::string("{\"state\":\"") + state + "\"}";
    return ev;
}

mcco::AgentEvent screen_lock(bool locked, uint64_t seq, const char* event_id) {
    mcco::AgentEvent ev;
    ev.event_id = event_id;
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::ScreenLockChanged;
    ev.payload_json = locked ? "{\"locked\":true}" : "{\"locked\":false}";
    return ev;
}

mcco::AgentEvent heartbeat(uint64_t seq) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_hb";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.type = mcco::AgentEventType::Heartbeat;
    ev.payload_json = "{\"boot_id\":\"b_3F8A11\",\"uptime_s\":5}";
    return ev;
}

// Submit a power command in Mode B with an accepting agent gate.
mcco::SubmissionOutcome submit_power_b(Ctx& c, mcco::CommandType t, const char* hash) {
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(power_sub(t, hash));
    EXPECT_TRUE(out.ok);
    EXPECT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);
    return out;
}

} // namespace

TEST(EngineAgent, GateRejectsPreLedgerInModeA) {
    Ctx c;
    // No gate wired (Mode A): agent commands are refused pre-ledger.
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::AgentNotPaired);

    // Gate wired and refusing: the gate's deterministic 409 wins.
    c.engine.set_agent_gate([](const mcco::Submission&) {
        return std::optional<mcco::ErrCode>(mcco::ErrCode::AgentOffline);
    });
    out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error, mcco::ErrCode::AgentOffline);
}

TEST(EngineAgent, AppLaunchCompletesOnAckPlusStarted) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.record.mode_at_accept, 'B');

    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    ASSERT_EQ(rec->state, mcco::CommandState::Confirming);

    // ack alone does not complete (spec 5.3.1)
    ASSERT_TRUE(c.engine.agent_event(ack(out.record.command_id, 2, "launch_app")));
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming);

    // matching application_started completes the predicate
    ASSERT_TRUE(c.engine.agent_event(started("com.x", 3)));
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "app_launch_confirmed");
    EXPECT_EQ(rec->evidence.size(), 2);
}

TEST(EngineAgent, AppLaunchCompletesWhenStartedArrivesFirst) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));

    ASSERT_TRUE(c.engine.agent_event(started("com.x", 2)));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);
    ASSERT_TRUE(c.engine.agent_event(ack(out.record.command_id, 3, "launch_app")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "app_launch_confirmed");
}

TEST(EngineAgent, AppQuitCompletesOnAckPlusExited) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppQuit, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    ASSERT_TRUE(c.engine.agent_event(ack(out.record.command_id, 2, "quit_app")));
    ASSERT_TRUE(c.engine.agent_event(exited("com.x", 3, "quit")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "app_quit_confirmed");
}

TEST(EngineAgent, FailurePaths) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));

    // MCA-reported failure terminates with the closed error code.
    ASSERT_TRUE(c.engine.agent_event(result_failed(out.record.command_id, 2, "launch_failed")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "launch_failed");

    // Evidence after a terminal state is ignored, never an error (6.1.1).
    EXPECT_FALSE(c.engine.agent_event(started("com.x", 3)));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Failed);

    // crashed exit fails an in-flight quit.
    auto out2 = c.engine.submit(app_sub(mcco::CommandType::AppQuit, "com.y"));
    ASSERT_TRUE(out2.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out2.record.command_id, true));
    ASSERT_TRUE(c.engine.agent_event(ack(out2.record.command_id, 4, "quit_app")));
    ASSERT_TRUE(c.engine.agent_event(exited("com.y", 5, "crashed")));
    rec = c.engine.get(out2.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "app_crashed");
}

TEST(EngineAgent, DeadlineSweepAndEvidenceLoss) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    EXPECT_EQ(c.engine.get(out.record.command_id)->deadline_at, c.clock.epoch + 30);

    // Channel lost before the deadline: unconfirmed/evidence_lost (5.2.1).
    EXPECT_EQ(c.engine.on_agent_offline(), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(rec->result, "evidence_lost");

    // Past the deadline without offline onset: timed_out/deadline_exceeded.
    // The sweep runs on the monotonic clock (epoch goes garbage across the
    // SNTP jump), so both clocks advance here.
    auto out2 = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.y"));
    ASSERT_TRUE(out2.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out2.record.command_id, true));
    c.clock.advance_seconds(31);
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    rec = c.engine.get(out2.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(EngineAgent, DeclaredOfflineMarksConfirmingRecords) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // agent_goodbye(sleep): the record gets the expected-offline window (5.3.2).
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming); // still confirming
    EXPECT_TRUE(rec->has_window);
    EXPECT_EQ(rec->window_open_after_s, 3);
    EXPECT_EQ(rec->window_close_after_s, 60);

    // A subsequent offline sweep must NOT sweep the windowed record.
    EXPECT_EQ(c.engine.on_agent_offline(), 0);
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming);

    // Non-confirming records are untouched.
    mcco::Submission sub2 = app_sub(mcco::CommandType::AppLaunch, "com.y");
    sub2.body_hash = "h2"; // distinct hash: derived coalescing must not replay
    auto out2 = c.engine.submit(sub2);
    ASSERT_TRUE(out2.ok);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0); // accepted, not confirming
    const mcco::CommandRecord* rec2 = c.engine.get(out2.record.command_id);
    EXPECT_EQ(rec2->state, mcco::CommandState::Accepted);
    EXPECT_FALSE(rec2->has_window);

    // A second declared-offline call is idempotent (already has_window).
    ASSERT_TRUE(c.engine.complete_dispatch(out2.record.command_id, true));
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 1); // only the new record
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0); // nothing left to mark
}

TEST(EngineAgent, ModeBPowerCommandStaysConfirmingAfterDispatch) {
    // Phase 5 behavior change (PRD §17.2.1): in Mode B every command type —
    // power commands included — stays `confirming` after successful dispatch;
    // the terminal verdict arrives via the §5.3.1/§8 predicates or the
    // deadline sweep. This replaces the interim Phase 4
    // "power commands terminate unconfirmed/hid_only even in Mode B" pin.
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(power_sub(mcco::CommandType::Lock, "h"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming);
    EXPECT_EQ(rec->deadline_at, c.clock.epoch + 15);

    // Without predicate evidence the deadline sweep ends it timed_out.
    c.clock.advance_seconds(16);
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(EngineAgent, ModeBPowerCommandsRequireLiveSession) {
    Ctx c;
    c.engine.set_mode('B');
    int gate_calls = 0;
    c.engine.set_agent_gate([&](const mcco::Submission&) {
        gate_calls++;
        return std::optional<mcco::ErrCode>(mcco::ErrCode::AgentOffline);
    });
    // lock/sleep/restart/shutdown are session-gated in Mode B (§12.3.1).
    for (mcco::CommandType t : {mcco::CommandType::Lock, mcco::CommandType::Sleep,
                                mcco::CommandType::Restart, mcco::CommandType::Shutdown}) {
        auto out = c.engine.submit(power_sub(t, std::string("h-") + mcco::command_type_to_string(t)));
        EXPECT_FALSE(out.ok);
        EXPECT_EQ(out.error, mcco::ErrCode::AgentOffline);
    }
    EXPECT_EQ(gate_calls, 4);

    // wake is exempt: at wake time the Mac is asleep and the session is
    // necessarily dead — wake is gated on pairing only (§8.1.2).
    auto wake = c.engine.submit(power_sub(mcco::CommandType::Wake, "h-wake"));
    EXPECT_TRUE(wake.ok);
    EXPECT_EQ(gate_calls, 4); // gate not consulted for wake
}


// ---- Step 0: result-admission conformance (pins the gate-run loss scenario)

TEST(EngineAgentConformance, LateResultAfterTimeoutIgnored) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));

    c.clock.advance_seconds(31); // app deadline is 30 s
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    ASSERT_EQ(rec->state, mcco::CommandState::TimedOut);
    const uint32_t rev = rec->revision;

    // A redelivered command_result after the timeout MUST NOT reopen the
    // terminal record (spec 5.1.1/6.1.1).
    EXPECT_FALSE(c.engine.agent_event(result_failed(out.record.command_id, 9, "action_timeout")));
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->revision, rev);
}

TEST(EngineAgentConformance, DuplicateResultOverCompletedIgnored) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    ASSERT_TRUE(c.engine.agent_event(ack(out.record.command_id, 2, "launch_app")));
    ASSERT_TRUE(c.engine.agent_event(started("com.x", 3)));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    ASSERT_EQ(rec->state, mcco::CommandState::Completed);
    const uint32_t rev = rec->revision;

    // The MCA redelivers command_result(failed): terminal record never reopens.
    EXPECT_FALSE(c.engine.agent_event(result_failed(out.record.command_id, 4, "action_timeout")));
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "app_launch_confirmed");
    EXPECT_EQ(rec->revision, rev);
}

TEST(EngineAgentConformance, DuplicateEvidenceDeduplicated) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.x"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));

    // Same event_id redelivered while confirming: exactly one evidence entry.
    mcco::AgentEvent dup = started("com.x", 2);
    dup.event_id = "evt_dup";
    ASSERT_TRUE(c.engine.agent_event(dup));
    ASSERT_TRUE(c.engine.agent_event(dup));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming);
    ASSERT_EQ(rec->evidence.size(), 1u);
    EXPECT_EQ(rec->evidence[0], "evt_dup");
}

// ---- Step 1: interval arithmetic on the monotonic clock

TEST(EngineAgentMono, DeadlineSweepIgnoresEpochSntpJump) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto a = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.a"));
    mcco::Submission sub_b = app_sub(mcco::CommandType::AppLaunch, "com.b");
    sub_b.body_hash = "h-b"; // distinct hash: derived coalescing must not replay
    auto b = c.engine.submit(sub_b);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(a.record.command_id, true));
    ASSERT_TRUE(c.engine.complete_dispatch(b.record.command_id, true));

    // SNTP sync jumps the epoch clock forward mid-session: no interval may
    // be computed from it (HANDOFF conventions).
    c.clock.epoch += 86400;
    EXPECT_EQ(c.engine.sweep_deadlines(), 0);
    EXPECT_EQ(c.engine.get(a.record.command_id)->state, mcco::CommandState::Confirming);

    // The monotonic clock reaching the deadline is what times out.
    c.clock.advance_ms(31000);
    EXPECT_EQ(c.engine.sweep_deadlines(), 2);
    EXPECT_EQ(c.engine.get(a.record.command_id)->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(c.engine.get(b.record.command_id)->state, mcco::CommandState::TimedOut);
}

TEST(EngineAgentMono, BootReconcileResumesConfirmingSleep) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    CapturingLog log;
    const std::string command_id = [&] {
        mcco::Ledger ledger(storage, clock, log, 64);
        mcco::CommandEngine engine(ledger, clock, rng, log);
        engine.set_mode('B');
        engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
        engine.init();
        auto out = engine.submit(power_sub(mcco::CommandType::Sleep, "h"));
        EXPECT_TRUE(out.ok);
        EXPECT_TRUE(engine.complete_dispatch(out.record.command_id, true));
        return out.record.command_id;
    }(); // reboot with the record confirming and its window still open

    clock.advance_seconds(10); // 10 s of downtime, deadline still open
    mcco::Ledger ledger2(storage, clock, log, 64);
    mcco::CommandEngine engine2(ledger2, clock, rng, log);
    ASSERT_TRUE(engine2.init());

    const mcco::CommandRecord* rec = engine2.get(command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming); // resumed, not esp32_restarted
    EXPECT_TRUE(rec->has_window);
    EXPECT_EQ(rec->revision, 3); // accepted -> dispatched -> confirming (no extra revision)

    // The window restarts at reconciliation (monotonic sidecar is RAM-only);
    // close_after_s after the reboot completes it once offline evidence lands.
    EXPECT_EQ(engine2.on_agent_declared_offline(), 0); // nothing newly windowed
    clock.advance_seconds(61);
    EXPECT_EQ(engine2.sweep_windows(true), 1);
    rec = engine2.get(command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "sleep_confirmed");
}

// ---- Step 3: verification predicates (spec 5.3.1, §8)

TEST(EngineAgentPredicates, LockCompletesOnScreenLockChanged) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Lock, "h");

    // locked:false does not advance the predicate.
    EXPECT_FALSE(c.engine.agent_event(screen_lock(false, 2, "evt_unlock")));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    ASSERT_TRUE(c.engine.agent_event(screen_lock(true, 3, "evt_lock")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "lock_confirmed");
    ASSERT_EQ(rec->evidence.size(), 1u);
    EXPECT_EQ(rec->evidence[0], "evt_lock");
}

TEST(EngineAgentPredicates, ModeBWakeFailedActuationStaysConfirmingForEvidence) {
    // HID wake actuation is best-effort: a deeply sleeping host powers the
    // USB port off, so the keypress cannot succeed — but the §8.1.2 evidence
    // (post-dispatch new-session hello + awake burst, e.g. from a human
    // keypress) can still confirm the wake. A failed actuation must advance
    // the record to confirming, not terminate it dispatch_error (AT-08 wake
    // rounds, 2026-09-30). Mode A keeps the immediate honest failure.
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_agent_gate([](const mcco::Submission&) { return std::optional<mcco::ErrCode>(); });
    auto out = c.engine.submit(power_sub(mcco::CommandType::Wake, "h"));
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, false));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // The late evidence completes it, exactly like a successful actuation.
    c.engine.agent_event(hello("b_3F8A11", 1, "evt_hello"));
    ASSERT_TRUE(c.engine.agent_event(state_changed("awake", 2, "evt_awake")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "wake_confirmed");

    // Mode A: failed actuation is an immediate honest failure.
    Ctx a;
    auto out_a = a.engine.submit(power_sub(mcco::CommandType::Wake, "h2"));
    ASSERT_TRUE(out_a.ok);
    ASSERT_TRUE(a.engine.complete_dispatch(out_a.record.command_id, false));
    const mcco::CommandRecord* rec_a = a.engine.get(out_a.record.command_id);
    EXPECT_EQ(rec_a->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec_a->error_code, "dispatch_error");
}

TEST(EngineAgentPredicates, WakeRequiresHelloThenAwakeBurst) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Wake, "h");

    // Heartbeat alone is insufficient (spec 5.3.1).
    EXPECT_FALSE(c.engine.agent_event(heartbeat(2)));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // The awake burst alone (no new-session hello) is insufficient.
    EXPECT_FALSE(c.engine.agent_event(state_changed("awake", 3, "evt_awake_early")));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // New-session hello, then the mandatory initial awake burst: confirmed.
    EXPECT_FALSE(c.engine.agent_event(hello("b_3F8A11", 1, "evt_hello")));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);
    ASSERT_TRUE(c.engine.agent_event(state_changed("awake", 2, "evt_awake")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "wake_confirmed");
}

TEST(EngineAgentPredicates, SleepCompletesAtCloseWhenOffline) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Sleep, "h");

    c.clock.advance_seconds(5); // inside [3 s, 60 s]
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0); // already windowed
    c.clock.advance_seconds(55); // window closes at dispatch + 60 s
    EXPECT_EQ(c.engine.sweep_windows(true), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "sleep_confirmed");
}

TEST(EngineAgentPredicates, SleepPreWindowOfflineIsEvidenceLost) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Sleep, "h");

    // Silence onset one second after dispatch: before open_after_s, the
    // offline cannot be attributed to the dispatch (§8) — and the epoch
    // deadline (90 s) must not save it.
    c.clock.advance_seconds(1);
    EXPECT_EQ(c.engine.on_agent_offline(), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(rec->result, "evidence_lost");
}

TEST(EngineAgentPredicates, SleepReconnectBeforeCloseRefutes) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Sleep, "h");

    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);
    c.clock.advance_seconds(30); // 35 s: before the 60 s close
    EXPECT_EQ(c.engine.on_agent_hello("b_3F8A11", true, "b_3F8A11"), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "unexpected_wake");
}

TEST(EngineAgentPredicates, RestartHappyPath) {
    Ctx c;
    c.engine.set_known_boot_id("b_OLD"); // glue-hydrated baseline (§8.2.1)
    auto out = submit_power_b(c, mcco::CommandType::Restart, "h");

    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0); // phase 1 evidence

    c.clock.advance_seconds(35); // 40 s after dispatch
    EXPECT_EQ(c.engine.on_agent_hello("b_NEW", true, "b_OLD"), 0); // progress, no revision
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    ASSERT_TRUE(c.engine.agent_event(state_changed("awake", 1, "evt_awake")));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "restart_confirmed");
}

TEST(EngineAgentPredicates, RestartUnchangedBootVoidsEvidenceThenTimesOut) {
    Ctx c;
    c.engine.set_known_boot_id("b_OLD");
    auto out = submit_power_b(c, mcco::CommandType::Restart, "h");

    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);
    c.clock.advance_seconds(35);
    // Same boot_id: an agent-process restart, not a machine reboot — phase 1
    // evidence is voided, the contradiction is logged, and the record stays
    // confirming until the deadline sweep ends it (§8.2.1).
    EXPECT_EQ(c.engine.on_agent_hello("b_OLD", true, "b_OLD"), 0);
    EXPECT_TRUE(c.log.has("unexpected_reconnect"));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Confirming);

    c.clock.advance_seconds(141); // past the 180 s restart deadline
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(EngineAgentPredicates, ShutdownProbeFailuresConfirm) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Shutdown, "h");

    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);
    c.clock.advance_seconds(55); // past close: probe phase (§8.2.2)

    EXPECT_EQ(c.engine.on_shutdown_probes(false), 0);
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);
    EXPECT_EQ(c.engine.on_shutdown_probes(false), 0);
    EXPECT_EQ(c.engine.on_shutdown_probes(false), 1); // third consecutive failure
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "shutdown_confirmed");
}

TEST(EngineAgentPredicates, ShutdownProbeReplyFailsHostStillReachable) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Shutdown, "h");
    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);

    EXPECT_EQ(c.engine.on_shutdown_probes(true), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "host_still_reachable");
}

TEST(EngineAgentPredicates, ShutdownReconnectRefutes) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Shutdown, "h");
    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);

    EXPECT_EQ(c.engine.on_agent_hello("b_OLD", true, "b_OLD"), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Failed);
    EXPECT_EQ(rec->error_code, "unexpected_reconnect");
}

TEST(EngineAgentPredicates, ShutdownProbeOpenGatesTheGlueProbePhase) {
    // shutdown_probe_open is the glue's (power_probe) gate: true only while a
    // shutdown record is confirming + windowed, past close on the monotonic
    // clock, with the channel offline (§8.2.2).
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Shutdown, "h");

    c.clock.advance_seconds(5);
    EXPECT_FALSE(c.engine.shutdown_probe_open(true)); // window still open
    EXPECT_FALSE(c.engine.shutdown_probe_open(false)); // channel online: never

    c.clock.advance_seconds(56); // 61 s after dispatch: past close
    EXPECT_TRUE(c.engine.shutdown_probe_open(true));
    EXPECT_FALSE(c.engine.shutdown_probe_open(false));

    // Terminal records close the phase (three failed rounds complete it).
    EXPECT_EQ(c.engine.on_shutdown_probes(false), 0);
    EXPECT_EQ(c.engine.on_shutdown_probes(false), 0);
    EXPECT_EQ(c.engine.on_shutdown_probes(false), 1);
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Completed);
    EXPECT_FALSE(c.engine.shutdown_probe_open(true));
}

TEST(EngineAgentPredicates, ShutdownProbeOpenIgnoresOtherTypes) {
    Ctx c;
    submit_power_b(c, mcco::CommandType::Sleep, "h");
    c.clock.advance_seconds(61); // past close
    EXPECT_FALSE(c.engine.shutdown_probe_open(true)); // sleep needs no probes
}

TEST(EngineAgentPredicates, PostTerminalReconnectLoggedNotReopened) {
    Ctx c;
    auto out = submit_power_b(c, mcco::CommandType::Sleep, "h");
    c.clock.advance_seconds(5);
    EXPECT_EQ(c.engine.on_agent_declared_offline(), 0);
    c.clock.advance_seconds(55);
    EXPECT_EQ(c.engine.sweep_windows(true), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    ASSERT_EQ(rec->state, mcco::CommandState::Completed);
    const uint32_t rev = rec->revision;

    // A reconnect after the completed sleep is logged as unexpected_wake
    // evidence and MUST NOT alter the terminal record (§5.3.2).
    EXPECT_EQ(c.engine.on_agent_hello("b_3F8A11", true, "b_3F8A11"), 0);
    EXPECT_TRUE(c.log.has("unexpected_wake"));
    rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->revision, rev);
}

// ---- Step 4: macro expected_event predicates (spec 10.3.1)

TEST(EngineAgentMacro, ExpectedEventCompletesMacro) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_macro_resolver([](const std::string& macro_id) {
        mcco::CommandEngine::MacroResolution r;
        if (macro_id == "mac_3F81") {
            r.found = true;
            r.timeout_ms = 10000;
            r.has_expected_event = true;
            r.expected_event_type = "application_started";
            r.expected_match_json = "{\"bundle_id\":\"com.x\"}";
        }
        return r;
    });
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"mac_3F81"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";
    auto out = c.engine.submit(sub);
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.mark_dispatched(out.record.command_id, 15));

    // Mode B + expected_event: stays confirming after interpretation.
    ASSERT_TRUE(c.engine.macro_interpret_done(out.record.command_id));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // Wrong bundle_id does not match...
    EXPECT_FALSE(c.engine.agent_event(started("com.other", 2)));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // ...the matching application_started completes it.
    mcco::AgentEvent ev = started("com.x", 3);
    ev.event_id = "evt_started";
    ASSERT_TRUE(c.engine.agent_event(ev));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::Completed);
    EXPECT_EQ(rec->result, "macro_confirmed");
    ASSERT_EQ(rec->evidence.size(), 1u);
    EXPECT_EQ(rec->evidence[0], "evt_started");
}

TEST(EngineAgentMacro, ExpectedEventDeadlineTimesOut) {
    Ctx c;
    c.engine.set_mode('B');
    c.engine.set_macro_resolver([](const std::string&) {
        mcco::CommandEngine::MacroResolution r;
        r.found = true;
        r.has_expected_event = true;
        r.expected_event_type = "screen_lock_changed";
        r.expected_match_json = "{\"locked\":true}";
        return r;
    });
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"mac_3F81"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";
    auto out = c.engine.submit(sub);
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.mark_dispatched(out.record.command_id, 15)); // timeout_ms/1000 + 5
    ASSERT_TRUE(c.engine.macro_interpret_done(out.record.command_id));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Confirming);

    // Non-matching payload (locked:false): still confirming.
    EXPECT_FALSE(c.engine.agent_event(screen_lock(false, 2, "evt_unlock")));
    c.clock.advance_seconds(16);
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(EngineAgentMacro, WithoutExpectedEventTerminatesUnconfirmed) {
    Ctx c;
    c.engine.set_macro_resolver([](const std::string&) {
        mcco::CommandEngine::MacroResolution r;
        r.found = true; // no expected_event: unverifiable even in Mode B
        return r;
    });
    for (char mode : {'A', 'B'}) {
        c.engine.set_mode(mode);
        mcco::Submission sub;
        sub.type = mcco::CommandType::MacroExecute;
        sub.parameters_json = R"({"macro_id":"mac_3F81"})";
        sub.requested_by = "apikey:key-01";
        sub.body_hash = std::string("h-") + mode;
        auto out = c.engine.submit(sub);
        ASSERT_TRUE(out.ok);
        ASSERT_TRUE(c.engine.mark_dispatched(out.record.command_id, 15));
        ASSERT_TRUE(c.engine.macro_interpret_done(out.record.command_id));
        const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
        EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
        EXPECT_EQ(rec->result, "hid_only");
    }
}

TEST(EngineAgentMacro, ModeAExpectedEventStillUnconfirmed) {
    Ctx c;
    c.engine.set_mode('A'); // Mode A: completed is unreachable, expected_event or not
    c.engine.set_macro_resolver([](const std::string&) {
        mcco::CommandEngine::MacroResolution r;
        r.found = true;
        r.has_expected_event = true;
        r.expected_event_type = "application_started";
        r.expected_match_json = "{\"bundle_id\":\"com.x\"}";
        return r;
    });
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = R"({"macro_id":"mac_3F81"})";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";
    auto out = c.engine.submit(sub);
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.mark_dispatched(out.record.command_id, 15));
    ASSERT_TRUE(c.engine.macro_interpret_done(out.record.command_id));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Unconfirmed);
    // Ambient events never advance a Mode A record.
    EXPECT_FALSE(c.engine.agent_event(started("com.x", 2)));
    EXPECT_EQ(c.engine.get(out.record.command_id)->state, mcco::CommandState::Unconfirmed);
}
