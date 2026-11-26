#include "kernel.h"
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstdarg>
#include <deque>
#include <mutex>

namespace kernel {

static std::vector<std::string> g_log;
static std::deque<Fault> g_faults;
static bool g_dev = false;
static String g_serialLine;
static std::recursive_mutex g_mx;
static volatile bool g_pan = false;
bool isPanicking() { return g_pan; }
static const char* g_resetName = "unknown";
const char* lastResetName() { return g_resetName; }
std::vector<std::string> logCopy() { std::lock_guard<std::recursive_mutex> lk(g_mx); return g_log; }
std::function<bool(const std::vector<std::string>&, std::string&)> ext;

const char* errName(Err e) {
  switch (e) {
    case E_DISPLAY_INIT: return "E_DISPLAY_INIT";
    case E_DISPLAY_SIZE: return "E_DISPLAY_SIZE";
    case E_TOUCH_RANGE: return "E_TOUCH_RANGE";
    case E_NO_PSRAM: return "E_NO_PSRAM";
    case E_OOM: return "E_OOM";
    case E_SURFACE_TOO_LARGE: return "E_SURFACE_TOO_LARGE";
    case E_APP_FAULT: return "E_APP_FAULT";
    case E_APP_NOT_INSTALLED: return "E_APP_NOT_INSTALLED";
    case E_SCRIPT: return "E_SCRIPT";
    case E_UNEXPECTED_RESET: return "E_UNEXPECTED_RESET";
    default: return "E_OK";
  }
}

static std::string vfmt(const char* fmt, va_list ap) {
  char b[384]; vsnprintf(b, sizeof b, fmt, ap); return b;
}

void log(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); std::string s = vfmt(fmt, ap); va_end(ap);
  char t[24]; snprintf(t, sizeof t, "[%7.2f] ", millis() / 1000.0f);
  s = t + s;
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  if (g_log.size() >= 64) g_log.erase(g_log.begin());
  g_log.push_back(s);
  Serial.println(s.c_str());
}
const std::vector<std::string>& logLines() { return g_log; }

bool devMode() { return g_dev; }
void setDevMode(bool on) {
  g_dev = on;
  Preferences p; p.begin("hrsos", false); p.putBool("dev", on); p.end();
  log("developer mode %s", on ? "ON" : "OFF");
}

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC (CPU exception)";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dip)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

void begin() {
  Serial.begin(115200);
  Preferences p; p.begin("hrsos", true); g_dev = p.getBool("dev", false); p.end();
  esp_reset_reason_t r = esp_reset_reason();
  g_resetName = resetName(r);
  log("HRS OS kernel 0.2 booting, last reset: %s", resetName(r));
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT)
    fault(E_UNEXPECTED_RESET, "The previous session ended unexpectedly: %s.", resetName(r));
  if (!psramFound() || ESP.getPsramSize() == 0)
    log("WARNING: no PSRAM detected - add -DBOARD_HAS_PSRAM and check the board setting");
}

void checkDisplay() {
  int w = M5.Display.width(), h = M5.Display.height();
  log("display reports %dx%d (built for %dx%d)", w, h, HRS_EXPECT_W, HRS_EXPECT_H);
  if (w <= 0 || h <= 0) panic(E_DISPLAY_INIT, "Display returned an invalid size %dx%d. The panel did not initialise.", w, h);
  if (w == HRS_EXPECT_H && h == HRS_EXPECT_W) {  // landscape by mistake: try to recover
    M5.Display.setRotation((M5.Display.getRotation() + 1) & 3);
    w = M5.Display.width(); h = M5.Display.height();
    log("rotation corrected, display now %dx%d", w, h);
  }
  if (w != HRS_EXPECT_W || h != HRS_EXPECT_H)
    panic(E_DISPLAY_SIZE, "Display is %dx%d but HRS OS is built for %dx%d (ST7123). Check the board/M5GFX version or set HRS_EXPECT_W/H in platformio.ini.",
          w, h, HRS_EXPECT_W, HRS_EXPECT_H);
  if (ESP.getPsramSize() < 4u * 1024 * 1024)
    panic(E_NO_PSRAM, "Only %u bytes of PSRAM available; the framebuffers need ~6 MB.", (unsigned)ESP.getPsramSize());
}

void fault(Err code, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); std::string s = vfmt(fmt, ap); va_end(ap);
  log("FAULT 0x%04X %s: %s", (unsigned)code, errName(code), s.c_str());
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  g_faults.push_back({code, s});
}
bool pollFault(Fault& f) {
  std::lock_guard<std::recursive_mutex> lk(g_mx);
  if (g_faults.empty()) return false;
  f = g_faults.front(); g_faults.pop_front(); return true;
}

void panic(Err code, const char* fmt, ...) {
  g_pan = true;
  va_list ap; va_start(ap, fmt); std::string msg = vfmt(fmt, ap); va_end(ap);
  log("KERNEL PANIC 0x%04X %s: %s", (unsigned)code, errName(code), msg.c_str());
  // Draw straight to the panel with whatever size it reports (never trusts our own surfaces).
  auto& d = M5.Display;
  int w = d.width(), h = d.height(); if (w <= 0) w = 320; if (h <= 0) h = 240;
  float u = w / 720.0f; if (u < 0.5f) u = 0.5f;
  d.setRotation(d.getRotation());
  d.fillScreen(0x0010);  // dark blue
  d.setTextColor(TFT_WHITE, 0x0010);
  d.setTextDatum(top_left);
  int x = (int)(40 * u), y = (int)(60 * u);
  d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(u * 1.6f);
  d.drawString(":(", x, y); y += (int)(150 * u);
  d.setFont(&fonts::FreeSansBold18pt7b); d.setTextSize(u * 0.9f);
  d.drawString("Kernel panic", x, y); y += (int)(70 * u);
  d.setFont(&fonts::FreeSans12pt7b); d.setTextSize(u);
  char line[200];
  snprintf(line, sizeof line, "%s (0x%04X)", errName(code), (unsigned)code);
  d.setTextColor(TFT_YELLOW, 0x0010); d.drawString(line, x, y); y += (int)(48 * u);
  d.setTextColor(TFT_WHITE, 0x0010);
  d.setTextWrap(true);
  d.setCursor(x, y); d.setClipRect(x, y, w - 2 * x, h - y);
  d.print(msg.c_str());
  d.clearClipRect();
  y = d.getCursorY() + (int)(40 * u);
  snprintf(line, sizeof line, "Display %dx%d  rot %d", w, h, d.getRotation());
  d.setCursor(x, y); d.print(line); y += (int)(34 * u);
  snprintf(line, sizeof line, "Heap %u  PSRAM %u free", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  d.setCursor(x, y); d.print(line); y += (int)(34 * u);
  snprintf(line, sizeof line, "Uptime %.1fs", millis() / 1000.0f);
  d.setCursor(x, y); d.print(line); y += (int)(50 * u);
  d.setFont(&fonts::Font2); d.setTextSize(u * 1.2f);
  size_t n = g_log.size(), from = n > 10 ? n - 10 : 0;
  for (size_t i = from; i < n; i++) { d.setCursor(x, y); d.print(g_log[i].c_str()); y += (int)(22 * u); }
  d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(u);
  d.setCursor(x, h - (int)(70 * u));
  d.print("Tap the screen or type 'reboot' on serial to restart");
  Serial.println("--- PANIC: tap screen or send 'reboot' ---");
  String s;
  for (;;) {
    M5.update();
    if (M5.Touch.getCount() > 0 && M5.Touch.getDetail(0).wasPressed()) ESP.restart();
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') { s.trim(); if (s == "reboot") ESP.restart(); s = ""; } else s += c;
    }
    delay(20);
  }
}

static std::vector<std::string> split(const std::string& l) {
  std::vector<std::string> v; std::string cur; bool inq = false;
  for (char c : l) {
    if (c == '"') { inq = !inq; continue; }
    if (c == ' ' && !inq) { if (!cur.empty()) v.push_back(cur), cur.clear(); } else cur += c;
  }
  if (!cur.empty()) v.push_back(cur);
  return v;
}

std::string exec(const std::string& line) {
  auto a = split(line);
  if (a.empty()) return "";
  const std::string& c = a[0];
  char b[256];
  if (c == "help")
    return "help info mem display log errors reboot dev [on|off]\n"
           "apps ps launch <app> kill matrix [sec]\n"
           "dev only: run <hrscript> | panic [code] | fault | stackoverflow";
  if (c == "info") {
    snprintf(b, sizeof b, "HRS OS kernel 0.1 | %s | chip rev %d | %d MHz | uptime %.1fs | dev=%s",
             ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getCpuFreqMHz(), millis() / 1000.0f, g_dev ? "on" : "off");
    return b;
  }
  if (c == "mem") {
    snprintf(b, sizeof b, "heap free %u / %u (min %u)\npsram free %u / %u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(), (unsigned)ESP.getMinFreeHeap(),
             (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
    return b;
  }
  if (c == "display") {
    snprintf(b, sizeof b, "panel %dx%d rot %d depth %d | expected %dx%d | %s",
             M5.Display.width(), M5.Display.height(), (int)M5.Display.getRotation(), (int)M5.Display.getColorDepth(),
             HRS_EXPECT_W, HRS_EXPECT_H,
             (M5.Display.width() == HRS_EXPECT_W && M5.Display.height() == HRS_EXPECT_H) ? "OK" : "SIZE MISMATCH");
    return b;
  }
  if (c == "log") {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string o; size_t n = g_log.size();
    for (size_t i = n > 12 ? n - 12 : 0; i < n; i++) o += g_log[i] + "\n";
    return o;
  }
  if (c == "errors")
    return "0x1001 E_DISPLAY_INIT  0x1002 E_DISPLAY_SIZE  0x1003 E_TOUCH_RANGE\n"
           "0x2001 E_NO_PSRAM  0x2002 E_OOM  0x2003 E_SURFACE_TOO_LARGE\n"
           "0x3001 E_APP_FAULT  0x3004 E_APP_NOT_INSTALLED  0x4001 E_SCRIPT  0x5001 E_UNEXPECTED_RESET";
  if (c == "reboot") { ESP.restart(); return ""; }
  if (c == "dev") {
    if (a.size() > 1) setDevMode(a[1] == "on");
    return std::string("developer mode ") + (g_dev ? "on" : "off");
  }
  if (c == "panic" || c == "fault" || c == "stackoverflow" || c == "run") {
    if (!g_dev) return "permission denied: enable Developer Mode in Settings (or 'dev on')";
    if (c == "panic") panic(E_APP_FAULT, "Manual panic requested from kernel shell.");
    if (c == "fault") { fault(E_APP_FAULT, "Manual test fault from kernel shell."); return "fault queued"; }
    if (c == "stackoverflow") {
      struct R { static int f(int x) { volatile char pad[512]; pad[0] = x; return f(x + 1) + pad[0]; } };
      return std::to_string(R::f(0));  // real stack overflow -> real CPU panic -> reboot
    }
  }
  std::string out;
  if (ext && ext(a, out)) return out;
  return "unknown command '" + c + "' (try 'help')";
}

void serialPoll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String l = g_serialLine; g_serialLine = ""; l.trim();
      if (l.length()) { Serial.println(exec(l.c_str()).c_str()); Serial.print("hrs> "); }
    } else g_serialLine += c;
  }
}

}  // namespace kernel
