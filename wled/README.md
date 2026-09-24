# WLED for the light: quicker reconnects

`prebuilt/WLED_16.0.1_ESP32_wledlink.bin` is the official WLED 16.0.1 for plain ESP32 boards (the "ESP32"
release, with the AudioReactive usermod, like the release file on GitHub) with one change, in
[`wledlink.patch`](wledlink.patch):

Once WLED has been connected, it looks for its Wi-Fi every 5 s instead of every 18 s when the network goes
away. Here the network is the bridge, which disappears whenever the laptop sleeps (it cuts USB power) or the
bridge is unplugged or restarted. With stock WLED the lights can take up to about 20 s to come back after that;
with this build it's a few seconds. The WLED hotspot settings behave as before: when WLED is set to open its
hotspot whenever it loses the bridge, it keeps the stock 18 s so the hotspot still opens.

Nothing else changes, and settings and presets stay as they are. Its version code is 2606301 (official
16.0.1: 2606300), which is how you can tell it apart in WLED's Info page.

- **Install**, through the link: `python pc\wledlink.py wled-update`. It uploads with WLED's own update page,
  waits for WLED to restart and puts the light back the way it was.
- **Back to stock WLED:** download `WLED_16.0.1_ESP32.bin` from the
  [16.0.1 release](https://github.com/wled/WLED/releases/tag/v16.0.1), then
  `python pc\wledlink.py wled-update path\to\WLED_16.0.1_ESP32.bin`.
- **Official WLED updates** replace this build. To keep quick reconnects on a newer version, change the tag
  in `build.ps1`, run it, and install the result with `wled-update`.
- **Rebuild:** `powershell -ExecutionPolicy Bypass -File wled\build.ps1` (needs git, Node.js and PlatformIO).
