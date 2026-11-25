#include "os.h"

void shellSetup();
void shellLoop();

void setup() {
  kernel::begin();
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);                // Tab5 native portrait 720x1280
  kernel::checkDisplay();                   // real display-size validation (may panic)
  M5.Display.setBrightness(180);
  shellSetup();
  kernel::log("boot complete, free heap %u psram %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  Serial.print("hrs> ");
}

void loop() {
  M5.update();
  kernel::serialPoll();
  shellLoop();
}
