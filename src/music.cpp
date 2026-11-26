// Music: plays .mp3 files from the SD card. Userspace app: decoding happens here (libhelix),
// the kernel only provides SD file access and the speaker through sys:: calls.
#include <algorithm>
#include <cstring>
#include "app_api.h"
#include "MP3DecoderHelix.h"
using namespace libhelix;

class Music : public App {
  static constexpr int ROW = 84, LIST_Y = 150, LIST_H = 730;
  static constexpr size_t CH = 9216;   // samples per audio buffer (~100 ms of 44.1 kHz stereo)
  std::vector<std::string> files; std::string status;
  bool sdOk = false, playing = false, paused = false, eof = false, gotPcm = false, dragging = false;
  int cur = -1, vol = 160, fh = -1, lastY = 0, ringIdx = 0; float scroll = 0;
  size_t fsize = 0; uint32_t rate = 44100, played = 0; bool stereo = true;
  MP3DecoderHelix* dec = nullptr; std::vector<int16_t> acc; int16_t* ring[3] = {nullptr, nullptr, nullptr};
  uint8_t buf[2048];

  static std::string title(const std::string& p) {
    size_t s = p.find_last_of('/'); std::string n = s == std::string::npos ? p : p.substr(s + 1);
    if (n.size() > 4) n = n.substr(0, n.size() - 4);
    return n;
  }
  static std::string clipS(std::string s, size_t n) { if (s.size() > n) s = s.substr(0, n - 1) + "~"; return s; }
  static void pcmCb(MP3FrameInfo& i, short* pcm, size_t len, void* ref) { ((Music*)ref)->onPcm(i, pcm, len); }
  void onPcm(MP3FrameInfo& i, short* pcm, size_t len) {
    if (i.samprate > 0) rate = i.samprate;
    stereo = i.nChans == 2; gotPcm = true;
    acc.insert(acc.end(), pcm, pcm + len);
  }
  void clampScroll() { float mx = fmaxf(0, files.size() * ROW - LIST_H); if (scroll < 0) scroll = 0; if (scroll > mx) scroll = mx; }

  void rescan() {
    stopTrack(); cur = -1; scroll = 0; status.clear();
    sdOk = sys::sdMount(true);
    files.clear(); if (sdOk) { files = sys::listMp3(); std::sort(files.begin(), files.end()); }
  }
  void stopTrack() {
    playing = false; paused = false;
    if (dec) { dec->end(); delete dec; dec = nullptr; }
    if (fh >= 0) { sys::fileClose(fh); fh = -1; }
    sys::audioStop(); acc.clear();
  }
  void startTrack(int i) {
    if (i < 0 || i >= (int)files.size()) return;
    stopTrack(); cur = i; status.clear(); eof = false; gotPcm = false; played = 0; ringIdx = 0;
    fh = sys::fileOpen(files[i].c_str());
    if (fh < 0) { status = "Can't open this file"; return; }
    fsize = sys::fileSize(fh);
    uint8_t h[10]; size_t n = sys::fileRead(fh, h, 10), skip = 0;   // skip an ID3v2 tag
    if (n == 10 && !memcmp(h, "ID3", 3)) skip = 10 + (((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f));
    sys::fileSeek(fh, skip);
    dec = new MP3DecoderHelix(pcmCb); dec->setReference(this); dec->begin();
    playing = true;
  }
  void next() { if (!files.empty()) startTrack((cur + 1) % files.size()); }
  void prev() { if (files.empty()) return; if (played / (rate ? rate : 1) > 3) startTrack(cur); else startTrack((cur + files.size() - 1) % files.size()); }
  void togglePause() {
    if (cur < 0) { startTrack(0); return; }
    if (!playing) { startTrack(cur); return; }
    paused = !paused; if (paused) sys::audioStop();
  }

  void drawGlyphs(M5Canvas& g, uint16_t red) {
    int cy = 1110;
    g.fillTriangle(190, cy, 150, cy - 24, 150, cy + 24, TFT_BLACK); g.fillRect(130, cy - 24, 8, 48, TFT_BLACK);       // prev
    g.fillCircle(360, cy, 46, red);
    if (playing && !paused) { g.fillRect(343, cy - 22, 12, 44, TFT_WHITE); g.fillRect(365, cy - 22, 12, 44, TFT_WHITE); }
    else g.fillTriangle(350, cy - 26, 350, cy + 26, 384, cy, TFT_WHITE);
    g.fillTriangle(530, cy, 570, cy - 24, 570, cy + 24, TFT_BLACK); g.fillRect(582, cy - 24, 8, 48, TFT_BLACK);       // next
  }

 public:
  const char* name() const override { return "Music"; }
  bool darkStatusBar() const override { return true; }
  uint16_t bg() const override { return TFT_WHITE; }
  int idleMs() const override { return playing && !paused ? 500 : 60000; }
  int tickMs() const override { return playing && !paused ? 8 : 0; }
  void onLaunch() override {
    for (auto& r : ring) r = (int16_t*)malloc(CH * sizeof(int16_t));
    sys::setVolume(vol);
    sdOk = sys::sdMount(); if (sdOk) { files = sys::listMp3(); std::sort(files.begin(), files.end()); }
  }
  void onClose() override { stopTrack(); for (auto& r : ring) { free(r); r = nullptr; } }

  void onTick() override {
    if (!playing || paused) return;
    for (int guard = 0; guard < 6; guard++) {
      if (acc.size() >= CH || (eof && !acc.empty())) {          // hand a full buffer to the speaker
        if (!sys::audioReady()) break;
        size_t n = std::min(acc.size(), CH);
        memcpy(ring[ringIdx], acc.data(), n * sizeof(int16_t));
        if (!sys::audioPlay(ring[ringIdx], n, rate, stereo)) break;
        played += n / (stereo ? 2 : 1); ringIdx = (ringIdx + 1) % 3;
        acc.erase(acc.begin(), acc.begin() + n); continue;
      }
      if (eof) {
        if (acc.empty() && !sys::audioBusy()) { if (!gotPcm) { sys::log("music: no audio decoded from %s", files[cur].c_str()); status = "Unsupported file"; } next(); }
        break;
      }
      size_t n = sys::fileRead(fh, buf, sizeof buf);             // feed the decoder
      if (n == 0) { eof = true; continue; }
      size_t off = 0; while (off < n) { size_t w = dec->write(buf + off, n - off); if (w == 0) break; off += w; }
    }
  }

  void onTouch(const Touch& t) override {
    if (t.pressed) { dragging = t.y > LIST_Y && t.y < LIST_Y + LIST_H; lastY = t.y; }
    if (t.down && dragging) { scroll -= (t.y - lastY); lastY = t.y; clampScroll(); }
    if (t.down && t.sy >= 1170 && t.sy <= 1245 && t.x >= 120 && t.x <= 660) { vol = (t.x - 120) * 255 / 540; sys::setVolume(vol); }
    if (!t.released) return;
    if (abs(t.x - t.sx) > 18 || abs(t.y - t.sy) > 18) return;   // it was a drag, not a tap
    if (t.sy < 140 && t.sx > SW - 220) { rescan(); return; }
    if (t.sy >= LIST_Y && t.sy < LIST_Y + LIST_H) { int i = (int)((t.sy - LIST_Y + scroll) / ROW); if (i >= 0 && i < (int)files.size()) startTrack(i); return; }
    if (t.sy > 1050 && t.sy < 1170) {
      if (abs(t.sx - 360) < 70) togglePause();
      else if (abs(t.sx - 160) < 70) prev();
      else if (abs(t.sx - 560) < 70) next();
    }
  }

  void onDraw(M5Canvas& g) override {
    const uint16_t red = rgb(252, 60, 68), grey = rgb(130, 130, 138);
    uiFont(&g, &fonts::FreeSansBold24pt7b); uiSize(&g, 1.25f); g.setTextColor(TFT_BLACK); g.setTextDatum(middle_left); g.drawString("Music", 36, 100);
    uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1); g.setTextColor(red); g.setTextDatum(middle_right); g.drawString("Rescan", SW - 36, 100);
    // ---- track list ----
    g.setClipRect(0, LIST_Y, SW, LIST_H);
    if (!sdOk || files.empty()) {
      g.setTextDatum(middle_center); g.setTextColor(grey); uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1);
      g.drawString(!sdOk ? "No SD card" : "No .mp3 files found", SW / 2, 420);
      uiFont(&g, &fonts::FreeSans9pt7b); uiSize(&g, 1.1f);
      g.drawString(!sdOk ? "Insert an SD card with .mp3 files, then tap Rescan" : "Copy .mp3 files to the card, then tap Rescan", SW / 2, 470);
    } else {
      uiFont(&g, &fonts::FreeSans12pt7b); uiSize(&g, 1); g.setTextDatum(middle_left);
      int first = (int)(scroll / ROW), last = (int)((scroll + LIST_H) / ROW) + 1;
      for (int i = first; i <= last && i < (int)files.size(); i++) {
        int y = LIST_Y + i * ROW - (int)scroll;
        bool isCur = i == cur;
        g.setTextColor(isCur ? red : TFT_BLACK); g.drawString(clipS(title(files[i]), 34).c_str(), 80, y + ROW / 2);
        if (isCur && playing && !paused) for (int b = 0; b < 3; b++) { int hgt = 10 + (int)((millis() / 120 + b * 7) % 5) * 5; g.fillRect(36 + b * 12, y + ROW / 2 + 14 - hgt, 8, hgt, red); }
        else if (isCur) g.fillTriangle(36, y + ROW / 2 - 12, 36, y + ROW / 2 + 12, 56, y + ROW / 2, red);
        g.drawFastHLine(80, y + ROW - 1, SW - 100, rgb(225, 225, 230));
      }
    }
    g.clearClipRect();
    // ---- player card ----
    g.fillRoundRect(20, 900, 680, 340, 28, rgb(242, 242, 247));
    g.fillRoundRect(44, 924, 150, 150, 22, red); g.fillCircle(100, 1030, 14, TFT_WHITE); g.fillCircle(140, 1022, 14, TFT_WHITE); g.fillRect(110, 958, 6, 74, TFT_WHITE); g.fillRect(150, 950, 6, 74, TFT_WHITE); g.fillRect(110, 950, 46, 12, TFT_WHITE);
    uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1); g.setTextColor(TFT_BLACK); g.setTextDatum(middle_left);
    g.drawString(cur >= 0 ? clipS(title(files[cur]), 26).c_str() : "Not playing", 220, 960);
    uiFont(&g, &fonts::FreeSans9pt7b); uiSize(&g, 1.1f); g.setTextColor(status.empty() ? grey : red);
    char b[32]; uint32_t s = rate ? played / rate : 0; snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    g.drawString(status.empty() ? (playing ? (paused ? "Paused" : b) : "Tap a track to play") : status.c_str(), 220, 1005);
    int px = 44, pw = 632, py = 1048; g.fillRoundRect(px, py, pw, 8, 4, rgb(210, 210, 216));
    if (fh >= 0 && fsize) { float f = (float)sys::filePos(fh) / fsize; if (f > 1) f = 1; g.fillRoundRect(px, py, (int)(pw * f), 8, 4, red); g.fillCircle(px + (int)(pw * f), py + 4, 9, red); }
    drawGlyphs(g, red);
    g.fillCircle(70, 1210, 10, grey); g.fillRect(70, 1200, 14, 20, grey);   // speaker glyph
    g.fillRoundRect(120, 1206, 540, 8, 4, rgb(210, 210, 216)); int kx = 120 + 540 * vol / 255;
    g.fillRoundRect(120, 1206, kx - 120, 8, 4, red); g.fillCircle(kx, 1210, 14, TFT_WHITE); g.drawCircle(kx, 1210, 14, rgb(190, 190, 196));
  }
};
App* makeMusic() { return new Music(); }
