// KERNEL: process manager + compositor. Apps (including Springboard) run as separate FreeRTOS tasks,
// draw into their own surfaces, and are composited here. Status bar / home indicator / alerts belong to the kernel.
#include <cstring>
#include <mutex>
#include "os.h"

bool g_showFps = false;
static M5Canvas fb(&M5.Display);
static Process *sb = nullptr, *fg = nullptr;
enum St { HOME, OPENING, APP, CLOSING };
static St st = HOME;
static Spring openS; static Touch tc; static float prevY = 0;
static int zIx = SW / 2 - 64, zIy = SH - 300, zSz = 128;
struct Pend { bool valid = false; char name[24] = ""; char arg[24] = ""; int idx = -1; bool zoom = false; int ix = 0, iy = 0, sz = 128; };
static Pend pend; static char pendPanic[160] = "";
static bool pendClose = false, touchWarned = false, alertOn = false;
static kernel::Fault alertF;
static std::mutex snapMx; static std::vector<sys::ProcInfo> procSnap; static sys::RuntimeInfo rtSnap;
static uint32_t presentCount = 0, rtFps = 0, killCount = 0, snapT = 0; static float rtWork = 0;
static int nextPid = 100;
static uint32_t lastUs = 0, fpsT = 0; static int fpsN = 0, fpsShown = 0;

void makeSprite(M5Canvas& c, int w, int h, int depth, const char* what) {
  if (w > SW || h > SH) kernel::panic(kernel::E_SURFACE_TOO_LARGE, "%s needs a %dx%d surface but the display is only %dx%d.", what, w, h, SW, SH);
  c.setPsram(true); c.setColorDepth(depth);
  if (!c.createSprite(w, h)) kernel::panic(kernel::E_OOM, "Cannot allocate %s (%dx%d, %d bpp). PSRAM free: %u bytes.", what, w, h, depth, (unsigned)ESP.getFreePsram());
}

// ---------------- processes ----------------
static void procEntry(void* arg) {
  Process* p = (Process*)arg; setCurrentProcess(p);
  try {
    p->beat = millis(); p->app->onLaunch(); p->beat = millis();
    Event e;
    for (;;) {
      if (xQueueReceive(p->q, &e, portMAX_DELAY) != pdTRUE) continue;
      p->beat = millis();
      if (e.type == 2) break;
      if (e.type == 0) p->app->onTouch(e.t);
      else if (e.type == 3) p->app->onCommand(e.cmd);
      else if (e.type == 1) {
        int b = 1 - p->frontIdx; p->surf[b]->fillSprite(p->app->bg());
        p->app->onDraw(*p->surf[b]);
        p->frontIdx = b; p->hasFrame = true; p->drawPending = false; p->lastDraw = millis(); p->frameSeq++;
      }
      p->beat = millis();
    }
    p->app->onClose();
  } catch (std::exception& ex) { kernel::log("CRASH 0x%04X %s: %s", (unsigned)kernel::E_APP_FAULT, p->name.c_str(), ex.what()); p->crashed = true; }
  catch (...) { kernel::log("CRASH 0x%04X %s: unknown exception", (unsigned)kernel::E_APP_FAULT, p->name.c_str()); p->crashed = true; }
  p->exited = true; p->task = nullptr;
  vTaskDelete(nullptr);
}
static void reap(Process* p) {
  if (!p) return;
  if (!p->exited && p->task) {
    Event e{}; e.type = 2; xQueueSend(p->q, &e, 0);
    for (int i = 0; i < 30 && !p->exited; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!p->exited) { kernel::log("force-killing %s (not responding)", p->name.c_str()); vTaskDelete(p->task); }
  }
  delete p->app;
  for (auto c : p->owned) kernelFreeCanvas(c);
  if (p->q) vQueueDelete(p->q);
  delete p->surf[0]; delete p->surf[1];
  delete p;
}
static Process* spawnApp(App* app, const char* name, int idx, const char* arg, bool system) {
  Process* p = new Process(); p->app = app; p->name = name; p->idx = idx; p->arg = arg ? arg : ""; p->system = system;
  p->quota = (system ? 20u : 12u) * 1024 * 1024; p->pid = nextPid++;
  for (int i = 0; i < 2; i++) {
    p->surf[i] = new M5Canvas(&M5.Display); p->surf[i]->setPsram(true); p->surf[i]->setColorDepth(16);
    if (!p->surf[i]->createSprite(SW, SH)) {
      kernel::fault(kernel::E_OOM, "Cannot start %s: out of PSRAM (%u bytes free).", name, (unsigned)ESP.getFreePsram());
      p->exited = true; reap(p); return nullptr;
    }
    p->surf[i]->setPivot(SW / 2.0f, SH / 2.0f);
  }
  p->surfBytes = 2 * (size_t)SW * SH * 2;
  p->q = xQueueCreate(32, sizeof(Event)); p->beat = millis();
  if (xTaskCreate(procEntry, name, app->stackBytes(), p, 2, &p->task) != pdPASS) {
    kernel::fault(kernel::E_OOM, "Cannot start %s: no memory for its task stack.", name); p->exited = true; reap(p); return nullptr;
  }
  kernel::log("spawn %s", name);
  return p;
}
static int findApp(const std::string& n) {
  std::string q = n; for (auto& c : q) c = tolower(c);
  for (int i = 0; i < kAppCount; i++) { std::string a = kApps[i].name; for (auto& c : a) c = tolower(c); if (a == q) return i; }
  return -1;
}
static void launchApp(int idx, const char* arg, bool zoom, int ix, int iy, int sz) {
  if (idx < 0 || idx >= kAppCount) return;
  if (!kApps[idx].make) { kernel::fault(kernel::E_APP_NOT_INSTALLED, "%s is not implemented in this build yet.", kApps[idx].name); return; }
  if (fg) { reap(fg); fg = nullptr; }
  fg = spawnApp(kApps[idx].make(), kApps[idx].name, idx, arg, false);
  if (!fg) { st = HOME; return; }
  if (zoom) { zIx = ix; zIy = iy; zSz = sz; openS.snap(0); openS.target = 1; st = OPENING; }
  else { zIx = SW / 2 - 64; zIy = SH - 300; zSz = 128; openS.snap(1); st = APP; }
}
static void postTouch(Process* p) {
  if (!p || p->exited || !(tc.down || tc.pressed || tc.released)) return;
  p->dirty = true;
  Event e{}; e.type = 0; e.t = tc;
  xQueueSend(p->q, &e, (tc.pressed || tc.released) ? 5 : 0);   // never drop press/release
}
static void requestDraw(Process* p) {   // draw only when dirty, animating, or the idle timer expired
  if (!p || p->exited || p->drawPending) return;
  bool want = p->dirty || !p->hasFrame || p->app->animating() || millis() - p->lastDraw >= (uint32_t)p->app->idleMs();
  if (!want) return;
  p->dirty = false; p->drawPending = true; Event e{}; e.type = 1;
  if (xQueueSend(p->q, &e, 0) != pdTRUE) { p->drawPending = false; p->dirty = true; }
}

std::string shell::psText() {
  std::string o = "PID NAME          STATE   STACK-FREE  SURFACES\n"; char b[96];
  auto row = [&](Process* p, int pid) {
    if (!p) return;
    snprintf(b, sizeof b, "%3d %-13s %-7s %8u  %6u KB\n", pid, p->name.c_str(), p->exited ? "exited" : (p == fg && st == APP) ? "fg" : "bg",
             p->task ? (unsigned)uxTaskGetStackHighWaterMark(p->task) : 0u, (unsigned)(p->surfBytes >> 10));
    o += b;
  };
  row(sb, 1); row(fg, 2);
  snprintf(b, sizeof b, "heap %u  psram %u free", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram()); o += b;
  return o;
}
void shell::requestLaunch(const char* n, const char* a) { snprintf(pend.name, sizeof pend.name, "%s", n); snprintf(pend.arg, sizeof pend.arg, "%s", a ? a : ""); pend.idx = -1; pend.zoom = false; pend.valid = true; }
void shell::requestLaunchZoom(int i, int x, int y, int s) { pend.name[0] = 0; pend.arg[0] = 0; pend.idx = i; pend.zoom = true; pend.ix = x; pend.iy = y; pend.sz = s; pend.valid = true; }
void shell::requestPanic(const char* m) { if (kernel::devMode()) snprintf(pendPanic, sizeof pendPanic, "%s", m); else kernel::log("panic request denied (Developer Mode is off)"); }

// ---------------- compositor UI ----------------
static void statusBar(M5Canvas& g, bool dark) {
  uint16_t c = dark ? TFT_BLACK : TFT_WHITE;
  g.setTextColor(c); uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1);
  g.setTextDatum(middle_left); g.drawString(sys::timeString().c_str(), 40, 30);
  int lvl = M5.Power.getBatteryLevel(); if (lvl < 0) lvl = 0; if (lvl > 100) lvl = 100;
  int bx = SW - 100, by = 20;
  g.drawRoundRect(bx, by, 52, 24, 7, c); g.fillRect(bx + 53, by + 8, 3, 8, c);
  g.fillRoundRect(bx + 3, by + 3, (46 * lvl) / 100, 18, 4, lvl <= 20 ? TFT_RED : c);
  char b[8]; snprintf(b, sizeof b, "%d%%", lvl);
  uiFont(&g, &fonts::FreeSans9pt7b); g.setTextDatum(middle_right); g.drawString(b, bx - 10, 30);
}
static void homeIndicator(M5Canvas& g, bool dark) { g.fillRoundRect(SW / 2 - 120, SH - 22, 240, 9, 4, dark ? TFT_BLACK : TFT_WHITE); }
static void wrapText(M5Canvas& g, const std::string& s, int x, int y, int w, int lh) {
  std::string line, word; int cy = y;
  auto flush = [&]() { g.drawString(line.c_str(), x, cy); cy += lh; line.clear(); };
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == ' ' || s[i] == '\n') {
      std::string t = line.empty() ? word : line + " " + word;
      if (!line.empty() && g.textWidth(t.c_str()) > w) { flush(); line = word; } else line = t;
      word.clear(); if (i < s.size() && s[i] == '\n') flush();
    } else word += s[i];
  }
  if (!line.empty()) flush();
}
static void drawAlert(M5Canvas& g) {
  int w = 580, h = 420, x = (SW - w) / 2, y = (SH - h) / 2;
  g.fillRoundRect(x + 6, y + 10, w, h, 36, rgb(30, 30, 40)); g.fillRoundRect(x, y, w, h, 36, rgb(236, 236, 242));
  g.setTextDatum(top_center); g.setTextColor(TFT_BLACK);
  uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1.2f); g.drawString(kernel::errName(alertF.code), SW / 2, y + 28);
  uiFont(&g, &fonts::FreeSans9pt7b); uiSize(&g, 1.15f); g.setTextDatum(top_left);
  wrapText(g, alertF.msg, x + 34, y + 90, w - 68, 34);
  g.drawFastHLine(x, y + h - 96, w, rgb(190, 190, 200));
  g.setTextDatum(middle_center); g.setTextColor(rgb(0, 122, 255)); uiFont(&g, &fonts::FreeSansBold12pt7b); uiSize(&g, 1.2f);
  g.drawString("OK", SW / 2, y + h - 48);
}
static void present(M5Canvas& src) {
  M5.Display.startWrite(); src.pushSprite(&M5.Display, 0, 0); M5.Display.endWrite();
  M5.Display.waitDisplay();
}
static void blit(Process* p) {
  if (p && p->hasFrame) p->surf[p->frontIdx]->pushSprite(&fb, 0, 0); else fb.fillScreen(TFT_BLACK);
}

static void updateSnapshots() {
  std::vector<sys::ProcInfo> v;
  v.push_back({0, "root", "kernel", "running", 0, 0}); v.push_back({1, "root", "compositor", "running", 0, 0});
  auto add = [&](Process* p) {
    if (!p) return;
    v.push_back({p->pid, "root", p->name, p->exited ? "exited" : (p == fg && st == APP) ? "fg" : "bg",
                 p->task ? (uint32_t)uxTaskGetStackHighWaterMark(p->task) : 0u, (uint32_t)(p->surfBytes >> 10)});
  };
  add(sb); add(fg);
  sys::RuntimeInfo r; r.uptimeMs = millis(); r.fps = rtFps; r.frameMs = (uint32_t)rtWork; r.heapFree = ESP.getFreeHeap(); r.heapMin = ESP.getMinFreeHeap();
  r.psramFree = ESP.getFreePsram(); r.fgBeatAgeMs = fg ? millis() - fg->beat : 0; r.kills = killCount; r.cpuMHz = ESP.getCpuFreqMHz();
  r.fgName = fg ? fg->name : "-"; r.resetReason = kernel::lastResetName();
  std::lock_guard<std::mutex> lk(snapMx); procSnap = v; rtSnap = r;
}
std::vector<sys::ProcInfo> sys::procList() { std::lock_guard<std::mutex> lk(snapMx); return procSnap; }
sys::RuntimeInfo sys::runtime() { std::lock_guard<std::mutex> lk(snapMx); return rtSnap; }
std::vector<std::string> sys::logTail(int n) { auto v = kernel::logCopy(); if ((int)v.size() > n) v.erase(v.begin(), v.end() - n); return v; }

void shellSetup() {
  makeSprite(fb, SW, SH, 16, "compositor framebuffer");
  fontInit();
  sb = spawnApp(makeSpringboard(), "Springboard", -1, "", true);
  if (!sb) kernel::panic(kernel::E_OOM, "Could not start Springboard (the home screen process).");
  kernel::ext = [](const std::vector<std::string>& a, std::string& out) -> bool {
    if (a[0] == "apps") { for (int i = 0; i < kAppCount; i++) out += std::string(kApps[i].name) + (kApps[i].make ? "" : " (not implemented)") + "\n"; return true; }
    if (a[0] == "ps") { out = shell::psText(); return true; }
    if (a[0] == "launch" && a.size() > 1) {
      int i = findApp(a[1]);
      if (i < 0) out = "no such app";
      else if (i >= kVisibleCount && !kernel::devMode()) out = "permission denied: enable Developer Mode";
      else { shell::requestLaunch(kApps[i].name, a.size() > 2 ? a[2].c_str() : ""); out = "launching"; }
      return true;
    }
    if (a[0] == "kill") { pendClose = true; out = "closing foreground app"; return true; }
    if (a[0] == "lock") { if (sb) { Event e{}; e.type = 3; strcpy(e.cmd, "lock"); xQueueSend(sb->q, &e, 0); } out = "locked"; return true; }
    if (a[0] == "run") { std::string src; for (size_t i = 1; i < a.size(); i++) src += (i > 1 ? " " : "") + a[i]; out = scriptRun(src); return true; }
    return false;
  };
  lastUs = micros();
}

static void readTouch(float dt) {
  auto d = M5.Touch.getDetail(0);
  tc.pressed = d.wasPressed(); tc.released = d.wasReleased(); tc.down = d.isPressed();
  if (d.isPressed() || d.wasPressed() || d.wasReleased()) {
    if (d.x < 0 || d.y < 0 || d.x >= SW || d.y >= SH) {
      if (!touchWarned) { touchWarned = true; kernel::fault(kernel::E_TOUCH_RANGE, "Touch at (%d,%d) is outside the %dx%d panel - touch and display geometry disagree.", d.x, d.y, SW, SH); }
    } else { tc.x = d.x; tc.y = d.y; }
  }
  if (tc.pressed) { tc.sx = tc.x; tc.sy = tc.y; tc.t0 = millis(); prevY = tc.y; tc.vy = 0; }
  if (tc.down) { tc.vy = tc.vy * 0.7f + ((tc.y - prevY) / dt) * 0.3f; prevY = tc.y; }
}

void shellLoop() {
  if (kernel::isPanicking()) return;
  uint32_t now = micros(); float dt = (now - lastUs) / 1e6f; lastUs = now;
  if (dt < 0.001f) dt = 0.001f; if (dt > 0.05f) dt = 0.05f;
  readTouch(dt);
  if (pendPanic[0]) { char m[160]; strcpy(m, pendPanic); pendPanic[0] = 0; kernel::panic(kernel::E_APP_FAULT, "%s", m); }
  if (!alertOn && kernel::pollFault(alertF)) alertOn = true;

  // process supervision: crashed or hung apps drop straight to the home screen, no animation
  if (sb && sb->exited) { kernel::log("Springboard exited - restarting"); reap(sb); sb = spawnApp(makeSpringboard(), "Springboard", -1, "", true); }
  if (fg) {
    bool dead = fg->exited;
    if (!dead && millis() - fg->beat > 5000) { kernel::log("%s is not responding", fg->name.c_str()); killCount++; dead = true; }
    if (dead) { reap(fg); fg = nullptr; st = HOME; openS.snap(0); }
  }
  if (pendClose) { pendClose = false; if (fg && st == APP) { openS.target = 0; st = CLOSING; } }
  if (pend.valid) {
    pend.valid = false;
    int i = pend.idx >= 0 ? pend.idx : findApp(pend.name);
    launchApp(i, pend.arg, pend.zoom, pend.ix, pend.iy, pend.sz);
  }

  // input
  if (alertOn) {
    if (tc.released && tc.x > SW / 2 - 290 && tc.x < SW / 2 + 290 && tc.y > SH / 2 + 114 && tc.y < SH / 2 + 210) alertOn = false;
  } else if (st == HOME) postTouch(sb);
  else if (st == OPENING || st == CLOSING) {}
  else if (st == APP && fg) {
    if (tc.released && tc.sy > SH - 70 && tc.sy - tc.y > 120) { openS.target = 0; st = CLOSING; }
    else if (!(tc.sy > SH - 70 && tc.down)) postTouch(fg);
  }
  if (st == OPENING) { openS.step(dt); if (openS.settled()) { openS.snap(1); st = APP; } }
  if (st == CLOSING) { openS.step(dt); if (openS.settled()) { openS.snap(0); reap(fg); fg = nullptr; st = HOME; } }

  // ask visible processes for a new frame (only when dirty / animating / idle timer)
  if (st != APP) requestDraw(sb);
  if (st != HOME) requestDraw(fg);

  // composite + present only when something changed: an idle OS does no work and cannot tear
  static uint32_t seqSb = 0, seqFg = 0, lastPresent = 0; static St lastSt = OPENING; static bool lastAlert = false;
  uint32_t nowMs = millis();
  bool need = st != lastSt || alertOn != lastAlert || st == OPENING || st == CLOSING || nowMs - lastPresent > 1000 || tc.down || tc.released || (g_showFps && kernel::devMode());
  if (sb && st != APP && sb->frameSeq != seqSb) need = true;
  if (fg && st != HOME && fg->frameSeq != seqFg) need = true;
  if (need) {
    if (st == APP) blit(fg); else blit(sb);
    if (fg && (st == OPENING || st == CLOSING) && fg->hasFrame) {
      float t = fminf(fmaxf(openS.x, 0.f), 1.04f);
      float cx0 = zIx + zSz / 2.0f, cy0 = zIy + zSz / 2.0f;
      float cx = cx0 + (SW / 2.0f - cx0) * t, cy = cy0 + (SH / 2.0f - cy0) * t;
      fg->surf[fg->frontIdx]->pushRotateZoom(&fb, cx, cy, 0, (zSz + (SW - zSz) * t) / SW, (zSz + (SH - zSz) * t) / SH);
    }
    bool dark = fg && st == APP && fg->app->darkStatusBar();
    statusBar(fb, dark); homeIndicator(fb, dark);
    if (alertOn) drawAlert(fb);
    if (g_showFps && kernel::devMode()) {
      uiFont(&fb, &fonts::FreeSansBold9pt7b); uiSize(&fb, 1); fb.setTextColor(TFT_GREEN, TFT_BLACK); fb.setTextDatum(top_left);
      char b[16]; snprintf(b, sizeof b, " %u fps ", (unsigned)rtFps); fb.drawString(b, 200, 8);
    }
    present(fb);
    presentCount++; seqSb = sb ? sb->frameSeq : 0; seqFg = fg ? fg->frameSeq : 0; lastSt = st; lastAlert = alertOn; lastPresent = nowMs;
  }
  uint32_t spent = micros() - now;
  rtWork = rtWork * 0.9f + (spent / 1000.0f) * 0.1f;
  if (nowMs - fpsT >= 1000) { rtFps = presentCount; presentCount = 0; fpsT = nowMs; }
  if (nowMs - snapT >= 250) { snapT = nowMs; updateSnapshots(); }
  if (spent < 16000) delay((16000 - spent) / 1000);   // ~60 Hz loop
}
