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
    String  depColor   = "00FF00";
    String  arrColor   = "0000FF";
    int     flashCount = 3;
    int     flashLeds  = 0;        // 0 = every LED
    int     depPreset  = 0;
    int     arrPreset  = 0;
    // cached lookup, stored so a reboot needs no new lookup
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

    static const char _name[];

    static uint32_t parseHex(const String& s, uint32_t fallback) {
      if (s.length() != 6) return fallback;
      char* end; uint32_t v = strtoul(s.c_str(), &end, 16);
      return *end ? fallback : v;
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
        int p = ev.kind == DEPARTURE ? depPreset : arrPreset;
        if (p > 0 && p < 251) applyPreset(p);
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
      xTaskCreate(taskEntry, "adsb", ADSB_TASK_STACK, this, 1, &task);
    }

    void loop() override {
      Event ev;
      while (events && xQueueReceive(events, &ev, 0) == pdTRUE) onEvent(ev);
      runFlash();
      if (mtx && lookupDone) {
        xSemaphoreTake(mtx, portMAX_DELAY); lookupDone = false; xSemaphoreGive(mtx);
        configNeedsWrite = true;     // persist the resolved coordinates
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

    void addToConfig(JsonObject& root) override {
      if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);   // the task may be writing lat/lon
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[F("enabled")]     = enabled;
      top[F("airport")]     = airport;
      top[F("radiusNm")]    = radiusNm;
      top[F("pollSec")]     = pollSec;
      top[F("airborneKt")]  = airKt;
      top[F("mode")]        = mode;
      top[F("depColor")]    = depColor;
      top[F("arrColor")]    = arrColor;
      top[F("flashCount")]  = flashCount;
      top[F("flashLeds")]   = flashLeds;
      top[F("depPreset")]   = depPreset;
      top[F("arrPreset")]   = arrPreset;
      top[F("resolvedIcao")] = resolvedIcao;
      top[F("lat")]         = lat;
      top[F("lon")]         = lon;
      if (mtx) xSemaphoreGive(mtx);
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool complete = !top.isNull();
      if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
      String oldAirport = airport; int oldRadius = radiusNm;
      complete &= getJsonValue(top[F("enabled")],    enabled, false);
      complete &= getJsonValue(top[F("airport")],    airport, String("KMSP"));
      complete &= getJsonValue(top[F("radiusNm")],   radiusNm, 3);
      complete &= getJsonValue(top[F("pollSec")],    pollSec, 10);
      complete &= getJsonValue(top[F("airborneKt")], airKt, 60);
      complete &= getJsonValue(top[F("mode")],       mode, (uint8_t)MODE_FLASH);
      complete &= getJsonValue(top[F("depColor")],   depColor, String("00FF00"));
      complete &= getJsonValue(top[F("arrColor")],   arrColor, String("0000FF"));
      complete &= getJsonValue(top[F("flashCount")], flashCount, 3);
      complete &= getJsonValue(top[F("flashLeds")],  flashLeds, 0);
      complete &= getJsonValue(top[F("depPreset")],  depPreset, 0);
      complete &= getJsonValue(top[F("arrPreset")],  arrPreset, 0);
      getJsonValue(top[F("resolvedIcao")], resolvedIcao, String(""));
      if (!top[F("lat")].isNull()) lat = top[F("lat")].as<float>();
      if (!top[F("lon")].isNull()) lon = top[F("lon")].as<float>();
      airport.trim(); airport.toUpperCase();
      depColor.trim(); depColor.toUpperCase(); if (depColor.startsWith("#")) depColor.remove(0, 1);
      arrColor.trim(); arrColor.toUpperCase(); if (arrColor.startsWith("#")) arrColor.remove(0, 1);
      radiusNm = constrain(radiusNm, 1, 250);
      pollSec  = constrain(pollSec, 2, 3600);
      flashCount = constrain(flashCount, 1, 20);
      if (airport != oldAirport || radiusNm != oldRadius) gen++;
      if (mtx) xSemaphoreGive(mtx);
      return complete;
    }

    void appendConfigData() override {
      oappend(F("addInfo('ADS-B Blinker:airport',1,'ICAO code, e.g. KMSP');"));
      oappend(F("addInfo('ADS-B Blinker:radiusNm',1,'NM around the airport (keep small: runways + short final)');"));
      oappend(F("addInfo('ADS-B Blinker:pollSec',1,'seconds between polls (adsb.fi limit: 1 req/s)');"));
      oappend(F("addInfo('ADS-B Blinker:airborneKt',1,'ground speed that counts as airborne');"));
      oappend(F("dd=addDropdown('ADS-B Blinker','mode');"));
      oappend(F("addOption(dd,'Flash overlay',0);"));
      oappend(F("addOption(dd,'Apply presets',1);"));
      oappend(F("addInfo('ADS-B Blinker:depColor',1,'hex RRGGBB (flash mode)');"));
      oappend(F("addInfo('ADS-B Blinker:arrColor',1,'hex RRGGBB (flash mode)');"));
      oappend(F("addInfo('ADS-B Blinker:flashLeds',1,'LEDs to flash, 0 = all');"));
      oappend(F("addInfo('ADS-B Blinker:depPreset',1,'preset ID (preset mode, 0 = none)');"));
      oappend(F("addInfo('ADS-B Blinker:arrPreset',1,'preset ID (preset mode, 0 = none)');"));
      oappend(F("addInfo('ADS-B Blinker:resolvedIcao',1,'set automatically');"));
    }

    // Test from the JSON API: {"adsb":{"test":"dep"}} or {"adsb":{"test":"arr"}}
    void readFromJsonState(JsonObject& root) override {
      JsonObject um = root[F("adsb")];
      if (um.isNull()) return;
      const char* t = um[F("test")] | "";
      if (!*t) return;
      Event ev; ev.kind = (t[0] == 'a') ? LANDING : DEPARTURE;
      strlcpy(ev.callsign, "TEST", sizeof(ev.callsign)); strlcpy(ev.type, "-", sizeof(ev.type));
      onEvent(ev);
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char AdsbBlinker::_name[] PROGMEM = "ADS-B Blinker";

static AdsbBlinker adsb_blinker;
REGISTER_USERMOD(adsb_blinker);
