# tasmota-adsb-blinker

Light up an LED whenever a plane lands at or departs from an airport you choose. It runs on an ESP32 and uses live ADS-B data from [adsb.fi](https://adsb.fi). No server or bridge is needed.

- **Departure:** green flashes
- **Landing:** blue flashes

There are two versions, which share the same detection logic:

| | [Tasmota script](tasmota/README.md) | [WLED usermod](wled/README.md) |
| --- | --- | --- |
| What it is | A Berry script (`tasmota/airport_watch.be`) | A C++ usermod (`wled/adsb_blinker.cpp`) compiled into WLED |
| Install | Upload one file to a running Tasmota device | Build custom WLED firmware with PlatformIO |
| LED hardware | Pixel 0 of Tasmota's WS2812 strip; other LEDs need code edits | Anything WLED supports, configured in WLED's LED settings, no code changes |
| Output | Fixed flashes | Flash overlay on top of the running effect, or apply any WLED preset |
| Settings | `set_airport("KMSP")` / `Airport KMSP` command | Usermod settings page in the WLED web UI |
| Polling | Blocks Tasmota for about 0.7 s every 10 s | Background task; effects never stall |
| Iterating | Edit and `load()` on the device | Compile and flash |

**Which one?**
- **Tasmota:** good if the device already runs Tasmota, or if you want to tweak behavior live without reflashing.
- **WLED:** good for custom boards and LED setups (pins, strip types and effects are all set in the WLED UI), or if you want proper effects or presets for each event.

## How detection works (both versions)

1. Look up the airport's coordinates once from the [aviationweather.gov airport API](https://aviationweather.gov/data/api/) and save them.
2. Every 10 s, fetch the aircraft within a small radius (3 NM by default) from `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>`.
3. Track each aircraft by its ICAO hex code:
   - `alt_baro: "ground"` means on the ground.
   - An altitude plus ground speed of at least 60 kt means airborne.
   - Anything in between is ambiguous and keeps the previous state.
4. **ground → airborne** is a departure. **airborne → ground** is a landing. Aircraft unseen for 3 minutes are forgotten.

Only transitions count, so aircraft that are already parked when it starts don't trigger anything. Detection relies on good ADS-B ground coverage at the airport, which busy airports usually have on adsb.fi.

## Repository layout

```
tasmota/airport_watch.be   Tasmota Berry script
tasmota/README.md          Tasmota install, usage, tuning, porting guide
wled/adsb_blinker.cpp      WLED usermod
wled/README.md             WLED build, settings, development guide
library.json               PlatformIO manifest so WLED can pull the usermod from this repo
```

## Data and terms

- Aircraft data: [adsb.fi](https://adsb.fi) open data API. Personal, non-commercial use only, and adsb.fi asks that you credit them with a link. Please consider [feeding](https://adsb.fi) them a receiver.
- Airport coordinates: [aviationweather.gov](https://aviationweather.gov/data/api/) (NOAA).
