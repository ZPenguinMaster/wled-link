"""Runs the simulated bridge + WLED so wledlink.py can be tried without hardware.

    python pc/tests/demo.py            (prints the socket:// port to use)
    python pc/wledlink.py run --port socket://127.0.0.1:<port>
"""

import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_bridge import FakeBridge  # noqa: E402
from fake_wled import FakeWled  # noqa: E402


async def main():
    wled = FakeWled()
    await wled.start()
    bridge = FakeBridge(wled.http_addr, wled.udp_addr)
    await bridge.start()
    print(f"simulated bridge: socket://127.0.0.1:{bridge.port}", flush=True)
    last = 0
    while True:
        await asyncio.sleep(2)
        if len(wled.udp_packets) != last:
            last = len(wled.udp_packets)
            print(f"colour packets received by the fake WLED: {last}", flush=True)
        if wled.posts:
            print(f"state posts: {wled.posts[-3:]}", flush=True)
            wled.posts.clear()


asyncio.run(main())
