#pragma once
#include <Arduino.h>
#include <functional>
#include <string>
#include <vector>

#ifndef HRS_EXPECT_W
#define HRS_EXPECT_W 720
#endif
#ifndef HRS_EXPECT_H
#define HRS_EXPECT_H 1280
#endif

namespace kernel {

enum Err : uint32_t {
  E_OK = 0,
  E_DISPLAY_INIT = 0x1001,    // display did not initialise
  E_DISPLAY_SIZE = 0x1002,    // panel size differs from what the OS was built for
  E_TOUCH_RANGE = 0x1003,     // touch coordinates outside the panel
  E_NO_PSRAM = 0x2001,        // PSRAM missing / not enabled
  E_OOM = 0x2002,             // allocation failed
  E_SURFACE_TOO_LARGE = 0x2003,  // surface larger than the display
  E_APP_FAULT = 0x3001,       // app threw an exception
  E_APP_NOT_INSTALLED = 0x3004,
  E_SCRIPT = 0x4001,          // HRScript error
  E_UNEXPECTED_RESET = 0x5001 // previous boot ended in panic/watchdog/brownout
};

struct Fault { Err code; std::string msg; };

const char* errName(Err e);
void begin();                              // serial + log + reset reason (call first)
void checkDisplay();                       // validates panel size; panics on mismatch
[[noreturn]] void panic(Err code, const char* fmt, ...);
void fault(Err code, const char* fmt, ...);  // non-fatal: shown as an alert by the shell
bool pollFault(Fault& f);
void log(const char* fmt, ...);
const std::vector<std::string>& logLines();
bool isPanicking();
std::vector<std::string> logCopy();
const char* lastResetName();   // true once a kernel panic screen is up (compositor stops)

bool devMode();
void setDevMode(bool on);

// Kernel shell. Extra commands (launch/kill/ps/run) are provided by hooks.
std::string exec(const std::string& line);
extern std::function<bool(const std::vector<std::string>&, std::string&)> ext;
void serialPoll();   // call every loop; executes lines typed on the USB serial console

}  // namespace kernel
