#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <WiFiClient.h>
#include "app_context.h"
#include "mc_error.h"

// Signed dual-partition OTA (spec 15.3), device side. All Arduino/IDF code
// lives here in src/ (maccontrol_core stays pure C++17).
namespace ota {

// setup(): detect a pending-verify boot (post-apply self-check window) and a
// reverted boot (the sibling slot was invalidated by a rollback).
void begin(AppContext* ctx);
// loop(): confirm the new image on first successful status serve; roll back
// when the 60 s self-check window expires first (spec 15.3).
void tick(AppContext* ctx);
// GET /api/v1/status hook: marks the Web UI / status surface responsive.
void noteStatusServed(AppContext* ctx);

// POST /api/v1/ota/upload. Streams the raw container (64-byte signature ||
// image) from the socket straight to the inactive OTA slot in ~4 KB chunks —
// the body never touches the capped request buffer or a heap allocation
// (AGENTS.md: 320 KB RAM). `buffered` carries any body bytes the request
// parser already consumed past the header terminator before the intercept
// ran (a fast client sends headers+body in one flight) — they are consumed
// FIRST, then the remainder is read from the socket. Verifies the ECDSA
// signature over the SHA-256 of the written image BEFORE reporting success;
// on failure the slot write is aborted so it cannot boot. Writes the full
// HTTP response (Connection: close) on every path.
void handleUpload(AppContext* ctx, WiFiClient& client, uint32_t content_length,
                  const std::string& buffered, const char* request_id, const char* actor);

// POST /api/v1/ota/apply body: {} / {"force":bool}. Returns false on a
// malformed body (caller answers 400 bad_request).
bool parseApplyBody(const std::string& body, bool& force);

// POST /api/v1/ota/apply. Returns true with a 200 JSON body in resp when the
// boot-partition switch succeeded (caller sends the response, then calls
// rebootNow()); on false, `err` is the deterministic error to send. With
// force, every non-terminal command record is terminated
// failed/esp32_restarted via the engine BEFORE the switch (spec 15.3),
// reusing the boot-reconciliation verdict so behavior matches exactly.
bool apply(AppContext* ctx, bool force, const char* request_id, const char* actor,
           char* resp, size_t resp_cap, mcco::ErrCode& err);

// GET /api/v1/ota/status (READ): running/pending versions + flags.
void statusJson(char* buf, size_t cap);

// Reboot into the pending slot (after the apply response hit the wire).
void rebootNow();

} // namespace ota
