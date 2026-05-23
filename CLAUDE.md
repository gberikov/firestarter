# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`firestarter` is an **ESP-IDF firmware project** for the **ESP32-C6** (RISC-V). The device boots as a Wi-Fi access point serving a single web page; pressing the on-page button runs a 9-second audible countdown and then pulses a GPIO that drives a MOSFET to fire a pyrotechnic igniter ("rocket launch"). UI text, log messages, and code comments are in **Russian**.

> Safety: the firmware energizes `MOSFET_GPIO` (GPIO2) HIGH for 2 seconds on launch. That output is wired to an igniter. Treat any change to the launch path (`launch_task`, `launch_handler`, `MOSFET_GPIO`) as safety-critical.

## Build / flash / configure

Requires the ESP-IDF toolchain installed and exported (`IDF_PATH` set; on Windows use the ESP-IDF PowerShell/CMD environment, e.g. `export.ps1`). Target is **esp32c6**.

```bash
idf.py set-target esp32c6      # only needed once / after target change
idf.py menuconfig              # edit Kconfig options (see below)
idf.py build                   # compile -> build/firestarter.bin
idf.py -p COM4 flash monitor   # flash + open serial log (Ctrl-] to exit)
idf.py fullclean               # wipe build/ if CMake cache goes stale
```

There are **no automated tests** — verification is done on hardware via `idf.py monitor` (watch the `firestarter` log tag) and the served web UI.

### Configurable options (`main/Kconfig.projbuild`)

Set via `idf.py menuconfig`, not by editing source:
- **Antenna control GPIOs** (XIAO ESP32-C6 FM8625H RF switch) — RF switch power (default GPIO3, active-low) + antenna select (default GPIO14: LOW = on-board, HIGH = external). Firmware defaults to the **external** antenna.
- **Wi-Fi SSID / Password** — SSID default `Firestarter`; the password has **no default** and must be set locally (see below). SoftAP, max 3 clients; WPA/WPA2-PSK when a password is set, otherwise the firmware falls back to an open network.

Hardware pins **hardcoded** in `main/firestarter.c`: `MOSFET_GPIO = GPIO2` (igniter), `BUZZER_GPIO = GPIO21` (LEDC PWM tone). Change these in source, not Kconfig.

### Wi-Fi password & sdkconfig (secrets)

The active `sdkconfig` is **git-ignored** because it stores the resolved Wi-Fi password. Shared, non-secret build settings (target, flash size, WebSocket support) live in the committed [sdkconfig.defaults](sdkconfig.defaults), which `idf.py` applies when regenerating `sdkconfig` on a fresh checkout. Set the password locally with `idf.py menuconfig` → *Firestarter Wi-Fi Configuration* (or via a git-ignored `sdkconfig.defaults.local`). Never commit a real password.

## Architecture

Effectively a single-file firmware: all logic lives in [main/firestarter.c](main/firestarter.c). The web UI is in [main/index.html](main/index.html) and is **embedded into the binary** at build time (`EMBED_TXTFILES "index.html"` in [main/CMakeLists.txt](main/CMakeLists.txt); `embed.txt` is unused leftover). The HTML is served from the `_binary_index_html_start/_end` symbols — editing `index.html` requires a rebuild to take effect.

Boot sequence (`app_main`): init NVS → netif → event loop → configure MOSFET GPIO (output, low) → set up LEDC timer/channel for the buzzer → `wifi_init_softap()` → `start_webserver()` → 5 s delay → `startup_melody()`.

`wifi_init_softap()` first calls `antenna_init()` — manual GPIO control of the XIAO's FM8625H RF switch (power GPIO3 low, select GPIO14 high = external antenna) — then brings up the AP and sets Wi-Fi country/TX power. The `esp_phy` antenna-diversity API is **not** used: its binary GPIO encoding doesn't match this board and leaves the RF switch unpowered. The `/ant/*` HTTP handlers switch antennas at runtime via the same `ant_gpio_apply()`.

### HTTP + WebSocket server (port 80)

`start_webserver()` registers these handlers:
- `GET /` — serves the embedded `index.html`
- `GET /launch` — spawns the `launch_task` FreeRTOS task; guarded by the `launch_in_progress` flag so only one launch runs at a time
- `GET /ws` — WebSocket endpoint (requires `CONFIG_HTTPD_WS_SUPPORT=y`, currently enabled)
- `GET /ant/ant0 | /ant/ant1 | /ant/auto | /ant/status | /ant/clients` — antenna control + AP client list (JSON). Note: `/ant/auto` currently maps to the external antenna.

### Launch + status flow

1. Browser opens `/`, JS connects to `/ws`, and `launchRocket()` fires `GET /launch`.
2. `launch_task` runs `countdown_beeps()`: a 9-step countdown driving the buzzer via `beep()` (LEDC frequency sweep) while broadcasting JSON status frames.
3. After the countdown it sets `MOSFET_GPIO` HIGH for 2 s, then LOW, then broadcasts a "launch done" status.
4. Status reaches the UI via `ws_broadcast_json()` → `send_ws_message()`, which pushes JSON (`countdown_start` / `countdown` / `launch` / `status`) to all clients in `ws_clients[]`. The browser's `handleMessage()` switches on `type` to update the countdown display and button state. The UI auto-reconnects with exponential backoff.

WebSocket handling is wrapped in `#ifdef CONFIG_HTTPD_WS_SUPPORT`; when disabled, `ws_broadcast_json()` compiles to a no-op so the launch logic still works without status push.

## Conventions

- New HTTP routes: write a `httpd_req_t*` handler, then register a `httpd_uri_t` inside `start_webserver()`. Build JSON responses with `snprintf` into a stack buffer + `httpd_resp_set_type(req, "application/json")` (see the `/ant/*` handlers for the pattern).
- Push state to the UI by emitting a JSON string through `send_ws_message()` and adding a matching `case` in `handleMessage()` in `index.html`.
- Keep Russian for user-facing strings and logs to match the existing code.
