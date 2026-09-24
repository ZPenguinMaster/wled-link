#include "wled.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <base64.h>
#include <errno.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

static const size_t RX_MAX = 8192;         // WLED's state+info push is ~3 KB
static const size_t CMD_MAX = 512;
static const uint32_t PING_EVERY_MS = 10000;
static const uint32_t SILENT_LIMIT_MS = 25000;  // no data (not even a pong) for this long = dead connection
static const uint32_t PROBE_TIMEOUT_MS = 2000;  // after a device joins or leaves the Wi-Fi, WLED must answer a ping by then
static const uint32_t RETRY_MS = 1000;
static const uint32_t STREAM_TIMEOUT_MS = 2500; // WLED's default: it leaves live mode this long after the last packet

struct State {
  int on = -1, bri = -1, lor = -1, live = -1, ps = -1, fx = -1, cct = -1;
  int col[4] = {-1, -1, -1, -1};
  bool fromWled = false;  // at least one real state push from WLED so far
};

struct Cmd {
  uint16_t len;
  char json[CMD_MAX];
};

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static State state;
static volatile uint32_t seq = 0;
static volatile bool connected = false;
static QueueHandle_t cmds = nullptr;

static uint32_t wledIp = 0;  // network byte order, 0 = look it up
static int sock = -1;
static uint8_t* rx = nullptr;
static size_t rxLen = 0;
static size_t skipBytes = 0;
static uint32_t lastRxMs = 0, lastPingMs = 0, lastTryMs = 0;
static Cmd lastCmd;          // resent once if WLED was too busy to take it
static bool retryUsed = true;
static volatile bool probeWanted = false;
static uint32_t probeSentMs = 0;          // 0 = no probe waiting for an answer
static volatile uint32_t lastStreamMs = 0;  // 0 = no stream yet

// ---------------------------------------------------------------------------------------------
// tiny JSON field reader (the few keys we need, no JSON library)

static const char* findKey(const char* from, const char* to, const char* key) {
  size_t n = strlen(key);
  for (const char* p = from; p && p + n <= to; p++)
    if (*p == key[0] && memcmp(p, key, n) == 0) return p + n;
  return nullptr;
}

static bool readValue(const char* p, int* out) {
  if (!p) return false;
  if (*p == 't' || *p == 'f') {  // true / false
    *out = *p == 't';
    return true;
  }
  char* end;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = (int)v;
  return true;
}

// Reads on/bri/lor/ps and the first segment's fx/cct/col from WLED JSON (a state push or a command).
static void readState(const char* from, const char* to, State& s) {
  int v;
  if (readValue(findKey(from, to, "\"on\":"), &v)) s.on = v;
  if (readValue(findKey(from, to, "\"bri\":"), &v)) s.bri = v;
  if (readValue(findKey(from, to, "\"lor\":"), &v)) s.lor = v;
  if (readValue(findKey(from, to, "\"ps\":"), &v)) s.ps = v;
  const char* seg = findKey(from, to, "\"seg\":");
  if (!seg) return;
  if (readValue(findKey(seg, to, "\"fx\":"), &v)) s.fx = v;
  if (readValue(findKey(seg, to, "\"cct\":"), &v)) s.cct = v;
  const char* col = findKey(seg, to, "\"col\":[[");
  for (int i = 0; col && i < 4; i++) {
    char* end;
    long c = strtol(col, &end, 10);
    if (end == col) break;
    s.col[i] = (int)c;
    col = *end == ',' ? end + 1 : nullptr;
  }
}

static void publish(const State& s) {
  portENTER_CRITICAL(&mux);
  state = s;
  seq++;
  portEXIT_CRITICAL(&mux);
}

static State snapshot() {
  portENTER_CRITICAL(&mux);
  State s = state;
  portEXIT_CRITICAL(&mux);
  return s;
}

// ---------------------------------------------------------------------------------------------
// HTTP (finding WLED, and a fallback when the WebSocket is down)

static int httpRequest(uint32_t ip, const char* method, const char* path, const char* body, String* out) {
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
    if (ip && httpRequest(ip, "GET", "/json/info", nullptr, &info) == 200 &&
        (info.indexOf("\"brand\":\"WLED\"") >= 0 || info.indexOf("\"leds\"") >= 0)) {
      wledIp = ip;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// WebSocket client

static void setConnected(bool on) {
  if (connected == on) return;
  connected = on;
  portENTER_CRITICAL(&mux);
  seq++;
  portEXIT_CRITICAL(&mux);
}

static void closeWs() {
  if (sock >= 0) lwip_close(sock);
  sock = -1;
  rxLen = 0;
  skipBytes = 0;
  probeSentMs = 0;
  setConnected(false);
}

static bool sendAll(const uint8_t* d, size_t n) {
  uint32_t start = millis();
  while (n) {
    int k = lwip_send(sock, d, n, MSG_DONTWAIT);
    if (k > 0) {
      d += k;
      n -= k;
    } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && millis() - start < 500) {
      vTaskDelay(1);
    } else {
      return false;
    }
  }
  return true;
}

static bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (sock < 0 || len > CMD_MAX) return false;
  uint8_t buf[8 + CMD_MAX];
  size_t h = 0;
  buf[h++] = 0x80 | opcode;  // FIN + opcode
  if (len < 126) {
    buf[h++] = 0x80 | len;   // client frames are always masked
  } else {
    buf[h++] = 0x80 | 126;
    buf[h++] = len >> 8;
    buf[h++] = len & 0xFF;
  }
  uint32_t key = esp_random();
  uint8_t* mask = buf + h;
  memcpy(mask, &key, 4);
  h += 4;
  for (size_t i = 0; i < len; i++) buf[h + i] = data[i] ^ mask[i & 3];
  return sendAll(buf, h + len);
}

static void onMessage(uint8_t opcode, uint8_t* payload, size_t len) {
  if (opcode == 0x9) {  // ping: answer with the same payload
    sendFrame(0xA, payload, len);
  } else if (opcode == 0x8) {
    closeWs();
  } else if (opcode == 0x1) {
    const char* msg = (const char*)payload;
    const char* end = msg + len;
    const char* st = findKey(msg, end, "\"state\":{");
    if (!st) {
      // {"success":true}, or {"error":3} when WLED's JSON buffer was busy: send the command again once
      if (findKey(msg, end, "\"error\":") && !retryUsed) {
        retryUsed = true;
        sendFrame(0x1, (const uint8_t*)lastCmd.json, lastCmd.len);
      }
      return;
    }
    const char* info = findKey(st, end, "\"info\":{");
    State s = snapshot();
    readState(st, info ? info : end, s);  // not WLED's "live": it isn't updated when a stream starts (see task)
    s.fromWled = true;
    publish(s);
  }
}

static void parseFrames() {
  size_t pos = 0;
  for (;;) {
    if (skipBytes) {  // tail of a frame too big to keep
      size_t d = min(skipBytes, rxLen - pos);
      pos += d;
      skipBytes -= d;
      if (skipBytes) break;
    }
    size_t avail = rxLen - pos;
    if (avail < 2) break;
    uint8_t b0 = rx[pos], b1 = rx[pos + 1];
    size_t h = 2;
    uint64_t plen = b1 & 0x7F;
    if (plen == 126) {
      if (avail < 4) break;
      plen = (rx[pos + 2] << 8) | rx[pos + 3];
      h = 4;
    } else if (plen == 127) {
      if (avail < 10) break;
      plen = 0;
      for (int i = 0; i < 8; i++) plen = (plen << 8) | rx[pos + 2 + i];
      h = 10;
    }
    bool masked = b1 & 0x80;
    if (masked) h += 4;
    if (h + plen > RX_MAX) {
      skipBytes = h + plen;
      continue;
    }
    if (avail < h + plen) break;
    uint8_t* payload = rx + pos + h;
    if (masked)
      for (size_t i = 0; i < plen; i++) payload[i] ^= rx[pos + h - 4 + (i & 3)];
    uint8_t saved = payload[plen];
    payload[plen] = 0;  // rx has one spare byte past RX_MAX for this
    onMessage(b0 & 0x0F, payload, plen);
    if (sock < 0) return;  // closed while handling
    payload[plen] = saved;
    pos += h + plen;
  }
  memmove(rx, rx + pos, rxLen - pos);
  rxLen -= pos;
}

static void pumpRx() {
  while (sock >= 0) {
    int k = lwip_recv(sock, rx + rxLen, RX_MAX - rxLen, MSG_DONTWAIT);
    if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return closeWs();
    if (k < 0) break;
    rxLen += k;
    lastRxMs = millis();
    probeSentMs = 0;  // WLED is still there
    parseFrames();
  }
}

static bool waitSocket(int s, bool forWrite, uint32_t ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
  return lwip_select(s + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv) > 0;
}

static void connectWs() {
  lastTryMs = millis();
  if (!wledIp && !findWled()) return;
  int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return;
  lwip_fcntl(s, F_SETFL, lwip_fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  int one = 1;
  lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = wledIp;
  addr.sin_port = htons(80);
  int err = 0;
  socklen_t elen = sizeof err;
  bool ok = (lwip_connect(s, (sockaddr*)&addr, sizeof addr) == 0 || errno == EINPROGRESS) && waitSocket(s, true, 1500) &&
            lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0;
  if (ok) {
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof nonce);
    String req = String("GET /ws HTTP/1.1\r\nHost: ") + IPAddress(wledIp).toString() +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64::encode(nonce, sizeof nonce) +
                 "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    sock = s;
    ok = sendAll((const uint8_t*)req.c_str(), req.length());
  }
  // read the handshake response; anything after it (WLED sends its state right away) stays in rx
  rxLen = 0;
  const char* body = nullptr;
  uint32_t start = millis();
  while (ok && !body && millis() - start < 1500) {
    if (!waitSocket(s, false, 100)) continue;
    int k = lwip_recv(s, rx + rxLen, RX_MAX - rxLen, MSG_DONTWAIT);
    if (k <= 0) {
      ok = k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
      continue;
    }
    rxLen += k;
    rx[rxLen] = 0;
    body = strstr((const char*)rx, "\r\n\r\n");
  }
  if (!ok || !body || strncmp((const char*)rx, "HTTP/1.1 101", 12) != 0) {
    sock = s;
    closeWs();
    wledIp = 0;  // WLED may have a new address; look again next time
    return;
  }
  size_t used = body + 4 - (const char*)rx;
  memmove(rx, rx + used, rxLen - used);
  rxLen -= used;
  sock = s;
  lastRxMs = lastPingMs = millis();
  setConnected(true);
  parseFrames();
}

static void applyCommand(const Cmd& cmd) {
  // publish what the command will do right away; WLED's own push confirms it later
  State s = snapshot();
  readState(cmd.json, cmd.json + cmd.len, s);
  if (s.bri > 0 && !findKey(cmd.json, cmd.json + cmd.len, "\"on\":") && findKey(cmd.json, cmd.json + cmd.len, "\"bri\":"))
    s.on = 1;  // WLED turns on when given a brightness
  publish(s);

  if (sock < 0) connectWs();
  lastCmd = cmd;
  retryUsed = false;
  if (sock >= 0 && sendFrame(0x1, (const uint8_t*)cmd.json, cmd.len)) return;
  closeWs();
  if (wledIp) httpRequest(wledIp, "POST", "/json/state", cmd.json, nullptr);  // slower path, still works
}

// WLED doesn't tell WebSocket clients when a stream starts or stops, but every stream reaches WLED
// through this bridge, so the bridge can tell by itself.
static void trackStream() {
  uint32_t at = lastStreamMs;
  int live = at && millis() - at < STREAM_TIMEOUT_MS ? 1 : 0;
  if (live == state.live) return;  // only this task changes the state, so no lock needed to read it
  State s = snapshot();
  s.live = live;
  publish(s);
}

static void task(void*) {
  static Cmd cmd;
  for (;;) {
    if (sock < 0 && millis() - lastTryMs > RETRY_MS) connectWs();
    if (xQueueReceive(cmds, &cmd, pdMS_TO_TICKS(sock >= 0 ? 2 : 50)) == pdTRUE) {
      cmd.json[cmd.len] = 0;
      applyCommand(cmd);
    }
    trackStream();
    if (sock < 0) continue;
    if (probeWanted) {
      probeWanted = false;
      if (sendFrame(0x9, nullptr, 0)) probeSentMs = millis() | 1;
      else closeWs();
    }
    pumpRx();
    uint32_t now = millis();
    if (sock >= 0 && now - lastPingMs > PING_EVERY_MS) {
      lastPingMs = now;
      if (!sendFrame(0x9, nullptr, 0)) closeWs();
    }
    if (sock >= 0 && probeSentMs && now - probeSentMs > PROBE_TIMEOUT_MS) {
      // A restarted WLED answers with a reset (handled in pumpRx); one that moved to another address
      // doesn't answer at all. Either way, start over now instead of after SILENT_LIMIT_MS.
      closeWs();
      wledIp = 0;
      lastTryMs = now - RETRY_MS - 1;
    }
    if (sock >= 0 && now - lastRxMs > SILENT_LIMIT_MS) closeWs();
  }
}

// ---------------------------------------------------------------------------------------------

void wledBegin() {
  rx = (uint8_t*)malloc(RX_MAX + 1);
  cmds = xQueueCreate(8, sizeof(Cmd));
  xTaskCreatePinnedToCore(task, "wled", 8192, nullptr, 2, nullptr, 1);
}

bool wledSend(const char* json, size_t len) {
  if (!cmds || len == 0 || len >= CMD_MAX) return false;
  Cmd cmd;
  cmd.len = len;
  memcpy(cmd.json, json, len);
  if (xQueueSend(cmds, &cmd, 0) == pdTRUE) return true;
  // full: drop the oldest so the newest input always gets through
  Cmd old;
  xQueueReceive(cmds, &old, 0);
  return xQueueSend(cmds, &cmd, 0) == pdTRUE;
}

uint32_t wledStateSeq() { return seq; }
bool wledConnected() { return connected; }
void wledNoteStream() { lastStreamMs = millis() | 1; }  // | 1: 0 means "no stream yet"
void wledCheckLink() { probeWanted = true; }

size_t wledStateJson(char* out, size_t cap) {
  State s = snapshot();
  int n = snprintf(out, cap,
      "{\"n\":%lu,\"ws\":%d,\"ok\":%d,\"on\":%d,\"bri\":%d,\"lor\":%d,\"live\":%d,\"ps\":%d,\"fx\":%d,\"cct\":%d,"
      "\"col\":[%d,%d,%d,%d]}",
      (unsigned long)seq, connected ? 1 : 0, s.fromWled ? 1 : 0, s.on, s.bri, s.lor, s.live, s.ps, s.fx, s.cct,
      s.col[0], s.col[1], s.col[2], s.col[3]);
  return n > 0 && (size_t)n < cap ? n : 0;
}
