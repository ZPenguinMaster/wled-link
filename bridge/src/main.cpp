// WLED Link bridge
//
// Runs on a spare ESP32 that stays plugged into the PC. It hosts a small private Wi-Fi
// network that the WLED controller joins, and tunnels TCP and UDP between that network
// and the PC over the USB serial port. pc/wledlink.py turns the tunnel back into
// http://127.0.0.2 (+ UDP 21324) on the PC, which is what the browser and SignalRGB use.
//
// Serial framing, both directions: COBS-encoded frames, each terminated by 0x00.
//   decoded frame = type(1) seq(1) payload(n) crc16(2, LE)
//   crc16 = CRC-16/CCITT-FALSE over type..payload
// seq counts frames per direction. A gap means bytes were lost, so the session is reset
// (all tunnelled connections dropped) and the host re-syncs with HELLO.

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <errno.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <lwip/sockets.h>
#include "config.h"
#include "link.h"
#include "phone.h"
#include "wled.h"

#define FW_VERSION "1.5.0"
static const uint8_t PROTO_VERSION = 1;

enum MsgType : uint8_t {
  // host -> bridge
  H_HELLO = 0x01,           // proto(1) nonce(4 LE): start a fresh session, answered with INFO
  H_PING = 0x02,            // answered with PONG
  H_STA_REQ = 0x03,         // answered with STA_LIST
  H_SET_CONFIG = 0x04,      // ssidLen ssid passLen pass channel hidden txq [withPc]; a lone 0xFF restores defaults
  H_REBOOT = 0x05,
  H_STATS_REQ = 0x06,       // answered with STATS
  H_PHONE_PAIR = 0x07,      // seconds(2 LE): let a new phone pair over Bluetooth
  H_PHONE_FORGET = 0x08,    // forget all paired phones
  H_PHONE_PIN = 0x09,       // pin(4 LE), answered with CONFIG_RESULT
  H_WLED_CMD = 0x0B,        // JSON WLED state command, sent on to WLED over the bridge's WebSocket
  H_SET_BAUD = 0x0C,        // rate(4 LE): answered with BAUD, then the bridge switches speed
  H_SET_LINK = 0x0D,        // mode(1: 0 ESP-NOW, 1 Wi-Fi, 0xFF keep) [key(16)]: answered with CONFIG_RESULT
  H_LINK_KEY_REQ = 0x0E,    // answered with LINK_KEY
  H_TCP_OPEN = 0x10,        // conn(1) ip(4) port(2 BE)
  H_TCP_DATA = 0x11,        // conn(1) data
  H_TCP_CLOSE = 0x12,       // conn(1): flush what is pending, then close
  H_UDP_SEND = 0x20,        // ip(4) port(2 BE) data
  // bridge -> host
  B_INFO = 0x81,            // JSON
  B_PONG = 0x82,
  B_STA_LIST = 0x83,        // JSON
  B_CONFIG_RESULT = 0x84,   // JSON
  B_STATS = 0x86,           // JSON
  B_WLED_STATE = 0x88,      // JSON: the light's state, sent whenever it changes
  B_BAUD = 0x89,            // ok(1) rate(4 LE)
  B_LINK_KEY = 0x8A,        // key(16), or nothing if there is none yet
  // bridge -> WLED over the radio link only
  L_RESET = 0x42,           // the PC's session ended: close every connection
  L_MODE = 0x43,            // mode(1) channel(1): about to switch, follow now
  B_TCP_OPEN_RESULT = 0x90, // conn(1) status(1)
  B_TCP_DATA = 0x91,        // conn(1) data
  B_TCP_CLOSED = 0x92,      // conn(1) reason(1); only after this may the host reuse the slot
  B_TCP_ACK = 0x93,         // conn(1) bytes(2 LE): host data handed to the socket (returns window)
};

enum OpenStatus : uint8_t { OPEN_OK = 0, OPEN_BAD_SLOT = 1, OPEN_NO_SOCKET = 2, OPEN_REFUSED = 3, OPEN_TIMEOUT = 4 };
enum CloseReason : uint8_t { CLOSE_PEER = 0, CLOSE_ERROR = 1, CLOSE_HOST = 2, CLOSE_OVERFLOW = 3 };

static const size_t MAX_PAYLOAD = 1600;
static const size_t MAX_FRAME = MAX_PAYLOAD + 4;                   // type + seq + payload + crc
static const size_t MAX_ENCODED = MAX_FRAME + MAX_FRAME / 254 + 2;
static const int MAX_CONNS = 6;            // browsers use at most 6 per site; each can hold ~6 KB of lwIP buffers
static const size_t CONN_WINDOW = 4096;    // unacknowledged host->WLED bytes allowed per connection
static const size_t LINK_WINDOW = 2048;    // the same, in ESP-NOW mode (WLED's usermod holds it)
static const char* LINK_WLED_IP = "192.168.77.2";  // how WLED shows up to the PC in ESP-NOW mode
static const size_t TCP_CHUNK = 1024;      // max WLED->host bytes per frame
static const uint32_t HOST_TIMEOUT_MS = 3000;
static const uint32_t CONNECT_TIMEOUT_MS = 4000;
static const uint32_t CLOSE_TIMEOUT_MS = 5000;
static const uint32_t BEACON_INTERVAL_MS = 500;
static const uint32_t AP_IDLE_OFF_MS = 60000;  // Wi-Fi goes quiet this long after the PC program stops talking

// ---------------------------------------------------------------------------------------------
// settings

struct Settings {
  char ssid[33];
  char pass[64];
  uint8_t channel;  // 0 = auto
  uint8_t hidden;
  uint8_t txq;      // max TX power in 0.25 dBm units
  uint8_t withPc;   // 1 = Wi-Fi only while the PC program is talking to us
};

static Settings settings;
static uint8_t apChannel = 0;
static bool apOn = false;

static void loadSettings() {
  Preferences p;
  bool ok = p.begin("wledlink", true);  // fails on first boot: namespace does not exist yet
  String ssid = ok ? p.getString("ssid", WL_DEFAULT_SSID) : String(WL_DEFAULT_SSID);
  String pass = ok ? p.getString("pass", WL_DEFAULT_PASS) : String(WL_DEFAULT_PASS);
  settings.channel = ok ? p.getUChar("chan", WL_DEFAULT_CHANNEL) : WL_DEFAULT_CHANNEL;
  settings.hidden = ok ? p.getUChar("hidden", WL_DEFAULT_HIDDEN) : WL_DEFAULT_HIDDEN;
  settings.txq = ok ? p.getUChar("txq", WL_DEFAULT_TXPOWER_QDBM) : WL_DEFAULT_TXPOWER_QDBM;
  settings.withPc = ok ? p.getUChar("withpc", WL_DEFAULT_WIFI_WITH_PC) : WL_DEFAULT_WIFI_WITH_PC;
  if (ok) p.end();
  strlcpy(settings.ssid, ssid.c_str(), sizeof settings.ssid);
  strlcpy(settings.pass, pass.c_str(), sizeof settings.pass);
  if (!settings.pass[0]) {  // none set (config.h leaves it empty on purpose): make one up, once, and keep it
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // nothing that reads as something else
    uint8_t r[12];
    randomBytes(r, sizeof r);
    char* o = settings.pass;
    for (int i = 0; i < 12; i++) {
      if (i && i % 4 == 0) *o++ = '-';
      *o++ = alphabet[r[i] % (sizeof alphabet - 1)];
    }
    *o = 0;
    Preferences w;
    if (w.begin("wledlink", false)) {
      w.putString("pass", settings.pass);
      w.end();
    }
  }
}

static bool saveSettings(const Settings& s) {
  Preferences p;
  if (!p.begin("wledlink", false)) return false;
  bool ok = p.putString("ssid", s.ssid) && p.putString("pass", s.pass) &&
            p.putUChar("chan", s.channel) && p.putUChar("hidden", s.hidden) && p.putUChar("txq", s.txq) &&
            p.putUChar("withpc", s.withPc);
  p.end();
  return ok;
}

static void clearSettings() {
  Preferences p;
  if (p.begin("wledlink", false)) {
    p.clear();
    p.end();
  }
}

// ---------------------------------------------------------------------------------------------
// framing

static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static size_t cobsEncode(const uint8_t* in, size_t len, uint8_t* out) {
  size_t read = 0, write = 1, codeIdx = 0;
  uint8_t code = 1;
  while (read < len) {
    if (in[read] == 0) {
      out[codeIdx] = code;
      code = 1;
      codeIdx = write++;
      read++;
    } else {
      out[write++] = in[read++];
      if (++code == 0xFF) {
        out[codeIdx] = code;
        code = 1;
        codeIdx = write++;
      }
    }
  }
  out[codeIdx] = code;
  return write;
}

static bool cobsDecode(const uint8_t* in, size_t len, uint8_t* out, size_t outMax, size_t* outLen) {
  size_t r = 0, w = 0;
  while (r < len) {
    uint8_t code = in[r++];
    if (code == 0) return false;
    for (uint8_t i = 1; i < code; i++) {
      if (r >= len || w >= outMax) return false;
      out[w++] = in[r++];
    }
    if (code < 0xFF && r < len) {
      if (w >= outMax) return false;
      out[w++] = 0;
    }
  }
  *outLen = w;
  return true;
}

static uint8_t txSeq = 0;
static uint8_t txFrame[MAX_FRAME];
static uint8_t txEncoded[MAX_ENCODED + 1];

// Worst-case bytes on the wire for a frame carrying `payload` bytes.
static size_t wireSize(size_t payload) {
  size_t n = payload + 4;
  return n + n / 254 + 2;
}

static void sendFrame(uint8_t type, const void* a, size_t alen, const void* b = nullptr, size_t blen = 0) {
  if (alen + blen > MAX_PAYLOAD) return;
  size_t n = 0;
  txFrame[n++] = type;
  txFrame[n++] = txSeq++;
  if (alen) memcpy(txFrame + n, a, alen), n += alen;
  if (blen) memcpy(txFrame + n, b, blen), n += blen;
  uint16_t c = crc16(txFrame, n);
  txFrame[n++] = c & 0xFF;
  txFrame[n++] = c >> 8;
  size_t e = cobsEncode(txFrame, n, txEncoded);
  txEncoded[e++] = 0;
  Serial.write(txEncoded, e);
}

// ---------------------------------------------------------------------------------------------
// state

struct Stats {
  uint32_t udpTx, udpDrop, rxBad, rxGaps, tcpOpened;
};

enum ConnState : uint8_t { C_FREE, C_CONNECTING, C_OPEN, C_CLOSING };

struct Conn {
  ConnState state;
  int sock;
  uint32_t since;
  uint8_t* pending;  // host->WLED bytes the socket has not accepted yet (<= CONN_WINDOW)
  size_t pendingLen;
  size_t ackOwed;
};

static Conn conns[MAX_CONNS];
static int udpSock = -1;
static Stats stats;
static uint32_t bootId = 0;
static uint32_t helloNonce = 0;
static bool hostActive = false;
static int rxExpectSeq = -1;
static uint32_t lastHostRxMs = 0;
static uint32_t lastBeaconMs = 0;
static uint32_t lastActivityMs = 0;
static uint32_t rebootAtMs = 0;
static volatile bool staDirty = true;
static int staCount = 0;
static uint32_t staCountAtMs = 0;

static uint8_t rxEncoded[MAX_ENCODED];
static size_t rxEncodedLen = 0;
static bool rxOverflow = false;
static uint8_t rxFrame[MAX_FRAME];
static uint8_t sockBuf[TCP_CHUNK];

static void apStart();  // Wi-Fi section below
static bool espnow() { return linkMode() == LINK_ESPNOW; }
static uint8_t relayed = 0;  // ESP-NOW mode: connections the PC has open through WLED's usermod (bit per slot)

static uint32_t serialBaud = WL_SERIAL_BAUD;
static uint32_t baudTrialUntil = 0;  // a new speed is kept only if the host is heard at it by then
static bool stateDirty = true;       // send the light's state to the host

static void setBaud(uint32_t rate) {
  Serial.flush();
  Serial.updateBaudRate(rate);
  serialBaud = rate;
}

// ---------------------------------------------------------------------------------------------
// messages to the host

static size_t jsonEscape(char* out, size_t cap, const char* s) {
  size_t n = 0;
  for (; *s && n + 2 < cap; s++) {
    if (*s == '"' || *s == '\\') out[n++] = '\\';
    if ((uint8_t)*s >= 0x20) out[n++] = *s;
  }
  out[n] = 0;
  return n;
}

// Counters shared by INFO and STATS (no braces, so it can be spliced into either object).
static int formatStats(char* out, size_t cap) {
  char lk[96];
  linkStatsText(lk, sizeof lk);
  return snprintf(out, cap,
      "\"uptime\":%lu,\"heap\":%lu,\"minHeap\":%lu,\"sta\":%d,\"udpTx\":%lu,\"udpDrop\":%lu,\"rxBad\":%lu,\"rxGaps\":%lu,"
      "\"tcpOpened\":%lu,\"phones\":%d,\"phonePin\":\"%06lu\",\"pairing\":%d,\"linkUp\":%d,\"lkSent\":%lu,\"lkLost\":%lu,\"lkStalls\":%lu,\"lk\":\"%s\"",
      (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
      staCount, (unsigned long)stats.udpTx,
      (unsigned long)stats.udpDrop, (unsigned long)stats.rxBad, (unsigned long)stats.rxGaps,
      (unsigned long)stats.tcpOpened, phoneCount(), (unsigned long)phonePin(), phonePairingLeft(), linkUp() ? 1 : 0,
      (unsigned long)linkFramesSent(), (unsigned long)linkFramesLost(), (unsigned long)linkStalls(), lk);
}

static void sendInfo() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);  // works while the Wi-Fi driver is off
  char ssid[70], pass[130], counters[480], buf[1152], lkf[9];
  jsonEscape(ssid, sizeof ssid, settings.ssid);
  jsonEscape(pass, sizeof pass, settings.pass);
  formatStats(counters, sizeof counters);
  linkFingerprint(lkf);
  int n = snprintf(buf, sizeof buf,
      "{\"proto\":%u,\"fw\":\"%s\",\"boot\":\"%08lx\",\"nonce\":%lu,\"host\":%d,"
      "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"wifi\":%d,\"ssid\":\"%s\",\"pass\":\"%s\","
      "\"ch\":%u,\"chCfg\":%u,\"hidden\":%u,\"txq\":%u,\"withPc\":%u,\"caps\":\"ws,baud,phone,espnow\",\"apIp\":\"%s\",\"maxConns\":%d,\"win\":%u,"
      "\"link\":\"%s\",\"linkSet\":%d,\"lkf\":\"%s\",%s}",
      PROTO_VERSION, FW_VERSION, (unsigned long)bootId, (unsigned long)helloNonce, hostActive ? 1 : 0,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], apOn ? 1 : 0, ssid, pass,
      apChannel, settings.channel, settings.hidden, settings.txq, settings.withPc, IPAddress(WL_AP_IP).toString().c_str(),
      MAX_CONNS, (unsigned)(espnow() ? LINK_WINDOW : CONN_WINDOW), espnow() ? "espnow" : "wifi", linkModeChosen() ? 1 : 0, lkf,
      counters);
  if (n > 0 && (size_t)n < sizeof buf) sendFrame(B_INFO, buf, n);
  lastBeaconMs = millis();
}

static void sendStats() {
  char buf[500];
  buf[0] = '{';
  int n = 1 + formatStats(buf + 1, sizeof buf - 2);
  if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '}';
  sendFrame(B_STATS, buf, n);
}

static void sendStaList() {
  char buf[96 * WL_MAX_STATIONS + 32];
  uint8_t m[6];
  if (espnow()) {
    bool up = linkUp() && linkPeerMac(m);
    int n = up ? snprintf(buf, sizeof buf, "{\"sta\":[{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\",\"rssi\":0,\"link\":\"espnow\"}]}",
                          m[0], m[1], m[2], m[3], m[4], m[5], LINK_WLED_IP)
               : snprintf(buf, sizeof buf, "{\"sta\":[]}");
    staCount = up ? 1 : 0;
    staCountAtMs = millis();
    sendFrame(B_STA_LIST, buf, n);
    return;
  }
  int n = snprintf(buf, sizeof buf, "{\"sta\":[");
  wifi_sta_list_t wifiList;
  esp_netif_sta_list_t ipList;
  int count = 0;
  if (esp_wifi_ap_get_sta_list(&wifiList) == ESP_OK && esp_netif_get_sta_list(&wifiList, &ipList) == ESP_OK) {
    for (int i = 0; i < ipList.num && i < WL_MAX_STATIONS; i++) {
      const uint8_t* m = ipList.sta[i].mac;
      n += snprintf(buf + n, sizeof buf - n,
          "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"" IPSTR "\",\"rssi\":%d}",
          count ? "," : "", m[0], m[1], m[2], m[3], m[4], m[5], IP2STR(&ipList.sta[i].ip), wifiList.sta[i].rssi);
      count++;
    }
  }
  n += snprintf(buf + n, sizeof buf - n, "]}");
  staCount = count;
  staCountAtMs = millis();
  sendFrame(B_STA_LIST, buf, n);
}

static void sendConfigResult(bool ok, const char* msg) {
  char buf[160];
  int n = snprintf(buf, sizeof buf, "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
  sendFrame(B_CONFIG_RESULT, buf, n);
}

static void sendOpenResult(uint8_t cid, uint8_t status) {
  uint8_t p[2] = {cid, status};
  sendFrame(B_TCP_OPEN_RESULT, p, 2);
}

// ---------------------------------------------------------------------------------------------
// tunnelled connections

static void connFree(int cid) {
  Conn& c = conns[cid];
  if (c.sock >= 0) lwip_close(c.sock);
  c.sock = -1;
  c.state = C_FREE;
  c.pendingLen = 0;
  c.ackOwed = 0;
}

static void connClosed(int cid, uint8_t reason) {
  connFree(cid);
  uint8_t p[2] = {(uint8_t)cid, reason};
  sendFrame(B_TCP_CLOSED, p, 2);
}

static void closeAllConns() {
  for (int i = 0; i < MAX_CONNS; i++)
    if (conns[i].state != C_FREE) connFree(i);
}

static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }

static void handleTcpOpen(const uint8_t* p, size_t n) {
  if (n < 7) return;
  uint8_t cid = p[0];
  if (cid >= MAX_CONNS || !conns[cid].pending) {
    sendOpenResult(cid, OPEN_BAD_SLOT);
    return;
  }
  // The host only reuses a slot after it saw TCP_CLOSED, so a busy slot here is a leftover.
  if (conns[cid].state != C_FREE) connFree(cid);

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, p + 1, 4);
  addr.sin_port = htons((uint16_t)((p[5] << 8) | p[6]));

  int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) {
    sendOpenResult(cid, OPEN_NO_SOCKET);
    return;
  }
  lwip_fcntl(s, F_SETFL, lwip_fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  int one = 1;
  lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int r = lwip_connect(s, (sockaddr*)&addr, sizeof addr);
  if (r != 0 && errno != EINPROGRESS) {
    lwip_close(s);
    sendOpenResult(cid, OPEN_REFUSED);
    return;
  }
  Conn& c = conns[cid];
  c.sock = s;
  c.state = r == 0 ? C_OPEN : C_CONNECTING;
  c.since = millis();
  c.pendingLen = 0;
  c.ackOwed = 0;
  stats.tcpOpened++;
  if (r == 0) sendOpenResult(cid, OPEN_OK);
}

static void handleTcpData(const uint8_t* p, size_t n) {
  if (n < 1 || p[0] >= MAX_CONNS) return;
  uint8_t cid = p[0];
  Conn& c = conns[cid];
  if (c.state == C_FREE || c.state == C_CLOSING) return;  // stale data for a connection that is gone
  const uint8_t* d = p + 1;
  size_t len = n - 1;
  if (c.state == C_OPEN && c.pendingLen == 0) {
    int k = lwip_send(c.sock, d, len, MSG_DONTWAIT);
    if (k < 0) {
      if (!wouldBlock()) {
        connClosed(cid, CLOSE_ERROR);
        return;
      }
      k = 0;
    }
    c.ackOwed += k;
    d += k;
    len -= k;
  }
  if (len == 0) return;
  if (c.pendingLen + len > CONN_WINDOW) {  // the host ignored the window
    connClosed(cid, CLOSE_OVERFLOW);
    return;
  }
  memcpy(c.pending + c.pendingLen, d, len);
  c.pendingLen += len;
}

static void handleTcpClose(const uint8_t* p, size_t n) {
  if (n < 1 || p[0] >= MAX_CONNS) return;
  uint8_t cid = p[0];
  Conn& c = conns[cid];
  if (c.state == C_FREE || c.state == C_CLOSING) return;
  if (c.state == C_CONNECTING || c.pendingLen == 0) {
    connClosed(cid, CLOSE_HOST);
    return;
  }
  c.state = C_CLOSING;  // finish sending what the host already gave us
  c.since = millis();
}

static void handleUdpSend(const uint8_t* p, size_t n) {
  if (n < 6) return;
  if (espnow()) {
    if (linkSendDatagram(H_UDP_SEND, p, n)) stats.udpTx++;
    else stats.udpDrop++;
    lastActivityMs = millis();
    if (n > 6 && !((p[4] << 8 | p[5]) == 21324 && p[6] == 0)) wledNoteStream();
    return;
  }
  if (udpSock < 0) return;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, p, 4);
  addr.sin_port = htons((uint16_t)((p[4] << 8) | p[5]));
  if (lwip_sendto(udpSock, p + 6, n - 6, MSG_DONTWAIT, (sockaddr*)&addr, sizeof addr) < 0)
    stats.udpDrop++;
  else
    stats.udpTx++;
  lastActivityMs = millis();
  if (n > 6 && !(ntohs(addr.sin_port) == 21324 && p[6] == 0)) wledNoteStream();  // LED data, not a WLED sync packet
}

// ESP-NOW mode: the PC's connections and UDP go to WLED's usermod over the radio link, which serves
// them from WLED itself; the answers come back through onLinkMessage().
static void relayClosed(uint8_t cid, uint8_t reason) {
  relayed &= ~(1 << cid);
  uint8_t p[2] = {cid, reason};
  sendFrame(B_TCP_CLOSED, p, 2);
}

static void relayCloseAll(uint8_t reason) {
  for (uint8_t cid = 0; cid < MAX_CONNS; cid++)
    if (relayed & (1 << cid)) relayClosed(cid, reason);
}

static void relayOpen(const uint8_t* p, size_t n) {
  if (n < 7) return;
  uint8_t cid = p[0];
  if (cid >= MAX_CONNS) return sendOpenResult(cid, OPEN_BAD_SLOT);
  if (!linkUp()) return sendOpenResult(cid, OPEN_REFUSED);
  if (!linkSendMsg(H_TCP_OPEN, p, n)) return sendOpenResult(cid, OPEN_NO_SOCKET);
  relayed |= 1 << cid;
  stats.tcpOpened++;
}

static void relayConn(uint8_t type, const uint8_t* p, size_t n) {
  if (n < 1 || p[0] >= MAX_CONNS || !(relayed & (1 << p[0]))) return;  // stale: that connection is gone
  if (!linkSendMsg(type, p, n)) relayClosed(p[0], CLOSE_OVERFLOW);
}

bool onLinkMessage(const uint8_t* m, size_t n) {
  if (n < 1) return true;
  switch (m[0]) {
    case B_TCP_OPEN_RESULT:
    case B_TCP_DATA:
    case B_TCP_CLOSED:
    case B_TCP_ACK: {
      if (!hostActive || n < 2 || m[1] >= MAX_CONNS || !(relayed & (1 << m[1]))) return true;  // nobody wants it
      if (Serial.availableForWrite() < (int)wireSize(n)) return false;  // USB is busy: WLED sends it again soon
      if (m[0] == B_TCP_CLOSED || (m[0] == B_TCP_OPEN_RESULT && n >= 3 && m[2] != OPEN_OK)) relayed &= ~(1 << m[1]);
      sendFrame(m[0], m + 1, n - 1);
      lastActivityMs = millis();
      return true;
    }
    case B_WLED_STATE:
      wledLinkState((const char*)m + 1, n - 1);
      return true;
    default:
      return true;
  }
}

static uint32_t radioRestartDue = 0, lastRadioRestart = 0;
void onLinkStall() {
  if (!radioRestartDue && (!lastRadioRestart || millis() - lastRadioRestart > 10000)) radioRestartDue = millis() | 1;
}

void onLinkChange(bool up) {
  wledLinkUp(up);
  staDirty = true;
  if (!up && hostActive) relayCloseAll(CLOSE_ERROR);
  if (!up) relayed = 0;
}

// Moves data between the sockets and the serial link. Returns true if anything happened.
static bool pumpConns() {
  bool busy = false;
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
          sendOpenResult(i, OPEN_REFUSED);
          continue;
        }
        c.state = C_OPEN;
        sendOpenResult(i, OPEN_OK);
        busy = true;
      } else {
        if (millis() - c.since > CONNECT_TIMEOUT_MS) {
          connFree(i);
          sendOpenResult(i, OPEN_TIMEOUT);
        }
        continue;
      }
    }

    // host -> WLED: flush what the socket could not take earlier
    if (c.pendingLen > 0) {
      int k = lwip_send(c.sock, c.pending, c.pendingLen, MSG_DONTWAIT);
      if (k > 0) {
        memmove(c.pending, c.pending + k, c.pendingLen - k);
        c.pendingLen -= k;
        c.ackOwed += k;
        busy = true;
      } else if (k < 0 && !wouldBlock()) {
        connClosed(i, CLOSE_ERROR);
        continue;
      }
    }
    if (c.ackOwed > 0) {
      uint8_t ack[3] = {(uint8_t)i, (uint8_t)(c.ackOwed & 0xFF), (uint8_t)(c.ackOwed >> 8)};
      sendFrame(B_TCP_ACK, ack, 3);
      c.ackOwed = 0;
    }

    if (c.state == C_CLOSING) {
      if (c.pendingLen == 0) connClosed(i, CLOSE_HOST);
      else if (millis() - c.since > CLOSE_TIMEOUT_MS) connClosed(i, CLOSE_ERROR);
      continue;
    }

    // WLED -> host, only as fast as the serial link drains
    int room = Serial.availableForWrite();
    if (room < (int)wireSize(64)) continue;
    size_t want = min(TCP_CHUNK, (size_t)room - wireSize(1));
    int k = lwip_recv(c.sock, sockBuf, want, MSG_DONTWAIT);
    if (k > 0) {
      uint8_t cid = i;
      sendFrame(B_TCP_DATA, &cid, 1, sockBuf, k);
      lastActivityMs = millis();
      busy = true;
    } else if (k == 0) {
      connClosed(i, CLOSE_PEER);
    } else if (!wouldBlock()) {
      connClosed(i, CLOSE_ERROR);
    }
  }
  return busy;
}

// ---------------------------------------------------------------------------------------------
// session

static void endSession() {
  hostActive = false;
  helloNonce = 0;
  rxExpectSeq = -1;
  closeAllConns();
  if (relayed) linkSendMsg(L_RESET, nullptr, 0);
  relayed = 0;
  if (serialBaud != WL_SERIAL_BAUD) setBaud(WL_SERIAL_BAUD);  // beacons always go out at the default speed
}

static void handleSetConfig(const uint8_t* p, size_t n) {
  if (n == 1 && p[0] == 0xFF) {
    clearSettings();
    sendConfigResult(true, "defaults restored, rebooting");
    rebootAtMs = millis() + 300;
    return;
  }
  Settings s = settings;
  size_t i = 0;
  size_t ssidLen = i < n ? p[i++] : 0;
  if (ssidLen < 1 || ssidLen > 32 || i + ssidLen > n) return sendConfigResult(false, "SSID must be 1-32 characters");
  memcpy(s.ssid, p + i, ssidLen);
  s.ssid[ssidLen] = 0;
  i += ssidLen;
  size_t passLen = i < n ? p[i++] : 0;
  if (passLen < 8 || passLen > 63 || i + passLen > n) return sendConfigResult(false, "password must be 8-63 characters");
  memcpy(s.pass, p + i, passLen);
  s.pass[passLen] = 0;
  i += passLen;
  if (i + 3 > n) return sendConfigResult(false, "truncated settings");
  s.channel = p[i++];
  s.hidden = p[i++] ? 1 : 0;
  s.txq = p[i++];
  if (i < n) s.withPc = p[i++] ? 1 : 0;  // optional, added in 1.2
  for (const char* c = s.ssid; *c; c++)
    if (*c < 0x20 || *c > 0x7E) return sendConfigResult(false, "SSID must be plain ASCII");
  for (const char* c = s.pass; *c; c++)
    if (*c < 0x20 || *c > 0x7E) return sendConfigResult(false, "password must be plain ASCII");
  if (s.channel > 11) return sendConfigResult(false, "channel must be 0 (auto) or 1-11");
  if (s.txq < 8 || s.txq > 78) return sendConfigResult(false, "TX power must be 2-19.5 dBm");
  if (!saveSettings(s)) return sendConfigResult(false, "could not save settings");
  if (s.channel == 0) linkForgetChannel();  // "auto": pick the quietest channel again at the next start
  sendConfigResult(true, "saved, rebooting");
  rebootAtMs = millis() + 300;
}

static void handleSetLink(const uint8_t* p, size_t n) {
  if (n < 1) return sendConfigResult(false, "empty request");
  if (n >= 17 && !linkSetKey(p + 1)) return sendConfigResult(false, "could not save the key");
  uint8_t m = p[0];
  if (m == 0xFF) return sendConfigResult(true, n >= 17 ? "key saved" : "no change");
  if (m > LINK_WIFI) return sendConfigResult(false, "unknown link mode");
  if (m == linkMode()) {
    linkSetMode((LinkMode)m);
    return sendConfigResult(true, "saved");
  }
  if (!linkSetMode((LinkMode)m)) return sendConfigResult(false, "could not save the link mode");
  if (linkUp()) {  // WLED switches with us, rather than having to look for us afterwards
    uint8_t mm[2] = {m, apChannel};
    linkSendMsg(L_MODE, mm, 2);
  }
  sendConfigResult(true, "switching, restarting");
  rebootAtMs = millis() + 600;  // time for L_MODE to reach WLED
}

static void handleHello(uint8_t seq, const uint8_t* p, size_t n) {
  closeAllConns();  // the host starts with no connections, so neither do we
  if (relayed) linkSendMsg(L_RESET, nullptr, 0);
  relayed = 0;
  if (!espnow()) apStart();
  hostActive = true;
  rxExpectSeq = (seq + 1) & 0xFF;
  helloNonce = n >= 5 ? ((uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24) : 0;
  sendInfo();
  staDirty = true;
  stateDirty = true;
}

static void handleFrame(const uint8_t* enc, size_t encLen) {
  size_t n;
  if (!cobsDecode(enc, encLen, rxFrame, sizeof rxFrame, &n) || n < 4) {
    stats.rxBad++;
    return;
  }
  uint16_t want = rxFrame[n - 2] | (rxFrame[n - 1] << 8);
  if (crc16(rxFrame, n - 2) != want) {
    stats.rxBad++;
    return;
  }
  uint8_t type = rxFrame[0], seq = rxFrame[1];
  const uint8_t* p = rxFrame + 2;
  size_t len = n - 4;
  lastHostRxMs = millis();
  baudTrialUntil = 0;  // a good frame: the current speed works

  if (type == H_HELLO) return handleHello(seq, p, len);
  if (!hostActive) {
    // Not in a session (we rebooted, or timed the host out): prompt it to send HELLO.
    if (millis() - lastBeaconMs > 200) sendInfo();
    return;
  }
  if (seq != rxExpectSeq) {
    stats.rxGaps++;
    endSession();
    sendInfo();  // nonce 0 tells the host its session is gone
    return;
  }
  rxExpectSeq = (seq + 1) & 0xFF;

  switch (type) {
    case H_PING: sendFrame(B_PONG, nullptr, 0); break;
    case H_STA_REQ: sendStaList(); break;
    case H_SET_CONFIG: handleSetConfig(p, len); break;
    case H_REBOOT: rebootAtMs = millis() + 100; break;
    case H_STATS_REQ: sendStats(); break;
    case H_PHONE_PAIR: phoneOpenPairing(len >= 2 ? (p[0] | p[1] << 8) : 120); sendStats(); break;
    case H_PHONE_FORGET: phoneForget(); sendStats(); break;
    case H_PHONE_PIN: {
      uint32_t pin = len >= 4 ? ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) : 0;
      bool ok = phoneSetPin(pin);
      sendConfigResult(ok, ok ? "PIN changed; pair your phone again" : "PIN must be 6 digits (not 123456)");
      break;
    }
    case H_WLED_CMD: wledSend((const char*)p, len); break;
    case H_SET_BAUD: {
      uint32_t rate = len >= 4 ? ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) : 0;
      bool ok = rate == 921600 || rate == 1000000 || rate == 1500000 || rate == 2000000 || rate == 3000000;
      uint8_t reply[5] = {(uint8_t)ok, p[0], p[1], p[2], p[3]};
      sendFrame(B_BAUD, reply, len >= 4 ? 5 : 1);
      if (ok && rate != serialBaud) {
        setBaud(rate);
        baudTrialUntil = millis() + 1500;
      }
      break;
    }
    case H_SET_LINK: handleSetLink(p, len); break;
    case H_LINK_KEY_REQ: sendFrame(B_LINK_KEY, linkKey(), linkKey() ? 16 : 0); break;
    case H_TCP_OPEN: espnow() ? relayOpen(p, len) : handleTcpOpen(p, len); break;
    case H_TCP_DATA: espnow() ? relayConn(H_TCP_DATA, p, len) : handleTcpData(p, len); break;
    case H_TCP_CLOSE: espnow() ? relayConn(H_TCP_CLOSE, p, len) : handleTcpClose(p, len); break;
    case H_UDP_SEND: handleUdpSend(p, len); break;
    default: break;
  }
}

static bool pumpSerial() {
  bool busy = false;
  uint8_t buf[256];
  int budget = 4096;  // leave time for the sockets between bursts
  while (budget > 0) {
    int avail = Serial.available();
    if (avail <= 0) break;
    int n = Serial.read(buf, min(avail, (int)sizeof buf));
    if (n <= 0) break;
    budget -= n;
    busy = true;
    for (int i = 0; i < n; i++) {
      uint8_t b = buf[i];
      if (b == 0) {
        if (!rxOverflow && rxEncodedLen > 0) handleFrame(rxEncoded, rxEncodedLen);
        rxEncodedLen = 0;
        rxOverflow = false;
      } else if (rxEncodedLen < sizeof rxEncoded) {
        rxEncoded[rxEncodedLen++] = b;
      } else {
        rxOverflow = true;
      }
    }
  }
  return busy;
}

// ---------------------------------------------------------------------------------------------
// Wi-Fi

static void setCountry() {
  wifi_country_t country = {};
  strlcpy(country.cc, WL_COUNTRY, sizeof country.cc);
  country.schan = 1;
  country.nchan = 11;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);
}

// Scores channels 1/6/11 by how many (and how loud) neighbouring networks overlap them.
static uint8_t pickChannel() {
  WiFi.mode(WIFI_STA);
  setCountry();
  WiFi.disconnect();
  int found = WiFi.scanNetworks(false, true, false, 100);  // active scan, 100 ms a channel: ~1.1 s
  const uint8_t candidates[3] = {1, 6, 11};
  uint32_t score[3] = {0, 0, 0};
  for (int i = 0; i < found; i++) {
    int rssi = WiFi.RSSI(i);
    uint32_t weight = rssi > -95 ? (uint32_t)(rssi + 96) : 1;
    weight *= weight;
    for (int k = 0; k < 3; k++) {
      int d = abs(WiFi.channel(i) - candidates[k]);
      if (d < 5) score[k] += weight * (5 - d);
    }
  }
  WiFi.scanDelete();
  int best = 0;
  for (int k = 1; k < 3; k++)
    if (score[k] < score[best]) best = k;
  return candidates[best];
}

// "Auto" picks the quietest channel once and keeps it (bridge-config --channel 0 picks again). WLED looks
// for the bridge on the channel it last saw it on, so a steady channel lets it reconnect at once after a
// restart or a power cut, and skipping the scan brings the radio back about a second sooner.
static void setupWifi() {
  WiFi.persistent(false);
  if (settings.channel) {
    apChannel = settings.channel;
  } else if (linkSavedChannel() >= 1 && linkSavedChannel() <= 11) {
    apChannel = linkSavedChannel();
  } else {
    apChannel = pickChannel();
    linkSaveChannel(apChannel);
  }
  WiFi.mode(WIFI_OFF);  // nothing on the air until the PC program connects
  // a device coming or going may be WLED restarting, which would leave the bridge's WebSocket dead
  auto onStation = [](arduino_event_id_t, arduino_event_info_t) { staDirty = true; wledCheckLink(); };
  WiFi.onEvent(onStation, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent(onStation, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent(onStation, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
}

// By default the access point runs whenever the bridge has power, so a phone can join it and reach
// WLED directly while the PC is off. With "withPc" set it only runs while the PC program is talking to
// the bridge; WLED keeps its last look meanwhile and reconnects on its own once the network is back.
static void apStart() {
  if (apOn) return;
  WiFi.mode(WIFI_AP);
  setCountry();
  WiFi.softAPConfig(IPAddress(WL_AP_IP), IPAddress(WL_AP_IP), IPAddress(255, 255, 255, 0));
  apOn = WiFi.softAP(settings.ssid, settings.pass, apChannel, settings.hidden, WL_MAX_STATIONS);
  wifi_config_t ap;
  if (apOn && esp_wifi_get_config(WIFI_IF_AP, &ap) == ESP_OK) {  // Arduino has no setter for the beacon interval
    ap.ap.beacon_interval = WL_BEACON_INTERVAL_TU;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
  }
  esp_wifi_set_max_tx_power((int8_t)settings.txq);
  if (apOn) linkStart(WIFI_IF_AP, apChannel, LINK_WIFI);  // alongside the network, for switching modes
  staDirty = true;
}

static void apStop() {
  if (!apOn) return;
  linkStop();
  WiFi.softAPdisconnect(true);  // also switches the radio off
  apOn = false;
  staCount = 0;
}

// ESP-NOW mode: the radio is on, on our channel, but there is no network: no access point, no beacons.
static void radioStart() {
  WiFi.mode(WIFI_STA);
  setCountry();
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);  // needed for set_channel while not connected to anything
  esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_max_tx_power((int8_t)settings.txq);
  linkStart(WIFI_IF_STA, apChannel, LINK_ESPNOW);
  staDirty = true;
}

// Starts the radio over, as at power-on: the link, and the network in Wi-Fi mode.
static void radioRestart() {
  lastRadioRestart = millis();
  if (espnow()) {
    linkStop();
    WiFi.mode(WIFI_OFF);
    radioStart();
  } else if (apOn) {
    apStop();
    apStart();
  }
}

// ---------------------------------------------------------------------------------------------

// Restarts cleanly. Left to esp_restart(), taking Bluetooth and Wi-Fi down while colour data is flowing
// can hang until the 5 s watchdog fires; stopped here in order it takes a few milliseconds.
static void restartNow() {
  Serial.flush();
  if (udpSock >= 0) lwip_close(udpSock);
  udpSock = -1;
  closeAllConns();
  phoneEnd();
  linkStop();
  WiFi.mode(WIFI_OFF);
  ESP.restart();
}

static void updateLed() {
  if (WL_STATUS_LED_PIN < 0) return;
  uint32_t t = millis();
  bool on;
  if (phonePairingLeft() > 0) on = (t % 200) < 100;  // fast blink: a phone may pair now
  else if (!hostActive) on = (t % 2000) < 80;        // blip: waiting for the PC program
  else if (staCount == 0) on = (t % 500) < 250;       // blinking: PC connected, WLED not joined yet
  else on = t - lastActivityMs > 40;                  // solid, flickers with traffic
  digitalWrite(WL_STATUS_LED_PIN, on ? HIGH : LOW);
}

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);  // the UART is ours; stray log lines would only cost CRC errors
  Serial.setRxBufferSize(16384);
  Serial.setTxBufferSize(8192);
  Serial.begin(WL_SERIAL_BAUD);
  if (WL_STATUS_LED_PIN >= 0) pinMode(WL_STATUS_LED_PIN, OUTPUT);

  bootId = esp_random();
  loadSettings();
  linkLoad();
  for (int i = 0; i < MAX_CONNS; i++) {
    conns[i].state = C_FREE;
    conns[i].sock = -1;
    // only Wi-Fi mode connects to WLED from here; in ESP-NOW mode WLED's usermod holds the PC's data
    // (switching modes restarts the bridge)
    conns[i].pending = espnow() ? nullptr : (uint8_t*)malloc(CONN_WINDOW);
  }
  sendInfo();  // tells a connected PC straight away that the bridge restarted, rather than after its timeout
  setupWifi();
  if (espnow()) radioStart();
  else if (!settings.withPc) apStart();
  linkEnsureKey();
  wledBegin(espnow());
  phoneBegin();

  udpSock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSock >= 0) lwip_fcntl(udpSock, F_SETFL, lwip_fcntl(udpSock, F_GETFL, 0) | O_NONBLOCK);
  sendInfo();
}

void loop() {
  bool busy = pumpSerial();
  busy |= pumpConns();

  uint32_t now = millis();
  if (hostActive && now - lastHostRxMs > HOST_TIMEOUT_MS) endSession();
  if (settings.withPc && apOn && now - lastHostRxMs > AP_IDLE_OFF_MS) apStop();
  if (!hostActive && now - lastBeaconMs >= BEACON_INTERVAL_MS) sendInfo();
  linkLoop();
  char cmd[512];
  for (size_t n; espnow() && (n = wledTakeLinkCmd(cmd, sizeof cmd));) linkSendMsg(H_WLED_CMD, (const uint8_t*)cmd, n);
  if (staDirty) {
    staDirty = false;
    if (hostActive) sendStaList();
    else staCount = espnow() ? linkUp() : WiFi.softAPgetStationNum();
  } else if (now - staCountAtMs > 1000) {
    staCount = espnow() ? linkUp() : WiFi.softAPgetStationNum();
    staCountAtMs = now;
  }
  if (rebootAtMs && (int32_t)(now - rebootAtMs) >= 0) restartNow();
  if (radioRestartDue) {
    radioRestartDue = 0;
    radioRestart();
  }
  phoneLoop();
  if (baudTrialUntil && (int32_t)(now - baudTrialUntil) > 0) {  // the host never got through at the new speed
    baudTrialUntil = 0;
    setBaud(WL_SERIAL_BAUD);
    endSession();
  }
  static uint32_t sentStateSeq = 0;
  if (hostActive && (stateDirty || wledStateSeq() != sentStateSeq) && Serial.availableForWrite() > 300) {
    stateDirty = false;
    sentStateSeq = wledStateSeq();
    char buf[200];
    size_t n = wledStateJson(buf, sizeof buf);
    if (n) sendFrame(B_WLED_STATE, buf, n);
  }
  updateLed();
  if (!busy) delay(1);
}
