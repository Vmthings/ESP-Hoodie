// KERNEL-side header (hardware access). Apps never include this.
#pragma once
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "app_api.h"
#include "kernel.h"

struct Event { uint8_t type; Touch t; char cmd[12]; };   // 0 touch, 1 draw, 2 close, 3 command

struct Process {
  App* app = nullptr; std::string name, arg; int idx = -1, pid = 0; bool system = false;
  volatile bool dirty = true; volatile uint32_t lastDraw = 0, frameSeq = 0;
  TaskHandle_t task = nullptr; QueueHandle_t q = nullptr;
  M5Canvas* surf[2] = {nullptr, nullptr}; volatile int frontIdx = 0;   // app draws the back buffer, then flips
  volatile bool drawPending = false, hasFrame = false, crashed = false, exited = false;
  volatile uint32_t beat = 0;
  size_t surfBytes = 0, quota = 0; std::vector<M5Canvas*> owned;
};

Process* currentProcess();            // thread-local: the process whose task is running
void setCurrentProcess(Process* p);
void kernelFreeCanvas(M5Canvas* c);
void kernelCloseFiles(Process* p);
void fontInit();
bool fsMounted();
void makeSprite(M5Canvas& c, int w, int h, int depth, const char* what);
extern bool g_showFps;

namespace shell {
void requestLaunch(const char* appName, const char* arg);
void requestLaunchZoom(int idx, int x, int y, int size);
void requestPanic(const char* msg);
std::string psText();
}
