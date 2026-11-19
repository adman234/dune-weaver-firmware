# dune-weaver-firmware

Firmware that turns a **single MKS-DLC32 (ESP32) board** into the whole brain of a
[Dune Weaver](https://github.com/tuanchris/dune-weaver) kinetic sand table — no
Raspberry Pi, no separate WLED controller. It is a fork of
[**FluidNC**](https://github.com/bdring/FluidNC) **v3.9.5** with the sand-table host's
features ported down into the firmware itself.

## What it does

A Dune Weaver normally splits its brain across a host (Raspberry Pi running the
ThetaRho / `.thr` logic, playlists, scheduler, LEDs) and a motion controller. This
firmware folds all of that onto the ESP32:

- **ThetaRho kinematics** and `.thr` pattern playback
- **Playlists** with shuffle, pauses, adaptive clear-between-patterns
- **WS2812 / SK6812 RGBW LED** effects driven directly from the board (no WLED)
- **Scheduler / quiet hours** ("Still Sands")
- **Selectable homing** — limit-switch (sensor) or blind crash-into-stop, plus a
  configurable theta zero offset
- A stateless **HTTP/JSON API** for clients to drive the table

## Headless by design

The table holds **no web UI**. It exposes an HTTP API and is driven by clients (a
phone/desktop/web app, Home Assistant, scripts). Motion is controlled over stateless
HTTP — `POST`/`GET` to `/command` and the `/sand_*` action routes — and status is
polled from `/sand_status` (~1 Hz). The WebUI WebSocket is disabled (it raced motion
on this hardware).

## Documentation

- [`COMMANDS.md`](COMMANDS.md) — copy-paste command / HTTP cheatsheet
- [`API.md`](API.md) — the stable HTTP contract and `$Sand/Status` JSON schema
- [`PORTING.md`](PORTING.md) — what was ported from the Dune Weaver host and how
- Upstream FluidNC config reference: <http://wiki.fluidnc.com>

## Flash from the browser

The [web flasher](flasher/) installs the firmware over USB from a desktop browser
with Web Serial (Chrome, Edge, or Firefox 151 or later), with no toolchain. It can
also:

- upload a table config (Pro, original, Mini Pro, Gold, DLC32 MAX), with rho
  steps, LED count and hostname you can edit;
- set WiFi and MQTT credentials over the same USB link (they are sent to the
  board only, never stored or put in a URL);
- download a backup of a table's settings and config.yaml, and restore one
  after flashing (passwords are never in backups).

`.github/workflows/pages.yml` builds both boards on every push to `main` and
publishes the flasher to GitHub Pages (on a fork: Settings > Pages > Source =
GitHub Actions). To try it locally, build both boards and run:

```bash
pio run -e sandtable && pio run -e sandtable -t buildfs
pio run -e sandtable_s3 && pio run -e sandtable_s3 -t buildfs
python3 flasher/make_site.py _site && python3 -m http.server -d _site 8000
```

Web Serial works on `http://localhost`, so open <http://localhost:8000>.

## Build & flash

Uses [PlatformIO](https://platformio.org/).

```bash
pio test -e tests                 # fast native unit tests (googletest + ASan/UBSan)
pio run  -e sandtable             # build the firmware
pio run  -e sandtable -t upload --upload-port /dev/cu.usbserial-XXXX   # flash over USB
```

The machine is described by a `config.yaml` on the board's littlefs (theta/rho axes,
homing, LEDs, etc.) — see the upstream FluidNC [config docs](http://wiki.fluidnc.com)
and the examples in this repo.

## Credits

Built on the work of others, and distributed under the same license (GPLv3):

- [**FluidNC**](https://github.com/bdring/FluidNC) by Bart Dring and contributors —
  the CNC firmware this is forked from.
- [**Grbl**](https://github.com/gnea/grbl) by Sungeun (Sonny) Jeon and Simen Svale
  Skogsrud — the motion-control foundation FluidNC itself builds on.
- The Wi-Fi / WebUI plumbing derives from [ESP3D-WebUI](https://github.com/luc-github/ESP3D-WEBUI).
- The [Dune Weaver](https://github.com/tuanchris/dune-weaver) project — the sand table
  this firmware is built to drive.

## License

GPLv3 — see [`LICENSE`](LICENSE). As a fork of FluidNC, all upstream copyrights and
the GPLv3 terms are preserved; see [`AUTHORS`](AUTHORS).
