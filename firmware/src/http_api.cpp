#include "http_api.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <map>
#include "command_dispatcher.h"
#include "esp_clock.h"
#include "esp_rng.h"
#include "log_sink.h"
#include "mc_auth.h"
#include "mc_engine.h"
#include "mc_error.h"
#include "mc_ids.h"
#include "mc_iso8601.h"
#include "mc_ledger.h"
#include "mc_log.h"
#include "mc_rate_limit.h"
#include "mc_sha256.h"
#include "status_cache.h"
#include "wifi_mgr.h"

namespace {

constexpr uint32_t kHeaderCap = 8192;
constexpr uint32_t kBodyCap = 16384;
constexpr uint32_t kIoTimeoutMs = 5000;

struct Request {
    std::string method;
    std::string path; // without query
    std::string query;
    std::string body;
    std::string auth;           // raw Authorization header value
    std::string idempotency_key; // Idempotency-Key header value
};

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
    }
    return "Internal Server Error";
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string toLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

char hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size() && hexVal(in[i + 1]) >= 0 && hexVal(in[i + 2]) >= 0) {
            out += (char)((hexVal(in[i + 1]) << 4) | hexVal(in[i + 2]));
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

// Query parameter lookup; returns true and fills `out` when present.
bool queryParam(const std::string& query, const char* name, std::string& out) {
    size_t pos = 0;
    const std::string prefix = std::string(name) + "=";
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos
                                                                      : amp - pos);
        if (pair.compare(0, prefix.size(), prefix) == 0) {
            out = urlDecode(pair.substr(prefix.size()));
            return true;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// ---- Failed-auth lockout (spec 13.1.1): 10 consecutive failures from one
// source IP -> 60 s lockout answering 401.
struct AuthTrack {
    char ip[40];
    uint32_t consecutive_fails = 0;
    uint64_t lockout_until_ms = 0;
    bool used = false;
};
constexpr size_t kAuthTrackSlots = 8;
AuthTrack g_auth_track[kAuthTrackSlots];
Mutex g_auth_track_mutex;

AuthTrack& trackFor(const char* ip) {
    for (auto& t : g_auth_track) {
        if (t.used && strcmp(t.ip, ip) == 0) return t;
    }
    for (auto& t : g_auth_track) {
        if (!t.used) {
            t.used = true;
            strncpy(t.ip, ip, sizeof(t.ip) - 1);
            t.ip[sizeof(t.ip) - 1] = '\0';
            return t;
        }
    }
    return g_auth_track[0]; // table full: evict the oldest slot's contents
}

} // namespace

void HttpApi::begin(AppContext* ctx, uint16_t port) {
    ctx_ = ctx;
    server_ = new WiFiServer(port);
    server_->begin();
    xTaskCreate(taskEntry, "mc_http", 12288, this, 5, &task_);
    esp_task_wdt_add(task_);
}

void HttpApi::taskEntry(void* arg) {
    HttpApi* self = static_cast<HttpApi*>(arg);
    for (;;) {
        esp_task_wdt_reset();
        if (!self->ctx_->wifi->connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        WiFiClient client = self->server_->accept();
        if (client) {
            client.setNoDelay(true);
            self->handleClient(client);
            client.stop();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void HttpApi::handleClient(WiFiClient& client) {
    AppContext* ctx = ctx_;
    const std::string request_id = mcco::make_request_id(*ctx->rng);

    // ---- Read headers (up to the blank line).
    std::string header_block;
    header_block.reserve(1024);
    uint32_t start_ms = (uint32_t)millis();
    while (header_block.find("\r\n\r\n") == std::string::npos) {
        if (header_block.size() >= kHeaderCap || (uint32_t)millis() - start_ms > kIoTimeoutMs ||
            !client.connected()) {
            return; // too large / slow / gone: close silently
        }
        while (client.available()) {
            int ch = client.read();
            if (ch < 0) break;
            header_block += (char)ch;
            if (header_block.size() >= kHeaderCap) break;
        }
        if (header_block.find("\r\n\r\n") == std::string::npos) vTaskDelay(pdMS_TO_TICKS(2));
    }

    Request req;
    size_t line_end = header_block.find("\r\n");
    std::string request_line = header_block.substr(0, line_end);
    {
        size_t sp1 = request_line.find(' ');
        size_t sp2 = request_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        req.method = request_line.substr(0, sp1);
        std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        size_t q = target.find('?');
        req.path = (q == std::string::npos) ? target : target.substr(0, q);
        req.query = (q == std::string::npos) ? "" : target.substr(q + 1);
    }
    // Headers.
    uint32_t content_length = 0;
    {
        size_t pos = line_end + 2;
        size_t end = header_block.find("\r\n\r\n");
        while (pos < end) {
            size_t eol = header_block.find("\r\n", pos);
            if (eol == std::string::npos || eol > end) eol = end;
            std::string hline = header_block.substr(pos, eol - pos);
            size_t colon = hline.find(':');
            if (colon != std::string::npos) {
                std::string name = toLower(trim(hline.substr(0, colon)));
                std::string value = trim(hline.substr(colon + 1));
                if (name == "content-length") {
                    content_length = (uint32_t)strtoul(value.c_str(), nullptr, 10);
                } else if (name == "authorization") {
                    req.auth = value;
                } else if (name == "idempotency-key") {
                    req.idempotency_key = value;
                }
            }
            pos = eol + 2;
        }
    }
    if (content_length > kBodyCap) {
        // Body too large for the fixed parsing buffer: refuse cleanly.
        std::string body = "{\"error\":{\"code\":\"bad_request\",\"message\":\"Request body too "
                           "large\",\"request_id\":\"" +
                           request_id + "\"}}";
        std::string hdrs = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        client.write(hdrs.data(), hdrs.size());
        client.write(body.data(), body.size());
        return;
    }
    // Any bytes of the body already read past the header terminator.
    size_t hdr_total = header_block.find("\r\n\r\n") + 4;
    req.body = header_block.substr(hdr_total);
    start_ms = (uint32_t)millis();
    while (req.body.size() < content_length) {
        if ((uint32_t)millis() - start_ms > kIoTimeoutMs || !client.connected()) return;
        while (client.available() && req.body.size() < content_length) {
            int ch = client.read();
            if (ch < 0) break;
            req.body += (char)ch;
        }
        if (req.body.size() < content_length) vTaskDelay(pdMS_TO_TICKS(2));
    }
    req.body.resize(content_length);

    auto sendRaw = [&](int status, const std::string& body) {
        std::string hdrs = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        client.write(hdrs.data(), hdrs.size());
        client.write(body.data(), body.size());
        client.flush();
    };
    auto sendError = [&](mcco::ErrCode code, const char* message = nullptr) {
        JsonDocument doc;
        JsonObject err = doc["error"].to<JsonObject>();
        err["code"] = mcco::error_code_string(code);
        err["message"] = message ? message : mcco::default_error_message(code);
        err["request_id"] = request_id;
        std::string body;
        serializeJson(doc, body);
        sendRaw(mcco::error_http_status(code), body);
        ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "http_error", nullptr,
                        request_id.c_str(), nullptr, nullptr);
    };
    auto sendJson = [&](int status, const JsonDocument& doc) {
        std::string body;
        serializeJson(doc, body);
        sendRaw(status, body);
    };

    // ---- Agent surface: no pairing exists in Phase 1 (spec 4.3.1).
    if (req.path.compare(0, 10, "/agent/v1/") == 0) {
        sendError(mcco::ErrCode::Unauthorized);
        return;
    }

    // ---- Association lost while serving (spec 15.1).
    if (!ctx->wifi->connected()) {
        sendError(mcco::ErrCode::NetworkUnavailable);
        return;
    }

    // ---- Authentication + lockout + rate limit.
    const std::string ip = client.remoteIP().toString().c_str();
    {
        Guard g(g_auth_track_mutex);
        AuthTrack& t = trackFor(ip.c_str());
        if (t.lockout_until_ms != 0 && ctx->clock->millis() < t.lockout_until_ms) {
            sendError(mcco::ErrCode::Unauthorized);
            return;
        }
        if (t.lockout_until_ms != 0) {
            t.lockout_until_ms = 0;
            t.consecutive_fails = 0;
        }
    }
    std::string raw_key;
    const std::string bearer = "Bearer ";
    if (req.auth.compare(0, bearer.size(), bearer) == 0) raw_key = trim(req.auth.substr(bearer.size()));

    mcco::Principal principal;
    mcco::AuthResult auth = ctx->keys->authenticate(raw_key, ctx->clock->epoch_seconds(), principal);
    if (auth != mcco::AuthResult::Ok) {
        Guard g(g_auth_track_mutex);
        AuthTrack& t = trackFor(ip.c_str());
        ++t.consecutive_fails;
        if (t.consecutive_fails >= 10) {
            t.lockout_until_ms = ctx->clock->millis() + 60000;
            t.consecutive_fails = 0;
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "auth_lockout",
                            nullptr, request_id.c_str(), (std::string("ip:") + ip).c_str(),
                            nullptr);
        } else {
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "auth_failed",
                            nullptr, request_id.c_str(), (std::string("ip:") + ip).c_str(),
                            nullptr);
        }
        if (auth == mcco::AuthResult::Revoked) {
            sendError(mcco::ErrCode::Forbidden);
        } else {
            sendError(mcco::ErrCode::Unauthorized); // missing/invalid or expired
        }
        return;
    }
    {
        Guard g(g_auth_track_mutex);
        AuthTrack& t = trackFor(ip.c_str());
        t.consecutive_fails = 0;
    }
    if (!ctx->limiter->allow(principal.key_id, principal.role)) {
        sendError(mcco::ErrCode::RateLimited);
        return;
    }

    // Mark key use (attribution, spec 13.1.1). Persisted lazily by main loop.
    ctx->keys->touch(principal.key_id, ctx->clock->epoch_seconds());
    ctx->keys_dirty = true;

    const std::string actor = "apikey:" + principal.key_id;
    auto roleCheck = [&](mcco::Role need) -> bool {
        if (!mcco::role_at_least(principal.role, need)) {
            sendError(mcco::ErrCode::Forbidden);
            return false;
        }
        return true;
    };

    // ---- Routing (closed surface; everything else 404 not_found).
    if (req.method == "GET" && req.path == "/api/v1/status") {
        if (!roleCheck(mcco::Role::Read)) return;
        JsonDocument doc;
        ctx->status_cache->buildStatus(doc);
        sendJson(200, doc);
        return;
    }
    if (req.method == "GET" && req.path == "/api/v1/capabilities") {
        if (!roleCheck(mcco::Role::Read)) return;
        JsonDocument doc;
        ctx->status_cache->buildCapabilities(doc);
        sendJson(200, doc);
        return;
    }
    if (req.method == "POST" && req.path == "/api/v1/commands") {
        if (!roleCheck(mcco::Role::Control)) return;

        JsonDocument doc;
        if (deserializeJson(doc, req.body) || !doc.is<JsonObject>()) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        bool bad_field = false;
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
            const char* k = kv.key().c_str();
            if (strcmp(k, "type") != 0 && strcmp(k, "parameters") != 0 &&
                strcmp(k, "idempotency_key") != 0)
                bad_field = true;
        }
        if (bad_field) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        JsonVariantConst type_var = doc["type"];
        if (!type_var.is<const char*>()) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        const char* type_str = type_var.as<const char*>();
        mcco::CommandType type;
        if (!mcco::command_type_from_string(type_str, type)) {
            sendError(mcco::ErrCode::BadRequest); // unknown type
            return;
        }
        std::string params = "{}";
        if (doc["parameters"].is<JsonObjectConst>()) {
            serializeJson(doc["parameters"], params);
        } else if (doc["parameters"].isNull() == false) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        std::string idem;
        JsonVariantConst idem_var = doc["idempotency_key"];
        if (idem_var.is<const char*>()) {
            idem = idem_var.as<const char*>();
        } else if (!req.idempotency_key.empty()) {
            idem = req.idempotency_key;
        }

        mcco::Submission sub;
        sub.type = type;
        sub.parameters_json = params;
        sub.idempotency_key = idem;
        sub.requested_by = actor;
        sub.body_hash = mcco::Sha256::hex_digest(req.body);

        mcco::SubmissionOutcome out;
        {
            Guard g(ctx->engine_mutex);
            out = ctx->engine->submit(sub);
        }
        if (!out.ok) {
            sendError(out.error);
            return;
        }
        ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "command_accepted",
                        out.record.command_id.c_str(), request_id.c_str(), actor.c_str(), nullptr);

        JsonDocument resp;
        resp["command_id"] = out.record.command_id;
        resp["state"] = mcco::command_state_to_string(out.record.state);
        resp["deadline_at"] = mcco::iso8601_format(out.record.deadline_at);
        resp["record_url"] = "/api/v1/commands/" + out.record.command_id;
        sendJson(out.http_status, resp);

        if (out.dispatch_pending) ctx->dispatcher->enqueue(out.record.command_id);
        return;
    }
    if (req.method == "GET" && req.path == "/api/v1/commands") {
        if (!roleCheck(mcco::Role::Read)) return;

        mcco::CommandEngine::ListFilter filter;
        std::string v;
        if (queryParam(req.query, "state", v)) {
            if (!mcco::command_state_from_string(v.c_str(), filter.state)) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            filter.has_state = true;
        }
        if (queryParam(req.query, "type", v)) {
            if (!mcco::command_type_from_string(v.c_str(), filter.type)) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            filter.has_type = true;
        }
        if (queryParam(req.query, "since", v)) {
            if (!mcco::iso8601_parse(v, filter.since)) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            filter.has_since = true;
        }
        size_t limit = 50;
        if (queryParam(req.query, "limit", v)) {
            char* endp = nullptr;
            unsigned long n = strtoul(v.c_str(), &endp, 10);
            if (!endp || *endp != '\0' || n == 0) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            limit = n > 200 ? 200 : (size_t)n;
        }
        // Keyset cursor: base64url of "<requested_at>:<command_id>" of the last
        // returned item (offsets are unstable under FIFO eviction).
        bool has_cursor = false;
        uint64_t cursor_ts = 0;
        std::string cursor_id;
        if (queryParam(req.query, "cursor", v)) {
            uint8_t buf[96];
            size_t out_len = 0;
            if (v.size() > 128 || !mcco::base64url_decode(v, buf, out_len) ||
                out_len >= sizeof(buf)) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            std::string decoded(reinterpret_cast<const char*>(buf), out_len);
            size_t colon = decoded.find(':');
            if (colon == std::string::npos) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
            cursor_ts = strtoull(decoded.substr(0, colon).c_str(), nullptr, 10);
            cursor_id = decoded.substr(colon + 1);
            has_cursor = true;
        }

        std::string body = "{\"commands\":[";
        bool first = true;
        std::string next_cursor;
        std::string cursor_payload;
        {
            Guard g(ctx->engine_mutex);
            std::vector<const mcco::CommandRecord*> all = ctx->engine->list(filter);
            size_t taken = 0;
            for (const mcco::CommandRecord* rec : all) {
                if (taken >= limit) {
                    // More items remain: emit the cursor of the last returned.
                    next_cursor = mcco::base64url_encode(
                        reinterpret_cast<const uint8_t*>(cursor_payload.data()),
                        cursor_payload.size());
                    break;
                }
                if (has_cursor &&
                    !(rec->requested_at < cursor_ts ||
                      (rec->requested_at == cursor_ts && rec->command_id < cursor_id)))
                    continue; // strictly older than the cursor
                if (!first) body += ",";
                first = false;
                body += mcco::Ledger::record_to_json(*rec);
                cursor_payload = std::to_string(rec->requested_at) + ":" + rec->command_id;
                ++taken;
            }
        }
        body += "],\"next_cursor\":";
        if (next_cursor.empty()) {
            body += "null";
        } else {
            body += "\"" + next_cursor + "\"";
        }
        body += "}";
        sendRaw(200, body);
        return;
    }
    if (req.method == "GET" && req.path.compare(0, 17, "/api/v1/commands/") == 0) {
        if (!roleCheck(mcco::Role::Read)) return;
        std::string id = req.path.substr(17);
        if (id.empty() || id.find('/') != std::string::npos) {
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        std::string body;
        {
            Guard g(ctx->engine_mutex);
            const mcco::CommandRecord* rec = ctx->engine->get(id);
            if (!rec) {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            body = mcco::Ledger::record_to_json(*rec);
        }
        sendRaw(200, body);
        return;
    }
    if (req.method == "POST" && req.path.compare(0, 15, "/api/v1/system/") == 0) {
        if (!roleCheck(mcco::Role::Control)) return;
        std::string name = req.path.substr(15);
        mcco::CommandType type;
        if (name.empty() || !mcco::command_type_from_string(name.c_str(), type)) {
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        // The convenience surface is exactly the five power commands (spec
        // 12.1.1); anything else on this prefix stays 404 (closed surface).
        switch (type) {
            case mcco::CommandType::Wake:
            case mcco::CommandType::Sleep:
            case mcco::CommandType::Restart:
            case mcco::CommandType::Shutdown:
            case mcco::CommandType::Lock:
                break;
            default:
                sendError(mcco::ErrCode::NotFound);
                return;
        }
        // Pure alias of POST /api/v1/commands {"type":name} (spec 12.1.1):
        // the body hash is computed over the canonical submission text so the
        // ledger record is identical to the generic form.
        std::string canonical = std::string("{\"type\":\"") + name + "\"}";
        mcco::Submission sub;
        sub.type = type;
        sub.parameters_json = "{}";
        sub.idempotency_key = req.idempotency_key;
        sub.requested_by = actor;
        sub.body_hash = mcco::Sha256::hex_digest(canonical);

        mcco::SubmissionOutcome out;
        {
            Guard g(ctx->engine_mutex);
            out = ctx->engine->submit(sub);
        }
        if (!out.ok) {
            sendError(out.error);
            return;
        }
        ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "command_accepted",
                        out.record.command_id.c_str(), request_id.c_str(), actor.c_str(), nullptr);
        JsonDocument resp;
        resp["command_id"] = out.record.command_id;
        resp["state"] = mcco::command_state_to_string(out.record.state);
        resp["deadline_at"] = mcco::iso8601_format(out.record.deadline_at);
        resp["record_url"] = "/api/v1/commands/" + out.record.command_id;
        sendJson(out.http_status, resp);
        if (out.dispatch_pending) ctx->dispatcher->enqueue(out.record.command_id);
        return;
    }

    // Closed surface: anything not listed above is 404 not_found (spec 13.3).
    sendError(mcco::ErrCode::NotFound);
}
