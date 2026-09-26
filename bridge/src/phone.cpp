#include "phone.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <algorithm>

#include "config.h"
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include "wled.h"

// The phone page (pc/phone/index.html) uses the same UUIDs.
static const char* SVC_UUID = "8d2a0001-3c55-4b6e-a7f1-6f2b5c0e9a41";
static const char* CMD_UUID = "8d2a0002-3c55-4b6e-a7f1-6f2b5c0e9a41";    // write: WLED JSON state, e.g. {"on":false}
static const char* STATE_UUID = "8d2a0003-3c55-4b6e-a7f1-6f2b5c0e9a41";  // read/notify: the light's state (wledStateJson)
static const char* TUNNEL_UUID = "8d2a0004-3c55-4b6e-a7f1-6f2b5c0e9a41"; // write/notify: WLED's web server (phone.h)
// Bump when characteristics are added: iPhones keep a paired device's layout, and are told to read it again.
static const uint32_t GATT_LAYOUT = 2;

static const int MAX_CONN = 3;
static const int BOOT_BUTTON = 0;
static const uint32_t UNPAIRED_KICK_MS = 30000;  // connections that don't pair in time get dropped
// Apple's advice for accessories: advertise every 20 ms while a phone may be about to connect (here: for 30 s
// after the bridge starts or a phone leaves, so reopening the phone page finds it at once), then at 152.5 ms.
static const uint16_t ADV_FAST = 32, ADV_SLOW = 244;  // units of 0.625 ms
static const uint32_t ADV_FAST_MS = 30000;

struct Link {
  uint16_t handle = 0xFFFF;  // 0xFFFF = unused
  uint32_t since = 0;
  bool approved = false;      // paired, and one of our phones
  bool subscribed = false;    // asked for state notifications
  bool tunnel = false;        // asked for the tunnel's notifications
};

static Link links[MAX_CONN];
static uint32_t pin = 0;
static volatile uint32_t pairingUntil = 0;
static NimBLEServer* server = nullptr;
static NimBLECharacteristic* stateChr = nullptr;
// diagnostics: times advertising had to be restarted, pairings refused
static volatile uint32_t advRestarts = 0, refused = 0;
static volatile uint32_t passkeyAt = 0;  // when a new pairing last asked for the PIN
static volatile uint8_t lastRefusal = 0; // bits: 1 encrypted, 2 authenticated, 4 new pairing, 8 pairing was open
static volatile uint32_t fastAdvUntil = 0;
static portMUX_TYPE listMux = portMUX_INITIALIZER_UNLOCKED;  // links[] is shared with the BLE task

// The tunnel: phone -> bridge bytes as written (the BLE task adds, the main loop takes), and bridge -> phone
// messages waiting to be notified (main loop only).
static const size_t TUN_IN = 6144, TUN_OUT = 8192;
static const size_t TUN_CONTROL = 256;  // kept free for everything but data, so a close always fits
static const size_t TUN_MSG_MAX = 2048;
static const int MSYS_RESERVE = 6;      // Bluetooth buffers left for everything else while the tunnel sends
static const uint8_t TUN_DATA = 0x91;   // B_TCP_DATA
static NimBLECharacteristic* tunnelChr = nullptr;
static uint8_t tunIn[TUN_IN];
static size_t inHead = 0, inLen = 0;
static volatile uint16_t tunOwner = 0xFFFF;
static volatile uint32_t tunEpoch = 0;
static volatile bool tunOverflow = false;
static portMUX_TYPE tunMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t tunOut[TUN_OUT];
static size_t outHead = 0, outLen = 0, headSent = 0;  // headSent: bytes of the first message already notified
static uint32_t outEpoch = 0;
static uint32_t drained[PHONE_SLOTS], queued[PHONE_SLOTS];
static const uint16_t TEST_OWNER = 0xFFFE;  // the PC, testing the tunnel over USB (phoneTunnelTest)
static bool (*testOut)(const uint8_t*, size_t) = nullptr;

// The phones allowed in are the ones the bridge holds a pairing (bond) for: it only keeps pairings made while
// pairing was open, and "Forget phones" deletes them all.
static void loadPin() {
  Preferences p;
  if (p.begin("wlble", true)) {
    pin = p.getUInt("pin", 0);
    p.end();
  }
  if (pin < 100000 || pin > 999999 || pin == 123456) {  // 123456 is NimBLE's "ask the callback" value
    do pin = esp_random() % 1000000; while (pin < 100000 || pin == 123456);
    Preferences w;
    if (w.begin("wlble", false)) {
      w.putUInt("pin", pin);
      w.end();
    }
  }
}

static void advertise(bool fast) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv->isAdvertising()) adv->stop();
  adv->setMinInterval(fast ? ADV_FAST : ADV_SLOW);
  adv->setMaxInterval(fast ? ADV_FAST : ADV_SLOW);
  adv->start();
}

// ---------------------------------------------------------------------------------------------
// connections

static Link* linkFor(uint16_t handle, bool create) {  // call with listMux held
  for (auto& l : links)
    if (l.handle == handle) return &l;
  if (!create) return nullptr;
  for (auto& l : links)
    if (l.handle == 0xFFFF) {
      l = Link();
      l.handle = handle;
      l.since = millis();
      return &l;
    }
  return nullptr;
}

static void pushState(bool notify) {
  char buf[200];
  size_t n = wledStateJson(buf, sizeof buf);
  if (!n || !stateChr) return;
  stateChr->setValue((const uint8_t*)buf, n);
  if (!notify) return;
  uint16_t targets[MAX_CONN];
  int count = 0;
  portENTER_CRITICAL(&listMux);
  for (auto& l : links)
    if (l.handle != 0xFFFF && l.approved && l.subscribed) targets[count++] = l.handle;
  portEXIT_CRITICAL(&listMux);
  // notified only to approved phones (NimBLE's own notify() would reach any subscriber)
  for (int i = 0; i < count; i++)
    if (server->getPeerMTU(targets[i]) >= n + 3)
      ble_gattc_notify_custom(targets[i], stateChr->getHandle(), ble_hs_mbuf_from_flat(buf, n));
}

static bool isApproved(ble_gap_conn_desc* desc) {
  bool ok = false;
  portENTER_CRITICAL(&listMux);
  if (Link* l = linkFor(desc->conn_handle, false)) ok = l->approved;
  portEXIT_CRITICAL(&listMux);
  return ok && desc->sec_state.authenticated;
}

// ---------------------------------------------------------------------------------------------
// the tunnel, bridge -> phone

static uint8_t outAt(size_t i) { return tunOut[(outHead + i) % TUN_OUT]; }

static void syncOut() {  // what was queued for another phone (or one that left) is dropped
  uint32_t e = tunEpoch;
  if (e == outEpoch) return;
  outEpoch = e;
  phoneTunnelClear();
}

// n bytes from the front went out: whole messages leave the queue, and data is counted for the credit
static void sent(size_t n) {
  while (n) {
    size_t total = 4 + (outAt(2) | outAt(3) << 8), k = min(n, total - headSent);
    uint8_t slot = outAt(1);
    if (outAt(0) == TUN_DATA && slot < PHONE_SLOTS && headSent + k > 4) {
      size_t data = headSent + k - max(headSent, (size_t)4);
      drained[slot] += data;
      queued[slot] -= data;
    }
    headSent += k;
    n -= k;
    if (headSent == total) {
      outHead = (outHead + total) % TUN_OUT;
      outLen -= total;
      headSent = 0;
    }
  }
}

// As much as the phone's link takes: packets as big as its MTU allows, while Bluetooth has buffers to spare.
static void pumpTunnel() {
  syncOut();
  uint16_t owner = tunOwner;
  if (owner == 0xFFFF || outLen == headSent || !tunnelChr) return;
  if (owner == TEST_OWNER) {
    uint8_t pkt[240];
    while (outLen > headSent && testOut) {
      size_t n = min(outLen - headSent, sizeof pkt);
      for (size_t i = 0; i < n; i++) pkt[i] = outAt(headSent + i);
      if (!testOut(pkt, n)) break;
      sent(n);
    }
    return;
  }
  bool listening = false;
  portENTER_CRITICAL(&listMux);
  if (Link* l = linkFor(owner, false)) listening = l->approved && l->tunnel;
  portEXIT_CRITICAL(&listMux);
  uint16_t mtu = server->getPeerMTU(owner);
  if (!listening || mtu < 23) return;
  size_t most = min((size_t)mtu - 3, (size_t)244);
  uint8_t pkt[244];
  while (outLen > headSent && os_msys_num_free() >= MSYS_RESERVE) {
    size_t n = min(outLen - headSent, most);
    for (size_t i = 0; i < n; i++) pkt[i] = outAt(headSent + i);
    os_mbuf* om = ble_hs_mbuf_from_flat(pkt, n);
    if (!om || ble_gattc_notify_custom(owner, tunnelChr->getHandle(), om) != 0) break;  // (om is freed either way)
    sent(n);
  }
}

size_t phoneTunnelRead(uint8_t* out, size_t cap, uint32_t* epoch) {
  size_t n = 0;
  portENTER_CRITICAL(&tunMux);
  *epoch = tunEpoch;
  if (inLen >= 4) {
    size_t len = tunIn[(inHead + 2) % TUN_IN] | tunIn[(inHead + 3) % TUN_IN] << 8;
    if (len > TUN_MSG_MAX) {
      tunOverflow = true;  // not a message of ours: start over
      inLen = 0;
    } else if (inLen >= 4 + len) {
      if (len + 2 <= cap) {
        out[0] = tunIn[inHead];
        out[1] = tunIn[(inHead + 1) % TUN_IN];
        for (size_t i = 0; i < len; i++) out[2 + i] = tunIn[(inHead + 4 + i) % TUN_IN];
        n = len + 2;
      }
      inHead = (inHead + 4 + len) % TUN_IN;
      inLen -= 4 + len;
    }
  }
  portEXIT_CRITICAL(&tunMux);
  return n;
}

uint32_t phoneTunnelEpoch() { return tunEpoch; }

static void takeOver(uint16_t owner) {  // call with tunMux held
  if (tunOwner == owner) return;
  tunOwner = owner;
  tunEpoch++;
  inHead = inLen = 0;
}

static void addInput(const uint8_t* d, size_t n) {  // call with tunMux held
  if (inLen + n > TUN_IN) {
    tunOverflow = true;  // the phone ignored the windows: start it over
    return;
  }
  for (size_t i = 0; i < n; i++) tunIn[(inHead + inLen + i) % TUN_IN] = d[i];
  inLen += n;
}

void phoneTunnelTest(const uint8_t* bytes, size_t len, bool (*out)(const uint8_t*, size_t)) {
  testOut = out;
  portENTER_CRITICAL(&tunMux);
  takeOver(TEST_OWNER);
  addInput(bytes, len);
  portEXIT_CRITICAL(&tunMux);
}

void phoneTunnelTestEnd() {
  portENTER_CRITICAL(&tunMux);
  if (tunOwner == TEST_OWNER) takeOver(0xFFFF);
  portEXIT_CRITICAL(&tunMux);
}

bool phoneTunnelSend(uint8_t type, uint8_t slot, const uint8_t* payload, size_t len) {
  syncOut();
  if (tunOwner == 0xFFFF) return true;  // nobody to tell
  if (outLen + 4 + len > (type == TUN_DATA ? TUN_OUT - TUN_CONTROL : TUN_OUT)) return false;
  const uint8_t head[4] = {type, slot, (uint8_t)len, (uint8_t)(len >> 8)};
  for (size_t i = 0; i < 4; i++) tunOut[(outHead + outLen + i) % TUN_OUT] = head[i];
  for (size_t i = 0; i < len; i++) tunOut[(outHead + outLen + 4 + i) % TUN_OUT] = payload[i];
  outLen += 4 + len;
  if (type == TUN_DATA && slot < PHONE_SLOTS) queued[slot] += len;
  return true;
}

size_t phoneTunnelRoom() {
  syncOut();
  return outLen + 4 + TUN_CONTROL < TUN_OUT ? TUN_OUT - TUN_CONTROL - outLen - 4 : 0;
}

void phoneTunnelClear() {
  outHead = outLen = headSent = 0;
  for (int i = 0; i < PHONE_SLOTS; i++) drained[i] = queued[i] = 0;
}

uint32_t phoneTunnelDrained(uint8_t slot, bool* idle) {
  syncOut();
  if (slot >= PHONE_SLOTS) return 0;
  *idle = queued[slot] == 0;
  return drained[slot];
}

void phoneTunnelCredited(uint8_t slot, uint32_t bytes) {
  if (slot < PHONE_SLOTS) drained[slot] -= min(bytes, drained[slot]);
}

// ---------------------------------------------------------------------------------------------

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    portENTER_CRITICAL(&listMux);
    linkFor(desc->conn_handle, true);
    portEXIT_CRITICAL(&listMux);
    NimBLEDevice::startSecurity(desc->conn_handle);  // encrypted at once (iOS asks for the PIN the first time)
    // 15-30 ms connection events (Apple's fastest) from the start, so setting up the page's link is quick too
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    server->setDataLen(desc->conn_handle, 251);  // long radio packets: WLED's page loads several times faster
    fastAdvUntil = 0;
    advertise(false);  // stay discoverable for another phone
  }

  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    portENTER_CRITICAL(&listMux);
    if (Link* l = linkFor(desc->conn_handle, false)) *l = Link();
    portEXIT_CRITICAL(&listMux);
    portENTER_CRITICAL(&tunMux);
    if (tunOwner == desc->conn_handle) {  // its connections to WLED go too (main.cpp sees the new epoch)
      tunOwner = 0xFFFF;
      tunEpoch++;
      inLen = 0;
    }
    portEXIT_CRITICAL(&tunMux);
    fastAdvUntil = millis() + ADV_FAST_MS;  // the phone may be back in a moment
    advertise(true);
  }

  // Only a new pairing asks for the PIN; a phone coming back encrypts with the pairing it already has.
  uint32_t onPassKeyRequest() override {
    passkeyAt = millis() | 1;
    return pin;
  }

  // A phone that comes back is recognised by its pairing, not its address: iPhones change their Bluetooth
  // address all the time, and matching addresses turned returning phones away (and deleted their pairing).
  // The bridge only keeps pairings made while pairing was open, so an encrypted, PIN-authenticated link on
  // an existing pairing is one of ours; a new pairing needs the pairing window.
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    bool secure = desc->sec_state.encrypted && desc->sec_state.authenticated;
    bool fresh = passkeyAt && millis() - passkeyAt < 30000;
    passkeyAt = 0;
    bool open = phonePairingLeft() > 0;
    bool ok = secure && (!fresh || open);
    if (ok && fresh) pairingUntil = 0;  // one phone per window
    if (!ok) {
      refused++;
      lastRefusal = (desc->sec_state.encrypted ? 1 : 0) | (desc->sec_state.authenticated ? 2 : 0) | (fresh ? 4 : 0) | (open ? 8 : 0);
      if (fresh) NimBLEDevice::deleteBond(NimBLEAddress(desc->peer_id_addr));  // a pairing we didn't want
      server->disconnect(desc->conn_handle);
      return;
    }
    portENTER_CRITICAL(&listMux);
    if (Link* l = linkFor(desc->conn_handle, true)) l->approved = true;
    portEXIT_CRITICAL(&listMux);
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 400);  // again, in case the first ask came too early
    pushState(true);
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    if (!isApproved(desc)) return;
    NimBLEAttValue v = c->getValue();
    wledSend((const char*)v.data(), v.length());
  }
};

class TunnelCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    if (!isApproved(desc)) return;
    NimBLEAttValue v = c->getValue();
    const uint8_t* d = v.data();
    size_t n = v.length();
    portENTER_CRITICAL(&tunMux);
    takeOver(desc->conn_handle);  // another phone takes the tunnel over, from scratch
    addInput(d, n);
    portEXIT_CRITICAL(&tunMux);
  }

  void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc* desc, uint16_t subValue) override {
    portENTER_CRITICAL(&listMux);
    if (Link* l = linkFor(desc->conn_handle, false)) l->tunnel = subValue & 1;
    portEXIT_CRITICAL(&listMux);
  }
};

class StateCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc* desc, uint16_t subValue) override {
    portENTER_CRITICAL(&listMux);
    if (Link* l = linkFor(desc->conn_handle, false)) l->subscribed = subValue & 1;
    portEXIT_CRITICAL(&listMux);
    if (subValue & 1) pushState(true);
  }
};

// ---------------------------------------------------------------------------------------------

void phoneBegin() {
  loadPin();
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  NimBLEDevice::init(WL_BLE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // 0 dBm: one room
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM protection (PIN), LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(false);  // ours: onDisconnect advertises fast
  NimBLEService* svc = server->createService(SVC_UUID);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512);
  cmd->setCallbacks(new CommandCallbacks());
  stateChr = svc->createCharacteristic(
      STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY, 200);
  stateChr->setCallbacks(new StateCallbacks());
  tunnelChr = svc->createCharacteristic(
      TUNNEL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN |
      NIMBLE_PROPERTY::NOTIFY, 512);
  tunnelChr->setCallbacks(new TunnelCallbacks());
  pushState(false);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);  // the name, for the phone's device list
  fastAdvUntil = millis() + ADV_FAST_MS;
  advertise(true);  // (this also starts the GATT server)

  Preferences p;  // characteristics were added: paired phones are told to read the layout again when they're back
  if (p.begin("wlble", false)) {
    if (p.getUInt("gatt", 0) != GATT_LAYOUT) {
      ble_svc_gatt_changed(0x0001, 0xFFFF);
      p.putUInt("gatt", GATT_LAYOUT);
    }
    p.end();
  }
}

void phoneLoop() {
  static uint32_t pressedAt = 0, lastSeq = 0;
  if (digitalRead(BOOT_BUTTON) == LOW) {  // hold BOOT for 2 s to let a new phone pair
    if (!pressedAt) pressedAt = millis();
    else if (millis() - pressedAt > 2000 && phonePairingLeft() == 0) phoneOpenPairing(120);
  } else {
    pressedAt = 0;
  }

  uint32_t seq = wledStateSeq();
  if (seq != lastSeq) {
    lastSeq = seq;
    pushState(true);
  }

  uint16_t kick = 0xFFFF;  // drop connections that never paired; disconnect outside the lock
  portENTER_CRITICAL(&listMux);
  for (auto& l : links)
    if (l.handle != 0xFFFF && !l.approved && millis() - l.since > UNPAIRED_KICK_MS && kick == 0xFFFF) kick = l.handle;
  portEXIT_CRITICAL(&listMux);
  if (kick != 0xFFFF) server->disconnect(kick);
  if (tunOverflow) {  // the tunnel's stream is out of step: the phone reconnects and starts it over
    tunOverflow = false;
    uint16_t owner = tunOwner;
    if (owner == TEST_OWNER) phoneTunnelTestEnd();
    else if (owner != 0xFFFF) server->disconnect(owner);
  }
  pumpTunnel();

  // Our own link table and NimBLE's connection list must agree, and a phone must always be able to find the
  // bridge while there is room for another connection. Checked every 2 s, so nothing can stay stuck.
  static uint32_t lastCheck = 0;
  if (server && millis() - lastCheck > 2000) {
    lastCheck = millis();
    std::vector<uint16_t> peers = server->getPeerDevices();
    portENTER_CRITICAL(&listMux);
    for (auto& l : links)
      if (l.handle != 0xFFFF && std::find(peers.begin(), peers.end(), l.handle) == peers.end()) l = Link();  // gone
    portEXIT_CRITICAL(&listMux);
    bool fast = fastAdvUntil && (int32_t)(millis() - fastAdvUntil) < 0;
    if (fastAdvUntil && !fast) {  // no phone came back quickly: slow down
      fastAdvUntil = 0;
      if (NimBLEDevice::getAdvertising()->isAdvertising()) advertise(false);
    }
    if ((int)peers.size() < MAX_CONN && !NimBLEDevice::getAdvertising()->isAdvertising()) {
      advRestarts++;
      advertise(fast);
    }
  }
}

void phoneEnd() { NimBLEDevice::deinit(true); }

void phoneOpenPairing(uint16_t seconds) { pairingUntil = millis() + seconds * 1000UL; }

void phoneForget() {
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

int phoneDiag(char* out, size_t cap) {
  if (!server) return snprintf(out, cap, "off");  // Bluetooth not started yet
  int conns = (int)server->getConnectedCount();
  bool adv = NimBLEDevice::getAdvertising()->isAdvertising();
  return snprintf(out, cap, "c%d a%d r%lu x%lu/%u b%d", conns, adv ? 1 : 0, (unsigned long)advRestarts, (unsigned long)refused,
                  lastRefusal, NimBLEDevice::getNumBonds());
}
int phoneCount() { return server ? NimBLEDevice::getNumBonds() : 0; }  // (NimBLE can only be asked once it runs)

int phonePairingLeft() {
  uint32_t until = pairingUntil;
  int32_t left = (int32_t)(until - millis());
  return until && left > 0 ? (left + 999) / 1000 : 0;
}
