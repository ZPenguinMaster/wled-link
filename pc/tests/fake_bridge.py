"""A software copy of the bridge firmware's protocol, reachable as socket://127.0.0.1:<port>.

Mirrors bridge/src/main.cpp: sessions, sequence checks, per-connection windows and acks,
beacons while no host is attached. Faults (reboot, lost frame, unplug, garbage) can be
injected to check that wledlink recovers.
"""

import asyncio
import json
import os
import random
import socket
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import wledlink as wl  # noqa: E402

WINDOW = 4096
MAX_CONNS = 8


class FakeBridge:
    def __init__(self, wled_http, wled_udp, station_ip="192.168.77.2", ack_delay=0.002):
        self.wled_http = wled_http
        self.wled_udp = wled_udp
        self.station_ip = station_ip
        self.stations = [{"mac": "aa:bb:cc:dd:ee:ff", "ip": station_ip, "rssi": -42}]
        self.ack_delay = ack_delay
        self.boot_id = random.getrandbits(32)
        self.writer = None
        self.tx_seq = 0
        self.rx_expect = None
        self.host_active = False
        self.hello_nonce = 0
        self.last_host_rx = 0.0
        self.conns = {}
        self.drop_next = False
        self.window_violations = 0
        self.config_payloads = []
        self.udp_sent = 0
        self.sessions = 0
        self.gaps = 0
        self.wled_cmds = []      # H_WLED_CMD payloads received
        self.baud_requests = []
        self.light = {"on": 1, "bri": 128, "lor": 0, "live": 0, "ps": -1, "fx": 9, "cct": 127, "col": [255, 0, 0, 0]}
        self.light_seq = 0
        self.last_stream = 0.0   # like the real bridge, "live" follows the LED data it forwards
        self.link_key = os.urandom(16)   # the radio link to the light (see bridge/src/link.cpp)
        self.link_mode = "wifi"
        self.link_set = False
        self.link_up = False             # tests set this: whether the ESP-NOW link to the light works
        self.link_requests = []

    async def start(self):
        self.server = await asyncio.start_server(self._on_host, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._timer = asyncio.create_task(self._housekeeping())

    def close(self):
        self._timer.cancel()
        self.server.close()
        if self.writer:
            self.writer.close()
        self.udp.close()

    # -- faults

    def reboot(self):
        self.boot_id = random.getrandbits(32)
        self._end_session()
        self._send_info()

    def unplug(self):
        if self.writer:
            self.writer.close()
            self.writer = None
        self._end_session()

    def garbage(self, n=300):
        if self.writer:
            self.writer.write(bytes(random.getrandbits(8) for _ in range(n)))

    def set_stations(self, stations):
        self.stations = stations
        if self.host_active:
            self._send_sta_list()

    # -- link

    def _send(self, ftype, payload=b""):
        seq = self.tx_seq
        self.tx_seq = (self.tx_seq + 1) & 0xFF
        if self.drop_next:
            self.drop_next = False
            return
        if self.writer:
            self.writer.write(wl.encode_frame(ftype, seq, payload))

    def _send_json(self, ftype, obj):
        self._send(ftype, json.dumps(obj).encode())

    def _send_info(self):
        self._send_json(wl.B_INFO, {
            "proto": 1, "fw": "fake", "boot": f"{self.boot_id:08x}", "nonce": self.hello_nonce,
            "host": 1 if self.host_active else 0, "mac": "de:ad:be:ef:00:01", "wifi": 1, "ssid": "WLEDLink",
            "pass": "quartz-basil-1769", "ch": 6, "chCfg": 0, "hidden": 1, "txq": 34, "withPc": 0, "caps": "ws,baud,phone,espnow", "apIp": "192.168.77.1",
            "maxConns": MAX_CONNS, "win": WINDOW, "uptime": 1, "heap": 150000, "minHeap": 120000, "sta": len(self.stations),
            "udpTx": self.udp_sent, "udpDrop": 0, "rxBad": 0, "rxGaps": self.gaps, "tcpOpened": 0,
            "phones": 1, "phonePin": "482913", "pairing": 0, "link": self.link_mode, "linkSet": int(self.link_set),
            "lkf": wl.key_fingerprint(self.link_key), "linkUp": int(self.link_up)})

    def _send_sta_list(self):
        self._send_json(wl.B_STA_LIST, {"sta": self.stations})

    def _end_session(self):
        self.host_active = False
        self.hello_nonce = 0
        self.rx_expect = None
        for conn in list(self.conns.values()):
            self._drop(conn)
        self.conns.clear()

    async def _on_host(self, reader, writer):
        if self.writer:
            self.writer.close()
        self._end_session()
        self.writer = writer
        buf = bytearray()
        try:
            while data := await reader.read(65536):
                buf += data
                while (zero := buf.find(0)) >= 0:
                    chunk = bytes(buf[:zero])
                    del buf[:zero + 1]
                    if chunk:
                        await self._on_frame(chunk)
        except ConnectionError:
            pass
        finally:
            if self.writer is writer:
                self.writer = None
                self._end_session()

    async def _housekeeping(self):
        loop = asyncio.get_running_loop()
        while True:
            await asyncio.sleep(0.5)
            if self.host_active and loop.time() - self.last_host_rx > 3:
                self._end_session()
            if not self.host_active:
                self._send_info()
            if self.light["live"] and loop.time() - self.last_stream > 2.5:
                self.light["live"] = 0
                if self.host_active:
                    self._send_light()

    async def _on_frame(self, encoded):
        frame = wl.decode_frame(encoded)
        if not frame:
            return
        ftype, seq, p = frame
        self.last_host_rx = asyncio.get_running_loop().time()
        if ftype == wl.H_HELLO:
            self._end_session()
            self.host_active = True
            self.sessions += 1
            self.rx_expect = (seq + 1) & 0xFF
            self.hello_nonce = struct.unpack_from("<I", p, 1)[0]
            self._send_info()
            self._send_sta_list()
            self._send_light()
            return
        if not self.host_active:
            self._send_info()
            return
        if seq != self.rx_expect:
            self.gaps += 1
            self._end_session()
            self._send_info()
            return
        self.rx_expect = (seq + 1) & 0xFF

        if ftype == wl.H_PING:
            self._send(wl.B_PONG)
        elif ftype == wl.H_STA_REQ:
            self._send_sta_list()
        elif ftype == wl.H_STATS_REQ:
            self._send_json(wl.B_STATS, {"uptime": 5, "heap": 150000, "minHeap": 120000, "sta": len(self.stations), "udpTx": self.udp_sent,
                                             "phones": 1, "phonePin": "482913", "pairing": 0, "linkUp": int(self.link_up)})
        elif ftype == wl.H_LINK_KEY_REQ:
            self._send(wl.B_LINK_KEY, self.link_key)
        elif ftype == wl.H_SET_LINK:
            self.link_requests.append(bytes(p))
            if len(p) >= 17:
                self.link_key = bytes(p[1:17])
            mode = {0: "espnow", 1: "wifi"}.get(p[0])
            if mode is None:
                self._send_json(wl.B_CONFIG_RESULT, {"ok": True, "msg": "key saved"})
            elif mode == self.link_mode:
                self.link_set = True
                self._send_json(wl.B_CONFIG_RESULT, {"ok": True, "msg": "saved"})
            else:
                self.link_mode, self.link_set = mode, True
                self._send_json(wl.B_CONFIG_RESULT, {"ok": True, "msg": "switching, restarting"})
                asyncio.get_running_loop().call_later(0.3, self.reboot)
        elif ftype == wl.H_SET_CONFIG:
            self.config_payloads.append(bytes(p))
            self._send_json(wl.B_CONFIG_RESULT, {"ok": True, "msg": "saved, rebooting"})
            asyncio.get_running_loop().call_later(0.3, self.reboot)
        elif ftype == wl.H_REBOOT:
            asyncio.get_running_loop().call_later(0.1, self.reboot)
        elif ftype == wl.H_TCP_OPEN:
            await self._open(p[0], socket.inet_ntoa(p[1:5]), struct.unpack(">H", p[5:7])[0])
        elif ftype == wl.H_TCP_DATA:
            conn = self.conns.get(p[0])
            if conn and not conn["closing"]:
                conn["unacked"] += len(p) - 1
                if conn["unacked"] > WINDOW:
                    self.window_violations += 1
                conn["queue"].put_nowait(bytes(p[1:]))
        elif ftype == wl.H_TCP_CLOSE:
            conn = self.conns.get(p[0])
            if conn and not conn["closing"]:
                conn["closing"] = True
                conn["queue"].put_nowait(None)
        elif ftype == wl.H_WLED_CMD:
            cmd = json.loads(p)
            self.wled_cmds.append(cmd)
            for key in ("on", "bri", "lor", "ps"):
                if key in cmd:
                    self.light[key] = int(cmd[key]) if isinstance(cmd[key], bool) else cmd[key]
            seg = cmd.get("seg") or {}
            if isinstance(seg, dict):
                for key in ("fx", "cct"):
                    if key in seg:
                        self.light[key] = seg[key]
                if seg.get("col"):
                    self.light["col"] = list(seg["col"][0]) + [0] * (4 - len(seg["col"][0]))
            self._send_light()  # like the real bridge: echo the new state at once
            asyncio.create_task(self._apply_to_wled(p))
        elif ftype == wl.H_SET_BAUD:
            rate = struct.unpack("<I", p[:4])[0]
            self.baud_requests.append(rate)
            self._send(wl.B_BAUD, bytes([1]) + p[:4])  # a socket has no baud rate; just agree
        elif ftype == wl.H_UDP_SEND:
            if socket.inet_ntoa(p[0:4]) == self.station_ip:
                self.udp.sendto(p[6:], self.wled_udp)
                self.udp_sent += 1
                self.last_stream = asyncio.get_running_loop().time()
                if not self.light["live"]:
                    self.light["live"] = 1
                    self._send_light()

    def _send_light(self):
        self.light_seq += 1
        self._send_json(wl.B_WLED_STATE, dict(self.light, n=self.light_seq, ws=1, ok=1))

    async def _apply_to_wled(self, body):
        """Forward a command to the fake WLED over HTTP (the real bridge uses a WebSocket)."""
        crlf = bytes([13, 10])
        head = [b"POST /json/state HTTP/1.1", b"Host: x", b"Content-Type: application/json",
                b"Content-Length: " + str(len(body)).encode(), b"", b""]
        try:
            r, w = await asyncio.open_connection(*self.wled_http)
            w.write(crlf.join(head) + bytes(body))
            await w.drain()
            await r.read()
            w.close()
        except OSError:
            pass

    async def _open(self, cid, ip, port):
        if cid in self.conns:
            self._drop(self.conns.pop(cid))
        if ip != self.station_ip or port != 80 or not any(s["ip"] == ip for s in self.stations):
            self._send(wl.B_TCP_OPEN_RESULT, bytes([cid, 3]))
            return
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(*self.wled_http), 4)
        except (OSError, asyncio.TimeoutError):
            self._send(wl.B_TCP_OPEN_RESULT, bytes([cid, 3]))
            return
        conn = {"id": cid, "r": r, "w": w, "unacked": 0, "closing": False, "closed": False,
                "queue": asyncio.Queue(), "session": self.sessions}
        self.conns[cid] = conn
        self._send(wl.B_TCP_OPEN_RESULT, bytes([cid, 0]))
        conn["tasks"] = [asyncio.create_task(self._to_wled(conn)), asyncio.create_task(self._from_wled(conn))]

    def _drop(self, conn):
        conn["closed"] = True
        for task in conn.get("tasks", []):
            task.cancel()
        conn["w"].close()

    def _closed(self, conn, reason):
        if conn["closed"]:
            return
        self._drop(conn)
        if self.conns.get(conn["id"]) is conn:
            del self.conns[conn["id"]]
            self._send(wl.B_TCP_CLOSED, bytes([conn["id"], reason]))

    async def _to_wled(self, conn):
        try:
            while (data := await conn["queue"].get()) is not None:
                conn["w"].write(data)
                await conn["w"].drain()
                await asyncio.sleep(self.ack_delay)  # a slow socket, so the window actually matters
                conn["unacked"] -= len(data)
                if not conn["closed"]:
                    self._send(wl.B_TCP_ACK, bytes([conn["id"]]) + struct.pack("<H", len(data)))
            self._closed(conn, 2)
        except ConnectionError:
            self._closed(conn, 1)

    async def _from_wled(self, conn):
        try:
            while data := await conn["r"].read(1024):
                if conn["closing"]:
                    continue
                self._send(wl.B_TCP_DATA, bytes([conn["id"]]) + data)
                await asyncio.sleep(0)
            if not conn["closing"]:
                self._closed(conn, 0)
        except ConnectionError:
            self._closed(conn, 1)
