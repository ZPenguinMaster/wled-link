#include "link.h"

#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_bt.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../../wled/usermod/link_proto.h"

namespace {

struct Frame { uint8_t mac[6]; uint8_t len; uint8_t data[wll::FRAME_MAX]; };

wll::Link blink;
QueueHandle_t rxq = nullptr;
bool running = false;
wifi_interface_t radioIf = WIFI_IF_STA;
portMUX_TYPE txMux = portMUX_INITIALIZER_UNLOCKED;
int txInFlight = 0;
uint32_t txWaitSince = 0;  // since when frames have been waiting without one being confirmed
uint32_t framesSent = 0, framesLost = 0, stalls = 0;

uint8_t mode = 0xFF;  // 0xFF: never chosen
uint8_t key[16];
bool haveKey = false;
uint8_t savedChannel = 0;

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const int TX_IN_FLIGHT_MAX = 4;  // ESP-NOW's own queue is small; more would just be refused
const uint32_t STALL_MS = 1500;  // no send confirmed for this long, with some waiting: the radio is stuck

void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  uint32_t now = millis();
  portENTER_CRITICAL(&txMux);
  if (txInFlight > 0) txInFlight--;
  txWaitSince = txInFlight ? now : 0;
  if (status != ESP_NOW_SEND_SUCCESS) framesLost++;
  portEXIT_CRITICAL(&txMux);
}

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!rxq || len < 2 || len > (int)wll::FRAME_MAX || data[0] != wll::MAGIC) return;
  Frame f;
  memcpy(f.mac, mac, 6);
  f.len = (uint8_t)len;
  memcpy(f.data, data, len);
  xQueueSend(rxq, &f, 0);
}

bool radioSend(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (!running) return false;
  uint32_t now = millis();
  portENTER_CRITICAL(&txMux);
  bool full = txInFlight >= TX_IN_FLIGHT_MAX;
  if (!full && txInFlight++ == 0) txWaitSince = now;
  portEXIT_CRITICAL(&txMux);
  if (full) return false;
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.ifidx = radioIf;
    p.channel = 0;  // whatever the radio is on
    p.encrypt = false;  // link_proto encrypts
    esp_now_add_peer(&p);
  }
  if (esp_now_send(mac, data, len) != ESP_OK) {
    portENTER_CRITICAL(&txMux);
    txInFlight--;
    portEXIT_CRITICAL(&txMux);
    return false;
  }
  framesSent++;
  return true;
}

bool deliver(const uint8_t* msg, size_t len, bool reliable) { return onLinkMessage(msg, len); }
void up() { onLinkChange(true); }
void down() { onLinkChange(false); }

}  // namespace

// The hardware RNG is only truly random while the radio runs; before that, borrow the bootloader's
// entropy source (which must not run alongside Wi-Fi or Bluetooth).
void randomBytes(void* out, size_t n) {
  wifi_mode_t m = WIFI_MODE_NULL;
  bool radio = (esp_wifi_get_mode(&m) == ESP_OK && m != WIFI_MODE_NULL) ||
               esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
  if (!radio) bootloader_random_enable();
  esp_fill_random(out, n);
  if (!radio) bootloader_random_disable();
}

void linkLoad() {
  Preferences p;
  if (p.begin("wllink", true)) {
    mode = p.getUChar("mode", 0xFF);
    haveKey = p.getBytes("key", key, 16) == 16;
    savedChannel = p.getUChar("chan", 0);
    p.end();
  }
}

void linkEnsureKey() {
  if (haveKey) return;
  randomBytes(key, 16);
  Preferences p;
  if (p.begin("wllink", false)) {
    haveKey = p.putBytes("key", key, 16) == 16;
    p.end();
  }
  if (haveKey) blink.setKey(key);
}

LinkMode linkMode() { return mode == LINK_ESPNOW ? LINK_ESPNOW : LINK_WIFI; }
bool linkModeChosen() { return mode != 0xFF; }

bool linkSetMode(LinkMode m) {
  Preferences p;
  if (!p.begin("wllink", false)) return false;
  bool ok = p.putUChar("mode", m) == 1;
  p.end();
  if (ok) mode = m;
  return ok;
}

bool linkSetKey(const uint8_t k[16]) {
  Preferences p;
  if (!p.begin("wllink", false)) return false;
  bool ok = p.putBytes("key", k, 16) == 16;
  p.end();
  if (!ok) return false;
  memcpy(key, k, 16);
  haveKey = true;
  blink.setKey(key);
  return true;
}

const uint8_t* linkKey() { return haveKey ? key : nullptr; }

void linkFingerprint(char out[9]) {
  if (haveKey) wll::fingerprint(key, out);
  else out[0] = 0;
}

uint8_t linkSavedChannel() { return savedChannel; }

void linkSaveChannel(uint8_t ch) {
  if (ch == savedChannel) return;
  Preferences p;
  if (p.begin("wllink", false)) {
    p.putUChar("chan", ch);
    p.end();
    savedChannel = ch;
  }
}

void linkForgetChannel() {
  Preferences p;
  if (p.begin("wllink", false)) {
    p.remove("chan");
    p.end();
  }
  savedChannel = 0;
}

void linkStart(wifi_interface_t ifx, uint8_t channel, uint8_t announceMode) {
  linkStop();
  if (!rxq) rxq = xQueueCreate(24, sizeof(Frame));
  radioIf = ifx;
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  esp_wifi_config_espnow_rate(ifx, WIFI_PHY_RATE_12M);  // a tenth of the airtime of the 1 Mbps default
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST, 6);
  p.ifidx = ifx;
  esp_now_add_peer(&p);
  txInFlight = 0;
  txWaitSince = 0;
  running = true;
  blink.send = radioSend;
  blink.deliver = deliver;
  blink.onUp = up;
  blink.onDown = down;
  static bool begun = false;
  if (!begun) {
    blink.begin(true, 16384);  // room for every open connection's window, and commands
    begun = true;
  }
  if (haveKey) blink.setKey(key);
  blink.setAnnounce(announceMode, channel);
}

void linkStop() {
  if (!running) return;
  blink.drop();
  running = false;
  esp_now_deinit();
}

void linkLoop() {
  if (!running) return;
  uint32_t now = millis();
  // Seen once: four frames never confirmed, and nothing else left the radio for hours (the link's HELLOs and
  // the Wi-Fi network's DHCP offers alike). Starting the radio over clears it.
  portENTER_CRITICAL(&txMux);
  bool stalled = txInFlight > 0 && now - txWaitSince > STALL_MS;
  if (stalled) txInFlight = 0;
  portEXIT_CRITICAL(&txMux);
  if (stalled) {
    stalls++;
    onLinkStall();
  }
  Frame f;
  while (xQueueReceive(rxq, &f, 0) == pdTRUE) blink.onFrame(f.mac, f.data, f.len, now);
  blink.tick(now);
}

bool linkUp() { return running && blink.up(); }

bool linkPeerMac(uint8_t out[6]) {
  if (!blink.hasPeer()) return false;
  memcpy(out, blink.peer(), 6);
  return true;
}

bool linkSendMsg(uint8_t type, const uint8_t* payload, size_t len) { return running && blink.sendMsg2(type, payload, len); }

bool linkSendDatagram(uint8_t type, const uint8_t* payload, size_t len) {
  return running && blink.sendDatagram2(type, payload, len);
}

uint32_t linkFramesSent() { return framesSent; }
uint32_t linkFramesLost() { return framesLost; }
uint32_t linkStalls() { return stalls; }
void linkStatsText(char* out, size_t cap) { blink.statsText(out, cap); }
