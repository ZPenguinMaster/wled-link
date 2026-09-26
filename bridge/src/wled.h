#pragma once
#include <Arduino.h>

// Keeps a persistent WebSocket to WLED so commands from the phone or the PC are applied without
// setting up a TCP connection or HTTP request each time, and tracks WLED's state for everyone.
//
// WLED pushes its state to WebSocket clients at most once a second, so after forwarding a command
// the bridge immediately publishes the state the command will produce ("optimistic" state), and
// WLED's own push corrects it if anything differs. WLED pushes nothing when a SignalRGB stream starts
// or stops, so "live" comes from the stream the bridge forwards instead.

void wledBegin(bool overLink);  // overLink: ESP-NOW mode, no WebSocket; commands via wledTakeLinkCmd()
bool wledSend(const char* json, size_t len);       // queue a WLED JSON state command
uint32_t wledAddress();                            // Wi-Fi mode: the light's IP (network order), 0 = unknown
uint32_t wledStateSeq();                            // changes whenever the state below changes
size_t wledStateJson(char* out, size_t cap);        // {"n":..,"ws":1,"on":1,"bri":..,"lor":..,"live":..,"ps":..,"fx":..,"cct":..,"col":[r,g,b,w],"tn":1}  (tn: the phone can open WLED's page)
void wledNoteStream();  // the bridge just passed realtime LED data (SignalRGB) on to WLED
void wledCheckLink();   // a device joined or left the Wi-Fi: make sure the WebSocket still reaches WLED
// ESP-NOW mode
size_t wledTakeLinkCmd(char* out, size_t cap);  // next command for the radio link (0 = none)
void wledLinkState(const char* json, size_t len);  // WLED's state, as its usermod reported it
void wledLinkUp(bool up);
