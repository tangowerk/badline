#!/usr/bin/env python3
"""A/B check of the video renderers (M-VK1): rendered screenshots of OpenGL and Vulkan.

  tests/renderer_ab.py [--binary target/release/badline] [--port 6466]

Starts a window instance (own XDG_CONFIG_HOME, fresh C64 config with nearest filter and
integer scaling), freezes a static picture and compares rendered screenshots after runtime
switches GL -> Vulkan -> GL:
  - picture without overlay: bit-identical (required)
  - with the overlay open: reported as differing pixels / max channel difference (visual check)
  - second variant (linear filter, rotation, free aspect): nearly identical
Without validation layers installed the validation check passes trivially.
"""

import argparse
import base64
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zlib

PASS = 0
FAIL = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name} {detail}")


class Client:
    def __init__(self, port):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.session = None
        self.next_id = 1

    def rpc(self, method, params=None, timeout=120):
        body = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        self.next_id += 1
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, json.dumps(body).encode(), headers)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            return json.loads(r.read())

    def call(self, name, **args):
        return self.rpc("tools/call", {"name": name, "arguments": args})["result"]


def text_of(result):
    return "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")


def image_of(result):
    for c in result.get("content", []):
        if c.get("type") == "image":
            return base64.b64decode(c["data"])
    return None


def decode_png(data):
    """-> (width, height, channels, bytes) for 8 bit RGB/RGBA PNGs."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8 and ctype in (2, 6), (depth, ctype)
            ch = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * ch
    out = bytearray(stride * height)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, ch, bytes(out)


def compare(a, b):
    """-> (same size, differing pixels, max channel difference)"""
    wa, ha, ca, pa = a
    wb, hb, cb, pb = b
    if (wa, ha) != (wb, hb):
        return False, None, None
    diff, maxd = 0, 0
    for i in range(wa * ha):
        da = pa[i * ca:i * ca + 3]
        db = pb[i * cb:i * cb + 3]
        if da != db:
            diff += 1
            maxd = max(maxd, max(abs(x - y) for x, y in zip(da, db)))
    return True, diff, maxd


def shot(client, label, save_dir):
    r = client.call("screenshot", rendered=True)
    png = image_of(r)
    if png is None:
        check(f"{label}: rendered screenshot", False, text_of(r))
        return None
    with open(os.path.join(save_dir, label + ".png"), "wb") as f:
        f.write(png)
    return decode_png(png)


def switch(client, renderer):
    r = client.call("renderer", set=renderer)
    ok = not r.get("isError") and json.loads(text_of(r)).get("renderer") == renderer
    check(f"switch to {renderer}", ok, text_of(r))
    # the switch shows the last frame again only after the next emulated frame
    client.call("run_frames", frames=2)
    return ok


# (name, C64 config lines, picture must be bit-identical)
VARIANTS = [
    ("nearest-integer", "video_filter:0\ninteger_scaling:1\n", True),
    ("linear-rotated-free", "video_filter:1\nrotation:1\naspect_mode:3\n", False),
]


def run_variant(args, name, config, exact, base):
    print(name)
    cfgdir = os.path.join(base, name)
    os.makedirs(cfgdir)
    cfg = os.path.join(cfgdir, "c64.ini")
    with open(cfg, "w") as f:
        f.write(config)
    env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
    proc = subprocess.Popen([args.binary, "-config-c64", cfg, "-renderer", "gl", "-mcp", str(args.port)],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    client = Client(args.port)
    try:
        for _ in range(100):
            try:
                client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "ab", "version": "1"}})
                break
            except OSError:
                time.sleep(0.1)
        r = client.call("renderer")
        check("starts with gl", json.loads(text_of(r)).get("renderer") == "gl", text_of(r))

        client.call("reset", type="hard")
        r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
        check("READY.", not r.get("isError"), text_of(r))
        # static picture: cursor blink off, a row of reversed spaces in all colours
        client.call("memory_write", address="$cc", bytes=[1])
        for i in range(16):
            client.call("memory_write", address=0xd800 + 200 + i, bytes=[i], bank="cpu")
            client.call("memory_write", address=0x0400 + 200 + i, bytes=[0xa0])
        client.call("run_frames", frames=5)
        client.call("pause")

        gl = shot(client, "gl", cfgdir)
        switch(client, "vulkan")
        vk = shot(client, "vulkan", cfgdir)
        if gl and vk:
            same, diff, maxd = compare(gl, vk)
            total = gl[0] * gl[1]
            print(f"       picture: {diff} of {total} pixels differ, max channel difference {maxd}")
            if exact:
                check(f"picture bit-identical ({gl[0]}x{gl[1]})", same and diff == 0,
                      f"size {gl[:2]} vs {vk[:2]}" if not same else f"{diff} pixels differ, max {maxd}")
            else:
                check(f"picture same size ({gl[0]}x{gl[1]})", same, f"{gl[:2]} vs {vk[:2]}")
                check("picture nearly identical (< 1 % pixels, max diff <= 2)",
                      same and diff * 100 < total and (maxd or 0) <= 2, f"{diff} pixels, max {maxd}")

        client.call("overlay", action="open")
        time.sleep(0.6)  # fade in
        vk_ov = shot(client, "vulkan_overlay", cfgdir)
        switch(client, "gl")
        time.sleep(0.3)
        gl_ov = shot(client, "gl_overlay", cfgdir)
        if gl_ov and vk_ov:
            same, diff, maxd = compare(gl_ov, vk_ov)
            total = gl_ov[0] * gl_ov[1]
            print(f"       overlay: {diff} of {total} pixels differ, max channel difference {maxd}")
            check("overlay screenshot same size", same)
            check("overlay nearly identical (< 1 % pixels, max diff <= 2)",
                  same and diff * 100 < total and (maxd or 0) <= 2, f"{diff} pixels, max {maxd}")
        client.call("overlay", action="close")
        time.sleep(0.6)

        gl2 = shot(client, "gl_again", cfgdir)
        if gl and gl2:
            same, diff, _ = compare(gl, gl2)
            check("GL picture unchanged after the round trip", same and diff == 0, f"{diff} pixels differ")

        if exact:
            # screen text: "CPU Jam" message (bottom right), messages do not survive a switch
            jams = {}
            for renderer in ("gl", "vulkan"):
                if renderer == "vulkan":
                    switch(client, "vulkan")
                client.call("resume")
                client.call("reset", type="hard")
                client.call("wait_for_text", text="READY.", timeout=10, warp=True)
                client.call("memory_write", address="$c000", bytes=[2])
                client.call("type_text", text="SYS 49152\n")
                client.call("run_frames", frames=60)
                jams[renderer] = shot(client, "jam_" + renderer, cfgdir)
            a, b = jams["gl"], jams["vulkan"]
            if a and b and a[:3] == b[:3]:
                w, h, ch = a[:3]
                rows = h // 6  # message area: bottom of the picture
                ra = (w, rows, ch, a[3][(h - rows) * w * ch:])
                rb = (w, rows, ch, b[3][(h - rows) * w * ch:])
                same, diff, maxd = compare(ra, rb)
                has_text = len(set(ra[3][i:i + 3] for i in range(0, len(ra[3]), ch))) > 2
                check("screen text shown (GL)", has_text)
                check("screen text bit-identical", same and diff == 0, f"{diff} pixels differ, max {maxd}")
            else:
                check("screen text screenshots", False, "missing or different size")

        client.call("quit")
        proc.wait(10)
        stderr = proc.stderr.read()
        errors = [l for l in stderr.splitlines() if "validation error" in l]
        check("no Vulkan validation errors", not errors, "\n".join(errors[:5]))
        proc = None
    finally:
        if proc:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "..", "target", "release", "badline"))
    ap.add_argument("--port", type=int, default=6466)
    ap.add_argument("--keep", action="store_true", help="keep the screenshots (printed folder)")
    args = ap.parse_args()

    base = tempfile.mkdtemp(prefix="badline-ab-")
    for name, config, exact in VARIANTS:
        run_variant(args, name, config, exact, base)

    if args.keep:
        print(f"screenshots in {base}")
    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
