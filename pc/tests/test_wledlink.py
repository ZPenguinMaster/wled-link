"""End-to-end test: fake WLED <- fake bridge <- (socket:// "serial") <- wledlink.py <- HTTP/UDP clients.

Runs the real wledlink.py in a subprocess, pointed at the software bridge. No hardware needed.
    python pc/tests/test_wledlink.py
"""

import asyncio
import hashlib
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

import wledlink as wl  # noqa: E402
from fake_bridge import FakeBridge  # noqa: E402
from fake_wled import BIG, HTML, FakeWled  # noqa: E402

LISTEN = "127.0.0.9"
CRLF = chr(13) + chr(10)
HTTP_PORT = 18080
BASE = f"http://{LISTEN}:{HTTP_PORT}"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not ok else ""))


def unit_tests():
    print("framing")
    check("crc16 matches CRC-16/CCITT-FALSE", __import__("binascii").crc_hqx(b"123456789", 0xFFFF) == 0x29B1)
    cases = [b"", b"\x00", b"\x00\x00", b"\x01" * 253, b"\x01" * 254, b"\x01" * 255, b"\x01" * 254 + b"\x00",
             b"\x00" + b"\x01" * 254, bytes(range(256)) * 3]
    rng = random.Random(1)
    cases += [bytes(rng.choice([0, 0, 1, 2, 255]) for _ in range(rng.randint(0, 2000))) for _ in range(300)]
    ok = all(wl.cobs_decode(wl.cobs_encode(c)) == c and 0 not in wl.cobs_encode(c) for c in cases)
    check("COBS round-trips (edge cases + 300 random)", ok)
    frame = wl.encode_frame(0x91, 7, b"\x03hello\x00world")
    check("frame round-trip", wl.decode_frame(frame[:-1]) == (0x91, 7, b"\x03hello\x00world"))
    damaged = bytearray(frame[:-1])
    damaged[4] ^= 0x10
    check("damaged frame rejected", wl.decode_frame(bytes(damaged)) is None)

    class VanishingPort:  # what the bridge's port does while the PC wakes up and USB re-enumerates
        in_waiting = 0
        def read(self, n):
            raise wl.serial.SerialException("ClearCommError failed (PermissionError(13, 'Access is denied.'))")
        def close(self):
            raise OSError("already gone")
    real_open = wl.open_serial
    wl.open_serial = lambda name, baud: VanishingPort()
    try:
        survived = wl.PortScanner().scan("COM99", wl.DEFAULT_BAUD, None) is None
    except Exception:
        survived = False
    finally:
        wl.open_serial = real_open
    check("a port that fails mid-probe doesn't stop the search for the bridge", survived)


def http(method, path, body=None, headers=None, timeout=15):
    req = urllib.request.Request(BASE + path, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


async def ahttp(*args, **kw):
    return await asyncio.to_thread(http, *args, **kw)


async def status():
    _, _, body = await ahttp("GET", "/__wledlink/status.json")
    return json.loads(body)


async def wait_for(pred, timeout, what):
    deadline = time.monotonic() + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            if await pred():
                return True
        except Exception as exc:  # daemon not up yet, etc.
            last_exc = exc
        await asyncio.sleep(0.25)
    print(f"    timed out waiting for {what} ({last_exc})")
    return False


async def wled_ready():
    s = await status()
    return s["wled"] is not None and s["bridge"]["state"] == "ready"


async def json_info_ok():
    code, _, body = await ahttp("GET", "/json/info/", timeout=5)
    return code == 200 and json.loads(body).get("ip") == LISTEN


async def raw_upgrade_session():
    reader, writer = await asyncio.open_connection(LISTEN, HTTP_PORT)
    writer.write(f"GET /ws HTTP/1.1\r\nHost: {LISTEN}:{HTTP_PORT}\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n".encode())
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
    echoes = []
    for msg in (b"hello", b"again after a pause"):
        writer.write(msg)
        await writer.drain()
        echoes.append(await asyncio.wait_for(reader.readexactly(len(msg)), 5))
        await asyncio.sleep(1.0)
    writer.close()
    return head, echoes


async def e2e():
    wled = FakeWled()
    await wled.start()
    bridge = FakeBridge(wled.http_addr, wled.udp_addr)
    await bridge.start()
    logdir = tempfile.mkdtemp(prefix="wledlink-test-")
    os.environ["WLEDLINK_HOME"] = logdir  # CLI calls too: never the real %LOCALAPPDATA%\\wledlink (link key!)
    env = dict(os.environ, WLEDLINK_HOME=logdir)
    log_path = os.path.join(logdir, "daemon.out")
    out = open(log_path, "w")
    daemon = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "..", "wledlink.py"), "--listen", LISTEN, "--http-port", str(HTTP_PORT),
         "run", "--port", f"socket://127.0.0.1:{bridge.port}", "-v"],
        stdout=out, stderr=subprocess.STDOUT, env=env)
    try:
        print("startup")
        check("daemon finds bridge and WLED", await wait_for(wled_ready, 20, "WLED to become reachable"))
        s = await status()
        check("status reports bridge Wi-Fi", s["bridge"].get("ssid") == "WLEDLink" and s["bridge"].get("password"))
        check("status reports WLED details", s["wled"] and s["wled"]["leds"] == 80 and s["wled"]["name"] == "Test WLED")
        check("listening on realtime UDP port", 21324 in s["udpListening"], s["udpListening"])

        print("what SignalRGB does")
        code, _, body = await ahttp("GET", "/json/info/", headers={"Accept": "application/json"})
        info = json.loads(body)
        check("GET /json/info/ answers with brand WLED", code == 200 and info.get("brand") == "WLED")
        check("info.ip rewritten to the loopback address", info.get("ip") == LISTEN, info.get("ip"))
        code, _, body = await ahttp("GET", "/json/")
        full = json.loads(body)
        check("GET /json/ rewritten too", code == 200 and full["info"]["ip"] == LISTEN and "state" in full)
        code, _, body = await ahttp("GET", "/json/si")
        si = json.loads(body)
        check("chunked /json/si decoded and rewritten", code == 200 and si["info"]["ip"] == LISTEN)
        code, _, _ = await ahttp("POST", "/json/state/", json.dumps({"on": True, "bri": 255, "live": False}).encode(),
                                 {"Content-Type": "application/json"})
        check("POST /json/state reaches WLED", code == 200 and wled.posts and wled.posts[-1].get("bri") == 255)
        led_count = 80
        sent = []
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for frame in range(30):
            pkt = bytes([4, 2, 0, 0]) + bytes((frame + i) & 0xFF for i in range(led_count * 3))
            sent.append(pkt)
            sock.sendto(pkt, (LISTEN, 21324))
            await asyncio.sleep(1 / 60)
        sock.close()
        await asyncio.sleep(0.5)
        got = [d for _, d in wled.udp_packets]
        check("DNRGB colour packets arrive at WLED", len(got) >= 28 and all(g in sent for g in got), f"{len(got)}/30")
        s = await status()
        check("the stream shows up as live at once", s["light"].get("live") == 1, s["light"])

        print("web UI traffic")
        code, headers, body = await ahttp("GET", "/", headers={"Accept": "text/html"})
        check("GET / passes the UI through byte-for-byte", code == 200 and body == HTML)
        check("responses are marked Connection: close", headers.get("Connection", "").lower() == "close")
        t0 = time.monotonic()
        code, _, body = await ahttp("GET", "/big")
        check("300 KB download intact", code == 200 and body == BIG, f"{len(body)} bytes")
        print(f"         ({len(body) / (time.monotonic() - t0) / 1024:.0f} KB/s over the fake link)")
        upload = os.urandom(200_000)
        code, _, body = await ahttp("POST", "/echo", upload, {"Content-Type": "application/octet-stream"})
        reply = json.loads(body) if code == 200 else {}
        check("200 KB upload intact", reply.get("sha") == hashlib.sha256(upload).hexdigest(), reply)
        check("upload never exceeded the bridge's window", bridge.window_violations == 0, bridge.window_violations)
        codes = await asyncio.gather(*(ahttp("GET", "/slow") for _ in range(14)))
        check("14 parallel requests share 8 bridge slots", all(c[0] == 200 for c in codes), [c[0] for c in codes])
        head, echoes = await raw_upgrade_session()
        check("websocket-style upgrade stays open and echoes", head.startswith(b"HTTP/1.1 101") and echoes == [b"hello", b"again after a pause"])
        await asyncio.sleep(0.5)
        s = await status()
        check("no tunnel connections leaked", s["openConnections"] == 0 and wled.active == 0,
              f"daemon {s['openConnections']}, wled {wled.active}")
        check("PC clock was sent to WLED", any("time" in p for p in wled.posts))

        print("pages")
        code, _, body = await ahttp("GET", "/__wledlink")
        check("control page served", code == 200 and b"PC Sync" in body and b"EventSource" in body)
        code, _, body = await ahttp("GET", "/__wledlink/phone")
        check("phone page served", code == 200 and b"Connect to your light" in body)
        s = await status()
        settings = (s["wled"] or {}).get("settings") or {}
        check("WLED settings read for the status page", settings.get("readable") and settings.get("apBehavior") == 0, settings)
        check("serial link sped up to 2 Mbaud", s.get("baud") == 2_000_000 and bridge.baud_requests == [2_000_000],
              (s.get("baud"), bridge.baud_requests))
        code, _, body = await ahttp("GET", "/__wledlink/presets")
        check("WLED presets listed", code == 200 and [p["name"] for p in json.loads(body)] == ["Chill", "Party"], body[:200])

        print("instant path")
        events = await Events.open()
        first = await events.next()
        check("live updates stream the status right away", first and "light" in first and first["light"].get("ws") == 1)
        check("a stopped stream stops counting as live",
              ((first or {}).get("light") or {}).get("live") == 0
              or await events.until(lambda st: st["light"].get("live") == 0, 4) is not None)
        t0 = time.monotonic()
        code, _, _ = await ahttp("POST", "/__wledlink/api/state", json.dumps({"on": False}).encode())
        update = await events.until(lambda st: st["light"].get("on") == 0, 2)
        latency = (time.monotonic() - t0) * 1000
        check("a command goes straight to the bridge (no HTTP round trip to WLED)",
              code == 200 and bridge.wled_cmds and bridge.wled_cmds[-1] == {"on": False})
        check("the new state is pushed to open pages at once", update is not None, f"{latency:.0f} ms")
        print(f"         (click -> page updated in {latency:.0f} ms through the simulated link)")
        check("WLED receives the forwarded command", await until(lambda: wled.state.get("on") is False, 3))
        events.close()

        print("white buttons")
        led, gamma = wled.cfg["hw"]["led"], wled.cfg["light"]["gc"]
        led["rgbwm"] = 3  # a global auto-white override (Dual) that a careless partial update would wipe
        reply = await white("neutral")
        cmd = await last_cmd(bridge)
        seg = cmd.get("seg", {})
        check("neutral: both white channels at full (CCT 127, white only, 100%, no fade)",
              reply.get("ok") and seg.get("cct") == 127 and seg["col"][0] == [0, 0, 0, 255]
              and cmd.get("bri") == 255 and cmd.get("lor") == 2 and cmd.get("tt") == 0, (reply, cmd))
        check("neutral: turned on 100% CCT additive blending", led["cb"] == 100 and any("blending" in n for n in reply["notes"]), led)
        check("settings change left gamma, auto-white and frame rate alone",
              gamma["col"] == 2.8 and gamma["bri"] == 1 and led["rgbwm"] == 3 and led["fps"] == 42, (gamma, led))
        check("settings change did not restart the LED outputs", wled.bus_reinits == 0)
        posts = len(wled.cfg_posts)
        reply = await white("cool", fresh=False)
        cmd = await last_cmd(bridge)
        check("cool: cool white only (CCT 255), from cached settings", reply.get("ok") and cmd["seg"]["cct"] == 255
              and len(wled.cfg_posts) == posts, (reply, cmd))
        rc = await cli("white", "warm", "--brightness", "50")
        cmd = await last_cmd(bridge)
        check("'wledlink.py white warm' command", rc.returncode == 0 and cmd["seg"]["cct"] == 0
              and abs(cmd["bri"] - 128) <= 1, rc.stdout + rc.stderr)
        led["rgbwm"] = 2  # Accurate: white has to be asked for as full RGB
        await white("neutral")
        check("adapts to 'Accurate' auto-white (RGB white -> white LEDs)", (await last_cmd(bridge))["seg"]["col"][0] == [255, 255, 255, 0])
        led["rgbwm"] = 1
        reply = await white("neutral")
        await last_cmd(bridge)
        check("explains 'Brighter' auto-white mixes RGB in", any("Brighter" in n for n in reply["notes"]))
        led["rgbwm"], led["cb"], wled.cfg_locked = 3, 0, True
        reply = await white("neutral")
        check("PIN-locked settings: still switches to white and says how to finish",
              reply.get("ok") and (await last_cmd(bridge))["seg"]["cct"] == 127 and any("100%" in n for n in reply["notes"]), reply)
        wled.cfg_locked = False
        code, _, body = await ahttp("POST", "/__wledlink/api/wled-transition", json.dumps({"instant": True}).encode())
        tr, sent = wled.cfg["light"]["tr"], wled.cfg_posts[-1]["light"]["tr"]
        check("instant changes: WLED's default fade set to 0, the rest of its settings kept",
              code == 200 and tr.get("dur") == 0 and sent == {"mode": True, "dur": 0, "pal": 0, "rpc": 5}
              and gamma["col"] == 2.8, (sent, body[:200]))
        rc = await cli("wled-hotspot", "fallback")
        check("'wled-hotspot fallback' makes WLED open its hotspot whenever it loses the bridge",
              rc.returncode == 0 and wled.cfg["ap"]["behav"] == 1 and gamma["col"] == 2.8 and led["rgbwm"] == 3, rc.stdout + rc.stderr)
        rc = await cli("wled-hotspot", "off")
        check("'wled-hotspot off' turns WLED's hotspot off", rc.returncode == 0 and wled.cfg["ap"]["behav"] == 3, rc.stdout + rc.stderr)
        s = await status()
        check("status reports the hotspot mode", s["wled"]["settings"].get("apBehavior") == 3)
        check("status reports the bridge Wi-Fi mode", s["bridge"].get("wifiWithPc") is False, s["bridge"])
        rc = await cli("signalrgb")
        check("'wledlink.py signalrgb' command hands control back", rc.returncode == 0 and (await last_cmd(bridge)).get("lor") == 0
              and await until(lambda: wled.state.get("lor") == 0, 3), rc.stdout + rc.stderr)
        cmd = await last_cmd(bridge)
        check("handing back restarts WLED's live session and blanks its own colour (no leftover LEDs)",
              cmd.get("live") is False and cmd["seg"]["col"][0] == [0, 0, 0, 0] and cmd["seg"]["fx"] == 0, cmd)
        rc = await cli("status")
        check("'wledlink.py status' command", rc.returncode == 0 and "Test WLED" in rc.stdout, rc.stdout + rc.stderr)

        print("browser security")
        code, _, _ = await ahttp("GET", "/__wledlink/status.json", headers={"Host": "evil.example"})
        check("DNS-rebinding style Host header refused", code == 403)
        code, _, _ = await ahttp("GET", "/json/info", headers={"Sec-Fetch-Site": "cross-site"})
        check("cross-site request from another website refused", code == 403)
        posts = len(wled.posts)
        code, _, _ = await ahttp("POST", "/json/state", b'{"on":false}',
                                 {"Content-Type": "application/json", "Origin": "http://evil.example"})
        check("POST from another website's page refused", code == 403 and len(wled.posts) == posts)
        code, _, _ = await ahttp("POST", "/__wledlink/api/white", b'{"tone":"cool"}',
                                 {"Origin": f"http://{LISTEN}:{HTTP_PORT}", "Sec-Fetch-Site": "same-origin"})
        check("the status page's own requests still work", code == 200)
        code, _, _ = await ahttp("POST", "/__wledlink/api/white", b'["not", "an object"]')
        check("malformed API input rejected cleanly", code == 400)

        print("recovery")
        sessions = bridge.sessions
        bridge.reboot()
        check("recovers after the bridge reboots", await wait_for(json_info_ok, 10, "recovery after reboot")
              and bridge.sessions > sessions)
        gaps_before = (await status())["stats"].get("resyncs", 0)
        bridge.drop_next = True
        await asyncio.sleep(2.5)  # the dropped frame is a PONG; the next one exposes the gap
        s = await status()
        check("detects a lost frame and resyncs", s["stats"].get("resyncs", 0) > gaps_before, s["stats"])
        check("works after the resync", await wait_for(json_info_ok, 10, "recovery after lost frame"))
        bridge.garbage(500)
        await asyncio.sleep(0.5)
        check("ignores garbage on the serial line", await wait_for(json_info_ok, 10, "recovery after garbage"))
        bridge.unplug()
        await asyncio.sleep(1)
        check("reconnects after the bridge is unplugged and replugged",
              await wait_for(json_info_ok, 20, "reconnect after unplug"))

        print("WLED leaving and coming back")
        bridge.set_stations([])
        check("notices WLED left", await wait_for(lambda: _no_wled(), 15, "WLED to disappear"))
        code, headers, _ = await ahttp("GET", "/", headers={"Accept": "text/html"})
        check("browser gets sent to the status page", code in (200, 302))
        code, _, body = await ahttp("GET", "/json/info/", headers={"Accept": "application/json"})
        check("SignalRGB gets a clean 503", code == 503 and "error" in json.loads(body))
        bridge.set_stations([{"mac": "aa:bb:cc:dd:ee:ff", "ip": "192.168.77.2", "rssi": -40}])
        check("finds WLED again when it rejoins", await wait_for(json_info_ok, 15, "WLED to come back"))

        print("bridge settings")
        payload = wl.config_payload({"ssid": "NewNet", "password": "longenough1", "channel": 6})
        code, _, body = await ahttp("POST", "/__wledlink/api/bridge-config",
                                    json.dumps({"ssid": "NewNet", "password": "longenough1", "channel": 6}).encode())
        check("bridge-config reaches the bridge", code == 200 and json.loads(body).get("ok") and bridge.config_payloads[-1] == payload)
        check("still works after the config reboot", await wait_for(json_info_ok, 10, "recovery after config"))

        print("ESP-NOW link")
        await ahttp("POST", "/__wledlink/api/rescan", b"{}")
        stored = lambda: json.loads(open(os.path.join(logdir, "link.json")).read()).get("key") if os.path.exists(os.path.join(logdir, "link.json")) else None
        check("the PC keeps a copy of the bridge's link key", await until(lambda: stored() == bridge.link_key.hex(), 15),
              stored())
        s = await status()
        check("stock WLED: the link stays on Wi-Fi and says what's needed",
              bridge.link_mode == "wifi" and "WLED Link build" in s["link"]["note"], s["link"])
        wled.wll = {"v": 1, "mode": "wifi", "run": "wifi", "link": False, "ch": 6, "kf": "", "restore": True}
        await ahttp("POST", "/__wledlink/api/rescan", b"{}")
        check("the light gets the key", await until(lambda: wled.wll_key == bridge.link_key, 15), wled.wll)
        s = await status()
        check("...and waits until the radio link works", bridge.link_mode == "wifi")
        bridge.link_up = True  # the light's usermod and the bridge now talk over ESP-NOW
        await ahttp("POST", "/__wledlink/api/rescan", b"{}")
        check("then switches to ESP-NOW by itself (the default)", await until(lambda: bridge.link_mode == "espnow", 15),
              bridge.link_requests)
        check("status shows the ESP-NOW link", await wait_for(lambda: _link_mode_is("espnow"), 10, "link status"))
        code, _, body = await ahttp("POST", "/__wledlink/api/link", json.dumps({"mode": "wifi"}).encode())
        check("the toggle switches back to Wi-Fi", code == 200 and await until(lambda: bridge.link_mode == "wifi", 10), body)
        rc = await cli("link")
        check("'wledlink.py link' shows the choice", rc.returncode == 0 and "Chosen : Wi-Fi network" in rc.stdout, rc.stdout + rc.stderr)
        old_key = bridge.link_key
        bridge.link_key = os.urandom(16)  # as if the bridge had been reset
        bridge.reboot()
        check("a reset bridge gets the light's key back from the PC", await until(lambda: bridge.link_key == old_key, 20))
        code, _, _ = await ahttp("POST", "/__wledlink/api/restore", json.dumps({"on": False}).encode())
        check("'remember the look' can be switched off", code == 200 and wled.wll["restore"] is False)
        bridge.link_up = False

        print("WLED firmware update")
        wled.state.update(on=True, bri=200, lor=2)
        firmware = os.path.join(HERE, "_fw_test.bin")
        with open(firmware, "wb") as f:
            f.write(bytes([0xE9]) + os.urandom(150_000))
        try:
            rc = await cli("wled-update", firmware)
        finally:
            os.remove(firmware)
        check("'wled-update' uploads the firmware through the link, intact",
              rc.returncode == 0 and wled.firmware_uploads == [150_001], (wled.firmware_uploads, rc.stdout + rc.stderr))
        check("after the restart the light gets its look back",
              wled.state.get("bri") == 200 and wled.state.get("lor") == 2, wled.state)
        wled.update_answers = False  # over ESP-NOW the light can restart before its answer is out
        with open(firmware, "wb") as f:
            f.write(bytes([0xE9]) + os.urandom(50_000))
        try:
            rc = await asyncio.wait_for(cli("wled-update", firmware), 60)
        finally:
            os.remove(firmware)
        wled.update_answers = True
        check("an update the light restarts from before answering still counts",
              rc.returncode == 0 and wled.firmware_uploads[-1] == 50_001 and "Done" in rc.stdout, rc.stdout + rc.stderr)

        print("stopping")
        rc = await cli("run", "--port", "socket://127.0.0.1:1")
        check("a second copy exits at once and leaves the running one alone (autostart watchdog)",
              rc.returncode == 0 and "already running" in rc.stdout and (await status())["bridge"]["state"] == "ready",
              rc.stdout + rc.stderr)
        rc = await cli("stop")
        exited = await asyncio.to_thread(lambda: daemon.wait(10) is not None)
        check("'wledlink.py stop' ends the running link", rc.returncode == 0 and exited, rc.stdout + rc.stderr)
    finally:
        daemon.terminate()
        with contextlib_suppress():
            daemon.wait(5)
        out.close()
        bridge.close()
        wled.close()
        if not all(ok for _, ok in results):
            print("\n--- daemon log ---")
            print(open(log_path).read()[-6000:])


async def white(tone, fresh=True):
    body = json.dumps({"tone": tone, "brightness": 100, "fresh": fresh}).encode()
    _, _, reply = await ahttp("POST", "/__wledlink/api/white", body)
    return json.loads(reply)


async def until(pred, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        await asyncio.sleep(0.02)
    return False


_seen_cmds = [0]


async def last_cmd(bridge):
    """The newest command the bridge received since the previous call (waits for it to arrive)."""
    await until(lambda: len(bridge.wled_cmds) > _seen_cmds[0], 3)
    _seen_cmds[0] = len(bridge.wled_cmds)
    return bridge.wled_cmds[-1] if bridge.wled_cmds else {}


class Events:
    """Reads the control page's server-sent events."""

    @classmethod
    async def open(cls):
        self = cls()
        self.reader, self.writer = await asyncio.open_connection(LISTEN, HTTP_PORT)
        self.writer.write(f"GET /__wledlink/events HTTP/1.1{CRLF}Host: {LISTEN}:{HTTP_PORT}{CRLF}{CRLF}".encode())
        await self.reader.readuntil((CRLF + CRLF).encode())
        return self

    async def next(self, timeout=3):
        try:
            while True:
                line = await asyncio.wait_for(self.reader.readline(), timeout)
                if not line:
                    return None
                if line.startswith(b"data: "):
                    return json.loads(line[6:])
        except (asyncio.TimeoutError, ValueError):
            return None

    async def until(self, pred, timeout):
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            st = await self.next(left)
            if st and pred(st):
                return st
        return None

    def close(self):
        self.writer.close()


async def cli(*args):
    """Runs a wledlink.py command without blocking the event loop the fakes live on."""
    cmd = [sys.executable, os.path.join(HERE, "..", "wledlink.py"), "--listen", LISTEN, "--http-port", str(HTTP_PORT), *args]
    return await asyncio.to_thread(subprocess.run, cmd, capture_output=True, text=True, timeout=30)


async def _link_mode_is(mode):
    s = await status()
    return s["link"].get("mode") == mode


async def _no_wled():
    s = await status()
    return s["wled"] is None


class contextlib_suppress:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return True


def main():
    unit_tests()
    asyncio.run(e2e())
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
