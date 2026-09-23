#include "mc_openapi.h"

namespace mcco {

const char kOpenApiJson[] = R"json({
  "openapi": "3.1.0",
  "info": {
    "title": "MacControl Endpoint API",
    "version": "1",
    "description": "Deterministic Mac controller surface (spec ch. 12). AMENDMENT NOTE: the following routes are ESP32-owned additions beyond the spec ch. 12 inventory and exist to support the Web UI (spec ch. 14): /api/v1/triggers*, /api/v1/device/identity, /api/v1/keys*, /api/v1/pairing* and the /ui/* session endpoints. A valid mc_session cookie is treated as the ADMIN role on /api/v1/* per the spec ch. 14 binding rule. API keys and the pairing record are managed only through an authenticated Web UI session (spec 13.1.1), never with an API key. Agent endpoints: /agent/v1/pair completes the pairing ceremony (spec 3.2); /agent/v1/ws is the MCA WebSocket channel on port 80 via HTTP Upgrade (spec 4.2.1); /agent/v1/events and /agent/v1/commands/pending are the polling fallback (spec 4.3.1). In Mode A, /api/v1/agent/status and /api/v1/apps/{bundle_id}/launch|quit return 409 agent_not_paired and create no ledger records. Phase 4 interim: power/lock/macro verification predicates (spec 5.3.1) are not yet implemented, so in Mode B those commands terminate unconfirmed/hid_only and every capabilities verified flag stays false except app_launch/app_quit with a live agent session; the monitored-app registry (display names, app_not_registered/app_control_disabled) is Phase 6. The OTA endpoints of spec ch. 12.1.1 are not yet implemented and are intentionally absent from this document."
  },
  "servers": [{"url": "http://{hostname}.local", "variables": {"hostname": {"default": "mac-b53478"}}}],
  "tags": [
    {"name": "status"},
    {"name": "commands"},
    {"name": "macros"},
    {"name": "triggers"},
    {"name": "security"},
    {"name": "logs"},
    {"name": "agent"},
    {"name": "webui"}
  ],
  "components": {
    "securitySchemes": {
      "bearerAuth": {"type": "http", "scheme": "bearer", "description": "API key: 'mck_' + 32 random bytes base64url (spec 13.1.1)"},
      "sessionCookie": {"type": "apiKey", "in": "cookie", "name": "mc_session", "description": "Web UI session; acts as ADMIN on /api/v1/* (spec ch. 14)"}
    },
    "schemas": {
      "Error": {
        "type": "object",
        "required": ["error"],
        "properties": {
          "error": {
            "type": "object",
            "required": ["code", "message", "request_id"],
            "properties": {
              "code": {"type": "string", "enum": ["bad_request", "unauthorized", "forbidden", "not_found", "conflict", "agent_not_paired", "agent_offline", "command_disabled", "app_not_allowlisted", "app_not_registered", "app_control_disabled", "validation_failed", "macro_invalid_step", "macro_queue_full", "store_corrupt", "rate_limited", "internal_error", "ledger_unavailable", "network_unavailable"]},
              "message": {"type": "string"},
              "request_id": {"type": "string"}
            }
          }
        }
      },
      "Tuple": {
        "type": "object",
        "required": ["value", "source", "observed_at", "ttl_s", "freshness"],
        "properties": {
          "value": {},
          "source": {"type": "string"},
          "observed_at": {"type": ["string", "null"]},
          "ttl_s": {"type": ["integer", "null"]},
          "freshness": {"type": "string"}
        }
      },
      "Status": {
        "type": "object",
        "properties": {
          "generated_at": {"type": "string"},
          "cache_epoch": {"type": "integer"},
          "device": {"type": "object", "properties": {"name": {"$ref": "#/components/schemas/Tuple"}, "hostname": {"$ref": "#/components/schemas/Tuple"}, "device_id": {"$ref": "#/components/schemas/Tuple"}}},
          "connection": {"type": "object", "properties": {"usb": {"$ref": "#/components/schemas/Tuple"}, "network": {"$ref": "#/components/schemas/Tuple"}, "agent": {"$ref": "#/components/schemas/Tuple"}}},
          "mac": {"type": "object", "properties": {"state": {"$ref": "#/components/schemas/Tuple"}, "locked": {"$ref": "#/components/schemas/Tuple"}, "user_logged_in": {"$ref": "#/components/schemas/Tuple"}, "user": {"$ref": "#/components/schemas/Tuple"}, "boot_id": {"$ref": "#/components/schemas/Tuple"}}},
          "applications": {"type": "object"}
        }
      },
      "CommandSubmission": {
        "type": "object",
        "required": ["type"],
        "properties": {
          "type": {"type": "string", "enum": ["wake", "sleep", "restart", "shutdown", "lock", "macro_execute", "app_launch", "app_quit"]},
          "parameters": {"type": "object"},
          "idempotency_key": {"type": "string"}
        }
      },
      "CommandRecord": {
        "type": "object",
        "properties": {
          "command_id": {"type": "string"},
          "idempotency_key": {"type": ["string", "null"]},
          "revision": {"type": "integer"},
          "type": {"type": "string"},
          "parameters": {"type": "object"},
          "requested_by": {"type": "string"},
          "requested_at": {"type": "string"},
          "mode_at_accept": {"type": "string"},
          "state": {"type": "string", "enum": ["accepted", "dispatched", "confirming", "completed", "failed", "timed_out", "unconfirmed"]},
          "dispatched_at": {"type": ["string", "null"]},
          "deadline_at": {"type": "string"},
          "expected_offline_window": {"type": ["string", "null"]},
          "evidence": {"type": "array", "items": {}},
          "result": {"type": ["string", "null"]},
          "error_code": {"type": ["string", "null"]}
        }
      },
      "MacroStep": {
        "type": "object",
        "required": ["order", "type"],
        "properties": {
          "order": {"type": "integer"},
          "type": {"type": "string", "enum": ["key_press", "key_combo", "modifier_down", "modifier_up", "key_release", "text", "delay"]},
          "key": {"type": "string"},
          "modifiers": {"type": "array", "items": {"type": "string", "enum": ["ctrl", "shift", "alt", "cmd"]}},
          "text": {"type": "string"},
          "value": {"type": "string"},
          "delay_ms": {"type": "integer"},
          "expected_event": {"type": "string"}
        }
      },
      "Macro": {
        "type": "object",
        "properties": {
          "macro_id": {"type": "string"},
          "name": {"type": "string"},
          "revision": {"type": "integer"},
          "timeout_ms": {"type": "integer"},
          "steps": {"type": "array", "items": {"$ref": "#/components/schemas/MacroStep"}}
        }
      },
      "TriggerBinding": {
        "type": "object",
        "properties": {
          "trigger_id": {"type": "string"},
          "source": {"type": "string", "enum": ["http_button", "webui_button", "gpio"]},
          "macro_id": {"type": "string"},
          "enabled": {"type": "boolean"},
          "gpio": {"type": "object", "properties": {"pin": {"type": "integer"}, "edge": {"type": "string"}, "debounce_ms": {"type": "integer"}}}
        }
      },
      "KeyRecord": {
        "type": "object",
        "properties": {
          "key_id": {"type": "string"},
          "label": {"type": "string"},
          "role": {"type": "string", "enum": ["READ", "CONTROL", "ADMIN"]},
          "key_sha256": {"type": "string"},
          "created_at": {"type": "string"},
          "last_used_at": {"type": ["string", "null"]},
          "expires_at": {"type": ["string", "null"]},
          "state": {"type": "string", "enum": ["active", "revoked"]}
        }
      },
      "LogEntry": {
        "type": "object",
        "properties": {
          "seq": {"type": "integer"},
          "ts": {"type": "string"},
          "category": {"type": "string", "enum": ["command", "session", "auth", "config", "ota", "system"]},
          "event": {"type": ["string", "null"]},
          "level": {"type": "string", "enum": ["info", "warn", "error"]},
          "command_id": {"type": ["string", "null"]},
          "request_id": {"type": ["string", "null"]},
          "session_id": {"type": ["string", "null"]},
          "actor": {"type": ["string", "null"]},
          "detail": {"type": "object"}
        }
      },
      "LogPage": {
        "type": "object",
        "properties": {
          "entries": {"type": "array", "items": {"$ref": "#/components/schemas/LogEntry"}},
          "dropped": {"type": "integer"}
        }
      },
      "Capabilities": {
        "type": "object",
        "properties": {
          "api_version": {"type": "string"},
          "mode": {"type": "string", "enum": ["A", "B"]},
          "capability_level": {"type": "string"},
          "device": {"type": "object", "properties": {"name": {"type": "string"}, "hostname": {"type": "string"}}},
          "commands": {"type": "object"},
          "agent": {"type": "object", "properties": {"paired": {"type": "boolean"}, "connected": {"type": "boolean"}, "enabled_commands": {"type": "array", "items": {"type": "string"}}, "allowlisted_apps": {"type": "array", "items": {"type": "string"}}}}
        }
      },
      "Identity": {
        "type": "object",
        "properties": {
          "device_name": {"type": "string"},
          "hostname": {"type": "string"},
          "location": {"type": "string"},
          "description": {"type": "string"},
          "device_id": {"type": "string"}
        }
      }
    },
    "responses": {
      "Error": {"description": "Deterministic error envelope", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "Unauthorized": {"description": "401 unauthorized (missing/invalid/expired key, or lockout)", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "Forbidden": {"description": "403 forbidden (insufficient role or revoked)", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}},
      "NotFound": {"description": "404 not_found (closed surface)", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}}
    },
    "parameters": {
      "category": {"name": "category", "in": "query", "schema": {"type": "string", "enum": ["command", "session", "auth", "config", "ota", "system"]}},
      "level": {"name": "level", "in": "query", "schema": {"type": "string", "enum": ["info", "warn", "error"]}},
      "sinceSeq": {"name": "since_seq", "in": "query", "schema": {"type": "integer"}},
      "limit": {"name": "limit", "in": "query", "schema": {"type": "integer", "default": 100, "maximum": 512}}
    }
  },
  "security": [{"bearerAuth": []}],
  "paths": {
    "/": {
      "get": {"tags": ["webui"], "summary": "Web UI document (no auth; login form is part of the page)", "responses": {"200": {"description": "HTML page"}}}
    },
    "/ui": {
      "get": {"tags": ["webui"], "summary": "Alias of GET /", "responses": {"200": {"description": "HTML page"}}}
    },
    "/ui/session": {
      "get": {"tags": ["webui"], "summary": "Session state", "responses": {"200": {"description": "{authenticated, password_set}"}}}
    },
    "/ui/login": {
      "post": {"tags": ["webui"], "summary": "Password login; 5 failures -> 60 s lockout. First login with no password set creates it (one-time setup).", "requestBody": {"required": true, "content": {"application/json": {"schema": {"type": "object", "required": ["password"], "properties": {"password": {"type": "string"}}}}}}, "responses": {"200": {"description": "ok"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "429": {"$ref": "#/components/responses/Error"}}}
    },
    "/ui/logout": {
      "post": {"tags": ["webui"], "summary": "End session", "responses": {"200": {"description": "ok"}}}
    },
    "/ui/password": {
      "post": {"tags": ["webui"], "summary": "Change admin password (session only, >= 10 chars)", "security": [{"sessionCookie": []}], "requestBody": {"required": true, "content": {"application/json": {"schema": {"type": "object", "required": ["password"], "properties": {"password": {"type": "string", "minLength": 10}}}}}}, "responses": {"200": {"description": "ok"}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/status": {
      "get": {"tags": ["status"], "summary": "Status cache document (READ)", "responses": {"200": {"description": "status", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Status"}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/capabilities": {
      "get": {"tags": ["status"], "summary": "Mode-aware capabilities (READ); live-generated", "responses": {"200": {"description": "capabilities", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Capabilities"}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/openapi.json": {
      "get": {"tags": ["status"], "summary": "This document (READ)", "responses": {"200": {"description": "OpenAPI 3.1 JSON"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/commands": {
      "post": {"tags": ["commands"], "summary": "Command submission (CONTROL); 202 + command_id; app_launch/app_quit rejected 409 agent_not_paired pre-ledger in Mode A", "requestBody": {"required": true, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/CommandSubmission"}}}}, "responses": {"202": {"description": "accepted", "content": {"application/json": {"schema": {"type": "object", "properties": {"command_id": {"type": "string"}, "state": {"type": "string"}, "deadline_at": {"type": "string"}, "record_url": {"type": "string"}}}}}}}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}, "429": {"$ref": "#/components/responses/Error"}}},
      "get": {"tags": ["commands"], "summary": "Ledger listing (READ) with filters", "parameters": [{"name": "state", "in": "query", "schema": {"type": "string"}}, {"name": "type", "in": "query", "schema": {"type": "string"}}, {"name": "since", "in": "query", "schema": {"type": "string"}}, {"name": "limit", "in": "query", "schema": {"type": "integer"}}, {"name": "cursor", "in": "query", "schema": {"type": "string"}}], "responses": {"200": {"description": "{commands[], next_cursor}", "content": {"application/json": {"schema": {"type": "object", "properties": {"commands": {"type": "array", "items": {"$ref": "#/components/schemas/CommandRecord"}}, "next_cursor": {"type": ["string", "null"]}}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/commands/{command_id}": {
      "get": {"tags": ["commands"], "summary": "Single ledger record, latest revision (READ)", "parameters": [{"name": "command_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "record", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/CommandRecord"}}}}, "404": {"$ref": "#/components/responses/NotFound"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/system/{command}": {
      "post": {"tags": ["commands"], "summary": "Convenience power routes (CONTROL): wake|sleep|restart|shutdown|lock; pure alias of POST /api/v1/commands", "parameters": [{"name": "command", "in": "path", "required": true, "schema": {"type": "string", "enum": ["wake", "sleep", "restart", "shutdown", "lock"]}}], "responses": {"202": {"description": "accepted"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}, "429": {"$ref": "#/components/responses/Error"}}}
    },
    "/api/v1/macros": {
      "get": {"tags": ["macros"], "summary": "Macro list (READ)", "responses": {"200": {"description": "{macros[]}", "content": {"application/json": {"schema": {"type": "object", "properties": {"macros": {"type": "array", "items": {"$ref": "#/components/schemas/Macro"}}}}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}},
      "post": {"tags": ["macros"], "summary": "Create macro (ADMIN); 400 macro_invalid_step on closed-schema violations", "requestBody": {"required": true, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Macro"}}}}, "responses": {"201": {"description": "created", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Macro"}}}}, "400": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}}}
    },
    "/api/v1/macros/{macro_id}": {
      "get": {"tags": ["macros"], "summary": "Read macro (READ)", "parameters": [{"name": "macro_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "macro", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Macro"}}}}, "404": {"$ref": "#/components/responses/NotFound"}}},
      "put": {"tags": ["macros"], "summary": "Update macro (ADMIN); bumps revision", "parameters": [{"name": "macro_id", "in": "path", "required": true, "schema": {"type": "string"}}], "requestBody": {"required": true, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Macro"}}}}, "responses": {"200": {"description": "updated"}, "400": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}}},
      "delete": {"tags": ["macros"], "summary": "Delete macro (ADMIN); auto-disables trigger bindings", "parameters": [{"name": "macro_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "deleted"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}}}
    },
    "/api/v1/macros/{macro_id}/execute": {
      "post": {"tags": ["macros"], "summary": "Execute macro, http_button trigger (CONTROL); 202", "parameters": [{"name": "macro_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"202": {"description": "accepted"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}, "409": {"$ref": "#/components/responses/Error"}, "429": {"$ref": "#/components/responses/Error"}}}
    },
    "/api/v1/triggers": {
      "get": {"tags": ["triggers"], "summary": "AMENDMENT: trigger bindings (READ)", "responses": {"200": {"description": "{triggers[]}", "content": {"application/json": {"schema": {"type": "object", "properties": {"triggers": {"type": "array", "items": {"$ref": "#/components/schemas/TriggerBinding"}}}}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}},
      "post": {"tags": ["triggers"], "summary": "AMENDMENT: bind trigger (ADMIN)", "requestBody": {"required": true, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/TriggerBinding"}}}}, "responses": {"201": {"description": "created"}, "400": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}}}
    },
    "/api/v1/triggers/{trigger_id}": {
      "put": {"tags": ["triggers"], "summary": "AMENDMENT: update binding (ADMIN)", "parameters": [{"name": "trigger_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "updated"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}}},
      "delete": {"tags": ["triggers"], "summary": "AMENDMENT: delete binding (ADMIN)", "parameters": [{"name": "trigger_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "deleted"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}}}
    },
    "/api/v1/device/identity": {
      "get": {"tags": ["security"], "summary": "AMENDMENT: read identity (READ)", "responses": {"200": {"description": "identity", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Identity"}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}}},
      "post": {"tags": ["security"], "summary": "AMENDMENT: update identity (ADMIN); hostname change re-announces mDNS within 2 s", "requestBody": {"required": true, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Identity"}}}}, "responses": {"200": {"description": "identity", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Identity"}}}}, "400": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}}}
    },
    "/api/v1/keys": {
      "get": {"tags": ["security"], "summary": "AMENDMENT: list API keys (Web UI session only, spec 13.1.1)", "security": [{"sessionCookie": []}], "responses": {"200": {"description": "{keys[]}", "content": {"application/json": {"schema": {"type": "object", "properties": {"keys": {"type": "array", "items": {"$ref": "#/components/schemas/KeyRecord"}}}}}}}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}}},
      "post": {"tags": ["security"], "summary": "AMENDMENT: create API key; raw key returned exactly once (Web UI session only)", "security": [{"sessionCookie": []}], "requestBody": {"required": true, "content": {"application/json": {"schema": {"type": "object", "required": ["role", "label"], "properties": {"role": {"type": "string", "enum": ["READ", "CONTROL", "ADMIN"]}, "label": {"type": "string"}}}}}}, "responses": {"201": {"description": "key record incl. one-time raw key"}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}}}
    },
    "/api/v1/keys/{key_id}": {
      "delete": {"tags": ["security"], "summary": "AMENDMENT: revoke API key (Web UI session only); immediate effect", "security": [{"sessionCookie": []}], "parameters": [{"name": "key_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "revoked"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}, "404": {"$ref": "#/components/responses/NotFound"}}}
    },
    "/api/v1/logs": {
      "get": {"tags": ["logs"], "summary": "Ring-buffer log (READ, spec 15.2); ascending seq; dropped counts lost entries", "parameters": [{"$ref": "#/components/parameters/category"}, {"$ref": "#/components/parameters/level"}, {"$ref": "#/components/parameters/sinceSeq"}, {"$ref": "#/components/parameters/limit"}], "responses": {"200": {"description": "log page", "content": {"application/json": {"schema": {"$ref": "#/components/schemas/LogPage"}}}}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/agent/status": {
      "get": {"tags": ["agent"], "summary": "Latest MCA status (READ); 409 agent_not_paired in Mode A", "responses": {"200": {"description": "agent status (Mode B)"}, "409": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/apps/{bundle_id}/launch": {
      "post": {"tags": ["agent"], "summary": "Launch allowlisted app via MCA (CONTROL); 409 agent_not_paired in Mode A, no ledger record", "parameters": [{"name": "bundle_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"202": {"description": "accepted (Mode B)"}, "409": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}}}
    },
    "/api/v1/apps/{bundle_id}/quit": {
      "post": {"tags": ["agent"], "summary": "Quit allowlisted app via MCA (CONTROL); 409 agent_not_paired in Mode A, no ledger record", "parameters": [{"name": "bundle_id", "in": "path", "required": true, "schema": {"type": "string"}}], "responses": {"202": {"description": "accepted (Mode B)"}, "409": {"$ref": "#/components/responses/Error"}, "403": {"$ref": "#/components/responses/Forbidden"}}}
    },
    "/api/v1/pairing": {
      "get": {"tags": ["agent"], "summary": "AMENDMENT: pairing state + record (Web UI session only); no secrets", "security": [{"sessionCookie": []}], "responses": {"200": {"description": "pairing state"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/pairing/window": {
      "post": {"tags": ["agent"], "summary": "AMENDMENT: open time-boxed pairing window 60-600 s (Web UI session only); returns the one-time code", "security": [{"sessionCookie": []}], "responses": {"200": {"description": "window opened with pairing_code"}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}}},
      "delete": {"tags": ["agent"], "summary": "AMENDMENT: close an open pairing window (Web UI session only)", "security": [{"sessionCookie": []}], "responses": {"200": {"description": "window closed"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/api/v1/pairing/revoke": {
      "post": {"tags": ["agent"], "summary": "AMENDMENT: revoke the active pairing (Web UI session only); immediate, closes in-flight sessions 4001", "security": [{"sessionCookie": []}], "responses": {"200": {"description": "revoked"}, "409": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}}}
    },
    "/agent/v1/pair": {
      "post": {"tags": ["agent"], "summary": "Pairing ceremony completion (spec 3.2.1); reachable only while a window is open; raw agent_token returned exactly once; 1 attempt/s, 5 failures close the window", "requestBody": {"required": true, "content": {"application/json": {"schema": {"type": "object", "required": ["pairing_code", "agent_instance_id"], "properties": {"pairing_code": {"type": "string"}, "agent_instance_id": {"type": "string"}, "agent_version": {"type": "string"}}}}}}, "responses": {"200": {"description": "pairing record + raw agent_token"}, "400": {"$ref": "#/components/responses/Error"}, "409": {"$ref": "#/components/responses/Error"}, "429": {"$ref": "#/components/responses/Error"}}}
    },
    "/agent/v1/ws": {
      "get": {"tags": ["agent"], "summary": "MCA WebSocket channel (spec 4.2.1); HTTP Upgrade on port 80; Authorization: Bearer <agent_token>; agent_hello then enveloped frames; close codes 1000/1001/1008/4001/4002/4003", "responses": {"101": {"description": "switching protocols"}}}
    },
    "/agent/v1/events": {
      "post": {"tags": ["agent"], "summary": "Polling-fallback event ingestion (spec 4.3.1); identical envelope; first agent_hello returns session_id, then X-Session-Id header", "parameters": [{"name": "X-Session-Id", "in": "header", "schema": {"type": "string"}}], "responses": {"200": {"description": "accepted (hello returns session_id)"}, "400": {"$ref": "#/components/responses/Error"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}}}
    },
    "/agent/v1/commands/pending": {
      "get": {"tags": ["agent"], "summary": "Polling-fallback dispatch drain (spec 4.3.1); returns actions awaiting MCA execution", "parameters": [{"name": "X-Session-Id", "in": "header", "required": true, "schema": {"type": "string"}}], "responses": {"200": {"description": "pending actions"}, "401": {"$ref": "#/components/responses/Unauthorized"}, "403": {"$ref": "#/components/responses/Forbidden"}, "409": {"$ref": "#/components/responses/Error"}}}
    }
  }
})json";

} // namespace mcco
