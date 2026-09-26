#pragma once
#include <Arduino.h>

// Bluetooth LE light control for a phone. Commands go to WLED through wled.cpp; the light's state
// is pushed back as notifications. Only encrypted, PIN-paired connections are served, and only
// phones paired while the pairing window was open (from the PC or by holding the BOOT button).

void phoneBegin();
void phoneLoop();
void phoneEnd();  // stops Bluetooth, before a restart
void phoneOpenPairing(uint16_t seconds);
void phoneForget();
bool phoneSetPin(uint32_t pin);
uint32_t phonePin();
int phoneCount();
int phoneDiag(char* out, size_t cap);  // "c1 a1 r0 x0/0 b1": connections, advertising, advertising restarts, refused/last reason, pairings
int phonePairingLeft();  // seconds, 0 = closed

// WLED's own web page on the phone: the TUNNEL characteristic carries TCP connections to WLED's web server,
// with the serial protocol's messages (H_TCP_OPEN/DATA/CLOSE from the phone, B_TCP_* back), each as
// type(1) slot(1) len(2 LE) payload, in one byte stream per direction. One phone uses it at a time: the one
// that last wrote to it. main.cpp serves the connections like the PC's (numbered after them). A reset (0x13)
// from the phone closes them all and is echoed back: anything that arrives before the echo is stale.
static const int PHONE_SLOTS = 2;
size_t phoneTunnelRead(uint8_t* out, size_t cap, uint32_t* epoch);  // next message: type slot payload (0 = none)
uint32_t phoneTunnelEpoch();  // changes when another phone takes the tunnel over, or the phone leaves
bool phoneTunnelSend(uint8_t type, uint8_t slot, const uint8_t* payload, size_t len);  // false: no room
size_t phoneTunnelRoom();     // data bytes that can be queued for the phone right now
void phoneTunnelClear();      // drop what is waiting for the phone (it started over)
uint32_t phoneTunnelDrained(uint8_t slot, bool* idle);  // data bytes of this slot the phone got since credited
void phoneTunnelCredited(uint8_t slot, uint32_t bytes); // (idle: none of its data is still waiting)
// Testing the phone's path from the PC, over USB: these bytes count as written by a phone, and what the tunnel
// sends back goes to `out` instead of Bluetooth (false: no room right now, try again later).
void phoneTunnelTest(const uint8_t* bytes, size_t len, bool (*out)(const uint8_t*, size_t));
void phoneTunnelTestEnd();  // the PC's session ended
