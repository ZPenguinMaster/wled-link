// WLED Link usermod: the light's end of the radio link to the bridge ESP32 (see link_proto.h), and
// bringing back the light's last look after a power cut.
//
// ESP-NOW mode: no Wi-Fi network at all. Everything the PC sends arrives over the encrypted ESP-NOW link
// and is served right here: TCP connections go to WLED's own web server through the loopback interface
// (127.0.0.1), so the full web UI, the JSON API and firmware updates work; UDP (SignalRGB's colours) goes
// to WLED's UDP ports the same way; JSON commands from the control pages and the phone go straight to
// WLED's state, and every change is pushed back to the bridge at once.
// Wi-Fi mode: WLED joins the bridge's hidden network as before, and the radio link runs alongside.
// The bridge decides: each HELLO it sends names the mode and channel, and the light follows.

#include "wled.h"
#include <Preferences.h>
#include <esp_now.h>
#include <lwip/sockets.h>
#include "link_proto.h"

#ifdef WLED_DISABLE_ESPNOW
#error "WLED Link needs ESP-NOW"
#endif

#define USERMOD_ID_WLEDLINK 190
#define JSON_LOCK_WLEDLINK 40

namespace {

// message types: the same numbers as the bridge's serial protocol (bridge/src/main.cpp)
enum : uint8_t {
  H_WLED_CMD = 0x0B, H_TCP_OPEN = 0x10, H_TCP_DATA = 0x11, H_TCP_CLOSE = 0x12, H_UDP_SEND = 0x20,
  L_RESET = 0x42,  // the PC's session with the bridge ended: close every connection
  L_MODE = 0x43,   // mode(1) channel(1): the bridge is about to switch; follow it now
  B_WLED_STATE = 0x88, B_TCP_OPEN_RESULT = 0x90, B_TCP_DATA = 0x91, B_TCP_CLOSED = 0x92, B_TCP_ACK = 0x93,
};
enum : uint8_t { OPEN_OK = 0, OPEN_BAD_SLOT = 1, OPEN_NO_SOCKET = 2, OPEN_REFUSED = 3, OPEN_TIMEOUT = 4 };
enum : uint8_t { CLOSE_PEER = 0, CLOSE_ERROR = 1, CLOSE_HOST = 2, CLOSE_OVERFLOW = 3 };

const int MAX_CONNS = 6;               // the bridge offers the PC as many
const size_t CONN_WINDOW = 2048;       // the bridge tells the PC this window in ESP-NOW mode
const size_t TCP_CHUNK = 1024;
const size_t CONTROL_ROOM = 512;       // queue space kept free for acks and closes
const uint32_t CONNECT_TIMEOUT_MS = 4000;
const uint32_t CLOSE_TIMEOUT_MS = 5000;
const uint32_t HUNT_AFTER_MS = 15000;  // link down this long: look for the bridge on the other channels
const uint32_t HUNT_EVERY_MS = 30000;
const uint32_t HUNT_DWELL_MS = 250;
const uint32_t CHANNEL_CHECK_MS = 2000;       // ESP-NOW mode: back on the bridge's channel if the radio drifted
const uint32_t WIFI_TRIAL_AFTER_MS = 600000;  // ESP-NOW silent for 10 min: try the bridge's Wi-Fi briefly
const uint32_t WIFI_TRIAL_MS = 25000;
const uint32_t ESPNOW_TRIAL_AFTER_MS = 60000; // Wi-Fi mode but no network for a minute: listen on ESP-NOW
const uint32_t ESPNOW_TRIAL_MS = 20000;
const uint32_t LOOK_SAVE_DELAY_MS = 5000;     // save the look once it has been left alone this long
const uint32_t LOOK_RESTORE_AFTER_MS = 0;     // on WLED's first loop, so its power-on look barely shows
const char* LOOK_FILE = "/wledlink_look.json";
const int RX_QUEUE_FRAMES = 16;               // frames waiting for loop(); a colour frame lost now and then is fine
const size_t LINK_QUEUE = 4096;               // reliable bytes waiting to go to the bridge

struct RxFrame { uint8_t mac[6]; uint8_t len; uint8_t data[250]; };
enum ConnState : uint8_t { C_FREE, C_CONNECTING, C_OPEN, C_CLOSING };
struct Conn {
  ConnState state = C_FREE;
  int sock = -1;
  uint32_t since = 0;
  uint8_t* pending = nullptr;  // only when WLED's side of the socket was full: most connections never need it
  size_t pendingLen = 0, ackOwed = 0;
};

wll::Link wlink;
QueueHandle_t rxq = nullptr;
volatile uint32_t rxFrames = 0;  // our frames that reached WLED at all
Conn conns[MAX_CONNS];
int udpSock = -1;
uint8_t sockBuf[1 + TCP_CHUNK];  // connection id, then the data

// settings: cfg.json (usermod "WLEDLink"), except the key, which stays in NVS where neither WLED's settings
// pages nor its config backups show it
uint8_t cfgMode = wll::MODE_WIFI, cfgChannel = 1;
uint8_t cfgKey[16];
bool cfgHaveKey = false, cfgRestore = true;
bool keyInNvs = false;  // false: NVS couldn't take it, so cfg.json keeps it

// what runs right now (differs from cfgMode during a trial)
uint8_t runMode = wll::MODE_WIFI;
volatile bool settingsChanged = false;
bool localUp = false;
uint32_t runSince = 0, lastUp = 0;
bool trial = false;
uint32_t trialUntil = 0;
bool hunting = false;
uint8_t huntChannel = 0;
uint32_t huntAt = 0, lastHunt = 0, lastChannelCheck = 0;

volatile bool stateDirty = true;
uint32_t lastStatePush = 0;
volatile bool lookDirty = false;
volatile uint32_t lookChangedAt = 0;
bool lookRestored = false;

bool hexToKey(const char* s, uint8_t out[16]) {
  if (!s || strlen(s) != 32) return false;
  for (int i = 0; i < 16; i++) {
    char b[3] = {s[i * 2], s[i * 2 + 1], 0};
    char* end;
    long v = strtol(b, &end, 16);
    if (*end) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

void keyToHex(const uint8_t k[16], char out[33]) {
  for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", k[i]);
}

bool loadKey(uint8_t out[16]) {
  Preferences p;
  if (!p.begin("wllink", true)) return false;  // fails until something was stored
  bool ok = p.getBytes("key", out, 16) == 16;
  p.end();
  return ok;
}

bool storeKey(const uint8_t k[16]) {
  Preferences p;
  if (!p.begin("wllink", false)) return false;
  bool ok = p.putBytes("key", k, 16) == 16;
  p.end();
  return ok;
}

// ---------------------------------------------------------------------------------------------
// radio

// Straight to ESP-NOW, not through QuickEspNow's send(): that one pins each peer to the channel the radio
// was on when ESP-NOW started, and in Wi-Fi mode it starts before WLED has joined the bridge's network, so
// every later send is refused. Channel 0 means "whatever channel the radio is on", in both modes.
bool radioSend(const uint8_t* mac, const uint8_t* frame, size_t len) {
  if (statusESPNow != ESP_NOW_STATE_ON) return false;
  esp_now_peer_info_t p = {};
  if (esp_now_get_peer(mac, &p) == ESP_OK) {
    if (p.channel != 0 || p.ifidx != WIFI_IF_STA) {
      p.channel = 0;
      p.ifidx = WIFI_IF_STA;
      esp_now_mod_peer(&p);
    }
  } else {
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;  // link_proto encrypts
    if (esp_now_add_peer(&p) != ESP_OK) return false;
  }
  return esp_now_send(mac, frame, len) == ESP_OK;
}

// Straight to the radio: QuickEspNow's setChannel() refuses while it follows the Wi-Fi channel.
void tuneTo(uint8_t ch) { esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }

// Switches WLED between an ESP-NOW-only radio (no network, no access point) and its normal Wi-Fi.
void runRadio(uint8_t mode) {
  uint8_t ch = mode == wll::MODE_ESPNOW ? cfgChannel : 0;
  runMode = mode;
  runSince = millis();
  if (espNowOnlyChannel == ch && enableESPNow && statusESPNow == ESP_NOW_STATE_ON) return;
  if (espNowOnlyChannel && ch && enableESPNow && statusESPNow == ESP_NOW_STATE_ON) {  // only the channel moves
    espNowOnlyChannel = ch;
    tuneTo(ch);
    hunting = false;
    return;
  }
  espNowOnlyChannel = ch;
  enableESPNow = true;
  localUp = false;
  hunting = false;
  // Directly, not through forceReconnect: that path also forgets that WLED was ever connected, and then it
  // opens its own hotspot if the bridge's network takes a moment to come back.
  WLED::instance().initConnection();
  if (mode == wll::MODE_WIFI) wasConnected = true;
}

// WLED only starts its web server and UDP ports once it joins a network; ESP-NOW mode never does.
void startLocalInterfaces() {
  server.begin();
  if (udpPort > 0 && udpPort != ntpLocalPort) {
    udpConnected = notifierUdp.begin(udpPort);
    if (udpConnected && udpRgbPort != udpPort) udpRgbConnected = rgbUdp.begin(udpRgbPort);
    if (udpConnected && udpPort2 != udpPort && udpPort2 != udpRgbPort) udp2Connected = notifier2Udp.begin(udpPort2);
  }
  e131.begin(e131Multicast, e131Port, e131Universe, E131_MAX_UNIVERSE_COUNT);
  ddp.begin(false, DDP_DEFAULT_PORT);
  esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_12M);  // a tenth of the airtime of the 1 Mbps default
  localUp = true;
}

// ---------------------------------------------------------------------------------------------
// connections from the PC, served by WLED's own web server over loopback

void sendCtl(uint8_t type, const uint8_t* p, size_t n) { wlink.sendMsg2(type, p, n); }

void connFree(int cid) {
  Conn& c = conns[cid];
  if (c.sock >= 0) lwip_close(c.sock);
  c.sock = -1;
  c.state = C_FREE;
  free(c.pending);
  c.pending = nullptr;
  c.pendingLen = c.ackOwed = 0;
}

void connClosed(int cid, uint8_t reason) {
  connFree(cid);
  uint8_t p[2] = {(uint8_t)cid, reason};
  sendCtl(B_TCP_CLOSED, p, 2);
}

void closeAll() {
  for (int i = 0; i < MAX_CONNS; i++)
    if (conns[i].state != C_FREE) connFree(i);
}

bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }

void openResult(uint8_t cid, uint8_t status) {
  uint8_t p[2] = {cid, status};
  sendCtl(B_TCP_OPEN_RESULT, p, 2);
}

void handleOpen(const uint8_t* p, size_t n) {
  if (n < 7) return;
  uint8_t cid = p[0];
  if (cid >= MAX_CONNS) return openResult(cid, OPEN_BAD_SLOT);
  Conn& c = conns[cid];
  if (c.state != C_FREE) connFree(cid);
  int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return openResult(cid, OPEN_NO_SOCKET);
  lwip_fcntl(s, F_SETFL, lwip_fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  int one = 1;
  lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // whatever address the PC used: it means this light
  addr.sin_port = htons((uint16_t)((p[5] << 8) | p[6]));
  int r = lwip_connect(s, (sockaddr*)&addr, sizeof addr);
  if (r != 0 && errno != EINPROGRESS) {
    lwip_close(s);
    return openResult(cid, OPEN_REFUSED);
  }
  c.sock = s;
  c.state = r == 0 ? C_OPEN : C_CONNECTING;
  c.since = millis();
  c.pendingLen = c.ackOwed = 0;
  if (r == 0) openResult(cid, OPEN_OK);
}

void handleData(const uint8_t* p, size_t n) {
  if (n < 1 || p[0] >= MAX_CONNS) return;
  uint8_t cid = p[0];
  Conn& c = conns[cid];
  if (c.state == C_FREE || c.state == C_CLOSING) return;
  const uint8_t* d = p + 1;
  size_t len = n - 1;
  if (c.state == C_OPEN && c.pendingLen == 0) {
    int k = lwip_send(c.sock, d, len, MSG_DONTWAIT);
    if (k < 0) {
      if (!wouldBlock()) return connClosed(cid, CLOSE_ERROR);
      k = 0;
    }
    c.ackOwed += k;
    d += k;
    len -= k;
  }
  if (len == 0) return;
  if (c.pendingLen + len > CONN_WINDOW) return connClosed(cid, CLOSE_OVERFLOW);
  if (!c.pending && !(c.pending = (uint8_t*)malloc(CONN_WINDOW))) return connClosed(cid, CLOSE_ERROR);
  memcpy(c.pending + c.pendingLen, d, len);
  c.pendingLen += len;
}

void handleClose(const uint8_t* p, size_t n) {
  if (n < 1 || p[0] >= MAX_CONNS) return;
  uint8_t cid = p[0];
  Conn& c = conns[cid];
  if (c.state == C_FREE || c.state == C_CLOSING) return;
  if (c.state == C_CONNECTING || c.pendingLen == 0) return connClosed(cid, CLOSE_HOST);
  c.state = C_CLOSING;
  c.since = millis();
}

void pumpConns() {
  for (int i = 0; i < MAX_CONNS; i++) {
    Conn& c = conns[i];
    if (c.state == C_FREE) continue;
    if (c.state == C_CONNECTING) {
      fd_set wr, ex;
      FD_ZERO(&wr);
      FD_ZERO(&ex);
      FD_SET(c.sock, &wr);
      FD_SET(c.sock, &ex);
      timeval tv = {0, 0};
      if (lwip_select(c.sock + 1, nullptr, &wr, &ex, &tv) > 0) {
        int err = 0;
        socklen_t len = sizeof err;
        lwip_getsockopt(c.sock, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0 || !FD_ISSET(c.sock, &wr)) {
          connFree(i);
          openResult(i, OPEN_REFUSED);
          continue;
        }
        c.state = C_OPEN;
        openResult(i, OPEN_OK);
      } else {
        if (millis() - c.since > CONNECT_TIMEOUT_MS) {
          connFree(i);
          openResult(i, OPEN_TIMEOUT);
        }
        continue;
      }
    }
    if (c.pendingLen > 0) {
      int k = lwip_send(c.sock, c.pending, c.pendingLen, MSG_DONTWAIT);
      if (k > 0) {
        memmove(c.pending, c.pending + k, c.pendingLen - k);
        c.pendingLen -= k;
        c.ackOwed += k;
      } else if (k < 0 && !wouldBlock()) {
        connClosed(i, CLOSE_ERROR);
        continue;
      }
    }
    if (c.ackOwed > 0) {
      uint8_t ack[3] = {(uint8_t)i, (uint8_t)(c.ackOwed & 0xFF), (uint8_t)(c.ackOwed >> 8)};
      sendCtl(B_TCP_ACK, ack, 3);
      c.ackOwed = 0;
    }
    if (c.state == C_CLOSING) {
      if (c.pendingLen == 0) connClosed(i, CLOSE_HOST);
      else if (millis() - c.since > CLOSE_TIMEOUT_MS) connClosed(i, CLOSE_ERROR);
      continue;
    }
    // WLED -> PC, only while the link has room (that is the back pressure towards WLED's web server)
    if (wlink.sendRoom() < TCP_CHUNK + 1 + 2 + CONTROL_ROOM) continue;
    int k = lwip_recv(c.sock, sockBuf + 1, TCP_CHUNK, MSG_DONTWAIT);
    if (k > 0) {
      sockBuf[0] = (uint8_t)i;
      wlink.sendMsg2(B_TCP_DATA, sockBuf, k + 1);
    } else if (k == 0) {
      connClosed(i, CLOSE_PEER);
    } else if (!wouldBlock()) {
      connClosed(i, CLOSE_ERROR);
    }
  }
}

void handleUdp(const uint8_t* p, size_t n) {
  if (n < 7) return;
  if (udpSock < 0) {
    udpSock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock < 0) return;
    lwip_fcntl(udpSock, F_SETFL, lwip_fcntl(udpSock, F_GETFL, 0) | O_NONBLOCK);
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)((p[4] << 8) | p[5]));
  lwip_sendto(udpSock, p + 6, n - 6, MSG_DONTWAIT, (sockaddr*)&addr, sizeof addr);
}

// A JSON state command from the PC or the phone. False (try again) while WLED's JSON buffer is busy.
bool handleCmd(const uint8_t* json, size_t len) {
  if (!requestJSONBufferLock(JSON_LOCK_WLEDLINK)) return false;
  DeserializationError err = deserializeJson(*pDoc, (const char*)json, len);
  if (!err) {
    JsonObject root = pDoc->as<JsonObject>();
    deserializeState(root);
  }
  releaseJSONBufferLock();
  stateDirty = true;  // answer with the resulting state, even if nothing changed
  return true;
}

// "White" in whatever form this strip's auto-white setting turns into the white LEDs alone (the phone can't
// know it; the PC page reads it from WLED's settings): Accurate makes white from full RGB and turns the RGB
// off, None and Dual use the white value as given, Brighter and Max only add white to RGB.
uint32_t fullWhite() {
  uint8_t aw = Bus::getGlobalAWMode();
  if (aw == AW_GLOBAL_DISABLED) {
    Bus* bus = BusManager::getBus(0);
    aw = bus ? bus->getAutoWhiteMode() : RGBW_MODE_MANUAL_ONLY;
  }
  switch (aw) {
    case RGBW_MODE_AUTO_ACCURATE: return RGBW32(255, 255, 255, 0);
    case RGBW_MODE_AUTO_BRIGHTER:
    case RGBW_MODE_MAX: return RGBW32(255, 255, 255, 255);
    default: return RGBW32(0, 0, 0, 255);
  }
}

void pushState() {
  Segment& seg = strip.getMainSegment();
  uint32_t c = seg.colors[0];
  char buf[200];
  int n = snprintf(buf, sizeof buf,
                   "{\"state\":{\"on\":%s,\"bri\":%u,\"lor\":%u,\"ps\":%d,\"seg\":[{\"fx\":%u,\"cct\":%u,\"col\":[[%u,%u,%u,%u]]}]}}",
                   bri ? "true" : "false", briLast, realtimeOverride, currentPreset > 0 ? currentPreset : -1, seg.mode,
                   seg.cct, R(c), G(c), B(c), W(c));
  if (n > 0 && wlink.sendMsg2(B_WLED_STATE, (const uint8_t*)buf, n)) stateDirty = false;
}

// The bridge names the mode and channel in every HELLO; the light follows.
void linkHello(uint8_t mode, uint8_t channel) {
  if (channel < 1 || channel > 13) return;
  bool changed = mode != cfgMode || channel != cfgChannel;
  cfgMode = mode;
  cfgChannel = channel;
  if (changed) {
    configNeedsWrite = true;
    settingsChanged = true;
  }
}

bool linkDeliver(const uint8_t* m, size_t n, bool reliable) {
  if (n < 1) return true;
  switch (m[0]) {
    case H_WLED_CMD: return handleCmd(m + 1, n - 1);
    case H_TCP_OPEN: handleOpen(m + 1, n - 1); return true;
    case H_TCP_DATA: handleData(m + 1, n - 1); return true;
    case H_TCP_CLOSE: handleClose(m + 1, n - 1); return true;
    case H_UDP_SEND: handleUdp(m + 1, n - 1); return true;
    case L_RESET: closeAll(); return true;
    case L_MODE: if (n >= 3) linkHello(m[1], m[2]); return true;
    default: return true;
  }
}

void linkUp() {
  lastUp = millis();
  stateDirty = true;
  hunting = false;
}

void linkDown() {
  closeAll();
}

// ---------------------------------------------------------------------------------------------
// last look

void saveLook() {
  if (!requestJSONBufferLock(JSON_LOCK_WLEDLINK)) return;  // busy: next loop
  pDoc->clear();
  JsonObject root = pDoc->to<JsonObject>();
  serializeState(root, true);
  root[F("lor")] = realtimeOverride;
  File f = WLED_FS.open(LOOK_FILE, "w");
  if (f) {
    serializeJson(*pDoc, f);
    f.close();
  }
  releaseJSONBufferLock();
  lookDirty = false;
}

void restoreLook() {
  if (cfgRestore && WLED_FS.exists(LOOK_FILE)) {
    if (!requestJSONBufferLock(JSON_LOCK_WLEDLINK)) return;  // busy: next loop
    File f = WLED_FS.open(LOOK_FILE, "r");
    DeserializationError err = f ? deserializeJson(*pDoc, f) : DeserializationError(DeserializationError::InvalidInput);
    if (f) f.close();
    if (!err) {
      JsonObject root = pDoc->as<JsonObject>();
      root[F("tt")] = 0;
      deserializeState(root, CALL_MODE_NO_NOTIFY);
    }
    releaseJSONBufferLock();
  }
  lookRestored = true;
  lookDirty = false;  // the restore itself isn't a change to save
}

// ---------------------------------------------------------------------------------------------
// keeping in touch with the bridge

void maintain(uint32_t now) {
  if (!cfgHaveKey) return;
  if (settingsChanged) {
    settingsChanged = false;
    trial = false;
    runRadio(cfgMode);
  }
  if (wlink.up()) {
    lastUp = now;
    trial = false;
    if (runMode != cfgMode) runRadio(cfgMode);  // e.g. found the bridge during a trial: run what it announced
    return;
  }
  // trials: look for the bridge in the other mode now and then, in case it switched while we were away
  if (trial) {
    if (runMode == wll::MODE_WIFI && WLED_CONNECTED) return;  // joined the bridge's network: wait for its HELLO
    if ((int32_t)(now - trialUntil) > 0) {
      trial = false;
      runRadio(cfgMode);
    }
    return;
  }
  uint32_t quiet = now - (lastUp > runSince ? lastUp : runSince);
  // (never with hotspot settings that could open WLED's own access point during the trial)
  if (runMode == wll::MODE_ESPNOW && quiet > WIFI_TRIAL_AFTER_MS && WLED_WIFI_CONFIGURED &&
      apBehavior != AP_BEHAVIOR_NO_CONN && apBehavior != AP_BEHAVIOR_ALWAYS) {
    trial = true;
    trialUntil = now + WIFI_TRIAL_MS;
    runRadio(wll::MODE_WIFI);
    return;
  }
  if (runMode == wll::MODE_WIFI && !WLED_CONNECTED && quiet > ESPNOW_TRIAL_AFTER_MS) {
    trial = true;
    trialUntil = now + ESPNOW_TRIAL_MS;
    runRadio(wll::MODE_ESPNOW);
    return;
  }
  // ESP-NOW mode: sweep the other channels now and then, in case the bridge moved
  if (runMode != wll::MODE_ESPNOW || statusESPNow != ESP_NOW_STATE_ON) return;
  if (!hunting && !wlink.handshaking() && now - lastChannelCheck > CHANNEL_CHECK_MS) {
    lastChannelCheck = now;
    uint8_t ch = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&ch, &second) == ESP_OK && ch != cfgChannel) tuneTo(cfgChannel);
  }
  if (!hunting && quiet > HUNT_AFTER_MS && now - lastHunt > HUNT_EVERY_MS) {
    hunting = true;
    huntChannel = 0;
    huntAt = 0;
  }
  if (hunting && now - huntAt >= HUNT_DWELL_MS) {
    do huntChannel++; while (huntChannel == cfgChannel);
    if (huntChannel > 11) {  // done: back home
      hunting = false;
      lastHunt = now;
      tuneTo(cfgChannel);
      wlink.requestHello(now);
      return;
    }
    tuneTo(huntChannel);
    wlink.requestHello(now);
    huntAt = now;
  }
}

// Frames in, answers out: from loop(), and from WLED's reset() while it waits for its last answer (say, to a
// firmware update) to leave, since over ESP-NOW that answer goes out from here.
void pumpLink() {
  if (!cfgHaveKey) return;
  uint32_t now = millis();
  RxFrame f;
  while (rxq && xQueueReceive(rxq, &f, 0) == pdTRUE) wlink.onFrame(f.mac, f.data, f.len, now);
  // the bridge answered on a channel we were only visiting: stay, that is its channel now
  if (hunting && (wlink.handshaking() || wlink.up())) {
    hunting = false;
    lastHunt = now;
  }
  wlink.tick(now);
  if (wlink.up()) {
    pumpConns();
    if (stateDirty && now - lastStatePush >= 20) {
      lastStatePush = now;
      pushState();
    }
  }
}

}  // namespace

class WledLinkUsermod : public Usermod {
 public:
  void setup() override {
    rxq = xQueueCreate(RX_QUEUE_FRAMES, sizeof(RxFrame));
    wlink.send = radioSend;
    wlink.deliver = linkDeliver;
    wlink.onUp = linkUp;
    wlink.onDown = linkDown;
    wlink.onHello = linkHello;
    wlink.begin(false, LINK_QUEUE);
    if (cfgHaveKey) wlink.setKey(cfgKey);
    runSince = millis();
    radioLinkPump = pumpLink;
  }

  void loop() override {
    uint32_t now = millis();
    if (!lookRestored && now >= LOOK_RESTORE_AFTER_MS) restoreLook();
    if (lookDirty && now - lookChangedAt > LOOK_SAVE_DELAY_MS) saveLook();
    if (!cfgHaveKey) return;

    if (runMode == wll::MODE_ESPNOW && !localUp && statusESPNow == ESP_NOW_STATE_ON) startLocalInterfaces();
    pumpLink();
    maintain(now);
  }

  // Called by the ESP-NOW receive task: just queue our frames for loop().
  bool onEspNowMessage(uint8_t* sender, uint8_t* payload, uint8_t len) override {
    if (len < 2 || payload[0] != wll::MAGIC) return false;
    if (!rxq) return true;
    RxFrame f;
    rxFrames++;
    memcpy(f.mac, sender, 6);
    f.len = len;
    memcpy(f.data, payload, len);
    xQueueSend(rxq, &f, 0);
    return true;
  }

  void onStateChange(uint8_t mode) override {
    stateDirty = true;
    lookDirty = true;
    lookChangedAt = millis();
  }

  void addToJsonInfo(JsonObject& root) override {
    char kf[9] = "";
    if (cfgHaveKey) wll::fingerprint(cfgKey, kf);
    JsonObject w = root.createNestedObject(F("wll"));
    w[F("v")] = 1;
    w[F("mode")] = cfgMode == wll::MODE_ESPNOW ? "espnow" : "wifi";
    w[F("run")] = runMode == wll::MODE_ESPNOW ? "espnow" : "wifi";
    w[F("link")] = wlink.up();
    w[F("ch")] = cfgChannel;
    w[F("kf")] = kf;
    w[F("restore")] = cfgRestore;
    char st[96];
    wlink.statsText(st, sizeof st);
    w[F("stats")] = st;
    w[F("frames")] = rxFrames;
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray row = user.createNestedArray(F("WLED Link"));
    if (!cfgHaveKey) row.add(F("not paired with a bridge"));
    else if (runMode == wll::MODE_ESPNOW) row.add(wlink.up() ? F("ESP-NOW, linked") : F("ESP-NOW, looking for the bridge"));
    else row.add(wlink.up() ? F("Wi-Fi, linked") : F("Wi-Fi"));
  }

  // {"WLEDLink": {"key": "32 hex digits", "ch": 1-13, "mode": "espnow"|"wifi", "restore": true, "white": true}}
  void readFromJsonState(JsonObject& root) override {
    JsonObject w = root[F("WLEDLink")];
    if (w.isNull()) return;
    if (w[F("white")] | false) {  // after WLED applied the rest of the command: the selected segments go full white
      uint32_t c = fullWhite();
      strip.suspend();
      strip.waitForIt();
      for (size_t i = 0; i < strip.getSegmentsNum(); i++) {
        Segment& seg = strip.getSegment(i);
        if (seg.isActive() && seg.isSelected()) seg.setColor(0, c);
      }
      strip.resume();
      stateDirty = true;
    }
    bool changed = false;
    const char* k = w[F("key")];
    uint8_t key[16];
    if (k && hexToKey(k, key) && (!cfgHaveKey || memcmp(key, cfgKey, 16))) {
      memcpy(cfgKey, key, 16);
      cfgHaveKey = true;
      keyInNvs = storeKey(cfgKey);
      wlink.setKey(cfgKey);
      changed = true;
    }
    int ch = w[F("ch")] | 0;
    if (ch >= 1 && ch <= 13 && ch != cfgChannel) {
      cfgChannel = ch;
      changed = true;
    }
    const char* m = w[F("mode")];
    if (m) {
      uint8_t mode = strcmp(m, "espnow") == 0 ? wll::MODE_ESPNOW : wll::MODE_WIFI;
      if (mode != cfgMode) {
        cfgMode = mode;
        changed = true;
      }
    }
    if (!w[F("restore")].isNull()) {
      cfgRestore = w[F("restore")];
      changed = true;
    }
    if (changed) {
      configNeedsWrite = true;
      settingsChanged = true;
    }
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(F("WLEDLink"));
    top[F("mode")] = cfgMode == wll::MODE_ESPNOW ? "espnow" : "wifi";
    top[F("ch")] = cfgChannel;
    top[F("restore")] = cfgRestore;
    if (cfgHaveKey && !keyInNvs) {  // only if NVS refused it
      char hex[33];
      keyToHex(cfgKey, hex);
      top[F("key")] = hex;
    }
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[F("WLEDLink")];
    keyInNvs = loadKey(cfgKey);
    uint8_t old[16];
    if (!top.isNull() && hexToKey(top[F("key")] | "", old)) {  // earlier builds kept it in cfg.json: move it
      if (!keyInNvs || memcmp(old, cfgKey, 16)) {
        memcpy(cfgKey, old, 16);
        keyInNvs = storeKey(cfgKey);
      }
      cfgHaveKey = true;
      if (keyInNvs) configNeedsWrite = true;  // and cfg.json goes without it from now on
    } else {
      cfgHaveKey = keyInNvs;
    }
    if (top.isNull()) {  // WLED's settings were reset: the key survived, so the bridge can still be found
      applyKey();
      return false;
    }
    const char* m = top[F("mode")] | "wifi";
    cfgMode = strcmp(m, "espnow") == 0 ? wll::MODE_ESPNOW : wll::MODE_WIFI;
    int ch = top[F("ch")] | 1;
    cfgChannel = ch >= 1 && ch <= 13 ? ch : 1;
    cfgRestore = top[F("restore")] | true;
    applyKey();
    return !top[F("restore")].isNull();
  }

  void applyKey() {
    if (!cfgHaveKey) return;
    wlink.setKey(cfgKey);
    // at boot this runs before WLED sets up its radio, so the first setup is already the right one
    runMode = cfgMode;
    espNowOnlyChannel = cfgMode == wll::MODE_ESPNOW ? cfgChannel : 0;
    enableESPNow = true;
    settingsChanged = true;
  }

  uint16_t getId() override { return USERMOD_ID_WLEDLINK; }
};

static WledLinkUsermod wledlink;
REGISTER_USERMOD(wledlink);
