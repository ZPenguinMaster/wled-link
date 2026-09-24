# WLED Link: WLED without campus Wi-Fi

Your WLED controller never touches URHome. A spare ESP32 plugged into the PC runs a tiny,
hidden Wi-Fi network just for the WLED board, and a small program on the PC makes WLED
show up at **http://127.0.0.2**. SignalRGB and your browser use that address as if WLED
were on a normal home network.

```
 SignalRGB ─┐                                    ┌──────────────────────┐
            ├─► 127.0.0.2 ─► wledlink.py ─USB──► │ bridge ESP32 (spare) │
 browser  ──┘   (this PC)                        └──────────┬───────────┘
                                                            │ hidden Wi-Fi "WLEDLink"
                                                 ┌──────────▼───────────┐
                                                 │ WLED ESP32 ─► strip  │
                                                 └──────────────────────┘
```

Everything WLED does keeps working: the full web UI (including settings and firmware updates),
presets, the JSON API, and SignalRGB's realtime colours.

| Folder | What it is |
|---|---|
| `bridge/` | Firmware for the spare ESP32 (PlatformIO project). Prebuilt image in `bridge/prebuilt/`. |
| `pc/wledlink.py` | The PC side. Needs only Python + pyserial. |
| `pc/install-autostart.ps1` | Starts `wledlink.py` in the background at every login. |
| `pc/tests/` | End-to-end test with a simulated bridge and WLED (`python pc\tests\test_wledlink.py`). |

**Bridge Wi-Fi:** network `WLEDLink` (hidden), password `quartz-basil-1769` (set in `bridge/src/config.h`).

---

## 1. Flash the bridge (one time)

1. Plug a **spare** ESP32 into the PC with a data USB cable.
2. Find its port: `python pc\wledlink.py ports` (it shows up as "Silicon Labs CP210x ... (COMx)").
3. Flash it:
   ```
   python pc\wledlink.py flash-bridge --port COMx
   ```
   The tool refuses to flash a board that answers as WLED, so you can't overwrite the lights by accident.
   If it gets stuck on `Connecting...`, hold the board's **BOOT** button until writing starts.

## 2. Start the link

```
python pc\wledlink.py
```
Open **http://127.0.0.2/__wledlink**. The status page walks you through the remaining steps and
shows what's connected. To have it start by itself at every login (in the background, no window),
double-click **`pc\install-autostart.cmd`**. The window stays open so you can read the result.
`install-autostart.cmd -Uninstall` removes it again, and `python pc\wledlink.py stop` stops a
background copy.

The bridge's blue LED shows its state: a short blip every 2 s means it's waiting for the PC program,
steady blinking means the PC is connected but WLED hasn't joined, and solid (flickering with traffic)
means everything is connected.

## 3. Move WLED onto the bridge Wi-Fi

The network is hidden, so it won't appear in WLED's scan list; type the name in. It must be the
**only** network WLED knows. With more than one, WLED picks by scanning, and a hidden network never
shows up in a scan.

- **Over USB (easiest).** Plug the WLED board into the PC by USB (if it's also wired to the 24 V
  supply, unplug that first; USB powers the ESP32 fine, the strip just stays dark). Then run:
  ```
  python pc\wledlink.py wled-wifi --port COMy
  ```
  This uses WLED's built-in Improv protocol to set the network name and password. Put the board back afterwards.
- **In WLED's settings.** Config → WiFi Setup: network `WLEDLink`, password `quartz-basil-1769`,
  **delete URHome** from the list, and set Static IP to `0.0.0.0` if you had one. Save.

Also in WiFi Setup, keep **Disable WiFi sleep** checked (the default on ESP32). It keeps realtime colours smooth.

**WLED's backup hotspot:** by default, if WLED starts up without the bridge (say the PC is off when the
lights get power), it opens its own hidden `WLED-AP` hotspot at http://4.3.2.1. The status page and
`python pc\wledlink.py wled-hotspot off|boot|fallback` choose when that happens: never, only in that boot
case (WLED's default), or whenever WLED loses the bridge. With it off, the way to fix WLED's Wi-Fi if
it ever gets lost is over USB with `wled-wifi`.

## Controlling it from your phone over Bluetooth

The bridge also runs a Bluetooth light control, named "Lamp". Your phone talks to it over Bluetooth
and stays on its normal Wi-Fi, and the bridge passes the commands on to WLED.
[pc/phone/index.html](pc/phone/index.html) is the control page: On/Off, the three whites at 100%,
Back to SignalRGB, and brightness.

**Locked to your phone:** the control only works over an encrypted link paired with a 6-digit PIN, shown
on the status page. On top of that, the bridge only accepts phones that paired while you had pairing open:
2 minutes, started from the status page (or `python pc\wledlink.py phone pair`, or by holding the bridge's
BOOT button for 2 s, when its LED blinks fast). Anyone else is disconnected.

**One-time setup on the iPhone:**
1. Host the page. Web Bluetooth only works on `https://` pages, so upload `pc/phone/index.html` to a
   free GitHub Pages site: create a public repo such as `lights` on github.com, *Add file → Upload files*
   with `index.html`, then *Settings → Pages → Deploy from branch → main*. The page is then at
   `https://<your-username>.github.io/lights/`. It contains no PIN or password.
2. Install **Bluefy** from the App Store (Safari can't use Bluetooth) and open that address in it.
   Add it to Bluefy's favourites.
3. On the laptop's status page, press **Let a new phone pair**. In Bluefy tap **Connect**, choose **Lamp**,
   and type the PIN when iOS asks.

After that, open the page in Bluefy, tap Connect → Lamp, and use the buttons. This works with the laptop
off as long as the bridge still gets USB power. To start over, use *Forget paired phones* (and remove
"Lamp" under iPhone Settings → Bluetooth); `python pc\wledlink.py phone pin 482913` changes the PIN.

## Controlling it from your phone over Wi-Fi

The bridge keeps its hidden Wi-Fi running whenever it has power. The PC program isn't needed for
this, so your phone can talk to WLED directly even with the laptop shut:

1. On the phone, add the Wi-Fi network by hand (it's hidden): name `WLEDLink`, WPA2, password
   `quartz-basil-1769`. Turn **auto-join / auto-connect off**, so the phone only uses this no-internet
   network when you pick it. On Android, answer **Stay connected** if it warns about no internet.
2. Open **http://192.168.77.2** (WLED's full UI; add it to your home screen), or use the **WLED app**,
   which finds the light by itself.
3. Switch back to your normal Wi-Fi when you're done.

This only works while the bridge has USB power. To check, shut the laptop down and look at the bridge:
if its blue LED keeps blipping every 2 s, you're set. If it goes dark, the laptop cuts USB power when
off. Either plug the bridge into the laptop's always-powered USB port (often marked with a battery or
lightning icon, sometimes a BIOS option), or use `wled-hotspot fallback` and join WLED's own `WLED-AP`
network (http://4.3.2.1) instead.

If you only want the bridge's Wi-Fi running while the PC program runs: `python pc\wledlink.py bridge-config --wifi with-pc`.
Phone control with the laptop off stops working then.

## 4. SignalRGB

Lighting Services → **WLED** → *Discover WLED device by IP* → `127.0.0.2` → **Discover**, then **Link**.
Delete the old entry that pointed at the URHome address. Nothing else changes: SignalRGB streams
to 127.0.0.2:21324 and `wledlink.py` forwards it.

## 5. Switching between SignalRGB and white light

While SignalRGB is streaming, WLED follows it and ignores its own effects. To take over for white:

- **Status page:** *Neutral white 100%* (warm and cool white LEDs both at full), *Cool white 100%*,
  *Warm white 100%*, and *Back to SignalRGB*. The RGB LEDs stay off for all three.
- **WLED's own UI** (http://127.0.0.2): it shows *Override once* / *Override until reboot* buttons while
  SignalRGB is streaming. Pick a white preset, then press the star in the top-left to go back to SignalRGB.
- **Command line / shortcuts:**
  ```
  python pc\wledlink.py white                     # neutral: both white channels at full
  python pc\wledlink.py white cool                # or: warm
  python pc\wledlink.py white warm --brightness 60
  python pc\wledlink.py signalrgb                 # hand control back
  python pc\wledlink.py off
  ```

Both whites at full needs WLED's **CCT additive blending** at 100% (Config → LED Preferences). At the
default 0%, the middle of the white range gives each channel only half. The first press of *Neutral*
sets it for you and leaves every other setting alone. The white buttons also adapt to WLED's
"Auto-calculate white channel" setting, so they light only the white LEDs whether that's None, Dual
or Accurate.

Your strip is a WS2811 dual-IC RGBCCT (one RGB chip + one warm/cool white chip per 63 mm segment).
BTF-Lighting's recommended WLED LED setup for it is **FW1906 GRBCW**, colour order **RGB**, with
warm/cool white swapped. A 5 m strip has 80 of those segments. If effects look squashed, or only part
of a SignalRGB gradient reaches the strip, check the LED count in WLED's LED Preferences.

## Commands

| Command | Does |
|---|---|
| `python pc\wledlink.py` | Run the link (same as `run`). `--port COM5` skips auto-detection. |
| `python pc\wledlink.py status` | Show the bridge, the devices on its Wi-Fi, WLED, and traffic counters. |
| `python pc\wledlink.py stop` | Stop the running link (handy when it runs in the background). |
| `python pc\wledlink.py white [neutral\|cool\|warm]` | White LEDs at full, overriding SignalRGB. `--brightness 60` for less. |
| `python pc\wledlink.py signalrgb` / `off` | Hand control back to SignalRGB / turn the lights off. |
| `python pc\wledlink.py phone pair\|forget\|pin NNNNNN` | Bluetooth phone control: open pairing for 2 min, forget phones, change the PIN. |
| `python pc\wledlink.py wled-hotspot off\|boot\|fallback` | When WLED opens its own `WLED-AP` hotspot: never, only if it boots without the bridge, or whenever it loses the bridge. |
| `python pc\wledlink.py bridge-config --wifi always\|with-pc` | Bridge Wi-Fi on whenever it has power (default, needed for phone control with the PC off) or only while the program runs. |
| `python pc\wledlink.py ports` | List serial ports. |
| `python pc\wledlink.py bridge-config --password NEW --channel 6` | Change the bridge Wi-Fi (the bridge restarts). Give WLED the new details too. |
| `python pc\wledlink.py flash-bridge --port COMx` | Flash `bridge/prebuilt/wledlink-bridge.bin`. |
| `python pc\wledlink.py wled-wifi --port COMy` | Point a USB-connected WLED board at the bridge Wi-Fi. |

To rebuild the firmware after editing `bridge/src/config.h`: `cd bridge`, then `python -m platformio run -t upload --upload-port COMx`.
Flashing the prebuilt image resets any `bridge-config` changes to the defaults in `config.h`.

## Troubleshooting

- **The status page says what's missing.** Logs are in `%LOCALAPPDATA%\wledlink\wledlink.log`.
- **"Cannot listen on 127.0.0.2:80"**: another copy is already running, maybe in the background. `python pc\wledlink.py stop` ends it.
- **WLED joined but isn't reachable**: WLED still has a static IP from the campus network. Set it to `0.0.0.0`.
- **Neutral white flickers, or the power supply clicks off**: both white channels at full is the most the whites
  can draw. BTF recommends a 24 V 4 A (96 W) supply for the 5 m strip. With a smaller one, use less brightness
  (`white neutral --brightness 70`).
- **Colours stutter**: the bridge picks the quietest of channels 1/6/11 at each start. In a crowded
  dorm you can pin one: `python pc\wledlink.py bridge-config --channel 11`.
- **After the PC sleeps**: the bridge loses USB power and WLED keeps its last look. On wake everything
  reconnects on its own within about half a minute.
- **Reflashing the bridge while the link runs** is fine. `flash-bridge` pauses the link while it works.

## Purdue's rules

Purdue's [ResNet Acceptable Use Policy](https://service.purdue.edu/TDClient/32/Purdue/KB/Article/13/What-is-the-ResNet-Acceptable-Use-Policy-AUP)
says residents won't use a router "or any other hotspot" in their room, because of interference with PAL.
The [residence-hall router article](https://service.purdue.edu/TDClient/32/Purdue/KB/Article/6/How-do-I-configure-my-router-for-use-in-the-Residence-Halls)
allows personal routers only with special permission from ResNet. Continued use gets reported to University
Residences, and IT can cut the room's connection until the device is removed.

The bridge's network is a small hotspot, even though it's hidden and not connected to Purdue's network or the
internet. Hiding it keeps it out of everyone's Wi-Fi list, and the bridge uses little airtime: 8.5 dBm and
one beacon every ~0.3 s. (`bridge-config --wifi with-pc` also limits it to when `wledlink.py` is running.)
It is still a radio network, though, and Purdue's access points can detect hidden networks. Hiding it
doesn't make it allowed. To be fully in the clear, either ask ResNet
for permission (the router article says they can grant it), or switch the link to ESP-NOW. ESP-NOW is a direct
ESP32-to-ESP32 radio link with no network and no beacons, much like a Bluetooth gadget. It needs a custom WLED
build on the light controller.

## How it works

The bridge firmware hosts a hidden WPA2 access point on its own subnet (192.168.77.x) that isn't connected to
anything else, and tunnels TCP and UDP over the USB serial link at 921600 baud. Frames are COBS-encoded with a
CRC and a sequence number. Any corruption resets the session instead of silently mangling data, and each
tunnelled connection has flow control so large uploads (like WLED firmware updates) can't overrun the ESP32.
`wledlink.py` serves 127.0.0.2:80 and the WLED UDP ports, finds WLED among the devices on the bridge network,
and rewrites the `"ip"` field in WLED's `/json` info. SignalRGB reads that field and would otherwise try to
reach the bridge-side address directly. It also hands WLED the PC's clock now and then, since WLED has no
internet for NTP here.

**Security.** The link only listens on this PC's loopback address, so other machines can't reach it. It
also refuses requests that come from other websites open in your browser: cross-site requests, POSTs
from foreign pages, and DNS-rebinding tricks. Those would otherwise be able to change settings or read
the status page, which shows the Wi-Fi password and the Bluetooth PIN. The bridge's Wi-Fi is WPA2, and
its Bluetooth control needs a PIN-paired, encrypted link from a phone you approved. This repository
contains the bridge's Wi-Fi password (`bridge/src/config.h` and the prebuilt image), so keep it private.

Why URHome was likely flaky (educated guess, not verified against Purdue's setup): campus device networks
commonly isolate clients from each other and from PCs on the main network, drop idle or low-signal clients,
and are tuned for phones and consoles rather than small ESP32 radios. The bridge sidesteps all of that
because the WLED board only ever talks to a network you control.
