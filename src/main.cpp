#include "os.h"

void shellSetup();
void shellLoop();

// The compositor (touch, frame composition, panel transfer) runs on core 0 at a higher priority than apps,
// so a busy app can never starve input or presentation. Apps run on core 1.
static void compositorTask(void*) {
  for (;;) {
    if (kernel::isPanicking()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    M5.update();
    kernel::serialPoll();
    shellLoop();
  }
}

void setup() {
  kernel::begin();
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);                // Tab5 native portrait 720x1280
  kernel::checkDisplay();                   // real display-size validation (may panic)
  M5.Display.setBrightness(180);
  M5.Speaker.setVolume(160);
  shellSetup();
  kernel::log("boot complete, free heap %u psram %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  Serial.print("hrs> ");
  xTaskCreatePinnedToCore(compositorTask, "compositor", 12288, nullptr, 4, nullptr, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
