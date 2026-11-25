// Springboard shell: wallpaper, lock screen, home screen, dock, zoom open/close, status bar, alerts.
#include "os.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>

bool g_showFps = false;
static M5Canvas fb(&M5.Display), wall(&M5.Display), appSpr(&M5.Display), dockSpr(&M5.Display);
static M5Canvas* icons[16];
static constexpr int IS = 128;  // icon size

enum St { LOCK, HOME, OPENING, APP, CLOSING };
static St st = LOCK;
static Spring lockS, openS;
static App* app = nullptr;
static int appIdx = -1, pressedIcon = -1;
static Touch tc;
static float prevY = 0;
static char pendLaunch[24] = "";
static bool pendClose = false, touchWarned = false;
static kernel::Fault alertF; static bool alertOn = false;
static uint32_t lastUs = 0, fpsT = 0; static int fpsN = 0, fpsShown = 0;

// ---------- helpers ----------
void makeSprite(M5Canvas& c, int w, int h, int depth, const char* what) {
  if (w > SW || h > SH)
    kernel::panic(kernel::E_SURFACE_TOO_LARGE, "%s needs a %dx%d surface but the display is only %dx%d.", what, w, h, SW, SH);
  c.setPsram(true); c.setColorDepth(depth);
  if (!c.createSprite(w, h))
    kernel::panic(kernel::E_OOM, "Cannot allocate %s (%dx%d, %d bpp). PSRAM free: %u bytes.", what, w, h, depth, (unsigned)ESP.getFreePsram());
}
void squircle(M5Canvas& g, int x, int y, int w, int h, int r, uint16_t col) { g.fillRoundRect(x, y, w, h, r, col); }

std::string timeString(bool sec) {
  char b[16];
  if (M5.Rtc.isEnabled()) {
    auto t = M5.Rtc.getTime();
    if (sec) snprintf(b, sizeof b, "%d:%02d:%02d", t.hours, t.minutes, t.seconds);
    else snprintf(b, sizeof b, "%d:%02d", t.hours, t.minutes);
  } else {
    uint32_t s = millis() / 1000 + 9 * 3600 + 41 * 60;
    if (sec) snprintf(b, sizeof b, "%u:%02u:%02u", (s / 3600) % 24, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof b, "%u:%02u", (s / 3600) % 24, (s / 60) % 60);
  }
  return b;
}
static std::string dateString() {
  static const char* wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char* mo[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (!M5.Rtc.isEnabled()) return "Set the RTC for the date";
  auto d = M5.Rtc.getDate(); char b[40];
  snprintf(b, sizeof b, "%s %d %s", wd[d.weekDay % 7], d.date, mo[(d.month + 11) % 12]);
  return b;
}

// ---------- wallpaper (original iOS-16-style gradient, rendered once at boot) ----------
static inline float sqf(float x) { return x * x; }
static void wallRGB(int x, int y, float& r, float& g, float& b) {
  float u = x / (float)SW, v = y / (float)SH;
  r = 40 + 150 * v; g = 50 + 20 * v; b = 215 - 40 * v;                // blue -> violet
  auto blob = [&](float cx, float cy, float rad, float R, float G, float B, float s) {
    float d = (sqf(u - cx) + sqf((v - cy) * 0.5625f)) / sqf(rad);
    if (d < 1) { float w = sqf(1 - d) * s; r += (R - r) * w; g += (G - g) * w; b += (B - b) * w; }
  };
  blob(0.85f, 0.72f, 0.55f, 255, 90, 150, 0.95f);   // pink
  blob(0.10f, 0.22f, 0.50f, 70, 200, 255, 0.85f);   // cyan
  blob(0.55f, 1.02f, 0.45f, 255, 150, 90, 0.9f);    // warm
  blob(0.75f, 0.05f, 0.35f, 150, 110, 255, 0.7f);   // lilac
}
static uint16_t c565(float r, float g, float b) {
  return M5.Display.color565((uint8_t)fminf(r, 255), (uint8_t)fminf(g, 255), (uint8_t)fminf(b, 255));
}
static void buildWallpaper() {
  uint32_t t = millis();
  wall.startWrite();
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) { float r, g, b; wallRGB(x, y, r, g, b); wall.writePixel(x, y, c565(r, g, b)); }
  wall.endWrite();
  kernel::log("wallpaper rendered in %u ms", (unsigned)(millis() - t));
}
static bool inRound(int x, int y, int w, int h, int r) {
  int cx = x < r ? r : (x >= w - r ? w - r - 1 : x), cy = y < r ? r : (y >= h - r ? h - r - 1 : y);
  return sqf(x - cx) + sqf(y - cy) <= sqf(r);
}
static void buildDock() {   // pre-rendered "frosted" dock: wallpaper lightened under a rounded mask
  const int DW = SW - 48, DH = 170, oy = SH - 190;
  makeSprite(dockSpr, DW, DH, 16, "dock");
  dockSpr.fillSprite(TFT_TRANSPARENT);
  dockSpr.startWrite();
  for (int y = 0; y < DH; y++)
    for (int x = 0; x < DW; x++) {
      if (!inRound(x, y, DW, DH, 52)) continue;
      float r, g, b; wallRGB(x + 24, y + oy, r, g, b);
      dockSpr.writePixel(x, y, c565(r * 0.62f + 255 * 0.38f, g * 0.62f + 255 * 0.38f, b * 0.62f + 255 * 0.38f));
    }
  dockSpr.endWrite();
}

// Optional: use your own PNG icons from LittleFS (data/icons/<appname>.png, lowercase). Falls back to the drawn glyph.
static bool loadPng(M5Canvas& c, const char* name) {
  static bool fsOk = LittleFS.begin(false);
  if (!fsOk) return false;
  std::string p = "/icons/"; for (const char* q = name; *q; q++) p += (char)tolower(*q); p += ".png";
  File f = LittleFS.open(p.c_str(), "r");
  if (!f) return false;
  size_t n = f.size();
  if (n < 33 || n > 600000) { kernel::log("icon %s: bad size %u", p.c_str(), (unsigned)n); return false; }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!buf) { kernel::log("icon %s: out of memory", p.c_str()); return false; }
  f.read(buf, n); f.close();
  uint32_t w = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19], h = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
  bool ok = false;
  if (w > 0 && h > 0 && w <= 4096 && h <= 4096) ok = c.drawPng(buf, n, 0, 0, IS, IS, 0, 0, (float)IS / w, (float)IS / h);
  heap_caps_free(buf);
  kernel::log("icon %s: %s", p.c_str(), ok ? "loaded" : "decode failed");
  return ok;
}
static void buildIcons() {
  for (int i = 0; i < kAppCount; i++) {
    icons[i] = new M5Canvas(&M5.Display);
    makeSprite(*icons[i], IS, IS, 16, "app icon");
    M5Canvas& c = *icons[i];
    c.fillSprite(TFT_TRANSPARENT);
    const AppInfo& a = kApps[i];
    for (int y = 0; y < IS; y++) {
      float t = y / (float)(IS - 1);
      uint16_t r1 = a.c1 >> 11, g1 = (a.c1 >> 5) & 63, b1 = a.c1 & 31, r2 = a.c2 >> 11, g2 = (a.c2 >> 5) & 63, b2 = a.c2 & 31;
      uint16_t col = (uint16_t)(((int)(r1 + (r2 - r1) * t) << 11) | ((int)(g1 + (g2 - g1) * t) << 5) | (int)(b1 + (b2 - b1) * t));
      for (int x = 0; x < IS; x++) c.drawPixel(x, y, col);
    }
    if (!loadPng(c, a.name)) a.glyph(c, IS / 2, IS / 2, IS);
    for (int y = 0; y < IS; y++) for (int x = 0; x < IS; x++) if (!inRound(x, y, IS, IS, 30)) c.drawPixel(x, y, TFT_TRANSPARENT);
    c.setPivot(IS / 2.0f, IS / 2.0f);
  }
}

// ---------- layout ----------
static void iconPos(int i, int& x, int& y) {
  if (i < 4) { x = 24 + (SW - 48) / 4 * i + ((SW - 48) / 4 - IS) / 2; y = SH - 190 + 21; }
  else { int j = i - 4; x = SW / 4 * (j % 4) + (SW / 4 - IS) / 2; y = 170 + (j / 4) * 190; }
}
static int hitIcon(int px, int py) {
  for (int i = 0; i < kVisibleCount; i++) {
    int x, y; iconPos(i, x, y);
    if (px >= x - 8 && px < x + IS + 8 && py >= y - 8 && py < y + IS + (i < 4 ? 8 : 40)) return i;
  }
  return -1;
}

// ---------- drawing ----------
static void statusBar(M5Canvas& g, bool dark) {
  uint16_t col = dark ? TFT_BLACK : TFT_WHITE;
  g.setTextColor(col); g.setFont(&fonts::FreeSansBold12pt7b); g.setTextSize(1);
  g.setTextDatum(middle_left); g.drawString(timeString().c_str(), 40, 30);
  int lvl = M5.Power.getBatteryLevel(); if (lvl < 0) lvl = 0; if (lvl > 100) lvl = 100;
  int bx = SW - 100, by = 20;
  g.drawRoundRect(bx, by, 52, 24, 7, col); g.fillRect(bx + 53, by + 8, 3, 8, col);
  g.fillRoundRect(bx + 3, by + 3, (46 * lvl) / 100, 18, 4, lvl <= 20 ? TFT_RED : col);
  char b[8]; snprintf(b, sizeof b, "%d%%", lvl);
  g.setFont(&fonts::FreeSans9pt7b); g.setTextDatum(middle_right); g.drawString(b, bx - 10, 30);
}
static void homeIndicator(M5Canvas& g, bool dark) { g.fillRoundRect(SW / 2 - 120, SH - 22, 240, 9, 4, dark ? TFT_BLACK : TFT_WHITE); }

static void drawHome(M5Canvas& g) {
  wall.pushSprite(&g, 0, 0);
  statusBar(g, false);
  g.setFont(&fonts::FreeSans9pt7b); g.setTextSize(1); g.setTextColor(TFT_WHITE); g.setTextDatum(top_center);
  for (int i = 4; i < kVisibleCount; i++) {
    int x, y; iconPos(i, x, y);
    float s = (i == pressedIcon) ? 0.9f : 1.0f;
    icons[i]->pushRotateZoom(&g, x + IS / 2.0f, y + IS / 2.0f, 0, s, s, TFT_TRANSPARENT);
    g.drawString(kApps[i].name, x + IS / 2, y + IS + 10);
  }
  dockSpr.pushSprite(&g, 24, SH - 190, TFT_TRANSPARENT);
  for (int i = 0; i < 4 && i < kVisibleCount; i++) {
    int x, y; iconPos(i, x, y);
    float s = (i == pressedIcon) ? 0.9f : 1.0f;
    icons[i]->pushRotateZoom(&g, x + IS / 2.0f, y + IS / 2.0f, 0, s, s, TFT_TRANSPARENT);
  }
  homeIndicator(g, false);
}
static void drawLock(M5Canvas& g, float off) {   // off <= 0: how far the lock screen has slid up
  int o = (int)off;
  wall.pushSprite(&g, 0, o);
  g.setTextColor(TFT_WHITE); g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSans12pt7b); g.setTextSize(1.3f); g.drawString(dateString().c_str(), SW / 2, 190 + o);
  g.setFont(&fonts::FreeSansBold24pt7b); g.setTextSize(3.4f); g.drawString(timeString().c_str(), SW / 2, 340 + o);
  g.setFont(&fonts::FreeSans12pt7b); g.setTextSize(1);
  g.drawString("Swipe up to unlock", SW / 2, SH - 100 + o);
  homeIndicator(g, false);
}
static void wrapText(M5Canvas& g, const std::string& s, int x, int y, int w, int lh) {
  std::string line, word; int cy = y;
  auto flush = [&]() { g.drawString(line.c_str(), x, cy); cy += lh; line.clear(); };
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == ' ' || s[i] == '\n') {
      std::string t = line.empty() ? word : line + " " + word;
      if (!line.empty() && g.textWidth(t.c_str()) > w) { flush(); line = word; } else line = t;
      word.clear();
      if (i < s.size() && s[i] == '\n') flush();
    } else word += s[i];
  }
  if (!line.empty()) flush();
}
static void drawAlert(M5Canvas& g) {
  int w = 580, h = 420, x = (SW - w) / 2, y = (SH - h) / 2;
  g.fillRoundRect(x + 6, y + 10, w, h, 36, rgb(30, 30, 40));       // drop shadow
  g.fillRoundRect(x, y, w, h, 36, rgb(236, 236, 242));
  g.setTextDatum(top_center); g.setTextColor(TFT_BLACK);
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextSize(1.2f);
  char t[48]; snprintf(t, sizeof t, "%s", kernel::errName(alertF.code)); g.drawString(t, SW / 2, y + 28);
  g.setFont(&fonts::FreeSans9pt7b); g.setTextSize(1.15f); g.setTextDatum(top_left);
  wrapText(g, alertF.msg, x + 34, y + 90, w - 68, 34);
  g.drawFastHLine(x, y + h - 96, w, rgb(190, 190, 200));
  g.setTextDatum(middle_center); g.setTextColor(rgb(0, 122, 255)); g.setFont(&fonts::FreeSansBold12pt7b); g.setTextSize(1.2f);
  g.drawString("OK", SW / 2, y + h - 48);
}

// ---------- app lifecycle ----------
template <typename F> static bool safe(const char* what, F f) {
  try { f(); return true; }
  catch (std::exception& e) { kernel::log("CRASH 0x%04X %s in %s: %s", (unsigned)kernel::E_APP_FAULT, app ? app->name() : "app", what, e.what()); }
  catch (...) { kernel::log("CRASH 0x%04X %s in %s (unknown exception)", (unsigned)kernel::E_APP_FAULT, app ? app->name() : "app", what); }
  return false;
}
static void killApp() {
  if (app) { safe("onClose", [&] { app->onClose(); }); delete app; app = nullptr; }
  appIdx = -1; pressedIcon = -1; if (st != LOCK) st = HOME;   // instant: no zoom-out
  openS.snap(0);
}
static void launch(int i, bool zoom) {
  if (i < 0 || i >= kAppCount) return;
  if (!kApps[i].make) { kernel::fault(kernel::E_APP_NOT_INSTALLED, "%s is not implemented in this build yet.", kApps[i].name); return; }
  if (app) killApp();
  app = kApps[i].make(); appIdx = i;
  if (!safe("onLaunch", [&] { app->onLaunch(); })) { killApp(); return; }
  openS.snap(0); if (zoom) { openS.target = 1; st = OPENING; } else { openS.snap(1); st = APP; }
  kernel::log("launch %s", kApps[i].name);
}
static void drawAppFrame() {
  if (!app) return;
  appSpr.fillSprite(app->bg());
  if (!safe("onDraw", [&] { app->onDraw(appSpr); })) { killApp(); return; }
  statusBar(appSpr, app->darkStatusBar()); homeIndicator(appSpr, app->darkStatusBar());
}

// ---------- kernel shell hooks ----------
std::string shell::psText() {
  char b[160];
  snprintf(b, sizeof b, "shell  running  fg=%s  state=%d  heap=%u psram=%u", app ? app->name() : "(springboard)", (int)st,
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  return b;
}
void shell::requestLaunch(const char* n) { snprintf(pendLaunch, sizeof pendLaunch, "%s", n); }
void shell::requestClose() { pendClose = true; }
static int findApp(const std::string& n) {
  for (int i = 0; i < kAppCount; i++) { std::string a = kApps[i].name; for (auto& c : a) c = tolower(c); std::string q = n; for (auto& c : q) c = tolower(c); if (a == q) return i; }
  return -1;
}

void shellSetup() {
  makeSprite(fb, SW, SH, 16, "framebuffer");
  makeSprite(wall, SW, SH, 16, "wallpaper");
  makeSprite(appSpr, SW, SH, 16, "app surface");
  appSpr.setPivot(SW / 2.0f, SH / 2.0f);
  buildWallpaper(); buildDock(); buildIcons();
  kernel::ext = [](const std::vector<std::string>& a, std::string& out) -> bool {
    if (a[0] == "apps") { for (int i = 0; i < kAppCount; i++) out += std::string(kApps[i].name) + (kApps[i].make ? "" : " (not implemented)") + "\n"; return true; }
    if (a[0] == "ps") { out = shell::psText(); return true; }
    if (a[0] == "launch" && a.size() > 1) { int i = findApp(a[1]); if (i >= kVisibleCount && !kernel::devMode()) { out = "permission denied: enable Developer Mode"; return true; } if (i < 0) out = "no such app"; else { shell::requestLaunch(kApps[i].name); out = "launching"; } return true; }
    if (a[0] == "kill") { shell::requestClose(); out = "closing foreground app"; return true; }
    if (a[0] == "lock") { if (st == HOME) { st = LOCK; lockS.snap(0); out = "locked"; } return true; }
    if (a[0] == "run") {
      std::string src; for (size_t i = 1; i < a.size(); i++) src += (i > 1 ? " " : "") + a[i];
      out = scriptRun(src); return true;
    }
    return false;
  };
  lastUs = micros();
}

static void present(M5Canvas& src) {
  M5.Display.startWrite();            // one atomic transfer of the finished frame
  src.pushSprite(&M5.Display, 0, 0);
  M5.Display.endWrite();
  M5.Display.waitDisplay();           // do not start the next frame until the transfer is done
}
static void fpsOverlay(M5Canvas& g) {
  if (!(g_showFps && kernel::devMode())) return;
  fpsN++; if (millis() - fpsT > 1000) { fpsShown = fpsN; fpsN = 0; fpsT = millis(); }
  g.setFont(&fonts::FreeSansBold9pt7b); g.setTextSize(1); g.setTextColor(TFT_GREEN, TFT_BLACK);
  g.setTextDatum(top_left); char b[16]; snprintf(b, sizeof b, " %d fps ", fpsShown); g.drawString(b, 200, 8);
}

static void readTouch(float dt) {
  auto d = M5.Touch.getDetail(0);
  tc.pressed = d.wasPressed(); tc.released = d.wasReleased();
  tc.down = d.isPressed();
  if (d.isPressed() || d.wasPressed() || d.wasReleased()) {
    if (d.x < 0 || d.y < 0 || d.x >= SW || d.y >= SH) {
      if (!touchWarned) { touchWarned = true; kernel::fault(kernel::E_TOUCH_RANGE, "Touch at (%d,%d) is outside the %dx%d panel - touch and display geometry disagree.", d.x, d.y, SW, SH); }
    } else { tc.x = d.x; tc.y = d.y; }
  }
  if (tc.pressed) { tc.sx = tc.x; tc.sy = tc.y; tc.t0 = millis(); prevY = tc.y; tc.vy = 0; }
  if (tc.down) { tc.vy = tc.vy * 0.7f + ((tc.y - prevY) / dt) * 0.3f; prevY = tc.y; }
}

void shellLoop() {
  uint32_t now = micros(); float dt = (now - lastUs) / 1e6f; lastUs = now;
  if (dt < 0.001f) dt = 0.001f; if (dt > 0.05f) dt = 0.05f;
  readTouch(dt);

  if (!alertOn && kernel::pollFault(alertF)) alertOn = true;
  if (pendClose) { pendClose = false; if (app) { openS.target = 0; st = CLOSING; } }
  if (pendLaunch[0]) { int i = findApp(pendLaunch); pendLaunch[0] = 0; launch(i, false); }

  if (alertOn) {
    if (tc.released && tc.x > SW / 2 - 290 && tc.x < SW / 2 + 290 && tc.y > SH / 2 + 114 && tc.y < SH / 2 + 210) alertOn = false;
  } else switch (st) {
    case LOCK:
      if (tc.down) { lockS.x = fminf(0, (float)(tc.y - tc.sy)); lockS.v = 0; }
      if (tc.released) lockS.target = (lockS.x < -SH * 0.22f || tc.vy < -800) ? -SH : 0;
      if (!tc.down) lockS.step(dt);
      if (lockS.x <= -SH + 2) { st = HOME; lockS.snap(0); }
      break;
    case HOME:
      pressedIcon = tc.down ? hitIcon(tc.sx, tc.sy) : -1;
      if (tc.pressed && tc.sy > SH - 40) pressedIcon = -1;
      if (tc.released) {
        int dx = abs(tc.x - tc.sx), dy = abs(tc.y - tc.sy);
        if (dx < 24 && dy < 24 && millis() - tc.t0 < 700) { int i = hitIcon(tc.sx, tc.sy); if (i >= 0) { pressedIcon = -1; launch(i, true); } }
      }
      break;
    case OPENING: openS.step(dt); if (openS.settled()) { openS.snap(1); st = APP; } break;
    case CLOSING: openS.step(dt); if (openS.settled()) { openS.snap(0); killApp(); } break;
    case APP:
      if (tc.released && tc.sy > SH - 70 && tc.sy - tc.y > 120) { openS.target = 0; st = CLOSING; break; }   // home gesture
      if (app && !(tc.sy > SH - 70 && tc.down)) safe("onTouch", [&] { app->onTouch(tc); });
      break;
  }

  // ---- render ----
  if (st == APP && app) {
    drawAppFrame();
    if (app) {   // may have been killed by a fault
      if (alertOn) drawAlert(appSpr);
      fpsOverlay(appSpr); present(appSpr);
    }
  } else {
    if (st == OPENING || st == CLOSING) drawAppFrame();
    if (st == LOCK) { drawHome(fb); drawLock(fb, lockS.x); }
    else {
      drawHome(fb);
      if ((st == OPENING || st == CLOSING) && app) {
        int ix, iy; iconPos(appIdx, ix, iy);
        float t = fminf(fmaxf(openS.x, 0.f), 1.04f);
        float cx = (ix + IS / 2.0f) + (SW / 2.0f - (ix + IS / 2.0f)) * t, cy = (iy + IS / 2.0f) + (SH / 2.0f - (iy + IS / 2.0f)) * t;
        float zx = (IS + (SW - IS) * t) / SW, zy = (IS + (SH - IS) * t) / SH;
        appSpr.pushRotateZoom(&fb, cx, cy, 0, zx, zy);
      }
    }
    if (alertOn) drawAlert(fb);
    fpsOverlay(fb); present(fb);
  }

  uint32_t spent = micros() - now;   // cap at ~60 fps (panel refresh) so frames are not pushed mid-scan
  if (spent < 16000) delay((16000 - spent) / 1000);
}
