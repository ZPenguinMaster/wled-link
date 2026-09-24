#pragma once
#include <Arduino.h>

// Keeps a persistent WebSocket to WLED so commands from the phone or the PC are applied without
// setting up a TCP connection or HTTP request each time, and tracks WLED's state for everyone.
//
// WLED pushes its state to WebSocket clients at most once a second, so after forwarding a command
// the bridge immediately publishes the state the command will produce ("optimistic" state), and
// WLED's own push corrects it if anything differs. WLED pushes nothing when a SignalRGB stream starts
// or stops, so "live" comes from the stream the bridge forwards instead.

void wledBegin();
bool wledSend(const char* json, size_t len);       // queue a WLED JSON state command
uint32_t wledStateSeq();                            // changes whenever the state below changes
size_t wledStateJson(char* out, size_t cap);        // {"n":..,"ws":1,"on":1,"bri":..,"lor":..,"live":..,"ps":..,"fx":..,"cct":..,"col":[r,g,b,w]}
bool wledConnected();
void wledNoteStream();  // the bridge just passed realtime LED data (SignalRGB) on to WLED
void wledCheckLink();   // a device joined or left the Wi-Fi: make sure the WebSocket still reaches WLED
