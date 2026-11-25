// Kernel side of the userspace boundary: system calls + UI font service.
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <cstdarg>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include "os.h"

static thread_local Process* tl_cur = nullptr;
Process* currentProcess() { return tl_cur; }
void setCurrentProcess(Process* p) { tl_cur = p; }

static std::set<M5Canvas*> g_live; static std::mutex g_liveMx;
bool fsMounted() { static bool ok = LittleFS.begin(false); return ok; }

namespace sys {
int displayW() { return M5.Display.width(); }
int displayH() { return M5.Display.height(); }
void setBrightness(int v) { M5.Display.setBrightness((uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v)); }
int battery() { return M5.Power.getBatteryLevel(); }
uint32_t freeHeap() { return ESP.getFreeHeap(); }
uint32_t freePsram() { return ESP.getFreePsram(); }
void log(const char* fmt, ...) { char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); kernel::log("%s", b); }
void launch(const char* n, const char* a) { shell::requestLaunch(n, a ? a : ""); }
void launchZoom(int i, int x, int y, int s) { shell::requestLaunchZoom(i, x, y, s); }
const char* arg() { return tl_cur ? tl_cur->arg.c_str() : ""; }
bool devMode() { return kernel::devMode(); }
void setDevMode(bool on) { kernel::setDevMode(on); }
bool showFps() { return g_showFps; }
void setShowFps(bool on) { g_showFps = on; }
void raiseFault(uint32_t code, const char* msg) { kernel::fault((kernel::Err)code, "%s", msg); }
void requestPanic(const char* msg) { shell::requestPanic(msg); }
std::string kshell(const std::string& l) { return kernel::exec(l); }
void pet() { if (tl_cur) tl_cur->beat = millis(); }

std::string timeString(bool sec) {
  char b[16];
  if (M5.Rtc.isEnabled()) {
    auto t = M5.Rtc.getTime();
    if (sec) snprintf(b, sizeof b, "%d:%02d:%02d", t.hours, t.minutes, t.seconds); else snprintf(b, sizeof b, "%d:%02d", t.hours, t.minutes);
  } else {
    uint32_t s = millis() / 1000 + 9 * 3600 + 41 * 60;
    if (sec) snprintf(b, sizeof b, "%u:%02u:%02u", (s / 3600) % 24, (s / 60) % 60, s % 60); else snprintf(b, sizeof b, "%u:%02u", (s / 3600) % 24, (s / 60) % 60);
  }
  return b;
}
std::string dateString() {
  static const char* wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char* mo[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (!M5.Rtc.isEnabled()) return "Set the RTC for the date";
  auto d = M5.Rtc.getDate(); char b[40];
  snprintf(b, sizeof b, "%s %d %s", wd[d.weekDay % 7], d.date, mo[(d.month + 11) % 12]);
  return b;
}

M5Canvas* newCanvas(int w, int h) {
  if (w <= 0 || h <= 0 || w > SW || h > SH)
    throw std::out_of_range("canvas " + std::to_string(w) + "x" + std::to_string(h) + " exceeds the display " + std::to_string(SW) + "x" + std::to_string(SH));
  Process* p = tl_cur; size_t bytes = (size_t)w * h * 2;
  if (p && p->surfBytes + bytes > p->quota) throw std::bad_alloc();   // per-process memory quota
  M5Canvas* c = new M5Canvas(&M5.Display);
  c->setPsram(true); c->setColorDepth(16);
  if (!c->createSprite(w, h)) { delete c; throw std::bad_alloc(); }
  if (p) { p->surfBytes += bytes; p->owned.push_back(c); }
  std::lock_guard<std::mutex> lk(g_liveMx); g_live.insert(c);
  return c;
}
void freeCanvas(M5Canvas* c) {
  if (!c) return;
  Process* p = tl_cur;
  if (p) for (size_t i = 0; i < p->owned.size(); i++) if (p->owned[i] == c) { p->surfBytes -= (size_t)c->width() * c->height() * 2; p->owned.erase(p->owned.begin() + i); break; }
  kernelFreeCanvas(c);
}
bool canvasValid(M5Canvas* c) { std::lock_guard<std::mutex> lk(g_liveMx); return g_live.count(c) > 0; }
bool readFile(const char* path, std::vector<uint8_t>& out) {
  std::string p = path;
  if (p.rfind("/icons/", 0) != 0 && p.rfind("/fonts/", 0) != 0) { kernel::log("readFile denied: %s", path); return false; }
  if (!fsMounted()) return false;
  File f = LittleFS.open(p.c_str(), "r"); if (!f) return false;
  size_t n = f.size(); if (n == 0 || n > 800000) return false;
  out.resize(n); f.read(out.data(), n); f.close(); return true;
}
}  // namespace sys

void kernelFreeCanvas(M5Canvas* c) {
  { std::lock_guard<std::mutex> lk(g_liveMx); g_live.erase(c); }
  delete c;
}

// ---------- UI font service ----------
static uint8_t *fReg = nullptr, *fBold = nullptr;
static thread_local float tl_scale = 1.f;
static thread_local const void* tl_lastG = nullptr;
static thread_local const uint8_t* tl_lastData = nullptr;
static uint8_t* slurp(const char* p) {
  File f = LittleFS.open(p, "r"); if (!f) return nullptr;
  size_t n = f.size(); uint8_t* b = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (b) f.read(b, n); f.close(); return b;
}
void fontInit() {
  if (!fsMounted()) return;
  fReg = slurp("/fonts/regular.vlw"); fBold = slurp("/fonts/bold.vlw");
  kernel::log("ui font: regular %s, bold %s", fReg ? "loaded" : "none (using built-in)", fBold ? "loaded" : "none");
}
void uiFont(lgfx::LGFXBase* g, const lgfx::IFont* f) {
  tl_scale = 1.f;
  if (!fReg || f == &fonts::Font2) { tl_lastData = nullptr; g->setFont(f); return; }
  static const struct { const lgfx::IFont* f; float px; bool bold; } T[] = {
    {&fonts::FreeSans9pt7b, 20, false}, {&fonts::FreeSans12pt7b, 26, false}, {&fonts::FreeSans18pt7b, 38, false}, {&fonts::FreeSans24pt7b, 50, false},
    {&fonts::FreeSansBold9pt7b, 20, true}, {&fonts::FreeSansBold12pt7b, 26, true}, {&fonts::FreeSansBold18pt7b, 38, true}, {&fonts::FreeSansBold24pt7b, 50, true}};
  for (auto& e : T) if (e.f == f) { const uint8_t* d = (e.bold && fBold) ? fBold : fReg; if (!(tl_lastG == g && tl_lastData == d)) { g->loadFont(d); tl_lastG = g; tl_lastData = d; } tl_scale = e.px / 40.0f; return; }   // VLW expected at 40 px
  tl_lastData = nullptr; g->setFont(f);
}
void uiSize(lgfx::LGFXBase* g, float s) { g->setTextSize(s * tl_scale); }
