#!/usr/bin/env python3
"""MacControl Phase 3 acceptance test AT-05 (spec section 16.1): capabilities
honesty in Mode A, plus the Phase 3 integration-contract checks (openapi.json
and the logs endpoint).

AT-05 (spec): every command type present in `commands` with `verified: false`;
`app_launch` reports `available: false` and submission fails with the
deterministic error envelope (409 agent_not_paired) without creating a ledger
record. Contract checks: /api/v1/openapi.json parses, covers the Chapter 12
paths plus the amendments, and a bogus path 404s; /api/v1/logs filters,
orders, clamps, and reports dropped entries.

Usage:
  python3 at05.py --hostname mac-a1b2c3.local \
      --read-key mck_... --control-key mck_...

Exit code 0 = all checks pass.
"""

import argparse
import json
import sys
import urllib.error
import urllib.request

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

CH12_PATHS = [
    "/api/v1/status",
    "/api/v1/capabilities",
    "/api/v1/commands",
    "/api/v1/commands/{command_id}",
    "/api/v1/system/{command}",
    "/api/v1/macros",
    "/api/v1/macros/{macro_id}",
    "/api/v1/macros/{macro_id}/execute",
    "/api/v1/agent/status",
    "/api/v1/logs",
    "/api/v1/openapi.json",
    "/api/v1/apps/{bundle_id}/launch",
    "/api/v1/apps/{bundle_id}/quit",
]

AMENDMENT_PATHS = [
    "/api/v1/triggers",
    "/api/v1/triggers/{trigger_id}",
    "/api/v1/device/identity",
    "/api/v1/keys",
    "/api/v1/keys/{key_id}",
]

COMMAND_TYPES = {"wake", "sleep", "restart", "shutdown", "lock",
                 "macro_execute", "app_launch", "app_quit"}


class Checker:
    def __init__(self):
        self.failures = []

    def check(self, label, cond, detail=""):
        status = PASS if cond else FAIL
        print(f"  [{status}] {label}" + (f" — {detail}" if detail and not cond else ""))
        if not cond:
            self.failures.append(f"{label}: {detail}")

    def section(self, title):
        print(f"\n=== {title} ===")


def http(method, base, path, key=None, body=None, timeout=10):
    url = base + path
    headers = {}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}
    except Exception as e:
        return None, {"transport_error": str(e)}


def error_code(doc):
    return (doc.get("error") or {}).get("code")


def at05_capabilities(checker, base, read_key, control_key):
    checker.section("AT-05 Capabilities honesty in Mode A")
    s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
    checker.check("GET /api/v1/capabilities -> 200", s == 200, f"status={s}")
    if s != 200:
        return

    checker.check("mode == 'A'", doc.get("mode") == "A", json.dumps(doc.get("mode")))
    checker.check("capability_level == 'L1'",
                  doc.get("capability_level") == "L1", json.dumps(doc.get("capability_level")))
    agent = doc.get("agent") or {}
    checker.check("agent.paired == false and agent.connected == false",
                  agent.get("paired") is False and agent.get("connected") is False,
                  json.dumps(agent))

    cmds = doc.get("commands") or {}
    checker.check("commands carries exactly the 8 closed types",
                  set(cmds.keys()) == COMMAND_TYPES, json.dumps(sorted(cmds.keys())))
    for name, entry in cmds.items():
        checker.check(f"{name} has available+verified and verified == false",
                      isinstance(entry, dict) and "available" in entry and
                      "verified" in entry and entry["verified"] is False,
                      json.dumps(entry))
    checker.check("app_launch available == false",
                  cmds.get("app_launch", {}).get("available") is False,
                  json.dumps(cmds.get("app_launch")))
    checker.check("app_quit available == false",
                  cmds.get("app_quit", {}).get("available") is False,
                  json.dumps(cmds.get("app_quit")))

    # Submission of an agent-dependent command: 409 agent_not_paired, and the
    # refusal MUST NOT create a ledger record.
    s, before = http("GET", base, "/api/v1/commands?limit=1", key=read_key)
    before_id = (before.get("commands") or [{}])[0].get("command_id") if s == 200 else None
    s, doc = http("POST", base, "/api/v1/commands", key=control_key,
                  body={"type": "app_launch", "parameters": {"bundle_id": "com.example.app"}})
    checker.check("app_launch submission -> 409 agent_not_paired",
                  s == 409 and error_code(doc) == "agent_not_paired",
                  f"status={s} body={doc}")
    s, after = http("GET", base, "/api/v1/commands?limit=1", key=read_key)
    after_id = (after.get("commands") or [{}])[0].get("command_id") if s == 200 else None
    checker.check("no ledger record created by the refusal",
                  before_id is not None and after_id == before_id,
                  f"before={before_id} after={after_id}")

    # Closed surface.
    s, doc = http("GET", base, "/api/v1/nope", key=read_key)
    checker.check("unknown path -> 404 not_found",
                  s == 404 and error_code(doc) == "not_found", f"status={s} body={doc}")


def contract_openapi(checker, base, read_key):
    checker.section("Contract: /api/v1/openapi.json")
    s, doc = http("GET", base, "/api/v1/openapi.json", key=read_key)
    checker.check("GET /api/v1/openapi.json -> 200", s == 200, f"status={s}")
    if s != 200:
        return
    checker.check("openapi version is 3.1.x",
                  str(doc.get("openapi", "")).startswith("3.1"), doc.get("openapi"))
    paths = doc.get("paths") or {}
    missing = [p for p in CH12_PATHS + AMENDMENT_PATHS if p not in paths]
    checker.check("all Chapter 12 + amendment paths present", not missing,
                  f"missing={missing}")
    desc = (doc.get("info") or {}).get("description", "")
    checker.check("amendment note present", "AMENDMENT" in desc and "/api/v1/keys" in desc)
    schemas = (doc.get("components") or {}).get("schemas") or {}
    for name in ("Error", "LogEntry", "KeyRecord", "CommandRecord"):
        checker.check(f"components.schemas.{name} present", name in schemas)
    if "Error" in schemas:
        codes = (schemas["Error"].get("properties", {}).get("error", {})
                 .get("properties", {}).get("code", {}).get("enum") or [])
        checker.check("error code table lists 13 closed codes", len(codes) == 13,
                      f"len={len(codes)}")


def contract_logs(checker, base, read_key):
    checker.section("Contract: /api/v1/logs (spec 15.2)")
    s, doc = http("GET", base, "/api/v1/logs", key=read_key)
    checker.check("GET /api/v1/logs -> 200", s == 200, f"status={s}")
    if s != 200:
        return
    entries = doc.get("entries") or []
    checker.check("entries present and non-empty", len(entries) > 0, f"count={len(entries)}")
    seqs = [e.get("seq") for e in entries]
    checker.check("entries ascending by seq", seqs == sorted(seqs) and all(seqs), str(seqs[:5]))
    checker.check("dropped count present", isinstance(doc.get("dropped"), int))

    s, doc2 = http("GET", base, "/api/v1/logs?limit=3", key=read_key)
    checker.check("limit respected", s == 200 and len(doc2.get("entries") or []) == 3,
                  f"status={s} count={len(doc2.get('entries') or [])}")

    s, doc3 = http("GET", base, "/api/v1/logs?limit=9999", key=read_key)
    checker.check("limit clamped to 512", s == 200 and len(doc3.get("entries") or []) <= 512,
                  f"count={len(doc3.get('entries') or [])}")

    s, doc4 = http("GET", base, "/api/v1/logs?category=auth", key=read_key)
    cats = {e.get("category") for e in (doc4.get("entries") or [])}
    checker.check("category filter returns only auth", s == 200 and cats == {"auth"},
                  f"status={s} cats={cats}")

    s, doc5 = http("GET", base, "/api/v1/logs?category=bogus", key=read_key)
    checker.check("invalid category -> 400", s == 400 and error_code(doc5) == "bad_request",
                  f"status={s} body={doc5}")

    if entries:
        latest = entries[-1]["seq"]
        s, doc6 = http("GET", base, f"/api/v1/logs?since_seq={latest}", key=read_key)
        checker.check("since_seq=latest returns no older entries",
                      s == 200 and all(e["seq"] > latest for e in (doc6.get("entries") or [])),
                      f"status={s}")
        # dropped must be reported when the cursor predates the oldest entry
        s, doc7 = http("GET", base, "/api/v1/logs?since_seq=0", key=read_key)
        first_seq = (doc7.get("entries") or [{}])[0].get("seq")
        if first_seq and first_seq > 1:
            checker.check("dropped > 0 when since_seq predates oldest entry",
                          (doc7.get("dropped") or 0) == first_seq - 1,
                          f"dropped={doc7.get('dropped')} first_seq={first_seq}")
        else:
            checker.check("no entries lost since seq 0 (dropped == 0)",
                          (doc7.get("dropped") or 0) == 0, f"dropped={doc7.get('dropped')}")

    s, doc8 = http("GET", base, "/api/v1/logs")
    checker.check("logs without credentials -> 401", s == 401 and error_code(doc8) == "unauthorized",
                  f"status={s}")


def contract_keys(checker, base, read_key):
    checker.section("Contract: /api/v1/keys (Web UI session only)")
    s, doc = http("GET", base, "/api/v1/keys")
    checker.check("keys without credentials -> 401", s == 401 and error_code(doc) == "unauthorized",
                  f"status={s}")
    s, doc = http("GET", base, "/api/v1/keys", key=read_key)
    checker.check("keys with API key -> 403 (session-only, spec 13.1.1)",
                  s == 403 and error_code(doc) == "forbidden", f"status={s} body={doc}")


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 3 acceptance AT-05 + contracts")
    ap.add_argument("--hostname", required=True)
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    args = ap.parse_args()

    checker = Checker()
    base = f"http://{args.hostname}:80"
    at05_capabilities(checker, base, args.read_key, args.control_key)
    contract_openapi(checker, base, args.read_key)
    contract_logs(checker, base, args.read_key)
    contract_keys(checker, base, args.read_key)

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-05 and Phase 3 contract checks: all checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
