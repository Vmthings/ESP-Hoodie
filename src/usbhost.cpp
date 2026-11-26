// KERNEL: USB-A host serial service (CDC-ACM), used by the Raspberry Pi control app through sys:: calls.
// Works with a Pi in USB-gadget serial mode (g_serial / ttyGS0) or any CDC-ACM serial device.
#include <deque>
#include <mutex>
#include "os.h"

#if __has_include("usb/usb_host.h")
#include "usb/usb_host.h"

static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t s_dev = nullptr;
static uint8_t s_inEp = 0, s_outEp = 0, s_commIf = 0xFF, s_dataIf = 0xFF; static uint16_t s_inMps = 64, s_outMps = 64;
static usb_transfer_t* s_in = nullptr;
static std::mutex s_mx; static std::deque<uint8_t> s_rx;
static volatile bool s_started = false, s_open = false, s_newDev = false, s_gone = false; static volatile uint8_t s_newAddr = 0;
static volatile int s_state = 0; static std::string s_info = "No USB device on USB-A";
static SemaphoreHandle_t s_sem = nullptr;

static void xferDone(usb_transfer_t* t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }
static void inDone(usb_transfer_t* t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
    std::lock_guard<std::mutex> lk(s_mx);
    for (int i = 0; i < t->actual_num_bytes && s_rx.size() < 8192; i++) s_rx.push_back(t->data_buffer[i]);
  }
  if (s_open && t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) usb_host_transfer_submit(t);
}
static bool ctrl(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, const uint8_t* data, uint16_t len) {
  usb_transfer_t* t = nullptr;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &t) != ESP_OK) return false;
  usb_setup_packet_t* s = (usb_setup_packet_t*)t->data_buffer;
  s->bmRequestType = bm; s->bRequest = req; s->wValue = val; s->wIndex = idx; s->wLength = len;
  if (len && data) memcpy(t->data_buffer + sizeof(usb_setup_packet_t), data, len);
  t->num_bytes = sizeof(usb_setup_packet_t) + len; t->device_handle = s_dev; t->bEndpointAddress = 0;
  t->callback = xferDone; t->context = s_sem; t->timeout_ms = 1000;
  bool ok = false;
  if (usb_host_transfer_submit_control(s_client, t) == ESP_OK) { xSemaphoreTake(s_sem, pdMS_TO_TICKS(1500)); ok = t->status == USB_TRANSFER_STATUS_COMPLETED; }
  usb_host_transfer_free(t); return ok;
}
static void closeDevice() {
  s_open = false;
  if (s_dev) {
    if (s_in) { usb_host_endpoint_halt(s_dev, s_inEp); usb_host_endpoint_flush(s_dev, s_inEp); vTaskDelay(pdMS_TO_TICKS(20)); usb_host_transfer_free(s_in); s_in = nullptr; }
    if (s_dataIf != 0xFF) usb_host_interface_release(s_client, s_dev, s_dataIf);
    if (s_commIf != 0xFF) usb_host_interface_release(s_client, s_dev, s_commIf);
    usb_host_device_close(s_client, s_dev); s_dev = nullptr;
  }
  s_dataIf = s_commIf = 0xFF; s_state = 0; s_info = "No USB device on USB-A";
}
static void tryOpen(uint8_t addr) {
  usb_device_handle_t dev;
  if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
  const usb_device_desc_t* dd = nullptr; const usb_config_desc_t* cfg = nullptr;
  usb_host_get_device_descriptor(dev, &dd); usb_host_get_active_config_descriptor(dev, &cfg);
  char b[96]; snprintf(b, sizeof b, "USB device %04x:%04x", dd ? dd->idVendor : 0, dd ? dd->idProduct : 0);
  uint8_t cls = 0, sub = 0, curIf = 0, comm = 0xFF, data = 0xFF, inEp = 0, outEp = 0; uint16_t inM = 64, outM = 64;
  const uint8_t* p = (const uint8_t*)cfg; int pos = 0, total = cfg ? cfg->wTotalLength : 0;
  while (p && pos + 2 <= total && p[pos] > 0) {
    uint8_t len = p[pos], type = p[pos + 1];
    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) { curIf = p[pos + 2]; cls = p[pos + 5]; sub = p[pos + 6]; if (cls == 0x02 && sub == 0x02) comm = curIf; }
    else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cls == 0x0A && (p[pos + 3] & 3) == 2) {
      uint8_t ep = p[pos + 2]; uint16_t m = p[pos + 4] | (p[pos + 5] << 8); data = curIf;
      if (ep & 0x80) { inEp = ep; inM = m; } else { outEp = ep; outM = m; }
    }
    pos += len;
  }
  if (data == 0xFF || !inEp || !outEp) {
    s_info = std::string(b) + " - not a CDC-ACM serial device (Pi needs USB gadget serial mode)"; s_state = 1;
    usb_host_device_close(s_client, dev); return;
  }
  s_dev = dev; s_dataIf = data; s_commIf = comm; s_inEp = inEp; s_outEp = outEp; s_inMps = inM; s_outMps = outM;
  if (comm != 0xFF) usb_host_interface_claim(s_client, dev, comm, 0);
  usb_host_interface_claim(s_client, dev, data, 0);
  s_info = std::string(b) + " - serial ready"; s_state = 2;
  kernel::log("%s", s_info.c_str());
}
static void libTask(void*) { for (;;) { uint32_t f; usb_host_lib_handle_events(portMAX_DELAY, &f); if (f & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all(); } }
static void clientCb(const usb_host_client_event_msg_t* m, void*) {
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { s_newAddr = m->new_dev.address; s_newDev = true; }
  else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) s_gone = true;
}
static void clientTask(void*) {
  for (;;) {
    usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
    if (s_gone) { s_gone = false; closeDevice(); }
    if (s_newDev) { s_newDev = false; if (!s_dev) tryOpen(s_newAddr); }
  }
}
static void ensure() {
  if (s_started) return;
  s_started = true; s_sem = xSemaphoreCreateBinary();
  usb_host_config_t hc = {}; hc.skip_phy_setup = false; hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hc) != ESP_OK) { s_info = "USB host failed to start"; kernel::log("%s", s_info.c_str()); return; }
  usb_host_client_config_t cc = {}; cc.is_synchronous = false; cc.max_num_event_msg = 5; cc.async.client_event_callback = clientCb; cc.async.callback_arg = nullptr;
  if (usb_host_client_register(&cc, &s_client) != ESP_OK) { s_info = "USB host client failed"; return; }
  xTaskCreatePinnedToCore(libTask, "usb_lib", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(clientTask, "usb_client", 6144, nullptr, 5, nullptr, 0);
  kernel::log("USB host started");
}

namespace sys {
int usbSerialState() { if (!kernel::devMode()) return 0; ensure(); return s_state; }
std::string usbSerialInfo() { if (!kernel::devMode()) return "Developer Mode required"; ensure(); return s_info; }
bool usbSerialOpen(uint32_t baud) {
  if (!kernel::devMode()) return false;
  ensure(); if (s_state != 2 || !s_dev) return false;
  if (s_open) return true;
  if (s_commIf != 0xFF) {
    uint8_t lc[7] = {(uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24), 0, 0, 8};
    ctrl(0x21, 0x20, 0, s_commIf, lc, 7);       // SET_LINE_CODING
    ctrl(0x21, 0x22, 0x0003, s_commIf, nullptr, 0);   // SET_CONTROL_LINE_STATE: DTR + RTS
  }
  { std::lock_guard<std::mutex> lk(s_mx); s_rx.clear(); }
  if (usb_host_transfer_alloc(s_inMps, 0, &s_in) != ESP_OK) return false;
  s_in->device_handle = s_dev; s_in->bEndpointAddress = s_inEp; s_in->num_bytes = s_inMps; s_in->callback = inDone; s_in->context = nullptr;
  s_open = true;
  if (usb_host_transfer_submit(s_in) != ESP_OK) { s_open = false; usb_host_transfer_free(s_in); s_in = nullptr; return false; }
  return true;
}
size_t usbSerialRead(uint8_t* b, size_t n) { std::lock_guard<std::mutex> lk(s_mx); size_t k = 0; while (k < n && !s_rx.empty()) { b[k++] = s_rx.front(); s_rx.pop_front(); } return k; }
bool usbSerialWrite(const uint8_t* d, size_t n) {
  if (!kernel::devMode() || !s_open || !s_dev) return false;
  while (n > 0) {
    size_t c = n > s_outMps ? s_outMps : n; usb_transfer_t* t = nullptr;
    if (usb_host_transfer_alloc(c, 0, &t) != ESP_OK) return false;
    memcpy(t->data_buffer, d, c); t->num_bytes = c; t->device_handle = s_dev; t->bEndpointAddress = s_outEp; t->callback = xferDone; t->context = s_sem; t->timeout_ms = 1000;
    bool ok = false;
    if (usb_host_transfer_submit(t) == ESP_OK) { xSemaphoreTake(s_sem, pdMS_TO_TICKS(1500)); ok = t->status == USB_TRANSFER_STATUS_COMPLETED; }
    usb_host_transfer_free(t); if (!ok) return false;
    d += c; n -= c;
  }
  return true;
}
void usbSerialClose() {
  if (!s_open) return;
  s_open = false;
  if (s_in && s_dev) { usb_host_endpoint_halt(s_dev, s_inEp); usb_host_endpoint_flush(s_dev, s_inEp); vTaskDelay(pdMS_TO_TICKS(20)); usb_host_transfer_free(s_in); s_in = nullptr; }
}
void usbPower(bool on) { if (kernel::devMode()) M5.Power.setUsbOutput(on); }
}  // namespace sys

#else   // USB host component not present in this Arduino core build
namespace sys {
int usbSerialState() { return 0; }
std::string usbSerialInfo() { return "USB host support is not compiled into this build (the Arduino core has no usb_host component)"; }
bool usbSerialOpen(uint32_t) { return false; }
size_t usbSerialRead(uint8_t*, size_t) { return 0; }
bool usbSerialWrite(const uint8_t*, size_t) { return false; }
void usbSerialClose() {}
void usbPower(bool) {}
}
#endif
