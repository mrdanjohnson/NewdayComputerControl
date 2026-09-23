#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <WiFiClient.h>

// RFC 6455 codec helpers on a WiFiClient (spec 4.2). Server frames are never
// masked; client frames MUST be masked (an unmasked client frame is a
// protocol error, close 1002). Fragmented messages are not supported.
// WiFiClient reads are non-blocking in this core, so read_frame enforces its
// own millisecond deadline (callers pass ~1 s in the session loop so outbound
// dispatches and liveness get serviced every iteration, ~5 s for the hello
// wait). The caller owns the receive buffer (one 4 KB buffer per task,
// PSRAM-backed); read pacing is a polled wait with delay(1) yields.
namespace ws {

enum class FrameStatus : uint8_t {
    Text,    // complete text frame in buf
    Binary,  // complete binary frame in buf
    Ping,    // ping frame; buf holds its payload
    Pong,    // pong frame; buf holds its payload
    Close,   // close frame; close_code filled (0 = no status payload)
    Timeout, // nothing arrived within the client read timeout
    Dropped, // socket died WITHOUT a close frame (unannounced loss)
    Error    // protocol error; a close frame was sent
};

// Sec-WebSocket-Accept for the handshake: base64(sha1(key + RFC 6455 GUID)).
std::string handshake_accept(const std::string& key);

bool send_text(WiFiClient& client, const std::string& payload);
bool send_close(WiFiClient& client, uint16_t code, const char* reason);
bool send_pong(WiFiClient& client, const uint8_t* data, size_t len);

// Reads one complete frame, blocking up to `timeout_ms` for the first byte
// (WiFiClient reads are non-blocking in this core, so the deadline is
// enforced here with a polled wait). On Text/Binary/Ping/Pong, buf holds the
// (unmasked) payload and len its size. Oversized frames are refused with
// close 1009. A mid-frame timeout or disconnect is unrecoverable (the partial
// frame is discarded): Error is returned and the caller tears the session
// down.
FrameStatus read_frame(WiFiClient& client, uint8_t* buf, size_t cap, size_t& len,
                       uint16_t& close_code, uint32_t timeout_ms);

} // namespace ws
