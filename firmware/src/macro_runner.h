#pragma once
#include <string>
#include "app_context.h"
#include "mc_engine.h"
#include "mc_error.h"

// Macro execution entry points (spec 10.3.1).

// Pre-ledger invocation gate shared by the HTTP execute endpoint, the Web UI
// RUN buttons and GPIO trigger firings: checks store_corrupt (spec 15.1),
// macro existence, and the macro queue depth (spec 10.3.1), then persists the
// accepted record and enqueues it. No ledger record is created on failure.
struct MacroExecOutcome {
    bool ok = false;
    mcco::ErrCode error = mcco::ErrCode::InternalError;
    mcco::SubmissionOutcome submit;
};
MacroExecOutcome submit_macro_execute(AppContext* ctx, const std::string& macro_id,
                                      const std::string& requested_by);

// Interprets one accepted macro_execute command end-to-end: dispatched
// revision first, then the step stream, then the Mode A terminal verdict
// (unconfirmed/hid_only on success; failed/macro_timeout, failed/
// usb_disconnected or failed/dispatch_delayed on abort paths, spec 10.3.1).
// Runs inside the dispatcher task; the dispatcher already serializes, so at
// most one macro executes at a time. Every ledger transition happens under
// ctx->engine_mutex; HID I/O happens outside it.
void run_macro(AppContext* ctx, const char* command_id);
