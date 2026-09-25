// Self-test for wled/usermod/link_proto.h, run on an ESP32: two ends of the link in one chip, joined by a
// simulated radio that loses, and can replay or corrupt, frames. Prints PASS/FAIL lines on the serial port.
#include <Arduino.h>
#include <vector>
#include <deque>
#include "../../../wled/usermod/link_proto.h"

struct Air { uint8_t from[6]; std::vector<uint8_t> f; };
static std::deque<Air> toB, toA;
static const uint8_t macA[6] = {0x02, 0, 0, 0, 0, 0xA1}, macB[6] = {0x02, 0, 0, 0, 0, 0xB2};
static wll::Link A, B;
static uint32_t lossPct = 0, busyPct = 0;
static std::vector<uint8_t> lastSealedToB, lastHelloFromA;
static uint32_t announced = 0;
static uint8_t annMode = 0xFF, annCh = 0;
static uint32_t dgB = 0, dgBad = 0, refusePct = 0;
static int passed = 0, failed = 0;

static bool coin(uint32_t pct) { return pct && (esp_random() % 100) < pct; }

static bool sendA(const uint8_t* mac, const uint8_t* f, size_t n) {
  if (coin(busyPct)) return false;
  if (f[1] == wll::F_SEALED) lastSealedToB.assign(f, f + n);
  if (f[1] == wll::F_HELLO) lastHelloFromA.assign(f, f + n);
  if (!coin(lossPct)) { Air a; memcpy(a.from, macA, 6); a.f.assign(f, f + n); toB.push_back(a); }
  return true;
}
static bool sendB(const uint8_t* mac, const uint8_t* f, size_t n) {
  if (coin(busyPct)) return false;
  if (!coin(lossPct)) { Air a; memcpy(a.from, macB, 6); a.f.assign(f, f + n); toA.push_back(a); }
  return true;
}
// Reliable messages are checked as they arrive against what the sender must have sent (a pattern
// derived from the message index), so nothing has to be kept in memory.
static uint32_t msgSize(uint32_t idx, bool big) { uint32_t h = idx * 2654435761u; return 1 + (h >> 8) % (big ? wll::MSG_MAX : 300); }
static uint8_t msgByte(uint32_t idx, uint32_t i) { return (uint8_t)(idx * 31 + i * 13 + (i >> 8)); }
static bool matches(const uint8_t* m, size_t n, uint32_t idx, bool big) {
  if (n != msgSize(idx, big)) return false;
  for (size_t i = 0; i < n; i++) if (m[i] != msgByte(idx, i)) return false;
  return true;
}
static uint32_t gotBCount = 0, gotACount = 0, badB = 0, badA = 0;
static std::vector<uint8_t> lastB;  // the latest message B received (for the replay and tamper checks)

static bool deliverB(const uint8_t* m, size_t n, bool reliable) {
  if (!reliable) {
    bool ok = n == 246;
    for (size_t i = 1; i < n && ok; i++) ok = m[i] == (uint8_t)(i * 7 + m[0]);
    ok ? dgB++ : dgBad++;
    return true;
  }
  if (coin(refusePct)) return false;
  if (!matches(m, n, gotBCount, true)) badB++;
  gotBCount++;
  lastB.assign(m, m + n);
  return true;
}
static bool deliverA(const uint8_t* m, size_t n, bool reliable) {
  if (coin(refusePct)) return false;
  if (!matches(m, n, 100000 + gotACount, false)) badA++;
  gotACount++;
  return true;
}

static uint32_t now = 1000;
static void step(int iterations = 1) {
  for (int i = 0; i < iterations; i++) {
    now += 1;
    size_t nb = toB.size(), na = toA.size();  // deliver what was on the air at the start of this step
    for (size_t k = 0; k < nb; k++) { Air a = toB.front(); toB.pop_front(); B.onFrame(a.from, a.f.data(), a.f.size(), now); }
    for (size_t k = 0; k < na; k++) { Air a = toA.front(); toA.pop_front(); A.onFrame(a.from, a.f.data(), a.f.size(), now); }
    A.tick(now);
    B.tick(now);
  }
}

static void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  ok ? passed++ : failed++;
}

static std::vector<uint8_t> makeMsg(uint32_t idx, bool big) {
  std::vector<uint8_t> v(msgSize(idx, big));
  for (uint32_t i = 0; i < v.size(); i++) v[i] = msgByte(idx, i);
  return v;
}

// Sends `count` messages each way with varying sizes and checks they all arrive, intact and in order.
static uint32_t sentA = 0, sentB = 0;
static bool transfer(int count, uint32_t maxSteps) {
  gotBCount = gotACount = badB = badA = 0;
  sentA = sentB = 0;
  uint32_t steps = 0;
  while ((gotBCount < (uint32_t)count || gotACount < (uint32_t)count) && steps < maxSteps) {
    if (sentA < (uint32_t)count) {
      auto m = makeMsg(sentA, true);
      if (A.sendMsg(m.data(), m.size())) sentA++;
    }
    if (sentB < (uint32_t)count) {
      auto m = makeMsg(100000 + sentB, false);
      if (B.sendMsg(m.data(), m.size())) sentB++;
    }
    step();
    steps++;
  }
  Serial.printf("         (%d messages each way in %lu simulated ms, %lu/%lu bad)\n", count, (unsigned long)steps,
                (unsigned long)badB, (unsigned long)badA);
  return gotBCount == (uint32_t)count && gotACount == (uint32_t)count && !badB && !badA;
}

void setup() {
  Serial.begin(921600);
  delay(300);
  Serial.println("\nlink_proto self-test");
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = i * 17 + 3;
  A.send = sendA; A.deliver = deliverA; A.begin(true, 16384); A.setKey(key);
  B.send = sendB; B.deliver = deliverB; B.begin(false, 8192); B.setKey(key);
  B.onHello = [](uint8_t m, uint8_t c) { announced++; annMode = m; annCh = c; };
  A.setAnnounce(wll::MODE_ESPNOW, 6);

  step(1200);
  check("handshake", A.up() && B.up());
  check("the light takes the bridge's mode and channel once the session is confirmed",
        announced == 1 && annMode == wll::MODE_ESPNOW && annCh == 6);

  uint32_t t0 = millis();
  check("200 messages each way, intact and in order (clean air)", transfer(200, 200000));
  Serial.printf("         (real time %lu ms incl. crypto)\n", (unsigned long)(millis() - t0));

  lossPct = 10;
  busyPct = 5;
  refusePct = 20;
  check("300 messages each way with 10% loss, busy radio and a slow consumer", transfer(300, 600000));
  check("still up after all that", A.up() && B.up());
  lossPct = busyPct = refusePct = 0;
  step(100);

  dgB = dgBad = 0;
  for (int i = 0; i < 50; i++) {
    uint8_t d[246];
    for (int k = 0; k < 246; k++) d[k] = (uint8_t)(k * 7 + i);
    d[0] = (uint8_t)i;
    for (int k = 1; k < 246; k++) d[k] = (uint8_t)(k * 7 + d[0]);
    A.sendDatagram(d, sizeof d);
    step(3);
  }
  step(20);
  check("datagrams (two fragments each) arrive intact", dgB == 50 && dgBad == 0);
  dgB = dgBad = 0;
  for (int i = 0; i < 20; i++) {
    uint8_t d[245];
    for (int k = 0; k < 245; k++) d[k] = (uint8_t)((k + 1) * 7 + i);
    A.sendDatagram2((uint8_t)i, d, sizeof d);  // type byte in front, no copy
    step(3);
  }
  step(20);
  check("datagrams with a type byte arrive intact", dgB == 20 && dgBad == 0);

  // replay: the last sealed frame A sent goes on the air again
  gotBCount = 0;
  auto m = makeMsg(0, true);
  A.sendMsg(m.data(), m.size());
  step(50);
  uint32_t before = gotBCount;
  std::vector<uint8_t> replay = lastSealedToB;
  for (int i = 0; i < 3; i++) B.onFrame(macA, replay.data(), replay.size(), now);
  step(50);
  check("a replayed frame is ignored", before == 1 && gotBCount == 1);

  // tampering: flip one ciphertext bit in a fresh frame
  lossPct = 100;  // hold everything back so we can tamper with it
  m = makeMsg(1, false);
  A.sendMsg(m.data(), m.size());
  step(1);
  lossPct = 0;
  std::vector<uint8_t> t = lastSealedToB;
  t[10] ^= 0x01;
  B.onFrame(macA, t.data(), t.size(), now);
  check("a tampered frame is rejected", gotBCount == 1);
  step(200);  // the real frame goes out again after the resend timeout
  check("the untampered resend gets through", gotBCount == 2 && lastB == m);

  // a stranger with another key can't join or inject
  wll::Link C;
  uint8_t other[16];
  for (int i = 0; i < 16; i++) other[i] = 0xEE ^ i;
  C.send = sendA; C.deliver = deliverA; C.begin(true, 4096); C.setKey(other);
  uint32_t beforeC = gotBCount;
  C.helloNow(now);
  step(20);
  check("a device with another key can't start a session", A.up() && B.up() && gotBCount == beforeC);

  // a recorded HELLO (announcing something else) played back to a working link changes nothing
  uint32_t upsB = B.stats.ups, annBefore = announced;
  A.setAnnounce(wll::MODE_WIFI, 11);
  A.helloNow(now);  // goes on the air, and is recorded
  step(50);
  std::vector<uint8_t> hello = lastHelloFromA;
  for (int i = 0; i < 3; i++) B.onFrame(macA, hello.data(), hello.size(), now);
  step(50);
  check("a HELLO played back to a working link is ignored", A.up() && B.up() && B.stats.ups == upsB && announced == annBefore);
  check("traffic keeps flowing meanwhile", transfer(20, 50000));

  // the bridge goes quiet, and while the light has no session the recording plays again
  lossPct = 100;
  step(wll::DEAD_MS + 200);
  check("both ends notice the silence", !A.up() && !B.up());
  B.onFrame(macA, hello.data(), hello.size(), now);
  step(10);
  check("a HELLO played back during an outage doesn't bring the light up", !B.up() && announced == annBefore);
  step(wll::SHAKE_MS + 100);
  check("the light gives up on it", !B.up() && !B.handshaking());
  lossPct = 0;
  step(3000);
  check("the real bridge is back within seconds", A.up() && B.up());
  check("and the light now follows what it announces", announced == annBefore + 1 && annMode == wll::MODE_WIFI && annCh == 11);

  // the bridge restarts: new session, traffic continues
  A.drop();
  step(3000);
  check("reconnects after one end restarts", A.up() && B.up());
  check("works after reconnecting", transfer(50, 100000));

  Serial.printf("\n%d/%d checks passed\n", passed, passed + failed);
}

void loop() { delay(1000); }
