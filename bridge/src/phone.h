#pragma once
#include <Arduino.h>

// Bluetooth LE light control for a phone, forwarded to WLED over the bridge's Wi-Fi.
// Only encrypted, PIN-paired connections are served, and only phones paired while the
// pairing window was open (from the PC or by holding the BOOT button) are accepted.

void phoneBegin();
void phoneLoop();
void phoneOpenPairing(uint16_t seconds);
void phoneForget();
bool phoneSetPin(uint32_t pin);
uint32_t phonePin();
int phoneCount();
int phonePairingLeft();  // seconds, 0 = closed
void phoneInject(const char* json, size_t len);  // test path: run a command as if a phone sent it
bool phoneTakeResult(char* out, size_t cap);     // result of the last injected command
