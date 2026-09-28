# DEBUG — Stale mDNS hostname after bench rename (mac-b53478 → control-graphics)

2026-09-27. Post-Phase-5 flash verification. Cost ~25 min of false
trail-chasing; root cause was bench config, not the new firmware.

## Symptom

The paired MCA (launchd or AT-harness daemon) logged `hello_ack timeout;
backing off` on every WS reconnect, forever, against the freshly flashed
Phase 5 binary. The device showed **no** session-log entries for any attempt
(not `agent_hello`, not `agent_hello_timeout`, not `agent_rejected`), while
manual `curl` WS-upgrade probes to the same endpoint got a 101 and the
expected hello-timeout close, and a raw `websockets` client sending a valid
`agent_hello` (correct token + instance) got a proper `hello_ack` instantly.

## Root cause

The bench S3 had been renamed ("MacControl for Graphics Computer",
hostname `control-graphics`) — its mDNS A record is `control-graphics.local`
→ 10.10.40.242, TTL 120. The AT agent state file
(`/tmp/mc_at_agent.json`) still stored `hostname: mac-b53478.local`, a name
that no longer has an mDNS record. macOS mDNSResponder intermittently
served a **stale cached A record** for the old name, so resolution sometimes
succeeded (ping/curl worked at random moments) and sometimes failed
 outright (`getaddrinfo` gaierror). When the stale entry pointed at an IP
that no longer hosted the endpoint, TCP either never connected or connected
to the wrong host — and the agent's retry loop mislabeled all of it.

## Contributing trap (agent-side)

`transport.py`'s reconnect loop catches **any** failure between
"open socket" and "hello_ack received" and logs it as
`hello_ack timeout; backing off` — including connect timeouts, DNS failures,
and upgrade-handshake hangs. Only an HTTP-status refusal gets its own
message (`upgrade refused (HTTP %s)`). So the log line does NOT mean the
hello was sent and unacknowledged; check name resolution and TCP reachability
before suspecting the endpoint's WS path.

## Diagnostic path that worked (network-only, no serial)

1. Device alive? `dns-sd -G v4 <name>.local` for BOTH the old and current
   names; compare against `arp -a` (the ESP32's MAC is `7c:4f:ad:b5:34:78`).
2. HTTP task alive? unauthenticated `curl` → expect a clean 401 envelope;
   a 401 proves WiFi + HTTP + auth layers.
3. WS path alive? `curl` with Upgrade headers + any bearer → expect
   `101 Switching Protocols`, then a `hello timeout` close ~5 s later.
4. Full hello path? raw `websockets` client (agent venv has the lib) with the
   real token + instance id from the agent state file → expect `hello_ack`.
5. Only when all four pass and the real agent still fails: the agent is
   talking to something else — re-check the hostname in its state file.

## Fix / prevention

- The agent state file hostname must track the device's identity hostname
  (AT scripts take `--hostname`; stale `/tmp/mc_at_agent.json` files are
  suspect after any bench rename).
- The endpoint's `GET /api/v1/status → device.hostname` is ground truth for
  what mDNS should answer.
- Doc landmines updated in HANDOFF.md; agent README notes the log mislabel.
