// Reusable on-screen keyboard (glossy blue pill keys, round shift/delete, green Send) for userspace apps.
#pragma once
#include <algorithm>
#include "app_api.h"

struct KbdEvent { int sp = -1; char ch = 0; };   // sp: -1 none, 0 char, 2 backspace, 4 enter, 6 tab, 7 esc, 8 ^C, 9 ^D, 10 up, 11 down, 12 left, 13 right

class Keyboard {
  struct Key { int x, y, w, h; std::string label; char ch; int sp; int style; };   // sp 1 shift, 3 layer, 5 space (plus event codes)
  // style: 0 blue glossy, 1 light-blue round (shift), 2 red round (delete), 3 green (send), 4 small flat
  std::vector<Key> keys; bool sym = false, shift = false, extra = true; int down = -1, topY = 870, kh = 62; std::string enterLabel = "Send";

  int hit(int px, int py) const { for (size_t i = 0; i < keys.size(); i++) { auto& k = keys[i]; if (px >= k.x && px < k.x + k.w && py >= k.y - 3 && py < k.y + k.h + 3) return (int)i; } return -1; }
  void build() {
    keys.clear(); int y = topY; const int TH = 40, GP = 6;
    if (extra) {
      static const char* L[] = {"esc", "tab", "^C", "^D", "up", "dn", "<", ">"}; static const int S[] = {7, 6, 8, 9, 10, 11, 12, 13};
      for (int i = 0; i < 8; i++) keys.push_back({10 + i * 88, y, 82, TH, L[i], 0, S[i], 4});
      y += TH + GP;
    }
    { std::string d = "1234567890"; int w = 64, g = 6, x0 = (SW - (10 * w + 9 * g)) / 2;       // thin number row
      for (int i = 0; i < 10; i++) keys.push_back({x0 + i * (w + g), y, w, TH, std::string(1, d[i]), d[i], 0, 4});
      y += TH + GP; }
    static const char* R[2][3] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {"(){}[]<>=+", "-*/%_;:!?\"", ".,'#@&|"}};
    for (int r = 0; r < 3; r++) {
      std::string row = R[sym][r]; int n = row.size(), w = 64, g = 6, tot = n * w + (n - 1) * g, x0 = (SW - tot) / 2;
      for (int i = 0; i < n; i++) {
        char c = row[i]; if (!sym && shift && isalpha(c)) c = toupper(c);
        std::string lab(1, c); if (!sym) lab[0] = toupper(c);   // keys always show capitals, like the reference
        keys.push_back({x0 + i * (w + g), y, w, kh, lab, c, 0, 0});
      }
      if (r == 2) { if (!sym) keys.push_back({14, y, 88, kh, "", 0, 1, 1}); keys.push_back({SW - 102, y, 88, kh, "", 0, 2, 2}); }
      y += kh + GP;
    }
    keys.push_back({14, y, 112, kh, sym ? "ABC" : "123", 0, 3, 0}); keys.push_back({134, y, 396, kh, "Space", ' ', 5, 0}); keys.push_back({538, y, 168, kh, enterLabel, 0, 4, 3});
  }
  static void keyShape(M5Canvas& g, const Key& k, bool pressed) {
    if (k.style == 4) { g.fillRoundRect(k.x, k.y, k.w, k.h, 9, pressed ? rgb(40, 90, 200) : rgb(10, 20, 60)); g.drawRoundRect(k.x, k.y, k.w, k.h, 9, rgb(50, 130, 255)); return; }
    uint16_t border, body, gloss, shade;
    switch (k.style) {
      case 1: border = rgb(10, 70, 120); body = rgb(40, 150, 240); gloss = rgb(150, 215, 255); shade = rgb(20, 105, 205); break;
      case 2: border = rgb(100, 10, 20); body = rgb(215, 35, 45); gloss = rgb(255, 130, 125); shade = rgb(155, 15, 30); break;
      case 3: border = rgb(10, 80, 20); body = rgb(40, 185, 55); gloss = rgb(140, 242, 125); shade = rgb(20, 135, 35); break;
      default: border = rgb(6, 14, 90); body = rgb(24, 58, 232); gloss = rgb(92, 140, 255); shade = rgb(10, 30, 175); break;
    }
    if (pressed) { body = shade; gloss = body; }
    int x = k.x, y = k.y, w = k.w, h = k.h, r = (k.style == 1 || k.style == 2) ? std::min(w, h) / 2 : (int)(h * 0.42f);
    g.fillRoundRect(x, y, w, h, r, border);                      // dark rim
    g.fillRoundRect(x + 3, y + 3, w - 6, h - 6, r - 3, body);    // body
    g.fillRoundRect(x + 3, y + h / 2, w - 6, h / 2 - 3, r - 3, shade);   // darker lower half
    if (!pressed) g.fillRoundRect(x + 8, y + 6, w - 16, (int)(h * 0.40f), (int)(r * 0.6f), gloss);   // glossy highlight
  }
 public:
  void layout(int y, int h, bool extraRow) { topY = y; kh = h; extra = extraRow; build(); }
  void setEnter(const char* l) { enterLabel = l; build(); }
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
    for (size_t i = 0; i < keys.size(); i++) {
      const Key& k = keys[i]; bool pr = (int)i == down; int cx = k.x + k.w / 2, cy = k.y + k.h / 2 + (pr && k.style != 4 ? 1 : 0);
      keyShape(g, k, pr);
      if (k.style == 4) {
        uint16_t c = rgb(120, 225, 255);
        if (k.sp >= 10 && k.sp <= 13) {   // arrow keys as triangles
          if (k.sp == 10) g.fillTriangle(cx, cy - 9, cx - 11, cy + 7, cx + 11, cy + 7, c);
          else if (k.sp == 11) g.fillTriangle(cx, cy + 9, cx - 11, cy - 7, cx + 11, cy - 7, c);
          else if (k.sp == 12) g.fillTriangle(cx - 10, cy, cx + 8, cy - 11, cx + 8, cy + 11, c);
          else g.fillTriangle(cx + 10, cy, cx - 8, cy - 11, cx - 8, cy + 11, c);
        } else { uiFont(&g, &fonts::FreeSansBold9pt7b); uiSize(&g, 1); g.setTextDatum(middle_center); g.setTextColor(c); g.drawString(k.label.c_str(), cx, cy); }
      } else if (k.sp == 1) {   // shift: white up arrow
        g.fillTriangle(cx, cy - 17, cx - 17, cy + 2, cx + 17, cy + 2, TFT_WHITE); g.fillRect(cx - 7, cy + 2, 14, 15, TFT_WHITE);
        if (shift) g.fillRect(cx - 17, cy + 22, 34, 4, TFT_WHITE);
      } else if (k.sp == 2) {   // delete: white left arrow
        g.fillTriangle(cx - 20, cy, cx - 2, cy - 15, cx - 2, cy + 15, TFT_WHITE); g.fillRect(cx - 2, cy - 7, 20, 14, TFT_WHITE);
      } else {
        bool letter = k.label.size() == 1; uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, letter ? 1.15f : 1.0f);
        g.setTextDatum(middle_center); g.setTextColor(rgb(5, 10, 60)); g.drawString(k.label.c_str(), cx + 1, cy + 1);   // soft shadow
        g.setTextColor(TFT_WHITE); g.drawString(k.label.c_str(), cx, cy);
      }
    }
  }
};
