# ADS-B Blinker for WLED

← [Back to the project overview](../README.md) · See also the [Tasmota script](../tasmota/README.md)

A [WLED usermod](https://kno.wled.ge/advanced/custom-features/) that reacts whenever a plane lands at or departs from an airport you choose, using live ADS-B data from [adsb.fi](https://adsb.fi). For each event it can either:

- **flash** a color over whatever effect is running (green for departures, blue for landings by default), or
- **apply a WLED preset** (one for departures, one for landings), so you can design any effect in the WLED UI.

It can also keep a **watch list** of callsigns and tail numbers, and apply a preset whenever one of them shows up nearby (see [Watch list](#watch-list)).

Your LED hardware (pins, strip type, LED count, color order, power limits) is configured in WLED's own LED settings, so a custom PCB needs no code changes.

**ESP32 family only** (ESP32, -S2, -S3, -C3). HTTPS polling needs more memory than an ESP8266 has.

## How it works

- At startup the usermod creates a background FreeRTOS task. All network work happens there, so the TLS handshake never stalls effects or the web UI.
- HTTPS goes through [ESP_SSLClient](https://github.com/mobizt/ESP_SSLClient), which bundles BearSSL and wraps a plain `WiFiClient`. WLED's ESP32 builds use a trimmed Arduino core with no `WiFiClientSecure` and no mbedTLS SSL layer, so the usermod brings its own TLS. Requests are plain HTTP/1.0 GETs (so the response is never chunked), and ArduinoJson parses the body straight off the TLS stream. Certificates aren't verified (`setInsecure()`); the data is public and nothing secret is sent.
- When the airport changes, the task looks up its coordinates from aviationweather.gov. It stores them in `/adsb.json`, so reboots don't need a new lookup.
- Every `pollSec` seconds it fetches `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>`, where `<nm>` is the airport radius, or the watch radius if a watch list is set and its radius is larger. The response is parsed with an ArduinoJson filter that keeps only `hex`, `flight`, `r`, `t`, `alt_baro`, `gs` and `dst`.
- Each aircraft within the airport radius (by adsb.fi's `dst`) is tracked by ICAO hex code (the [detection rules](../README.md#how-detection-works-both-versions) are shared with the Tasmota version). A ground↔airborne transition puts an event on a FreeRTOS queue. Watch-list matches anywhere in the polled area queue a watch event.
- The main WLED `loop()` takes events off the queue and either:
  - starts a flash sequence, drawn in `handleOverlayDraw()` on top of the current effect, or
  - calls `applyPreset()`.

## Prebuilt firmware

Every push to `main` builds WLED **v16.0.1** with this usermod (and WLED's default AudioReactive usermod) and publishes it to the [**latest** release](https://github.com/adamgoose/tasmota-adsb-blinker/releases/tag/latest):

| File | For |
| --- | --- |
| `WLED_16.0.1_ESP32_adsb-blinker.bin` | Classic ESP32 (4 MB flash), WLED's `esp32dev` |
| `WLED_16.0.1_ESP32-C3_adsb-blinker.bin` | ESP32-C3 (4 MB flash), WLED's `esp32c3dev` |

Upload the file matching your chip through **Config → Security & Updates → Manual OTA update**. WLED refuses a file whose release name (`ESP32` / `ESP32-C3`) doesn't match the running firmware. `SHA256SUMS` is attached for checking downloads.

The workflow is [`.github/workflows/firmware.yml`](../.github/workflows/firmware.yml). To build against a newer WLED, change `WLED_VERSION` there. To add a board, add a row to its `matrix` (a WLED environment name plus its release name). It can also be run by hand from the repo's **Actions** tab.

## Build it yourself

The usermod is a PlatformIO library (the manifest is [`library.json`](../library.json) at the repo root, pointing at `wled/`). WLED pulls it in through `custom_usermods`, which needs WLED 16 or newer.

| WLED | `esp32dev` (classic ESP32) | `esp32c3dev` |
| --- | --- | --- |
| **v16.0.1** (stable, recommended) | Builds clean (locally and in CI) | Built in CI (see the toolchain note below) |
| `main` (`17.0.0-devV5`, `3f065cb`) | Builds clean; ran on a classic ESP32 | Builds clean; running on a QT Py ESP32-C3 with live traffic, both output modes and the watch list |

Toolchain note: on WLED 16 the ESP32-C3 environment uses GCC 8.4.0, which is only published for Intel. On an Apple Silicon Mac you need Rosetta (`softwareupdate --install-rosetta`) or a Linux machine to build it, or just use the CI build above. The classic ESP32 environment builds natively.

1. Clone the WLED release and install [PlatformIO](https://platformio.org/). WLED's ESP32 platform needs Python 3.10 or newer.
   ```sh
   git clone --branch v16.0.1 --depth 1 https://github.com/wled/WLED.git && cd WLED
   ```
   Use a release tag rather than `main`: `main` is WLED's development branch and shows a "development build" banner.
2. Create `platformio_override.ini` in the WLED folder. Extend the environment for your board, add the usermod, and remove `-DBEARSSL_SSL_BASIC`:
   ```ini
   [platformio]
   default_envs = my_board

   [env:my_board]
   extends = env:esp32dev            ; or env:esp32c3dev, env:esp32s3dev_8MB_opi, ...
   custom_usermods = ${env:esp32dev.custom_usermods}
     https://github.com/adamgoose/tasmota-adsb-blinker.git
   build_unflags = ${env:esp32dev.build_unflags} -DBEARSSL_SSL_BASIC
   ```
   Replace `esp32dev` in both `${...}` references with the env you extend.

   Why `build_unflags`: WLED adds `-DBEARSSL_SSL_BASIC` to every build to save flash on ESP8266. On ESP32 nothing else in WLED uses BearSSL, but that flag would restrict this usermod's TLS library to ciphers without elliptic curves, and both APIs require elliptic-curve (ECDHE) ciphers. If you forget it, the build stops with an `#error` explaining this.
   To work on a local checkout instead, name it explicitly (WLED can't infer the library name from a `symlink://` path, and without it the build fails with `wled.h: No such file or directory`):
   ```ini
     tasmota-adsb-blinker = symlink:///absolute/path/to/tasmota-adsb-blinker
   ```
3. Build and flash:
   ```sh
   pio run -e my_board -t upload
   ```
   Or build with `pio run -e my_board` and upload `build_output/release/*.bin` from WLED's **Settings → Security & Updates → Manual OTA update**.

## Setup

1. Configure your LEDs as usual in **Config → LED Preferences**.
2. Open **Config → Usermods**, find **ADS-B Blinker**, tick **Enabled**, set the **ICAO code**, choose an action, and **Save**.
3. Open the **Info** panel (the ⓘ button in the main UI). The **ADS-B Blinker** line shows the status, for example `KMSP: 14 aircraft · dep 2 arr 3`, or an error such as `unknown airport ZZZZ`.

### Flight log

- **Info panel:** the **Recent flights** row shows the last 3 events (`↓ SWA4833 B738 · 2m ago`, or `★` for a watch-list sighting) and a **Full log** button.
- **`http://<wled-ip>/adsb`:** a phone-friendly table of the last 50 landings, departures and watch-list sightings (time, event, callsign, aircraft type). It refreshes every 10 s. Times are worked out in your browser from each event's age, so they show in your local time even if WLED's NTP is off. If you change airports, earlier entries are tagged with the old airport's code.
- **`http://<wled-ip>/adsb/log`:** the same data as JSON, newest first:
  ```json
  {"icao":"KDEN","status":"KDEN: 14 aircraft","deps":3,"arrs":2,"spots":1,
   "events":[{"k":"arr","cs":"SWA4833","t":"B738","ap":"KDEN","ago":95}, ...]}
  ```
  `k` is `dep`, `arr` or `watch`.
- The log is kept in RAM, so a reboot or firmware update clears it, along with the counts. Test events (see below) show up in the log as `TEST`.

WLED's four bottom tabs (Colors, Effects, Segments, Presets) are hard-coded in its UI, and usermods can only add to the Info panel. That's why the log is a separate page rather than a fifth tab.

### Settings

The settings page groups everything into sections. Only the section for the chosen action (Flash or Preset) is shown.

| Section | Setting | Default | Meaning |
| --- | --- | --- | --- |
| | Enabled | off | Turn the usermod on. |
| Airport | ICAO code | `KMSP` | Any airport known to aviationweather.gov, including non-US ones. |
| | Radius | `3` NM | 1–250. Keep it small: big enough for the runways and short final, small enough to leave out nearby airports. |
| Action | When a plane lands or departs | Flash the LEDs | **Flash the LEDs** or **Apply a preset**. |
| Flash | Departure color, Landing color | green, blue | Color pickers. |
| | Flashes | `3` | Flashes per event (1–20). |
| | LEDs | `0` | Number of LEDs to flash, counted from the start of the strip. `0` = all. |
| Preset | Departure preset, Landing preset | None | Dropdowns listing your saved presets. |
| | Show for | `10` s | How long the preset stays on before your previous look comes back. `0` = leave it on. |
| Watch | Callsigns / tail numbers | (empty) | See [Watch list](#watch-list). Empty = off. |
| | Default preset | None | Preset for watch hits whose entry doesn't name its own. |
| | Show for | `30` s | As above, for watch hits. `0` = leave it on. |
| | Watch within | `10` NM | How far from the airport to look for watched aircraft (1–100). |
| Detection | Check every | `10` s | Seconds between polls (minimum 2). adsb.fi allows 1 request per second. |
| | Airborne above | `60` kt | Ground speed that counts as airborne. |

In `cfg.json` these are stored as nested objects under `"ADS-B Blinker"` (`Airport.icao`, `Preset.seconds`, and so on). Settings from the first version's flat layout are migrated automatically. The airport's looked-up coordinates live in their own file, `/adsb.json`. Delete it to force a new lookup.

### Flash mode notes

- Flashes are drawn on top of the current effect, scaled by WLED's master brightness. With brightness very low, or the LEDs switched **off**, flashes are dim or invisible. If you want events to show even when the lights are off, use preset mode.
- Blue looks much dimmer than green at the same value. If landings are hard to see, pick a lighter blue.
- If several events arrive together, they play one after another.

### Preset mode

- Build the look you want (a chase, a pulse, a color) and save it as a preset, then pick it in the Departure or Landing dropdown.
- **Show for** sets how long the preset stays on. When the time is up, the usermod restores exactly what was showing before: power, brightness, effects, colors and segments. If a **playlist** was running, it is restarted instead. If the LEDs were off, they switch back off.
- If another event arrives while a preset is showing, its preset takes over and the timer restarts. The look restored at the end is still the one from before the *first* event.
- Changes you make by hand while an event preset is showing are overwritten when the time runs out.
- Set **Show for** to `0` to leave the event preset on indefinitely.

### Watch list

Apply a preset whenever a particular aircraft shows up, such as a friend's plane, an airline, or a callsign you follow.

- **Entries:** callsigns (`SWA2833`) or tail numbers (`N8518R`), separated by commas, spaces or new lines. Each entry is checked against both the aircraft's callsign and its registration as reported by adsb.fi, ignoring case.
- **Wildcards:** `*` matches any run of characters and `?` matches one. `DAL*` catches every Delta flight, and `N12?AB` matches N123AB, N124AB, and so on.
- **Per-entry presets:** add `=ID` to give an entry its own preset, for example `N123AB=4, UAL*=5, SWA2833`. Entries without one use the **Default preset**.
- **When it fires:** once, the first time a matching aircraft appears within **Watch within** NM of the airport. It can fire again only after the aircraft has been out of range for 3 minutes. When the usermod starts, aircraft already in range count as appearing.
- **Presets only:** watch hits always use presets (with **Show for** and the same restore as above), whichever action you chose for landings and departures. With no preset set, a hit is still logged but nothing changes on the LEDs.
- **One request:** the usermod polls the larger of the two radii in a single request and uses each aircraft's distance (`dst`) to keep landing and departure detection inside the airport radius. A large watch radius means bigger responses. Watch within is capped at 100 NM, and the status line shows `too many aircraft, lower radius` if a response doesn't fit.
- With a watch list set, the status line shows both circles, for example `KDEN: 10 nearby, 13 within 30 NM`.

### Testing without traffic

Fire a fake event through the JSON API:

```sh
curl -X POST http://<wled-ip>/json/state -H 'Content-Type: application/json' -d '{"adsb":{"test":"dep"}}'
curl -X POST http://<wled-ip>/json/state -H 'Content-Type: application/json' -d '{"adsb":{"test":"arr"}}'
curl -X POST http://<wled-ip>/json/state -H 'Content-Type: application/json' -d '{"adsb":{"test":"watch"}}'   # uses the default watch preset
```

## Development

### Code map (`adsb_blinker.cpp`)

| Part | Role |
| --- | --- |
| `readFromConfig()` / `addToConfig()` | Settings, stored as nested objects (each becomes a titled section in the UI). A changed airport or radius bumps `gen`, which resets tracking and triggers a new lookup. Also migrates the old flat layout. |
| `appendConfigData()` | JavaScript for the settings page. `L()` relabels a field and adds units, `G()` wraps a section in a `<div>` so it can be shown or hidden, and it also adds the color pickers, the action dropdown, and preset dropdowns filled from `/presets.json`. WLED's buffer for this script is about 3 KB, so keep it compact. |
| `startPreset()` / `endPreset()` | Preset mode with timed restore. On the first event the current look is captured with `serializeState()` and **stored as JSON text**: WLED writes segment colors as pre-rendered raw JSON, which only becomes a real array once it's parsed again. A running playlist is remembered by ID instead. `endPreset()` re-parses the text and calls `deserializeState()`. |
| `loadCache()` / `saveCache()` | The `/adsb.json` coordinate cache, read and written whole with `WLED_FS`. |
| `adsbGetJson()` | One HTTPS GET with ESP_SSLClient: connect, send the request, skip headers, parse the body through a filter. The TLS receive buffer (`ADSB_TLS_RX_BUF`) holds one full 16 KB TLS record and is freed after each request. |
| `setup()` | Creates the mutex, the event queue and the polling task (`ADSB_TASK_STACK` = 12 KB; the BearSSL handshake needs it). |
| `taskLoop()` | Runs in the background task: waits for the network, looks up the airport if needed, polls, sleeps. |
| `lookupAirport()` | aviationweather.gov lookup. Sets `lookupDone`, so `loop()` writes `/adsb.json`. |
| `pollAdsb()` | Fetches adsb.fi over the larger of the two radii, queues a `WATCH` event the first time a watch-list aircraft appears (tracked in `watched`), then runs the landing/departure state machine for aircraft within the airport radius. |
| `parseWatch()` / `globMatch()` | Turn the watch list text into `pattern[=preset]` rules, and match them case-insensitively with `*` and `?` wildcards. The list is re-parsed on every poll, so edits apply right away. |
| `loop()` | Takes events off the queue (`onEvent()`), advances the flash sequence (`runFlash()`), ends an expired preset (`endPreset()`), and saves the cache after a lookup. |
| `onEvent()` | The output decision: logs the event, then starts a preset (always, for watch hits) or queues a flash. |
| `handleOverlayDraw()` | Paints the flash color over the first `flashLeds` LEDs on the frames where the flash is on. |
| `logEvent()` / `logAt()` | A 50-entry ring buffer of events (`ADSB_LOG_SIZE`), plus the counts. Written from `loop()`, read by the web server, guarded by `mtx`. Callsigns and types are reduced to `[A-Za-z0-9-]` by `cleanCopy()` before they're stored, because they end up in HTML. |
| `sendLog()` / `ADSB_PAGE` | `GET /adsb/log` (JSON) and `GET /adsb` (the page, stored in flash). Both are registered in `setup()`; `/adsb/log` comes first because the `/adsb` route also matches anything under `/adsb/`. |
| `addToJsonInfo()` | The two Info panel rows. Their values are rendered as HTML, which is how the **Full log** button gets there. |
| `readFromJsonState()` | The `{"adsb":{"test":...}}` hook. |

### Threading rules

- Settings and `status` are shared between the WLED main loop and the polling task, and are guarded by `mtx`. The task never reads `String` settings directly; it copies a `Snapshot` under the mutex.
- Only the main loop touches the strip, presets, and the counters. The task communicates **only** through the `events` queue and the `lookupDone` flag. Keep it that way: calling WLED/strip functions from the task is not thread-safe.
- `loop()` must never block (WLED rule). Anything slow belongs in the task.

### Ideas for adapting

- **Different events:** `pollAdsb()` has the whole aircraft object. Add fields to the ArduinoJson `filter` (e.g. `category`, `baro_rate`, `dst`), then add new `Kind`s, for example heavies (`category == "A5"`), helicopters (`A7`), or anything below 1000 ft.
- **Different output:** change `onEvent()`. For example, set a segment's color with `strip.getSegment(id)`, publish over MQTT (`mqtt->publish(...)` inside `#ifndef WLED_DISABLE_MQTT`), or drive a GPIO through WLED's pin manager.
- **Flash pattern:** `runFlash()` steps through on/off phases of `ADSB_FLASH_ON_MS` each, with `ADSB_FLASH_GAP_MS` between events. Replace `fxOn` with a brightness ramp for a pulse instead of a blink.
- **Memory:** the filtered document is `ADSB_DOC_SIZE` = 40 KB, about 150 bytes per aircraft, so roughly 250 aircraft. It's only allocated during a poll. A large watch radius around a busy area can exceed that; the status then shows `too many aircraft, lower radius`.
- **Debug output:** build with `-D WLED_DEBUG` to get `ADS-B: dep ...` lines on the serial console.
- **Longer or persistent log:** raise `ADSB_LOG_SIZE` (each entry is about 30 bytes). To keep the log across reboots, write it to a file the way `saveCache()` does. Avoid writing on every event, though, since that wears the flash.

## Data and terms

- Aircraft data: [adsb.fi](https://adsb.fi) open data API. Personal, non-commercial use only, and adsb.fi asks that you credit them with a link.
- Airport coordinates: [aviationweather.gov](https://aviationweather.gov/data/api/) (NOAA).
