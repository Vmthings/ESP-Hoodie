#pragma once
#include <M5Unified.h>
#include <cmath>
#include <string>
#include <vector>
#include "kernel.h"

constexpr int SW = HRS_EXPECT_W, SH = HRS_EXPECT_H;

struct Touch {
  bool down = false, pressed = false, released = false;
  int x = 0, y = 0, sx = 0, sy = 0;   // current, start
  float vy = 0;                       // smoothed vertical velocity px/s
  uint32_t t0 = 0;
};

struct Spring {
  float x = 0, v = 0, target = 0, k = 240, c = 26;
  void step(float dt) {
    for (int i = 0; i < 4; i++) {   // sub-steps keep it stable at low fps
      float h = dt / 4, a = -k * (x - target) - c * v;
      v += a * h; x += v * h;
    }
  }
  bool settled() const { return fabsf(x - target) < 0.002f * (1 + fabsf(target)) && fabsf(v) < 0.05f; }
  void snap(float t) { x = target = t; v = 0; }
};

inline uint16_t rgb(int r, int g, int b) { return M5.Display.color565(r, g, b); }

class App {
 public:
  virtual ~App() {}
  virtual const char* name() const = 0;
  virtual void onLaunch() {}
  virtual void onDraw(M5Canvas& g) = 0;         // paint full SWxSH frame; keep y<56 free for status bar
  virtual void onTouch(const Touch&) {}
  virtual void onKey(char) {}
  virtual void onClose() {}
  virtual bool darkStatusBar() const { return false; }  // true = black status text (light app)
  virtual uint16_t bg() const { return 0; }
};

struct AppInfo {
  const char* name;
  uint16_t c1, c2;                              // icon gradient
  void (*glyph)(M5Canvas&, int cx, int cy, int s);
  App* (*make)();                               // nullptr = not implemented yet
};

namespace shell {
void requestLaunch(const char* appName);        // switch to app (no zoom)
void requestClose();
std::string psText();
}

// helpers shared by apps
void makeSprite(M5Canvas& c, int w, int h, int depth, const char* what);
void squircle(M5Canvas& g, int x, int y, int w, int h, int r, uint16_t col);
extern const AppInfo kApps[];
extern const int kAppCount;
extern const int kVisibleCount;   // apps shown on the home screen (Console is hidden)
std::string timeString(bool seconds = false);

extern bool g_showFps;
std::string scriptRun(const std::string& src);   // run HRScript from kernel shell / serial (dev mode)
