# ADS-B Blinker for Tasmota

← [Back to the project overview](../README.md) · See also the [WLED usermod](../wled/README.md)

A [Berry](https://tasmota.github.io/docs/Berry/) script for [Tasmota](https://tasmota.github.io/) that flashes an LED whenever a plane lands at or departs from an airport you choose. It watches live ADS-B data from [adsb.fi](https://adsb.fi) and runs entirely on the ESP32. No server, bridge, or MQTT broker is needed.

- **Departure:** 3 green flashes
- **Landing:** 3 blue flashes

It was built and tested on an [Adafruit QT Py ESP32-C3](https://learn.adafruit.com/adafruit-qt-py-esp32-c3-wifi-dev-board) (one NeoPixel on GPIO2) running Tasmota 16.0.0.7 (`tasmota32`). Other ESP32 boards work with small changes; see [Development](#development).

## How it works

1. **Airport lookup.** `set_airport("KMSP")` fetches the airport's coordinates from the [aviationweather.gov airport API](https://aviationweather.gov/data/api/). The ICAO code, coordinates and radius are saved with Tasmota's `persist` module, so a reboot needs no new lookup.
2. **Polling.** Every 10 seconds the script requests `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>`, which lists every aircraft within the radius (3 NM by default).
3. **Detection.** The script remembers whether each aircraft (keyed by its ICAO hex code) was last seen on the ground or in the air:
   - `alt_baro: "ground"` means on the ground.
   - An altitude plus ground speed of at least 60 kt means airborne.
   - Anything else (slow but not reported as ground, or no altitude) is ambiguous and keeps the previous state, so ADS-B flicker while taxiing doesn't produce fake events.
   - **ground → airborne** is a departure. **airborne → ground** is a landing.
   - Aircraft not seen for 3 minutes are forgotten.
4. **Flashing.** Each event queues a 3-flash sequence driven by `tasmota.set_timer`, so the flashes never block Tasmota. If two events arrive together, they play one after the other.

Only transitions count. An aircraft that is already parked when the script starts, or one that first appears already airborne, won't trigger anything until its state changes.

### Things worth knowing

- **Each poll blocks for about 0.7 s**, mostly the TLS handshake. Berry's `webclient.GET()` is synchronous in standard builds, so Tasmota (web UI, MQTT, rules) pauses briefly every 10 s. That has been fine in practice.
- **Why polling instead of a stream?** Berry in standard Tasmota has no WebSocket client, and its only TLS-capable client (`webclient`) returns complete responses only. A never-ending HTTPS or WSS stream can't be consumed on the device without a plain-TCP relay somewhere.
- **The JSON trailer gets trimmed.** Berry integers are 32-bit, and `json.load()` returns `nil` if *any* number is larger. adsb.fi responses end with millisecond timestamps (`"now"`, `"ctime"`), so the script cuts the body at `,"msg"` and parses only `{"ac":[...]}`.
- **Rate limits.** The adsb.fi public endpoints allow 1 request per second. Polling every 10 s stays well under that.
- **Ground coverage matters.** Detection relies on aircraft reporting `alt_baro: "ground"`. Busy airports usually have good surface coverage on adsb.fi. If an airport doesn't, landings may be missed; see [Tuning](#tuning).

## Install

Requirements: an ESP32 running a `tasmota32` build with Berry, and an LED strip configured in Tasmota or a NeoPixel on GPIO2 (see [Development](#development) for other LEDs).

1. In Tasmota, go to **Tools → Manage File system** and upload `airport_watch.be`.
2. Add this line to `autoexec.be` so the script starts on boot. Create the file if it doesn't exist; **Manage File system** can create and edit files.
   ```berry
   load("/airport_watch.be")
   ```
3. Open **Tools → Berry Scripting console** and run:
   ```berry
   load("/airport_watch.be")
   set_airport("KMSP")
   ```
   The airport is saved, so this only needs doing once.

To test the LED without waiting for traffic:

```berry
airport_watch.flash(airport_watch.DEP_COLOR)   # green
airport_watch.flash(airport_watch.ARR_COLOR)   # blue
```

## Usage

### Berry console

| Command | What it does |
| --- | --- |
| `set_airport("KDEN")` | Look up, save and watch an airport (3 NM radius). Returns `true` or `false`. |
| `set_airport("EFHK", 5)` | Same, with a custom radius in NM (1–250). |
| `airport_watch.stop()` | Stop polling and turn the LED off. |
| `load("/airport_watch.be")` | Start again (replaces any running instance). |
| `airport_watch.debug = true` | Log a summary line after every poll. |
| `airport_watch.deps`, `airport_watch.arrs` | Counts since the last start or airport change. |

### Tasmota console, MQTT and rules

The script registers an `Airport` command, so it works anywhere Tasmota commands do (the Console, `cmnd/<topic>/Airport` over MQTT, rules):

```
Airport          -> {"Airport":"KMSP","Departures":3,"Landings":5}
Airport KDEN     -> looks up and switches to Denver
```

Event lines (`AIRPORT: KMSP LANDED SCX612 (B738) aba8ff`) and errors go to the Tasmota log. They appear in **Tools → Console**, not in the Berry console.

Lower-case codes are fine. An unknown code returns `false` (the Tasmota command responds `Failed`) and leaves the current airport unchanged. Any ICAO code known to aviationweather.gov works, including non-US airports.

## Tuning

These constants are at the top of the `AirportWatch` class:

| Constant | Default | Meaning |
| --- | --- | --- |
| `DEFAULT_NM` | `3` | Radius when `set_airport` isn't given one. Keep it small: big enough to cover the runways and short final, small enough not to include other airports. |
| `POLL_S` | `10` | Seconds between polls. Each poll blocks about 0.7 s, and adsb.fi allows at most 1 request per second. |
| `AIR_KT` | `60` | Ground speed at which an aircraft counts as airborne. |
| `FORGET_MS` | `180000` | Forget aircraft not seen for this long. |
| `DEP_COLOR`, `ARR_COLOR` | `0x00FF00`, `0x0000FF` | Flash colors (`0xRRGGBB`). |
| `DEP_BRI`, `ARR_BRI` | `120`, `220` | Brightness (0–255) for each event type. Blue looks much dimmer than green at the same level, so landings get more. Brightness is gamma-corrected, so 120 gives a channel value of about 32. |

The flash pattern (3 flashes of 150 ms with a 400 ms gap between events) is in `fx_next()`.

## Development

### Code map

Everything lives in one class, `AirportWatch`. A single global instance is created at the end of the file:

| Method | Role |
| --- | --- |
| `init()` | Picks the LED, sets brightness, restores the saved airport, registers as a Tasmota driver. |
| `set_airport(code, nm)` | Validates the code, does the lookup, saves the result to `persist`, calls `use()`. |
| `use(icao, lat, lon, nm)` | Builds the adsb.fi URL and resets tracking state. |
| `every_second()` | Driver callback. Calls `poll()` every `POLL_S` seconds. |
| `poll()` | Blocking HTTPS GET, trims the JSON trailer, parses, calls `process()`. |
| `process(acs)` | The detection state machine. Calls `flash(color)` on each event. |
| `flash(color)` / `fx_next()` | The output side: a queue plus a timer-driven flash sequence. |
| `stop()` | Removes the driver and timer, clears the LED. |

After the class, the file stops any previous instance (so `load()` can be re-run), creates `global.airport_watch`, defines the global `set_airport()` helper, and registers the `Airport` command.

### Adapting to a different board or LED

Detection and output are separate. To support different hardware, change only the LED setup in `init()` and the output in `flash()` / `fx_next()`. Leave `process()` alone.

**Addressable LEDs (WS2812/SK6812) on another pin.** If the strip is configured in Tasmota (**Configuration → Module**, set the pin to `WS2812`), `init()` already picks it up with `Leds()`, which drives Tasmota's own strip. Otherwise it falls back to `Leds(1, 2)`. Change that `2` to your pin, and the `1` to your pixel count if you want more than pixel 0. Note that `Leds()` uses Tasmota's `Pixels` setting (default 30); the script only writes pixel 0.

To light every pixel instead of just the first, replace the `set_pixel_color(0, ...)` call in `fx_next()` with:

```berry
self.px.clear_to(self.fx_step % 2 == 0 ? self.fx_color : 0)
```

**Plain (single-color) LEDs on GPIOs.** Replace the `Leds` setup with pin numbers and drive them directly. For example, a green LED for departures and a red one for landings:

```berry
# in init(), instead of the Leds block:
self.dep_pin = 4
self.arr_pin = 5
for p: [self.dep_pin, self.arr_pin]
  gpio.pin_mode(p, gpio.OUTPUT)
  gpio.digital_write(p, 0)
end

# in fx_next(), instead of set_pixel_color/show:
import gpio
var pin = self.fx_color == self.ARR_COLOR ? self.arr_pin : self.dep_pin
gpio.digital_write(pin, self.fx_step % 2 == 0 ? 1 : 0)
```

Then remove the remaining `self.px` calls (`set_bri` in `fx_next()`, `clear()` in `stop()`). For active-low LEDs, swap the 1 and 0. Don't assign these pins to anything in Tasmota's Module configuration, or Tasmota will fight the script for them.

**Relays, buzzers or other Tasmota outputs.** Swap the LED writes for `tasmota.set_power(idx, on)` or `tasmota.cmd("...")`. Keep each callback short, because Berry blocks Tasmota while it runs.

**Different events or colors.** `process()` decides what counts as an event. Each aircraft dict has the adsb.fi/readsb fields: `hex`, `flight`, `t` (type), `alt_baro`, `gs`, `baro_rate`, `lat`, `lon`, `category`, `squawk`, `dst` (distance in NM), and so on. You could, for example, use a different color for heavies (`category == "A5"`) or ignore helicopters (`category == "A7"`).

### Firmware constraints to keep in mind

Learned the hard way on Tasmota 16.0.0.7:

- **`webclient` is blocking.** Some newer builds expose `async_get_start()` / `async_state()`, but unless the firmware is compiled with `USE_BERRY_WEBCLIENT_ASYNC` they're stubs that raise `type_error`. Check with `webclient().async_state()` before relying on them.
- **No TLS on `tcpclient` / `tcpclientasync`**, and no WebSocket client. Streaming endpoints need a plain-TCP relay.
- **32-bit integers.** `json.load` silently returns `nil` on numbers over 2³¹. Trim or rewrite them before parsing.
- **Don't block for long.** Tasmota's guidance is to stay under about 50 ms per callback. The 0.7 s poll is the exception. Use `tasmota.set_timer` for anything time-based, never `tasmota.delay()` loops.
- **Memory.** A 3 NM response at a busy airport is about 7 KB and parses in about 30 ms on an ESP32-C3, with about 140 KB of heap free. A much larger radius means bigger responses: watch `tasmota.memory()`.

### Workflow tips

- **Run code from the Berry console** (**Tools → Berry Scripting console**). Multi-line code runs when you press Enter twice.
- **Iterating on the script:** edit locally, serve the folder from your computer, and pull the file onto the device. That's quicker than re-uploading through the web UI.
  ```sh
  python3 -m http.server 8765        # on your computer, in the repo folder
  ```
  ```berry
  tasmota.urlfetch("http://<your-computer-ip>:8765/airport_watch.be", "/airport_watch.be")
  load("/airport_watch.be")
  ```
- **Inspect live state:**
  ```berry
  airport_watch.debug = true
  for k: airport_watch.state.keys() print(k, airport_watch.state[k]) end
  ```
- **Compare against the source** to check detection:
  ```sh
  curl -s https://opendata.adsb.fi/api/v3/lat/44.8820/lon/-93.2218/dist/3 | jq '.ac[] | [.flight, .alt_baro, .gs]'
  ```
- On some builds, opening console URLs like `/bc` directly in the address bar returns an empty page. Navigate there through **Tools** instead.

## Data and terms

- Aircraft data: [adsb.fi](https://adsb.fi) open data API. Personal, non-commercial use only, and adsb.fi asks that you credit them with a link. Please consider [feeding](https://adsb.fi) them a receiver.
- Airport coordinates: [aviationweather.gov](https://aviationweather.gov/data/api/) (NOAA).
