// USERSPACE API. Apps include only this header. It deliberately does NOT include M5Unified,
// so apps cannot reach the hardware (M5.Display / M5.Power / M5.Touch ...) - they draw into the
// surface the kernel gives them and talk to the kernel through the sys:: calls below.
#pragma once
#include <M5GFX.h>
#include <Arduino.h>
#include <cmath>
#include <string>
#include <vector>

#ifndef HRS_EXPECT_W
#define HRS_EXPECT_W 720
#endif
#ifndef HRS_EXPECT_H
#define HRS_EXPECT_H 1280
#endif
constexpr int SW = HRS_EXPECT_W, SH = HRS_EXPECT_H;

constexpr inline uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

struct Touch {
  bool down = false, pressed = false, released = false;
  int x = 0, y = 0, sx = 0, sy = 0;   // current, start
  float vy = 0;                       // smoothed vertical velocity px/s
  uint32_t t0 = 0;
};

struct Spring {
  float x = 0, v = 0, target = 0, k = 240, c = 26;
  void step(float dt) {
    for (int i = 0; i < 4; i++) { float h = dt / 4, a = -k * (x - target) - c * v; v += a * h; x += v * h; }
  }
  bool settled() const { return fabsf(x - target) < 0.002f * (1 + fabsf(target)) && fabsf(v) < 0.05f; }
  void snap(float t) { x = target = t; v = 0; }
};

// Every app runs as its own process (FreeRTOS task) and only ever sees these callbacks.
class App {
 public:
  virtual ~App() {}
  virtual const char* name() const = 0;
  virtual void onLaunch() {}                    // runs in the app's own task
  virtual void onDraw(M5Canvas& g) = 0;         // paint the full SWxSH surface; keep y<56 free for the status bar
  virtual void onTouch(const Touch&) {}
  virtual void onCommand(const char*) {}        // kernel-to-app messages (e.g. "lock")
  virtual void onClose() {}
  virtual bool darkStatusBar() const { return false; }
  virtual uint16_t bg() const { return 0; }
  virtual int stackBytes() const { return 24576; }
  virtual int idleMs() const { return 1000; }        // redraw at least this often (kernel skips draws when nothing changed)
  virtual bool animating() const { return false; }    // true while an animation needs every frame
  virtual int tickMs() const { return 0; }           // >0: onTick() runs about every tickMs (background work such as audio)
  virtual void onTick() {}
};

struct AppInfo {
  const char* name;
  uint16_t c1, c2;
  void (*glyph)(M5Canvas&, int cx, int cy, int s);
  App* (*make)();                               // nullptr = not implemented
};
extern const AppInfo kApps[];
extern const int kAppCount;
extern const int kVisibleCount;                 // apps on the home screen (Console is hidden)
App* makeSpringboard();
App* makeMusic();                         // the home/lock screen app
std::string scriptRun(const std::string& src);
std::string matrixCommand(const std::vector<std::string>& args);   // 'matrix [seconds]' (kernel shell + serial)  // HRScript service, used by the kernel 'run' command

// ---- system calls (implemented by the kernel in sys.cpp) ----
namespace sys {
int displayW(); int displayH();
void setBrightness(int v);
int battery();
std::string timeString(bool seconds = false);
std::string dateString();
uint32_t freeHeap(); uint32_t freePsram();
void log(const char* fmt, ...);
void launch(const char* appName, const char* arg = nullptr);     // start an app (no zoom)
void launchZoom(int appIndex, int iconX, int iconY, int iconSize);  // start an app with the zoom animation
const char* arg();                                                // argument this process was started with
bool devMode(); void setDevMode(bool on);
bool showFps(); void setShowFps(bool on);
void raiseFault(uint32_t code, const char* msg);                  // show a kernel alert
void requestPanic(const char* msg);                               // dev mode only; kernel decides
std::string kshell(const std::string& line);                      // kernel shell (permission-checked by the kernel)
M5Canvas* newCanvas(int w, int h);                                // quota-checked, freed automatically at exit
void freeCanvas(M5Canvas* c);
bool canvasValid(M5Canvas* c);
bool readFile(const char* path, std::vector<uint8_t>& out);       // only /icons/* and /fonts/*
void pet();
// SD card (read-only, .mp3 only) and audio output - the kernel owns the hardware
bool sdMount(bool force = false);
std::vector<std::string> listMp3();
int fileOpen(const char* path); size_t fileRead(int h, uint8_t* buf, size_t n); bool fileSeek(int h, size_t pos);
size_t fileSize(int h); size_t filePos(int h); void fileClose(int h);
bool audioAvailable(); bool audioReady(); bool audioBusy();
bool audioPlay(const int16_t* pcm, size_t samples, uint32_t rate, bool stereo); void audioStop(); void setVolume(int v);
struct ProcInfo { int pid; std::string user, name, state; uint32_t stackFree, surfKB; };
struct RuntimeInfo { uint32_t uptimeMs = 0, fps = 0, frameMs = 0, heapFree = 0, heapMin = 0, psramFree = 0, watchdogLimitMs = 5000, fgBeatAgeMs = 0, kills = 0, cpuMHz = 0; std::string fgName, resetReason; };
std::vector<ProcInfo> procList();                 // snapshot of the process table (all owned by root)
RuntimeInfo runtime();                            // uptime, fps, memory, watchdog
std::vector<std::string> logTail(int n);          // last n kernel log lines                                                       // heartbeat during long work (watchdog kills silent apps after 5 s)
}

// ---- text: uses your VLW font from LittleFS (/fonts/regular.vlw, /fonts/bold.vlw) if present ----
void uiFont(lgfx::LGFXBase* g, const lgfx::IFont* builtin);
void uiSize(lgfx::LGFXBase* g, float s);
