# companion-module-maccontrol

Bitfocus Companion module for a **MacControl ESP32 endpoint** — Stream Deck
buttons for macros, Mac power/session commands, app launch/quit, and live
status/feedback. See [companion/HELP.md](companion/HELP.md) for user docs.

## Requirements

- Companion **4.3+ / 5.0.x**
- Node 22 (module runtime is `node22`)
- A MacControl device on the LAN with a **CONTROL-role** (or better) API key

## Development

```bash
npm install        # requires npm (not Yarn); --legacy-peer-deps if needed
npm test           # pure unit tests (no network)
npm run test:live  # live read-only tests against the real device (GET only, never POST)
npm run check      # companion-module-check (see note below)
```

### Note on `companion-module-check`

The current `@companion-module/tools` release expects the 1.x layout of
`@companion-module/base` (its `validateManifest` import), so the check script
crashes against base 2.x. As an equivalent, the manifest is validated with the
2.x validator and all sources pass a syntax/import check:

```bash
node -e "import('@companion-module/base/manifest').then(({validateManifest}) =>
  validateManifest(JSON.parse(require('fs').readFileSync('companion/manifest.json','utf8'))))"
node --check src/main.js && node --check src/api.js && node --check src/state.js
```

## Install (users)

This is a developer module.

**Option A — Companion Launcher (recommended):**

1. Open Companion Launcher → cog icon → **Advanced Settings**.
2. Enable **Developer modules** and pick the folder **containing** this
   module's folder (e.g. `.../integrations/`, not the module folder itself).
3. Restart Companion. "MacControl Endpoint" appears on the Connections page —
   add a connection, set the hostname and API key.

**Option B — manual:**

Copy (or symlink) this folder into `~/companion/modules/`, e.g.

```bash
ln -s "$PWD" ~/companion/modules/companion-module-maccontrol
```

then restart Companion.

## Layout

| Path | Purpose |
| --- | --- |
| `src/main.js` | `InstanceBase` subclass: poller, actions, feedbacks, variables, presets |
| `src/api.js` | Pure REST client (no Companion imports) with typed errors |
| `src/state.js` | Change-detection / record-tracking state (pure) |
| `companion/manifest.json` | Module manifest (node22 runtime) |
| `companion/HELP.md` | In-Companion help |
| `test/` | `node:test` suites: unit (`state`, `api`) + live read-only (`api.live`) |

## Rate limiting

The poller stays under the device's 30 req/min CONTROL cap (~27.5/min at the
default 5 s interval) and backs off exponentially on `429`. See
[companion/HELP.md](companion/HELP.md) for details.
