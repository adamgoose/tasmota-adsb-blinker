/*
 * ADS-B Blinker - WLED usermod
 *
 * Flashes the LEDs (or applies a WLED preset) whenever a plane lands at or
 * departs from a chosen airport, using live ADS-B data from adsb.fi.
 *
 *   Departure: aircraft was on the ground, now airborne at >= airKt knots
 *   Landing:   aircraft was airborne, now reports alt_baro "ground"
 *
 * Data: adsb.fi open data API (https://adsb.fi) - personal, non-commercial use.
 * Airport coordinates: aviationweather.gov airport API.
 *
 * ESP32 only: HTTPS polling runs in its own FreeRTOS task so the TLS handshake
 * never stalls effects. All settings live on the Usermod settings page.
 */

#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
  #error "ADS-B Blinker needs an ESP32 (HTTPS + FreeRTOS task)"
#endif

// WLED's ESP32 builds use a trimmed Arduino core with no WiFiClientSecure and
// no mbedTLS SSL layer, so TLS comes from ESP_SSLClient (bundled BearSSL)
// wrapped around a plain WiFiClient.
#ifdef BEARSSL_SSL_BASIC
  // WLED's common build_flags set this for ESP8266 builds. It strips the
  // elliptic-curve ciphers that adsb.fi and aviationweather.gov need.
  #error "Remove -DBEARSSL_SSL_BASIC: add 'build_unflags = ${env:<base env>.build_unflags} -DBEARSSL_SSL_BASIC' to your env (see wled/README.md)"
#endif
#include <WiFiClient.h>
#include <ESP_SSLClient.h>
#include <map>

#define ADSB_TASK_STACK   12288     // BearSSL handshake needs a deep stack
#define ADSB_TLS_RX_BUF   16709     // a full TLS record (16 KB + overhead)
#define ADSB_TLS_TX_BUF   1024
#define ADSB_DOC_SIZE     40960     // filtered aircraft list; ~150 B per aircraft (~250 aircraft)
#define ADSB_FORGET_MS    180000UL  // drop aircraft not seen for 3 minutes
#define ADSB_FLASH_ON_MS  150
#define ADSB_FLASH_GAP_MS 400
#define ADSB_LOG_SIZE     50        // events kept for the log page (RAM only)
#define ADSB_INFO_RECENT  3         // events shown in the Info panel

// The /adsb log page. Data comes from /adsb/log; times are computed in the
// browser from each event's age, so they're right even without NTP on the device.
static const char ADSB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Flights</title><style>
body{background:#222;color:#fff;font-family:Helvetica,Verdana,sans-serif;margin:0;padding:16px;text-align:center}
h2{margin:8px 0 4px}#s{color:#aaa;font-size:14px;margin-bottom:14px}
table{margin:0 auto;border-collapse:collapse;width:100%;max-width:560px}
td,th{padding:9px 6px;border-bottom:1px solid #333;text-align:left}th{color:#aaa;font-weight:normal;font-size:13px}
.d{color:#5e5}.a{color:#6af}.w{color:#fc4}.m{color:#888}
button{background:#333;color:#fff;border:0;border-radius:24px;padding:8px 20px;margin:8px 4px;font-size:15px;cursor:pointer}
</style></head><body><button onclick="location.href='/'">Back</button>
<h2 id="h">Flights</h2><div id="s"></div>
<table><thead><tr><th>Time</th><th>Event</th><th>Callsign</th><th>Type</th></tr></thead><tbody id="b"></tbody></table>
<script>
function E(x){return String(x).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';')}
function f(){fetch('/adsb/log').then(r=>r.json()).then(j=>{
h.textContent=j.icao?j.icao+' landings & departures':'Flights';
let P=(n,w)=>n+' '+w+(n==1?'':'s');
s.textContent=j.status+' · '+P(j.deps,'departure')+', '+P(j.arrs,'landing')+(j.spots?', '+P(j.spots,'sighting'):'')+' since boot';
let n=Date.now(),K={dep:['d','↑ Departure'],arr:['a','↓ Landing'],watch:['w','★ Spotted']};
b.innerHTML=j.events.length?j.events.map(e=>{let t=new Date(n-e.ago*1000),k=K[e.k]||K.arr;
return '<tr><td>'+t.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})+'</td><td class='+k[0]+'>'
+k[1]+(e.ap!=j.icao?' <span class=m>'+E(e.ap)+'</span>':'')
+'</td><td>'+E(e.cs)+'</td><td>'+E(e.t)+'</td></tr>'}).join('')
:'<tr><td colspan=4 class=m style="text-align:center">No landings or departures yet</td></tr>'
}).catch(()=>{s.textContent='Can\'t reach the device'})}
f();setInterval(f,10000)
</script></body></html>)HTML";

// GET https://host/path and parse the JSON body through a filter.
// HTTP/1.0 so the server never uses chunked encoding and ArduinoJson can read
// the body straight off the TLS stream (nothing is buffered).
// Returns the HTTP status, or -1 if the connection failed; *err gets the parse result.
// The data is public and not sensitive, so the certificate isn't verified.
static int adsbGetJson(const char* host, const char* path, JsonDocument& filter,
                       JsonDocument& doc, DeserializationError* err) {
  WiFiClient tcp;
  ESP_SSLClient tls;
  tls.setInsecure();
  tls.setBufferSizes(ADSB_TLS_RX_BUF, ADSB_TLS_TX_BUF);
  tls.setTimeout(8000);
  tls.setClient(&tcp);
  if (!tls.connect(host, 443)) { tls.stop(); return -1; }

  char req[224];
  int n = snprintf(req, sizeof(req),
    "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: WLED-adsb-blinker\r\n"
    "Accept: application/json\r\nConnection: close\r\n\r\n", path, host);
  tls.write((const uint8_t*)req, n);

  String line = tls.readStringUntil('\n');           // "HTTP/1.1 200 OK"
  int status = (line.length() >= 12) ? line.substring(9, 12).toInt() : -1;
  while (tls.connected() || tls.available()) {       // skip headers
    line = tls.readStringUntil('\n');
    if (line.length() <= 1) break;                   // "\r" = end of headers
  }
  if (status == 200) *err = deserializeJson(doc, tls, DeserializationOption::Filter(filter));
  tls.stop();
  return status;
}

class AdsbBlinker : public Usermod {
  public:
    enum Kind : uint8_t { DEPARTURE = 0, LANDING = 1, WATCH = 2 };
    enum Mode : uint8_t { MODE_FLASH = 0, MODE_PRESET = 1 };

  private:
    // preset: WATCH only, the entry's own preset (0 = use the default watch preset)
    struct Event { uint8_t kind; uint8_t preset; char callsign[10]; char type[6]; };
    struct Track { bool ground; uint32_t seen; };
    struct WatchRule { String pattern; uint8_t preset; };
    // What the polling task needs, copied under the mutex
    struct Snapshot {
      bool enabled; char icao[8]; float lat, lon; int radius, pollSec, airKt; uint32_t gen;
      String watchList; int watchRadius;
    };

    // ---- settings (Usermod settings page / cfg.json) ----
    bool    enabled    = false;
    String  airport    = "KMSP";
    int     radiusNm   = 3;
    int     pollSec    = 10;
    int     airKt      = 60;
    uint8_t mode       = MODE_FLASH;
    String  depColor   = "#00ff00";
    String  arrColor   = "#0000ff";
    int     flashCount = 3;
    int     flashLeds  = 0;        // 0 = every LED
    int     depPreset  = 0;
    int     arrPreset  = 0;
    int     presetSec  = 10;       // 0 = leave the preset on
    String  watchList;             // "N123AB, DAL*, UAL123=4" (callsigns or tail numbers)
    int     watchPreset = 0;       // default preset for watch hits
    int     watchSec   = 30;       // 0 = leave the preset on
    int     watchRadius = 10;      // NM; polled area = max(radiusNm, watchRadius)
    // cached lookup, kept in /adsb.json (not in the settings) so a reboot needs no new lookup
    String  resolvedIcao;
    float   lat = NAN, lon = NAN;

    // ---- shared with the task (guarded by mtx) ----
    SemaphoreHandle_t mtx = nullptr;
    QueueHandle_t     events = nullptr;
    TaskHandle_t      task = nullptr;
    uint32_t gen = 0;              // bumped when the airport or radius changes
    bool     lookupDone = false;   // task resolved new coordinates -> save cfg
    char     status[48] = "starting";

    // ---- event log (written in loop, read by the web server; guarded by mtx) ----
    struct LogEntry { uint32_t ms; uint8_t kind; char callsign[10]; char type[6]; char icao[8]; };
    LogEntry evLog[ADSB_LOG_SIZE];
    uint8_t  logHead = 0, logLen = 0;   // logHead = next slot to write
    uint32_t deps = 0, arrs = 0, spots = 0;

    // ---- runtime (main loop only) ----
    uint32_t fxColor = 0;
    int      fxStep = -1;          // -1 idle; otherwise index into the on/off sequence
    uint32_t fxNext = 0;
    bool     fxOn = false;
    uint8_t  fxQueue[8]; uint8_t fxHead = 0, fxLen = 0;
    // preset mode: what to go back to when the preset's time is up
    bool     presetActive = false;
    uint32_t presetUntil = 0;
    int16_t  savedPlaylist = -1;   // a running playlist is restarted rather than snapshotted
    byte     savedPreset = 0;
    // Kept as JSON text: serializeState() writes segment colors as raw
    // pre-rendered JSON, which only becomes a real array once re-parsed.
    String   savedState;

    static const char _name[];
    static const char _cacheFile[];

    static uint32_t parseHex(String s, uint32_t fallback) {
      s.trim(); if (s.startsWith("#")) s.remove(0, 1);
      if (s.length() != 6) return fallback;
      char* end; uint32_t v = strtoul(s.c_str(), &end, 16);
      return *end ? fallback : v;
    }

    static String normColor(String s, const char* fallback) {
      s.trim(); s.toLowerCase(); if (!s.startsWith("#")) s = "#" + s;
      return (s.length() == 7 && parseHex(s, 0xFFFFFFFF) != 0xFFFFFFFF) ? s : String(fallback);
    }

    // ---------- preset mode with timed restore ----------

    // secs = how long to show it (0 = leave it on). Overlapping events extend the
    // timer; the look restored at the end is the one from before the first event.
    void startPreset(int p, int secs) {
      if (p <= 0 || p > 250) return;
      if (!presetActive && secs > 0) {           // first event: remember the current look
        savedPlaylist = currentPlaylist;
        savedPreset = currentPreset;
        savedState = "";
        if (savedPlaylist < 0) {
          DynamicJsonDocument snap(6144);
          serializeState(snap.to<JsonObject>(), true);
          if (!snap.overflowed()) serializeJson(snap, savedState);
        }
      }
      applyPreset(p);
      if (secs > 0) { presetActive = true; presetUntil = millis() + secs * 1000UL; }
      else presetActive = false;                 // "leave on" wins: nothing to restore
    }

    // ---------- watch list ----------

    // "N123AB, DAL*  UAL123=4" -> rules; separators are commas, spaces or newlines
    static std::vector<WatchRule> parseWatch(const String& list) {
      std::vector<WatchRule> rules;
      String tok;
      auto flush = [&]() {
        tok.trim(); if (!tok.length()) return;
        WatchRule r; r.preset = 0;
        int eq = tok.indexOf('=');
        if (eq >= 0) { r.preset = constrain(tok.substring(eq + 1).toInt(), 0, 250); tok = tok.substring(0, eq); }
        tok.trim(); tok.toUpperCase();
        if (tok.length()) { r.pattern = tok; rules.push_back(r); }
        tok = "";
      };
      for (size_t i = 0; i < list.length(); i++) {
        char c = list[i];
        if (c == ',' || c == ';' || c == ' ' || c == '\n' || c == '\r' || c == '\t') flush(); else tok += c;
      }
      flush();
      return rules;
    }

    // Case-insensitive glob: '*' = any run of characters, '?' = one character
    static bool globMatch(const char* p, const char* s) {
      if (!*p) return !*s;
      if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
      if (!*s) return false;
      if (*p == '?' || toupper((unsigned char)*p) == toupper((unsigned char)*s)) return globMatch(p + 1, s + 1);
      return false;
    }

    void endPreset() {
      presetActive = false;
      bool restored = false;
      if (savedPlaylist >= 0) { applyPreset(savedPlaylist); restored = true; }   // restart the playlist
      else if (savedState.length()) {
        DynamicJsonDocument snap(6144);
        if (!deserializeJson(snap, savedState)) {
          deserializeState(snap.as<JsonObject>(), CALL_MODE_DIRECT_CHANGE, savedPreset);
          restored = true;
        }
      }
      if (!restored && savedPreset) applyPreset(savedPreset);              // best effort
      savedState = "";
    }

    // ---------- coordinate cache (/adsb.json) ----------

    // A tiny standalone file, so plain read/write of the whole file is enough.
    void loadCache() {
      File f = WLED_FS.open(_cacheFile, "r");
      if (!f) return;
      StaticJsonDocument<192> c;
      DeserializationError err = deserializeJson(c, f);
      f.close();
      if (err || !c["lat"].is<float>() || !c["lon"].is<float>()) return;
      xSemaphoreTake(mtx, portMAX_DELAY);
      resolvedIcao = c["icao"] | "";
      lat = c["lat"]; lon = c["lon"];
      xSemaphoreGive(mtx);
    }

    void saveCache() {
      StaticJsonDocument<192> c;
      xSemaphoreTake(mtx, portMAX_DELAY);
      c["icao"] = resolvedIcao; c["lat"] = lat; c["lon"] = lon;
      xSemaphoreGive(mtx);
      File f = WLED_FS.open(_cacheFile, "w");
      if (!f) return;
      serializeJson(c, f);
      f.close();
    }

    void setStatus(const char* s) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      strlcpy(status, s, sizeof(status));
      xSemaphoreGive(mtx);
    }

    Snapshot snapshot() {
      Snapshot s;
      xSemaphoreTake(mtx, portMAX_DELAY);
      s.enabled = enabled;
      strlcpy(s.icao, airport.c_str(), sizeof(s.icao));
      s.lat = (resolvedIcao == airport) ? lat : NAN;
      s.lon = (resolvedIcao == airport) ? lon : NAN;
      s.radius = radiusNm; s.pollSec = pollSec; s.airKt = airKt; s.gen = gen;
      s.watchList = watchList; s.watchRadius = watchRadius;
      xSemaphoreGive(mtx);
      return s;
    }

    // ---------- polling task ----------

    static void taskEntry(void* arg) { static_cast<AdsbBlinker*>(arg)->taskLoop(); }

    void taskLoop() {
      std::map<uint32_t, Track> tracks;
      std::map<uint32_t, uint32_t> watched;     // watch-list aircraft in the area -> last seen
      uint32_t trackGen = UINT32_MAX;
      for (;;) {
        Snapshot s = snapshot();
        if (!s.enabled) { setStatus("disabled"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!WLED_CONNECTED) { setStatus("waiting for network"); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        if (isnan(s.lat)) {
          if (!lookupAirport(s.icao)) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }
          continue;  // re-snapshot with the new coordinates
        }
        if (s.gen != trackGen) { tracks.clear(); watched.clear(); trackGen = s.gen; }

        uint32_t started = millis();
        pollAdsb(s, tracks, watched);
        uint32_t waitMs = max(1000, s.pollSec * 1000);   // adsb.fi allows 1 req/s
        uint32_t spent = millis() - started;
        vTaskDelay(pdMS_TO_TICKS(spent < waitMs ? waitMs - spent : 0));
      }
    }

    bool lookupAirport(const char* icao) {
      char path[64];
      snprintf(path, sizeof(path), "/api/data/airport?format=json&ids=%s", icao);
      StaticJsonDocument<96> filter;
      filter[0]["lat"] = true; filter[0]["lon"] = true; filter[0]["icaoId"] = true;
      DynamicJsonDocument doc(512);
      DeserializationError err;
      int code = adsbGetJson("aviationweather.gov", path, filter, doc, &err);
      if (code != 200) {
        char msg[48];
        if (code == 204) snprintf(msg, sizeof(msg), "unknown airport %s", icao);
        else             snprintf(msg, sizeof(msg), "lookup HTTP %d", code);
        setStatus(msg); return false;
      }
      JsonVariant a = doc[0];
      if (err || a.isNull() || !a["lat"].is<float>() || !a["lon"].is<float>()) {
        char msg[48]; snprintf(msg, sizeof(msg), "unknown airport %s", icao); setStatus(msg); return false;
      }
      xSemaphoreTake(mtx, portMAX_DELAY);
      if (airport == icao) {        // settings may have changed while we waited
        lat = a["lat"]; lon = a["lon"];
        resolvedIcao = icao;
        gen++;
        lookupDone = true;
      }
      xSemaphoreGive(mtx);
      return true;
    }

    void pollAdsb(const Snapshot& s, std::map<uint32_t, Track>& tracks, std::map<uint32_t, uint32_t>& watched) {
      // One request covers both circles; adsb.fi's "dst" (NM from the center)
      // keeps landing/departure detection to the inner radius.
      std::vector<WatchRule> rules = parseWatch(s.watchList);
      int pollRadius = rules.empty() ? s.radius : max(s.radius, s.watchRadius);
      char path[80];
      snprintf(path, sizeof(path), "/api/v3/lat/%.5f/lon/%.5f/dist/%d", s.lat, s.lon, pollRadius);
      // Keep only the fields we use; this also skips the 13-digit timestamps
      StaticJsonDocument<256> filter;
      JsonObject f = filter["ac"].createNestedObject();
      f["hex"] = true; f["flight"] = true; f["r"] = true; f["t"] = true;
      f["alt_baro"] = true; f["gs"] = true; f["dst"] = true;
      DynamicJsonDocument doc(ADSB_DOC_SIZE);
      DeserializationError err;
      int code = adsbGetJson("opendata.adsb.fi", path, filter, doc, &err);
      if (code != 200) {
        char msg[48]; snprintf(msg, sizeof(msg), code < 0 ? "adsb.fi: connection failed" : "adsb.fi HTTP %d", code);
        setStatus(msg); return;
      }
      if (err) {
        char msg[48];
        snprintf(msg, sizeof(msg), err == DeserializationError::NoMemory ? "too many aircraft, lower radius" : "adsb JSON: %s", err.c_str());
        setStatus(msg); return;
      }

      uint32_t now = millis();
      unsigned nearby = 0;
      JsonArray acs = doc["ac"];
      for (JsonObject a : acs) {
        const char* hex = a["hex"] | "";
        bool nonIcao = (*hex == '~');           // TIS-B / non-ICAO addresses
        uint32_t id = strtoul(hex + (nonIcao ? 1 : 0), nullptr, 16) | (nonIcao ? 0x1000000UL : 0);
        if (!id) continue;

        // Watch list: fire once when a matching aircraft shows up in the polled area
        if (!rules.empty()) {
          char cs[12], reg[12];
          cleanCopy(cs, a["flight"] | "", sizeof(cs));
          cleanCopy(reg, a["r"] | "", sizeof(reg));
          for (const WatchRule& r : rules) {
            bool hit = (cs[0] != '?' && globMatch(r.pattern.c_str(), cs)) ||
                       (reg[0] != '?' && globMatch(r.pattern.c_str(), reg));
            if (!hit) continue;
            if (!watched.count(id)) {
              Event ev; ev.kind = WATCH; ev.preset = r.preset;
              strlcpy(ev.callsign, cs[0] != '?' ? cs : reg, sizeof(ev.callsign));
              cleanCopy(ev.type, a["t"] | "?", sizeof(ev.type));
              xQueueSend(events, &ev, 0);
            }
            watched[id] = now;
            break;
          }
        }

        // Landings and departures: only within the airport radius
        float dst = a["dst"] | 0.0f;
        if (dst > s.radius) continue;
        nearby++;
        JsonVariant alt = a["alt_baro"];
        float gs = a["gs"] | 0.0f;
        int8_t ground = -1;                      // -1 = ambiguous, keep previous state
        if (alt.is<const char*>() && strcmp(alt.as<const char*>(), "ground") == 0) ground = 1;
        else if (alt.is<float>() && gs >= s.airKt) ground = 0;

        auto it = tracks.find(id);
        if (ground < 0) { if (it != tracks.end()) it->second.seen = now; continue; }
        if (it != tracks.end() && it->second.ground != (bool)ground) {
          Event ev; ev.kind = ground ? LANDING : DEPARTURE; ev.preset = 0;
          cleanCopy(ev.callsign, a["flight"] | hex, sizeof(ev.callsign));
          cleanCopy(ev.type, a["t"] | "?", sizeof(ev.type));
          xQueueSend(events, &ev, 0);
        }
        tracks[id] = { (bool)ground, now };
      }
      for (auto it = tracks.begin(); it != tracks.end(); )
        it = (now - it->second.seen > ADSB_FORGET_MS) ? tracks.erase(it) : std::next(it);
      // a watched aircraft gone this long can trigger again when it returns
      for (auto it = watched.begin(); it != watched.end(); )
        it = (now - it->second > ADSB_FORGET_MS) ? watched.erase(it) : std::next(it);

      char msg[48];
      if (pollRadius > s.radius)
        snprintf(msg, sizeof(msg), "%s: %u nearby, %u within %d NM", s.icao, nearby, (unsigned)acs.size(), pollRadius);
      else
        snprintf(msg, sizeof(msg), "%s: %u aircraft", s.icao, nearby);
      setStatus(msg);
    }

    // ---------- output (main loop) ----------

    // Copy keeping only [A-Za-z0-9-] (the text ends up in HTML), "?" if nothing is left
    static void cleanCopy(char* dst, const char* src, size_t n) {
      size_t j = 0;
      for (; *src && j + 1 < n; src++) if (isalnum((unsigned char)*src) || *src == '-') dst[j++] = *src;
      if (!j && n > 1) dst[j++] = '?';
      dst[j] = 0;
    }

    void logEvent(const Event& ev) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      LogEntry& e = evLog[logHead];
      e.ms = millis(); e.kind = ev.kind;
      strlcpy(e.callsign, ev.callsign, sizeof(e.callsign));
      strlcpy(e.type, ev.type, sizeof(e.type));
      strlcpy(e.icao, airport.c_str(), sizeof(e.icao));
      logHead = (logHead + 1) % ADSB_LOG_SIZE;
      if (logLen < ADSB_LOG_SIZE) logLen++;
      if (ev.kind == DEPARTURE) deps++; else if (ev.kind == LANDING) arrs++; else spots++;
      xSemaphoreGive(mtx);
    }

    // i = 0 is the newest entry; call with mtx held
    const LogEntry& logAt(uint8_t i) { return evLog[(logHead + ADSB_LOG_SIZE - 1 - i) % ADSB_LOG_SIZE]; }

    static void fmtAgo(char* out, size_t n, uint32_t ms) {
      uint32_t s = (millis() - ms) / 1000;
      if (s < 60)        snprintf(out, n, "just now");
      else if (s < 3600) snprintf(out, n, "%um ago", (unsigned)(s / 60));
      else               snprintf(out, n, "%uh ago", (unsigned)(s / 3600));
    }

    // GET /adsb/log -> {"icao","status","deps","arrs","events":[{"k","cs","t","ap","ago"}]} (newest first)
    void sendLog(AsyncWebServerRequest* request) {
      DynamicJsonDocument doc(512 + ADSB_LOG_SIZE * 160);
      xSemaphoreTake(mtx, portMAX_DELAY);
      doc["icao"] = airport; doc["status"] = status; doc["deps"] = deps; doc["arrs"] = arrs; doc["spots"] = spots;
      JsonArray evs = doc.createNestedArray("events");
      for (uint8_t i = 0; i < logLen; i++) {
        const LogEntry& e = logAt(i);
        JsonObject o = evs.createNestedObject();
        o["k"] = e.kind == DEPARTURE ? "dep" : e.kind == LANDING ? "arr" : "watch";
        o["cs"] = e.callsign; o["t"] = e.type; o["ap"] = e.icao;
        o["ago"] = (millis() - e.ms) / 1000;
      }
      xSemaphoreGive(mtx);
      String out; serializeJson(doc, out);
      request->send(200, F("application/json"), out);
    }

    void onEvent(const Event& ev) {
      logEvent(ev);
      DEBUG_PRINTF_P(PSTR("ADS-B: %s %s (%s)\n"), ev.kind == DEPARTURE ? "dep" : ev.kind == LANDING ? "arr" : "watch", ev.callsign, ev.type);
      if (ev.kind == WATCH) {                   // watch hits always use presets
        startPreset(ev.preset ? ev.preset : watchPreset, watchSec);
        return;
      }
      if (mode == MODE_PRESET) {
        startPreset(ev.kind == DEPARTURE ? depPreset : arrPreset, presetSec);
        return;
      }
      if (fxLen < sizeof(fxQueue)) fxQueue[(fxHead + fxLen++) % sizeof(fxQueue)] = ev.kind;
    }

    void runFlash() {
      uint32_t now = millis();
      if (fxStep < 0) {
        if (!fxLen) return;
        uint8_t kind = fxQueue[fxHead]; fxHead = (fxHead + 1) % sizeof(fxQueue); fxLen--;
        fxColor = kind == DEPARTURE ? parseHex(depColor, 0x00FF00) : parseHex(arrColor, 0x0000FF);
        fxStep = 0; fxNext = now;
      }
      if ((int32_t)(now - fxNext) < 0) return;
      int steps = max(1, flashCount) * 2;
      if (fxStep < steps) {
        fxOn = (fxStep % 2 == 0);
        fxNext = now + ADSB_FLASH_ON_MS;
        fxStep++;
      } else {                       // gap before the next queued event
        fxOn = false;
        fxStep = -1;
        fxNext = now + ADSB_FLASH_GAP_MS;
      }
      strip.trigger();               // redraw even if the effect is static
    }

  public:
    void setup() override {
      mtx = xSemaphoreCreateMutex();
      events = xQueueCreate(8, sizeof(Event));
      if (isnan(lat)) loadCache();
      else saveCache();              // migrated from cfg.json (older versions kept it there)
      xTaskCreate(taskEntry, "adsb", ADSB_TASK_STACK, this, 1, &task);
      // the log page and its data (registered once, before WLED's catch-all handler)
      server.on(F("/adsb/log"), HTTP_GET, [this](AsyncWebServerRequest* r) { sendLog(r); });
      server.on(F("/adsb"), HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send_P(200, PSTR("text/html"), ADSB_PAGE);
      });
    }

    void loop() override {
      Event ev;
      while (events && xQueueReceive(events, &ev, 0) == pdTRUE) onEvent(ev);
      runFlash();
      if (presetActive && ((int32_t)(millis() - presetUntil) >= 0 || !enabled)) endPreset();
      if (mtx && lookupDone) {
        xSemaphoreTake(mtx, portMAX_DELAY); lookupDone = false; xSemaphoreGive(mtx);
        saveCache();
      }
    }

    void handleOverlayDraw() override {
      if (!fxOn) return;
      unsigned total = strip.getLengthTotal();
      unsigned n = (flashLeds > 0 && (unsigned)flashLeds < total) ? flashLeds : total;
      for (unsigned i = 0; i < n; i++) strip.setPixelColor(i, fxColor);
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      // The Info panel renders these values as HTML
      char line[96];
      String recent;
      xSemaphoreTake(mtx, portMAX_DELAY);
      snprintf(line, sizeof(line), "%s &middot; dep %u arr %u", status, (unsigned)deps, (unsigned)arrs);
      if (spots) snprintf(line + strlen(line), sizeof(line) - strlen(line), " &#9733; %u", (unsigned)spots);
      for (uint8_t i = 0; i < logLen && i < ADSB_INFO_RECENT; i++) {
        const LogEntry& e = logAt(i);
        char ago[16]; fmtAgo(ago, sizeof(ago), e.ms);
        char row[64];
        snprintf(row, sizeof(row), "%s %s %s &middot; %s<br>",
                 e.kind == DEPARTURE ? "&uarr;" : e.kind == LANDING ? "&darr;" : "&#9733;", e.callsign, e.type, ago);
        recent += row;
      }
      xSemaphoreGive(mtx);
      if (!recent.length()) recent = F("none yet<br>");
      // (btn-xs is a fixed-size round icon button, so size this one explicitly)
      recent += F("<button class=\"btn\" style=\"width:auto;height:auto;min-width:0;padding:4px 14px;margin:6px 0 0;"
                  "font-size:13px;white-space:nowrap\" onclick=\"location.href='/adsb'\">Full log</button>");
      user.createNestedArray(F("ADS-B Blinker")).add(line);
      user.createNestedArray(F("Recent flights")).add(recent);
    }

    // Settings are grouped into nested objects; WLED shows each one as a titled
    // section on the Usermod settings page. appendConfigData() then relabels the
    // fields and adds color pickers, preset dropdowns and show/hide by action.
    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[F("enabled")] = enabled;
      JsonObject a = top.createNestedObject(F("Airport"));
      a[F("icao")]        = airport;
      a[F("radius_nm")]   = radiusNm;
      JsonObject act = top.createNestedObject(F("Action"));
      act[F("type")]      = mode;
      JsonObject f = top.createNestedObject(F("Flash"));
      f[F("departure")]   = depColor;
      f[F("landing")]     = arrColor;
      f[F("count")]       = flashCount;
      f[F("leds")]        = flashLeds;
      JsonObject p = top.createNestedObject(F("Preset"));
      p[F("departure")]   = depPreset;
      p[F("landing")]     = arrPreset;
      p[F("seconds")]     = presetSec;
      JsonObject w = top.createNestedObject(F("Watch"));
      w[F("list")]        = watchList;
      w[F("preset")]      = watchPreset;
      w[F("seconds")]     = watchSec;
      w[F("radius_nm")]   = watchRadius;
      JsonObject det = top.createNestedObject(F("Detection"));
      det[F("poll_s")]      = pollSec;
      det[F("airborne_kt")] = airKt;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
      String oldAirport = airport; int oldRadius = radiusNm;
      bool complete = !top.isNull() && !top[F("Airport")].isNull();
      getJsonValue(top[F("enabled")], enabled, false);
      if (complete) {
        JsonObject a = top[F("Airport")], act = top[F("Action")], f = top[F("Flash")],
                   p = top[F("Preset")], det = top[F("Detection")];
        complete &= getJsonValue(a[F("icao")],          airport, String("KMSP"));
        complete &= getJsonValue(a[F("radius_nm")],     radiusNm, 3);
        complete &= getJsonValue(act[F("type")],        mode, (uint8_t)MODE_FLASH);
        complete &= getJsonValue(f[F("departure")],     depColor, String("#00ff00"));
        complete &= getJsonValue(f[F("landing")],       arrColor, String("#0000ff"));
        complete &= getJsonValue(f[F("count")],         flashCount, 3);
        complete &= getJsonValue(f[F("leds")],          flashLeds, 0);
        complete &= getJsonValue(p[F("departure")],     depPreset, 0);
        complete &= getJsonValue(p[F("landing")],       arrPreset, 0);
        complete &= getJsonValue(p[F("seconds")],       presetSec, 10);
        complete &= getJsonValue(det[F("poll_s")],      pollSec, 10);
        complete &= getJsonValue(det[F("airborne_kt")], airKt, 60);
        JsonObject w = top[F("Watch")];         // added later: missing = defaults + rewrite
        complete &= getJsonValue(w[F("list")],      watchList, String(""));
        complete &= getJsonValue(w[F("preset")],    watchPreset, 0);
        complete &= getJsonValue(w[F("seconds")],   watchSec, 30);
        complete &= getJsonValue(w[F("radius_nm")], watchRadius, 10);
      } else if (!top.isNull()) {
        // migrate settings from the first (flat) layout; returning false rewrites them
        getJsonValue(top[F("airport")],    airport, String("KMSP"));
        getJsonValue(top[F("radiusNm")],   radiusNm, 3);
        getJsonValue(top[F("pollSec")],    pollSec, 10);
        getJsonValue(top[F("airborneKt")], airKt, 60);
        getJsonValue(top[F("mode")],       mode, (uint8_t)MODE_FLASH);
        getJsonValue(top[F("depColor")],   depColor, String("#00ff00"));
        getJsonValue(top[F("arrColor")],   arrColor, String("#0000ff"));
        getJsonValue(top[F("flashCount")], flashCount, 3);
        getJsonValue(top[F("flashLeds")],  flashLeds, 0);
        getJsonValue(top[F("depPreset")],  depPreset, 0);
        getJsonValue(top[F("arrPreset")],  arrPreset, 0);
        getJsonValue(top[F("resolvedIcao")], resolvedIcao, String(""));
        if (!top[F("lat")].isNull()) lat = top[F("lat")].as<float>();
        if (!top[F("lon")].isNull()) lon = top[F("lon")].as<float>();
      }
      airport.trim(); airport.toUpperCase();
      depColor = normColor(depColor, "#00ff00");
      arrColor = normColor(arrColor, "#0000ff");
      radiusNm   = constrain(radiusNm, 1, 250);
      pollSec    = constrain(pollSec, 2, 3600);
      flashCount = constrain(flashCount, 1, 20);
      presetSec  = constrain(presetSec, 0, 86400);
      watchList.trim();
      watchPreset = constrain(watchPreset, 0, 250);
      watchSec    = constrain(watchSec, 0, 86400);
      watchRadius = constrain(watchRadius, 1, 100);   // keeps responses within ADSB_DOC_SIZE
      if (airport != oldAirport || radiusNm != oldRadius) gen++;
      if (mtx) xSemaphoreGive(mtx);
      return complete;
    }

    void appendConfigData() override {
      // L(field,label,after): relabel a field and add text after it
      oappend(F("var A='ADS-B Blinker:';"
        "function I(f){let a=d.getElementsByName(A+f);return a[a.length-1];}"
        "function L(f,t,u){let i=I(f);if(!i)return;let p=i.previousSibling;"
          "while(p&&p.nodeType!=3)p=p.previousSibling;if(p)p.textContent=' '+t+' ';"
          "if(u)i.insertAdjacentHTML('afterend','&nbsp;'+u);}"
        // wrap a titled group (hr + title + fields) in a div so it can be hidden
        "function G(t){let c=I('enabled').parentElement,p=[...c.querySelectorAll('p')].find(x=>x.textContent==t);"
          "if(!p)return cE('div');let w=cE('div'),n=p.previousSibling&&p.previousSibling.nodeName=='HR'?p.previousSibling:p;"
          "n.before(w);do{let x=n.nextSibling;w.appendChild(n);n=x;}while(n&&n.nodeName!='HR');return w;}"));
      oappend(F("L('Airport:icao','ICAO code');I('Airport:icao').placeholder='KMSP';"
        "L('Airport:radius_nm','Radius','NM');"
        "L('Detection:poll_s','Check every','seconds');"
        "L('Detection:airborne_kt','Airborne above','knots');"
        "L('Action:type','When a plane lands or departs');"
        "L('Flash:departure','Departure color');L('Flash:landing','Landing color');"
        "L('Flash:count','Flashes');L('Flash:leds','LEDs','(0 = all)');"
        "L('Preset:departure','Departure preset');L('Preset:landing','Landing preset');"
        "L('Preset:seconds','Show for','seconds, then restore (0 = leave on)');"
        "for(let f of['Flash:departure','Flash:landing'])I(f).type='color';"
        "L('Watch:list','Callsigns / tail numbers');I('Watch:list').placeholder='N123AB, DAL*, UAL123=4';"
        "I('Watch:list').insertAdjacentHTML('afterend','<br><i>comma-separated &middot; * = wildcard &middot; =ID for its own preset</i>');"
        "L('Watch:preset','Default preset');L('Watch:seconds','Show for','seconds (0 = leave on)');"
        "L('Watch:radius_nm','Watch within','NM');"));
      oappend(F("let s=addDropdown('ADS-B Blinker','Action:type');addOption(s,'Flash the LEDs',0);addOption(s,'Apply a preset',1);"
        "let gf=G('Flash'),gp=G('Preset'),T=()=>{gf.style.display=s.value==0?'':'none';gp.style.display=s.value==1?'':'none';};"
        "s.addEventListener('change',T);T();"
        "let pd=['Preset:departure','Preset:landing','Watch:preset'].map(f=>addDropdown('ADS-B Blinker',f));"
        "pd.forEach(x=>addOption(x,'None',0));"
        "fetch('/presets.json').then(r=>r.json()).then(j=>{for(let k in j)if(k>0&&j[k].n)pd.forEach(x=>addOption(x,k+': '+j[k].n,k));});"));
    }

    // Test from the JSON API: {"adsb":{"test":"dep"}}, "arr" or "watch"
    // This runs in the web server's task while WLED is applying the request, so
    // only queue the event; loop() handles it like a real one.
    void readFromJsonState(JsonObject& root) override {
      JsonObject um = root[F("adsb")];
      if (um.isNull() || !events) return;
      const char* t = um[F("test")] | "";
      if (!*t) return;
      Event ev; ev.kind = (t[0] == 'a') ? LANDING : (t[0] == 'w') ? WATCH : DEPARTURE; ev.preset = 0;
      strlcpy(ev.callsign, "TEST", sizeof(ev.callsign)); strlcpy(ev.type, "-", sizeof(ev.type));
      xQueueSend(events, &ev, 0);
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char AdsbBlinker::_name[] PROGMEM = "ADS-B Blinker";
const char AdsbBlinker::_cacheFile[] PROGMEM = "/adsb.json";

static AdsbBlinker adsb_blinker;
REGISTER_USERMOD(adsb_blinker);
