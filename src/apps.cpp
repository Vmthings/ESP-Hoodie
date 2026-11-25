// Apps: Settings (with Developer Mode), Console (HRScript interpreter + kernel shell), Calculator, Clock.
#include <algorithm>
#include <cstdlib>
#include <esp_random.h>
#include <new>
#include <stdexcept>
#include "interp.h"
#include "os.h"

#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static int g_consoleMode = 0;   // 0 = HRScript, 1 = kernel shell

// ================= HRScript host (shared by Console app and `run` in kernel shell) =================
struct Line { std::string s; uint16_t col; };
static std::vector<Line> g_out;
static void emit(const std::string& s, uint16_t col = TFT_WHITE) {
  size_t st = 0;
  while (st <= s.size()) {
    size_t e = s.find('\n', st); if (e == std::string::npos) e = s.size();
    g_out.push_back({s.substr(st, e - st), col}); st = e + 1;
  }
  while (g_out.size() > 300) g_out.erase(g_out.begin());
}

struct ScriptHost {
  hrs::Interp I; M5Canvas* scr = nullptr;
  ScriptHost() {
    using V = std::vector<hrs::Value>; using hrs::Value;
    I.out = [](const std::string& s) { emit(s); };
    auto need = [](V& a, size_t n, const char* sig) { if (a.size() != n) throw std::invalid_argument(std::string("expected ") + sig); };
    auto num = [](V& a, size_t i) -> double { if (a[i].t != Value::NUM) throw std::invalid_argument("argument " + std::to_string(i + 1) + " must be a number"); return a[i].n; };
    auto canvas = [this]() -> M5Canvas& { if (!scr) throw std::runtime_error("no script canvas - open the Console app (script mode)"); return *scr; };
    auto bounds = [this, canvas](const char* fn, int x, int y, int w, int h) {
      M5Canvas& c = canvas();
      if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > c.width() || y + h > c.height())
        throw std::out_of_range(std::string(fn) + "(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h) +
                                ") exceeds the script canvas " + std::to_string(c.width()) + "x" + std::to_string(c.height()));
    };
    I.bind("print", [this](V& a) { std::string o; for (size_t i = 0; i < a.size(); i++) o += (i ? " " : "") + a[i].toString(); I.out(o); return Value(); });
    I.bind("rgb", [=](V& a) { need(a, 3, "rgb(r,g,b)"); return Value::num(C565((int)num(a, 0), (int)num(a, 1), (int)num(a, 2))); });
    I.bind("clear", [=](V& a) { need(a, 1, "clear(color)"); canvas().fillSprite((uint16_t)num(a, 0)); return Value(); });
    I.bind("rect", [=](V& a) { need(a, 5, "rect(x,y,w,h,color)"); int x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3); bounds("rect", x, y, w, h); canvas().fillRect(x, y, w, h, (uint16_t)num(a, 4)); return Value(); });
    I.bind("circle", [=](V& a) { need(a, 4, "circle(x,y,r,color)"); int x = num(a, 0), y = num(a, 1), r = num(a, 2); bounds("circle", x - r, y - r, 2 * r, 2 * r); canvas().fillCircle(x, y, r, (uint16_t)num(a, 3)); return Value(); });
    I.bind("line", [=](V& a) { need(a, 5, "line(x1,y1,x2,y2,color)"); int x1 = num(a, 0), y1 = num(a, 1), x2 = num(a, 2), y2 = num(a, 3); bounds("line", std::min(x1, x2), std::min(y1, y2), abs(x2 - x1), abs(y2 - y1)); canvas().drawLine(x1, y1, x2, y2, (uint16_t)num(a, 4)); return Value(); });
    I.bind("text", [=](V& a) { need(a, 5, "text(x,y,str,size,color)"); int x = num(a, 0), y = num(a, 1); bounds("text", x, y, 0, 0); M5Canvas& c = canvas(); c.setFont(&fonts::FreeSans12pt7b); c.setTextSize(num(a, 3)); c.setTextColor((uint16_t)num(a, 4)); c.setTextDatum(top_left); c.drawString(a[2].toString().c_str(), x, y); return Value(); });
    I.bind("width", [=](V&) { return Value::num(canvas().width()); });
    I.bind("height", [=](V&) { return Value::num(canvas().height()); });
    I.bind("millis", [](V&) { return Value::num(millis()); });
    I.bind("delay", [=](V& a) { need(a, 1, "delay(ms)"); double ms = num(a, 0); if (ms < 0 || ms > 2000) throw std::out_of_range("delay() must be 0..2000 ms"); delay((uint32_t)ms); return Value(); });
    I.bind("rand", [=](V& a) { need(a, 1, "rand(n)"); double n = num(a, 0); if (n < 1) throw std::runtime_error("rand(n) needs n >= 1"); return Value::num(esp_random() % (uint32_t)n); });
    I.bind("str", [](V& a) { return Value::str(a.empty() ? "" : a[0].toString()); });
    I.bind("num", [](V& a) { if (a.size() != 1) throw std::invalid_argument("expected num(x)"); return Value::num(atof(a[0].toString().c_str())); });
    I.bind("len", [](V& a) { if (a.size() != 1 || a[0].t != Value::STR) throw std::invalid_argument("len() needs a string"); return Value::num(a[0].s.size()); });
    I.bind("sqrt", [=](V& a) { need(a, 1, "sqrt(x)"); if (num(a, 0) < 0) throw std::runtime_error("sqrt of a negative number"); return Value::num(sqrt(a[0].n)); });
    I.bind("sin", [=](V& a) { need(a, 1, "sin(x)"); return Value::num(sin(num(a, 0))); });
    I.bind("cos", [=](V& a) { need(a, 1, "cos(x)"); return Value::num(cos(num(a, 0))); });
    I.bind("abs", [=](V& a) { need(a, 1, "abs(x)"); return Value::num(fabs(num(a, 0))); });
    I.bind("floor", [=](V& a) { need(a, 1, "floor(x)"); return Value::num(floor(num(a, 0))); });
    I.bind("heap", [](V&) { return Value::num(ESP.getFreeHeap()); });
    I.bind("psram", [](V&) { return Value::num(ESP.getFreePsram()); });
    I.bind("brightness", [=](V& a) { need(a, 1, "brightness(0..255)"); double b = num(a, 0); if (b < 0 || b > 255) throw std::out_of_range("brightness must be 0..255"); M5.Display.setBrightness((uint8_t)b); return Value(); });
  }
};
static ScriptHost& host() { static ScriptHost h; return h; }

static bool runLine(const std::string& src) {
  hrs::ScriptError e;
  if (host().I.run(src, e, true)) return true;
  emit(e.format(), TFT_RED);
  kernel::log("script %s", e.format().c_str());
  return false;
}
std::string scriptRun(const std::string& src) {
  size_t before = g_out.size(); runLine(src);
  std::string o; for (size_t i = before < g_out.size() ? before : 0; i < g_out.size(); i++) o += g_out[i].s + "\n";
  return o;
}

// ================= Console =================
class Console : public App {
  int mode; M5Canvas* scr = nullptr; std::string in; bool sym = false, shift = false; int down = -1;
  struct Key { int x, y, w, h; std::string label; char ch; int sp; };   // sp: 1 shift, 2 backspace, 3 layer, 4 enter, 5 space
  std::vector<Key> keys;
  void layout() {
    keys.clear(); const int ky = 858, kh = 74;
    static const char* L[2][4] = {{"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}, {"1234567890", "(){}[];:\"'", "+-*/%=<>!_", ".,?#@&|~"}};
    for (int r = 0; r < 4; r++) {
      std::string row = L[sym][r]; int n = row.size(), w = 68, tot = n * w + (n - 1) * 6, x0 = (SW - tot) / 2;
      for (int i = 0; i < n; i++) {
        char ch = row[i]; if (!sym && shift && isalpha(ch)) ch = toupper(ch);
        keys.push_back({x0 + i * (w + 6), ky + r * (kh + 6), w, kh, std::string(1, ch), ch, 0});
      }
    }
    int y = ky + 3 * (kh + 6);
    keys.push_back({8, y, 96, kh, "shift", 0, 1}); keys.push_back({SW - 104, y, 96, kh, "del", 0, 2});
    y += kh + 6;
    keys.push_back({8, y, 130, kh, sym ? "abc" : "123", 0, 3}); keys.push_back({146, y, 420, kh, "space", ' ', 5}); keys.push_back({574, y, 138, kh, "run", 0, 4});
  }
  int hit(int px, int py) { for (size_t i = 0; i < keys.size(); i++) { auto& k = keys[i]; if (px >= k.x && px < k.x + k.w && py >= k.y - 3 && py < k.y + k.h + 3) return i; } return -1; }
  void submit() {
    emit((mode ? "$ " : "> ") + in, rgb(120, 200, 255));
    if (mode) emit(kernel::exec(in)); else runLine(in);
    in.clear();
  }
  void type(const Key& k) {
    if (k.sp == 1) shift = !shift;
    else if (k.sp == 2) { if (!in.empty()) in.pop_back(); }
    else if (k.sp == 3) sym = !sym;
    else if (k.sp == 4) submit();
    else { in += k.ch; shift = false; }
    layout();
  }
 public:
  explicit Console(int m) : mode(m) {}
  const char* name() const override { return "Console"; }
  uint16_t bg() const override { return C565(12, 12, 16); }
  void onLaunch() override {
    layout();
    if (!mode) {
      scr = new M5Canvas(&M5.Display); scr->setPsram(true); scr->setColorDepth(16);
      if (!scr->createSprite(SW, 420)) { delete scr; scr = nullptr; throw std::bad_alloc(); }
      scr->fillSprite(TFT_BLACK); host().scr = scr;
    }
    if (g_out.empty()) emit(mode ? "HRS OS kernel shell - type 'help'" : "HRScript REPL - e.g. let x = 2; print(x*21);", rgb(150, 150, 160));
  }
  void onClose() override { host().scr = nullptr; delete scr; scr = nullptr; }
  void onTouch(const Touch& t) override {
    if (t.pressed || t.down) down = hit(t.x, t.y);
    if (t.pressed && t.y > 60 && t.y < 104 && t.x > SW - 220) { g_consoleMode = mode ? 0 : 1; shell::requestLaunch("Console"); return; }
    if (t.released) { int k = hit(t.x, t.y); if (k >= 0 && k == hit(t.sx, t.sy)) type(keys[k]); down = -1; }
  }
  void onDraw(M5Canvas& g) override {
    g.setTextDatum(middle_left); g.setFont(&fonts::FreeSansBold12pt7b); g.setTextSize(1); g.setTextColor(TFT_WHITE);
    g.drawString(mode ? "Kernel shell" : "HRScript console", 30, 82);
    g.setTextDatum(middle_right); g.setTextColor(rgb(10, 132, 255)); g.drawString(mode ? "switch to script" : "switch to shell", SW - 30, 82);
    int top = 112;
    if (scr) { scr->pushSprite(&g, 0, top); top += 428; }
    int bot = 806;
    g.setFont(&fonts::Font2); g.setTextSize(1.3f); g.setTextDatum(top_left);
    int lh = 26, n = (bot - top) / lh, from = (int)g_out.size() > n ? g_out.size() - n : 0;
    for (size_t i = from; i < g_out.size(); i++) { g.setTextColor(g_out[i].col); g.drawString(g_out[i].s.c_str(), 16, top + (i - from) * lh); }
    g.fillRoundRect(10, bot + 2, SW - 20, 44, 10, C565(40, 40, 48));
    g.setTextColor(TFT_WHITE); g.drawString(((mode ? "$ " : "> ") + in + ((millis() / 500) % 2 ? "_" : "")).c_str(), 24, bot + 12);
    g.setFont(&fonts::FreeSans12pt7b); g.setTextSize(1); g.setTextDatum(middle_center);
    for (size_t i = 0; i < keys.size(); i++) {
      auto& k = keys[i]; bool special = k.sp != 0 && k.sp != 5;
      uint16_t col = (int)i == down ? C565(120, 120, 130) : special ? C565(60, 60, 70) : C565(95, 95, 105);
      if (k.sp == 4) col = (int)i == down ? C565(60, 160, 255) : C565(10, 132, 255);
      g.fillRoundRect(k.x, k.y, k.w, k.h, 12, col); g.setTextColor(TFT_WHITE); g.drawString(k.label.c_str(), k.x + k.w / 2, k.y + k.h / 2);
    }
  }
};

// ================= Settings =================
class Settings : public App {
  float devAnim = 0, fpsAnim = 0; bool sliding = false; uint32_t brightness = 180;
  static constexpr int RH = 76, GX = 30, GW = SW - 60;
  int cy; M5Canvas* G; int tx, ty; bool tap, drag;
  void group(const char* title, int rows) {
    if (G) {
      G->setFont(&fonts::FreeSans9pt7b); G->setTextSize(1); G->setTextColor(rgb(110, 110, 118)); G->setTextDatum(bottom_left); G->drawString(title, GX + 16, cy - 6);
      G->fillRoundRect(GX, cy, GW, rows * RH, 22, TFT_WHITE);
    }
    gy0 = cy; cy += 0;
  }
  int gy0 = 0;
  bool rowHit(int y) { return tap && tx >= GX && tx < GX + GW && ty >= y && ty < y + RH; }
  void label(int y, const char* l, const char* v, uint16_t lc = TFT_BLACK) {
    if (!G) return;
    G->setFont(&fonts::FreeSans12pt7b); G->setTextSize(1); G->setTextColor(lc); G->setTextDatum(middle_left); G->drawString(l, GX + 24, y + RH / 2);
    if (v) { G->setTextColor(rgb(130, 130, 138)); G->setTextDatum(middle_right); G->drawString(v, GX + GW - 24, y + RH / 2); }
    if (y > gy0) G->drawFastHLine(GX + 24, y, GW - 24, rgb(220, 220, 226));
  }
  void toggle(int y, float anim) {
    if (!G) return;
    int x = GX + GW - 100, yy = y + (RH - 44) / 2;
    G->fillRoundRect(x, yy, 76, 44, 22, anim > 0.5f ? rgb(52, 199, 89) : rgb(210, 210, 216));
    G->fillCircle(x + 22 + (int)(32 * anim), yy + 22, 19, TFT_WHITE);
  }
  void pass(M5Canvas* g) {
    G = g; cy = 150; char b[48];
    if (G) { G->setFont(&fonts::FreeSansBold24pt7b); G->setTextSize(1.25f); G->setTextColor(TFT_BLACK); G->setTextDatum(middle_left); G->drawString("Settings", 36, 100); }
    cy += 20; group("DISPLAY", 1); {
      label(cy, "Brightness", nullptr);
      int sx = GX + 250, sw = GW - 290, sy = cy + RH / 2;
      if (G) { G->fillRoundRect(sx, sy - 4, sw, 8, 4, rgb(210, 210, 216)); int kx = sx + sw * brightness / 255; G->fillRoundRect(sx, sy - 4, kx - sx, 8, 4, rgb(10, 132, 255)); G->fillCircle(kx, sy, 17, TFT_WHITE); G->drawCircle(kx, sy, 17, rgb(190, 190, 196)); }
      if (drag && ty >= cy && ty < cy + RH && tx >= sx - 20 && tx <= sx + sw + 20) {
        int v = (tx - sx) * 255 / sw; brightness = v < 10 ? 10 : v > 255 ? 255 : v; M5.Display.setBrightness(brightness);
      }
    }
    cy += RH + 54; group("ABOUT", 5); {
      int y = cy;
      label(y, "Model", "M5Stack Tab5"); y += RH;
      label(y, "Kernel", "HRS OS 0.1"); y += RH;
      snprintf(b, sizeof b, "%d x %d", M5.Display.width(), M5.Display.height()); label(y, "Display", b); y += RH;
      snprintf(b, sizeof b, "%u MB PSRAM free", (unsigned)(ESP.getFreePsram() >> 20)); label(y, "Memory", b); y += RH;
      snprintf(b, sizeof b, "%d%%", (int)M5.Power.getBatteryLevel()); label(y, "Battery", b);
    }
    cy += 5 * RH + 54; group("DEVELOPER", kernel::devMode() ? 6 : 1); {
      int y = cy;
      label(y, "Developer Mode", nullptr); toggle(y, devAnim);
      if (rowHit(y)) kernel::setDevMode(!kernel::devMode());
      if (kernel::devMode()) {
        y += RH; label(y, "HRScript Console", ">"); if (rowHit(y)) { g_consoleMode = 0; shell::requestLaunch("Console"); }
        y += RH; label(y, "Kernel Shell", ">"); if (rowHit(y)) { g_consoleMode = 1; shell::requestLaunch("Console"); }
        y += RH; label(y, "Show FPS", nullptr); toggle(y, fpsAnim); if (rowHit(y)) g_showFps = !g_showFps;
        y += RH; label(y, "Raise test fault", nullptr, rgb(0, 122, 255)); if (rowHit(y)) kernel::fault(kernel::E_APP_FAULT, "Test fault raised from Settings > Developer.");
        y += RH; label(y, "Trigger kernel panic", nullptr, TFT_RED); if (rowHit(y)) kernel::panic(kernel::E_APP_FAULT, "Test panic triggered from Settings > Developer.");
      }
    }
  }
 public:
  const char* name() const override { return "Settings"; }
  bool darkStatusBar() const override { return true; }
  uint16_t bg() const override { return C565(242, 242, 247); }
  void onLaunch() override { devAnim = kernel::devMode(); fpsAnim = g_showFps; }
  void onTouch(const Touch& t) override {
    tx = t.x; ty = t.y; tap = t.released && abs(t.x - t.sx) < 20 && abs(t.y - t.sy) < 20; drag = t.down;
    if (tap || drag) pass(nullptr);
  }
  void onDraw(M5Canvas& g) override {
    devAnim += ((kernel::devMode() ? 1.f : 0.f) - devAnim) * 0.3f; fpsAnim += ((g_showFps ? 1.f : 0.f) - fpsAnim) * 0.3f;
    tap = false; drag = false; pass(&g);
  }
};

// ================= Calculator =================
class Calc : public App {
  double acc = 0; char op = 0; std::string cur = "0"; bool fresh = true; int down = -1;
  static constexpr int B = 156, GAP = 14, X0 = 27, Y0 = 400;
  static const char* L(int i) { static const char* k[] = {"AC", "+/-", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-", "1", "2", "3", "+", "0", ".", "="}; return k[i]; }
  void rect(int i, int& x, int& y, int& w) { int r, c; if (i < 16) { r = i / 4; c = i % 4; w = B; } else if (i == 16) { r = 4; c = 0; w = 2 * B + GAP; } else { r = 4; c = i - 14; w = B; } x = X0 + c * (B + GAP); y = Y0 + r * (B + GAP); }
  int hit(int px, int py) { for (int i = 0; i < 19; i++) { int x, y, w; rect(i, x, y, w); if (px >= x && px < x + w && py >= y && py < y + B) return i; } return -1; }
  static std::string fmt(double v) { if (!std::isfinite(v)) return "Error"; char b[32]; snprintf(b, sizeof b, "%.9g", v); return b; }
  double apply(double a, double b) { if (op == '+') return a + b; if (op == '-') return a - b; if (op == '*') return a * b; if (op == '/') return b == 0 ? NAN : a / b; return b; }
  void press(const std::string& k) {
    if (cur == "Error" && k != "AC") return;
    if (isdigit(k[0])) { if (fresh || cur == "0") { cur = k; fresh = false; } else if (cur.size() < 12) cur += k; }
    else if (k == ".") { if (fresh) { cur = "0."; fresh = false; } else if (cur.find('.') == std::string::npos) cur += "."; }
    else if (k == "AC") { acc = 0; op = 0; cur = "0"; fresh = true; }
    else if (k == "+/-") { if (cur != "0") cur = cur[0] == '-' ? cur.substr(1) : "-" + cur; }
    else if (k == "%") cur = fmt(atof(cur.c_str()) / 100);
    else { double v = atof(cur.c_str()); if (op && !fresh) acc = apply(acc, v); else if (!op) acc = v; cur = fmt(acc); if (k == "=") op = 0; else op = k[0]; fresh = true; }
  }
 public:
  const char* name() const override { return "Calculator"; }
  uint16_t bg() const override { return TFT_BLACK; }
  void onTouch(const Touch& t) override {
    if (t.down) down = hit(t.x, t.y);
    if (t.released) { int a = hit(t.x, t.y); if (a >= 0 && a == hit(t.sx, t.sy)) press(L(a)); down = -1; }
  }
  void onDraw(M5Canvas& g) override {
    g.setFont(&fonts::FreeSansBold24pt7b); g.setTextColor(TFT_WHITE); g.setTextDatum(bottom_right);
    g.setTextSize(cur.size() > 9 ? 1.5f : cur.size() > 6 ? 2.0f : 2.6f); g.drawString(cur.c_str(), SW - 36, Y0 - 30);
    g.setFont(&fonts::FreeSansBold18pt7b); g.setTextSize(1);
    for (int i = 0; i < 19; i++) {
      int x, y, w; rect(i, x, y, w); std::string k = L(i); bool opk = i % 4 == 3 || i == 18, top = i < 3;
      bool active = opk && op && k[0] == op && fresh && k != "=";
      uint16_t c = opk ? (active ? TFT_WHITE : C565(255, 149, 0)) : top ? C565(165, 165, 165) : C565(51, 51, 51);
      if (i == down) c = opk ? C565(255, 200, 120) : top ? C565(220, 220, 220) : C565(115, 115, 115);
      g.fillRoundRect(x, y, w, B, B / 2, c);
      g.setTextColor(top || active ? TFT_BLACK : TFT_WHITE); g.setTextDatum(middle_center);
      std::string lab = k == "*" ? "x" : k == "/" ? "/" : k;
      g.drawString(lab.c_str(), x + (i == 16 ? B / 2 + 30 : w / 2), y + B / 2);
    }
  }
};

// ================= Clock =================
class ClockApp : public App {
 public:
  const char* name() const override { return "Clock"; }
  uint16_t bg() const override { return TFT_BLACK; }
  void onDraw(M5Canvas& g) override {
    int h = 0, m = 0, s = 0; sscanf(timeString(true).c_str(), "%d:%d:%d", &h, &m, &s);
    int cx = SW / 2, cy = 500, R = 290;
    g.fillCircle(cx, cy, R, C565(28, 28, 30)); g.drawCircle(cx, cy, R, C565(70, 70, 74));
    for (int i = 0; i < 60; i++) {
      float a = i * PI / 30; int len = i % 5 == 0 ? 28 : 12;
      g.drawLine(cx + sinf(a) * (R - 8), cy - cosf(a) * (R - 8), cx + sinf(a) * (R - 8 - len), cy - cosf(a) * (R - 8 - len), i % 5 == 0 ? TFT_WHITE : C565(120, 120, 126));
    }
    auto hand = [&](float a, int len, int wd, uint16_t col) { float dx = sinf(a), dy = -cosf(a); for (int o = -wd / 2; o <= wd / 2; o++) g.drawLine(cx + o * dy, cy + o * dx, cx + dx * len + o * dy, cy + dy * len + o * dx, col); };
    hand((h % 12 + m / 60.f) * PI / 6, 150, 12, TFT_WHITE); hand((m + s / 60.f) * PI / 30, 225, 8, TFT_WHITE);
    hand(s * PI / 30, 255, 3, C565(255, 149, 0)); g.fillCircle(cx, cy, 12, C565(255, 149, 0));
    g.setFont(&fonts::FreeSansBold24pt7b); g.setTextSize(1.6f); g.setTextColor(TFT_WHITE); g.setTextDatum(middle_center);
    g.drawString(timeString(true).c_str(), cx, 940);
    uint32_t u = millis() / 1000; char b[48]; snprintf(b, sizeof b, "Uptime %uh %02um %02us", u / 3600, (u / 60) % 60, u % 60);
    g.setFont(&fonts::FreeSans12pt7b); g.setTextSize(1); g.setTextColor(C565(150, 150, 156)); g.drawString(b, cx, 1030);
  }
};

// ================= icon glyphs + registry =================

static void gSettings(M5Canvas& c, int x, int y, int) {
  for (int i = 0; i < 8; i++) { float t = i * PI / 4; c.fillCircle(x + cosf(t) * 40, y + sinf(t) * 40, 13, rgb(225, 225, 230)); }
  c.fillCircle(x, y, 40, rgb(225, 225, 230)); c.fillCircle(x, y, 20, rgb(120, 120, 128)); c.fillCircle(x, y, 13, rgb(200, 200, 206)); }
static void gConsole(M5Canvas& c, int x, int y, int) { c.setFont(&fonts::FreeSansBold18pt7b); c.setTextSize(1.1f); c.setTextColor(rgb(80, 230, 120)); c.setTextDatum(middle_center); c.drawString(">_", x, y); }
static void gCalc(M5Canvas& c, int, int, int) {
  c.fillRoundRect(16, 16, 96, 28, 6, rgb(60, 60, 64));
  for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) c.fillRoundRect(16 + k * 25, 52 + r * 21, 21, 17, 5, k == 3 ? rgb(255, 159, 10) : rgb(100, 100, 106)); }
static void gClock(M5Canvas& c, int x, int y, int) {
  c.fillCircle(x, y, 56, TFT_WHITE);
  for (int i = 0; i < 12; i++) { float t = i * PI / 6; c.drawWideLine(x + sinf(t) * 47, y - cosf(t) * 47, x + sinf(t) * 52, y - cosf(t) * 52, 2, rgb(90, 90, 95)); }
  c.drawWideLine(x, y, x + sinf(PI / 3 + 0.05f) * 28, y - cosf(PI / 3 + 0.05f) * 28, 3, TFT_BLACK);   // hour (~10)
  c.drawWideLine(x, y, x + sinf(0.3f) * 42, y - cosf(0.3f) * 42, 2, TFT_BLACK);                         // minute (~09... decorative)
  c.drawWideLine(x, y, x - sinf(2.4f) * 38, y + cosf(2.4f) * 38, 1, rgb(255, 149, 0)); c.fillCircle(x, y, 4, rgb(255, 149, 0)); }
static void gNotes(M5Canvas& c, int, int, int) {
  c.fillRect(0, 0, 128, 36, rgb(255, 204, 0));
  for (int i = 0; i < 3; i++) c.fillRect(20, 62 + i * 22, 88, 3, rgb(215, 215, 220)); }
static void gWeather(M5Canvas& c, int, int, int) {
  c.fillCircle(44, 42, 24, rgb(255, 214, 10));
  c.fillCircle(48, 84, 16, TFT_WHITE); c.fillCircle(70, 74, 22, TFT_WHITE); c.fillCircle(92, 86, 14, TFT_WHITE); c.fillRect(48, 84, 44, 16, TFT_WHITE); }
static void gPhotos(M5Canvas& c, int x, int y, int) {
  uint16_t cs[] = {rgb(255, 204, 0), rgb(255, 149, 0), rgb(255, 59, 48), rgb(255, 45, 130), rgb(175, 82, 222), rgb(10, 132, 255), rgb(50, 190, 200), rgb(52, 199, 89)};
  for (int i = 0; i < 8; i++) { float t = i * PI / 4; c.fillCircle(x + cosf(t) * 24, y + sinf(t) * 24, 21, cs[i]); } }
static void gCamera(M5Canvas& c, int, int, int) {
  c.fillRoundRect(44, 30, 34, 18, 5, rgb(40, 40, 44)); c.fillRoundRect(18, 42, 92, 62, 11, rgb(40, 40, 44));
  c.fillCircle(64, 74, 24, rgb(205, 205, 210)); c.fillCircle(64, 74, 16, rgb(30, 30, 36)); c.fillCircle(64, 74, 6, rgb(80, 100, 180)); c.fillCircle(97, 54, 4, rgb(255, 204, 0)); }
static void gSafari(M5Canvas& c, int x, int y, int) {
  c.fillCircle(x, y, 54, TFT_WHITE); c.fillCircle(x, y, 49, rgb(30, 140, 255));
  for (int i = 0; i < 12; i++) { float t = i * PI / 6; c.drawWideLine(x + sinf(t) * 42, y - cosf(t) * 42, x + sinf(t) * 47, y - cosf(t) * 47, 2, TFT_WHITE); }
  c.fillTriangle(92, 36, 58, 70, 70, 58, rgb(255, 59, 48)); c.fillTriangle(36, 92, 58, 70, 70, 58, TFT_WHITE); }
static void gMusic(M5Canvas& c, int, int, int) {
  c.fillCircle(46, 90, 14, TFT_WHITE); c.fillCircle(86, 82, 14, TFT_WHITE);
  c.fillRect(56, 38, 8, 52, TFT_WHITE); c.fillRect(96, 30, 8, 52, TFT_WHITE); c.drawWideLine(60, 40, 100, 32, 7, TFT_WHITE); }
static void gMaps(M5Canvas& c, int x, int y, int) {
  c.drawWideLine(10, 100, 118, 40, 10, rgb(255, 255, 255)); c.drawWideLine(24, 30, 70, 120, 7, rgb(255, 214, 10));
  c.fillCircle(x, y - 8, 26, rgb(255, 59, 48)); c.fillTriangle(x - 22, y + 2, x + 22, y + 2, x, y + 42, rgb(255, 59, 48)); c.fillCircle(x, y - 8, 10, TFT_WHITE); }
static void gFiles(M5Canvas& c, int, int, int) {
  c.fillRoundRect(22, 34, 38, 18, 6, rgb(20, 130, 255)); c.fillRoundRect(22, 44, 84, 56, 9, rgb(20, 130, 255)); c.fillRoundRect(22, 54, 84, 46, 9, rgb(70, 170, 255)); }

const AppInfo kApps[] = {
  // dock (first 4), then home grid; Console is last and hidden from the home screen
  {"Settings", C565(190,190,196), C565(120,120,128), gSettings, []() -> App* { return new Settings(); }},
  {"Calculator", C565(70,70,74), C565(28,28,30), gCalc, []() -> App* { return new Calc(); }},
  {"Clock", C565(20,20,22), C565(0,0,0), gClock, []() -> App* { return new ClockApp(); }},
  {"Notes", C565(255,255,255), C565(245,245,248), gNotes, nullptr},
  {"Weather", C565(80,175,255), C565(20,110,230), gWeather, nullptr},
  {"Photos", C565(255,255,255), C565(240,240,245), gPhotos, nullptr},
  {"Camera", C565(205,205,210), C565(125,125,132), gCamera, nullptr},
  {"Safari", C565(255,255,255), C565(235,240,250), gSafari, nullptr},
  {"Music", C565(255,95,110), C565(235,30,70), gMusic, nullptr},
  {"Maps", C565(120,225,140), C565(60,185,110), gMaps, nullptr},
  {"Files", C565(255,255,255), C565(235,240,250), gFiles, nullptr},
  {"Console", C565(40, 40, 46), C565(8, 8, 10), gConsole, []() -> App* { int m = g_consoleMode; return new Console(m); }},
};
const int kAppCount = sizeof(kApps) / sizeof(kApps[0]);
const int kVisibleCount = kAppCount - 1;   // Console: reachable only via Settings > Developer
