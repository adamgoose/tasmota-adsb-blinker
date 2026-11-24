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
#define ADSB_DOC_SIZE     24576     // filtered aircraft list; ~100 B per aircraft
#define ADSB_FORGET_MS    180000UL  // drop aircraft not seen for 3 minutes
#define ADSB_FLASH_ON_MS  150
#define ADSB_FLASH_GAP_MS 400

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
    enum Kind : uint8_t { DEPARTURE = 0, LANDING = 1 };
    enum Mode : uint8_t { MODE_FLASH = 0, MODE_PRESET = 1 };

  private:
    struct Event { uint8_t kind; char callsign[10]; char type[6]; };
    struct Track { bool ground; uint32_t seen; };
    // What the polling task needs, copied under the mutex
    struct Snapshot {
      bool enabled; char icao[8]; float lat, lon; int radius, pollSec, airKt; uint32_t gen;
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

    // ---- runtime (main loop only) ----
    uint32_t deps = 0, arrs = 0;
    char     lastEvent[40] = "";
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

    void startPreset(int p) {
      if (p <= 0 || p > 250) return;
      if (!presetActive && presetSec > 0) {      // first event: remember the current look
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
      if (presetSec > 0) { presetActive = true; presetUntil = millis() + presetSec * 1000UL; }
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
      xSemaphoreGive(mtx);
      return s;
    }

    // ---------- polling task ----------

    static void taskEntry(void* arg) { static_cast<AdsbBlinker*>(arg)->taskLoop(); }

    void taskLoop() {
      std::map<uint32_t, Track> tracks;
      uint32_t trackGen = UINT32_MAX;
      for (;;) {
        Snapshot s = snapshot();
        if (!s.enabled) { setStatus("disabled"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!WLED_CONNECTED) { setStatus("waiting for network"); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        if (isnan(s.lat)) {
          if (!lookupAirport(s.icao)) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }
          continue;  // re-snapshot with the new coordinates
        }
        if (s.gen != trackGen) { tracks.clear(); trackGen = s.gen; }

        uint32_t started = millis();
        pollAdsb(s, tracks);
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

    void pollAdsb(const Snapshot& s, std::map<uint32_t, Track>& tracks) {
      char path[80];
      snprintf(path, sizeof(path), "/api/v3/lat/%.5f/lon/%.5f/dist/%d", s.lat, s.lon, s.radius);
      // Keep only the fields we use; this also skips the 13-digit timestamps
      StaticJsonDocument<192> filter;
      JsonObject f = filter["ac"].createNestedObject();
      f["hex"] = true; f["flight"] = true; f["t"] = true; f["alt_baro"] = true; f["gs"] = true;
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
      JsonArray acs = doc["ac"];
      for (JsonObject a : acs) {
        const char* hex = a["hex"] | "";
        bool nonIcao = (*hex == '~');           // TIS-B / non-ICAO addresses
        uint32_t id = strtoul(hex + (nonIcao ? 1 : 0), nullptr, 16) | (nonIcao ? 0x1000000UL : 0);
        if (!id) continue;
        JsonVariant alt = a["alt_baro"];
        float gs = a["gs"] | 0.0f;
        int8_t ground = -1;                      // -1 = ambiguous, keep previous state
        if (alt.is<const char*>() && strcmp(alt.as<const char*>(), "ground") == 0) ground = 1;
        else if (alt.is<float>() && gs >= s.airKt) ground = 0;

        auto it = tracks.find(id);
        if (ground < 0) { if (it != tracks.end()) it->second.seen = now; continue; }
        if (it != tracks.end() && it->second.ground != (bool)ground) {
          Event ev; ev.kind = ground ? LANDING : DEPARTURE;
          const char* cs = a["flight"] | hex;
          strlcpy(ev.callsign, cs, sizeof(ev.callsign));
          for (int i = strlen(ev.callsign) - 1; i >= 0 && ev.callsign[i] == ' '; i--) ev.callsign[i] = 0;
          strlcpy(ev.type, a["t"] | "?", sizeof(ev.type));
          xQueueSend(events, &ev, 0);
        }
        tracks[id] = { (bool)ground, now };
      }
      for (auto it = tracks.begin(); it != tracks.end(); )
        it = (now - it->second.seen > ADSB_FORGET_MS) ? tracks.erase(it) : std::next(it);

      char msg[48]; snprintf(msg, sizeof(msg), "%s: %u aircraft", s.icao, (unsigned)acs.size());
      setStatus(msg);
    }

    // ---------- output (main loop) ----------

    void onEvent(const Event& ev) {
      if (ev.kind == DEPARTURE) deps++; else arrs++;
      snprintf(lastEvent, sizeof(lastEvent), "%s %s (%s)", ev.kind == DEPARTURE ? "dep" : "arr", ev.callsign, ev.type);
      DEBUG_PRINTF_P(PSTR("ADS-B: %s\n"), lastEvent);
      if (mode == MODE_PRESET) {
        startPreset(ev.kind == DEPARTURE ? depPreset : arrPreset);
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
    }

    void loop() override {
      Event ev;
      while (events && xQueueReceive(events, &ev, 0) == pdTRUE) onEvent(ev);
      runFlash();
      if (presetActive && ((int32_t)(millis() - presetUntil) >= 0 || mode != MODE_PRESET || !enabled)) endPreset();
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
      char line[96];
      xSemaphoreTake(mtx, portMAX_DELAY);
      snprintf(line, sizeof(line), "%s | dep %u arr %u%s%s", status, (unsigned)deps, (unsigned)arrs,
               lastEvent[0] ? " | last " : "", lastEvent);
      xSemaphoreGive(mtx);
      JsonArray arr = user.createNestedArray(F("ADS-B Blinker"));
      arr.add(line);
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
        "for(let f of['Flash:departure','Flash:landing'])I(f).type='color';"));
      oappend(F("let s=addDropdown('ADS-B Blinker','Action:type');addOption(s,'Flash the LEDs',0);addOption(s,'Apply a preset',1);"
        "let gf=G('Flash'),gp=G('Preset'),T=()=>{gf.style.display=s.value==0?'':'none';gp.style.display=s.value==1?'':'none';};"
        "s.addEventListener('change',T);T();"
        "let pd=['Preset:departure','Preset:landing'].map(f=>addDropdown('ADS-B Blinker',f));"
        "pd.forEach(x=>addOption(x,'None',0));"
        "fetch('/presets.json').then(r=>r.json()).then(j=>{for(let k in j)if(k>0&&j[k].n)pd.forEach(x=>addOption(x,k+': '+j[k].n,k));});"));
    }

    // Test from the JSON API: {"adsb":{"test":"dep"}} or {"adsb":{"test":"arr"}}
    // This runs in the web server's task while WLED is applying the request, so
    // only queue the event; loop() handles it like a real one.
    void readFromJsonState(JsonObject& root) override {
      JsonObject um = root[F("adsb")];
      if (um.isNull() || !events) return;
      const char* t = um[F("test")] | "";
      if (!*t) return;
      Event ev; ev.kind = (t[0] == 'a') ? LANDING : DEPARTURE;
      strlcpy(ev.callsign, "TEST", sizeof(ev.callsign)); strlcpy(ev.type, "-", sizeof(ev.type));
      xQueueSend(events, &ev, 0);
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char AdsbBlinker::_name[] PROGMEM = "ADS-B Blinker";
const char AdsbBlinker::_cacheFile[] PROGMEM = "/adsb.json";

static AdsbBlinker adsb_blinker;
REGISTER_USERMOD(adsb_blinker);
