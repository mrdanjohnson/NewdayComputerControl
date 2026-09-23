#include "ws_server.h"
#include <Arduino.h>
#include <string.h>
#include <esp_task_wdt.h>
#include "mc_ids.h"
#include "mc_sha1.h"

namespace ws {

namespace {
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// Mirrors the HTTP server's kWriteTimeoutMs: a stalled peer must fail the
// frame, not pin the agent task into the task-WDT abort (a momentary full
// socket buffer must also not drop the frame — retry until the deadline).
constexpr uint32_t kWsWriteTimeoutMs = 4000;

// Read result: 1 = exactly n bytes, 0 = deadline hit while connected,
// -1 = disconnect or socket error.
int read_fully(WiFiClient& client, uint8_t* buf, size_t n, uint32_t timeout_ms) {
    const uint32_t start = millis();
    size_t got = 0;
    while (got < n) {
        int r = client.read(buf + got, n - got);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (!client.connected()) return -1;
        if ((uint32_t)millis() - start >= timeout_ms) return 0;
        esp_task_wdt_reset();
        delay(1);
    }
    return 1;
}

bool write_fully(WiFiClient& client, const uint8_t* data, size_t n) {
    const uint32_t start = millis();
    size_t sent = 0;
    while (sent < n) {
        int w = client.write(data + sent, n - sent);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (!client.connected()) return false;
        if ((uint32_t)millis() - start >= kWsWriteTimeoutMs) {
            client.stop();
            return false;
        }
        esp_task_wdt_reset();
        delay(1);
    }
    return true;
}

bool send_frame(WiFiClient& client, uint8_t opcode, const uint8_t* payload, size_t len) {
    uint8_t hdr[10];
    size_t hl = 0;
    hdr[hl++] = 0x80 | opcode; // FIN + opcode (server frames are unmasked)
    if (len < 126) {
        hdr[hl++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[hl++] = 126;
        hdr[hl++] = (uint8_t)(len >> 8);
        hdr[hl++] = (uint8_t)len;
    } else {
        return false; // no single frame exceeds 64 KB here
    }
    if (!write_fully(client, hdr, hl)) return false;
    if (len > 0 && !write_fully(client, payload, len)) return false;
    client.flush();
    return true;
}
} // namespace

std::string handshake_accept(const std::string& key) {
    mcco::Sha1 sha;
    sha.update(key);
    sha.update(kGuid);
    uint8_t digest[20];
    sha.final(digest);
    return mcco::base64_encode(digest, sizeof(digest));
}

bool send_text(WiFiClient& client, const std::string& payload) {
    return send_frame(client, 0x1, reinterpret_cast<const uint8_t*>(payload.data()),
                      payload.size());
}

bool send_close(WiFiClient& client, uint16_t code, const char* reason) {
    uint8_t payload[125];
    size_t len = 0;
    if (code != 0) {
        payload[len++] = (uint8_t)(code >> 8);
        payload[len++] = (uint8_t)code;
        if (reason) {
            size_t rl = strlen(reason);
            if (rl > sizeof(payload) - 2) rl = sizeof(payload) - 2;
            memcpy(payload + len, reason, rl);
            len += rl;
        }
    }
    const bool ok = send_frame(client, 0x8, payload, len);
    client.flush();
    return ok;
}

bool send_pong(WiFiClient& client, const uint8_t* data, size_t len) {
    if (len > 125) len = 125;
    return send_frame(client, 0xA, data, len);
}

FrameStatus read_frame(WiFiClient& client, uint8_t* buf, size_t cap, size_t& len,
                       uint16_t& close_code, uint32_t timeout_ms) {
    len = 0;
    close_code = 0;
    const uint32_t start = millis();

    // First byte pair: a silent period is normal (agent heartbeat cadence),
    // so a timeout before any byte is not an error.
    uint8_t hdr[2];
    int r = read_fully(client, hdr, 2, timeout_ms);
    if (r == 0) return FrameStatus::Timeout;
    if (r < 0) return FrameStatus::Dropped; // EOF/RST without a close frame

    const bool fin = (hdr[0] & 0x80) != 0;
    const uint8_t opcode = hdr[0] & 0x0F;
    if (!fin || (hdr[0] & 0x70) != 0) { // no fragmented messages, no RSV bits
        send_close(client, 1002, "fragmented/RSV frame unsupported");
        return FrameStatus::Error;
    }
    const bool masked = (hdr[1] & 0x80) != 0;
    uint64_t plen = hdr[1] & 0x7F;
    if (!masked) { // RFC 6455 5.3: client MUST mask
        send_close(client, 1002, "client frame not masked");
        return FrameStatus::Error;
    }
    // The remaining header must arrive within the same deadline.
    const uint32_t remaining = (timeout_ms > (uint32_t)millis() - start)
                                   ? timeout_ms - ((uint32_t)millis() - start)
                                   : 1;
    if (plen == 126) {
        uint8_t ext[2];
        if (read_fully(client, ext, 2, remaining) != 1)
            return FrameStatus::Dropped; // peer vanished mid-header
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (read_fully(client, ext, 8, remaining) != 1) return FrameStatus::Dropped;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    if (plen > cap) {
        send_close(client, 1009, "frame too large");
        return FrameStatus::Error;
    }
    uint8_t mask[4];
    if (read_fully(client, mask, 4, remaining) != 1) return FrameStatus::Dropped;
    if (plen > 0 && read_fully(client, buf, (size_t)plen, remaining) != 1)
        return FrameStatus::Dropped;
    for (uint64_t i = 0; i < plen; i++) buf[i] ^= mask[i & 3];
    len = (size_t)plen;

    switch (opcode) {
        case 0x1: return FrameStatus::Text;
        case 0x2: return FrameStatus::Binary;
        case 0x8:
            if (len >= 2) close_code = (uint16_t)((buf[0] << 8) | buf[1]);
            return FrameStatus::Close;
        case 0x9: return FrameStatus::Ping;
        case 0xA: return FrameStatus::Pong;
        default:
            send_close(client, 1002, "unknown opcode");
            return FrameStatus::Error;
    }
}

} // namespace ws
