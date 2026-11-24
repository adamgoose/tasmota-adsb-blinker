# airport_watch.be - flash the first WS2812 pixel (QT Py ESP32-C3 NeoPixel) when a plane lands at
# or departs from an airport, using ADS-B data from adsb.fi (https://adsb.fi).
# adsb.fi open data: personal, non-commercial use only.
# Airport coordinates: aviationweather.gov airport API.
#
# Berry:
#   load("airport_watch.be")          start (re-running replaces the old instance)
#   set_airport("KDEN")               look up, save and watch a new airport
#   set_airport("EFHK", 4)            ...with a custom radius in NM (default 3)
#   airport_watch.stop()              stop polling and turn the pixel off
#   airport_watch.debug = true        log every poll
# Tasmota console:
#   Airport                           show the current airport
#   Airport KDEN                      same as set_airport("KDEN")
#
# The airport is saved with `persist`, so it survives reboots without a new lookup.
#
# Departure: was on the ground, now airborne at >= 60 kt  -> 3 green flashes
# Landing:   was airborne, now reports "ground"           -> 3 blue flashes

import json
import string
import global
import persist

class AirportWatch
  static LOOKUP = "https://aviationweather.gov/api/data/airport?format=json&ids="
  static ADSB = "https://opendata.adsb.fi/api/v3/lat/%.5f/lon/%.5f/dist/%d"
  static DEFAULT_NM = 3
  static POLL_S = 10          # seconds between polls (API limit: 1 req/s)
  static AIR_KT = 60          # ground speed that counts as "flying"
  static FORGET_MS = 180000   # drop aircraft not seen for 3 minutes
  static DEP_COLOR = 0x00FF00
  static ARR_COLOR = 0x0000FF
  static DEP_BRI = 120        # 0..255; blue looks much dimmer than green
  static ARR_BRI = 220        # at the same level, so landings get more

  var px, wc, url, icao, state, tick, queue, fx_step, fx_color, debug, deps, arrs

  def init()
    import gpio
    if gpio.pin(gpio.WS2812) >= 0
      self.px = Leds()        # the strip Tasmota manages (Configure Module: WS2812)
    else
      self.px = Leds(1, 2)    # fallback: QT Py ESP32-C3 NeoPixel on GPIO2
    end
    self.px.set_bri(self.DEP_BRI)
    self.px.clear()
    self.wc = webclient()
    self.wc.set_timeouts(4000, 3000)
    self.queue = []
    self.fx_step = 0
    self.debug = false
    self.reset()
    if persist.has("aw_icao")
      self.use(persist.aw_icao, persist.aw_lat, persist.aw_lon, persist.find("aw_nm", self.DEFAULT_NM))
    else
      print("AIRPORT: none set, run set_airport(\"KMSP\") or console 'Airport KMSP'")
    end
    tasmota.add_driver(self)
  end

  def reset()
    self.state = {}           # hex -> [on_ground:bool, last_seen_ms:int]
    self.tick = 0
    self.deps = 0
    self.arrs = 0
  end

  def use(icao, lat, lon, nm)
    self.icao = icao
    self.url = format(self.ADSB, lat, lon, nm)
    self.reset()
    print(format("AIRPORT: watching %s (%.4f, %.4f) within %d NM", icao, lat, lon, nm))
  end

  # Look up an airport by ICAO code, save it and start watching it.
  # Returns true on success. Blocks ~1 s for the HTTPS lookup.
  def set_airport(code, nm)
    if nm == nil nm = self.DEFAULT_NM end
    code = string.toupper(string.tr(str(code), " ", ""))
    if size(code) < 3 || size(code) > 4
      print("AIRPORT: expected a 3-4 letter ICAO code, got", code)
      return false
    end
    if nm < 1 || nm > 250 print("AIRPORT: radius must be 1..250 NM") return false end
    var body
    try
      self.wc.begin(self.LOOKUP + code)
      var rc = self.wc.GET()
      if rc == 200 body = self.wc.get_string() end
      self.wc.close()
      if rc == 204 print("AIRPORT: unknown airport", code) return false end
      if rc != 200 print("AIRPORT: lookup HTTP", rc) return false end
    except .. as e, m
      print("AIRPORT: lookup error", e, m)
      return false
    end
    var j = json.load(body)
    if !isinstance(j, list) || size(j) == 0 print("AIRPORT: unknown airport", code) return false end
    var a = j[0]
    var lat = a.find("lat")
    var lon = a.find("lon")
    if lat == nil || lon == nil print("AIRPORT: no coordinates for", code) return false end
    var icao = a.find("icaoId", code)
    persist.aw_icao = icao
    persist.aw_lat = real(lat)
    persist.aw_lon = real(lon)
    persist.aw_nm = int(nm)
    persist.save()
    print(format("AIRPORT: %s = %s", icao, str(a.find("name", "?"))))
    self.use(icao, real(lat), real(lon), int(nm))
    return true
  end

  def stop()
    tasmota.remove_driver(self)
    tasmota.remove_timer("aw_fx")
    self.queue = []
    self.px.clear()
  end

  def every_second()
    if self.url == nil return end
    self.tick += 1
    if self.tick % self.POLL_S == 0 self.poll() end
  end

  def poll()
    var body
    try
      self.wc.begin(self.url)
      var code = self.wc.GET()      # blocks ~0.7 s, mostly the TLS handshake
      if code == 200 body = self.wc.get_string() end
      self.wc.close()
      if code != 200 print("AIRPORT: adsb.fi HTTP", code) return end
    except .. as e, m
      print("AIRPORT: fetch error", e, m)
      return
    end
    # Berry ints are 32-bit, and the trailer has ms timestamps that break
    # json.load, so keep only {"ac":[...]}
    var cut = string.find(body, ',"msg"')
    if cut > 0 body = body[0 .. cut - 1] + "}" end
    var j = json.load(body)
    if j == nil || !j.contains("ac") print("AIRPORT: bad JSON from adsb.fi") return end
    self.process(j["ac"])
  end

  def process(acs)
    var now = tasmota.millis()
    for a: acs
      var hex = a.find("hex")
      if hex == nil continue end
      var alt = a.find("alt_baro")
      var gs = a.find("gs", 0)
      var on_ground
      if alt == "ground"
        on_ground = true
      elif alt != nil && gs >= self.AIR_KT
        on_ground = false
      end
      # anything else (slow but not "ground", no altitude) is ambiguous:
      # keep the previous state so ADS-B flicker doesn't fake an event
      var prev = self.state.find(hex)
      if on_ground == nil
        if prev != nil prev[1] = now end
        continue
      end
      if prev != nil && prev[0] != on_ground
        var cs = string.tr(a.find("flight", hex), " ", "")
        if on_ground
          self.arrs += 1
          print(format("AIRPORT: %s LANDED   %s (%s) %s", self.icao, cs, a.find("t", "?"), hex))
          self.flash(self.ARR_COLOR)
        else
          self.deps += 1
          print(format("AIRPORT: %s DEPARTED %s (%s) %s %s ft %d kt", self.icao, cs, a.find("t", "?"), hex, str(alt), int(gs)))
          self.flash(self.DEP_COLOR)
        end
      end
      self.state[hex] = [on_ground, now]
    end
    var stale = []
    for k: self.state.keys()
      if now - self.state[k][1] > self.FORGET_MS stale.push(k) end
    end
    for k: stale self.state.remove(k) end
    if self.debug
      print(format("AIRPORT: %s %d aircraft, tracking %d, deps %d, arrs %d", self.icao, size(acs), size(self.state), self.deps, self.arrs))
    end
  end

  def flash(color)
    self.queue.push(color)
    if self.fx_step == 0 self.fx_next() end
  end

  # 3 flashes = 6 steps of 150 ms, then the next queued event
  def fx_next()
    if self.fx_step == 0
      if size(self.queue) == 0 return end
      self.fx_color = self.queue.pop(0)
      self.px.set_bri(self.fx_color == self.ARR_COLOR ? self.ARR_BRI : self.DEP_BRI)
    end
    self.px.set_pixel_color(0, self.fx_step % 2 == 0 ? self.fx_color : 0)
    self.px.show()
    self.fx_step += 1
    if self.fx_step >= 6 self.fx_step = 0 end
    tasmota.set_timer(self.fx_step == 0 ? 400 : 150, /-> self.fx_next(), "aw_fx")
  end
end

if global.contains("airport_watch") && global.airport_watch != nil global.airport_watch.stop() end
global.airport_watch = AirportWatch()

def set_airport(code, nm)
  return global.airport_watch.set_airport(code, nm)
end
global.set_airport = set_airport

# Tasmota console command: "Airport" shows, "Airport KDEN" sets
tasmota.remove_cmd("Airport")
tasmota.add_cmd("Airport", def (cmd, idx, payload)
  var w = global.airport_watch
  if size(payload) > 0 && !w.set_airport(payload)
    tasmota.resp_cmnd_failed()
    return
  end
  tasmota.resp_cmnd(format('{"Airport":"%s","Departures":%d,"Landings":%d}', str(w.icao), w.deps, w.arrs))
end)
