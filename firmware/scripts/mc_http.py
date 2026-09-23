"""Shared HTTP helper for the MacControl acceptance scripts.

Connection reuse (keep-alive) is load-bearing, not an optimization: the
ESP32-S3 test unit has a 320 KB part, and each fresh TCP connection costs
the firmware lwIP/Wi-Fi buffers that wedge the network stack under
sustained polling. Keep one connection per endpoint and retry (with a
pause — the server may need a moment to cut a stalled transfer before it
can serve the retry) on dropped sockets. Same signature/semantics as the
old urllib helper: returns (status, parsed_json_body), never raises for
HTTP error statuses.
"""
import http.client as http_client
import json
import time

_conns = {}


def http(method, base, path, key=None, body=None, timeout=10, extra_headers=None):
    hostport = base.split("://", 1)[1]
    headers = {}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    if extra_headers:
        headers.update(extra_headers)
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    headers["Connection"] = "keep-alive"

    def _once(conn):
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        return resp.status, resp.read()

    conn = _conns.get(hostport)
    if conn is None:
        conn = http_client.HTTPConnection(hostport, timeout=timeout)
        _conns[hostport] = conn
    last_err = None
    # Up to 3 attempts: the Wi-Fi link to the bench unit stalls mid-response
    # occasionally (lwIP TX stall; the firmware cuts it after ~4 s) — a fresh
    # connection after a short pause nearly always succeeds immediately.
    for attempt in range(3):
        try:
            status, raw = _once(conn)
            break
        except Exception as e:
            last_err = e
            try:
                conn.close()
            except Exception:
                pass
            conn = http_client.HTTPConnection(hostport, timeout=timeout)
            _conns[hostport] = conn
            if attempt < 2:
                time.sleep(1.0)
    else:
        return None, {"transport_error": str(last_err)}
    try:
        return status, json.loads(raw.decode() or "{}")
    except Exception:
        return status, {}
