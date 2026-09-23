#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_engine.h"
#include <vector>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800;
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
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

struct Ctx {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger{storage, clock, log, 64};
    mcco::CommandEngine engine{ledger, clock, rng, log};
    Ctx() { engine.init(); }
};

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
    auto out2 = c.engine.submit(app_sub(mcco::CommandType::AppLaunch, "com.y"));
    ASSERT_TRUE(out2.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out2.record.command_id, true));
    c.clock.epoch += 31;
    EXPECT_EQ(c.engine.sweep_deadlines(), 1);
    rec = c.engine.get(out2.record.command_id);
    EXPECT_EQ(rec->state, mcco::CommandState::TimedOut);
    EXPECT_EQ(rec->error_code, "deadline_exceeded");
}

TEST(EngineAgent, ModeBPowerCommandStillNeverCompletes) {
    Ctx c;
    c.engine.set_mode('B');
    mcco::Submission sub;
    sub.type = mcco::CommandType::Lock;
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h";
    auto out = c.engine.submit(sub);
    ASSERT_TRUE(out.ok);
    ASSERT_TRUE(c.engine.complete_dispatch(out.record.command_id, true));
    const mcco::CommandRecord* rec = c.engine.get(out.record.command_id);
    // Interim Phase 4 behavior: power/lock predicates arrive in Phase 5, so
    // the record terminates honestly unconfirmed/hid_only — never completed.
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(rec->result, "hid_only");
}
