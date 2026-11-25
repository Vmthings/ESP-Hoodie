// Springboard: the lock screen + home screen, running as a normal userspace app (its own process).
#include <cstring>
#include "app_api.h"

namespace {
constexpr int IS = 128;
inline float sqf(float x) { return x * x; }
inline uint16_t col(float r, float g, float b) { return rgb((int)fminf(r, 255), (int)fminf(g, 255), (int)fminf(b, 255)); }
bool inRound(int x, int y, int w, int h, int r) {
  int cx = x < r ? r : (x >= w - r ? w - r - 1 : x), cy = y < r ? r : (y >= h - r ? h - r - 1 : y);
  return sqf(x - cx) + sqf(y - cy) <= sqf(r);
}
// original iOS-16-style gradient wallpaper
void wallRGB(int x, int y, float& r, float& g, float& b) {
  float u = x / (float)SW, v = y / (float)SH;
  r = 40 + 150 * v; g = 50 + 20 * v; b = 215 - 40 * v;
  auto blob = [&](float cx, float cy, float rad, float R, float G, float B, float s) {
    float d = (sqf(u - cx) + sqf((v - cy) * 0.5625f)) / sqf(rad);
    if (d < 1) { float w = sqf(1 - d) * s; r += (R - r) * w; g += (G - g) * w; b += (B - b) * w; }
  };
  blob(0.85f, 0.72f, 0.55f, 255, 90, 150, 0.95f); blob(0.10f, 0.22f, 0.50f, 70, 200, 255, 0.85f);
  blob(0.55f, 1.02f, 0.45f, 255, 150, 90, 0.9f);  blob(0.75f, 0.05f, 0.35f, 150, 110, 255, 0.7f);
}
}  // namespace

class Springboard : public App {
  M5Canvas *wall = nullptr, *dock = nullptr, *icons[16] = {};
  bool ready = false, locked = true, held = false;
  Spring lockS; int pressedIcon = -1; uint32_t lastMs = 0;

  void build() {
    wall = sys::newCanvas(SW, SH);
    wall->startWrite();
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { float r, g, b; wallRGB(x, y, r, g, b); wall->writePixel(x, y, col(r, g, b)); }
    wall->endWrite();
    const int DW = SW - 48, DH = 170, oy = SH - 190;     // pre-rendered frosted dock
    dock = sys::newCanvas(DW, DH); dock->fillSprite(TFT_TRANSPARENT);
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) {
      if (!inRound(x, y, DW, DH, 52)) continue;
      float r, g, b; wallRGB(x + 24, y + oy, r, g, b);
      dock->drawPixel(x, y, col(r * 0.62f + 97, g * 0.62f + 97, b * 0.62f + 97));
    }
    for (int i = 0; i < kAppCount && i < 16; i++) {
      M5Canvas* c = icons[i] = sys::newCanvas(IS, IS);
      const AppInfo& a = kApps[i];
      std::vector<uint8_t> png; std::string p = "/icons/"; for (const char* q = a.name; *q; q++) p += (char)tolower(*q); p += ".png";
      bool ok = false;
      if (sys::readFile(p.c_str(), png) && png.size() > 33) {
        uint32_t w = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19], h = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];
        if (w && h && w <= 4096 && h <= 4096) ok = c->drawPng(png.data(), png.size(), 0, 0, IS, IS, 0, 0, (float)IS / w, (float)IS / h);
      }
      if (!ok) {
        uint16_t r1 = a.c1 >> 11, g1 = (a.c1 >> 5) & 63, b1 = a.c1 & 31, r2 = a.c2 >> 11, g2 = (a.c2 >> 5) & 63, b2 = a.c2 & 31;
        for (int y = 0; y < IS; y++) {
          float t = y / (float)(IS - 1);
          uint16_t cc = (uint16_t)(((int)(r1 + (r2 - r1) * t) << 11) | ((int)(g1 + (g2 - g1) * t) << 5) | (int)(b1 + (b2 - b1) * t));
          for (int x = 0; x < IS; x++) c->drawPixel(x, y, cc);
        }
        a.glyph(*c, IS / 2, IS / 2, IS);
      }
      for (int y = 0; y < IS; y++) for (int x = 0; x < IS; x++) if (!inRound(x, y, IS, IS, 30)) c->drawPixel(x, y, TFT_TRANSPARENT);
      c->setPivot(IS / 2.0f, IS / 2.0f);
    }
    ready = true; lastMs = millis();
  }
  static void iconPos(int i, int& x, int& y) {
    if (i < 4) { x = 24 + (SW - 48) / 4 * i + ((SW - 48) / 4 - IS) / 2; y = SH - 190 + 21; }
    else { int j = i - 4; x = SW / 4 * (j % 4) + (SW / 4 - IS) / 2; y = 170 + (j / 4) * 190; }
  }
  int hit(int px, int py) {
    for (int i = 0; i < kVisibleCount; i++) { int x, y; iconPos(i, x, y); if (px >= x - 8 && px < x + IS + 8 && py >= y - 8 && py < y + IS + (i < 4 ? 8 : 40)) return i; }
    return -1;
  }
  void drawHome(M5Canvas& g) {
    wall->pushSprite(&g, 0, 0);
    dock->pushSprite(&g, 24, SH - 190, TFT_TRANSPARENT);
    uiFont(&g, &fonts::FreeSans9pt7b); uiSize(&g, 1); g.setTextColor(TFT_WHITE); g.setTextDatum(top_center);
    for (int i = 0; i < kVisibleCount; i++) {
      int x, y; iconPos(i, x, y);
      if (i == pressedIcon) icons[i]->pushRotateZoom(&g, x + IS / 2.0f, y + IS / 2.0f, 0, 0.9f, 0.9f, TFT_TRANSPARENT);
      else icons[i]->pushSprite(&g, x, y, TFT_TRANSPARENT);   // plain blit: much cheaper than rotate/zoom
      if (i >= 4) g.drawString(kApps[i].name, x + IS / 2, y + IS + 10);
    }
  }
  void drawLock(M5Canvas& g, float off) {
    int o = (int)off; wall->pushSprite(&g, 0, o);
    g.setTextColor(TFT_WHITE); g.setTextDatum(middle_center);
    uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1.3f); g.drawString(sys::dateString().c_str(), SW / 2, 190 + o);
    uiFont(&g, &fonts::FreeSansBold24pt7b); uiSize(&g, 3.4f); g.drawString(sys::timeString().c_str(), SW / 2, 340 + o);
    uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1); g.drawString("Swipe up to unlock", SW / 2, SH - 100 + o);
  }
 public:
  const char* name() const override { return "Springboard"; }
  int stackBytes() const override { return 16384; }
  int idleMs() const override { return locked ? 1000 : 60000; }
  bool animating() const override { return locked && !lockS.settled(); }
  void onLaunch() override { build(); }
  void onCommand(const char* c) override { if (!strcmp(c, "lock")) { locked = true; lockS.snap(0); } }
  void onTouch(const Touch& t) override {
    held = t.down;
    if (!ready) return;
    if (locked) {
      if (t.down) { lockS.x = fminf(0, (float)(t.y - t.sy)); lockS.v = 0; }
      if (t.released) lockS.target = (lockS.x < -SH * 0.22f || t.vy < -800) ? -SH : 0;
      return;
    }
    pressedIcon = t.down ? hit(t.sx, t.sy) : -1;
    if (t.pressed && t.sy > SH - 40) pressedIcon = -1;
    if (t.released && abs(t.x - t.sx) < 24 && abs(t.y - t.sy) < 24 && millis() - t.t0 < 700) {
      int i = hit(t.sx, t.sy);
      if (i >= 0) { int x, y; iconPos(i, x, y); pressedIcon = -1; sys::launchZoom(i, x, y, IS); }
    }
  }
  void onDraw(M5Canvas& g) override {
    if (!ready) { g.fillScreen(TFT_BLACK); return; }
    uint32_t now = millis(); float dt = (now - lastMs) / 1000.f; lastMs = now; if (dt > 0.05f) dt = 0.05f;
    if (locked && !held) lockS.step(dt);
    if (locked && lockS.x <= -SH + 2) { locked = false; lockS.snap(0); }
    drawHome(g);
    if (locked) drawLock(g, lockS.x);
  }
  void onClose() override { /* canvases are owned by the process and freed by the kernel */ }
};
App* makeSpringboard() { return new Springboard(); }
