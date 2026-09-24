#!/usr/bin/env python3
"""WLED Link: reach a WLED controller through a USB-connected ESP32 bridge.

The bridge ESP32 (firmware in ../bridge) hosts a small private Wi-Fi network that the
WLED controller joins. This program talks to the bridge over USB serial and makes WLED
appear on this PC as if it were on the local network:

    http://127.0.0.2/             WLED web UI, JSON API and websocket
    UDP 127.0.0.2:21324           WLED realtime port (SignalRGB streams here)
    http://127.0.0.2/__wledlink   status page for the link itself

Run it with no arguments to start the link; see --help for the other commands.
"""

from __future__ import annotations

import argparse
import asyncio
import binascii
import collections
import contextlib
import html
import importlib.util
import json
import logging
import logging.handlers
import os
import re
import secrets
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # reported in main() so --help still works
    serial = None

VERSION = "1.2.0"
PROTO_VERSION = 1

# Message types, see bridge/src/main.cpp for the payload layouts.
H_HELLO, H_PING, H_STA_REQ, H_SET_CONFIG, H_REBOOT, H_STATS_REQ = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
H_PHONE_PAIR, H_PHONE_FORGET, H_PHONE_PIN, H_PHONE_TEST = 0x07, 0x08, 0x09, 0x0A
H_TCP_OPEN, H_TCP_DATA, H_TCP_CLOSE, H_UDP_SEND = 0x10, 0x11, 0x12, 0x20
B_INFO, B_PONG, B_STA_LIST, B_CONFIG_RESULT, B_STATS, B_PHONE_RESULT = 0x81, 0x82, 0x83, 0x84, 0x86, 0x87
B_TCP_OPEN_RESULT, B_TCP_DATA, B_TCP_CLOSED, B_TCP_ACK = 0x90, 0x91, 0x92, 0x93

OPEN_ERRORS = {
    1: "no free connection slot on the bridge",
    2: "the bridge ran out of sockets",
    3: "connection refused",
    4: "connection timed out",
}

MAX_PAYLOAD = 1600
TCP_CHUNK = 1024
MAX_UDP_PAYLOAD = MAX_PAYLOAD - 6
LINK_TIMEOUT = 5.0        # seconds without any frame before the bridge counts as gone
PROBE_SECONDS = 4.0       # how long to listen on a serial port for the bridge's beacon
DEFAULT_BAUD = 921600
DEFAULT_LISTEN = "127.0.0.2"
DEFAULT_UDP_PORTS = (21324, 4048, 5568, 6454)  # WLED realtime/sync, DDP, E1.31, Art-Net
USB_SERIAL_IDS = {(0x10C4, 0xEA60), (0x1A86, 0x7523), (0x1A86, 0x55D4), (0x0403, 0x6001), (0x303A, 0x1001)}
WLED_BRANDS = ("WLED", "GLEDOPTO")

# Responses that carry WLED's own IP address. SignalRGB reads "ip" from them and talks
# to that address afterwards, so it has to be the address WLED is reachable at *here*.
REWRITE_PATHS = {"/json", "/json/info", "/json/si"}
IP_FIELD = re.compile(rb'"ip"\s*:\s*"[0-9.]*"')

# WLED segment CCT: 0 = warm white only, 255 = cool white only. With "CCT additive blending" at
# 100%, 127 drives both white channels at (practically) full.
WHITE_TONES = {"neutral": 127, "cool": 255, "warm": 0}
AUTO_WHITE_MODES = {0: "None", 1: "Brighter", 2: "Accurate", 3: "Dual", 4: "Max"}
# WLED's "AP opens" setting for its own WLED-AP hotspot, by the names the CLI and status page use.
HOTSPOT_MODES = {"off": 3, "boot": 0, "fallback": 1}  # never / only if it boots without the bridge / whenever it loses it


def settings_summary(cfg: dict | None) -> dict:
    """The few WLED settings the status page and the white buttons care about."""
    if not cfg:
        return {"readable": False}
    led = (cfg.get("hw") or {}).get("led") or {}
    buses = led.get("ins") or []
    auto_white = led.get("rgbwm", 255)  # global override; 255 means "use each output's own setting"
    if not isinstance(auto_white, int) or auto_white == 255:
        auto_white = buses[0].get("rgbwm", 0) if buses else 0
    ap = cfg.get("ap") or {}
    return {"readable": True, "cctBlend": led.get("cb", 0), "autoWhite": auto_white,
            "autoWhiteName": AUTO_WHITE_MODES.get(auto_white, str(auto_white)),
            "cctFromRgb": bool(led.get("cr")), "apBehavior": ap.get("behav", 0), "apHidden": bool(ap.get("hide"))}

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FIRMWARE_BIN = ROOT / "bridge" / "prebuilt" / "wledlink-bridge.bin"
FIRMWARE_CONFIG = ROOT / "bridge" / "src" / "config.h"
CONFIG_FILE = HERE / "wledlink.json"
APP_DIR = Path(os.environ.get("WLEDLINK_HOME") or Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "wledlink")

log = logging.getLogger("wledlink")


# ---------------------------------------------------------------------------------------
# framing: COBS + CRC-16/CCITT-FALSE, identical to the firmware


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    pos, n = 0, len(data)
    while True:
        zero = data.find(0, pos, min(pos + 254, n))
        if zero >= 0:
            out.append(zero - pos + 1)
            out += data[pos:zero]
            pos = zero + 1
            continue
        end = min(pos + 254, n)
        if end - pos == 254:
            out.append(0xFF)
            out += data[pos:end]
            pos = end
            continue
        out.append(end - pos + 1)
        out += data[pos:end]
        return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    pos, n = 0, len(data)
    while pos < n:
        code = data[pos]
        if code == 0:
            raise ValueError("zero byte inside COBS data")
        end = pos + code
        if end > n:
            raise ValueError("truncated COBS data")
        out += data[pos + 1:end]
        pos = end
        if code != 0xFF and pos < n:
            out.append(0)
    return bytes(out)


def encode_frame(ftype: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes((ftype, seq)) + payload
    return cobs_encode(body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))) + b"\x00"


def decode_frame(encoded: bytes):
    """Returns (type, seq, payload), or None if the frame is damaged."""
    try:
        body = cobs_decode(encoded)
    except ValueError:
        return None
    if len(body) < 4 or binascii.crc_hqx(body[:-2], 0xFFFF) != struct.unpack_from("<H", body, len(body) - 2)[0]:
        return None
    return body[0], body[1], body[2:-2]


# ---------------------------------------------------------------------------------------
# serial port


def open_serial(name: str, baud: int):
    """Opens a port without toggling DTR/RTS, which would reset an ESP32 dev board."""
    ser = serial.serial_for_url(name, do_not_open=True)
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 3
    with contextlib.suppress(Exception):
        ser.dtr = False
        ser.rts = False
    ser.open()
    return ser


def usb_serial_ports(prefer: str | None = None) -> list[str]:
    """USB serial ports, likely ESP32 boards first. Bluetooth COM ports are skipped: opening
    them can hang for many seconds."""
    ports = [p for p in serial.tools.list_ports.comports() if p.vid is not None]
    ports.sort(key=lambda p: (p.device != prefer, (p.vid, p.pid) not in USB_SERIAL_IDS, p.device))
    return [p.device for p in ports]


def listen_for_beacon(ser, seconds: float, abort=lambda: False) -> bool:
    """True if a bridge INFO frame arrives within `seconds`. Sends nothing, so probing a port
    that belongs to some other device (like the WLED board itself) is harmless."""
    deadline = time.monotonic() + seconds
    buf = bytearray()
    while time.monotonic() < deadline and not abort():
        data = ser.read(ser.in_waiting or 1)
        if not data:
            continue
        buf += data
        while (zero := buf.find(0)) >= 0:
            frame = decode_frame(bytes(buf[:zero]))
            del buf[:zero + 1]
            if frame and frame[0] == B_INFO:
                return True
        if len(buf) > 8192:
            del buf[:-2048]
    return False


class PortScanner:
    """Finds the bridge among the serial ports, backing off from ports that are something else."""

    def __init__(self):
        self._failed: dict[str, float] = {}

    def scan(self, explicit: str | None, baud: int, prefer: str | None, abort=lambda: False):
        if explicit:
            names = [explicit]
        else:
            now = time.monotonic()
            names = [n for n in usb_serial_ports(prefer) if n == prefer or now - self._failed.get(n, -1e9) > 30]
        for name in names:
            if abort():
                return None
            try:
                ser = open_serial(name, baud)
            except (OSError, serial.SerialException) as exc:
                log.debug("cannot open %s: %s", name, exc)
                self._failed[name] = time.monotonic()
                continue
            if listen_for_beacon(ser, PROBE_SECONDS, abort):
                self._failed.pop(name, None)
                return ser, name
            ser.close()
            self._failed[name] = time.monotonic()
            log.debug("no bridge on %s", name)
        return None


class SerialLink:
    """Moves frames over an open serial port with one reader and one writer thread.

    Reliable frames (control and TCP) are sent in order. Lossy frames (UDP colour data) wait
    in a short queue that drops the oldest entry when full, so a busy link never delays
    realtime data by more than a few frames.
    """

    def __init__(self, ser, loop: asyncio.AbstractEventLoop, on_frame, on_lost):
        self.ser = ser
        self._loop = loop
        self._on_frame = on_frame
        self._on_lost = on_lost
        self._cv = threading.Condition()
        self._reliable: collections.deque = collections.deque()
        self._lossy: collections.deque = collections.deque(maxlen=4)
        self._seq = 0
        self._closed = False
        self._lost_reported = False
        self.bytes_in = 0
        self.bytes_out = 0
        self.bad_frames = 0
        self.lossy_dropped = 0

    def start(self):
        threading.Thread(target=self._read_loop, name="serial-read", daemon=True).start()
        threading.Thread(target=self._write_loop, name="serial-write", daemon=True).start()

    def send(self, ftype: int, payload: bytes = b"", lossy: bool = False):
        with self._cv:
            if self._closed:
                return
            if lossy:
                if len(self._lossy) == self._lossy.maxlen:
                    self.lossy_dropped += 1
                self._lossy.append((ftype, payload))
            else:
                self._reliable.append((ftype, payload))
            self._cv.notify()

    def close(self, reason: str = "closed"):
        with self._cv:
            was_open = not self._closed
            self._closed = True
            self._cv.notify_all()
            report = not self._lost_reported
            self._lost_reported = True
        if was_open:
            with contextlib.suppress(Exception):
                self.ser.close()
        if report:
            with contextlib.suppress(RuntimeError):  # event loop already closed on shutdown
                self._loop.call_soon_threadsafe(self._on_lost, self, reason)

    def _write_loop(self):
        try:
            while True:
                with self._cv:
                    while not self._closed and not self._reliable and not self._lossy:
                        self._cv.wait()
                    if self._closed:
                        return
                    batch, size = [], 0
                    while self._reliable and size < 4096:
                        item = self._reliable.popleft()
                        batch.append(item)
                        size += len(item[1])
                    if self._lossy:
                        batch.append(self._lossy.popleft())
                out = bytearray()
                for ftype, payload in batch:
                    out += encode_frame(ftype, self._seq, payload)
                    self._seq = (self._seq + 1) & 0xFF
                self.ser.write(out)
                self.bytes_out += len(out)
        except Exception as exc:
            self.close(f"serial write error ({exc})")

    def _read_loop(self):
        buf = bytearray()
        try:
            while not self._closed:
                data = self.ser.read(self.ser.in_waiting or 1)
                if not data:
                    continue
                self.bytes_in += len(data)
                buf += data
                frames, start = [], 0
                while (zero := buf.find(0, start)) >= 0:
                    if zero > start:
                        frame = decode_frame(bytes(buf[start:zero]))
                        if frame:
                            frames.append(frame)
                        else:
                            self.bad_frames += 1
                    start = zero + 1
                if start:
                    del buf[:start]
                if len(buf) > 8192:  # junk with no delimiters (e.g. boot messages at another baud rate)
                    buf.clear()
                if frames:
                    self._loop.call_soon_threadsafe(self._deliver, frames)
        except Exception as exc:
            self.close(f"serial read error ({exc})")

    def _deliver(self, frames):
        for ftype, seq, payload in frames:
            self._on_frame(self, ftype, seq, payload)


# ---------------------------------------------------------------------------------------
# tunnel


class LinkDown(Exception):
    pass


class TunnelConn:
    """One TCP connection from the bridge to a device on its Wi-Fi network."""

    def __init__(self, bridge: "Bridge", cid: int, window: int):
        self.bridge = bridge
        self.id = cid
        self.window = window
        self.opened: asyncio.Future = asyncio.get_running_loop().create_future()
        self.opened.add_done_callback(lambda f: f.cancelled() or f.exception())  # never "unretrieved"
        self._rx: asyncio.Queue = asyncio.Queue()
        self._unacked = 0
        self._window_open = asyncio.Event()
        self._window_open.set()
        self.closing = False    # we asked the bridge to close it
        self.finished = False   # the bridge closed it (or the session is gone)

    async def read(self) -> bytes:
        """The next chunk from the device, or b"" once the connection is closed."""
        if self.finished and self._rx.empty():
            return b""
        return await self._rx.get()

    async def write(self, data: bytes):
        view = memoryview(data)
        while view:
            if self.finished or self.closing:
                raise ConnectionResetError("tunnel connection closed")
            chunk = bytes(view[:TCP_CHUNK])
            while self._unacked + len(chunk) > self.window:
                self._window_open.clear()
                await self._window_open.wait()
                if self.finished:
                    raise ConnectionResetError("tunnel connection closed")
            self._unacked += len(chunk)
            self.bridge.send(H_TCP_DATA, bytes([self.id]) + chunk)
            view = view[len(chunk):]

    def close(self):
        """Closes after the bridge has delivered everything already written."""
        if not self.closing and not self.finished:
            self.closing = True
            self.bridge.send(H_TCP_CLOSE, bytes([self.id]))

    def _on_open(self):
        if not self.opened.done():
            self.opened.set_result(True)

    def _on_data(self, data: bytes):
        if data:
            self._rx.put_nowait(data)

    def _on_ack(self, count: int):
        self._unacked = max(0, self._unacked - count)
        self._window_open.set()

    def _finish(self, error: Exception | None = None):
        if self.finished:
            return
        self.finished = True
        if not self.opened.done():
            self.opened.set_exception(error or ConnectionResetError("closed before it opened"))
        self._rx.put_nowait(b"")
        self._window_open.set()


def dechunk(buf: bytes | bytearray):
    """Decodes an HTTP chunked body. Returns (complete, body)."""
    out, pos = bytearray(), 0
    while True:
        eol = buf.find(b"\r\n", pos)
        if eol < 0:
            return False, None
        size = int(bytes(buf[pos:eol]).split(b";")[0].strip() or b"0", 16)
        pos = eol + 2
        if size == 0:
            if buf[pos:pos + 2] == b"\r\n" or buf.find(b"\r\n\r\n", pos) >= 0:
                return True, bytes(out)
            return False, None
        if len(buf) < pos + size + 2:
            return False, None
        out += buf[pos:pos + size]
        pos += size + 2


async def read_http_response(conn: TunnelConn, timeout: float):
    """Reads one whole HTTP response. Returns (status line, [(name, value)], body)."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout

    async def next_chunk() -> bytes:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise asyncio.TimeoutError()
        return await asyncio.wait_for(conn.read(), remaining)

    buf = bytearray()
    while (end := buf.find(b"\r\n\r\n")) < 0:
        chunk = await next_chunk()
        if not chunk:
            raise ConnectionError("connection closed before the response headers")
        buf += chunk
        if len(buf) > 65536:
            raise ConnectionError("response headers too large")
    lines = bytes(buf[:end]).decode("latin-1").split("\r\n")
    headers = [(k.strip(), v.strip()) for k, sep, v in (line.partition(":") for line in lines[1:]) if sep]
    fields = {k.lower(): v for k, v in headers}
    body = bytearray(buf[end + 4:])
    if "chunked" in fields.get("transfer-encoding", "").lower():
        while True:
            complete, decoded = dechunk(body)
            if complete:
                body = bytearray(decoded)
                break
            chunk = await next_chunk()
            if not chunk:
                raise ConnectionError("truncated chunked response")
            body += chunk
    elif fields.get("content-length", "").isdigit():
        length = int(fields["content-length"])
        while len(body) < length:
            chunk = await next_chunk()
            if not chunk:
                break
            body += chunk
        del body[length:]
    else:
        while chunk := await next_chunk():
            body += chunk
    return lines[0], headers, bytes(body)


# ---------------------------------------------------------------------------------------
# bridge session


@dataclass
class Config:
    port: str | None = None
    baud: int = DEFAULT_BAUD
    listen: str = DEFAULT_LISTEN
    http_port: int = 80
    udp_ports: tuple = DEFAULT_UDP_PORTS
    wled_mac: str | None = None
    time_sync: bool = True
    verbose: bool = False


class UdpIn(asyncio.DatagramProtocol):
    def __init__(self, bridge: "Bridge", port: int):
        self.bridge = bridge
        self.port = port

    def datagram_received(self, data, addr):
        self.bridge.forward_udp(self.port, data)

    def error_received(self, exc):
        log.debug("UDP %d: %s", self.port, exc)


class Bridge:
    """Keeps a session with the bridge ESP32 and multiplexes tunnelled connections over it."""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.loop: asyncio.AbstractEventLoop | None = None
        self.link: SerialLink | None = None
        self.port_name: str | None = None
        self.state = "searching"  # searching -> handshake -> ready
        self.info: dict = {}
        self.bridge_stats: dict = {}
        self.stations: list[dict] = []
        self.target: dict | None = None  # {"ip", "mac", "info", "verified", "failures"}
        self.window = 4096
        self.stats: collections.Counter = collections.Counter()
        self.conns: dict[int, TunnelConn] = {}
        self._free: list[int] = []
        self._slots_changed = asyncio.Event()
        self._link_lost = asyncio.Event()
        self._resolve_now = asyncio.Event()
        self._hello_nonce = 0
        self._hello_at = 0.0
        self._rx_expect: int | None = None
        self._last_rx = 0.0
        self._last_boot = None
        self._last_port: str | None = None
        self._paused_until = 0.0
        self._config_waiters: list[asyncio.Future] = []
        self._phone_waiters: list[asyncio.Future] = []
        self._udp: dict[int, asyncio.BaseTransport | None] = {}
        self._scanner = PortScanner()
        self._last_time_sync = 0.0

    # -- lifecycle

    async def run(self):
        self.loop = asyncio.get_running_loop()
        for port in self.cfg.udp_ports:
            await self.listen_udp(port)
        helpers = [asyncio.create_task(self._keepalive()), asyncio.create_task(self._resolver())]
        try:
            while True:
                if time.monotonic() < self._paused_until:
                    self.state = "paused"
                    await asyncio.sleep(0.5)
                    continue
                self.state = "searching"
                found = await asyncio.to_thread(self._scanner.scan, self.cfg.port, self.cfg.baud, self._last_port,
                                                lambda: time.monotonic() < self._paused_until)
                if not found:
                    await asyncio.sleep(2)
                    continue
                if time.monotonic() < self._paused_until:
                    found[0].close()
                    continue
                self._attach(*found)
                await self._link_lost.wait()
                await asyncio.sleep(0.5)
        finally:
            for task in helpers:
                task.cancel()
            if self.link:
                self.link.close("shutting down")

    def _attach(self, ser, name: str):
        self._link_lost.clear()
        self.port_name = self._last_port = name
        self.link = SerialLink(ser, self.loop, self._on_frame, self._on_link_lost)
        self._last_rx = time.monotonic()
        self.link.start()
        log.info("Bridge found on %s", name)
        self._start_handshake()

    def _on_link_lost(self, link: SerialLink, reason: str):
        if link is not self.link:
            return
        log.warning("Lost the bridge on %s: %s", self.port_name, reason)
        self.stats["link_lost"] += 1
        self.link = None
        self._reset_session("bridge disconnected")
        self.state = "searching"
        self._link_lost.set()

    def pause(self, seconds: float):
        """Lets go of the serial port for a while, e.g. so the bridge can be reflashed."""
        self._paused_until = time.monotonic() + seconds
        if self.link:
            self.link.close("paused")

    def resume(self):
        self._paused_until = 0.0

    def send(self, ftype: int, payload: bytes = b"", lossy: bool = False):
        if self.link:
            self.link.send(ftype, payload, lossy)

    # -- session

    def _start_handshake(self):
        self._reset_session("session restarted")
        self.state = "handshake"
        self._hello_nonce = secrets.randbits(32) or 1
        self._hello_at = time.monotonic()
        self.send(H_HELLO, struct.pack("<BI", PROTO_VERSION, self._hello_nonce))

    def _reset_session(self, reason: str):
        conns, self.conns = self.conns, {}
        self._free = []
        for conn in conns.values():
            conn._finish(ConnectionResetError(reason))
        for fut in self._config_waiters + self._phone_waiters:
            if not fut.done():
                fut.set_exception(LinkDown(reason))
        self._config_waiters.clear()
        self._phone_waiters.clear()
        self._rx_expect = None
        self._slots_changed.set()

    def _on_frame(self, link: SerialLink, ftype: int, seq: int, payload: bytes):
        if link is not self.link:
            return
        self._last_rx = time.monotonic()
        if ftype == B_INFO:
            self._rx_expect = (seq + 1) & 0xFF
            self._on_info(payload)
            return
        if self.state != "ready":
            return
        if seq != self._rx_expect:
            log.warning("Data from the bridge was lost; restarting the session")
            self.stats["resyncs"] += 1
            self._start_handshake()
            return
        self._rx_expect = (seq + 1) & 0xFF
        try:
            self._dispatch(ftype, payload)
        except (IndexError, ValueError) as exc:
            log.warning("Malformed frame 0x%02x from the bridge: %s", ftype, exc)

    def _dispatch(self, ftype: int, payload: bytes):
        if ftype == B_TCP_DATA:
            conn = self.conns.get(payload[0])
            if conn:
                conn._on_data(payload[1:])
        elif ftype == B_TCP_ACK:
            conn = self.conns.get(payload[0])
            if conn:
                conn._on_ack(payload[1] | payload[2] << 8)
        elif ftype == B_TCP_OPEN_RESULT:
            cid, status = payload[0], payload[1]
            if cid in self.conns:
                if status == 0:
                    self.conns[cid]._on_open()
                else:
                    self._release(cid, ConnectionRefusedError(OPEN_ERRORS.get(status, f"error {status}")))
        elif ftype == B_TCP_CLOSED:
            self._release(payload[0])
        elif ftype == B_STA_LIST:
            self._on_stations(json.loads(payload))
        elif ftype == B_STATS:
            self.bridge_stats = json.loads(payload)
        elif ftype == B_CONFIG_RESULT:
            result = json.loads(payload)
            for fut in self._config_waiters:
                if not fut.done():
                    fut.set_result(result)
            self._config_waiters.clear()
        elif ftype == B_PHONE_RESULT:
            result = json.loads(payload)
            for fut in self._phone_waiters:
                if not fut.done():
                    fut.set_result(result)
            self._phone_waiters.clear()

    def _on_info(self, payload: bytes):
        try:
            info = json.loads(payload)
        except ValueError:
            return
        if self._last_boot and info.get("boot") != self._last_boot:
            log.info("The bridge restarted")
        self._last_boot = info.get("boot")
        self.info = info
        self.bridge_stats = info
        if info.get("host") and info.get("nonce") == self._hello_nonce:
            if self.state != "ready":
                self.state = "ready"
                self.window = int(info.get("win", 4096))
                self._free = list(range(int(info.get("maxConns", 8))))
                self._slots_changed.set()
                log.info("Bridge ready: firmware %s, Wi-Fi \"%s\" on channel %s",
                         info.get("fw"), info.get("ssid"), info.get("ch"))
                self.send(H_STA_REQ)
                self.request_resolve()
            return
        if self.state == "ready":
            log.warning("The bridge dropped the session; starting a new one")
            self.stats["resyncs"] += 1
            self._start_handshake()
        elif self.state == "handshake" and time.monotonic() - self._hello_at > 1.5:
            self._start_handshake()

    def _on_stations(self, data: dict):
        stations = data.get("sta", [])
        changed = [(s.get("mac"), s.get("ip")) for s in stations] != [(s.get("mac"), s.get("ip")) for s in self.stations]
        self.stations = stations
        if changed:
            joined = ", ".join(f"{s.get('mac')} ({s.get('ip')})" for s in stations) or "none"
            log.info("Devices on the bridge Wi-Fi: %s", joined)
            self.request_resolve()

    async def _keepalive(self):
        tick = 0
        while True:
            await asyncio.sleep(1)
            tick += 1
            link = self.link
            if not link:
                continue
            if time.monotonic() - self._last_rx > LINK_TIMEOUT:
                link.close("the bridge stopped responding")
            elif self.state == "handshake" and time.monotonic() - self._hello_at > 1.5:
                self._start_handshake()
            elif self.state == "ready":
                # any frame keeps the session alive; the station list is also pushed on changes
                self.send(H_STA_REQ if tick % 10 == 0 else H_STATS_REQ if tick % 5 == 0 else H_PING)

    # -- connections

    def _release(self, cid: int, error: Exception | None = None):
        conn = self.conns.pop(cid, None)
        if conn is None:
            return
        conn._finish(error)
        self._free.append(cid)
        self._slots_changed.set()

    async def open_tcp(self, ip: str, port: int, timeout: float = 8.0) -> TunnelConn:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while self.state != "ready" or not self._free:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise LinkDown("the bridge is not connected" if self.state != "ready" else "all bridge connections are busy")
            self._slots_changed.clear()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self._slots_changed.wait(), remaining)
        cid = self._free.pop(0)
        conn = TunnelConn(self, cid, self.window)
        self.conns[cid] = conn
        self.send(H_TCP_OPEN, bytes([cid]) + socket.inet_aton(ip) + struct.pack(">H", port))
        try:
            await asyncio.wait_for(asyncio.shield(conn.opened), max(0.5, deadline - loop.time()))
        except asyncio.TimeoutError:
            conn.close()
            raise ConnectionError(f"timed out connecting to {ip}:{port}") from None
        return conn

    async def http_request(self, ip: str, method: str, path: str, body: bytes | None = None, timeout: float = 6.0):
        conn = await self.open_tcp(ip, 80, timeout)
        try:
            lines = [f"{method} {path} HTTP/1.1", f"Host: {ip}", "Connection: close", "Accept: application/json"]
            if body is not None:
                lines += ["Content-Type: application/json", f"Content-Length: {len(body)}"]
            await conn.write(("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b""))
            status_line, _, content = await read_http_response(conn, timeout)
            parts = status_line.split(" ")
            if len(parts) < 2 or not parts[1].isdigit():
                raise ConnectionError(f"not an HTTP response: {status_line[:40]!r}")
            return int(parts[1]), content
        finally:
            conn.close()

    # -- UDP

    async def listen_udp(self, port: int):
        if port in self._udp:
            return
        self._udp[port] = None
        try:
            transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: UdpIn(self, port), local_addr=(self.cfg.listen, port))
            self._udp[port] = transport
        except OSError as exc:
            log.warning("Cannot listen on UDP %s:%d (%s)", self.cfg.listen, port, exc)

    def forward_udp(self, port: int, data: bytes):
        target = self.target
        if self.state != "ready" or not target:
            self.stats["udp_unreachable"] += 1  # e.g. SignalRGB streaming while the bridge restarts
            return
        if len(data) > MAX_UDP_PAYLOAD:
            self.stats["udp_dropped"] += 1
            return
        self.send(H_UDP_SEND, socket.inet_aton(target["ip"]) + struct.pack(">H", port) + data, lossy=True)
        self.stats["udp_forwarded"] += 1

    # -- finding WLED on the bridge network

    def request_resolve(self):
        self._resolve_now.set()

    async def _resolver(self):
        while True:
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self._resolve_now.wait(), 10)
            forced = self._resolve_now.is_set()
            self._resolve_now.clear()
            if self.state != "ready":
                continue
            try:
                await self._resolve(forced)
                await self._sync_time()
                if self.target and time.monotonic() - self.target.get("settings_at", -1e9) > 300:
                    await self.wled_settings()
            except Exception as exc:  # keep looking on the next round
                log.debug("looking for WLED failed: %s", exc)

    async def _probe(self, ip: str) -> dict | None:
        try:
            status, body = await self.http_request(ip, "GET", "/json/info", timeout=5)
            info = json.loads(body) if status == 200 else None
        except Exception:
            return None
        if isinstance(info, dict) and (info.get("brand") in WLED_BRANDS or "leds" in info):
            return info
        return None

    async def _resolve(self, forced: bool):
        target = self.target
        if target and not forced and time.monotonic() - target["verified"] < 60:
            return
        if target and any(s.get("ip") == target["ip"] for s in self.stations):
            info = await self._probe(target["ip"])
            if info:
                target.update(info=info, verified=time.monotonic(), failures=0)
                await self._listen_for(info)
                return
            target["failures"] = target.get("failures", 0) + 1
            if target["failures"] < 3:
                return

        prefer = (self.cfg.wled_mac or "").lower()
        for station in sorted(self.stations, key=lambda s: s.get("mac", "").lower() != prefer):
            ip = station.get("ip")
            if not ip or ip == "0.0.0.0":
                continue
            info = await self._probe(ip)
            if info:
                if not target or target["ip"] != ip:
                    leds = (info.get("leds") or {}).get("count")
                    log.info("WLED \"%s\" (v%s, %s LEDs) is reachable at http://%s/",
                             info.get("name"), info.get("ver"), leds, self.cfg.listen)
                    self._last_time_sync = 0.0
                self.target = {"ip": ip, "mac": station.get("mac"), "info": info,
                               "verified": time.monotonic(), "failures": 0}
                await self._listen_for(info)
                return
        if target:
            log.warning("WLED is no longer reachable through the bridge")
        self.target = None

    async def _listen_for(self, info: dict):
        port = info.get("udpport")
        if isinstance(port, int) and 0 < port < 65536:
            await self.listen_udp(port)

    async def _sync_time(self):
        """WLED has no internet for NTP here, so hand it the PC's clock now and then."""
        if not self.cfg.time_sync or not self.target:
            return
        if self._last_time_sync and time.monotonic() - self._last_time_sync < 6 * 3600:
            return
        body = json.dumps({"time": int(time.time())}).encode()
        status, _ = await self.http_request(self.target["ip"], "POST", "/json/state", body)
        if status == 200:
            self._last_time_sync = time.monotonic()
            log.debug("sent the current time to WLED")

    # -- WLED settings and white light

    def _require_target(self) -> dict:
        if self.state != "ready" or not self.target:
            raise LinkDown("WLED is not reachable right now")
        return self.target

    async def wled_settings(self) -> dict | None:
        """WLED's /json/cfg (None if it can't be read, e.g. behind a settings PIN). Also refreshes the
        summary shown on the status page."""
        target = self._require_target()
        try:
            status, body = await self.http_request(target["ip"], "GET", "/json/cfg")
            cfg = json.loads(body) if status == 200 else None
        except (ConnectionError, ValueError, asyncio.TimeoutError):
            cfg = None
        target["settings"] = settings_summary(cfg)
        target["settings_at"] = time.monotonic()
        return cfg

    async def update_wled_settings(self, cfg: dict, changes: dict):
        """Changes a few WLED settings through /json/cfg without side effects.

        WLED resets some settings whose keys are missing from a partial update: 0.15 switches gamma
        correction off when "light" is absent, and the global auto-white override (plus the frame rate
        on newer versions) falls back to its default. So the current "light" section and LED options go
        back along with the change, minus the LED outputs themselves, which would re-initialise the strip.
        """
        led = {k: v for k, v in ((cfg.get("hw") or {}).get("led") or {}).items() if k not in ("ins", "matrix")}
        body: dict = {"hw": {"led": led}}
        if "light" in cfg:
            body["light"] = cfg["light"]
        for section, values in changes.items():
            if section == "hw":
                body["hw"]["led"].update(values.get("led", {}))
            else:
                body.setdefault(section, {}).update(values)
        status, _ = await self.http_request(self._require_target()["ip"], "POST", "/json/cfg", json.dumps(body).encode())
        if status != 200:
            raise ConnectionError(f"WLED refused the settings change (HTTP {status}); is a settings PIN set?")

    async def set_white(self, tone: str, brightness: int = 100) -> dict:
        """Solid white from the strip's white LEDs only, overriding SignalRGB until "Back to SignalRGB"."""
        if tone not in WHITE_TONES:
            raise ValueError(f"tone must be one of {', '.join(WHITE_TONES)}")
        target = self._require_target()
        cfg = await self.wled_settings()
        s = target["settings"]
        lc = (target["info"].get("leds") or {}).get("lc", 7)  # capability bits: 1 RGB, 2 white, 4 CCT
        notes = []
        if not lc & 0x02:
            col = [255, 255, 255, 0]
            notes.append("WLED reports no white channel for this strip, so this white is mixed from RGB.")
        elif s.get("autoWhite", 0) in (0, 3):   # None / Dual: the white value is used as given
            col = [0, 0, 0, 255]
        elif s.get("autoWhite") == 2:           # Accurate: full RGB turns into full white with RGB off
            col = [255, 255, 255, 0]
        else:                                   # Brighter / Max: white only ever comes with RGB
            col = [255, 255, 255, 255]
            notes.append("WLED's \"Auto-calculate white channel\" is set to Brighter/Max, so the RGB LEDs light up too. "
                         "For white LEDs only, set it to None, Accurate or Dual in Config > LED Preferences.")
        if lc & 0x04:
            if s.get("cctFromRgb"):
                notes.append("WLED's \"Calculate CCT from RGB\" is on, which overrides warm/cool. Turn it off in Config > LED Preferences.")
            if tone == "neutral" and s.get("cctBlend", 0) < 100:
                if cfg is None:
                    notes.append("For both white channels at full, set \"CCT additive blending\" to 100% in WLED's "
                                 "Config > LED Preferences (the settings couldn't be read, maybe a settings PIN).")
                else:
                    await self.update_wled_settings(cfg, {"hw": {"led": {"cb": 100}}})
                    s["cctBlend"] = 100
                    notes.append("Set WLED's \"CCT additive blending\" to 100%, which is what lets warm and cool white "
                                 "both run at full. WLED's own white-temperature slider now peaks in the middle too.")
        bri = max(1, min(255, round(brightness * 255 / 100)))
        state = {"on": True, "bri": bri, "lor": 2,
                 "seg": {"fx": 0, "col": [col, [0, 0, 0, 0], [0, 0, 0, 0]], "cct": WHITE_TONES[tone]}}
        status, _ = await self.http_request(target["ip"], "POST", "/json/state", json.dumps(state).encode())
        return {"ok": status == 200, "notes": notes}

    async def set_wled_hotspot(self, mode: str) -> dict:
        """When WLED opens its own "WLED-AP" hotspot: never, if it boots without the bridge, or whenever
        it loses the bridge (so a phone can still reach it while the bridge has no power)."""
        if mode not in HOTSPOT_MODES:
            raise ValueError(f"mode must be one of {', '.join(HOTSPOT_MODES)}")
        cfg = await self.wled_settings()
        if cfg is None:
            raise ConnectionError("couldn't read WLED's settings (is a settings PIN set?)")
        if (cfg.get("ap") or {}).get("behav") != HOTSPOT_MODES[mode]:
            await self.update_wled_settings(cfg, {"ap": {"behav": HOTSPOT_MODES[mode]}})
        await self.wled_settings()
        notes = {
            "off": "WLED won't open its WLED-AP hotspot any more. If it ever can't reach the bridge, "
                   "plug it into the PC and run: python wledlink.py wled-wifi",
            "boot": "WLED opens its WLED-AP hotspot only if it starts up without the bridge (WLED's default).",
            "fallback": "WLED opens its WLED-AP hotspot (http://4.3.2.1) whenever it loses the bridge, and closes it "
                        "when the bridge is back.",
        }
        return {"ok": self._require_target()["settings"].get("apBehavior") == HOTSPOT_MODES[mode], "notes": [notes[mode]]}

    # -- bridge settings

    async def phone(self, action: str, args: dict) -> dict:
        """Bluetooth phone control on the bridge: pair / forget / pin / test."""
        if self.state != "ready":
            raise LinkDown("the bridge is not connected")
        if "phonePin" not in self.bridge_stats:
            raise ValueError("the bridge firmware has no Bluetooth yet; flash it with: python wledlink.py flash-bridge")
        if action == "pair":
            seconds = min(max(int(args.get("seconds", 120)), 10), 600)
            self.send(H_PHONE_PAIR, struct.pack("<H", seconds))
            return {"ok": True, "notes": [f"A new phone can pair for the next {seconds} seconds "
                                          f"(PIN {self.bridge_stats.get('phonePin')}). The bridge's LED blinks fast meanwhile."]}
        if action == "forget":
            self.send(H_PHONE_FORGET)
            return {"ok": True, "notes": ["All paired phones forgotten. On the iPhone, also remove \"Lamp\" in Settings > Bluetooth."]}
        if action == "pin":
            pin = str(args.get("pin", ""))
            if not re.fullmatch(r"\d{6}", pin):
                raise ValueError("the PIN must be 6 digits")
            fut = asyncio.get_running_loop().create_future()
            self._config_waiters.append(fut)
            self.send(H_PHONE_PIN, struct.pack("<I", int(pin)))
            result = await asyncio.wait_for(fut, 5)
            return {"ok": result.get("ok"), "notes": [result.get("msg", "")]}
        if action == "test":
            fut = asyncio.get_running_loop().create_future()
            self._phone_waiters.append(fut)
            self.send(H_PHONE_TEST, json.dumps(args.get("state", {})).encode())
            result = await asyncio.wait_for(fut, 15)
            return {"ok": bool(result.get("ok")), "result": result,
                    "notes": [] if result.get("ok") else [result.get("err", "failed")]}
        raise ValueError("unknown phone action")

    async def set_config(self, payload: bytes) -> dict:
        if self.state != "ready":
            raise LinkDown("the bridge is not connected")
        fut = asyncio.get_running_loop().create_future()
        self._config_waiters.append(fut)
        self.send(H_SET_CONFIG, payload)
        return await asyncio.wait_for(fut, 5)

    def status(self) -> dict:
        info, counters, target, link = self.info, self.bridge_stats, self.target, self.link
        wled = None
        if target:
            i = target["info"]
            wled = {"ip": target["ip"], "mac": target["mac"], "name": i.get("name"), "version": i.get("ver"),
                    "leds": (i.get("leds") or {}).get("count"), "udpPort": i.get("udpport"),
                    "signal": (i.get("wifi") or {}).get("signal"), "settings": target.get("settings")}
        return {
            "version": VERSION,
            "listen": self.cfg.listen,
            "bridge": {
                "state": self.state, "port": self.port_name, "firmware": info.get("fw"), "mac": info.get("mac"),
                "wifiUp": bool(info.get("wifi")), "ssid": info.get("ssid"), "password": info.get("pass"),
                "channel": info.get("ch"), "channelSetting": info.get("chCfg"), "hidden": bool(info.get("hidden")),
                "txPowerDbm": (info.get("txq") or 0) / 4, "wifiWithPc": bool(info.get("withPc")),
                "uptime": counters.get("uptime"),
                "phones": counters.get("phones"), "phonePin": counters.get("phonePin"),
                "phonePairing": counters.get("pairing"),
                "freeHeap": counters.get("heap"), "udpSent": counters.get("udpTx"),
                "udpFailed": counters.get("udpDrop"), "badFrames": counters.get("rxBad"),
                "lostFrames": counters.get("rxGaps"),
            } if self.state != "searching" or info else {"state": self.state},
            "stations": self.stations,
            "wled": wled,
            "openConnections": len(self.conns),
            "udpListening": sorted(p for p, t in self._udp.items() if t),
            "stats": {
                **self.stats,
                "serialIn": link.bytes_in if link else 0,
                "serialOut": link.bytes_out if link else 0,
                "serialBadFrames": link.bad_frames if link else 0,
                "udpQueueDrops": link.lossy_dropped if link else 0,
            },
        }


# ---------------------------------------------------------------------------------------
# HTTP front end on 127.0.0.2:80


class Request:
    def __init__(self, method: str, target: str, version: str, headers: list[tuple[str, str]]):
        self.method, self.target, self.version, self.headers = method, target, version, headers
        self.path = urlsplit(target).path or "/"

    @classmethod
    def parse(cls, head: bytes) -> "Request | None":
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) != 3:
            return None
        headers = []
        for line in lines[1:]:
            if line:
                name, sep, value = line.partition(":")
                if not sep:
                    return None
                headers.append((name.strip(), value.strip()))
        return cls(parts[0], parts[1], parts[2], headers)

    def header(self, name: str) -> str:
        name = name.lower()
        return next((v for k, v in self.headers if k.lower() == name), "")

    def head_bytes(self, close: bool = False) -> bytes:
        headers = self.headers
        if close:
            headers = [(k, v) for k, v in headers if k.lower() not in ("connection", "keep-alive")]
            headers.append(("Connection", "close"))
        lines = [f"{self.method} {self.target} {self.version}"] + [f"{k}: {v}" for k, v in headers]
        return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")


def http_response(status: int, reason: str, body: bytes, content_type: str, extra: list[str] | None = None) -> bytes:
    lines = [f"HTTP/1.1 {status} {reason}", f"Content-Type: {content_type}", f"Content-Length: {len(body)}",
             "Cache-Control: no-store", "Connection: close"] + (extra or [])
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


class HttpFront:
    def __init__(self, bridge: Bridge, cfg: Config):
        self.bridge = bridge
        self.cfg = cfg
        self.server = None

    async def start(self):
        self.server = await asyncio.start_server(self._handle, self.cfg.listen, self.cfg.http_port, limit=1 << 16)

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        try:
            await self._serve(reader, writer)
        except (ConnectionError, OSError, asyncio.TimeoutError, LinkDown) as exc:
            log.debug("client connection ended: %s", exc)
        except Exception:
            log.exception("error while serving a request")
        finally:
            with contextlib.suppress(Exception):
                writer.close()

    async def _serve(self, reader, writer):
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 30)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError):
            return
        req = Request.parse(head)
        if req is None:
            writer.write(http_response(400, "Bad Request", b"bad request\n", "text/plain"))
            return
        if why := self._refuse(req):
            log.warning("Refused a request for %s: %s", req.path, why)
            writer.write(http_response(403, "Forbidden", why.encode() + b"\n", "text/plain"))
            return
        self.bridge.stats["http_requests"] += 1
        if req.path == "/__wledlink" or req.path.startswith("/__wledlink/"):
            return await self._local(req, reader, writer)

        target = self.bridge.target
        if self.bridge.state != "ready" or not target:
            return await self._unavailable(req, writer, self._why_unavailable())
        try:
            conn = await self.bridge.open_tcp(target["ip"], 80)
        except Exception as exc:
            self.bridge.request_resolve()
            return await self._unavailable(req, writer, f"Could not reach WLED through the bridge: {exc}")

        # One request per connection (websockets aside), so every request passes through here
        # and none of the /json responses can slip past the rewrite on a reused connection.
        upgrade = "upgrade" in req.header("connection").lower()
        rewrite = req.method in ("GET", "POST") and req.path.rstrip("/") in REWRITE_PATHS
        try:
            await conn.write(head if upgrade else req.head_bytes(close=True))
            upstream = asyncio.create_task(self._pump_up(reader, conn))
            try:
                if rewrite:
                    await self._relay_rewritten(conn, writer)
                else:
                    await self._relay(conn, writer, upgrade)
            finally:
                upstream.cancel()
        finally:
            conn.close()

    def _refuse(self, req: Request) -> str | None:
        """Only this PC's own programs and pages may use the link. Listening on loopback keeps other
        machines out, but any website open in a browser here could still send requests to 127.0.0.2
        (cross-site requests) or rebind its own domain name to it (DNS rebinding) to read the status
        page, which shows the Wi-Fi password and the Bluetooth PIN."""
        host = req.header("host").rsplit(":", 1)[0].strip("[]").lower() if req.header("host") else ""
        if host not in (self.cfg.listen, "localhost", "127.0.0.1"):
            return "unexpected Host header"
        if req.header("sec-fetch-site").lower() == "cross-site":
            return "cross-site request"
        origin = urlsplit(req.header("origin"))
        if origin.scheme in ("http", "https") and req.method not in ("GET", "HEAD"):  # websites only; apps may send other schemes
            if (origin.hostname or "") not in (self.cfg.listen, "localhost", "127.0.0.1"):
                return "request from another website"
        return None

    async def _pump_up(self, reader, conn: TunnelConn):
        try:
            while data := await reader.read(16384):
                await conn.write(data)
        except (ConnectionError, OSError):
            pass
        finally:
            conn.close()  # the client is done (or gone); WLED closes its side when it finishes

    async def _relay(self, conn: TunnelConn, writer, upgrade: bool):
        if not upgrade:
            # Mark the response "Connection: close" so the client never reuses this connection.
            buf = bytearray()
            while (end := buf.find(b"\r\n\r\n")) < 0:
                chunk = await asyncio.wait_for(conn.read(), 30)
                if not chunk:
                    writer.write(buf)
                    return await writer.drain()
                buf += chunk
                if len(buf) > 65536:
                    raise ConnectionError("response headers too large")
            lines = bytes(buf[:end]).decode("latin-1").split("\r\n")
            lines = [lines[0]] + [l for l in lines[1:] if l.partition(":")[0].strip().lower() not in ("connection", "keep-alive")]
            writer.write(("\r\n".join(lines + ["Connection: close"]) + "\r\n\r\n").encode("latin-1") + buf[end + 4:])
            await writer.drain()
        while chunk := await conn.read():
            writer.write(chunk)
            await writer.drain()

    async def _relay_rewritten(self, conn: TunnelConn, writer):
        status_line, headers, body = await read_http_response(conn, 15)
        body = IP_FIELD.sub(b'"ip":"' + self.cfg.listen.encode() + b'"', body)
        drop = ("content-length", "transfer-encoding", "connection")
        lines = [status_line] + [f"{k}: {v}" for k, v in headers if k.lower() not in drop]
        lines += [f"Content-Length: {len(body)}", "Connection: close"]
        writer.write(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body)
        await writer.drain()

    def _why_unavailable(self) -> str:
        b = self.bridge
        if b.state == "paused":
            return "The link is paused (the bridge is being flashed)."
        if b.state != "ready":
            return "The bridge ESP32 is not connected to this PC."
        if not b.stations:
            return f"WLED has not joined the bridge's Wi-Fi \"{b.info.get('ssid')}\" yet."
        return "WLED is not answering yet."

    async def _unavailable(self, req: Request, writer, why: str):
        if "text/html" in req.header("accept"):
            if req.path == "/":
                writer.write(http_response(302, "Found", b"", "text/plain", ["Location: /__wledlink"]))
            else:
                body = (f"<!doctype html><meta charset=utf-8><title>WLED Link</title><p>{html.escape(why)}</p>"
                        f"<p><a href=\"/__wledlink\">Open the WLED Link status page</a></p>").encode()
                writer.write(http_response(503, "Service Unavailable", body, "text/html; charset=utf-8"))
        else:
            body = json.dumps({"error": why}).encode()
            writer.write(http_response(503, "Service Unavailable", body, "application/json"))
        await writer.drain()

    async def _local(self, req: Request, reader, writer):
        body = b""
        length = int(req.header("content-length")) if req.header("content-length").isdigit() else 0
        if length > 65536:
            writer.write(http_response(413, "Payload Too Large", b"too large\n", "text/plain"))
            return
        if length:
            body = await asyncio.wait_for(reader.readexactly(length), 10)
        path = req.path.rstrip("/")
        b = self.bridge

        async def reply_json(obj, status=200):
            data = json.dumps(obj, indent=1).encode()
            writer.write(http_response(status, "OK" if status == 200 else "Error", data, "application/json"))
            await writer.drain()

        if path == "/__wledlink" and req.method == "GET":
            writer.write(http_response(200, "OK", STATUS_PAGE.encode(), "text/html; charset=utf-8"))
            return await writer.drain()
        if path == "/__wledlink/status.json":
            return await reply_json(b.status())
        if path == "/__wledlink/phone" and req.method == "GET":
            page = (HERE / "phone" / "index.html").read_bytes()
            writer.write(http_response(200, "OK", page, "text/html; charset=utf-8"))
            return await writer.drain()
        if req.method != "POST":
            return await reply_json({"error": "not found"}, 404)
        if path == "/__wledlink/api/quit":
            log.info("Stopping (asked to by \"wledlink.py stop\")")
            await reply_json({"ok": True})
            asyncio.get_running_loop().call_later(0.3, os._exit, 0)
            return
        try:
            args = json.loads(body or b"{}")
        except ValueError:
            args = None
        if not isinstance(args, dict):
            return await reply_json({"error": "expected a JSON object"}, 400)
        try:
            if path == "/__wledlink/api/bridge-config":
                return await reply_json(await b.set_config(config_payload(args)))
            if path == "/__wledlink/api/bridge-reboot":
                b.send(H_REBOOT)
                return await reply_json({"ok": True})
            if path == "/__wledlink/api/pause":
                b.pause(min(max(float(args.get("seconds", 120)), 0), 600))
                return await reply_json({"ok": True})
            if path == "/__wledlink/api/resume":
                b.resume()
                return await reply_json({"ok": True})
            if path == "/__wledlink/api/rescan":
                b.request_resolve()
                return await reply_json({"ok": True})
            if path == "/__wledlink/api/white":
                return await reply_json(await b.set_white(str(args.get("tone", "neutral")), int(args.get("brightness", 100))))
            if path.startswith("/__wledlink/api/phone-"):
                return await reply_json(await b.phone(path.rsplit("-", 1)[1], args))
            if path == "/__wledlink/api/wled-hotspot":
                return await reply_json(await b.set_wled_hotspot(str(args.get("mode", ""))))
        except (LinkDown, ValueError, TypeError, ConnectionError, asyncio.TimeoutError) as exc:
            return await reply_json({"ok": False, "msg": str(exc) or "timed out"}, 409)
        return await reply_json({"error": "not found"}, 404)


def config_payload(args: dict) -> bytes:
    """Builds a SET_CONFIG payload from {"ssid", "password", "channel", "hidden", "txPowerDbm", "wifiWithPc"}
    or {"reset": true}."""
    if args.get("reset"):
        return b"\xff"
    ssid = str(args.get("ssid", "")).encode()
    password = str(args.get("password", "")).encode()
    channel = int(args.get("channel", 0))
    txq = round(float(args.get("txPowerDbm", 8.5)) * 4)
    if not 1 <= len(ssid) <= 32:
        raise ValueError("SSID must be 1-32 characters")
    if not 8 <= len(password) <= 63:
        raise ValueError("password must be 8-63 characters")
    if not 0 <= channel <= 11:
        raise ValueError("channel must be 0 (auto) or 1-11")
    if not 8 <= txq <= 78:
        raise ValueError("TX power must be between 2 and 19.5 dBm")
    return (bytes([len(ssid)]) + ssid + bytes([len(password)]) + password +
            bytes([channel, 1 if args.get("hidden") else 0, txq, 1 if args.get("wifiWithPc") else 0]))


STATUS_PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLED Link</title>
<style>
:root{--bg:#121212;--card:#1e1e1e;--fg:#e8e8e8;--dim:#9a9a9a;--line:#2e2e2e;--ok:#4caf50;--warn:#f0a020;--bad:#e5484d;--acc:#4a8cff}
@media (prefers-color-scheme:light){:root{--bg:#f3f3f3;--card:#fff;--fg:#1b1b1b;--dim:#666;--line:#e3e3e3}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 "Segoe UI",system-ui,sans-serif}
a{color:var(--acc)}
main{max-width:760px;margin:0 auto;padding:20px 16px 40px}h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:0 0 10px}
.sub{color:var(--dim);margin:0 0 18px}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin:0 0 14px}
.step{display:flex;gap:10px;padding:6px 0}.dot{flex:none;width:12px;height:12px;border-radius:50%;margin-top:5px;background:var(--dim)}
.ok .dot{background:var(--ok)}.wait .dot{background:var(--warn)}.bad .dot{background:var(--bad)}.step b{display:block}
.step span{color:var(--dim)}code{background:var(--line);padding:1px 5px;border-radius:4px;font-size:14px}
.btns{display:flex;flex-wrap:wrap;gap:8px}button,.btn{font:inherit;border:1px solid var(--line);background:var(--bg);color:var(--fg);padding:8px 14px;border-radius:8px;cursor:pointer;text-decoration:none}
button.primary,.btn.primary{background:var(--acc);border-color:var(--acc);color:#fff}button:disabled{opacity:.45;cursor:default}
dl{display:grid;grid-template-columns:max-content 1fr;gap:4px 16px;margin:0}dt{color:var(--dim)}dd{margin:0;overflow-wrap:anywhere}
#mode{margin:0 0 12px}details summary{cursor:pointer;color:var(--dim)}label{display:block;margin:8px 0 2px;color:var(--dim)}
input,select{font:inherit;width:100%;padding:6px 8px;border-radius:6px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
.note{color:var(--dim);font-size:13px;margin-top:8px}
</style></head><body><main>
<h1>WLED Link</h1><p class="sub">WLED through the USB bridge &mdash; no campus Wi-Fi involved.</p>
<section class="card"><h2>Setup</h2><div id="steps">Loading&hellip;</div></section>
<section class="card"><h2>Lights</h2><p id="mode">&nbsp;</p>
<div class="btns">
<button data-white="neutral">Neutral white 100%</button><button data-white="cool">Cool white 100%</button>
<button data-white="warm">Warm white 100%</button>
</div>
<div class="btns" style="margin-top:8px">
<a class="btn primary" href="/">Open WLED</a><button data-act="signal">Back to SignalRGB</button><button data-act="off">Lights off</button>
</div>
<p class="note">Neutral runs the warm and cool white LEDs both at full, Cool and Warm run just one of them at full; the RGB LEDs stay off.
The white buttons tell WLED to ignore SignalRGB (the same as WLED's own &ldquo;Override&rdquo; button) until you press Back to SignalRGB.</p>
<p class="note" id="notes"></p></section>
<section class="card"><h2>Bridge</h2><dl id="bridge"></dl>
<details style="margin-top:12px"><summary>Change the bridge Wi-Fi</summary>
<p class="note">WLED has to be switched to the new name/password as well, or it loses the connection. Change WLED first, then save here.</p>
<form id="cfg"><label>Network name</label><input name="ssid" maxlength="32" required>
<label>Password (8-63 characters)</label><input name="password" minlength="8" maxlength="63" required>
<label>Channel</label><select name="channel"><option value="0">Automatic (least crowded of 1/6/11)</option>
<option>1</option><option>6</option><option>11</option></select>
<p><button class="primary" type="submit">Save and restart bridge</button> <span id="cfgmsg"></span></p></form></details></section>
<section class="card" id="phoneCard" style="display:none"><h2>Phone (Bluetooth)</h2><dl id="phone"></dl>
<div class="btns" style="margin-top:12px"><button id="pairBtn">Let a new phone pair (2 min)</button><button id="forgetBtn">Forget paired phones</button></div>
<p class="note">On the iPhone, open the light page in the Bluefy browser, tap Connect, pick &ldquo;Lamp&rdquo; and type the PIN when iOS asks.
Only phones paired while this window is open are accepted (holding the bridge's BOOT button for 2 s opens it too).</p>
<p class="note" id="phoneNotes"></p></section>
<section class="card"><h2>WLED</h2><dl id="wled"></dl>
<div id="hotspotRow" style="display:none"><label for="hotspot">WLED's own backup hotspot (&ldquo;WLED-AP&rdquo;, http://4.3.2.1)</label>
<select id="hotspot"><option value="boot">Only if WLED starts up without the bridge (WLED's default)</option>
<option value="fallback">Whenever WLED loses the bridge (phone control even if the bridge has no power)</option>
<option value="off">Never</option></select><p class="note" id="hotspotNotes"></p></div></section>
</main>
<script>
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
let last = null, formFilled = false;
function step(state, title, text) {
  return `<div class="step ${state}"><div class="dot"></div><div><b>${title}</b><span>${text}</span></div></div>`;
}
function dl(rows) { return rows.filter(r => r[1] !== undefined && r[1] !== null && r[1] !== "").map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join(""); }
function render(s) {
  last = s;
  const b = s.bridge, w = s.wled, ready = b.state === "ready", joined = s.stations.length > 0;
  const noIp = s.stations.filter(x => !x.ip || x.ip === "0.0.0.0");
  let html = "";
  if (ready) html += step("ok", "Bridge ESP32 connected", `on ${esc(b.port)}, private Wi-Fi <code>${esc(b.ssid)}</code> on channel ${esc(b.channel)}`);
  else if (b.state === "paused") html += step("wait", "Link paused", "Released the serial port so the bridge can be flashed.");
  else if (b.state === "handshake") html += step("wait", "Bridge found", `on ${esc(b.port)}, starting a session&hellip;`);
  else html += step("bad", "Bridge ESP32 not found", "Plug the bridge ESP32 into this PC. All USB serial ports are being checked.");
  if (!ready) html += step("", "WLED joins the bridge Wi-Fi", "Waiting for the bridge.");
  else if (!joined) html += step("bad", "WLED has not joined the bridge Wi-Fi", `In WLED's Wi-Fi settings type network <code>${esc(b.ssid)}</code>${b.hidden ? " (it's hidden, so it won't appear in WLED's scan list)" : ""} with password <code>${esc(b.password)}</code>, and remove any other networks. Or run <code>python wledlink.py wled-wifi</code> with the WLED board on USB.`);
  else if (noIp.length && !w) html += step("wait", "A device joined but has no IP address", "If you gave WLED a static IP for the campus network, clear it in WLED's Wi-Fi settings (use 0.0.0.0).");
  else html += step("ok", "WLED joined the bridge Wi-Fi", s.stations.map(x => `${esc(x.mac)} (${esc(x.ip)}, ${esc(x.rssi)} dBm)`).join(", "));
  if (w) html += step("ok", "WLED reachable", `<a href="/">http://${esc(s.listen)}/</a> &mdash; ${esc(w.name)}, ${esc(w.leds)} LEDs, v${esc(w.version)}`);
  else html += step(ready && joined ? "wait" : "", "WLED reachable", "Waiting for WLED to answer.");
  const ws = (w && w.settings) || {};
  html += step(w ? "ok" : "", "SignalRGB", `Lighting Services &rarr; WLED &rarr; &ldquo;Discover WLED device by IP&rdquo;: <code>${esc(s.listen)}</code>, then Link. Colours go to UDP port ${esc((w && w.udpPort) || 21324)}.`);
  if (ready && w) html += step("", "Phone (optional)", `Join the hidden Wi-Fi <code>${esc(b.ssid)}</code> (password <code>${esc(b.password)}</code>, turn auto-join off), then open <code>http://${esc(w.ip)}/</code> or the WLED app. ${b.wifiWithPc ? "The bridge's Wi-Fi is set to run only while this program does, so this won't work with the PC off." : "Works with the PC off too, as long as the bridge still gets USB power."}`);
  $("steps").innerHTML = html;
  const up = b.uptime != null ? `${Math.floor(b.uptime / 3600)}h ${Math.floor(b.uptime / 60) % 60}m` : null;
  $("bridge").innerHTML = dl([["State", esc(b.state)], ["Serial port", esc(b.port)], ["Firmware", esc(b.firmware)],
    ["Wi-Fi", b.ssid ? `<code>${esc(b.ssid)}</code> / <code>${esc(b.password)}</code>${b.hidden ? " (hidden)" : ""}` : null],
    ["Channel", b.channel != null ? `${esc(b.channel)}${b.channelSetting ? "" : " (automatic)"}` : null],
    ["TX power", b.txPowerDbm ? `${b.txPowerDbm} dBm` : null],
    ["Wi-Fi on", b.ssid ? (b.wifiWithPc ? "only while this program runs" : "whenever the bridge has power") : null], ["Uptime", up],
    ["Colour packets", s.stats.udp_forwarded != null ? `${s.stats.udp_forwarded} forwarded` +
      (s.stats.udp_unreachable ? `, ${s.stats.udp_unreachable} arrived while WLED was unreachable` : "") +
      (s.stats.udp_dropped || s.stats.udpQueueDrops ? `, ${(s.stats.udp_dropped || 0) + (s.stats.udpQueueDrops || 0)} dropped` : "") : null],
    ["Link errors", `${s.stats.resyncs || 0} resyncs, ${s.stats.serialBadFrames || 0} bad frames`]]);
  $("wled").innerHTML = w ? dl([["Name", esc(w.name)], ["Version", esc(w.version)], ["LEDs", esc(w.leds)],
    ["Bridge-side IP", esc(w.ip)], ["MAC", esc(w.mac)], ["Signal", w.signal != null ? `${esc(w.signal)}%` : null],
    ["White channel", ws.readable ? `auto-calculate: ${esc(ws.autoWhiteName)}, CCT additive blending ${esc(ws.cctBlend)}%` : null]])
    : "<dd>Not connected.</dd>";
  document.querySelectorAll("[data-act],[data-white]").forEach(el => el.disabled = !w);
  $("phoneCard").style.display = ready && b.phonePin ? "block" : "none";
  if (b.phonePin) $("phone").innerHTML = dl([["Bluetooth name", "Lamp"], ["PIN", `<code>${esc(b.phonePin)}</code>`],
    ["Paired phones", esc(b.phones)], ["New pairing", b.phonePairing ? `open for ${esc(b.phonePairing)} s` : "closed"]]);
  $("hotspotRow").style.display = ws.readable ? "block" : "none";
  if (ws.readable && document.activeElement !== $("hotspot"))
    $("hotspot").value = {3: "off", 0: "boot", 1: "fallback"}[ws.apBehavior] || "boot";
  if (ready && !formFilled) {
    const f = $("cfg"); f.ssid.value = b.ssid || ""; f.password.value = b.password || ""; f.channel.value = String(b.channelSetting || 0); formFilled = true;
  }
}
async function refresh() {
  try { render(await (await fetch("/__wledlink/status.json", {cache: "no-store"})).json()); }
  catch (e) { $("steps").innerHTML = step("bad", "wledlink is not running", "Start it with <code>python wledlink.py</code>."); }
  if (last && last.wled) {
    try {
      const si = await (await fetch("/json/si", {cache: "no-store"})).json();
      const s = si.state, i = si.info;
      let m = !s.on ? "Off" : s.lor ? "WLED override (SignalRGB is being ignored)" : i.live ? "SignalRGB is streaming" : "WLED effects / presets";
      $("mode").textContent = `Now: ${m}` + (s.on ? `, brightness ${Math.round(s.bri / 2.55)}%` : "");
    } catch (e) {}
  } else $("mode").textContent = " ";
}
const actions = {off: {on: false}, signal: {lor: 0, on: true}};
async function api(path, body, where = "notes") {
  $(where).textContent = "";
  try {
    const r = await (await fetch(path, {method: "POST", body: JSON.stringify(body)})).json();
    $(where).textContent = (r.notes || []).join(" ") || (r.ok === false ? `Didn't work: ${r.msg || "unknown error"}` : "");
  } catch (e) { $(where).textContent = "wledlink didn't answer."; }
  refresh();
}
$("pairBtn").addEventListener("click", () => api("/__wledlink/api/phone-pair", {seconds: 120}, "phoneNotes"));
$("forgetBtn").addEventListener("click", () => {
  if (confirm("Forget every paired phone?")) api("/__wledlink/api/phone-forget", {}, "phoneNotes");
});
$("hotspot").addEventListener("change", (ev) => {
  api("/__wledlink/api/wled-hotspot", {mode: ev.target.value}, "hotspotNotes");
  ev.target.blur();
});
document.querySelectorAll("[data-white]").forEach(el => el.addEventListener("click", () =>
  api("/__wledlink/api/white", {tone: el.dataset.white, brightness: 100})));
document.querySelectorAll("[data-act]").forEach(el => el.addEventListener("click", async () => {
  $("notes").textContent = "";
  await fetch("/json/state", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(actions[el.dataset.act])});
  refresh();
}));
$("cfg").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = ev.target, b = (last && last.bridge) || {};
  const body = {ssid: f.ssid.value, password: f.password.value, channel: +f.channel.value, hidden: !!b.hidden,
                txPowerDbm: b.txPowerDbm || 8.5, wifiWithPc: !!b.wifiWithPc};
  const r = await (await fetch("/__wledlink/api/bridge-config", {method: "POST", body: JSON.stringify(body)})).json();
  $("cfgmsg").textContent = r.msg || (r.ok ? "Saved" : "Failed"); formFilled = false;
});
refresh(); setInterval(refresh, 2500);
</script></body></html>
"""


# ---------------------------------------------------------------------------------------
# commands


def setup_logging(verbose: bool):
    APP_DIR.mkdir(parents=True, exist_ok=True)
    handlers: list[logging.Handler] = [logging.handlers.RotatingFileHandler(
        APP_DIR / "wledlink.log", maxBytes=1_000_000, backupCount=2, encoding="utf-8")]
    if sys.stderr is not None:  # pythonw has no console
        handlers.append(logging.StreamHandler())
    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S", handlers=handlers)


async def run_daemon(cfg: Config) -> int:
    bridge = Bridge(cfg)
    front = HttpFront(bridge, cfg)
    try:
        await front.start()
    except OSError as exc:
        log.error("Cannot listen on %s:%d (%s). Is wledlink already running?", cfg.listen, cfg.http_port, exc)
        return 1
    where = cfg.listen if cfg.http_port == 80 else f"{cfg.listen}:{cfg.http_port}"
    log.info("WLED Link %s: WLED will be at http://%s/ (status page: http://%s/__wledlink)", VERSION, where, where)
    await bridge.run()
    return 0


def daemon_url(cfg: Config, path: str) -> str:
    host = cfg.listen if cfg.http_port == 80 else f"{cfg.listen}:{cfg.http_port}"
    return f"http://{host}{path}"


def daemon_call(cfg: Config, path: str, body: dict | None = None, timeout: float = 10):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(daemon_url(cfg, path), data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as exc:
        return json.loads(exc.read() or b"null")


def daemon_running(cfg: Config) -> bool:
    try:
        daemon_call(cfg, "/__wledlink/status.json", timeout=2)
        return True
    except (OSError, ValueError):
        return False


@contextlib.contextmanager
def daemon_paused(cfg: Config, seconds: float = 180):
    """Makes a running wledlink release the serial ports while another tool needs them."""
    paused = False
    if daemon_running(cfg):
        daemon_call(cfg, "/__wledlink/api/pause", {"seconds": seconds})
        paused = True
        time.sleep(1.5)
    try:
        yield
    finally:
        if paused:
            with contextlib.suppress(OSError, ValueError):
                daemon_call(cfg, "/__wledlink/api/resume", {})


def cmd_status(cfg: Config) -> int:
    try:
        s = daemon_call(cfg, "/__wledlink/status.json", timeout=3)
    except (OSError, ValueError):
        print("wledlink is not running. Start it with: python wledlink.py")
        return 1
    b, w = s["bridge"], s["wled"]
    print(f"Bridge : {b['state']}" + (f" on {b.get('port')}, Wi-Fi \"{b.get('ssid')}\" / \"{b.get('password')}\", channel {b.get('channel')}" if b.get("ssid") else ""))
    print("Devices: " + (", ".join(f"{x['mac']} {x['ip']} ({x['rssi']} dBm)" for x in s["stations"]) or "none joined"))
    if w:
        print(f"WLED   : \"{w['name']}\" v{w['version']}, {w['leds']} LEDs -> http://{s['listen']}/")
    else:
        print("WLED   : not reachable yet")
    st = s["stats"]
    print(f"Stats  : {st.get('udp_forwarded', 0)} colour packets forwarded "
          f"({st.get('udp_unreachable', 0)} while WLED was unreachable, {st.get('udp_dropped', 0) + st.get('udpQueueDrops', 0)} dropped), "
          f"{st.get('resyncs', 0)} resyncs, {st.get('link_lost', 0)} disconnects")
    print(f"Status page: {daemon_url(cfg, '/__wledlink')}")
    return 0


def cmd_ports() -> int:
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found. Is the ESP32 plugged in with a data (not charge-only) USB cable?")
    for p in ports:
        ids = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----:----"
        likely = "  <- ESP32-style USB serial" if (p.vid, p.pid) in USB_SERIAL_IDS else ""
        print(f"{p.device:8} {ids}  {p.description}{likely}")
    return 0


def cmd_bridge_config(cfg: Config, args) -> int:
    try:
        current = daemon_call(cfg, "/__wledlink/status.json", timeout=3)["bridge"]
    except (OSError, ValueError):
        print("wledlink is not running. Start it (python wledlink.py) with the bridge plugged in, then try again.")
        return 1
    if args.reset:
        body = {"reset": True}
    else:
        body = {"ssid": args.ssid or current.get("ssid"),
                "password": args.password or current.get("password"),
                "channel": (current.get("channelSetting") or 0) if args.channel is None else args.channel,
                "hidden": current.get("hidden") if args.hidden is None else args.hidden,
                "txPowerDbm": (current.get("txPowerDbm") or 8.5) if args.txpower is None else args.txpower,
                "wifiWithPc": current.get("wifiWithPc") if args.wifi is None else args.wifi == "with-pc"}
    result = daemon_call(cfg, "/__wledlink/api/bridge-config", body) or {}
    print(result.get("msg") or result.get("error") or result)
    if result.get("ok") and not args.reset and (body["ssid"], body["password"]) != (current.get("ssid"), current.get("password")):
        print("Remember to give WLED the new network name/password too (python wledlink.py wled-wifi).")
    return 0 if result.get("ok") else 1


def cmd_light(cfg: Config, state: dict) -> int:
    req = urllib.request.Request(daemon_url(cfg, "/json/state"), data=json.dumps(state).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            resp.read()
    except urllib.error.HTTPError as exc:
        print(f"WLED is not reachable: {json.loads(exc.read() or b'{}').get('error', exc)}")
        return 1
    except OSError as exc:
        print(f"wledlink is not running ({exc}). Start it with: python wledlink.py")
        return 1
    return 0


def cmd_api(cfg: Config, path: str, body: dict) -> int:
    try:
        result = daemon_call(cfg, path, body, timeout=30) or {}
    except (OSError, ValueError) as exc:
        print(f"wledlink is not running ({exc}). Start it with: python wledlink.py")
        return 1
    for note in result.get("notes", []):
        print(note)
    if "result" in result:
        print(json.dumps(result["result"]))
    if not result.get("ok"):
        print(f"Didn't work: {result.get('msg') or result.get('error') or result}")
        return 1
    return 0


def firmware_default(name: str) -> str | None:
    """Reads a #define from the bridge's config.h (used when wledlink isn't running)."""
    with contextlib.suppress(OSError):
        m = re.search(rf'#define\s+{name}\s+"([^"]*)"', FIRMWARE_CONFIG.read_text())
        return m.group(1) if m else None
    return None


def pick_port(explicit: str | None, purpose: str) -> str | None:
    if explicit:
        return explicit
    ports = usb_serial_ports()
    if len(ports) == 1:
        print(f"Using {ports[0]} ({purpose}).")
        return ports[0]
    if not ports:
        print("No USB serial ports found. Plug the board in with a data USB cable (see: python wledlink.py ports).")
    else:
        print(f"Several USB serial ports found ({', '.join(ports)}). Pick one with --port.")
    return None


def query_wled_version(ser, wait: float = 1.5) -> str | None:
    """WLED answers "v" on its serial port (115200 baud) with its version."""
    ser.reset_input_buffer()
    ser.write(b"v")
    buf, deadline = b"", time.monotonic() + wait
    while time.monotonic() < deadline:
        buf += ser.read(256)
        if m := re.search(rb"WLED ([0-9][^\r\n ]*)", buf):
            return m.group(1).decode(errors="replace")
    return None


def wled_version_on(port: str) -> str | None:
    try:
        ser = open_serial(port, 115200)
    except (OSError, serial.SerialException):
        return None
    try:
        return query_wled_version(ser)
    finally:
        ser.close()


def improv_packet(ptype: int, data: bytes) -> bytes:
    pkt = b"IMPROV" + bytes([1, ptype, len(data)]) + data
    return pkt + bytes([sum(pkt) & 0xFF])


def cmd_wled_wifi(cfg: Config, args) -> int:
    """Points a USB-connected WLED board at the bridge's Wi-Fi using WLED's Improv serial protocol."""
    ssid, password = args.ssid, args.password
    if not ssid or not password:
        with contextlib.suppress(OSError, ValueError, KeyError, TypeError):
            b = daemon_call(cfg, "/__wledlink/status.json", timeout=2)["bridge"]
            ssid, password = ssid or b.get("ssid"), password or b.get("password")
        ssid = ssid or firmware_default("WL_DEFAULT_SSID")
        password = password or firmware_default("WL_DEFAULT_PASS")
    if not ssid or not password:
        print("Pass --ssid and --password.")
        return 1
    with daemon_paused(cfg):
        port = pick_port(args.port, "WLED board")
        if not port:
            return 1
        try:
            ser = open_serial(port, 115200)
        except (OSError, serial.SerialException) as exc:
            print(f"Cannot open {port}: {exc}")
            return 1
        try:
            version = None
            for _ in range(10):  # plugging the board in (or opening the port) may have just reset it
                if version := query_wled_version(ser):
                    break
            if not version:
                print(f"No WLED answered on {port}. Is this the WLED board, and is nothing else (like a serial monitor) using the port?")
                return 1
            print(f"WLED {version} on {port}: setting its Wi-Fi to \"{ssid}\"...")
            rpc = bytes([len(ssid)]) + ssid.encode() + bytes([len(password)]) + password.encode()
            ser.reset_input_buffer()
            ser.write(improv_packet(0x03, bytes([0x01, len(rpc)]) + rpc))
            buf, deadline, provisioning = b"", time.monotonic() + 45, False
            while time.monotonic() < deadline:
                buf += ser.read(256)
                while (i := buf.find(b"IMPROV")) >= 0 and len(buf) >= i + 9 and len(buf) >= i + 10 + buf[i + 8]:
                    ptype, data = buf[i + 7], buf[i + 9:i + 9 + buf[i + 8]]
                    buf = buf[i + 10 + buf[i + 8]:]
                    if ptype == 0x01 and data[:1] == b"\x03" and not provisioning:
                        provisioning = True
                        print("WLED saved the settings and is connecting...")
                    elif ptype == 0x01 and data[:1] == b"\x04":
                        print("WLED is connected to the bridge Wi-Fi.")
                        print(f"Once wledlink is running it will be at http://{cfg.listen}/")
                        return 0
                    elif ptype == 0x02 and data[:1] == b"\x03":
                        print(f"WLED could not connect to \"{ssid}\". Is the bridge ESP32 plugged in?")
                        return 1
            if provisioning:
                print("WLED has the new settings but hasn't reported a connection yet. Check the status page once the bridge is running.")
                return 0
            print("WLED didn't respond to the Wi-Fi command.")
            return 1
        finally:
            ser.close()


def cmd_flash_bridge(cfg: Config, args) -> int:
    if importlib.util.find_spec("esptool") is None:
        print("esptool is needed for flashing. Install it with:  python -m pip install esptool")
        return 1
    if not FIRMWARE_BIN.exists():
        print(f"No firmware image at {FIRMWARE_BIN}. Build it with PlatformIO first (see README).")
        return 1
    with daemon_paused(cfg):
        port = pick_port(args.port, "bridge board")
        if not port:
            return 1
        if not args.force and (version := wled_version_on(port)):
            print(f"{port} is running WLED {version}. That's the board for the lights, not the bridge!")
            print("Pick the spare board's port, or add --force if you really want to overwrite WLED.")
            return 1
        from importlib.metadata import version as package_version
        write = "write-flash" if int(package_version("esptool").split(".")[0]) >= 5 else "write_flash"
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", port, "--baud", "460800",
               write, "0x0", str(FIRMWARE_BIN)]
        print(" ".join(cmd))
        return subprocess.call(cmd)


def load_config(args) -> Config:
    cfg = Config()
    if CONFIG_FILE.exists():
        data = json.loads(CONFIG_FILE.read_text())
        for key, value in data.items():
            if hasattr(cfg, key):
                setattr(cfg, key, tuple(value) if key == "udp_ports" else value)
    # --port on other commands names a different board, so only "run" may set the bridge port.
    keys = ("listen", "http_port") + (("port", "baud", "wled_mac") if args.command in (None, "run") else ())
    for key in keys:
        if getattr(args, key, None) is not None:
            setattr(cfg, key, getattr(args, key))
    if getattr(args, "no_time_sync", False):
        cfg.time_sync = False
    cfg.verbose = getattr(args, "verbose", False)
    return cfg


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Reach WLED through a USB-connected ESP32 bridge.")
    parser.add_argument("--listen", help=f"loopback address to serve WLED on (default {DEFAULT_LISTEN})")
    parser.add_argument("--http-port", type=int, help="HTTP port (default 80; SignalRGB needs 80)")
    sub = parser.add_subparsers(dest="command")

    run = sub.add_parser("run", help="start the link (default)")
    run.add_argument("--port", help="bridge serial port, e.g. COM5 (default: find it automatically)")
    run.add_argument("--baud", type=int, help=f"serial speed (default {DEFAULT_BAUD})")
    run.add_argument("--wled-mac", help="prefer this WLED if several devices join the bridge")
    run.add_argument("--no-time-sync", action="store_true", help="don't send the PC's clock to WLED")
    run.add_argument("-v", "--verbose", action="store_true")

    sub.add_parser("status", help="show what the running link is doing")
    sub.add_parser("stop", help="stop the running link (e.g. one running in the background)")
    sub.add_parser("ports", help="list serial ports")

    cfgp = sub.add_parser("bridge-config", help="change the bridge's Wi-Fi (bridge restarts)")
    cfgp.add_argument("--ssid")
    cfgp.add_argument("--password")
    cfgp.add_argument("--channel", type=int, help="0 = automatic, or 1-11")
    cfgp.add_argument("--hidden", action=argparse.BooleanOptionalAction, default=None)
    cfgp.add_argument("--txpower", type=float, help="TX power in dBm (2-19.5, default 8.5)")
    cfgp.add_argument("--wifi", choices=["always", "with-pc"],
                      help="always = on whenever the bridge has power (default, lets a phone reach WLED with the PC off); "
                           "with-pc = only while this program runs")
    cfgp.add_argument("--reset", action="store_true", help="go back to the defaults compiled into the firmware")

    white = sub.add_parser("white", help="override SignalRGB with white from the white LEDs")
    white.add_argument("tone", nargs="?", default="neutral", choices=list(WHITE_TONES),
                       help="neutral = warm + cool white both at full (default), cool or warm = just that one")
    white.add_argument("--brightness", type=int, default=100, help="percent, default 100")
    sub.add_parser("signalrgb", help="hand the lights back to SignalRGB")
    sub.add_parser("off", help="turn the lights off")
    phone = sub.add_parser("phone", help="Bluetooth phone control: pair a phone, forget phones, change the PIN")
    phone.add_argument("action", choices=["pair", "forget", "pin", "test"])
    phone.add_argument("value", nargs="?", help="pin: the new 6-digit PIN; test: WLED JSON, e.g. '{\"on\":true}'")
    hotspot = sub.add_parser("wled-hotspot", help="when WLED opens its own WLED-AP hotspot")
    hotspot.add_argument("mode", choices=list(HOTSPOT_MODES),
                         help="off = never; boot = only if it starts without the bridge (WLED default); "
                              "fallback = whenever it loses the bridge")

    wifi = sub.add_parser("wled-wifi", help="point a USB-connected WLED board at the bridge Wi-Fi (Improv)")
    wifi.add_argument("--port", help="the WLED board's serial port")
    wifi.add_argument("--ssid")
    wifi.add_argument("--password")

    flash = sub.add_parser("flash-bridge", help="flash the bridge firmware onto a spare ESP32")
    flash.add_argument("--port", help="the spare board's serial port")
    flash.add_argument("--force", action="store_true", help="flash even if the board is running WLED")

    args = parser.parse_args(argv)
    if serial is None:
        print("pyserial is missing. Install it with:  python -m pip install pyserial")
        return 1
    cfg = load_config(args)
    command = args.command or "run"

    if command == "run":
        setup_logging(cfg.verbose)
        try:
            return asyncio.run(run_daemon(cfg))
        except KeyboardInterrupt:
            return 0
    if command == "status":
        return cmd_status(cfg)
    if command == "stop":
        if (rc := cmd_api(cfg, "/__wledlink/api/quit", {})) == 0:
            print("wledlink stopped. Start it again with: python wledlink.py")
        return rc
    if command == "ports":
        return cmd_ports()
    if command == "bridge-config":
        return cmd_bridge_config(cfg, args)
    if command == "white":
        return cmd_api(cfg, "/__wledlink/api/white", {"tone": args.tone, "brightness": args.brightness})
    if command == "phone":
        body = {"seconds": 120}
        if args.action == "pin":
            body = {"pin": args.value or ""}
        elif args.action == "test":
            body = {"state": json.loads(args.value or "{}")}
        return cmd_api(cfg, f"/__wledlink/api/phone-{args.action}", body)
    if command == "wled-hotspot":
        return cmd_api(cfg, "/__wledlink/api/wled-hotspot", {"mode": args.mode})
    if command == "signalrgb":
        return cmd_light(cfg, {"lor": 0, "on": True})
    if command == "off":
        return cmd_light(cfg, {"on": False})
    if command == "wled-wifi":
        return cmd_wled_wifi(cfg, args)
    if command == "flash-bridge":
        return cmd_flash_bridge(cfg, args)
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
