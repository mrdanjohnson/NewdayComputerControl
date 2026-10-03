#include "ota.h"
#include <Arduino.h>
#include <cstdarg>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <ArduinoJson.h>
#include "log_sink.h"
#include "mc_engine.h"
#include "mc_log.h"
#include "mc_ota.h"
#include "ota_pubkey.h"

#ifndef MC_FW_VERSION
#define MC_FW_VERSION "dev"
#endif

namespace {

// Version marker embedded in EVERY firmware image (spec 15.3: the running
// version must be attributable; the upload response reports the target
// version by scanning the freshly written slot for this marker). Referenced
// at runtime by scanVersion() so the linker never garbage-collects it.
const mcco::OtaVersionMarker kOwnVersionMarker = {mcco::kOtaVersionMarkerMagic,
                                                  MC_FW_VERSION};

constexpr uint32_t kOtaIoTimeoutMs = 120000;  // generous bound for a 1.4 MB stream
constexpr uint32_t kWriteTimeoutMs = 4000;    // mirror of http_api.cpp kWriteTimeoutMs
constexpr uint32_t kSelfCheckWindowMs = 60000; // spec 15.3
constexpr size_t kStreamChunk = 4096;

// Single-threaded HTTP task state (no locking needed).
bool g_busy = false;
bool g_pending_validated = false;
esp_partition_t g_pending_partition{};
char g_pending_version[32] = "";
bool g_selfcheck_pending = false;
uint32_t g_selfcheck_deadline_ms = 0;
bool g_status_served = false;

void otaLog(AppContext* ctx, mcco::LogLevel lvl, const char* event, const char* request_id,
            const char* actor, const char* detail_json) {
    ctx->log->write(mcco::LogCategory::Ota, lvl, event, nullptr, request_id, actor,
                    detail_json);
}

// detail with the running version pinned first (spec 15.3: every ota entry
// carries the running version).
void detailVersion(char* buf, size_t cap, const char* extra_fmt, ...) {
    int n = snprintf(buf, cap, "{\"version\":\"%s\"", MC_FW_VERSION);
    if (extra_fmt && n > 0 && (size_t)n < cap) {
        va_list ap;
        va_start(ap, extra_fmt);
        vsnprintf(buf + n, cap - (size_t)n, extra_fmt, ap);
        va_end(ap);
    }
    const size_t len = strlen(buf);
    if (cap - len >= 2) strcat(buf, "}");
    else buf[cap - 2] = '}';
    buf[cap - 1] = '\0';
}

const char* statusPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
    }
    return "Internal Server Error";
}

bool writeAll(WiFiClient& client, const char* data, size_t len) {
    const int32_t deadline = (int32_t)(millis() + kWriteTimeoutMs);
    size_t off = 0;
    while (off < len) {
        const int w = client.write(reinterpret_cast<const uint8_t*>(data + off), len - off);
        if (w <= 0) {
            if (!client.connected()) return false;
            if ((int32_t)(millis() - deadline) > 0) return false;
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        off += (size_t)w;
    }
    return true;
}

void sendJsonClose(WiFiClient& client, int status, const char* body) {
    char hdr[192];
    const int hl = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                            "Content-Length: %u\r\nConnection: close\r\n\r\n",
                            status, statusPhrase(status), (unsigned)strlen(body));
    writeAll(client, hdr, (size_t)hl);
    writeAll(client, body, strlen(body));
    client.flush();
}

void sendErrorClose(AppContext* ctx, WiFiClient& client, mcco::ErrCode code,
                    const char* request_id) {
    char body[320];
    snprintf(body, sizeof(body),
             "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"request_id\":\"%s\"}}",
             mcco::error_code_string(code), mcco::default_error_message(code), request_id);
    sendJsonClose(client, mcco::error_http_status(code), body);
    ctx->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "http_error", nullptr,
                    request_id, nullptr, nullptr);
}

// read exactly n bytes, WDT-fed, absolute deadline
bool readFully(WiFiClient& client, uint8_t* buf, size_t n, uint32_t deadline_ms) {
    size_t off = 0;
    while (off < n) {
        if ((int32_t)(millis() - deadline_ms) > 0 || !client.connected()) return false;
        const int r = client.read(buf + off, n - off);
        if (r > 0) {
            off += (size_t)r;
        } else {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return true;
}

// Mixed byte source for the upload stream: bytes the request parser already
// buffered past the header terminator are consumed first (`cursor` advances),
// the remainder comes from the socket via readFully. Without this the
// streamed read desyncs whenever the client sent headers+body in one flight
// (bench finding: the device blocked until the client timed out, then logged
// truncated_signature).
bool readMixed(WiFiClient& client, const std::string& buffered, size_t& cursor,
               uint8_t* buf, size_t n, uint32_t deadline_ms) {
    if (cursor < buffered.size()) {
        const size_t take = n < buffered.size() - cursor ? n : buffered.size() - cursor;
        memcpy(buf, buffered.data() + cursor, take);
        cursor += take;
        buf += take;
        n -= take;
        if (n == 0) return true;
    }
    return readFully(client, buf, n, deadline_ms);
}

// ECDSA P-256 verify of the image digest against the compiled-in public key.
// mbedTLS only (device-side; the host check is scripts/ota_sign.py verify).
bool verifySignature(const uint8_t digest[32], const uint8_t sig[64]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &q, kOtaPublicKeyDev,
                                      sizeof(kOtaPublicKeyDev)) == 0 &&
        mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
        mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0) {
        ok = mbedtls_ecdsa_verify(&grp, digest, 32, &q, &r, &s) == 0;
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

// Scan the freshly written slot for this firmware's version marker. Best
// effort: "unknown" when absent (never fails the upload — the signature is
// the gate, the marker only labels the response/log).
bool scanVersion(const esp_partition_t* part, char* out, size_t cap) {
    static uint8_t block[kStreamChunk]; // BSS, same single user as the stream buffer
    for (size_t base = 0; base < part->size; base += sizeof(block) - 3) {
        esp_task_wdt_reset();
        const size_t n = (part->size - base) < sizeof(block) ? part->size - base
                                                             : sizeof(block);
        if (esp_partition_read(part, base, block, n) != ESP_OK) return false;
        for (size_t i = 0; i + sizeof(mcco::OtaVersionMarker) <= n; i++) {
            if (memcmp(block + i, &kOwnVersionMarker.magic,
                       sizeof(kOwnVersionMarker.magic)) != 0)
                continue;
            const char* v = reinterpret_cast<const char*>(block + i + sizeof(uint32_t));
            const size_t len = strnlen(v, 32);
            if (len > 0 && len < cap) {
                memcpy(out, v, len);
                out[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

} // namespace

namespace ota {

void begin(AppContext* ctx) {
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run) return;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        // Booted into a freshly applied image: the 60 s self-check window is
        // open (spec 15.3). Confirmation happens on the first successful
        // /api/v1/status serve (ota::noteStatusServed); the bootloader
        // reverts automatically if the window expires or the chip resets.
        g_selfcheck_pending = true;
        g_selfcheck_deadline_ms = millis() + kSelfCheckWindowMs;
        char detail[96];
        detailVersion(detail, sizeof(detail), ",\"state\":\"pending_verify\"");
        otaLog(ctx, mcco::LogLevel::Info, "self_check", nullptr, nullptr, detail);
        return;
    }
    // Not pending: if a sibling OTA slot was invalidated, this boot IS the
    // result of a rollback — log it at error (spec 15.3).
    for (esp_partition_subtype_t s = ESP_PARTITION_SUBTYPE_APP_OTA_0;
         s < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
         s = static_cast<esp_partition_subtype_t>(s + 1)) {
        const esp_partition_t* p =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, s, nullptr);
        if (!p || p == run) continue;
        esp_ota_img_states_t ps = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(p, &ps) == ESP_OK &&
            ps == ESP_OTA_IMG_INVALID) {
            char detail[160];
            detailVersion(detail, sizeof(detail), ",\"invalidated_slot\":\"%s\"",
                          p->label);
            otaLog(ctx, mcco::LogLevel::Error, "rollback", nullptr, nullptr, detail);
        }
    }
}

void tick(AppContext* ctx) {
    if (!g_selfcheck_pending) return;
    if ((int32_t)(millis() - g_selfcheck_deadline_ms) > 0) {
        // Window expired without a responsive status surface: invalidate this
        // slot and reboot into the previous image (the bootloader reverts).
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"self_check_timeout\"");
        otaLog(ctx, mcco::LogLevel::Error, "rollback", nullptr, nullptr, detail);
        g_selfcheck_pending = false;
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return; // unreachable on success
    }
    if (!g_status_served) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        g_selfcheck_pending = false;
        char detail[96];
        detailVersion(detail, sizeof(detail), ",\"state\":\"confirmed\"");
        otaLog(ctx, mcco::LogLevel::Info, "confirmed", nullptr, nullptr, detail);
    } else {
        // Never retry in a tight loop; the bootloader still guards the slot.
        g_selfcheck_pending = false;
    }
}

void noteStatusServed(AppContext* ctx) {
    (void)ctx;
    g_status_served = true;
}

void handleUpload(AppContext* ctx, WiFiClient& client, uint32_t content_length,
                  const std::string& buffered, const char* request_id, const char* actor) {
    if (g_busy) {
        sendErrorClose(ctx, client, mcco::ErrCode::OtaInProgress, request_id);
        return;
    }
    const esp_partition_t* target =
        esp_ota_get_next_update_partition(esp_ota_get_running_partition());
    if (!target || target->type != ESP_PARTITION_TYPE_APP) {
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"no_target_slot\"");
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::InternalError, request_id);
        return;
    }
    if (!mcco::ota_container_size_valid(content_length, target->size)) {
        char detail[160];
        detailVersion(detail, sizeof(detail),
                      ",\"reason\":\"invalid_size\",\"declared\":%u,\"slot\":%u",
                      (unsigned)content_length, (unsigned)target->size);
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::BadRequest, request_id);
        return;
    }
    g_busy = true;
    const uint32_t deadline = millis() + kOtaIoTimeoutMs;
    size_t cursor = 0; // drains `buffered` first, then the socket

    uint8_t sig[mcco::kOtaSignatureLen];
    if (!readMixed(client, buffered, cursor, sig, sizeof(sig), deadline)) {
        char detail[96];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"truncated_signature\"");
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::BadRequest, request_id);
        g_busy = false;
        return;
    }

    esp_task_wdt_reset(); // Update.begin erases the slot (seconds, blocking)
    const uint32_t image_len = content_length - mcco::kOtaSignatureLen;
    if (!Update.begin(image_len, U_FLASH)) {
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"begin_failed\",\"err\":%u",
                      (unsigned)Update.getError());
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::InternalError, request_id);
        g_busy = false;
        return;
    }

    static uint8_t chunk[kStreamChunk]; // BSS (AGENTS.md: no big stack/heap churn)
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    uint32_t remaining = image_len;
    bool stream_ok = true;
    while (remaining > 0) {
        if ((int32_t)(millis() - deadline) > 0 || !client.connected()) {
            stream_ok = false;
            break;
        }
        const size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        int r = 0;
        if (cursor < buffered.size()) {
            // Parser-buffered prefix: no socket wait needed.
            const size_t take = want < buffered.size() - cursor ? want : buffered.size() - cursor;
            memcpy(chunk, buffered.data() + cursor, take);
            cursor += take;
            r = (int)take;
        } else {
            r = client.read(chunk, want);
        }
        if (r > 0) {
            if (Update.write(chunk, (size_t)r) != (size_t)r) {
                stream_ok = false;
                break;
            }
            mbedtls_sha256_update_ret(&sha, chunk, (size_t)r);
            remaining -= (uint32_t)r;
        } else {
            esp_task_wdt_reset(); // the 120 s stream must never starve the WDT
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (!stream_ok) {
        Update.abort();
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"stream_incomplete\"");
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::BadRequest, request_id);
        g_busy = false;
        return;
    }
    if (!verifySignature(digest, sig)) {
        Update.abort(); // slot is not bootable: signature never verified
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"signature_invalid\"");
        otaLog(ctx, mcco::LogLevel::Error, "signature_invalid", request_id, actor,
               detail);
        sendErrorClose(ctx, client, mcco::ErrCode::BadRequest, request_id);
        g_busy = false;
        return;
    }
    if (!Update.end(true)) {
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"end_failed\",\"err\":%u",
                      (unsigned)Update.getError());
        otaLog(ctx, mcco::LogLevel::Error, "upload_rejected", request_id, actor, detail);
        sendErrorClose(ctx, client, mcco::ErrCode::InternalError, request_id);
        g_busy = false;
        return;
    }

    char version[32] = "unknown";
    scanVersion(target, version, sizeof(version));

    char detail[192];
    detailVersion(detail, sizeof(detail),
                  ",\"image_version\":\"%s\",\"size\":%u,\"slot\":\"%s\"", version,
                  (unsigned)content_length, target->label);
    otaLog(ctx, mcco::LogLevel::Info, "upload_validated", request_id, actor, detail);
    if (mcco::ota_is_downgrade(version, MC_FW_VERSION)) {
        char ddown[192];
        detailVersion(ddown, sizeof(ddown), ",\"image_version\":\"%s\"", version);
        otaLog(ctx, mcco::LogLevel::Warn, "downgrade", request_id, actor, ddown);
    }

    g_pending_partition = *target;
    g_pending_validated = true;
    strncpy(g_pending_version, version, sizeof(g_pending_version) - 1);
    g_pending_version[sizeof(g_pending_version) - 1] = '\0';

    char body[96];
    snprintf(body, sizeof(body), "{\"validated\":true,\"version\":\"%s\"}", version);
    sendJsonClose(client, 200, body);
    g_busy = false;
}

bool parseApplyBody(const std::string& body, bool& force) {
    force = false;
    if (body.empty()) return true; // absent body == {"force":false}
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) return false;
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        if (strcmp(kv.key().c_str(), "force") != 0) return false;
    }
    if (doc["force"].isNull()) return true;
    if (!doc["force"].is<bool>()) return false;
    force = doc["force"].as<bool>();
    return true;
}

bool apply(AppContext* ctx, bool force, const char* request_id, const char* actor,
           char* resp, size_t resp_cap, mcco::ErrCode& err) {
    if (g_busy) {
        err = mcco::ErrCode::OtaInProgress;
        return false;
    }
    if (!g_pending_validated) {
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"no_validated_image\"");
        otaLog(ctx, mcco::LogLevel::Error, "apply_rejected", request_id, actor, detail);
        err = mcco::ErrCode::BadRequest;
        return false;
    }
    size_t in_flight = 0;
    {
        Guard g(ctx->engine_mutex);
        in_flight = ctx->engine->count_non_terminal();
    }
    if (in_flight > 0 && !force) {
        err = mcco::ErrCode::OtaInProgress;
        return false;
    }
    if (in_flight > 0) {
        Guard g(ctx->engine_mutex);
        ctx->engine->terminate_all_non_terminal();
    }
    if (esp_ota_set_boot_partition(&g_pending_partition) != ESP_OK) {
        char detail[128];
        detailVersion(detail, sizeof(detail), ",\"reason\":\"set_boot_failed\"");
        otaLog(ctx, mcco::LogLevel::Error, "apply_rejected", request_id, actor, detail);
        err = mcco::ErrCode::InternalError;
        return false;
    }
    g_busy = true;
    char detail[192];
    detailVersion(detail, sizeof(detail),
                  ",\"image_version\":\"%s\",\"force\":%s,\"terminated\":%u",
                  g_pending_version, force ? "true" : "false", (unsigned)in_flight);
    otaLog(ctx, mcco::LogLevel::Info, "apply", request_id, actor, detail);
    snprintf(resp, resp_cap, "{\"applied\":true,\"version\":\"%s\",\"reboot\":true}",
             g_pending_version);
    return true;
}

void statusJson(char* buf, size_t cap) {
    snprintf(buf, cap,
             "{\"running_version\":\"%s\",\"pending_version\":%s%s%s,"
             "\"ota_in_progress\":%s,\"self_check_pending\":%s}",
             MC_FW_VERSION, g_pending_validated ? "\"" : "",
             g_pending_validated ? g_pending_version : "null",
             g_pending_validated ? "\"" : "", g_busy ? "true" : "false",
             g_selfcheck_pending ? "true" : "false");
}

void rebootNow() {
    delay(200); // let the response drain before the reset
    esp_restart();
}

} // namespace ota
