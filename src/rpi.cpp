// Raspberry Pi control (Developer Mode): serial terminal + login to a Pi on the USB-A port.
// The Pi must expose a USB serial console (gadget mode: dwc2 + g_serial + serial-getty@ttyGS0).
#include <algorithm>
#include <cstring>
#include "app_api.h"
#include "kbd.h"

class Rpi : public App {
  enum St { FORM, WAIT_LOGIN, WAIT_PASS, WAIT_RESULT, TERM };
  static constexpr int COLS = 59, TOP = 110, KB_Y = 896;
  St st = FORM; Keyboard kb; std::string user = "pi", pass, msg, info, recent; int focus = 0, pokes = 0, scrollBack = 0, lastY = 0;
  bool opened = false, power = false, termDrag = false; uint32_t stT = 0;
  std::vector<std::string> lines; std::string cur, escBuf; int col = 0, esc = 0;

  // ---------- tiny terminal emulator ----------
  void newline() { lines.push_back(cur); cur.clear(); col = 0; if (lines.size() > 400) lines.erase(lines.begin()); }
  void put(char c) { if (col < (int)cur.size()) cur[col] = c; else cur += c; col++; if (col >= COLS) newline(); }
  void csi(char f) {
    int p = escBuf.empty() ? 0 : atoi(escBuf.c_str()); if (p < 1 && (f == 'C' || f == 'D')) p = 1;
    if (f == 'K') { if (p == 2) cur.clear(); else if (col < (int)cur.size()) cur.resize(col); }
    else if (f == 'J') { if (p >= 2) { lines.clear(); cur.clear(); col = 0; } }
    else if (f == 'C') { col += p; if (col > COLS - 1) col = COLS - 1; while ((int)cur.size() < col) cur += ' '; }
    else if (f == 'D') { col = std::max(0, col - p); }
  }
  void feed(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
      uint8_t c = d[i];
      if (esc == 1) { if (c == '[') { esc = 2; escBuf.clear(); } else if (c == ']') esc = 3; else esc = 0; continue; }
      if (esc == 2) { if (c >= 0x40 && c <= 0x7e) { csi((char)c); esc = 0; } else escBuf += (char)c; continue; }
      if (esc == 3) { if (c == 7) esc = 0; else if (c == 0x1b) esc = 1; continue; }
      if (c == 0x1b) esc = 1;
      else if (c == '\n') newline();
      else if (c == '\r') col = 0;
      else if (c == 8 || c == 0x7f) { if (col > 0) col--; }
      else if (c == '\t') { do put(' '); while (col % 8); }
      else if (c >= 0x20 && c < 0x7f) { put((char)c); recent += (char)tolower(c); if (recent.size() > 300) recent.erase(0, recent.size() - 300); }
      else if (c >= 0xC0) put('?');
    }
  }
  // ---------- connection + login ----------
  void send(const std::string& s) { sys::usbSerialWrite((const uint8_t*)s.data(), s.size()); }
  void wipePass() { std::fill(pass.begin(), pass.end(), '\0'); pass.clear(); pass.shrink_to_fit(); }   // password lives in RAM only, briefly
  void fail(const char* m) { msg = m; st = FORM; sys::usbSerialClose(); opened = false; }
  bool promptSeen() const { size_t n = recent.size(); if (!n) return false; char a = recent[n - 1], b = n > 1 ? recent[n - 2] : ' '; return a == '$' || a == '#' || (a == ' ' && (b == '$' || b == '#')); }
  bool connect(bool withLogin) {
    msg.clear();
    if (!sys::usbSerialOpen(115200)) { msg = sys::usbSerialInfo(); return false; }
    opened = true; lines.clear(); cur.clear(); col = 0; recent.clear(); pokes = 0; scrollBack = 0; stT = millis();
    st = withLogin ? WAIT_LOGIN : TERM; send("\r"); return true;
  }
  void doKey(const KbdEvent& e) {
    if (e.sp < 0) return;
    if (st == TERM) {   // pass keys straight through to the Pi
      static const char* seq[] = {"", "", "\x7f", "", "\r", "", "\t", "\x1b", "\x03", "\x04", "\x1b[A", "\x1b[B", "\x1b[D", "\x1b[C"};
      if (e.sp == 0) send(std::string(1, e.ch)); else if (e.sp <= 13) send(seq[e.sp]);
      scrollBack = 0; return;
    }
    if (st != FORM) return;
    std::string& f = focus == 0 ? user : pass;
    if (e.sp == 0 && f.size() < 40) f += e.ch;
    else if (e.sp == 2 && !f.empty()) f.pop_back();
    else if (e.sp == 4) { if (focus == 0) focus = 1; else connect(true); }
  }

 public:
  const char* name() const override { return "RPi"; }
  uint16_t bg() const override { return rgb(8, 8, 12); }
  int idleMs() const override { return 400; }
  int tickMs() const override { return opened ? 20 : 400; }
  void onLaunch() override { kb.layout(KB_Y, 62, true); kb.setEnter("Send"); info = sys::usbSerialInfo(); }
  void onClose() override { sys::usbSerialClose(); wipePass(); sys::usbPower(false); }

  void onTick() override {
    if (!opened) { info = sys::usbSerialInfo(); return; }
    if (sys::usbSerialState() != 2) { fail("Device disconnected"); return; }
    uint8_t b[256]; size_t n; bool got = false;
    while ((n = sys::usbSerialRead(b, sizeof b)) > 0) { feed(b, n); got = true; if (lines.size() > 380) break; }
    (void)got;
    auto has = [&](const char* s) { return recent.find(s) != std::string::npos; };
    uint32_t el = millis() - stT;
    if (st == WAIT_LOGIN) {
      if (has("login:")) { send(user + "\r"); recent.clear(); st = WAIT_PASS; stT = millis(); }
      else if (promptSeen()) st = TERM;                                  // already logged in
      else if (el > 2500 && pokes == 0) { send("\r"); pokes++; }
      else if (el > 15000) fail("No login prompt. Is the Pi booted with serial getty enabled?");
    } else if (st == WAIT_PASS) {
      if (has("password:")) { send(pass + "\r"); wipePass(); recent.clear(); st = WAIT_RESULT; stT = millis(); }
      else if (el > 10000) fail("No password prompt");
    } else if (st == WAIT_RESULT) {
      if (has("login incorrect")) fail("Login incorrect");
      else if (promptSeen() || has("last login")) { st = TERM; recent.clear(); }
      else if (el > 12000) st = TERM;                                   // custom prompt: just give the terminal
    }
  }

  void onTouch(const Touch& t) override {
    if (opened) { if (t.pressed && t.y < 100 && t.x > SW - 220) { fail("Disconnected"); return; } }
    if (st == TERM && t.pressed) { termDrag = t.y > TOP && t.y < KB_Y - 4; lastY = t.y; }
    if (st == TERM && t.down && termDrag) { scrollBack += (t.y - lastY) / 16; lastY = lastY + ((t.y - lastY) / 16) * 16; if (scrollBack < 0) scrollBack = 0; if (scrollBack > (int)lines.size()) scrollBack = lines.size(); }
    if (st == FORM && t.released && abs(t.x - t.sx) < 20 && abs(t.y - t.sy) < 20) {
      if (t.sx > 40 && t.sx < 680) {
        if (t.sy > 300 && t.sy < 370) focus = 0;
        else if (t.sy > 400 && t.sy < 470) focus = 1;
        else if (t.sy > 500 && t.sy < 580) connect(true);
        else if (t.sy > 596 && t.sy < 660) connect(false);
        else if (t.sy > 676 && t.sy < 740) { power = !power; sys::usbPower(power); }
      }
    }
    doKey(kb.touch(t));
  }

  void onDraw(M5Canvas& g) override {
    const uint16_t gr = rgb(80, 255, 120), dim = rgb(120, 130, 125);
    uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1); g.setTextDatum(middle_left); g.setTextColor(TFT_WHITE); g.drawString("Raspberry Pi", 24, 80);
    uiFont(&g, &fonts::Font2); uiSize(&g, 1); g.setTextDatum(middle_right);
    if (opened) { g.drawRoundRect(SW - 200, 62, 188, 36, 6, gr); g.setTextColor(gr); g.setTextDatum(middle_center); g.drawString("DISCONNECT", SW - 106, 80); }
    else { g.setTextColor(info.find("ready") != std::string::npos ? gr : dim); g.drawString(info.substr(0, 46).c_str(), SW - 12, 80); }
    if (st == TERM) {
      uiFont(&g, &fonts::Font0); uiSize(&g, 2); g.setTextDatum(top_left); g.setTextColor(gr);
      int rows = (KB_Y - 6 - TOP) / 16, total = (int)lines.size() + 1, first = std::max(0, total - rows - scrollBack);
      for (int r = 0; r < rows && first + r < total; r++) {
        int i = first + r; const std::string& s = i < (int)lines.size() ? lines[i] : cur;
        g.drawString(s.c_str(), 6, TOP + r * 16);
        if (i == (int)lines.size() && scrollBack == 0 && (millis() / 500) % 2) g.fillRect(6 + col * 12, TOP + r * 16, 10, 14, gr);
      }
    } else {
      g.fillRoundRect(20, 112, SW - 40, 180, 16, rgb(24, 26, 30));
      uiFont(&g, &fonts::FreeSans9pt7b); uiSize(&g, 1.05f); g.setTextDatum(top_left); g.setTextColor(dim);
      g.drawString("Pi setup: dtoverlay=dwc2 (config.txt), modules-load=dwc2,g_serial", 36, 124);
      g.drawString("(cmdline.txt), then: systemctl enable serial-getty@ttyGS0", 36, 152);
      g.drawString("Use a DATA cable from this USB-A port to the Pi's USB", 36, 190);
      g.drawString("data port. Power the Pi separately (or use the 5V switch).", 36, 218);
      g.setTextColor(st == FORM ? (msg.empty() ? dim : rgb(255, 90, 80)) : gr);
      std::string s = st == FORM ? msg : st == WAIT_LOGIN ? "Waiting for the login prompt..." : st == WAIT_PASS ? "Sending password..." : "Checking login...";
      g.drawString(s.c_str(), 36, 258);
      auto field = [&](int y, const char* lab, const std::string& v, bool hide, bool f) {
        g.fillRoundRect(40, y, 640, 70, 12, rgb(30, 32, 38)); g.drawRoundRect(40, y, 640, 70, 12, f ? gr : rgb(70, 72, 80));
        uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1); g.setTextDatum(middle_left); g.setTextColor(dim); g.drawString(lab, 58, y + 35);
        std::string shown = hide ? std::string(v.size(), '*') : v; if (f && (millis() / 500) % 2) shown += "_";
        g.setTextColor(TFT_WHITE); g.drawString(shown.c_str(), 230, y + 35);
      };
      field(300, "Username", user, false, focus == 0); field(400, "Password", pass, true, focus == 1);
      auto btn = [&](int y, int h, const char* l, uint16_t c) { g.fillRoundRect(40, y, 640, h, 14, c); uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1); g.setTextDatum(middle_center); g.setTextColor(TFT_WHITE); g.drawString(l, SW / 2, y + h / 2); };
      btn(500, 80, "Connect & log in", rgb(10, 132, 255)); btn(596, 64, "Open terminal (no login)", rgb(60, 62, 70));
      btn(676, 64, power ? "USB-A 5V output: ON" : "USB-A 5V output: off", power ? rgb(200, 120, 20) : rgb(60, 62, 70));
    }
    kb.draw(g);
  }
};
App* makeRpi() { return new Rpi(); }
