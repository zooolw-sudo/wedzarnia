/*
  WĘDZARNIA ESP32
  Sterownik wędzarni: 4 sondy mięsa + sonda komory (NTC, np. Sonoff MPS01),
  grzałka 230 V przez przekaźnik SSR, dane i sterowanie przez WiFi
  (aplikacja „Wędzarnia” na tablecie albo przeglądarka).

  Podłączenie (schemat w README):
    S1..S4 (mięso) -> GPIO32, GPIO33, GPIO34, GPIO35
    K (komora)     -> GPIO36 (VP)
    każdy kanał:   3V3 -> 47 kΩ -> pin -> sonda -> GND, 100 nF pin–GND
    SSR            -> GPIO26 -> 1 kΩ -> baza BC547 (10 kΩ baza–GND), kolektor -> SSR 4−, SSR 3+ -> 5V

  Biblioteki: tylko wbudowane w pakiet ESP32 (WiFi, WebServer, ESPmDNS, Preferences).
  Płytka w Arduino IDE: „ESP32 Dev Module”.

  Bezpieczeństwo:
  - grzałka działa tylko w trakcie wędzenia (START) i tylko przy poprawnym odczycie komory,
  - przegrzanie komory, brak sondy komory przez 5 min, maks. czas -> grzałka OFF,
  - grzałka pracuje 20 min bez przerwy, a komora nie rośnie -> awaria (sonda komory poza komorą?),
  - zawieszony program -> watchdog restartuje ESP32,
  - po restarcie ESP32 proces NIE wznawia się sam (grzałka zostaje wyłączona),
  - sterowanie odbywa się w ESP32: utrata WiFi albo wyłączony tablet nie przerywa procesu,
  - termik NC w obwodzie 230 V jest i tak konieczny (zabezpieczenie niezależne od elektroniki).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>
#include <stdarg.h>

// ================== SPRZĘT ==================
static const uint8_t PIN_PROBE[5] = {32, 33, 34, 35, 36};  // S1..S4, K
static const uint8_t PIN_HEATER = 26;   // wejście sterownika SSR (stan wysoki = grzałka ON)
static const uint8_t PIN_LED = 2;       // dioda na płytce: świeci = grzałka ON, miga = brak WiFi
static const uint8_t PIN_BOOT = 0;      // przycisk BOOT: przytrzymaj 5 s = konfiguracja WiFi od nowa

static const float R_FIX = 47000.0f;    // rezystor dzielnika (ok. połowa rezystancji sondy w 25 °C)
static const float VCC = 3.30f;         // napięcie zasilania dzielnika

// ================== SIEĆ ==================
static const char *HOSTNAME = "wedzarnia";        // http://wedzarnia.local
static const char *AP_SSID = "Wedzarnia-ESP32";   // własna sieć, gdy nie ma połączenia z domowym WiFi
static const char *AP_PASS = "wedzarnia";

// ================== REGULACJA ==================
static const uint16_t MIN_ON_GAP_S = 10;     // najkrótsza przerwa przed ponownym włączeniem grzałki
static const uint16_t K_FAULT_LIMIT_S = 300; // brak odczytu komory dłużej niż 5 min = przerwanie
static const uint16_t NO_RISE_LIMIT_S = 1200; // grzałka pracuje 20 min bez przerwy, a komora nie rośnie = awaria
static const float NO_RISE_MIN_C = 2.0f;      // …o co najmniej tyle stopni
static const int HIST_N = 720;               // historia: 720 punktów co minutę = 12 h

#define NP 5   // liczba sond
#define KI 4   // indeks sondy komory

enum State : uint8_t { ST_IDLE, ST_RUN, ST_DONE, ST_FAULT };
static const char *STATE_NAME[] = {"idle", "run", "done", "fault"};

struct Probe {
  float v = 0;          // napięcie na pinie [V]
  float r = NAN;        // rezystancja sondy [Ω] (filtrowana)
  float t = NAN;        // temperatura [°C]
  uint8_t err = 1;      // 0 OK, 1 brak sondy, 2 zwarcie, 3 poza zakresem
  uint8_t badCnt = 0;   // kolejne złe odczyty (odporność na zakłócenia)
  // kalibracja: krzywa beta przechodząca przez punkt (tref, rref)
  float tref = 25.0f, rref = 100000.0f, beta = 3950.0f;
  float c1t = NAN, c1r = NAN;  // pierwszy punkt kalibracji (do wyliczenia beta z drugiego)
};
Probe pr[NP];

struct Cfg {
  float kSet = 70.0f;      // zadana temperatura komory
  float hyst = 2.0f;       // histereza regulacji
  float kMax = 140.0f;     // przegrzanie komory -> awaria
  float target[4] = {70, 70, 70, 70};  // docelowa temp. mięsa; 0 = sonda tylko do podglądu
  uint8_t endMode = 0;     // 0 = koniec, gdy wszystkie sondy dojdą; 1 = gdy dowolna dojdzie
  uint16_t maxMin = 720;   // maksymalny czas procesu [min]
} cfg;

Preferences prefs;
WebServer server(80);

State st = ST_IDLE;
char msg[112] = "";
bool heater = false;
uint32_t lastOnMs = 0;       // kiedy grzałka ostatnio się WYŁĄCZYŁA (do przerwy MIN_ON_GAP_S)
uint32_t runStartMs = 0;
uint32_t runEndSec = 0;      // czas trwania zakończonego procesu
bool req[4] = {false, false, false, false};  // sondy, na które czekamy
bool hit[4] = {false, false, false, false};  // sondy, które doszły do celu
uint16_t kBadSec = 0;
uint32_t heatOnSince = 0;     // od kiedy grzałka pracuje bez przerwy
float heatOnTemp = NAN;       // temperatura komory w chwili włączenia
uint32_t evNo = 0;
char evTxt[112] = "";

// historia co minutę: temperatury ×10 (INT16_MIN = brak) + udział pracy grzałki w %
int16_t hist[HIST_N][NP];
uint8_t histH[HIST_N];
int histCount = 0, histHead = 0;
uint32_t lastHistMs = 0;
uint16_t minuteSec = 0, minuteOnSec = 0;

// WiFi
String wifiSsid, wifiPass;
bool staOk = false, apOn = false;
uint32_t staLostMs = 0, apOffAtMs = 0;

// ================== POMOCNICZE ==================
static void setHeater(bool on) {
  if (on == heater) return;
  heater = on;
  digitalWrite(PIN_HEATER, on ? HIGH : LOW);
  if (!on) lastOnMs = millis();
  else { heatOnSince = millis(); heatOnTemp = pr[KI].t; }
}

static void event(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(evTxt, sizeof(evTxt), fmt, a);
  va_end(a);
  evNo++;
  Serial.printf("[zdarzenie %lu] %s\n", (unsigned long)evNo, evTxt);
}

static uint32_t runSeconds() {
  if (st == ST_RUN) return (millis() - runStartMs) / 1000UL;
  return runEndSec;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ================== PAMIĘĆ NIEULOTNA ==================
static void loadCfg() {
  prefs.begin("cfg", true);
  cfg.kSet = prefs.getFloat("kSet", cfg.kSet);
  cfg.hyst = prefs.getFloat("hyst", cfg.hyst);
  cfg.kMax = prefs.getFloat("kMax", cfg.kMax);
  for (int i = 0; i < 4; i++) {
    char k[4] = {'t', char('1' + i), 0};
    cfg.target[i] = prefs.getFloat(k, cfg.target[i]);
  }
  cfg.endMode = prefs.getUChar("end", cfg.endMode);
  cfg.maxMin = prefs.getUShort("maxMin", cfg.maxMin);
  prefs.end();

  prefs.begin("cal", true);
  for (int i = 0; i < NP; i++) {
    char k[8];
    snprintf(k, sizeof(k), "tr%d", i); pr[i].tref = prefs.getFloat(k, pr[i].tref);
    snprintf(k, sizeof(k), "rr%d", i); pr[i].rref = prefs.getFloat(k, pr[i].rref);
    snprintf(k, sizeof(k), "b%d", i);  pr[i].beta = prefs.getFloat(k, pr[i].beta);
    snprintf(k, sizeof(k), "ct%d", i); pr[i].c1t = prefs.getFloat(k, NAN);
    snprintf(k, sizeof(k), "cr%d", i); pr[i].c1r = prefs.getFloat(k, NAN);
  }
  prefs.end();
}

static void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putFloat("kSet", cfg.kSet);
  prefs.putFloat("hyst", cfg.hyst);
  prefs.putFloat("kMax", cfg.kMax);
  for (int i = 0; i < 4; i++) {
    char k[4] = {'t', char('1' + i), 0};
    prefs.putFloat(k, cfg.target[i]);
  }
  prefs.putUChar("end", cfg.endMode);
  prefs.putUShort("maxMin", cfg.maxMin);
  prefs.end();
}

static void saveCal() {
  prefs.begin("cal", false);
  for (int i = 0; i < NP; i++) {
    char k[8];
    snprintf(k, sizeof(k), "tr%d", i); prefs.putFloat(k, pr[i].tref);
    snprintf(k, sizeof(k), "rr%d", i); prefs.putFloat(k, pr[i].rref);
    snprintf(k, sizeof(k), "b%d", i);  prefs.putFloat(k, pr[i].beta);
    snprintf(k, sizeof(k), "ct%d", i); prefs.putFloat(k, pr[i].c1t);
    snprintf(k, sizeof(k), "cr%d", i); prefs.putFloat(k, pr[i].c1r);
  }
  prefs.end();
}

// ================== POMIAR ==================
static float ntcTemp(const Probe &p, float r) {
  float invT = 1.0f / (p.tref + 273.15f) + logf(r / p.rref) / p.beta;
  return 1.0f / invT - 273.15f;
}

static void readProbes() {
  for (int i = 0; i < NP; i++) {
    uint32_t mv = 0;
    for (int s = 0; s < 32; s++) mv += analogReadMilliVolts(PIN_PROBE[i]);
    float v = mv / 32.0f / 1000.0f;
    Probe &p = pr[i];
    p.v = v;

    uint8_t e = 0;
    float r = NAN, t = NAN;
    if (v > 3.08f) e = 1;            // obwód otwarty: brak sondy albo przerwany przewód
    else if (v < 0.05f) e = 2;       // zwarcie
    else {
      r = R_FIX * v / (VCC - v);
      float rf = (p.err == 0 && !isnan(p.r)) ? p.r + 0.3f * (r - p.r) : r;   // filtr
      t = ntcTemp(p, rf);
      if (isnan(t) || t < -30.0f || t > 320.0f) e = 3;
      r = rf;
    }

    if (e == 0) {
      p.err = 0; p.badCnt = 0; p.r = r; p.t = t;
    } else if (p.err == 0 && ++p.badCnt < 3) {
      // pojedynczy zły odczyt ignorujemy (zostaje poprzednia wartość)
    } else {
      p.err = e; p.badCnt = 3; p.t = NAN;
      p.r = (e == 3) ? R_FIX * v / (VCC - v) : NAN;
    }
  }
}

// ================== HISTORIA ==================
static void histClear() { histCount = 0; histHead = 0; minuteSec = minuteOnSec = 0; }

static void histPush() {
  for (int i = 0; i < NP; i++)
    hist[histHead][i] = pr[i].err == 0 ? (int16_t)lroundf(pr[i].t * 10.0f) : INT16_MIN;
  histH[histHead] = minuteSec ? (uint8_t)(minuteOnSec * 100UL / minuteSec) : (heater ? 100 : 0);
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
  minuteSec = minuteOnSec = 0;
  lastHistMs = millis();
}

// ================== PROCES ==================
static void finish(State s, const char *fmt, ...) {
  runEndSec = runSeconds();
  st = s;
  setHeater(false);
  va_list a;
  va_start(a, fmt);
  vsnprintf(msg, sizeof(msg), fmt, a);
  va_end(a);
  event("%s", msg);
  histPush();
}

static bool startProcess(char *err, size_t errLen) {
  if (st == ST_RUN) { snprintf(err, errLen, "Wędzenie już trwa"); return false; }
  if (pr[KI].err != 0) { snprintf(err, errLen, "Podłącz sondę komory (K) – bez niej grzałka nie ruszy"); return false; }
  for (int i = 0; i < 4; i++) {
    hit[i] = false;
    req[i] = cfg.target[i] > 0 && pr[i].err == 0;
  }
  kBadSec = 0;
  msg[0] = 0;
  runStartMs = millis();
  runEndSec = 0;
  st = ST_RUN;
  histClear();
  histPush();
  event("Start wędzenia: komora %.0f °C", cfg.kSet);
  return true;
}

static void control1s() {
  minuteSec++;
  if (heater) minuteOnSec++;
  if (millis() - lastHistMs >= 60000UL) histPush();

  if (st != ST_RUN) { setHeater(false); return; }

  Probe &k = pr[KI];

  // sondy mięsa
  int nReq = 0, nHit = 0;
  for (int i = 0; i < 4; i++) {
    if (cfg.target[i] <= 0) { req[i] = false; hit[i] = false; continue; }
    if (pr[i].err == 0) req[i] = true;   // sonda podłączona w trakcie też się liczy
    if (req[i] && !hit[i] && pr[i].err == 0 && pr[i].t >= cfg.target[i]) {
      hit[i] = true;
      event("Sonda S%d osiągnęła %.0f °C", i + 1, cfg.target[i]);
    }
    if (req[i]) { nReq++; if (hit[i]) nHit++; }
  }

  bool done = cfg.endMode == 0 ? (nReq > 0 && nHit == nReq) : (nHit > 0);
  if (done) {
    finish(ST_DONE, "Mięso osiągnęło zadaną temperaturę – grzałka wyłączona");
    return;
  }
  if (runSeconds() >= (uint32_t)cfg.maxMin * 60UL) {
    finish(ST_DONE, "Minął maksymalny czas wędzenia (%u min) – grzałka wyłączona", cfg.maxMin);
    return;
  }
  if (k.err == 0 && k.t > cfg.kMax) {
    finish(ST_FAULT, "Przegrzanie komory (%.0f °C) – grzałka wyłączona", k.t);
    return;
  }
  if (k.err != 0) {
    if (++kBadSec >= K_FAULT_LIMIT_S) {
      finish(ST_FAULT, "Brak odczytu z sondy komory przez 5 min – proces przerwany");
      return;
    }
  } else {
    kBadSec = 0;
  }

  // regulacja komory z histerezą
  if (k.err != 0) { setHeater(false); return; }

  // grzałka grzeje długo, a komora stoi daleko poniżej zadanej: sonda komory wypadła na zewnątrz
  // albo grzałka nie działa
  if (heater && millis() - heatOnSince >= NO_RISE_LIMIT_S * 1000UL) {
    if (k.t < cfg.kSet - 15.0f && !(k.t - heatOnTemp >= NO_RISE_MIN_C)) {
      finish(ST_FAULT, "Grzałka pracuje 20 min, a komora się nie nagrzewa – sprawdź sondę komory i grzałkę");
      return;
    }
    heatOnSince = millis(); heatOnTemp = k.t;   // sprawdzamy kolejne 20 min
  }

  float lo = cfg.kSet - cfg.hyst / 2.0f, hi = cfg.kSet + cfg.hyst / 2.0f;
  if (heater && k.t >= hi) setHeater(false);
  else if (!heater && k.t <= lo && millis() - lastOnMs >= MIN_ON_GAP_S * 1000UL) setHeater(true);
}

// ================== JSON ==================
static char jb[6144];
static size_t jl = 0;
static void J(const char *fmt, ...) {
  if (jl >= sizeof(jb) - 1) return;
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(jb + jl, sizeof(jb) - jl, fmt, a);
  va_end(a);
  if (n > 0) jl = (jl + n < sizeof(jb) - 1) ? jl + n : sizeof(jb) - 1;
}
static void Jf(float v, const char *fmt = "%.1f") {
  if (isnan(v)) J("null"); else J(fmt, v);
}
static void Jstr(const char *s) {   // napis z ucieczką cudzysłowów
  J("\"");
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') J("\\%c", *s);
    else if ((uint8_t)*s >= 0x20) J("%c", *s);
  }
  J("\"");
}

static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Cache-Control", "no-store");
}
static void sendJb(int code = 200) {
  cors();
  server.send(code, "application/json; charset=utf-8", jb);
}
static void sendResult(bool ok, const char *text) {
  jl = 0;
  J("{\"ok\":%d,\"msg\":", ok ? 1 : 0); Jstr(text); J("}");
  sendJb(ok ? 200 : 400);
}

static void buildState() {
  jl = 0;
  Probe &k = pr[KI];
  J("{\"v\":1,\"st\":\"%s\",\"msg\":", STATE_NAME[st]); Jstr(msg);
  J(",\"heat\":%d,\"run\":%lu,\"up\":%lu", heater ? 1 : 0, (unsigned long)runSeconds(), (unsigned long)(millis() / 1000UL));
  J(",\"end\":%u,\"maxMin\":%u,\"ev\":%lu,\"evt\":", cfg.endMode, cfg.maxMin, (unsigned long)evNo); Jstr(evTxt);
  J(",\"k\":{\"t\":"); Jf(k.t);
  J(",\"err\":%u,\"set\":", k.err); Jf(cfg.kSet);
  J(",\"hyst\":"); Jf(cfg.hyst);
  J(",\"max\":"); Jf(cfg.kMax, "%.0f");
  J(",\"r\":"); Jf(k.r, "%.0f");
  J(",\"bad\":%u}", kBadSec);
  J(",\"p\":[");
  for (int i = 0; i < 4; i++) {
    if (i) J(",");
    J("{\"t\":"); Jf(pr[i].t);
    J(",\"err\":%u,\"tg\":", pr[i].err); Jf(cfg.target[i], "%.0f");
    J(",\"req\":%d,\"hit\":%d,\"r\":", req[i] ? 1 : 0, hit[i] ? 1 : 0); Jf(pr[i].r, "%.0f");
    J("}");
  }
  J("],\"wifi\":{\"sta\":%d,\"ap\":%d,\"rssi\":%d,\"ip\":\"%s\",\"ssid\":",
    staOk ? 1 : 0, apOn ? 1 : 0, staOk ? (int)WiFi.RSSI() : 0,
    staOk ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
  Jstr(wifiSsid.c_str());
  J("}}");
}

// ================== HTTP: API ==================
static void handleState() { buildState(); sendJb(); }

static void handleHistory() {
  cors();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
  char buf[1400];
  size_t n = snprintf(buf, sizeof(buf), "{\"step\":60,\"age\":%lu,\"run\":%lu,\"d\":[",
                      (unsigned long)((millis() - lastHistMs) / 1000UL), (unsigned long)runSeconds());
  int start = (histHead - histCount + HIST_N) % HIST_N;
  for (int j = 0; j < histCount; j++) {
    int idx = (start + j) % HIST_N;
    char row[64];
    size_t m = 0;
    m += snprintf(row + m, sizeof(row) - m, "%s[", j ? "," : "");
    for (int i = 0; i < NP; i++) {
      if (hist[idx][i] == INT16_MIN) m += snprintf(row + m, sizeof(row) - m, "null,");
      else m += snprintf(row + m, sizeof(row) - m, "%d,", hist[idx][i]);
    }
    m += snprintf(row + m, sizeof(row) - m, "%u]", histH[idx]);
    if (n + m >= sizeof(buf) - 4) { server.sendContent(buf, n); n = 0; }
    memcpy(buf + n, row, m);
    n += m;
  }
  n += snprintf(buf + n, sizeof(buf) - n, "]}");
  server.sendContent(buf, n);
  server.sendContent("");
}

static void handleCmd() {
  String a = server.arg("do");
  char err[112];
  if (a == "start") {
    if (startProcess(err, sizeof(err))) sendResult(true, "Wędzenie rozpoczęte");
    else sendResult(false, err);
  } else if (a == "stop") {
    if (st == ST_RUN) {
      runEndSec = runSeconds();
      st = ST_IDLE;
      setHeater(false);
      snprintf(msg, sizeof(msg), "Zatrzymane ręcznie");
      event("Wędzenie zatrzymane ręcznie");
      histPush();
    }
    sendResult(true, "Zatrzymane – grzałka wyłączona");
  } else if (a == "ack") {
    if (st == ST_DONE || st == ST_FAULT) { st = ST_IDLE; msg[0] = 0; }
    sendResult(true, "OK");
  } else {
    sendResult(false, "Nieznane polecenie");
  }
}

static void handleSet() {
  bool any = false;
  if (server.hasArg("k"))      { cfg.kSet = clampf(server.arg("k").toFloat(), 20, 200); any = true; }
  if (server.hasArg("h"))      { cfg.hyst = clampf(server.arg("h").toFloat(), 0.5f, 20); any = true; }
  if (server.hasArg("kmax"))   { cfg.kMax = clampf(server.arg("kmax").toFloat(), 50, 250); any = true; }
  if (server.hasArg("end"))    { cfg.endMode = server.arg("end").toInt() ? 1 : 0; any = true; }
  if (server.hasArg("maxmin")) { cfg.maxMin = (uint16_t)clampf(server.arg("maxmin").toInt(), 10, 2880); any = true; }
  for (int i = 0; i < 4; i++) {
    char k[3] = {'t', char('1' + i), 0};
    if (server.hasArg(k)) {
      float t = server.arg(k).toFloat();
      cfg.target[i] = t <= 0 ? 0 : clampf(t, 30, 150);
      if (cfg.target[i] <= 0 || pr[i].err != 0 || pr[i].t < cfg.target[i]) hit[i] = false;  // podniesiony cel
      any = true;
    }
  }
  if (cfg.kMax < cfg.kSet + 10) cfg.kMax = cfg.kSet + 10;
  if (any) saveCfg();
  buildState();
  sendJb();
}

// kalibracja: /api/cal?t=100  (wszystkie podłączone sondy w tej samej wodzie)
//             /api/cal?t=0&p=2 (tylko S3; p=0..3 mięso, 4 komora)
//             /api/cal?reset=1
static void handleCal() {
  if (server.hasArg("reset")) {
    for (int i = 0; i < NP; i++) {
      pr[i].tref = 25; pr[i].rref = 100000; pr[i].beta = 3950; pr[i].c1t = NAN; pr[i].c1r = NAN;
    }
    saveCal();
    sendResult(true, "Kalibracja przywrócona do ustawień fabrycznych");
    return;
  }
  if (server.hasArg("t")) {
    if (st == ST_RUN) { sendResult(false, "Kalibracja niedostępna w trakcie wędzenia"); return; }
    float T = server.arg("t").toFloat();
    int only = server.hasArg("p") ? server.arg("p").toInt() : -1;
    char done[64] = "", bad[96] = "";
    for (int i = 0; i < NP; i++) {
      if (only >= 0 && i != only) continue;
      Probe &p = pr[i];
      char name[4];
      snprintf(name, sizeof(name), i == KI ? "K" : "S%d", i + 1);
      if (p.err != 0 || isnan(p.r)) continue;
      if (p.v > 3.0f || p.v < 0.12f) {  // poza zakresem, w którym ADC mierzy wiarygodnie
        strncat(bad, " ", sizeof(bad) - strlen(bad) - 1);
        strncat(bad, name, sizeof(bad) - strlen(bad) - 1);
        continue;
      }
      float r = p.r;
      if (isnan(p.c1t) || fabsf(T - p.c1t) < 15.0f) {
        // pierwszy punkt (albo powtórzony): przesuwa krzywą, beta bez zmian
        p.tref = T; p.rref = r; p.c1t = T; p.c1r = r;
      } else {
        // drugi punkt: wylicza beta z dwóch temperatur
        float b = logf(p.c1r / r) / (1.0f / (p.c1t + 273.15f) - 1.0f / (T + 273.15f));
        if (b < 2000 || b > 6000 || isnan(b)) {
          strncat(bad, " ", sizeof(bad) - strlen(bad) - 1);
          strncat(bad, name, sizeof(bad) - strlen(bad) - 1);
          continue;
        }
        p.beta = b; p.tref = p.c1t; p.rref = p.c1r;
      }
      strncat(done, " ", sizeof(done) - strlen(done) - 1);
      strncat(done, name, sizeof(done) - strlen(done) - 1);
    }
    saveCal();
    char out[320];
    if (done[0] && !bad[0]) snprintf(out, sizeof(out), "Zapisano punkt %.1f °C dla:%s", T, done);
    else if (done[0]) snprintf(out, sizeof(out), "Zapisano %.1f °C dla:%s. Pominięte (zły odczyt):%s", T, done, bad);
    else if (bad[0]) snprintf(out, sizeof(out), "Nic nie zapisano – odczyt poza zakresem:%s", bad);
    else snprintf(out, sizeof(out), "Nic nie zapisano – brak podłączonych sond");
    sendResult(done[0] != 0, out);
    return;
  }
  // bez parametrów: aktualne dane kalibracji
  jl = 0;
  J("{\"rfix\":%.0f,\"p\":[", R_FIX);
  for (int i = 0; i < NP; i++) {
    if (i) J(",");
    J("{\"tref\":"); Jf(pr[i].tref);
    J(",\"rref\":"); Jf(pr[i].rref, "%.0f");
    J(",\"beta\":"); Jf(pr[i].beta, "%.0f");
    J(",\"c1\":"); Jf(pr[i].c1t);
    J(",\"r\":"); Jf(pr[i].r, "%.0f");
    J(",\"v\":%.3f,\"err\":%u}", pr[i].v, pr[i].err);
  }
  J("]}");
  sendJb();
}

// ================== HTTP: STRONY ==================
static const char PAGE_HOME[] PROGMEM = R"HTML(<!doctype html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Wędzarnia</title>
<style>body{font:16px system-ui,sans-serif;margin:0;padding:16px;background:#0d121b;color:#e7ebf2}
h1{font-size:20px;margin:0 0 12px}.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px}
.c{background:#151c29;border:1px solid #253044;border-radius:10px;padding:12px}.t{font-size:32px;font-weight:700}
.m{color:#93a0b6;font-size:13px}a{color:#f0a53a}</style></head><body>
<h1>Wędzarnia ESP32</h1><div id="s" class="m">Łączenie…</div><div class="g" id="g"></div>
<p class="m">Pełne sterowanie: aplikacja „Wędzarnia”. <a href="/wifi">Ustawienia WiFi</a> · <a href="/api/state">JSON</a></p>
<script>
const N=['S1','S2','S3','S4'];
function f(t){return t==null?'—':t.toFixed(1)+' °C'}
async function u(){try{const d=await(await fetch('/api/state')).json();
const st={idle:'Czeka',run:'Wędzenie trwa',done:'Zakończone',fault:'Awaria'}[d.st];
document.getElementById('s').textContent=st+(d.msg?' – '+d.msg:'')+' · grzałka '+(d.heat?'WŁ':'WYŁ');
let h='<div class="c"><div class="m">Komora (zadana '+d.k.set+' °C)</div><div class="t">'+f(d.k.t)+'</div></div>';
d.p.forEach((p,i)=>{h+='<div class="c"><div class="m">'+N[i]+' (cel '+(p.tg>0?p.tg+' °C':'—')+')</div><div class="t">'+f(p.t)+'</div></div>'});
document.getElementById('g').innerHTML=h;}catch(e){document.getElementById('s').textContent='Brak połączenia'}}
u();setInterval(u,2000);
</script></body></html>)HTML";

static void handleHome() {
  server.send(200, "text/html; charset=utf-8", PAGE_HOME);
}

static String htmlEsc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static void handleWifiPage() {
  String h = F("<!doctype html><html lang='pl'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi – Wędzarnia</title>"
               "<style>body{font:16px system-ui,sans-serif;margin:0;padding:16px;background:#0d121b;color:#e7ebf2}"
               "input,select,button{font:inherit;width:100%;padding:10px;margin:6px 0 14px;border-radius:8px;border:1px solid #253044;background:#151c29;color:#e7ebf2;box-sizing:border-box}"
               "button{background:#f0a53a;color:#131926;border:0;font-weight:600}.m{color:#93a0b6;font-size:13px}</style></head><body>"
               "<h1 style='font-size:20px'>WiFi wędzarni</h1>");
  if (st == ST_RUN) {
    h += F("<p>Zmiana WiFi jest zablokowana w trakcie wędzenia (wymaga restartu ESP32).</p></body></html>");
    server.send(200, "text/html; charset=utf-8", h);
    return;
  }
  h += "<p class='m'>Obecnie: ";
  h += staOk ? ("połączono z „" + htmlEsc(wifiSsid) + "”, IP " + WiFi.localIP().toString()) : String("brak połączenia z domowym WiFi");
  h += F("</p><form method='post' action='/wifi'><label>Sieć</label><select name='s' onchange=\"document.getElementById('o').value=this.value\">");
  feedLoopWDT();
  int n = WiFi.scanNetworks(false, false, false, 150);
  feedLoopWDT();
  for (int i = 0; i < n && i < 20; i++) {
    String s = htmlEsc(WiFi.SSID(i));
    h += "<option value=\"" + s + "\">" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  h += F("</select><label>…albo wpisz nazwę</label><input id='o' name='ssid' value='");
  h += htmlEsc(n > 0 ? WiFi.SSID(0) : wifiSsid);
  h += F("'><label>Hasło</label><input name='pass' type='password'>"
         "<button>Zapisz i uruchom ponownie</button></form>"
         "<p class='m'>Po restarcie ESP32 połączy się z wybraną siecią. Jeśli się nie uda, znów pojawi się sieć "
         "„Wedzarnia-ESP32” (hasło: wedzarnia), a ta strona będzie pod adresem 192.168.4.1/wifi.</p></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleWifiSave() {
  if (st == ST_RUN) { server.send(409, "text/plain; charset=utf-8", "W trakcie wędzenia nie można zmienić WiFi"); return; }
  String s = server.arg("ssid");
  if (s.length() == 0) s = server.arg("s");
  s.trim();
  if (s.length() == 0) { server.send(400, "text/plain; charset=utf-8", "Podaj nazwę sieci"); return; }
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><body style='font:16px sans-serif;background:#0d121b;color:#e7ebf2;padding:16px'>"
              "Zapisano. ESP32 uruchamia się ponownie i łączy z „" + htmlEsc(s) + "”…</body>");
  delay(800);
  ESP.restart();
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
  cors();
  server.send(404, "text/plain; charset=utf-8", "Nie ma takiej strony");
}

// ================== WIFI ==================
static void startAP() {
  if (apOn) return;
  WiFi.mode(wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apOn = true;
  Serial.printf("Sieć konfiguracyjna „%s” (hasło %s), adres %s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

static void stopAP() {
  if (!apOn) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  Serial.println("Sieć konfiguracyjna wyłączona (jest połączenie z domowym WiFi)");
}

static void wifiBegin() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  staLostMs = millis();
  if (wifiSsid.length()) {
    Serial.printf("Łączę z WiFi „%s”…\n", wifiSsid.c_str());
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  } else {
    startAP();
  }
}

static void wifiLoop() {
  bool c = wifiSsid.length() && WiFi.status() == WL_CONNECTED;
  if (c && !staOk) {
    staOk = true;
    Serial.printf("WiFi OK, IP %s, http://%s.local\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addServiceTxt("http", "tcp", "app", "wedzarnia");
    }
    apOffAtMs = millis() + 60000UL;
  } else if (!c && staOk) {
    staOk = false;
    staLostMs = millis();
    Serial.println("Utracono WiFi – sterowanie działa dalej, ponawiam połączenie");
  }
  if (staOk && apOn && (int32_t)(millis() - apOffAtMs) > 0 && WiFi.softAPgetStationNum() == 0) stopAP();
  if (!staOk && !apOn && millis() - staLostMs > 30000UL) startAP();   // własna sieć jako zapas
}

// ================== SETUP / LOOP ==================
void setup() {
  pinMode(PIN_HEATER, OUTPUT);
  digitalWrite(PIN_HEATER, LOW);   // grzałka wyłączona od pierwszej chwili
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Wędzarnia ESP32 ===");

  loadCfg();
  for (int i = 0; i < 6; i++) { readProbes(); delay(20); }
  histClear();
  lastHistMs = millis();
  histPush();
  snprintf(evTxt, sizeof(evTxt), "Sterownik uruchomiony");

  wifiBegin();

  server.on("/", HTTP_GET, handleHome);
  server.on("/api/state", HTTP_ANY, handleState);
  server.on("/api/history", HTTP_ANY, handleHistory);
  server.on("/api/cmd", HTTP_ANY, handleCmd);
  server.on("/api/set", HTTP_ANY, handleSet);
  server.on("/api/cal", HTTP_ANY, handleCal);
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);
  server.begin();

  enableLoopWDT();   // zawieszony program = restart ESP32 (a po restarcie grzałka jest wyłączona)
}

void loop() {
  static uint32_t tRead = 0, tCtl = 0, tLog = 0, bootDown = 0;
  uint32_t now = millis();

  server.handleClient();

  if (now - tRead >= 500) { tRead = now; readProbes(); }
  if (now - tCtl >= 1000) { tCtl = now; control1s(); wifiLoop(); }

  // dioda: grzałka ON = świeci, brak WiFi = miga
  if (heater) digitalWrite(PIN_LED, HIGH);
  else if (!staOk) digitalWrite(PIN_LED, ((now / (apOn ? 150 : 500)) & 1) ? HIGH : LOW);
  else digitalWrite(PIN_LED, LOW);

  // BOOT przytrzymany 5 s (poza wędzeniem) = kasowanie WiFi i sieć konfiguracyjna
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!bootDown) bootDown = now;
    if (now - bootDown > 5000 && st != ST_RUN) {
      prefs.begin("wifi", false);
      prefs.clear();
      prefs.end();
      Serial.println("WiFi skasowane – restart do trybu konfiguracji");
      delay(300);
      ESP.restart();
    }
  } else {
    bootDown = 0;
  }

  if (now - tLog >= 5000) {
    tLog = now;
    Serial.printf("[%s] K=%.1f (zad. %.0f) S1=%.1f S2=%.1f S3=%.1f S4=%.1f grzałka=%s\n",
                  STATE_NAME[st], pr[KI].t, cfg.kSet, pr[0].t, pr[1].t, pr[2].t, pr[3].t, heater ? "WŁ" : "WYŁ");
  }
}
