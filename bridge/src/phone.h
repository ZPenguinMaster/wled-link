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
