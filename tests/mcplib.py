"""Shared helpers of the window test scripts: MCP client, check counters, PNG decoding.

Used by tests/color_check.py and tests/slang_presets.py (formerly part of the GL/Vulkan A/B
check renderer_ab.py, removed with the OpenGL renderer in M-VK3d).
"""

import base64
import json
import struct
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


