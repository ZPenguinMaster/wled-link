#include "phone.h"

#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <esp_wifi.h>

#include "config.h"

// The phone page (pc/phone/index.html) uses the same UUIDs.
static const char* SVC_UUID = "8d2a0001-3c55-4b6e-a7f1-6f2b5c0e9a41";
static const char* CMD_UUID = "8d2a0002-3c55-4b6e-a7f1-6f2b5c0e9a41";    // write: WLED JSON state, e.g. {"on":false}
static const char* STATE_UUID = "8d2a0003-3c55-4b6e-a7f1-6f2b5c0e9a41";  // read: {"n":seq,"ok":1,"on":..,"bri":..,"lor":..}

static const int MAX_PHONES = 4;
static const int BOOT_BUTTON = 0;
static const uint32_t UNPAIRED_KICK_MS = 30000;  // connections that don't pair in time get dropped

struct PhoneId {
  uint8_t type;
  uint8_t addr[6];
};

struct Job {
  uint16_t len;  // 0 = just refresh the state
  bool fromPc;
  char json[512];
};

static PhoneId phones[MAX_PHONES];
static int numPhones = 0;
static uint32_t pin = 0;
static volatile uint32_t pairingUntil = 0;
static NimBLEServer* server = nullptr;
static NimBLECharacteristic* stateChr = nullptr;
static QueueHandle_t jobs = nullptr;
static uint32_t wledIp = 0;  // network byte order, 0 = not found yet
static uint32_t seq = 0;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char pcResult[200];
static volatile bool pcResultReady = false;
static uint16_t pendingConn[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
static uint32_t pendingSince[4];

// ---------------------------------------------------------------------------------------------
// paired phones (our own allowlist, on top of NimBLE's bond store)

static void loadPhones() {
  Preferences p;
  if (p.begin("wlble", true)) {
    numPhones = p.getBytes("ids", phones, sizeof phones) / sizeof(PhoneId);
    pin = p.getUInt("pin", 0);
    p.end();
  }
  if (pin == 0 || pin > 999999 || pin == 123456) {  // 123456 is NimBLE's "ask the callback" value
    do pin = esp_random() % 1000000; while (pin < 100000 || pin == 123456);
    Preferences w;
    if (w.begin("wlble", false)) {
      w.putUInt("pin", pin);
      w.end();
    }
  }
}

static void savePhones() {
  Preferences p;
  if (p.begin("wlble", false)) {
    p.putBytes("ids", phones, numPhones * sizeof(PhoneId));
    p.end();
  }
}

static portMUX_TYPE listMux = portMUX_INITIALIZER_UNLOCKED;  // phones[] and pendingConn[] are shared with the BLE task

static bool known(const ble_addr_t& a) {
  bool found = false;
  portENTER_CRITICAL(&listMux);
  for (int i = 0; i < numPhones && !found; i++)
    found = phones[i].type == a.type && memcmp(phones[i].addr, a.val, 6) == 0;
  portEXIT_CRITICAL(&listMux);
  return found;
}

static void addPhone(const ble_addr_t& a) {
  if (known(a)) return;
  portENTER_CRITICAL(&listMux);
  if (numPhones == MAX_PHONES) {  // forget the oldest
    memmove(phones, phones + 1, (MAX_PHONES - 1) * sizeof(PhoneId));
    numPhones--;
  }
  phones[numPhones].type = a.type;
  memcpy(phones[numPhones].addr, a.val, 6);
  numPhones++;
  portEXIT_CRITICAL(&listMux);
  savePhones();
}

// ---------------------------------------------------------------------------------------------
// talking to WLED over the bridge's Wi-Fi (runs in its own task, never in the BLE callbacks)

static int wledHttp(uint32_t ip, const char* method, const char* path, const char* body, String* out) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(client, IPAddress(ip).toString(), 80, path)) return -1;
  int code;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)body, strlen(body));
  } else {
    code = http.GET();
  }
  if (out && code > 0) *out = http.getString();
  http.end();
  return code;
}

static bool findWled() {
  wifi_sta_list_t wifiList;
  esp_netif_sta_list_t ipList;
  if (esp_wifi_ap_get_sta_list(&wifiList) != ESP_OK || esp_netif_get_sta_list(&wifiList, &ipList) != ESP_OK) return false;
  for (int i = 0; i < ipList.num; i++) {
    uint32_t ip = ipList.sta[i].ip.addr;
    String info;
    if (ip && wledHttp(ip, "GET", "/json/info", nullptr, &info) == 200 &&
        (info.indexOf("\"brand\":\"WLED\"") >= 0 || info.indexOf("\"leds\"") >= 0)) {
      wledIp = ip;
      return true;
    }
  }
  return false;
}

// Picks "on", "bri" and "lor" out of WLED's state JSON (no JSON library needed for three fields).
static int field(const String& s, const char* key) {
  int at = s.indexOf(key);
  if (at < 0) return -1;
  at += strlen(key);
  if (s.startsWith("true", at)) return 1;
  if (s.startsWith("false", at)) return 0;
  return s.substring(at).toInt();
}

static void worker(void*) {
  static Job job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    job.json[job.len < sizeof job.json ? job.len : sizeof job.json - 1] = 0;
    String state;
    bool ok = false;
    const char* err = "WLED isn't connected to the bridge";
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!wledIp && !findWled()) continue;
      if (job.len && wledHttp(wledIp, "POST", "/json/state", job.json, nullptr) != 200) {
        wledIp = 0;
        err = "WLED didn't accept the command";
        continue;
      }
      ok = wledHttp(wledIp, "GET", "/json/state", nullptr, &state) == 200;
      if (!ok) wledIp = 0;
    }
    char summary[200];
    seq++;
    if (ok)
      snprintf(summary, sizeof summary, "{\"n\":%lu,\"ok\":1,\"on\":%s,\"bri\":%d,\"lor\":%d}", (unsigned long)seq,
               field(state, "\"on\":") == 1 ? "true" : "false", field(state, "\"bri\":"), field(state, "\"lor\":"));
    else
      snprintf(summary, sizeof summary, "{\"n\":%lu,\"ok\":0,\"err\":\"%s\"}", (unsigned long)seq, err);
    if (stateChr) stateChr->setValue((const uint8_t*)summary, strlen(summary));
    if (job.fromPc) {
      portENTER_CRITICAL(&mux);
      strlcpy(pcResult, summary, sizeof pcResult);
      pcResultReady = true;
      portEXIT_CRITICAL(&mux);
    }
  }
}

static void enqueue(const char* json, size_t len, bool fromPc) {
  Job job;  // copied into the queue; the BLE host task and the main loop can both call this
  job.len = len < sizeof job.json ? len : sizeof job.json - 1;
  job.fromPc = fromPc;
  memcpy(job.json, json, job.len);
  xQueueSend(jobs, &job, 0);
}

// ---------------------------------------------------------------------------------------------
// BLE

static void unpend(uint16_t handle) {
  portENTER_CRITICAL(&listMux);
  for (int i = 0; i < 4; i++)
    if (pendingConn[i] == handle) pendingConn[i] = 0xFFFF;
  portEXIT_CRITICAL(&listMux);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    portENTER_CRITICAL(&listMux);
    for (int i = 0; i < 4; i++)
      if (pendingConn[i] == 0xFFFF) {
        pendingConn[i] = desc->conn_handle;
        pendingSince[i] = millis();
        break;
      }
    portEXIT_CRITICAL(&listMux);
    NimBLEDevice::startSecurity(desc->conn_handle);  // iOS asks for the PIN the first time
    NimBLEDevice::startAdvertising();                // stay discoverable for a second phone
  }

  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* desc) override { unpend(desc->conn_handle); }

  uint32_t onPassKeyRequest() override { return pin; }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    unpend(desc->conn_handle);
    bool ok = desc->sec_state.encrypted && desc->sec_state.authenticated;
    if (ok && !known(desc->peer_id_addr)) {
      if (phonePairingLeft() > 0) {
        addPhone(desc->peer_id_addr);
        pairingUntil = 0;  // one phone per window
      } else {
        ok = false;  // right PIN, but pairing wasn't open
      }
    }
    if (!ok) {
      NimBLEDevice::deleteBond(NimBLEAddress(desc->peer_id_addr));
      server->disconnect(desc->conn_handle);
    }
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    if (!desc->sec_state.authenticated || !known(desc->peer_id_addr)) return;
    NimBLEAttValue v = c->getValue();
    enqueue((const char*)v.data(), v.length(), false);
  }
};

void phoneBegin() {
  loadPhones();
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  jobs = xQueueCreate(4, sizeof(Job));
  xTaskCreatePinnedToCore(worker, "phone", 8192, nullptr, 1, nullptr, 1);

  NimBLEDevice::init(WL_BLE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // 0 dBm: one room
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM protection (PIN), LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* svc = server->createService(SVC_UUID);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512);
  cmd->setCallbacks(new CommandCallbacks());
  stateChr = svc->createCharacteristic(
      STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN, 200);
  stateChr->setValue("{\"n\":0}");
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
}

void phoneLoop() {
  static uint32_t pressedAt = 0;
  if (digitalRead(BOOT_BUTTON) == LOW) {  // hold BOOT for 2 s to let a new phone pair
    if (!pressedAt) pressedAt = millis();
    else if (millis() - pressedAt > 2000 && phonePairingLeft() == 0) phoneOpenPairing(120);
  } else {
    pressedAt = 0;
  }
  uint16_t kick = 0xFFFF;  // disconnect outside the lock
  portENTER_CRITICAL(&listMux);
  for (int i = 0; i < 4 && kick == 0xFFFF; i++)
    if (pendingConn[i] != 0xFFFF && millis() - pendingSince[i] > UNPAIRED_KICK_MS) {
      kick = pendingConn[i];
      pendingConn[i] = 0xFFFF;
    }
  portEXIT_CRITICAL(&listMux);
  if (kick != 0xFFFF) server->disconnect(kick);
}

void phoneOpenPairing(uint16_t seconds) { pairingUntil = millis() + seconds * 1000UL; }

void phoneForget() {
  portENTER_CRITICAL(&listMux);
  numPhones = 0;
  portEXIT_CRITICAL(&listMux);
  savePhones();
  NimBLEDevice::deleteAllBonds();
  if (server)
    for (uint16_t id : server->getPeerDevices()) server->disconnect(id);
}

bool phoneSetPin(uint32_t newPin) {
  if (newPin < 100000 || newPin > 999999 || newPin == 123456) return false;
  pin = newPin;
  Preferences p;
  if (p.begin("wlble", false)) {
    p.putUInt("pin", pin);
    p.end();
  }
  NimBLEDevice::setSecurityPasskey(pin);
  return true;
}

uint32_t phonePin() { return pin; }
int phoneCount() { return numPhones; }

int phonePairingLeft() {
  uint32_t until = pairingUntil;
  int32_t left = (int32_t)(until - millis());
  return until && left > 0 ? (left + 999) / 1000 : 0;
}

void phoneInject(const char* json, size_t len) { enqueue(json, len, true); }

bool phoneTakeResult(char* out, size_t cap) {
  if (!pcResultReady) return false;
  portENTER_CRITICAL(&mux);
  strlcpy(out, pcResult, cap);
  pcResultReady = false;
  portEXIT_CRITICAL(&mux);
  return true;
}
