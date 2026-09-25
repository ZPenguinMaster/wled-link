#pragma once
#include <Arduino.h>
#include <esp_wifi_types.h>

// The radio link to the WLED controller (wled/usermod/link_proto.h): ESP-NOW, encrypted, no Wi-Fi network.
// In ESP-NOW mode everything to and from WLED goes over it; in Wi-Fi mode it runs alongside the hidden
// network and carries the mode switch. The bridge decides the mode and WLED follows.

enum LinkMode : uint8_t { LINK_ESPNOW = 0, LINK_WIFI = 1 };

void linkLoad();                     // mode, key and remembered channel from flash
void linkEnsureKey();                // makes a key on first use (call once the radio is on: true randomness)
LinkMode linkMode();                 // Wi-Fi until the PC chose a mode, so a new light can be set up
bool linkModeChosen();
bool linkSetMode(LinkMode m);        // saves it; takes effect after a restart
bool linkSetKey(const uint8_t key[16]);
const uint8_t* linkKey();
void linkFingerprint(char out[9]);   // "" without a key
uint8_t linkSavedChannel();          // the channel picked once for good (0 = none yet)
void linkSaveChannel(uint8_t ch);
void linkForgetChannel();

void linkStart(wifi_interface_t ifx, uint8_t channel, uint8_t announceMode);  // on the running radio
void linkStop();
void linkLoop();
bool linkUp();
bool linkPeerMac(uint8_t out[6]);
bool linkSendMsg(uint8_t type, const uint8_t* payload, size_t len);        // reliable, in order
bool linkSendDatagram(uint8_t type, const uint8_t* payload, size_t len);   // best effort
uint32_t linkFramesSent();
uint32_t linkFramesLost();
uint32_t linkStalls();               // times the radio stopped confirming frames (and was started over)
void linkStatsText(char* out, size_t cap);  // handshake and frame counters, for diagnostics

// implemented by main.cpp
bool onLinkMessage(const uint8_t* msg, size_t len);  // false: not now, WLED sends it again
void onLinkChange(bool up);
void onLinkStall();                  // the radio stopped confirming frames: start it over (not from inside linkLoop)
