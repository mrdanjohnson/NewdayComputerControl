#include "http_api.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <map>
#include "command_dispatcher.h"
#include "agent_link.h"
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
#include "mc_openapi.h"
#include "mc_pairing.h"
#include "mc_rate_limit.h"
#include "mc_sha256.h"
#include "mdns_service.h"
#include "nvs_config.h"
#include "status_cache.h"
#include "trigger_store.h"
#include "macro_runner.h"
#include "mc_macro.h"
#include "web_ui.h"
#include "wifi_mgr.h"
#include "ws_server.h"

namespace {

constexpr uint32_t kHeaderCap = 8192;
constexpr uint32_t kBodyCap = 16384;
constexpr uint32_t kIoTimeoutMs = 5000;
// Idle bound between requests on a kept-alive connection: the server is
// single-threaded, so a long idle hold blocks every other client (an AT
// harness opening a fresh connection would queue behind it).
constexpr uint32_t kKeepAliveIdleMs = 3000;

struct Request {
    std::string method;
    std::string path; // without query
    std::string query;
    std::string body;
    std::string auth;           // raw Authorization header value
    std::string cookie;         // raw Cookie header value
    std::string idempotency_key; // Idempotency-Key header value
    std::string upgrade;        // raw Upgrade header value (WebSocket detection)
    std::string ws_key;         // Sec-WebSocket-Key (RFC 6455 handshake)
    std::string session_hdr;    // X-Session-Id (polling transport)
    bool keepalive = false;     // Connection: keep-alive (transport reuse)
};

// Authorization: Bearer <token> extraction; "" when absent/malformed.
// (trim is defined below; forward use via local copy.)
std::string bearerToken(const std::string& auth);

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

std::string bearerToken(const std::string& auth) {
    const std::string prefix = "Bearer ";
    if (auth.compare(0, prefix.size(), prefix) == 0) return trim(auth.substr(prefix.size()));
    return "";
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

// Cookie header lookup ("a=b; c=d"): returns the decoded value or "".
std::string cookieValue(const std::string& cookie, const char* name) {
    size_t pos = 0;
    const std::string prefix = std::string(name) + "=";
    while (pos <= cookie.size()) {
        size_t semi = cookie.find(';', pos);
        std::string pair = trim(cookie.substr(
            pos, semi == std::string::npos ? std::string::npos : semi - pos));
        if (pair.compare(0, prefix.size(), prefix) == 0) {
            std::string v = trim(pair.substr(prefix.size()));
            // Strip surrounding quotes if present.
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
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
    xTaskCreate(taskEntry, "mc_http", 10240, this, 5, &task_);
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
            self->ws_handed_off_ = false;
            self->handleClient(client, kIoTimeoutMs);
            // Connection reuse (transport churn wedges the lwIP/Wi-Fi stack
            // under sustained polling): honor keep-alive until the peer
            // closes, goes silent (per-request read timeout), or upgrades.
            while (self->last_keepalive_ && !self->ws_handed_off_ && client.connected()) {
                self->last_keepalive_ = false;
                self->handleClient(client, kKeepAliveIdleMs);
            }
            // An upgraded WebSocket connection now belongs to the agent task
            // (spec 4.2.1); the HTTP task must not touch it again.
            if (!self->ws_handed_off_) client.stop();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// WiFiClient::write() can return short counts (full socket buffer); every
// response path must loop. Single-byte Print writes also silently drop on
// EAGAIN — never stream JSON through Print.
static bool writeFully(WiFiClient& client, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = client.write(reinterpret_cast<const uint8_t*>(data + off), len - off);
        if (w <= 0) {
            if (!client.connected()) return false;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        off += (size_t)w;
    }
    return true;
}

void HttpApi::handleClient(WiFiClient& client, uint32_t header_timeout_ms) {
    AppContext* ctx = ctx_;
    const std::string request_id = mcco::make_request_id(*ctx->rng);

    // ---- Read headers (up to the blank line).
    std::string header_block;
    header_block.reserve(1024);
    uint32_t start_ms = (uint32_t)millis();
    while (header_block.find("\r\n\r\n") == std::string::npos) {
        if (header_block.size() >= kHeaderCap || (uint32_t)millis() - start_ms > header_timeout_ms ||
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
                } else if (name == "cookie") {
                    req.cookie = value;
                } else if (name == "idempotency-key") {
                    req.idempotency_key = value;
                } else if (name == "upgrade") {
                    req.upgrade = value;
                } else if (name == "sec-websocket-key") {
                    req.ws_key = value;
                } else if (name == "x-session-id") {
                    req.session_hdr = value;
                } else if (name == "connection") {
                    req.keepalive = (toLower(value) == "keep-alive");
                }
            }
            pos = eol + 2;
        }
    }
    last_keepalive_ = req.keepalive;
    if (content_length > kBodyCap) {
        // Body too large for the fixed parsing buffer: refuse cleanly.
        std::string body = "{\"error\":{\"code\":\"bad_request\",\"message\":\"Request body too "
                           "large\",\"request_id\":\"" +
                           request_id + "\"}}";
        std::string hdrs = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, body.data(), body.size());
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

    auto connToken = [&]() -> const char* { return req.keepalive ? "keep-alive" : "close"; };
    auto sendRaw = [&](int status, const std::string& body) {
        std::string hdrs = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: " + connToken() + "\r\nX-Request-Id: " + request_id +
                           "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, body.data(), body.size());
        client.flush();
    };
    auto sendError = [&](mcco::ErrCode code, const char* message = nullptr) {
        JsonDocument doc;
        JsonObject err = doc["error"].to<JsonObject>();
        err["code"] = mcco::error_code_string(code);
        err["message"] = message ? message : mcco::default_error_message(code);
        err["request_id"] = request_id;
        const size_t len = measureJson(doc);
        std::string hdrs = "HTTP/1.1 " + std::to_string(mcco::error_http_status(code)) + " " +
                           reasonPhrase(mcco::error_http_status(code)) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(len) + "\r\nConnection: " + connToken() +
                           "\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        // Exact-size transient buffer: WiFiClient single-byte Print writes
        // silently drop bytes on a full socket buffer, and a RETAINED
        // response buffer once suffocated the heap — so neither streaming
        // Print nor a static buffer; serialize, then write fully.
        std::string body;
        body.reserve(len + 1);
        serializeJson(doc, body);
        writeFully(client, body.data(), body.size());
        client.flush();
        ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "http_error", nullptr,
                        request_id.c_str(), nullptr, nullptr);
    };
    auto sendJson = [&](int status, const JsonDocument& doc) {
        const size_t len = measureJson(doc);
        std::string hdrs = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(len) + "\r\nConnection: " + connToken() +
                           "\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        std::string body;
        body.reserve(len + 1);
        serializeJson(doc, body);
        writeFully(client, body.data(), body.size());
        client.flush();
    };
    auto sendHtml = [&](int status, const char* html) {
        std::string hdrs = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) +
                           "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                           std::to_string(strlen(html)) +
                           "\r\nConnection: close\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, html, strlen(html));
        client.flush();
    };

    const std::string ip = client.remoteIP().toString().c_str();

    // ---- Web UI document + session endpoints (spec ch. 14). GET / is a
    // document, never 401: the login form lives inside the page.
    if (req.method == "GET" && (req.path == "/" || req.path == "/ui")) {
        if (ctx->web_ui) sendHtml(200, ctx->web_ui->pageHtml());
        else sendError(mcco::ErrCode::InternalError);
        return;
    }
    if (req.method == "GET" && req.path == "/ui/session") {
        const std::string tok = cookieValue(req.cookie, "mc_session");
        const bool authed = ctx->web_ui && ctx->web_ui->validateSession(tok);
        JsonDocument doc;
        doc["authenticated"] = authed;
        doc["password_set"] = ctx->web_ui ? ctx->web_ui->passwordSet() : false;
        sendJson(200, doc);
        return;
    }
    if (req.method == "POST" && req.path == "/ui/login") {
        JsonDocument doc;
        if (deserializeJson(doc, req.body) || !doc.is<JsonObject>() ||
            !doc["password"].is<const char*>()) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        std::string token;
        const WebUi::LoginResult lr =
            ctx->web_ui->login(doc["password"].as<const char*>(), ip, token);
        if (lr != WebUi::LoginResult::Ok) {
            sendError(mcco::ErrCode::Unauthorized,
                      lr == WebUi::LoginResult::LockedOut
                          ? "Locked out after failed logins; try again in 60 seconds"
                          : nullptr);
            return;
        }
        std::string hdrs =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
            "Set-Cookie: mc_session=" +
            token +
            "; Path=/; HttpOnly; Max-Age=28800\r\nConnection: close\r\nX-Request-Id: " +
            request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, "{\"ok\":true}", 11);
        client.flush();
        return;
    }
    if (req.method == "POST" && req.path == "/ui/logout") {
        const std::string tok = cookieValue(req.cookie, "mc_session");
        if (ctx->web_ui) ctx->web_ui->logout(tok);
        const char* body = "{\"ok\":true}";
        std::string hdrs =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
            "Set-Cookie: mc_session=; Path=/; HttpOnly; Max-Age=0\r\nConnection: close\r\n"
            "X-Request-Id: " +
            request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, body, 11);
        client.flush();
        return;
    }

    if (req.method == "POST" && req.path == "/ui/password") {
        // Admin password change; requires a valid session (spec 13.1.1).
        const std::string tok = cookieValue(req.cookie, "mc_session");
        if (!ctx->web_ui || !ctx->web_ui->validateSession(tok)) {
            sendError(mcco::ErrCode::Unauthorized);
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, req.body) || !doc.is<JsonObject>() ||
            !doc["password"].is<const char*>()) {
            sendError(mcco::ErrCode::BadRequest, "expected {password}");
            return;
        }
        std::string err;
        if (!ctx->web_ui->setPassword(doc["password"].as<const char*>(), err)) {
            sendError(mcco::ErrCode::BadRequest, err.c_str());
            return;
        }
        // The password itself is never printed or logged (spec 13.1.1).
        ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "admin_password_set",
                        nullptr, request_id.c_str(), "webui", nullptr);
        JsonDocument resp;
        resp["ok"] = true;
        sendJson(200, resp);
        return;
    }

    // ---- Agent surface (spec 4.2/4.3): the pairing token is the credential,
    // not API keys, so this is handled BEFORE the auth-key path.
    if (req.path.compare(0, 10, "/agent/v1/") == 0) {
        if (!ctx->wifi->connected()) {
            sendError(mcco::ErrCode::NetworkUnavailable);
            return;
        }

        // WebSocket channel (spec 4.2.1).
        if (req.path == "/agent/v1/ws") {
            if (req.method != "GET" || toLower(trim(req.upgrade)) != "websocket") {
                sendError(mcco::ErrCode::BadRequest, "WebSocket upgrade required");
                return;
            }
            if (req.ws_key.empty()) {
                sendError(mcco::ErrCode::BadRequest, "missing Sec-WebSocket-Key");
                return;
            }
            // Write the 101 BEFORE publishing the client to the agent task:
            // otherwise the task could read these very bytes as a WS frame.
            // A queue slot is reserved first so the handoff cannot fail after
            // the upgrade; if it somehow does, closing is correct (the MCA
            // treats it as a failed attempt and backs off).
            if (!ctx->agent_link->canAcceptWs()) {
                sendError(mcco::ErrCode::InternalError, "agent channel busy");
                return;
            }
            std::string hdrs =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                ws::handshake_accept(req.ws_key) + "\r\n\r\n";
            writeFully(client, hdrs.data(), hdrs.size());
            client.flush();
            if (!ctx->agent_link->offerWsClient(client, bearerToken(req.auth))) {
                client.stop(); // upgraded then rejected: the MCA backs off
                return;
            }
            ws_handed_off_ = true; // caller must not stop the client
            return;
        }

        // Pairing ceremony (spec 3.2.2).
        if (req.path == "/agent/v1/pair") {
            if (req.method != "POST") {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, req.body) || !doc.is<JsonObject>()) {
                sendError(mcco::ErrCode::ValidationFailed);
                return;
            }
            bool bad_field = false;
            for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
                const char* k = kv.key().c_str();
                if (strcmp(k, "pairing_code") != 0 && strcmp(k, "agent_instance_id") != 0 &&
                    strcmp(k, "agent_version") != 0)
                    bad_field = true;
            }
            if (bad_field || !doc["pairing_code"].is<const char*>() ||
                !doc["agent_instance_id"].is<const char*>()) {
                sendError(mcco::ErrCode::ValidationFailed);
                return;
            }
            const char* code = doc["pairing_code"].as<const char*>();
            const char* instance = doc["agent_instance_id"].as<const char*>();

            std::string token;
            bool window_open;
            {
                Guard g(ctx->engine_mutex);
                window_open = ctx->pairing->windowOpen();
            }
            if (!window_open) {
                sendError(mcco::ErrCode::AgentNotPaired);
                return;
            }
            mcco::PairCodeResult vres = mcco::PairCodeResult::NoWindow;
            {
                Guard g(ctx->engine_mutex);
                vres = ctx->pairing->validateCode(code);
                if (vres == mcco::PairCodeResult::Ok) {
                    token = ctx->pairing->completePairing(instance);
                }
            }
            if (vres == mcco::PairCodeResult::RateLimited) {
                sendError(mcco::ErrCode::RateLimited);
                return;
            }
            if (vres == mcco::PairCodeResult::WrongCode) {
                if (!ctx->pairing->windowOpen()) {
                    sendError(mcco::ErrCode::AgentNotPaired); // 5th failure closed it
                } else {
                    sendError(mcco::ErrCode::Forbidden, "invalid pairing code");
                }
                return;
            }
            if (vres != mcco::PairCodeResult::Ok || token.empty()) {
                sendError(mcco::ErrCode::AgentNotPaired);
                return;
            }
            // Persist SYNCHRONOUSLY: the pairing record is security state and
            // must survive the reboot that follows a serial-port close (the
            // DTR/RTS landmine can strike within milliseconds of the
            // ceremony, long before the loop() dirty-drain would flush).
            {
                Guard g(ctx->engine_mutex);
                ctx->config->persistPairing(*ctx->pairing);
            }
            ctx->pairing_dirty = true; // loop(): mode flip + mDNS TXT
            mcco::PairingRecord rec;
            {
                Guard g(ctx->engine_mutex);
                if (const mcco::PairingRecord* r = ctx->pairing->activeRecord()) rec = *r;
            }
            JsonDocument resp;
            resp["pairing_id"] = rec.pairing_id;
            resp["device_id"] = rec.device_id;
            resp["agent_instance_id"] = rec.agent_instance_id;
            resp["agent_token"] = token.c_str();
            resp["created_at"] = (uint64_t)rec.created_at;
            resp["state"] = "active";
            resp["heartbeat_interval_s"] = 5;
            sendJson(200, resp);
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "pairing_completed",
                            nullptr, request_id.c_str(), nullptr,
                            (std::string("{\"pairing_id\":\"") + rec.pairing_id + "\"}")
                                .c_str());
            return;
        }

        // Polling ingress (spec 4.3.1).
        if (req.path == "/agent/v1/events") {
            if (req.method != "POST") {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            std::string resp_json;
            mcco::ErrCode err = mcco::ErrCode::InternalError;
            if (!ctx->agent_link->pollEvent(bearerToken(req.auth), req.session_hdr, req.body,
                                            resp_json, err)) {
                sendError(err);
                return;
            }
            sendRaw(200, resp_json);
            return;
        }
        if (req.path == "/agent/v1/commands/pending") {
            if (req.method != "GET") {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            std::string resp_json;
            mcco::ErrCode err = mcco::ErrCode::InternalError;
            if (!ctx->agent_link->pollPending(bearerToken(req.auth), req.session_hdr, resp_json,
                                              err)) {
                sendError(err);
                return;
            }
            sendRaw(200, resp_json);
            return;
        }

        sendError(mcco::ErrCode::NotFound); // closed surface (spec 13.3)
        return;
    }

    // ---- Unversioned or unknown top-level paths: 404 before authentication
    // (spec 4.1.1 — the surface is versioned; / and /ui are the documents).
    if (req.path.compare(0, 8, "/api/v1/") != 0) {
        sendError(mcco::ErrCode::NotFound);
        return;
    }

    // ---- Association lost while serving (spec 15.1).
    if (!ctx->wifi->connected()) {
        sendError(mcco::ErrCode::NetworkUnavailable);
        return;
    }

    // ---- Authentication + lockout + rate limit.
    // A valid mc_session cookie is treated as the ADMIN role for /api/v1/*
    // calls made by the Web UI (spec ch. 14: the Web UI is bound to ADMIN).
    // Session-authenticated calls bypass the per-key rate limiter and key
    // last_used_at attribution (both are keyed to API keys, spec 13.1.1).
    mcco::Principal principal;
    bool session_auth = false;
    {
        const std::string tok = cookieValue(req.cookie, "mc_session");
        if (!tok.empty() && ctx->web_ui && ctx->web_ui->validateSession(tok)) {
            principal.key_id = "webui";
            principal.role = mcco::Role::Admin;
            session_auth = true;
        }
    }
    if (!session_auth) {
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
    if (!session_auth) {
        std::string raw_key;
        const std::string bearer = "Bearer ";
        if (req.auth.compare(0, bearer.size(), bearer) == 0)
            raw_key = trim(req.auth.substr(bearer.size()));

        mcco::AuthResult auth =
            ctx->keys->authenticate(raw_key, ctx->clock->epoch_seconds(), principal);
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
    }
    {
        Guard g(g_auth_track_mutex);
        AuthTrack& t = trackFor(ip.c_str());
        t.consecutive_fails = 0;
    }
    if (!session_auth) {
        if (!ctx->limiter->allow(principal.key_id, principal.role)) {
            sendError(mcco::ErrCode::RateLimited);
            return;
        }

        // Mark key use (attribution, spec 13.1.1). Persisted lazily by main loop.
        ctx->keys->touch(principal.key_id, ctx->clock->epoch_seconds());
        ctx->keys_dirty = true;
    }

    const std::string actor = session_auth ? "webui" : "apikey:" + principal.key_id;
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
    if (req.method == "GET" && req.path == "/api/v1/openapi.json") {
        if (!roleCheck(mcco::Role::Read)) return;
        // Static contract document (spec 17.1.1); the string lives in
        // maccontrol_core so native tests can validate it against the routes.
        sendRaw(200, mcco::kOpenApiJson);
        return;
    }
    if (req.method == "GET" && req.path == "/api/v1/logs") {
        if (!roleCheck(mcco::Role::Read)) return;
        // Ring-buffer log (spec 15.2). Filters are closed enums; a value
        // outside the enum is a client error, not an empty result.
        const mcco::LogCategory* catp = nullptr;
        const mcco::LogLevel* lvlp = nullptr;
        mcco::LogCategory cat;
        mcco::LogLevel lvl;
        std::string v;
        if (queryParam(req.query, "category", v)) {
            if (!mcco::log_category_from_string(v.c_str(), cat)) {
                sendError(mcco::ErrCode::BadRequest, "invalid category");
                return;
            }
            catp = &cat;
        }
        if (queryParam(req.query, "level", v)) {
            if (!mcco::log_level_from_string(v.c_str(), lvl)) {
                sendError(mcco::ErrCode::BadRequest, "invalid level");
                return;
            }
            lvlp = &lvl;
        }
        uint32_t since = 0;
        if (queryParam(req.query, "since_seq", v)) since = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        size_t limit = 100;
        if (queryParam(req.query, "limit", v)) {
            limit = (size_t)strtoul(v.c_str(), nullptr, 10);
            if (limit == 0) limit = 100;
            if (limit > 512) limit = 512; // spec 15.2 maximum
        }
        uint32_t dropped = 0;
        std::vector<std::string> entries =
            ctx->log->entries_since(since, limit, catp, lvlp, &dropped);
        // Stream to the socket: a 512-entry page is ~130 KB, far too large
        // to buffer on a constrained heap.
        size_t total = std::string("{\"entries\":[],\"dropped\":}").size() +
                       std::to_string(dropped).size();
        for (size_t i = 0; i < entries.size(); i++) total += entries[i].size() + (i ? 1 : 0);
        std::string hdrs = "HTTP/1.1 200 " + std::string(reasonPhrase(200)) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(total) + "\r\nConnection: " + connToken() +
                           "\r\nX-Request-Id: " + request_id + "\r\n\r\n";
        writeFully(client, hdrs.data(), hdrs.size());
        writeFully(client, "{\"entries\":[", 12);
        for (size_t i = 0; i < entries.size(); i++) {
            if (i) writeFully(client, ",", 1);
            writeFully(client, entries[i].data(), entries[i].size());
        }
        writeFully(client, "],\"dropped\":", 12);
        std::string d = std::to_string(dropped);
        writeFully(client, d.data(), d.size());
        writeFully(client, "}", 1);
        client.flush();
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

    // ---- Macros (spec 10.1, 10.3.1, 12.1.1) ----
    if (req.path == "/api/v1/macros" || req.path.compare(0, 15, "/api/v1/macros/") == 0) {
        const std::string rest = (req.path.size() > 15) ? req.path.substr(15) : "";
        // rest is "{id}" or "{id}/execute"; ids never contain '/'.
        const size_t slash = rest.find('/');
        const std::string macro_id = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        const bool is_execute = (slash != std::string::npos && rest.substr(slash) == "/execute");

        if (rest.empty()) {
            if (req.method == "GET") {
                if (!roleCheck(mcco::Role::Read)) return;
                std::string body = "{\"macros\":[";
                bool first = true;
                {
                    Guard g(ctx->engine_mutex);
                    for (const mcco::Macro* m : ctx->macros->list()) {
                        if (!first) body += ",";
                        first = false;
                        body += mcco::macro_to_json(*m);
                    }
                }
                body += "]}";
                sendRaw(200, body);
                return;
            }
            if (req.method == "POST") {
                if (!roleCheck(mcco::Role::Admin)) return;
                if (ctx->store_corrupt.load()) {
                    sendError(mcco::ErrCode::StoreCorrupt); // spec 15.1
                    return;
                }
                mcco::Macro def, created;
                mcco::MacroError merr = mcco::MacroError::Ok;
                if (!mcco::macro_from_json(req.body, def, merr)) {
                    sendError(mcco::macro_error_code(merr), mcco::macro_error_detail(merr));
                    return;
                }
                {
                    Guard g(ctx->engine_mutex);
                    if (!ctx->macros->add(def, created, merr, *ctx->rng)) {
                        sendError(mcco::macro_error_code(merr), mcco::macro_error_detail(merr));
                        return;
                    }
                }
                ctx->macros_dirty = true;
                ctx->status_cache->onMacrosChanged();
                ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "macro_created",
                                nullptr, request_id.c_str(), actor.c_str(),
                                (std::string("{\"macro_id\":\"") + created.macro_id + "\"}")
                                    .c_str());
                sendRaw(201, mcco::macro_to_json(created));
                return;
            }
            sendError(mcco::ErrCode::NotFound);
            return;
        }

        if (is_execute) {
            if (req.method != "POST") {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            if (!roleCheck(mcco::Role::Control)) return; // http_button, spec 10.2.1
            MacroExecOutcome out = submit_macro_execute(ctx, macro_id, actor);
            if (!out.ok) {
                sendError(out.error);
                return;
            }
            JsonDocument resp;
            resp["command_id"] = out.submit.record.command_id;
            resp["state"] = mcco::command_state_to_string(out.submit.record.state);
            resp["deadline_at"] = mcco::iso8601_format(out.submit.record.deadline_at);
            resp["record_url"] = "/api/v1/commands/" + out.submit.record.command_id;
            sendJson(202, resp);
            return;
        }

        if (req.method == "PUT") {
            if (!roleCheck(mcco::Role::Admin)) return;
            if (ctx->store_corrupt.load()) {
                sendError(mcco::ErrCode::StoreCorrupt);
                return;
            }
            mcco::Macro def;
            mcco::MacroError merr = mcco::MacroError::Ok;
            if (!mcco::macro_from_json(req.body, def, merr)) {
                sendError(mcco::macro_error_code(merr), mcco::macro_error_detail(merr));
                return;
            }
            {
                Guard g(ctx->engine_mutex);
                if (!ctx->macros->update(macro_id, def, merr)) {
                    sendError(mcco::macro_error_code(merr), mcco::macro_error_detail(merr));
                    return;
                }
            }
            ctx->macros_dirty = true;
            ctx->status_cache->onMacrosChanged();
            std::string body;
            {
                Guard g(ctx->engine_mutex);
                const mcco::Macro* m = ctx->macros->get(macro_id);
                body = m ? mcco::macro_to_json(*m) : "{}";
            }
            ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "macro_updated",
                            nullptr, request_id.c_str(), actor.c_str(),
                            (std::string("{\"macro_id\":\"") + macro_id + "\"}").c_str());
            sendRaw(200, body);
            return;
        }
        if (req.method == "DELETE") {
            if (!roleCheck(mcco::Role::Admin)) return;
            if (ctx->store_corrupt.load()) {
                sendError(mcco::ErrCode::StoreCorrupt);
                return;
            }
            bool removed;
            {
                Guard g(ctx->engine_mutex);
                removed = ctx->macros->remove(macro_id);
            }
            if (!removed) {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            // Spec 10.2.1: bindings to the deleted macro are auto-disabled
            // (never executed against a stale revision) and logged.
            for (const std::string& tid : ctx->triggers->onMacroDeleted(macro_id)) {
                ctx->triggers_dirty = true;
                ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Warn,
                                "trigger_auto_disabled", nullptr, request_id.c_str(),
                                actor.c_str(),
                                (std::string("{\"trigger_id\":\"") + tid +
                                 "\",\"macro_id\":\"" + macro_id + "\"}")
                                    .c_str());
            }
            ctx->macros_dirty = true;
            ctx->status_cache->onMacrosChanged();
            ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "macro_deleted",
                            nullptr, request_id.c_str(), actor.c_str(),
                            (std::string("{\"macro_id\":\"") + macro_id + "\"}").c_str());
            sendRaw(204, "");
            return;
        }
        sendError(mcco::ErrCode::NotFound);
        return;
    }

    // ---- Triggers (spec 10.2.1). NOT in the spec ch.12 inventory; added as
    // an ADMIN-gated surface because the Web UI owns trigger bindings
    // (spec ch.14 matrix). Phase 3 reconciles with /openapi.json.
    if (req.path == "/api/v1/triggers" || req.path.compare(0, 17, "/api/v1/triggers/") == 0) {
        const std::string rest = (req.path.size() > 17) ? req.path.substr(17) : "";
        if (rest.empty()) {
            if (req.method == "GET") {
                if (!roleCheck(mcco::Role::Read)) return;
                std::string body = "{\"triggers\":[";
                bool first = true;
                for (const Trigger* t : ctx->triggers->list()) {
                    if (!first) body += ",";
                    first = false;
                    body += trigger_to_json_pub(*t);
                }
                body += "]}";
                sendRaw(200, body);
                return;
            }
            if (req.method == "POST") {
                if (!roleCheck(mcco::Role::Admin)) return;
                Trigger created;
                mcco::ErrCode terr = mcco::ErrCode::InternalError;
                if (!ctx->triggers->add(req.body, created, terr)) {
                    sendError(terr);
                    return;
                }
                ctx->triggers_dirty = true;
                ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "trigger_created",
                                nullptr, request_id.c_str(), actor.c_str(),
                                (std::string("{\"trigger_id\":\"") + created.trigger_id + "\"}")
                                    .c_str());
                sendRaw(201, trigger_to_json_pub(created));
                return;
            }
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        if (rest.find('/') != std::string::npos) {
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        if (req.method == "PUT") {
            if (!roleCheck(mcco::Role::Admin)) return;
            Trigger updated;
            mcco::ErrCode terr = mcco::ErrCode::InternalError;
            if (!ctx->triggers->update(rest, req.body, updated, terr)) {
                sendError(terr);
                return;
            }
            ctx->triggers_dirty = true;
            ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "trigger_updated",
                            nullptr, request_id.c_str(), actor.c_str(),
                            (std::string("{\"trigger_id\":\"") + rest + "\"}").c_str());
            sendRaw(200, trigger_to_json_pub(updated));
            return;
        }
        if (req.method == "DELETE") {
            if (!roleCheck(mcco::Role::Admin)) return;
            if (!ctx->triggers->remove(rest)) {
                sendError(mcco::ErrCode::NotFound);
                return;
            }
            ctx->triggers_dirty = true;
            ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "trigger_deleted",
                            nullptr, request_id.c_str(), actor.c_str(),
                            (std::string("{\"trigger_id\":\"") + rest + "\"}").c_str());
            sendRaw(204, "");
            return;
        }
        sendError(mcco::ErrCode::NotFound);
        return;
    }

    // ---- Device identity (spec ch.14 matrix: the ESP32 Web UI owns identity).
    // No identity REST endpoint exists in the spec ch.12 inventory; this
    // ADMIN-gated surface backs the Device page (Phase 3 reconciles).
    if (req.method == "GET" && req.path == "/api/v1/device/identity") {
        if (!roleCheck(mcco::Role::Read)) return;
        const mcco::Identity id = ctx->config->identity();
        JsonDocument resp;
        resp["device_name"] = id.device_name;
        resp["hostname"] = id.hostname;
        resp["location"] = id.location;
        resp["description"] = id.description;
        resp["device_id"] = id.device_id;
        sendJson(200, resp);
        return;
    }
    if (req.method == "POST" && req.path == "/api/v1/device/identity") {
        if (!roleCheck(mcco::Role::Admin)) return;
        JsonDocument doc;
        if (deserializeJson(doc, req.body) || !doc.is<JsonObject>()) {
            sendError(mcco::ErrCode::BadRequest);
            return;
        }
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
            const char* k = kv.key().c_str();
            if (strcmp(k, "device_name") != 0 && strcmp(k, "hostname") != 0 &&
                strcmp(k, "location") != 0 && strcmp(k, "description") != 0) {
                sendError(mcco::ErrCode::BadRequest);
                return;
            }
        }
        mcco::Identity id = ctx->config->identity();
        const std::string old_hostname = id.hostname;
        if (doc["device_name"].is<const char*>()) id.device_name = doc["device_name"].as<const char*>();
        if (doc["hostname"].is<const char*>()) {
            const std::string h = doc["hostname"].as<const char*>();
            if (!mcco::hostname_valid(h)) {
                sendError(mcco::ErrCode::BadRequest, "invalid hostname (1-57 chars [a-z0-9-])");
                return;
            }
            id.hostname = h;
        }
        if (doc["location"].is<const char*>()) id.location = doc["location"].as<const char*>();
        if (doc["description"].is<const char*>())
            id.description = doc["description"].as<const char*>();
        if (!ctx->config->saveIdentity(id)) {
            sendError(mcco::ErrCode::InternalError);
            return;
        }
        ctx->status_cache->onIdentityChanged();
        if (id.hostname != old_hostname) ctx->mdns->reannounce(id); // spec 3.1.1
        ctx->log->write(mcco::LogCategory::Config, mcco::LogLevel::Info, "identity_updated",
                        nullptr, request_id.c_str(), actor.c_str(), nullptr);
        JsonDocument resp;
        resp["device_name"] = id.device_name;
        resp["hostname"] = id.hostname;
        resp["location"] = id.location;
        resp["description"] = id.description;
        resp["device_id"] = id.device_id;
        sendJson(200, resp);
        return;
    }

    // ---- API key management (AMENDMENT: Web UI session only, spec 13.1.1 —
    // keys are created/listed/revoked through the Web UI by an ADMIN session,
    // never with an API key).
    if (req.path == "/api/v1/keys" || req.path.compare(0, 13, "/api/v1/keys/") == 0) {
        if (!session_auth) {
            sendError(mcco::ErrCode::Forbidden, "key management requires a Web UI session");
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/keys") {
            JsonDocument doc;
            JsonArray arr = doc["keys"].to<JsonArray>();
            Guard g(ctx->engine_mutex);
            for (const mcco::KeyRecord& r : ctx->keys->records()) {
                JsonObject o = arr.add<JsonObject>();
                o["key_id"] = r.key_id;
                o["label"] = r.label;
                o["role"] = mcco::role_to_string(r.role);
                o["key_sha256"] = r.key_sha256;
                o["created_at"] = r.created_at ? mcco::iso8601_format(r.created_at).c_str() : "";
                o["last_used_at"] = r.last_used_at ? mcco::iso8601_format(r.last_used_at).c_str() : "";
                o["expires_at"] = r.expires_at ? mcco::iso8601_format(r.expires_at).c_str() : "";
                o["state"] = r.active ? "active" : "revoked";
            }
            sendJson(200, doc);
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/keys") {
            JsonDocument body;
            if (deserializeJson(body, req.body) || !body.is<JsonObject>() ||
                !body["role"].is<const char*>() || !body["label"].is<const char*>()) {
                sendError(mcco::ErrCode::BadRequest, "expected {role, label}");
                return;
            }
            mcco::Role role;
            if (!mcco::role_from_string(body["role"].as<const char*>(), role)) {
                sendError(mcco::ErrCode::BadRequest, "role must be READ, CONTROL or ADMIN");
                return;
            }
            const char* label = body["label"].as<const char*>();
            std::string raw = mcco::make_api_key(*ctx->rng);
            std::string key_id;
            bool added;
            {
                Guard g(ctx->engine_mutex);
                added = ctx->keys->add(raw, role, label, ctx->clock->epoch_seconds(), key_id);
            }
            if (!added) {
                sendError(mcco::ErrCode::Conflict, "key store full (max 8 active keys)");
                return;
            }
            if (!ctx->config->persistKeys(*ctx->keys)) {
                sendError(mcco::ErrCode::InternalError, "persist failed; key will be lost on reboot");
                return;
            }
            // The raw key is shown exactly once here and never written to the log.
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "key_created",
                            nullptr, request_id.c_str(), nullptr,
                            (std::string("{\"key_id\":\"") + key_id + "\"}").c_str());
            JsonDocument resp;
            resp["key_id"] = key_id;
            resp["label"] = label;
            resp["role"] = mcco::role_to_string(role);
            resp["key"] = raw.c_str();
            resp["state"] = "active";
            sendJson(201, resp);
            return;
        }
        if (req.method == "DELETE" && req.path.size() > 13) {
            const std::string key_id = req.path.substr(13);
            bool revoked;
            {
                Guard g(ctx->engine_mutex);
                revoked = ctx->keys->revoke(key_id);
            }
            if (!revoked) {
                sendError(mcco::ErrCode::NotFound, "no such active key");
                return;
            }
            ctx->config->persistKeys(*ctx->keys);
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "key_revoked",
                            nullptr, request_id.c_str(), nullptr,
                            (std::string("{\"key_id\":\"") + key_id + "\"}").c_str());
            JsonDocument resp;
            resp["key_id"] = key_id;
            resp["state"] = "revoked";
            sendJson(200, resp);
            return;
        }
        sendError(mcco::ErrCode::NotFound);
        return;
    }

    // ---- Pairing administration (spec 3.2). Session-authenticated ADMIN,
    // same pattern as /api/v1/keys.
    if (req.path == "/api/v1/pairing" || req.path.compare(0, 17, "/api/v1/pairing/") == 0) {
        if (!session_auth) {
            sendError(mcco::ErrCode::Forbidden, "pairing administration requires a Web UI session");
            return;
        }
        const std::string rest = (req.path.size() > 17) ? req.path.substr(17) : "";
        if (rest.empty() && req.method == "GET") {
            JsonDocument resp;
            {
                Guard g(ctx->engine_mutex);
                resp["state"] = mcco::pairing_state_to_string(ctx->pairing->state());
                if (const mcco::PairingRecord* rec = ctx->pairing->activeRecord()) {
                    JsonObject r = resp["record"].to<JsonObject>();
                    r["pairing_id"] = rec->pairing_id;
                    r["device_id"] = rec->device_id;
                    r["agent_instance_id"] = rec->agent_instance_id;
                    r["created_at"] = (uint64_t)rec->created_at;
                    r["last_used_at"] = (uint64_t)rec->last_used_at;
                    r["state"] = "active";
                } else {
                    resp["record"] = nullptr;
                }
                JsonObject w = resp["window"].to<JsonObject>();
                const bool open = ctx->pairing->windowOpen();
                w["open"] = open;
                w["seconds_remaining"] = open ? ctx->pairing->windowSecondsRemaining() : 0;
            }
            sendJson(200, resp);
            return;
        }
        if (rest == "window" && req.method == "POST") {
            uint32_t duration_s = mcco::PairingStore::kDefaultWindowS;
            if (!req.body.empty()) {
                JsonDocument doc;
                if (deserializeJson(doc, req.body) || !doc.is<JsonObject>()) {
                    sendError(mcco::ErrCode::BadRequest);
                    return;
                }
                for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
                    if (strcmp(kv.key().c_str(), "duration_s") != 0) {
                        sendError(mcco::ErrCode::BadRequest);
                        return;
                    }
                }
                if (doc["duration_s"].is<int>()) {
                    duration_s = (uint32_t)doc["duration_s"].as<int>();
                } else if (!doc["duration_s"].isNull()) {
                    sendError(mcco::ErrCode::BadRequest);
                    return;
                }
            }
            if (duration_s < mcco::PairingStore::kMinWindowS ||
                duration_s > mcco::PairingStore::kMaxWindowS) {
                sendError(mcco::ErrCode::BadRequest,
                          "duration_s must be 60-600 (spec 3.2.1)");
                return;
            }
            JsonDocument resp;
            {
                Guard g(ctx->engine_mutex);
                if (!ctx->pairing->openWindow(duration_s)) {
                    sendError(mcco::ErrCode::InternalError);
                    return;
                }
                resp["pairing_code"] = ctx->pairing->pairingCode();
                resp["seconds_remaining"] = ctx->pairing->windowSecondsRemaining();
            }
            sendJson(200, resp);
            return;
        }
        if (rest == "window" && req.method == "DELETE") {
            {
                Guard g(ctx->engine_mutex);
                ctx->pairing->closeWindow();
            }
            JsonDocument resp;
            resp["ok"] = true;
            sendJson(200, resp);
            return;
        }
        if (rest == "revoke" && req.method == "POST") {
            {
                Guard g(ctx->engine_mutex);
                if (!ctx->pairing->revoke()) {
                    sendError(mcco::ErrCode::AgentNotPaired);
                    return;
                }
            }
            // Immediate: terminate the live agent session (spec 3.2.2).
            ctx->agent_link->requestClose((uint16_t)mcco::AgentClose::Unpaired);
            ctx->pairing_dirty = true; // loop(): persist + mode flip + mDNS TXT
            const std::string corr = mcco::make_request_id(*ctx->rng);
            ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "pairing_revoked",
                            nullptr, request_id.c_str(), actor.c_str(),
                            (std::string("{\"correlation_id\":\"") + corr + "\"}").c_str());
            JsonDocument resp;
            resp["ok"] = true;
            sendJson(200, resp);
            return;
        }
        sendError(mcco::ErrCode::NotFound);
        return;
    }

    // ---- Agent surface (Mode A: honest 409s, no ledger records, spec 12.1.1).
    if (req.method == "GET" && req.path == "/api/v1/agent/status") {
        if (!roleCheck(mcco::Role::Read)) return;
        bool paired;
        mcco::AgentStatus st;
        mcco::PairingRecord rec_copy;
        {
            Guard g(ctx->engine_mutex);
            paired = ctx->pairing->paired();
            if (paired) {
                st = ctx->status_cache->snapshotAgent();
                if (const mcco::PairingRecord* rec = ctx->pairing->activeRecord()) rec_copy = *rec;
            }
        }
        if (!paired) {
            sendError(mcco::ErrCode::AgentNotPaired);
            return;
        }
        // Composite MCA status report (spec 6.3) from the cached evidence
        // snapshot; absent fields are null, never invented.
        JsonDocument resp;
        resp["agent_instance_id"] = rec_copy.agent_instance_id;
        if (st.has_boot) {
            resp["boot_id"] = st.boot_id;
        } else {
            resp["boot_id"] = nullptr;
        }
        resp["reported_at"] = mcco::iso8601_format(ctx->clock->epoch_seconds()).c_str();
        {
            std::string sid = ctx->agent_link->sessionId();
            if (sid.empty()) resp["session_id"] = nullptr;
            else resp["session_id"] = sid;
        }
        resp["session_active"] = ctx->agent_link->sessionActive();
        if (st.has_system) {
            resp["system"]["state"] = st.system_state;
        } else {
            resp["system"]["state"] = nullptr;
        }
        resp["user"]["logged_in"] = st.has_user ? st.user_logged_in : false;
        if (st.has_user && !st.user.empty()) {
            resp["user"]["name"] = st.user;
        } else {
            resp["user"]["name"] = nullptr;
        }
        resp["user"]["screen_locked"] = st.has_lock ? st.locked : false;
        JsonObject apps = resp["applications"].to<JsonObject>();
        for (const auto& kv : st.apps) {
            JsonObject a = apps[kv.first.c_str()].to<JsonObject>();
            a["running"] = kv.second.running;
            a["pid"] = (int64_t)kv.second.pid;
        }
        sendJson(200, resp);
        return;
    }
    if (req.method == "POST" && req.path.compare(0, 13, "/api/v1/apps/") == 0) {
        if (!roleCheck(mcco::Role::Control)) return;
        // app_launch/app_quit route through the MCA (spec 11.2): expand into
        // a generic submission; the engine's agent gate produces the
        // deterministic pre-ledger 409s, the ledger record goes to
        // `confirming`, and the dispatcher hands the action to the agent.
        std::string rest = req.path.substr(13);
        mcco::CommandType type;
        if (rest.size() > 7 && rest.compare(rest.size() - 7, 7, "/launch") == 0) {
            type = mcco::CommandType::AppLaunch;
            rest.resize(rest.size() - 7);
        } else if (rest.size() > 5 && rest.compare(rest.size() - 5, 5, "/quit") == 0) {
            type = mcco::CommandType::AppQuit;
            rest.resize(rest.size() - 5);
        } else {
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        const std::string bundle_id = urlDecode(rest);
        if (bundle_id.empty()) {
            sendError(mcco::ErrCode::NotFound);
            return;
        }
        JsonDocument pdoc;
        pdoc["bundle_id"] = bundle_id;
        std::string params;
        serializeJson(pdoc, params);

        mcco::Submission sub;
        sub.type = type;
        sub.parameters_json = params;
        sub.requested_by = actor;
        sub.body_hash = mcco::Sha256::hex_digest(std::string("apps:") + bundle_id);

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
