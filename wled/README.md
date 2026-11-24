# ADS-B Blinker for WLED

← [Back to the project overview](../README.md) · See also the [Tasmota script](../tasmota/README.md)

A [WLED usermod](https://kno.wled.ge/advanced/custom-features/) that reacts whenever a plane lands at or departs from an airport you choose, using live ADS-B data from [adsb.fi](https://adsb.fi). For each event it can either:

- **flash** a color over whatever effect is running (green for departures, blue for landings by default), or
- **apply a WLED preset** (one for departures, one for landings), so you can design any effect in the WLED UI.

Your LED hardware (pins, strip type, LED count, color order, power limits) is configured in WLED's own LED settings, so a custom PCB needs no code changes.

**ESP32 family only** (ESP32, -S2, -S3, -C3). HTTPS polling needs more memory than an ESP8266 has.

## How it works

- At startup the usermod creates a background FreeRTOS task. All network work happens there, so the TLS handshake never stalls effects or the web UI.
- HTTPS goes through [ESP_SSLClient](https://github.com/mobizt/ESP_SSLClient), which bundles BearSSL and wraps a plain `WiFiClient`. WLED's ESP32 builds use a trimmed Arduino core with no `WiFiClientSecure` and no mbedTLS SSL layer, so the usermod brings its own TLS. Requests are plain HTTP/1.0 GETs (so the response is never chunked), and ArduinoJson parses the body straight off the TLS stream. Certificates aren't verified (`setInsecure()`); the data is public and nothing secret is sent.
- When the airport changes, the task looks up its coordinates from aviationweather.gov. It stores them in `cfg.json` (`resolvedIcao`, `lat`, `lon`), so reboots don't need a new lookup.
- Every `pollSec` seconds it fetches `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<radiusNm>`. The response is parsed with an ArduinoJson filter that keeps only `hex`, `flight`, `t`, `alt_baro` and `gs`.
- Each aircraft is tracked by ICAO hex code (the [detection rules](../README.md#how-detection-works-both-versions) are shared with the Tasmota version). A ground↔airborne transition puts an event on a FreeRTOS queue.
- The main WLED `loop()` takes events off the queue and either:
  - starts a flash sequence, drawn in `handleOverlayDraw()` on top of the current effect, or
  - calls `applyPreset()`.

## Build

The usermod is a PlatformIO library (the manifest is [`library.json`](../library.json) at the repo root, pointing at `wled/`). WLED pulls it in through `custom_usermods`. It builds against WLED `main` (`17.0.0-devV5`, commit `3f065cb`) with the `esp32dev` and `esp32c3dev` environments. It has been run on an Adafruit QT Py ESP32-C3 (`esp32c3dev`), with live traffic and both output modes.

1. Clone WLED and install [PlatformIO](https://platformio.org/). WLED's ESP32 platform needs Python 3.10 or newer.
   ```sh
   git clone https://github.com/wled/WLED.git && cd WLED
   ```
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
3. Open the **Info** panel (the ⓘ button in the main UI). The **ADS-B Blinker** line shows the status, for example `KMSP: 14 aircraft | dep 2 arr 3 | last arr SCX612 (B738)`, or an error such as `unknown airport ZZZZ`.

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

### Testing without traffic

Fire a fake event through the JSON API:

```sh
curl -X POST http://<wled-ip>/json/state -H 'Content-Type: application/json' -d '{"adsb":{"test":"dep"}}'
curl -X POST http://<wled-ip>/json/state -H 'Content-Type: application/json' -d '{"adsb":{"test":"arr"}}'
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
| `pollAdsb()` | Fetches adsb.fi, runs the detection state machine, and queues an `Event` for each transition. |
| `loop()` | Takes events off the queue (`onEvent()`), advances the flash sequence (`runFlash()`), ends an expired preset (`endPreset()`), and saves the cache after a lookup. |
| `onEvent()` | The output decision: counts the event, then starts a preset or queues a flash. |
| `handleOverlayDraw()` | Paints the flash color over the first `flashLeds` LEDs on the frames where the flash is on. |
| `addToJsonInfo()` | The Info panel line. |
| `readFromJsonState()` | The `{"adsb":{"test":...}}` hook. |

### Threading rules

- Settings and `status` are shared between the WLED main loop and the polling task, and are guarded by `mtx`. The task never reads `String` settings directly; it copies a `Snapshot` under the mutex.
- Only the main loop touches the strip, presets, and the counters. The task communicates **only** through the `events` queue and the `lookupDone` flag. Keep it that way: calling WLED/strip functions from the task is not thread-safe.
- `loop()` must never block (WLED rule). Anything slow belongs in the task.

### Ideas for adapting

- **Different events:** `pollAdsb()` has the whole aircraft object. Add fields to the ArduinoJson `filter` (e.g. `category`, `baro_rate`, `dst`), then add new `Kind`s, for example heavies (`category == "A5"`), helicopters (`A7`), or anything below 1000 ft.
- **Different output:** change `onEvent()`. For example, set a segment's color with `strip.getSegment(id)`, publish over MQTT (`mqtt->publish(...)` inside `#ifndef WLED_DISABLE_MQTT`), or drive a GPIO through WLED's pin manager.
- **Flash pattern:** `runFlash()` steps through on/off phases of `ADSB_FLASH_ON_MS` each, with `ADSB_FLASH_GAP_MS` between events. Replace `fxOn` with a brightness ramp for a pulse instead of a blink.
- **Memory:** the filtered document is `ADSB_DOC_SIZE` = 24 KB, enough for roughly 200 aircraft. A large radius around a busy area can exceed that; the status then shows `too many aircraft, lower radius`.
- **Debug output:** build with `-D WLED_DEBUG` to get `ADS-B: dep ...` lines on the serial console.

## Data and terms

- Aircraft data: [adsb.fi](https://adsb.fi) open data API. Personal, non-commercial use only, and adsb.fi asks that you credit them with a link.
- Airport coordinates: [aviationweather.gov](https://aviationweather.gov/data/api/) (NOAA).
