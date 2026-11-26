// Reusable on-screen keyboard for userspace apps (touch in, key events out, draws into the app surface).
#pragma once
#include "app_api.h"

struct KbdEvent { int sp = -1; char ch = 0; };   // sp: -1 none, 0 char, 2 backspace, 4 enter, 6 tab, 7 esc, 8 ^C, 9 ^D, 10 up, 11 down, 12 left, 13 right

class Keyboard {
  struct Key { int x, y, w, h; std::string label; char ch; int sp; };   // sp 1 shift, 3 layer, 5 space (plus the event codes above)
  std::vector<Key> keys; bool sym = false, shift = false, extra = true; int down = -1, topY = 870, kh = 58;
  int hit(int px, int py) const { for (size_t i = 0; i < keys.size(); i++) { auto& k = keys[i]; if (px >= k.x && px < k.x + k.w && py >= k.y - 3 && py < k.y + k.h + 3) return (int)i; } return -1; }
  void build() {
    keys.clear(); int y = topY;
    if (extra) {
      static const char* L[] = {"esc", "tab", "^C", "^D", "up", "dn", "<", ">"}; static const int S[] = {7, 6, 8, 9, 10, 11, 12, 13};
      for (int i = 0; i < 8; i++) keys.push_back({8 + i * 88, y, 84, kh, L[i], 0, S[i]});
      y += kh + 5;
    }
    static const char* R[2][4] = {{"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}, {"1234567890", "(){}[];:\"'", "+-*/%=<>!_", ".,?#@&|~$"}};
    for (int r = 0; r < 4; r++) {
      std::string row = R[sym][r]; int n = row.size(), w = 68, tot = n * w + (n - 1) * 6, x0 = (SW - tot) / 2;
      if (r == 3) x0 = 110;
      for (int i = 0; i < n; i++) { char c = row[i]; if (!sym && shift && isalpha(c)) c = toupper(c); keys.push_back({x0 + i * (w + 6), y, w, kh, std::string(1, c), c, 0}); }
      if (r == 3) { keys.push_back({8, y, 96, kh, "shift", 0, 1}); keys.push_back({SW - 104, y, 96, kh, "del", 0, 2}); }
      y += kh + 5;
    }
    keys.push_back({8, y, 130, kh, sym ? "abc" : "123", 0, 3}); keys.push_back({146, y, 420, kh, "space", ' ', 5}); keys.push_back({574, y, 138, kh, "enter", 0, 4});
  }
 public:
  void layout(int y, int h, bool extraRow) { topY = y; kh = h; extra = extraRow; build(); }
  KbdEvent touch(const Touch& t) {
    KbdEvent ev;
    if (t.pressed || t.down) down = hit(t.x, t.y);
    if (t.released) {
      int k = hit(t.x, t.y);
      if (k >= 0 && k == hit(t.sx, t.sy)) {
        Key key = keys[k];
        if (key.sp == 1) { shift = !shift; build(); }
        else if (key.sp == 3) { sym = !sym; build(); }
        else { ev.sp = key.sp == 5 ? 0 : key.sp; ev.ch = key.ch; if ((key.sp == 0 || key.sp == 5) && shift) { shift = false; build(); } }
      }
      down = -1;
    }
    return ev;
  }
  void draw(M5Canvas& g) {
    uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1); g.setTextDatum(middle_center);
    for (size_t i = 0; i < keys.size(); i++) {
      auto& k = keys[i]; bool special = k.sp != 0 && k.sp != 5;
      uint16_t c = (int)i == down ? rgb(120, 120, 130) : special ? rgb(60, 60, 70) : rgb(95, 95, 105);
      if (k.sp == 4) c = (int)i == down ? rgb(60, 160, 255) : rgb(10, 132, 255);
      g.fillRoundRect(k.x, k.y, k.w, k.h, 10, c); g.setTextColor(TFT_WHITE); g.drawString(k.label.c_str(), k.x + k.w / 2, k.y + k.h / 2);
    }
  }
};
