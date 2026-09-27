#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <WiFiServer.h>
#include <ArduinoJson.h>
#include "app_context.h"

// Synchronous HTTP/1.1 server on WiFiServer port 80: Connection: close, one
// request at a time, served from its own FreeRTOS task (28 KB stack; HWM
// measured 21 KB under AT load, ~7 KB headroom — Phase 5 trim from 40 KB).
// The closed controller surface of spec 12.1.1 / 13.3 (power commands + macros/
// triggers/identity + Phase 4 pairing administration) is implemented here;
// everything else — including /api/v1/exec, /api/v1/ota*, unversioned paths —
// is 404 not_found. The /agent/v1/* surface (spec 4.2/4.3) authenticates with
// the pairing token, not API keys, and is routed before the auth-key path;
// an upgraded /agent/v1/ws connection is handed to AgentLink by reference and
// must not be stopped here. GET / and /ui serve the single-page Web UI
// (always 200; the login form lives in the document), and a valid mc_session
// cookie is accepted as the ADMIN role for /api/v1/* (spec ch. 14).
// DEVIATIONS (Phase 3 reconciles with /openapi.json): GET/POST/PUT/DELETE
// /api/v1/triggers and POST /api/v1/device/identity are not in the spec
// ch.12 inventory; both are ADMIN-gated and exist because the Web UI owns
// trigger bindings and device identity (spec ch. 14 matrix).
// Fixed-capacity allocator backing the reusable route documents: a
// JsonDocument over this arena never touches the heap, so the per-request
// build/clear cycles cannot fragment it. First-fit over an address-ordered
// block list with coalescing (ArduinoJson pools strings and variants, then
// frees them individually and in bulk on clear()).
class JsonDocArena : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override;
    void deallocate(void* ptr) override;
    void* reallocate(void* ptr, size_t new_size) override;

private:
    struct alignas(8) Chunk {
        size_t size;   // payload bytes (multiple of 8)
        bool free;
        Chunk* next;   // address-ordered list
    };
    static constexpr size_t kCapacity = 3072;
    static constexpr size_t kMinPayload = 8;
    Chunk* init();
    Chunk* split(Chunk* c, size_t size);
    alignas(8) uint8_t buf_[kCapacity];
    Chunk* head_ = nullptr;
};

class HttpApi {
public:
    void begin(AppContext* ctx, uint16_t port = 80);
    TaskHandle_t taskHandle() const { return task_; }

private:
    static void taskEntry(void* arg);
    void handleClient(WiFiClient& client, uint32_t header_timeout_ms);

    AppContext* ctx_ = nullptr;
    WiFiServer* server_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool ws_handed_off_ = false; // upgraded /agent/v1/ws client now owned by AgentLink
    bool last_keepalive_ = false; // last request asked for connection reuse
    // Fairness preemption: a client accepted from the backlog while a
    // keep-alive connection sat in its idle wait (see handleClient).
    WiFiClient preempt_client_;
    // Hot-route reuse (single-threaded server: safe). The JSON documents sit
    // on fixed static arenas and clear() recycles them, the header block and
    // response-header buffer are reused members, and the serialization
    // scratch avoids a per-request std::string — the alloc/free churn
    // fragmented the heap until lwIP could not allocate TX buffers (an
    // uncaught bad_alloc in sendJson's reserve decoded from a panic
    // backtrace; abort -> reboot -> RAM state lost).
    JsonDocArena status_arena_;
    JsonDocArena caps_arena_;
    JsonDocument status_doc_{&status_arena_};
    JsonDocument caps_doc_{&caps_arena_};
    char json_scratch_[2560];
    std::string header_block_; // request header accumulation, cleared per request
    char hdr_buf_[384];        // response status-line + header block
};
