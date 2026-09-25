# WLED Link: WLED without campus Wi-Fi

Your WLED controller never touches URHome. A spare ESP32 plugged into the PC talks to the WLED
board directly over **ESP-NOW**, a device-to-device radio link with no network at all (or, if you
choose, over a tiny hidden Wi-Fi network), and a small program on the PC makes WLED show up at
**http://127.0.0.2**. SignalRGB and your browser use that address as if WLED were on a normal home
network.

```
 SignalRGB ─┐                                    ┌──────────────────────┐
            ├─► 127.0.0.2 ─► wledlink.py ─USB──► │ bridge ESP32 (spare) │ ◄── Bluetooth ── phone
 browser  ──┘   (this PC)                        └──────────┬───────────┘
                                                            │ ESP-NOW (or hidden Wi-Fi)
                                                 ┌──────────▼───────────┐
                                                 │ WLED ESP32 ─► strip  │
                                                 └──────────────────────┘
```

Everything WLED does keeps working: the full web UI (including settings and firmware updates),
presets, the JSON API, and SignalRGB's realtime colours. On top of that you get a control page on
the PC and a Bluetooth remote for your phone, and both react the moment you tap.

| Folder | What it is |
|---|---|
| `bridge/` | Firmware for the spare ESP32 (PlatformIO project). Prebuilt image in `bridge/prebuilt/`. |
| `pc/wledlink.py` | The PC side. Needs only Python + pyserial. |
| `pc/install-autostart.ps1` | Starts `wledlink.py` in the background at every login. |
| `pc/tests/` | End-to-end test with a simulated bridge and WLED (`python pc\tests\test_wledlink.py`). |
| `wled/` | The WLED Link build of WLED for the light: the ESP-NOW link, remembering its look, quicker reconnects. |

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

To update the bridge later, run the same command again. It pauses a running link while it works and
keeps the bridge's settings, Bluetooth PIN and paired phones (`--reset-settings` wipes them back to the
defaults in `bridge/src/config.h`). Older bridge firmware keeps working with the newest `wledlink.py`,
just without Bluetooth and with slower status updates.

## 2. Start the link

```
python pc\wledlink.py
```
Open **http://127.0.0.2/__wledlink**. That's the Lights control page: power, brightness, the modes
(PC Sync and the three whites), your WLED presets, phone pairing, and the settings. Until everything
is connected it also walks you through the remaining steps.

**Keep it running:** double-click **`pc\install-autostart.cmd`** once. It sets up a Windows scheduled
task that runs the link in the background (no window) and starts it at log-in, when the PC wakes up or is
unlocked, and, as a watchdog, within a minute whenever it isn't running for any reason. The window stays
open so you can read the result. `install-autostart.cmd -Uninstall` removes it again. `python
pc\wledlink.py stop` stops the running copy, but with the watchdog installed it comes back within a
minute; uninstall autostart to keep it off.

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
lights get power), it opens its own hidden `WLED-AP` hotspot at http://4.3.2.1. The *WLED backup hotspot*
setting on the control page and `python pc\wledlink.py wled-hotspot off|boot|fallback` choose when that happens: never, only in that boot
case (WLED's default), or whenever WLED loses the bridge. With it off, the way to fix WLED's Wi-Fi if
it ever gets lost is over USB with `wled-wifi`.

### No Wi-Fi network at all: ESP-NOW (the default)

Once the light runs the **WLED Link build of WLED** (`python pc\wledlink.py wled-update`, over the Wi-Fi
link you just set up), the PC pairs the light with the bridge and moves them onto ESP-NOW by itself.
The bridge's Wi-Fi network then goes off the air, and so does WLED's: nothing shows up in anyone's
Wi-Fi list, and the two ESP32s only transmit when there is something to say. Everything keeps working
the same way: the control pages, the phone, SignalRGB, WLED's own web UI and even its firmware updates.

The choice is **Connection to the light** in the control page's Settings (or `python pc\wledlink.py link
espnow|wifi`). The bridge decides and the light follows: the bridge tells it before it switches, and a
light that missed the switch finds the bridge again on its own (it listens on the bridge's channel, sweeps
the others now and then, and after 10 minutes without hearing the bridge checks for its Wi-Fi network).

**Private:** the link is encrypted (AES-128) and every frame is authenticated with a key only your bridge,
your light and this PC have, so nobody nearby can read it or send the light commands. The PC keeps a copy
of the key, so a bridge that gets reset or replaced is paired again automatically.

## Controlling it from your phone over Bluetooth

The bridge also runs a Bluetooth light control, named "Lamp". Your phone talks to it over Bluetooth
and stays on its normal Wi-Fi, and the bridge passes the commands on to WLED.
[pc/phone/index.html](pc/phone/index.html) is the phone's control page, built like the one on the PC:
power, a brightness slider, and tiles for PC Sync and the three whites at 100%. The bridge pushes every
change to the phone, so the page also shows what you did on the PC, and the other way round. It
reconnects by itself when you come back to it.

**Locked to your phone:** the control only works over an encrypted link paired with a 6-digit PIN, shown
on the control page. On top of that, the bridge only accepts phones that paired while you had pairing open:
2 minutes, started from the control page (or `python pc\wledlink.py phone pair`, or by holding the bridge's
BOOT button for 2 s, when its LED blinks fast). Anyone else is disconnected.

**One-time setup on the iPhone:**
1. Host the page. Web Bluetooth only works on `https://` pages, so upload `pc/phone/index.html` to a
   free GitHub Pages site: create a public repo such as `lights` on github.com, *Add file → Upload files*
   with `index.html`, then *Settings → Pages → Deploy from branch → main*. The page is then at
   `https://<your-username>.github.io/lights/`. It contains no PIN or password.
2. Install **Bluefy** from the App Store (Safari can't use Bluetooth) and open that address in it.
   Add it to Bluefy's favourites.
3. On the laptop's control page, press **Pair a phone**. In Bluefy tap **Connect**, choose **Lamp**,
   and type the PIN when iOS asks.

After that, open the page in Bluefy (tap Connect → Lamp if it doesn't connect by itself) and use it. This works with the laptop
off as long as the bridge still gets USB power. To start over, use *Forget phones* (and remove
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

- **Control page or phone:** *Neutral* (warm and cool white LEDs both at full), *Cool*, *Warm*, and
  *PC Sync* to hand back to SignalRGB. The RGB LEDs stay off for all three whites.
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

**Remember the look** (Settings, on by default, needs the WLED Link build): the light saves its look a few
seconds after every change and puts it back after a power cut: the same colour, brightness, on or off, and
whether it follows SignalRGB.

*PC Sync* also sets WLED's own colour to black and restarts its live session, which blanks the whole
strip before SignalRGB's next frame. LEDs that your SignalRGB layout leaves out stay dark instead of
keeping whatever colour they had, and when SignalRGB stops (PC asleep or off) the strip goes dark rather
than falling back to an old colour.

Both whites at full needs WLED's **CCT additive blending** at 100% (Config → LED Preferences). At the
default 0%, the middle of the white range gives each channel only half. The first press of *Neutral*
sets it for you and leaves every other setting alone. The white buttons also adapt to WLED's
"Auto-calculate white channel" setting, so they light only the white LEDs whether that's None, Dual
or Accurate.

Your strip is a WS2811 dual-IC RGBCCT (one RGB chip + one warm/cool white chip per 63 mm segment).
BTF-Lighting's recommended WLED LED setup for it is **FW1906 GRBCW**, colour order **RGB**, with
warm/cool white swapped. A 5 m strip has 80 of those segments. If effects look squashed, or only part
of a SignalRGB gradient reaches the strip, check the LED count in WLED's LED Preferences.

## Staying connected

Anything can be unplugged, restarted or put to sleep; everything finds its way back without you doing anything.
Times measured on this setup, over ESP-NOW:

| What happens | What you notice |
|---|---|
| WLED Link crashes or gets closed | The autostart watchdog starts it again within a minute (an internal error restarts it within 5 s). |
| The bridge is unplugged and plugged back in, or the laptop wakes up | Lights and controls are back about 5 s after the bridge gets power again, on any USB port. |
| The bridge restarts (update, settings change) | About 5 s. |
| WLED loses power | Back with the same look about 2 s after the power returns. |
| Switching between ESP-NOW and Wi-Fi | About 3 s to ESP-NOW, 5 s to Wi-Fi. |
| The bridge's radio stops getting its frames out (seen once, for hours, before this was handled) | The bridge notices within 2 s and starts its radio over. |
| The laptop sleeps | WLED keeps its last look; SignalRGB takes over again when the laptop wakes. |
| The phone page loses the bridge | It keeps trying for as long as the page is open, and reconnects the moment the bridge is back. |

All of this needs the light to run the **WLED Link build of WLED** in [`wled/`](wled/README.md) (installed with
`python pc\wledlink.py wled-update`). In Wi-Fi mode it also looks for the bridge's network every 5 s instead of
every 18 s after losing it, so the lights are back within about 6 s there too (up to about 20 s with stock WLED).
Official WLED updates replace it; `wled\build.ps1` rebuilds it on a newer version.

## Commands

| Command | Does |
|---|---|
| `python pc\wledlink.py` | Run the link (same as `run`). `--port COM5` skips auto-detection. |
| `python pc\wledlink.py status` | Show the bridge, how the light is connected, WLED, and traffic counters. |
| `python pc\wledlink.py stop` | Stop the running link. With autostart installed, its watchdog starts it again within a minute. |
| `python pc\wledlink.py white [neutral\|cool\|warm]` | White LEDs at full, overriding SignalRGB. `--brightness 60` for less. |
| `python pc\wledlink.py signalrgb` / `off` | Hand control back to SignalRGB / turn the lights off. |
| `python pc\wledlink.py phone pair\|forget\|pin NNNNNN` | Bluetooth phone control: open pairing for 2 min, forget phones, change the PIN. |
| `python pc\wledlink.py wled-hotspot off\|boot\|fallback` | When WLED opens its own `WLED-AP` hotspot: never, only if it boots without the bridge, or whenever it loses the bridge. |
| `python pc\wledlink.py bridge-config --wifi always\|with-pc` | Bridge Wi-Fi on whenever it has power (default, needed for phone control with the PC off) or only while the program runs. |
| `python pc\wledlink.py link [espnow\|wifi]` | How the bridge reaches the light: ESP-NOW (no Wi-Fi network, the default) or the hidden Wi-Fi network. Without a choice, shows the current one. |
| `python pc\wledlink.py ports` | List serial ports. |
| `python pc\wledlink.py bridge-config --password NEW --channel 6` | Change the bridge Wi-Fi (the bridge restarts). Give WLED the new details too. |
| `python pc\wledlink.py flash-bridge --port COMx` | Flash `bridge/prebuilt/wledlink-bridge.bin`, keeping the bridge's settings and paired phones (`--reset-settings` to wipe them). |
| `python pc\wledlink.py wled-wifi --port COMy` | Point a USB-connected WLED board at the bridge Wi-Fi. |
| `python pc\wledlink.py wled-update [file.bin]` | Install WLED firmware on the light through the link (default: the WLED Link build in `wled/prebuilt/`), keeping its look. |

To rebuild the firmware after editing `bridge/src/config.h`: `cd bridge`, `python -m platformio run` (this also
refreshes the prebuilt image), then `flash-bridge`. Settings saved with `bridge-config` take priority over
`config.h`; `flash-bridge --reset-settings` goes back to `config.h`.

## Troubleshooting

- **The control page says what's missing.** Logs are in `%LOCALAPPDATA%\wledlink\wledlink.log`.
- **"Cannot listen on 127.0.0.2:80"** in the log: another program is using that address. (A second copy of
  WLED Link just exits quietly, so that isn't it.)
- **WLED joined but isn't reachable**: WLED still has a static IP from the campus network. Set it to `0.0.0.0`.
- **The light doesn't link over ESP-NOW** (the control page says so after 20 s): usually the light lost its
  settings or runs other firmware. Switch to Wi-Fi (`python pc\wledlink.py link wifi`): the light checks for the
  bridge's network every 10 minutes, and once it's back the PC pairs it again, so you can switch back. With the
  light's board on USB, `python pc\wledlink.py wled-wifi --port COMy` points it at the bridge's network directly.
- **Neutral white flickers, or the power supply clicks off**: both white channels at full is the most the whites
  can draw. BTF recommends a 24 V 4 A (96 W) supply for the 5 m strip. With a smaller one, use less brightness
  (`white neutral --brightness 70`).
- **Colours stutter**: the bridge picks the quietest of channels 1/6/11 when it powers up (a restart keeps it). In a crowded
  dorm you can pin one: `python pc\wledlink.py bridge-config --channel 11`.
- **After the PC sleeps**: laptops like this one (S3 sleep) usually cut USB power, so the bridge restarts when it wakes. WLED
  keeps its last look meanwhile and rejoins a few seconds after the bridge is back (see *Staying connected*).
  Plugged into a USB port that stays powered in sleep (often marked with a battery or lightning icon) or a
  powered USB hub, the bridge never goes away: the lights pick up the moment the PC wakes, and phone control
  works while the laptop sleeps.
- **Didn't start, or stopped**: with autostart installed, the watchdog starts it within a minute. The log
  (`%LOCALAPPDATA%\wledlink\wledlink.log`) records unexpected errors with details.
- **Reflashing the bridge while the link runs** is fine. `flash-bridge` pauses the link while it works.

## Purdue's rules

Purdue's [ResNet Acceptable Use Policy](https://service.purdue.edu/TDClient/32/Purdue/KB/Article/13/What-is-the-ResNet-Acceptable-Use-Policy-AUP)
says residents won't use a router "or any other hotspot" in their room, because of interference with PAL.
The [residence-hall router article](https://service.purdue.edu/TDClient/32/Purdue/KB/Article/6/How-do-I-configure-my-router-for-use-in-the-Residence-Halls)
allows personal routers only with special permission from ResNet. Continued use gets reported to University
Residences, and IT can cut the room's connection until the device is removed.

That is why the link runs over **ESP-NOW** by default: a direct ESP32-to-ESP32 radio link like a Bluetooth
gadget's, with no network, no access point and no beacons. Neither ESP32 shows up in any Wi-Fi list, and they
only transmit while there is something to send (SignalRGB's colours, a tap, a keep-alive once a second), at
low power and at 12 Mbps, so each frame is on the air for a fraction of a millisecond.

The **Wi-Fi network** option is still there for later (say, if ResNet grants permission, which the router article
says they can): a hidden WPA2 network at 8.5 dBm with one beacon every ~0.3 s. Hidden isn't the same as allowed,
though: Purdue's access points can detect hidden networks. (`bridge-config --wifi with-pc` also limits it to
when `wledlink.py` is running.)

## How it works

The bridge firmware hosts a hidden WPA2 access point on its own subnet (192.168.77.x) that isn't connected to
anything else, and tunnels TCP and UDP over the USB serial link at 921600 baud, or 2 Mbaud when the board's
USB chip can take it (CP2102N and CH340 can, the older CP2102 can't; `wledlink.py` tries once and falls back). Frames are COBS-encoded with a
CRC and a sequence number. Any corruption resets the session instead of silently mangling data, and each
tunnelled connection has flow control so large uploads (like WLED firmware updates) can't overrun the ESP32.
`wledlink.py` serves 127.0.0.2:80 and the WLED UDP ports, finds WLED among the devices on the bridge network,
and rewrites the `"ip"` field in WLED's `/json` info. SignalRGB reads that field and would otherwise try to
reach the bridge-side address directly. It also hands WLED the PC's clock now and then, since WLED has no
internet for NTP here.

**The ESP-NOW link** (`wled/usermod/link_proto.h`, the same code in the bridge and in the light) carries
what the Wi-Fi network would: the PC's connections, SignalRGB's UDP and the control commands. Each side proves
it has the key in a short handshake that also makes fresh keys for the session; after that every frame is
encrypted, authenticated and numbered, so frames can't be read, forged or replayed. Connections and commands
go through a small reliable layer (acknowledged, resent when lost, in order); colours go as they are, because
a late frame is useless. In the light, the WLED Link usermod hands everything to WLED itself: connections to
its own web server and UDP to its own ports through the loopback interface, and commands straight to its
state, which it reports back at once.

**Why taps feel instant.** Every control path takes the shortest route and nothing waits for a reply:
- Commands from the PC page, the phone and the command line go straight to WLED's state: over ESP-NOW the
  link hands them to the usermod in the light, and over Wi-Fi the bridge keeps a WebSocket open to WLED. No
  new connection or HTTP request per tap.
- The moment the bridge forwards a command, it pushes the resulting state to the PC (over USB) and to the phone
  (as a Bluetooth notification). The pages redraw before WLED has even finished, and WLED's own report follows
  to confirm. Pages also redraw locally the instant you touch them.
- Every command carries `"tt":0`, so WLED switches without its usual 0.7 s fade. The *Instant changes*
  switch sets WLED's default fade to 0 as well, for everything else (WLED's own UI, presets).
- Brightness drags send at most 20 (PC) or 25 (phone) updates a second, always the newest, so a drag
  never builds a queue. On the phone, commands are Bluetooth writes without a response, and the
  bridge asks for a 15–30 ms connection interval (the fastest Apple allows) once your phone is paired.
- WLED doesn't announce when a SignalRGB stream starts or stops, but every stream passes through the
  bridge, so the bridge tells the pages itself.

A click on the PC page shows up on every open page in about 16 ms, and WLED's own confirmation follows about
10 ms later (measured over ESP-NOW: 27 ms from the click, 47 ms at worst).

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
