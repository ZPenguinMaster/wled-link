#pragma once
// WLED Link radio link: an encrypted, reliable connection over ESP-NOW between the bridge ESP32 (plugged
// into the PC) and the WLED controller, so the two need no Wi-Fi network at all. The same file is built
// into the bridge firmware (bridge/src) and the WLED usermod (wled/usermod).
//
// Every frame is one ESP-NOW packet (at most 250 bytes) starting with MAGIC:
//   HELLO      bridge -> any : A7 01 mode chan bridgeNonce[8] tag[8]     tag = HMAC(K, "WLLH" | frame)
//   HELLO_ACK  WLED -> bridge: A7 02 bridgeNonce[8] wledNonce[8] tag[8]   tag = HMAC(K, "WLLA" | frame)
//   HELLO_REQ  WLED -> any   : A7 03 wledNonce[8] tag[8]                  tag = HMAC(K, "WLLR" | frame)
//   SEALED     both          : A7 10 counter[4] ciphertext tag[8]
// K is a 16-byte key both devices were given by the PC. A HELLO / HELLO_ACK pair gives each direction fresh
// keys, derived from K and both nonces. SEALED frames are AES-128-CTR, then HMAC-SHA256 over the whole
// frame (encrypt-then-MAC); the counter only goes up, so a recorded frame can't be played back.
// Inside a SEALED frame: DATA (reliable and in order: go-back-N with a window of 4), ACK, or DGRAM (best
// effort, for LED colour data, which is useless if late).
//
// The owner calls onFrame() for every received packet and tick() often, from one task.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <esp_system.h>

namespace wll {

static const uint8_t MAGIC = 0xA7;
enum : uint8_t { F_HELLO = 0x01, F_HELLO_ACK = 0x02, F_HELLO_REQ = 0x03, F_SEALED = 0x10 };
enum : uint8_t { K_DATA = 0x01, K_ACK = 0x02, K_DGRAM = 0x03 };
enum : uint8_t { MODE_ESPNOW = 0, MODE_WIFI = 1 };

static const size_t FRAME_MAX = 250;
static const size_t TAG_LEN = 8;
static const size_t SEAL_HDR = 6;                               // magic, type, counter
static const size_t PLAIN_MAX = FRAME_MAX - SEAL_HDR - TAG_LEN; // 236
static const size_t DATA_HDR = 4;                               // kind, seq, ack, flags
static const size_t FRAG_MAX = PLAIN_MAX - DATA_HDR;            // 232
static const size_t MSG_MAX = 1700;                             // largest message (serial frames are <= 1604)
static const uint8_t WINDOW = 4;
static const uint8_t FLAG_FIRST = 1, FLAG_LAST = 2;
static const uint32_t RTO_MS = 40;             // resend unacknowledged DATA after this long
static const uint32_t KEEPALIVE_MS = 1000;     // something is sent at least this often while up
static const uint32_t DEAD_MS = 4000;          // nothing valid heard for this long: the link is down
static const uint32_t MAX_RESENDS = 60;        // ~2.5 s of resending without progress: down

// HMAC-SHA256 over up to three pieces.
inline void hmac(const uint8_t* key, size_t keyLen, const void* a, size_t alen, const void* b, size_t blen,
                 const void* c, size_t clen, uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  if (alen) mbedtls_md_hmac_update(&ctx, (const uint8_t*)a, alen);
  if (blen) mbedtls_md_hmac_update(&ctx, (const uint8_t*)b, blen);
  if (clen) mbedtls_md_hmac_update(&ctx, (const uint8_t*)c, clen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

inline bool equalCT(const uint8_t* a, const uint8_t* b, size_t n) {  // constant time
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// First 4 bytes of HMAC(K, "WLLF"), as 8 hex digits: shows which key a device has without revealing it.
inline void fingerprint(const uint8_t key[16], char out[9]) {
  uint8_t h[32];
  hmac(key, 16, "WLLF", 4, nullptr, 0, nullptr, 0, h);
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    out[i * 2] = hex[h[i] >> 4];
    out[i * 2 + 1] = hex[h[i] & 15];
  }
  out[8] = 0;
}

struct DirKeys {
  mbedtls_aes_context aes;
  uint8_t mac[32];
  bool ready = false;
  void set(const uint8_t enc[16], const uint8_t macKey[32]) {
    if (ready) mbedtls_aes_free(&aes);
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, enc, 128);
    memcpy(mac, macKey, 32);
    ready = true;
  }
  void crypt(uint32_t counter, uint8_t* buf, size_t len) {  // AES-CTR: the same call encrypts and decrypts
    uint8_t block[16], stream[16];
    for (size_t off = 0, i = 0; off < len; off += 16, i++) {
      memset(block, 0, sizeof block);
      memcpy(block, &counter, 4);
      block[4] = (uint8_t)i;
      mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, stream);
      for (size_t j = 0; j < 16 && off + j < len; j++) buf[off + j] ^= stream[j];
    }
  }
};

class Link {
 public:
  // Set by the owner. send() puts one frame on the air (false if the radio can't take it right now).
  // deliver() gets each complete message; for reliable ones it may return false to refuse it for now
  // (the peer sends it again a little later), which is how a slow consumer pushes back.
  bool (*send)(const uint8_t* mac, const uint8_t* frame, size_t len) = nullptr;
  bool (*deliver)(const uint8_t* msg, size_t len, bool reliable) = nullptr;
  void (*onUp)() = nullptr;
  void (*onDown)() = nullptr;
  void (*onHello)(uint8_t mode, uint8_t channel) = nullptr;  // WLED side: what the bridge announced

  // queueSize: bytes of reliable messages that can wait to be sent (the bridge needs room for every open
  // connection's window; WLED only reads its sockets while there is room).
  void begin(bool bridgeSide, size_t queueSize) {
    isBridge = bridgeSide;
    if (q && queueSize != TXQ) {  // allocated earlier (setKey before begin) with another size
      free(q);
      q = nullptr;
    }
    TXQ = queueSize;
    reset();
  }

  void setKey(const uint8_t k[16]) {
    bool changed = !haveKey || memcmp(key, k, 16) != 0;
    memcpy(key, k, 16);
    haveKey = true;
    if (changed) goDown();
  }
  bool hasKey() const { return haveKey; }

  // Bridge side: what the HELLO announces.
  void setAnnounce(uint8_t mode, uint8_t channel) { annMode = mode; annChannel = channel; }

  // What happened so far, for diagnostics.
  struct Stats {
    uint32_t helloTx, helloRx, helloBad, ackTx, ackRx, ackBad, ackStale, reqTx, reqRx, reqBad;
    uint32_t sealedRx, sealedBad, sealedOld, sendFail, ups;
  };
  Stats stats = {};

  // "h1/2/0 a3/4/0/1 r5/6/0 s7/0/0 f0 u1": hello tx/rx/bad, ack tx/rx/bad/stale, req tx/rx/bad,
  // sealed rx/bad/old, send failures, sessions
  int statsText(char* out, size_t cap) const {
    return snprintf(out, cap, "h%lu/%lu/%lu a%lu/%lu/%lu/%lu r%lu/%lu/%lu s%lu/%lu/%lu f%lu u%lu",
                    (unsigned long)stats.helloTx, (unsigned long)stats.helloRx, (unsigned long)stats.helloBad,
                    (unsigned long)stats.ackTx, (unsigned long)stats.ackRx, (unsigned long)stats.ackBad,
                    (unsigned long)stats.ackStale, (unsigned long)stats.reqTx, (unsigned long)stats.reqRx,
                    (unsigned long)stats.reqBad, (unsigned long)stats.sealedRx, (unsigned long)stats.sealedBad,
                    (unsigned long)stats.sealedOld, (unsigned long)stats.sendFail, (unsigned long)stats.ups);
  }

  bool up() const { return isUp; }
  const uint8_t* peer() const { return peerMac; }
  bool hasPeer() const { return havePeer; }
  uint32_t lastHeard() const { return lastRx; }

  size_t sendRoom() const { return q ? TXQ - qLen : 0; }

  // Queues a message for reliable, in-order delivery. False if there is no room (or the link is down).
  bool sendMsg(const uint8_t* msg, size_t len) {
    if (!isUp || !q || len == 0 || len > MSG_MAX || qLen + len + 2 > TXQ) return false;
    qPush((uint8_t)(len & 0xFF));
    qPush((uint8_t)(len >> 8));
    for (size_t i = 0; i < len; i++) qPush(msg[i]);
    return true;
  }
  bool sendMsg2(uint8_t type, const uint8_t* payload, size_t len) {  // type byte + payload, without a copy
    if (!isUp || !q || len + 1 > MSG_MAX || qLen + len + 3 > TXQ) return false;
    qPush((uint8_t)((len + 1) & 0xFF));
    qPush((uint8_t)((len + 1) >> 8));
    qPush(type);
    for (size_t i = 0; i < len; i++) qPush(payload[i]);
    return true;
  }

  // Best effort, unordered with respect to sendMsg; dropped if the radio is busy.
  bool sendDatagram(const uint8_t* msg, size_t len) {
    if (!isUp || len == 0 || len > MSG_MAX) return false;
    uint8_t count = (uint8_t)((len + FRAG_MAX - 1) / FRAG_MAX);
    uint8_t id = ++dgTxId;
    uint8_t plain[PLAIN_MAX];
    for (uint8_t i = 0; i < count; i++) {
      size_t off = (size_t)i * FRAG_MAX, n = len - off < FRAG_MAX ? len - off : FRAG_MAX;
      plain[0] = K_DGRAM;
      plain[1] = id;
      plain[2] = i;
      plain[3] = count;
      memcpy(plain + 4, msg + off, n);
      if (!sendSealed(plain, n + 4)) return false;
    }
    return true;
  }

  void onFrame(const uint8_t mac[6], const uint8_t* f, size_t len, uint32_t now) {
    millisNow = now;
    if (len < 2 || f[0] != MAGIC || !haveKey) return;
    switch (f[1]) {
      case F_HELLO: if (!isBridge) rxHello(mac, f, len, now); break;
      case F_HELLO_ACK: if (isBridge) rxHelloAck(mac, f, len, now); break;
      case F_HELLO_REQ: if (isBridge) rxHelloReq(f, len, now); break;
      case F_SEALED: rxSealed(mac, f, len, now); break;
      default: break;
    }
  }

  void tick(uint32_t now) {
    millisNow = now;
    if (!haveKey) return;
    if (isUp && (now - lastRx > DEAD_MS || resends > MAX_RESENDS)) goDown();
    if (!isUp) {
      if (isBridge) {
        uint32_t every = now - downSince < 30000 ? 500 : 2000;  // eager at first, then relaxed
        if (now - lastHello >= every) sendHello(now);
      } else if (now - lastHello >= 1000) {
        sendHelloReq(now);
      }
      return;
    }
    pumpTx(now);
    if (ackOwed || now - lastTx >= KEEPALIVE_MS) sendAck();
  }

  // WLED side: send a HELLO_REQ now (e.g. right after moving to another channel).
  void requestHello(uint32_t now) { if (!isBridge && haveKey) sendHelloReq(now); }
  // Bridge side: send a HELLO now.
  void helloNow(uint32_t now) { if (isBridge && haveKey) sendHello(now); }
  void drop() { goDown(); }

 private:
  size_t TXQ = 8192;

  bool isBridge = true;
  uint8_t key[16];
  bool haveKey = false;
  bool isUp = false;
  uint8_t peerMac[6];
  bool havePeer = false;
  uint8_t bNonce[8], wNonce[8];
  uint8_t annMode = MODE_ESPNOW, annChannel = 1;
  DirKeys txKeys, rxKeys;
  uint32_t txCtr = 0, rxCtr = 0;
  uint32_t lastRx = 0, lastTx = 0, lastHello = 0, lastHelloReqSeen = 0, downSince = 0;
  uint8_t helloAckCache[26];
  bool helloAckValid = false;

  // reliable send: queued bytes not yet cut into fragments, and fragments waiting for an ACK
  uint8_t* q = nullptr;
  size_t qHead = 0, qLen = 0;
  size_t curLeft = 0;  // bytes of the message being cut up that are still in the queue
  bool curStarted = false;
  struct Pending { uint8_t seq, flags, len; uint8_t data[FRAG_MAX]; };
  Pending pend[WINDOW];
  uint8_t pendCount = 0, nextSeq = 0;
  uint32_t pendSince = 0, resends = 0;

  // reliable receive
  uint8_t rxExpect = 0;
  uint8_t rxBuf[MSG_MAX];
  size_t rxLen = 0;
  bool rxInMsg = false, ackOwed = false;

  // datagrams
  uint8_t dgTxId = 0, dgRxId = 0, dgNext = 0, dgCount = 0;
  uint8_t dgBuf[MSG_MAX];
  size_t dgLen = 0;
  bool dgActive = false;

  void qPush(uint8_t b) { q[(qHead + qLen++) % TXQ] = b; }
  uint8_t qPop() { uint8_t b = q[qHead]; qHead = (qHead + 1) % TXQ; qLen--; return b; }

  void reset() {
    if (!q) q = (uint8_t*)malloc(TXQ);  // stays null if memory is short: then nothing reliable is queued
    qHead = qLen = 0;
    curLeft = 0;
    curStarted = false;
    pendCount = 0;
    nextSeq = 0;
    resends = 0;
    rxExpect = 0;
    rxLen = 0;
    rxInMsg = false;
    ackOwed = false;
    dgActive = false;
    txCtr = rxCtr = 0;
  }

  void goDown() {
    bool was = isUp;
    isUp = false;
    helloAckValid = false;
    reset();
    downSince = lastHello = 0;
    if (was && onDown) onDown();
  }

  void goUp(uint32_t now) {
    if (isUp && onDown) onDown();  // a new session replaces the old one: whatever was open is gone
    reset();
    isUp = true;
    stats.ups++;
    lastRx = lastTx = now;
    if (onUp) onUp();
  }

  void tag(const char* label, const uint8_t* f, size_t n, uint8_t out[TAG_LEN]) {
    uint8_t h[32];
    hmac(key, 16, label, 4, f, n, nullptr, 0, h);
    memcpy(out, h, TAG_LEN);
  }

  void deriveKeys() {
    uint8_t enc[32], mac[32];
    static const char* labels[2][2] = {{"EB2W", "MB2W"}, {"EW2B", "MW2B"}};
    for (int dir = 0; dir < 2; dir++) {
      hmac(key, 16, labels[dir][0], 4, bNonce, 8, wNonce, 8, enc);
      hmac(key, 16, labels[dir][1], 4, bNonce, 8, wNonce, 8, mac);
      bool bridgeToWled = dir == 0;
      DirKeys& k = (bridgeToWled == isBridge) ? txKeys : rxKeys;
      k.set(enc, mac);
    }
  }

  void sendHello(uint32_t now) {
    stats.helloTx++;
    uint8_t f[20] = {MAGIC, F_HELLO, annMode, annChannel};
    esp_fill_random(bNonce, 8);
    memcpy(f + 4, bNonce, 8);
    tag("WLLH", f, 12, f + 12);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (send) send(bcast, f, sizeof f);
    lastHello = now;
    if (!downSince) downSince = now;
  }

  void sendHelloReq(uint32_t now) {
    stats.reqTx++;
    uint8_t f[18] = {MAGIC, F_HELLO_REQ};
    esp_fill_random(f + 2, 8);
    tag("WLLR", f, 10, f + 10);
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (send) send(bcast, f, sizeof f);
    lastHello = now;
  }

  void rxHello(const uint8_t mac[6], const uint8_t* f, size_t len, uint32_t now) {
    uint8_t t[TAG_LEN];
    if (len != 20) return;
    tag("WLLH", f, 12, t);
    if (!equalCT(t, f + 12, TAG_LEN)) { stats.helloBad++; return; }
    stats.helloRx++;
    if (onHello) onHello(f[2], f[3]);
    if (isUp && havePeer && !memcmp(mac, peerMac, 6) && !memcmp(bNonce, f + 4, 8) && helloAckValid) {
      if (send && !send(peerMac, helloAckCache, sizeof helloAckCache)) stats.sendFail++;  // our answer got lost: repeat it
      stats.ackTx++;
      return;
    }
    memcpy(bNonce, f + 4, 8);
    esp_fill_random(wNonce, 8);
    memcpy(peerMac, mac, 6);
    havePeer = true;
    deriveKeys();
    uint8_t* a = helloAckCache;
    a[0] = MAGIC;
    a[1] = F_HELLO_ACK;
    memcpy(a + 2, bNonce, 8);
    memcpy(a + 10, wNonce, 8);
    tag("WLLA", a, 18, a + 18);
    helloAckValid = true;
    goUp(now);
    if (send && !send(peerMac, a, sizeof helloAckCache)) stats.sendFail++;
    stats.ackTx++;
  }

  void rxHelloAck(const uint8_t mac[6], const uint8_t* f, size_t len, uint32_t now) {
    uint8_t t[TAG_LEN];
    if (len != 26) { stats.ackBad++; return; }
    if (memcmp(f + 2, bNonce, 8) != 0) { stats.ackStale++; return; }  // must answer our latest HELLO
    tag("WLLA", f, 18, t);
    if (!equalCT(t, f + 18, TAG_LEN)) { stats.ackBad++; return; }
    stats.ackRx++;
    if (isUp && havePeer && !memcmp(mac, peerMac, 6) && !memcmp(wNonce, f + 10, 8)) return;  // repeat
    memcpy(wNonce, f + 10, 8);
    memcpy(peerMac, mac, 6);
    havePeer = true;
    deriveKeys();
    goUp(now);
    sendAck();  // lets WLED know we are there, and starts the counters
  }

  void rxHelloReq(const uint8_t* f, size_t len, uint32_t now) {
    uint8_t t[TAG_LEN];
    if (len != 18) return;
    tag("WLLR", f, 10, t);
    if (!equalCT(t, f + 10, TAG_LEN)) { stats.reqBad++; return; }
    stats.reqRx++;
    if (now - lastHelloReqSeen < 100) return;  // one answer per burst
    lastHelloReqSeen = now;
    sendHello(now);
  }

  bool sendSealed(const uint8_t* plain, size_t n) {
    uint8_t f[FRAME_MAX];
    uint32_t ctr = ++txCtr;
    f[0] = MAGIC;
    f[1] = F_SEALED;
    memcpy(f + 2, &ctr, 4);
    memcpy(f + SEAL_HDR, plain, n);
    txKeys.crypt(ctr, f + SEAL_HDR, n);
    uint8_t h[32];
    hmac(txKeys.mac, 32, f, SEAL_HDR + n, nullptr, 0, nullptr, 0, h);
    memcpy(f + SEAL_HDR + n, h, TAG_LEN);
    if (!send || !send(peerMac, f, SEAL_HDR + n + TAG_LEN)) {
      stats.sendFail++;
      return false;
    }
    lastTx = millisNow;
    return true;
  }

  void rxSealed(const uint8_t mac[6], const uint8_t* f, size_t len, uint32_t now) {
    if (!isUp || !havePeer || memcmp(mac, peerMac, 6) != 0 || len < SEAL_HDR + TAG_LEN + 1) return;
    uint32_t ctr;
    memcpy(&ctr, f + 2, 4);
    if (ctr <= rxCtr) { stats.sealedOld++; return; }  // replayed or duplicated
    uint8_t h[32];
    hmac(rxKeys.mac, 32, f, len - TAG_LEN, nullptr, 0, nullptr, 0, h);
    if (!equalCT(h, f + len - TAG_LEN, TAG_LEN)) { stats.sealedBad++; return; }
    stats.sealedRx++;
    rxCtr = ctr;
    lastRx = now;
    uint8_t plain[PLAIN_MAX];
    size_t n = len - SEAL_HDR - TAG_LEN;
    memcpy(plain, f + SEAL_HDR, n);
    rxKeys.crypt(ctr, plain, n);
    switch (plain[0]) {
      case K_DATA: if (n >= DATA_HDR) { rxAck(plain[2], now); rxData(plain, n); } break;
      case K_ACK: if (n >= 2) rxAck(plain[1], now); break;
      case K_DGRAM: if (n > 4) rxDgram(plain, n); break;
      default: break;
    }
  }

  void rxAck(uint8_t ack, uint32_t now) {
    if (!pendCount) return;
    uint8_t acked = (uint8_t)(ack - pend[0].seq);
    if (acked == 0 || acked > pendCount) return;
    memmove(pend, pend + acked, (pendCount - acked) * sizeof(Pending));
    pendCount -= acked;
    pendSince = now;
    resends = 0;
  }

  void rxData(const uint8_t* p, size_t n) {
    uint8_t seq = p[1], flags = p[3];
    const uint8_t* frag = p + DATA_HDR;
    size_t fragLen = n - DATA_HDR;
    ackOwed = true;
    if (seq != rxExpect) return;  // duplicate or gap: our ACK tells the peer where we are
    if (flags & FLAG_FIRST) {
      rxLen = 0;
      rxInMsg = true;
    }
    if (!rxInMsg || rxLen + fragLen > MSG_MAX) {  // can't happen with a well-behaved peer
      rxInMsg = false;
      rxLen = 0;
      rxExpect++;
      return;
    }
    memcpy(rxBuf + rxLen, frag, fragLen);
    if (flags & FLAG_LAST) {
      if (deliver && !deliver(rxBuf, rxLen + fragLen, true)) return;  // not now: the peer resends it
      rxInMsg = false;
      rxLen = 0;
    } else {
      rxLen += fragLen;
    }
    rxExpect++;
  }

  void rxDgram(const uint8_t* p, size_t n) {
    uint8_t id = p[1], idx = p[2], count = p[3];
    if (idx == 0) {
      dgRxId = id;
      dgNext = 0;
      dgCount = count;
      dgLen = 0;
      dgActive = true;
    }
    if (!dgActive || id != dgRxId || idx != dgNext || dgLen + (n - 4) > MSG_MAX) {
      dgActive = false;
      return;
    }
    memcpy(dgBuf + dgLen, p + 4, n - 4);
    dgLen += n - 4;
    if (++dgNext == dgCount) {
      dgActive = false;
      if (deliver) deliver(dgBuf, dgLen, false);
    }
  }

  bool transmit(const Pending& p) {
    uint8_t plain[PLAIN_MAX];
    plain[0] = K_DATA;
    plain[1] = p.seq;
    plain[2] = rxExpect;
    plain[3] = p.flags;
    memcpy(plain + DATA_HDR, p.data, p.len);
    if (!sendSealed(plain, DATA_HDR + p.len)) return false;
    ackOwed = false;
    return true;
  }

  void pumpTx(uint32_t now) {
    if (pendCount && now - pendSince >= RTO_MS) {  // go-back-N: send everything unacknowledged again
      for (uint8_t i = 0; i < pendCount; i++)
        if (!transmit(pend[i])) break;
      pendSince = now;
      resends++;
    }
    while (pendCount < WINDOW && qLen) {
      if (!curStarted) {
        curLeft = qPop();
        curLeft |= (size_t)qPop() << 8;
        curStarted = true;
        cutFirst = true;
      }
      Pending& p = pend[pendCount];
      p.seq = nextSeq;
      p.len = (uint8_t)(curLeft < FRAG_MAX ? curLeft : FRAG_MAX);
      for (uint8_t i = 0; i < p.len; i++) p.data[i] = qPop();
      curLeft -= p.len;
      p.flags = (cutFirst ? FLAG_FIRST : 0) | (curLeft == 0 ? FLAG_LAST : 0);
      cutFirst = false;
      if (curLeft == 0) curStarted = false;
      if (!pendCount) pendSince = now;
      pendCount++;
      nextSeq++;
      if (!transmit(p)) break;  // radio busy: it goes out with the next resend
    }
  }
  bool cutFirst = false;
  uint32_t millisNow = 0;

  void sendAck() {
    uint8_t plain[2] = {K_ACK, rxExpect};
    if (sendSealed(plain, 2)) ackOwed = false;
  }
};

}  // namespace wll
