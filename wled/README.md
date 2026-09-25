# WLED for the light: the WLED Link build

`prebuilt/WLED_16.0.1_ESP32_wledlink.bin` is the official WLED 16.0.1 for plain ESP32 boards (the "ESP32"
release, with the AudioReactive usermod, like the release file on GitHub) plus two things:

- **The WLED Link usermod** ([`usermod/`](usermod)): the light's end of the ESP-NOW link to the bridge
  (the protocol is [`usermod/link_proto.h`](usermod/link_proto.h), the same file the bridge uses), and
  **remember the look**, which saves the light's look a few seconds after each change and puts it back after
  a power cut.
- **A few changes to WLED itself** ([`wledlink.patch`](wledlink.patch)):
  - an ESP-NOW-only radio mode: no Wi-Fi network and no hotspot, just ESP-NOW on the bridge's channel;
  - once WLED has been connected, it looks for its Wi-Fi every 5 s instead of every 18 s (Wi-Fi mode: the
    lights come back within seconds after the bridge restarts or the laptop wakes up);
  - fixes in how WLED drives its ESP-NOW library: each reconnect attempt used to lose about 1.2 KB of memory
    (without its network WLED ran out within minutes and restarted), and a fixed channel could be ignored;
  - connections relayed by the usermod (they come from inside the light) count as local, so WLED's own
    update and settings pages work over ESP-NOW;
  - while WLED waits to restart it keeps the usermod running, so its last answer (such as "Update
    successful") still gets back over the radio.

Its version code is 2606309 (official 16.0.1: 2606300), which is how you can tell it apart in WLED's Info
page. Settings and presets stay as they are.

- **Install**, through the link: `python pc\wledlink.py wled-update`. It uploads with WLED's own update page,
  waits for WLED to restart and puts the light back the way it was. Over ESP-NOW that takes about a minute.
- **Install over USB** (the light's board plugged into the PC, on COMx; keeps its settings and presets): the
  first command makes it start from the first app slot again (after an update over the air it runs from the
  second), the second puts the build there.
  ```
  python -m esptool --chip esp32 --port COMx erase-region 0xe000 0x2000
  python -m esptool --chip esp32 --port COMx write-flash 0x10000 wled\prebuilt\WLED_16.0.1_ESP32_wledlink.bin
  ```
- **Back to stock WLED:** first `python pc\wledlink.py link wifi` (stock WLED can't do ESP-NOW, so the light
  has to be on the bridge's Wi-Fi), then download `WLED_16.0.1_ESP32.bin` from the
  [16.0.1 release](https://github.com/wled/WLED/releases/tag/v16.0.1) and
  `python pc\wledlink.py wled-update path\to\WLED_16.0.1_ESP32.bin`.
- **Official WLED updates** replace this build (and with it the ESP-NOW link: switch to Wi-Fi first, as above).
  To keep it on a newer version, change the tag in `build.ps1`, run it, and install the result with
  `wled-update`.
- **Rebuild:** `powershell -ExecutionPolicy Bypass -File wled\build.ps1` (needs git, Node.js and PlatformIO).
  It checks out WLED next to this folder (`wled-src`), applies the patch, adds the usermod and builds.
