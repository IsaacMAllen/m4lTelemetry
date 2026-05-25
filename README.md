# bz.telemetry

Opt-in error reporting + analytics framework for **bugbytz** Max for Live
devices.  Drop a single `[bz.telemetry]` object into any device and you
get:

- A persistent, per-user **consent flag** so the installer prompt only
  shows up once across every bugbytz device.
- An **anonymous device ID** (UUID, generated on first run) that lets you
  correlate events from the same install without learning anything PII.
- A **disk-backed event queue** that survives offline sessions, host
  crashes and force-quits.  Pending events drain on the next launch.
- A **background uploader thread** that POSTs newline-delimited JSON to
  any REST endpoint you control.
- Opt-in **crash capture** via `std::set_terminate` and platform crash
  hooks (POSIX signals on macOS; `SetUnhandledExceptionFilter` on
  Windows).  Crashes write tombstone files on disk and are converted to
  upload events the next time any bugbytz device loads.

The whole thing is one shared singleton — multiple `[bz.telemetry]`
boxes in different devices reuse the same uploader and queue, so loading
ten bugbytz devices in one Live set does **not** start ten threads.

---

## Files

| File | Platform | Purpose |
| --- | --- | --- |
| `bz.telemetry.cpp`        | all     | Min external `[bz.telemetry]` |
| `telemetry_core.hpp`      | all     | Public C++ API for re-use by other externals |
| `telemetry_core.cpp`      | all     | Consent + queue + worker thread (cross-platform) |
| `telemetry_http.mm`       | macOS   | HTTP transport (`NSURLSession`) |
| `telemetry_crash.mm`      | macOS   | Crash hooks (POSIX signals + `std::terminate`) |
| `telemetry_http_win.cpp`  | Windows | HTTP transport (WinHTTP) + platform helpers |
| `telemetry_crash_win.cpp` | Windows | Crash hooks (`SetUnhandledExceptionFilter` + `std::terminate`) |
| `CMakeLists.txt`          | all     | Build target |

The help patcher lives at `help/bz.telemetry.maxhelp` and contains a
ready-to-copy installer-prompt panel.

---

## Wiring it into a new device

```
[bz.telemetry @device "livesaver" @version "2.0.4" @endpoint "https://telemetry.bugbytz.com/v1/events"]
       |        |         |         |
       |        |         |         +-- last_error / device_id / etc.
       |        |         +------------ list:  queue <pending> <sent> <dropped>
       |        +---------------------- int:   1 if last upload OK, else 0
       +------------------------------- symbol: unknown / granted / denied
```

### 1. Consent prompt

On load the external emits the current consent state out outlet 1.
The first time the device runs on a machine the value will be `unknown`
— show your installer dialog, and route the user's answer back into the
inlet:

```
[route unknown granted denied]
   |
   +- show prompt panel  →  [consent_grant ( | [consent_revoke (  →  [bz.telemetry]
```

Consent is persisted in the platform app-support directory:
- **macOS** — `~/Library/Application Support/bugbytz/telemetry/config.json`
- **Windows** — `%APPDATA%\bugbytz\telemetry\config.json`

It applies to **every** bugbytz device on the machine, so the user is only
ever asked once.

### 2. Logging

Send any of these messages to the inlet:

| Message | Effect |
| --- | --- |
| `log_event <name> [k v]...`            | Custom analytics event. |
| `log_error <name> <message> [k v]...`  | Recoverable error. |
| `log_metric <name> <number> [unit] [k v]...` | Numeric metric. |
| `report_crash <name> <message> [k v]...` | Manually report a fatal condition (e.g. an exception caught in JS). |
| `set_user <id>`                        | Attach an optional user identifier (e.g. an email) to subsequent events. |
| `flush`                                | Force an immediate upload attempt. |
| `clear_queue`                          | Drop pending events from disk. |
| `consent_grant` / `consent_revoke` / `consent_reset` | Update consent. |
| `status` / `bang`                      | Re-emit all status outlets. |
| `storage_dir`                          | Print the storage path to the Max console. |

### 3. Attributes

All attributes can be set at instantiation time with `@attr value` or
later by sending `<attr> <value>`.

| Attribute | Default | Description |
| --- | --- | --- |
| `endpoint`        | `""`         | REST URL.  Leave empty to queue-only. |
| `token`           | `""`         | Optional bearer token. |
| `vendor`          | `"bugbytz"`  | Top-level group + storage subdir. |
| `device`          | `""`         | Device name, e.g. `"livesaver"`. |
| `version`         | `""`         | Device version, e.g. `"2.0.4"`. |
| `flush_interval`  | `30`         | Seconds between upload attempts. |
| `dry_run`         | `0`          | If 1, never actually POST. |
| `crash_capture`   | `1`          | Install signal + `std::terminate` hooks. |

---

## REST API contract

The uploader POSTs **newline-delimited JSON** (`Content-Type:
application/x-ndjson`) to your endpoint.  Each line is one fully-stamped
event:

```json
{"vendor":"bugbytz","device_name":"livesaver","device_version":"2.0.4",
 "device_id":"f47ac10b-58cc-4372-a567-0e02b2c3d479",
 "session_id":"3c1c39a6-...","user_id":"",
 "platform":"macOS 14.4.1","max_version":"Live 12.1.0",
 "type":"event","level":"info","name":"device_loaded",
 "ts":"2026-05-23T12:34:56.789Z","ts_ms":1748000096789,
 "props":{"build":"May 23 2026 08:51:00"}}
```

Required envelope fields on every line:

- `vendor`           — `"bugbytz"` unless overridden
- `device_name`      — value of the `@device` attribute
- `device_version`   — value of the `@version` attribute
- `device_id`        — anonymous UUID, persistent per machine
- `session_id`       — UUID re-rolled per process
- `user_id`          — empty unless `set_user` was sent
- `platform`         — e.g. `"macOS 14.4.1"`
- `max_version`      — best-effort host process version (Live / Max / etc.)

Per-event fields:

- `type`             — `event` / `error` / `metric` / `crash`
- `level`            — `info` / `warning` / `error` / `fatal`
- `name`             — caller-supplied event name
- `message`          — free-form (errors / crashes only)
- `value` + `unit`   — numeric metrics only
- `ts` + `ts_ms`     — ISO-8601 + Unix ms
- `props`            — flat string→string map of extra attributes

The receiver should:

1. Authenticate via the bearer token (if you set one).
2. Reply with **2xx on success**.  Anything else makes the uploader
   keep the events on disk and retry on the next flush interval.
3. Be idempotent — a flaky network can re-deliver the same event id
   if the response is lost.

### Reference receiver (Express)

```js
import express from "express";
import readline from "readline";
import { Readable } from "stream";

const app = express();
app.post("/v1/events", async (req, res) => {
  if (req.headers.authorization !== `Bearer ${process.env.BZ_TOKEN}`)
    return res.sendStatus(401);
  const lines = [];
  const rl = readline.createInterface({ input: Readable.from(req) });
  for await (const line of rl) if (line.trim()) lines.push(JSON.parse(line));
  // ... persist lines somewhere ...
  res.sendStatus(204);
});
app.listen(8080);
```

---

## On-disk layout

**macOS** (`~/Library/Application Support/bugbytz/telemetry/`):

```
bugbytz/telemetry/
├── config.json          # consent + anonymous device id (~200 bytes)
├── pending/             # one file per queued event
│   └── 00000017482137...-abc123.json
└── tombstones/          # crash dumps written async-signal-safely
    └── 1748213723-11-12345.tomb
```

**Windows** (`%APPDATA%\bugbytz\telemetry\`):

```
bugbytz\telemetry\
├── config.json
├── pending\
│   └── 00000017482137...-abc123.json
└── tombstones\
    └── 20260523-123456-0000005-1234.tomb
```

`pending/<ts_ms>-<uuid>.json` filenames are sortable so the worker
uploads in chronological order.  On success the file is deleted.

`tombstones/*.tomb` are simple `key=value\n` text files written using
only async-signal-safe system calls (no heap allocation, no Objective-C
or COM).  On the next launch any `[bz.telemetry]` instance drains the
folder and converts each entry to a `crash` event.

The user can clear everything by sending `clear_queue` or by deleting
the folder manually.

---

## Privacy notes worth saying in the prompt

- Nothing is uploaded until the user opts in.  Without consent the
  external still tracks counts (so you can show "0 pending" in the UI)
  but **drops every event on the floor**.
- The `device_id` is a randomly-generated UUID, not derived from the
  hardware, MAC address or username.
- No project content, audio, file paths, or window contents are
  captured.  Only what the device explicitly logs via `log_event` /
  `log_error` / `log_metric`.
- Crash tombstones contain the signal name, signal info, faulting
  address, the `device_id` / `device_name` / `device_version` and the
  current session id — nothing else.

---

## Re-using the framework from another C++ external

The Max wrapper is optional.  If you want to call into the framework
from another external in this repo, just `#include "telemetry_core.hpp"`
and link against `telemetry_core.cpp` + the appropriate platform files
(`telemetry_http.mm` + `telemetry_crash.mm` on macOS, or
`telemetry_http_win.cpp` + `telemetry_crash_win.cpp` on Windows).
Alternatively, add this folder as a CMake `target_link_libraries` after
exposing the `bz.telemetry` target as a library (see CMakeLists.txt).

```cpp
#include "telemetry_core.hpp"

bz::telemetry::config cfg;
cfg.endpoint        = "https://telemetry.bugbytz.com/v1/events";
cfg.device_name     = "livesaver";
cfg.device_version  = "2.0.4";
bz::telemetry::client::get().start(cfg);

bz::telemetry::client::get().log_error("save_failed", e.what(), {
    {"path", project_path},
    {"errno", std::to_string(errno)},
});
```

The singleton refcounts itself so multiple modules sharing the process
all use the same queue.

---

## Roadmap

- [ ] Stack traces in crash tombstones (libunwind / StackWalk64).
- [x] WinHTTP backend so devices ship cross-platform.
- [ ] Optional Sentry-compatible payload format toggle.
- [ ] Sampling for high-volume `log_event` / `log_metric` calls.
