"""A stand-in for a WLED controller: just enough HTTP/UDP behaviour to test wledlink.

Like WLED's AsyncWebServer it answers one request per connection and then closes.
"""

import asyncio
import hashlib
import hmac
import json
import time

HTML = b"<!DOCTYPE html><html><head><title>WLED</title></head><body>fake WLED UI " + b"x" * 5000 + b"</body></html>"
BIG = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(300_000))


class FakeWled:
    def __init__(self, reported_ip="192.168.77.2"):
        self.reported_ip = reported_ip
        self.state = {"on": True, "bri": 128, "lor": 0, "seg": [{"id": 0, "fx": 9, "col": [[255, 0, 0, 0]], "cct": 127}]}
        self.posts = []          # bodies of POST /json/state
        self.udp_packets = []    # (addr, data)
        self.active = 0          # open HTTP connections
        self.requests = 0
        # /json/cfg, trimmed to the parts wledlink touches. "ins" is the LED output list.
        self.cfg = {
            "hw": {"led": {"total": 80, "maxpwr": 5000, "ledma": 0, "cct": False, "cr": False, "ic": False,
                           "cb": 0, "fps": 42, "rgbwm": 255, "ld": True,
                           "ins": [{"start": 0, "len": 80, "pin": [16], "order": 0, "type": 32, "rgbwm": 0}]}},
            "light": {"scale-bri": 100, "pal-mode": 0, "aseg": False, "gc": {"bri": 1, "col": 2.8, "val": 2.8},
                      "tr": {"mode": True, "dur": 7, "pal": 0, "rpc": 5}, "nl": {"mode": 1, "dur": 60, "tbri": 0, "macro": 0}},
            "ap": {"ssid": "WLED-AP", "pskl": 8, "chan": 1, "hide": 0, "behav": 0, "ip": [4, 3, 2, 1]},
        }
        self.cfg_locked = False  # behaves like a settings PIN is set
        self.cfg_posts = []
        self.bus_reinits = 0
        self.booted = time.monotonic() - 5000
        self.firmware_uploads = []  # sizes of the files posted to /update
        self.update_answers = True  # False: restarts before its answer is out, as it can over ESP-NOW
        self.wll = None             # the WLED Link usermod's info, once "installed" by a test
        self.wll_key = None

    def apply_cfg(self, doc):
        """Like WLED 0.15's deserializeConfig for these keys, including what it resets when a key is missing."""
        led, new_led = self.cfg["hw"]["led"], (doc.get("hw") or {}).get("led") or {}
        led["rgbwm"] = new_led.get("rgbwm", 255)            # global auto-white override: reset if missing
        for key in ("cct", "cr", "ic", "cb", "ld", "maxpwr"):
            if key in new_led:
                led[key] = new_led[key]
        if new_led.get("fps"):
            led["fps"] = new_led["fps"]
        if "ins" in new_led:
            self.bus_reinits += 1                            # would restart the LED outputs
        gc = (doc.get("light") or {}).get("gc") or {}
        self.cfg["light"]["gc"]["bri"] = gc.get("bri", 0)   # gamma correction: switched off if missing
        self.cfg["light"]["gc"]["col"] = gc.get("col", 0)
        for key, value in ((doc.get("light") or {}).get("tr") or {}).items():  # fade settings: kept if missing
            self.cfg["light"]["tr"][key] = value
        ap = doc.get("ap") or {}
        for key in ("chan", "hide", "behav"):
            if key in ap:
                self.cfg["ap"][key] = ap[key]

    def info(self):
        return {"ver": "0.15.0", "leds": {"count": 80, "rgbw": True, "cct": 1, "lc": 7}, "name": "Test WLED",
                "udpport": 21324, "live": False, "brand": "WLED", "product": "FOSS", "mac": "aabbccddeeff",
                **({"wll": self.wll} if self.wll is not None else {}),
                "ip": self.reported_ip, "uptime": int(time.monotonic() - self.booted), "release": "ESP32", "wifi": {"bssid": "00:00:00:00:00:00", "rssi": -40, "signal": 100, "channel": 6}}

    async def start(self):
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        self.http_addr = self.server.sockets[0].getsockname()[:2]
        loop = asyncio.get_running_loop()
        self.udp_transport, _ = await loop.create_datagram_endpoint(lambda: _Udp(self), local_addr=("127.0.0.1", 0))
        self.udp_addr = self.udp_transport.get_extra_info("sockname")[:2]

    def close(self):
        self.server.close()
        self.udp_transport.close()

    async def _handle(self, reader, writer):
        self.active += 1
        try:
            head = await reader.readuntil(b"\r\n\r\n")
            lines = head.decode("latin-1").split("\r\n")
            method, target, _ = lines[0].split(" ")
            headers = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:] if l)}
            body = b""
            if "content-length" in headers:
                body = await reader.readexactly(int(headers["content-length"]))
            self.requests += 1
            await self._route(method, target.split("?")[0], headers, body, reader, writer)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            self.active -= 1
            writer.close()

    async def _route(self, method, path, headers, body, reader, writer):
        def send(status, content, ctype="application/json", extra=""):
            writer.write(f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {len(content)}\r\n{extra}\r\n".encode() + content)

        if path in ("/json/info", "/json/info/"):
            send("200 OK", json.dumps(self.info()).encode())
        elif path in ("/json", "/json/"):
            send("200 OK", json.dumps({"state": self.state, "info": self.info(), "effects": ["Solid"], "palettes": ["Default"]}).encode())
        elif path in ("/json/si", "/json/si/"):
            # chunked on purpose, to exercise the rewrite path's decoder
            data = json.dumps({"state": self.state, "info": self.info()}).encode()
            chunks = b"".join(b"%x\r\n%s\r\n" % (len(data[i:i + 100]), data[i:i + 100]) for i in range(0, len(data), 100))
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks + b"0\r\n\r\n")
        elif path == "/presets.json":
            send("200 OK", json.dumps({"0": {}, "1": {"n": "Chill", "on": True}, "2": {"n": "Party", "playlist": {"ps": [1]}}}).encode())
        elif path in ("/json/cfg", "/json/cfg/"):
            if self.cfg_locked:
                send("401 Unauthorized", b'{"error":1}')
            elif method == "POST":
                self.cfg_posts.append(json.loads(body))
                self.apply_cfg(self.cfg_posts[-1])
                send("200 OK", b'{"success":true}')
            else:
                send("200 OK", json.dumps(self.cfg).encode())
        elif path == "/update" and method == "POST":
            # a multipart upload: keep the file part, answer like WLED, then "restart"
            boundary = headers["content-type"].split("boundary=")[1].encode()
            part = body.split(b"--" + boundary)[1]
            self.firmware_uploads.append(len(part.split(bytes([13, 10, 13, 10]), 1)[1]) - 2)
            if self.update_answers:
                send("200 OK", b"<html><body><h2>Update successful!</h2>Rebooting...</body></html>", "text/html")
            else:
                writer.transport.abort()
            self.booted = time.monotonic() - 2
            self.state.update(on=True, bri=128, lor=0)  # WLED's power-on look
        elif path in ("/json/state", "/json/state/") and method == "GET":
            send("200 OK", json.dumps(self.state).encode())
        elif path in ("/json/state", "/json/state/") and method == "POST":
            posted = json.loads(body)
            self.posts.append(posted)
            self.state.update({k: v for k, v in posted.items() if k in ("on", "bri", "lor")})
            link = posted.get("WLEDLink")
            if isinstance(link, dict) and self.wll is not None:
                if "key" in link:
                    self.wll_key = bytes.fromhex(link["key"])
                    self.wll["kf"] = hmac.new(self.wll_key, b"WLLF", hashlib.sha256).digest()[:4].hex()
                if "restore" in link:
                    self.wll["restore"] = bool(link["restore"])
            send("200 OK", b'{"success":true}')
        elif path == "/":
            send("200 OK", HTML, "text/html")
        elif path == "/big":
            send("200 OK", BIG, "application/octet-stream")
        elif path == "/echo" and method == "POST":
            send("200 OK", json.dumps({"len": len(body), "sha": hashlib.sha256(body).hexdigest()}).encode())
        elif path == "/slow":
            await asyncio.sleep(0.3)
            send("200 OK", b'{"slow":true}')
        elif path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
            await writer.drain()
            while data := await reader.read(4096):
                writer.write(data)
                await writer.drain()
        else:
            send("404 Not Found", b"not found", "text/plain")
        await writer.drain()


class _Udp(asyncio.DatagramProtocol):
    def __init__(self, wled):
        self.wled = wled

    def datagram_received(self, data, addr):
        self.wled.udp_packets.append((addr, data))
