# ESP-Hoodie

iOS-16-style OS for the M5Stack Tab5 (PlatformIO).

iOS-16-style shell for the Tab5 (ST7123, 720x1280): lock screen, home screen, dock, spring zoom open/close,
Settings, Calculator, Clock, plus a kernel shell and Developer Mode with a real interpreter (HRScript).

Build: `pio run -t upload` then `pio device monitor` (115200). Type `help` on the serial console.

## Kernel / errors
- Boot checks the panel size against HRS_EXPECT_W/H (platformio.ini). Mismatch -> full-screen kernel panic `E_DISPLAY_SIZE (0x1002)`.
- Surfaces larger than the display -> `E_SURFACE_TOO_LARGE`; failed PSRAM allocs -> `E_OOM`; touch outside the panel -> `E_TOUCH_RANGE`.
- App exceptions are caught: the app is closed and an alert shows the error code. Unfinished apps raise `E_APP_NOT_INSTALLED`.
- After a CPU panic / watchdog / brownout reset, the next boot shows `E_UNEXPECTED_RESET` with the reset reason.
- Kernel shell commands: help info mem display log errors apps ps launch kill lock reboot dev; dev-only: run panic fault stackoverflow.

## Developer Mode
Settings > Developer > Developer Mode. Unlocks: HRScript Console (on-screen keyboard, script canvas), Kernel Shell, FPS counter, test fault/panic.

## HRScript
```
let x = 5;  fn sq(n) { return n*n; }
for i in 0..10 { rect(i*30, 20, 24, sq(i), rgb(255,120,0)); }
if x > 3 { print("big", x); } else { print("small"); }
```
Types: number, string, nil. Builtins: print rgb clear rect circle line text width height millis delay rand str num len sqrt sin cos abs floor heap psram brightness.
Errors carry line:col and a kind (SyntaxError, TypeError, RuntimeError, LimitError, DisplayError).
`src/interp.*` has no Arduino dependency and is host-testable with g++.

## Notes
- Console is hidden from the home screen; open it from Settings > Developer (Developer Mode must be on).
- Custom icons: see data/icons/README.txt (`pio run -t uploadfs`).
- Push to GitHub: `git init && git add . && git commit -m "ESP-Hoodie" && git branch -M main && git remote add origin <your-repo-url> && git push -u origin main`

## Architecture (v0.2)
- **Kernel** (kernel.*, shell.cpp, sys.cpp): boots the panel, validates size, supervises processes, composites frames, owns the status bar / alerts / home gesture.
- **Userspace** (apps.cpp, springboard.cpp): every app, including **Springboard** (lock + home screen), runs as its own FreeRTOS task with its own
  double-buffered surface. Apps include only `app_api.h` (no M5Unified, so no direct hardware access) and use `sys::` system calls
  (brightness, battery, time, launch, quota-checked canvases, file read limited to /icons and /fonts).
- Per-process memory quota for surfaces, 5 s heartbeat watchdog (hung apps are force-killed), crashes drop straight to the home screen.
- Limit: the ESP32-P4 has no per-process MMU/privilege split in this setup, so isolation is by API boundary, tasks and quotas, not hardware memory protection.
  Truly sandboxed code is HRScript (the interpreter cannot touch memory outside its builtins).
- `ps` in the kernel shell lists processes. Fonts: see data/fonts/README.txt.

## v0.3
- Settings is now categorised (Display, Battery, About, Developer): tap a category, then its options appear; "< Settings" goes back.
- HRScript console is a 4-pane dashboard: SCRIPTS (top, theme colour changeable with COLOR), ERRORS (middle: display + runtime errors),
  USERS (bottom left: process table, everything owned by root, entries load one per millisecond), RUNTIME/WATCHDOG (bottom right).
  GFX toggles the script canvas, CLR clears history, SHELL switches to the kernel shell.
- Performance: apps redraw only when dirty/animating/idle-timed, the compositor presents only changed frames, Springboard uses plain blits.

## HRScript reference
Statements: `let x = 1;`  `x = x + 1;`  `fn name(a, b) { return a + b; }`  `if cond { } else { }`  `while cond { }`  `for i in 0..10 { }`  `return v;`  `// comment`
Types: number, string ("..." with \n \t \\ \"), nil. Operators: + - * / % == != < > <= >= && || ! (strings: + joins, == compares, < > order).
Builtins: print(...) rgb(r,g,b) clear(c) rect(x,y,w,h,c) circle(x,y,r,c) line(x1,y1,x2,y2,c) text(x,y,s,size,c) width() height() millis() delay(ms 0-2000)
rand(n) str(x) num(s) len(s) sqrt(x) sin(x) cos(x) abs(x) floor(x) heap() psram() brightness(0-255) matrix([seconds 0-60]).
Shell: `matrix [seconds]` too (0 stops). Limits: 400k steps per run, call depth 40, canvas 720x420.

## Music app (SD .mp3)
Music scans the microSD card (up to 3 folders deep) for .mp3 files, lists them, and plays with prev/play/next, a progress bar and volume.
Tab5 microSD is read on SPI pins SCK 43, MISO 39, MOSI 44, CS 42. Decoding runs in the app (arduino-libhelix); the kernel provides SD reads and the speaker.
Audio stops when you leave the app (no background playback yet). Format: FAT32 card, MPEG-1/2 Layer III files.

## Lag fixes (v0.4)
Compositor on its own core-0 task above app priority; apps pinned to core 1; battery/RTC I2C reads cached (were per frame);
a held finger that has not moved no longer forces redraws.

## Raspberry Pi control (Developer Mode)
Settings > Developer > Raspberry Pi (USB-A) opens a login + terminal for a Pi on the USB-A port (CDC-ACM serial; Developer Mode required).
Pi setup (gadget serial): add `dtoverlay=dwc2` to config.txt, `modules-load=dwc2,g_serial` to cmdline.txt, run `sudo systemctl enable serial-getty@ttyGS0`.
Use a data cable from the Tab5 USB-A port to the Pi's USB data port; power the Pi separately. The Tab5 5V output is OFF unless you switch it on in the app.
The password is only held in RAM until it is sent. A Pi acting as a plain USB host (A-to-A cable) or a USB-UART chip that is not CDC-ACM (FTDI/CP210x) is not supported yet.
Kernel shell: `usb` shows the USB-A device status.
