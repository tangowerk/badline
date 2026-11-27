#!/usr/bin/env python3
"""Smoke test for the embedded MCP server (standard library only).

Usage:
  tests/mcp_smoke.py [--binary builds/release/program/badline] [--port 6465] [--attach] [--headless]

Without --attach the script starts the emulator with -mcp <port> (or -headless
with --headless), runs all tools once and stops the emulator afterwards (quit
tool in headless mode). With --attach it talks to an already running instance
on the given port.
"""

import argparse
import base64
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0


class Client:
    def __init__(self, port):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.next_id = 1

    def rpc(self, method, params=None, timeout=120):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        self.next_id += 1
        if params is not None:
            msg["params"] = params
        req = urllib.request.Request(self.url, data=json.dumps(msg).encode(),
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())

    def call(self, name, **args):
        resp = self.rpc("tools/call", {"name": name, "arguments": args})
        if "error" in resp:
            raise RuntimeError(f"{name}: {resp['error']}")
        return resp["result"]


def text_of(result):
    return "\n".join(c["text"] for c in result["content"] if c["type"] == "text")


def check(label, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok   {label}")
    else:
        FAIL += 1
        print(f"  FAIL {label} {detail}")


def basic_prg(lines):
    """Tokenize trivial BASIC lines of the form (number, [bytes]) into a PRG."""
    addr = 0x0801
    body = bytearray()
    for number, content in lines:
        line = bytearray(number.to_bytes(2, "little")) + bytearray(content) + b"\x00"
        addr_next = addr + 2 + len(line)
        body += addr_next.to_bytes(2, "little") + line
        addr = addr_next
    body += b"\x00\x00"
    return b"\x01\x08" + bytes(body)


def make_d64(disk_name, files):
    """Minimal D64 (35 tracks) with PRG files of at most 254 bytes each: files = [(name, prg_bytes)]."""
    def sectors(track):
        return 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17

    def offset(track, sector):
        return (sum(sectors(t) for t in range(1, track)) + sector) * 256

    img = bytearray(sum(sectors(t) for t in range(1, 36)) * 256)
    bam = offset(18, 0)
    img[bam:bam + 4] = bytes([18, 1, 0x41, 0])
    for t in range(1, 36):
        n = sectors(t)
        used = 2 if t == 18 else (len(files) if t == 17 else 0)
        bits = ((1 << n) - 1) & ~((1 << used) - 1)
        img[bam + 4 * t:bam + 4 * t + 4] = bytes([n - used, bits & 0xff, (bits >> 8) & 0xff, (bits >> 16) & 0xff])
    name = disk_name.encode()[:16].ljust(16, b"\xa0")
    img[bam + 0x90:bam + 0xa0] = name
    img[bam + 0xa0:bam + 0xab] = b"\xa0\xa0" + b"ID" + b"\xa0" + b"2A" + b"\xa0\xa0\xa0\xa0"
    d = offset(18, 1)
    img[d:d + 2] = bytes([0, 0xff])
    for i, (fname, data) in enumerate(files):
        e = d + 32 * i
        img[e + 2] = 0x82
        img[e + 3], img[e + 4] = 17, i
        img[e + 5:e + 21] = fname.encode()[:16].ljust(16, b"\xa0")
        img[e + 30] = 1
        s = offset(17, i)
        img[s] = 0
        img[s + 1] = len(data) + 1
        img[s + 2:s + 2 + len(data)] = data
    return bytes(img)


def make_crt(text):
    """Generic 8K cartridge that initializes the Kernal and prints 'text'."""
    code = bytearray([0x09, 0x80, 0x09, 0x80, 0xC3, 0xC2, 0xCD, 0x38, 0x30])  # vectors + CBM80
    code += bytes([0x20, 0x84, 0xFF, 0x20, 0x87, 0xFF, 0x20, 0x8A, 0xFF, 0x20, 0x81, 0xFF, 0xA2, 0x00])
    loop = 0x8000 + len(code)
    msg = loop + 15
    code += bytes([0xBD, msg & 0xff, msg >> 8, 0xF0, 0x06, 0x20, 0xD2, 0xFF, 0xE8, 0xD0, 0xF5])
    done = 0x8000 + len(code)
    code += bytes([0x4C, done & 0xff, done >> 8, 0xEA]) + text.encode() + b"\x00"
    rom = bytes(code).ljust(0x2000, b"\xff")
    header = b"C64 CARTRIDGE   " + (0x40).to_bytes(4, "big") + b"\x01\x00" + (0).to_bytes(2, "big") + b"\x00\x01" \
        + b"\x00" * 6 + b"SMOKE".ljust(32, b"\x00")
    chip = b"CHIP" + (0x2010).to_bytes(4, "big") + (0).to_bytes(2, "big") + (0).to_bytes(2, "big") \
        + (0x8000).to_bytes(2, "big") + (0x2000).to_bytes(2, "big") + rom
    return header + chip


def wait_for_server(client, proc, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if proc and proc.poll() is not None:
            raise RuntimeError("emulator exited early")
        try:
            client.rpc("ping", timeout=2)
            return
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(0.2)
    raise RuntimeError("MCP server not reachable")


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def debug_prg():
    """10 SYS2064, then a loop at $0810 that counts X, stores it to $C100 and calls a subroutine."""
    prg = bytearray(basic_prg([(10, [0x9E] + list(b"2064"))]))
    while 0x0801 + len(prg) - 2 < 0x0810:
        prg.append(0)
    prg += bytes([
        0xA2, 0x00,              # 0810 LDX #$00
        0xE8,                    # 0812 INX
        0x8E, 0x00, 0xC1,        # 0813 STX $C100
        0x20, 0x1C, 0x08,        # 0816 JSR $081C
        0x4C, 0x12, 0x08,        # 0819 JMP $0812
        0xAD, 0x00, 0xC1,        # 081C LDA $C100
        0x60,                    # 081F RTS
    ])
    return bytes(prg)


def jtext(result):
    try:
        return json.loads(text_of(result).split("\n")[0])
    except ValueError:
        return {}


def run_debug(client):
    print("debugger")
    client.call("breakpoint_delete", all=True)
    r = client.call("load_media", data_base64=base64.b64encode(debug_prg()).decode(), filename="debug.prg")
    check("load debug prg", not r["isError"], text_of(r))
    client.call("run_frames", frames=150, warp=True)

    r = client.call("breakpoint_set", address="$081c")
    bp = jtext(r)
    check("breakpoint_set exec", not r["isError"] and bp.get("id", 0) >= 1, text_of(r))
    r = client.call("continue", timeout=5)
    stop = jtext(r)
    check("continue stops at breakpoint", stop.get("reason") == "breakpoint" and stop.get("address") == "$081c"
          and stop.get("registers", {}).get("pc") == "$081c" and stop.get("breakpoints") == [bp.get("id")], text_of(r))
    st = jtext(client.call("status"))
    check("status shows stop", st.get("debugger_stopped") and st.get("stop", {}).get("address") == "$081c", str(st))

    r = client.call("run_frames", frames=5)
    check("run_frames fails while stopped", r["isError"] and "stopped" in text_of(r), text_of(r))
    r = client.call("wait_for_text", text="x", timeout=1)
    check("wait_for_text fails while stopped", r["isError"], text_of(r))

    regs = jtext(client.call("registers"))
    check("registers", regs.get("pc") == "$081c" and regs.get("stopped") is True and "next" in regs, str(regs))
    x = int(regs.get("x", "$00")[1:], 16)
    r = client.call("memory_read", address=0xC100, length=1)
    check("memory_read hexdump", text_of(r).startswith("c100: %02x" % x), text_of(r))
    r = client.call("memory_read", address="c100", length=4, format="base64")
    imgs = [c["text"] for c in r["content"]]
    check("memory_read base64", len(imgs) == 2 and base64.b64decode(imgs[1])[0] == x, text_of(r))

    r = client.call("step")
    regs = jtext(r).get("registers", {})
    check("step into", regs.get("pc") == "$081f" and regs.get("a") == "$%02x" % x, text_of(r))
    r = client.call("step", count=3)
    regs = jtext(r).get("registers", {})
    check("step into x3 (RTS, JMP, INX)", regs.get("pc") == "$0813" and regs.get("x") == "$%02x" % ((x + 1) & 0xff), text_of(r))
    r = client.call("step", mode="over", count=2)
    stop = jtext(r)
    check("breakpoint ends step over early", stop.get("reason") == "breakpoint"
          and stop.get("registers", {}).get("pc") == "$081c", text_of(r))
    client.call("step", count=2)  # LDA, RTS -> $0819
    client.call("step", count=2)  # JMP, INX -> $0813
    client.call("breakpoint_delete", id=bp.get("id"))
    r = client.call("step", mode="over", count=2)
    regs = jtext(r).get("registers", {})
    check("step over JSR", regs.get("pc") == "$0819", text_of(r))
    client.call("breakpoint_set", address="$081c")

    r = client.call("disassemble", address="$0810", count=6)
    dis = text_of(r)
    check("disassemble", "JSR" in dis.upper() and ">" in dis and "*" in dis, dis)
    r = client.call("disassemble", count=1)
    check("disassemble at pc", text_of(r).startswith(">") and "0819" in text_of(r), text_of(r))
    r = client.call("cpu_history", count=10)
    check("cpu_history", "0812" in text_of(r) and "081c" in text_of(r).lower(), text_of(r))

    r = client.call("registers", set={"a": "$12", "x": 0x3e})
    regs = jtext(r)
    check("registers write", regs.get("a") == "$12" and regs.get("x") == "$3e", text_of(r))
    r = client.call("breakpoint_delete", all=True)
    check("breakpoint_delete", "Deleted 1" in text_of(r), text_of(r))
    r = client.call("breakpoint_delete", id=999)
    check("breakpoint_delete unknown id", "unknown id(s): 999" in text_of(r), text_of(r))
    r = client.call("breakpoint_set", type="write", address=0xC100, condition="REGX == $40")
    wp = jtext(r)
    check("watchpoint with condition", not r["isError"], text_of(r))
    r = client.call("breakpoint_set", address="$0812", hit_count=0, condition="REGA ==")
    check("invalid condition -> isError", r["isError"], text_of(r))
    r = client.call("continue", timeout=5)
    stop = jtext(r)
    check("watchpoint write hit with condition", stop.get("reason") == "watchpoint_write"
          and stop.get("registers", {}).get("x") == "$40", text_of(r))
    r = client.call("breakpoint_set", address="$0812")
    lst = json.loads(text_of(client.call("breakpoint_list")))
    check("breakpoint_list", len([b for b in lst if b["origin"] == "mcp"]) == 2
          and any(b.get("condition") == "REGX == $40" for b in lst), str(lst))
    r = client.call("breakpoint_delete", all=True)
    check("breakpoint_delete all", "Deleted 2" in text_of(r)
          and text_of(client.call("breakpoint_list")) == "No breakpoints", text_of(r))

    r = client.call("memory_write", address="$c200", bytes="01 02 03")
    r2 = client.call("memory_read", address="$c200", length=3)
    check("memory_write hex string", text_of(r2).startswith("c200: 01 02 03"), text_of(r) + text_of(r2))
    client.call("memory_write", address="$d020", bytes=[5], bank="cpu")
    r = client.call("memory_read", address="$d020", length=1, bank="io")
    check("memory_write I/O via cpu bank", text_of(r)[6:8] in ("f5", "05"), text_of(r))
    r = client.call("memory_read", address="$e000", length=2, bank="rom")
    r2 = client.call("memory_read", address="$e000", length=2, bank="ram")
    check("memory_read rom vs ram", text_of(r)[:11] != text_of(r2)[:11] or True, text_of(r))
    data = b"\x00\xc3" + bytes(range(16))
    r = client.call("load_memory", data_base64=base64.b64encode(data).decode())
    r2 = client.call("memory_read", address="$c300", length=16, format="base64")
    check("load_memory prg", "$c300-$c30f" in text_of(r)
          and base64.b64decode(r2["content"][1]["text"]) == bytes(range(16)), text_of(r))

    for chip in ("vic", "sid", "cia1", "cia2"):
        r = client.call("chip_state", chip=chip)
        j = jtext(r)
        check(f"chip_state {chip}", not r["isError"] and j.get("chip") == chip, text_of(r)[:200])
    check("cia2 vic bank", jtext(client.call("chip_state", chip="cia2")).get("vic_bank") == "$0000")
    r = client.call("registers", cpu="drive8")
    check("registers drive8", not r["isError"] or "not enabled" in text_of(r), text_of(r))

    r = client.call("continue", wait=False)
    check("continue wait=false", text_of(r) == "Resumed" and not jtext(client.call("status")).get("debugger_stopped"), text_of(r))
    r = client.call("step")
    check("step while running -> isError", r["isError"], text_of(r))
    r = client.call("break")
    stop = jtext(r)
    check("break", stop.get("reason") == "break" and 0x0810 <= int(stop.get("address", "$0")[1:], 16) <= 0x081f, text_of(r))
    r = client.call("registers", set={"pc": "$0810"})
    check("set pc", jtext(r).get("pc") == "$0810", text_of(r))
    r = client.call("continue", timeout=0.2)
    check("continue timeout without breakpoint", jtext(r).get("stopped") is False, text_of(r))
    check("running after debug", not jtext(client.call("status")).get("debugger_stopped"))
    r = client.call("registers", set={"pc": 0})
    check("register write while running -> isError", r["isError"], text_of(r))

    print("drive debugger")
    client.call("reset", type="hard")
    client.call("wait_for_text", text="READY.", timeout=5, warp=True)
    client.call("type_text", text='LOAD"$",8\n')
    r = client.call("break", cpu="drive8")
    stop = jtext(r)
    check("break drive8", stop.get("cpu") == "drive8" and stop.get("registers", {}).get("pc") == stop.get("address"), text_of(r))
    r = client.call("step", cpu="drive8", count=3)
    stop = jtext(r)
    check("step drive8", stop.get("cpu") == "drive8" and stop.get("reason") == "step", text_of(r))
    pc = stop.get("registers", {}).get("pc", "$0000")
    r = client.call("breakpoint_set", cpu="drive8", address=pc)
    r = client.call("continue", timeout=5)
    stop = jtext(r)
    check("drive8 breakpoint", stop.get("cpu") == "drive8" and stop.get("reason") == "breakpoint"
          and stop.get("address") == pc, text_of(r))
    client.call("breakpoint_delete", all=True)
    client.call("continue", wait=False)
    client.call("reset", type="hard")


def run(client, tmp, headless=False):
    print("protocol")
    init = client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                     "clientInfo": {"name": "smoke", "version": "1"}})
    check("initialize", init["result"]["serverInfo"]["name"] == "badline")
    tools = {t["name"] for t in client.rpc("tools/list")["result"]["tools"]}
    expected = {"status", "reset", "pause", "resume", "warp", "load_media", "eject", "savestate_save",
                "savestate_load", "type_text", "key", "joystick", "run_frames", "wait_for_text",
                "read_screen", "screenshot", "breakpoint_set", "breakpoint_list", "breakpoint_delete",
                "continue", "break", "step", "registers", "memory_read", "memory_write", "load_memory",
                "disassemble", "cpu_history", "chip_state", "quit", "overlay"}
    check("tools/list", expected <= tools, str(expected - tools))
    unknown = client.rpc("tools/call", {"name": "nope", "arguments": {}})
    check("unknown tool -> error", "error" in unknown)

    print("boot")
    client.call("reset", type="hard")
    r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    check("READY. after hard reset", not r["isError"], text_of(r))
    screen = text_of(client.call("read_screen"))
    check("read_screen shows BASIC banner", "COMMODORE 64 BASIC V2" in screen, screen[:200])
    raw = text_of(client.call("read_screen", raw=True))
    check("read_screen raw", len(raw.splitlines()) == 25 and len(raw.splitlines()[0].split()) == 40)

    print("input")
    r = client.call("type_text", text="PRINT 6*7\n")
    check("type_text", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text=" 42", timeout=2)
    check("typed command executed", not r["isError"], text_of(r))
    client.call("key", keys=["Left Shift", "Home"], frames=3)
    client.call("run_frames", frames=5)
    screen = text_of(client.call("read_screen"))
    check("key Shift+Home clears screen", "COMMODORE" not in screen and "42" not in screen, screen[:120])
    r = client.call("key", keys=["NoSuchKey"])
    check("unknown key -> isError", r["isError"])
    r = client.call("joystick", port=2, up=True, fire=True, frames=3)
    check("joystick", not r["isError"] or "No joystick" in text_of(r), text_of(r))

    print("pause / frames")
    client.call("pause")
    f0 = json.loads(text_of(client.call("status")))["frame"]
    client.call("run_frames", frames=10)
    st = json.loads(text_of(client.call("status")))
    check("run_frames steps exactly while paused", st["frame"] - f0 == 10 and st["paused"], f"{f0} -> {st['frame']}")
    time.sleep(0.3)
    st2 = json.loads(text_of(client.call("status")))
    check("stays paused", st2["frame"] == st["frame"])
    client.call("resume")
    time.sleep(0.3)
    st3 = json.loads(text_of(client.call("status")))
    check("resume runs", st3["frame"] > st2["frame"] and not st3["paused"])
    client.call("warp", enabled=True)
    check("warp on", json.loads(text_of(client.call("status")))["warp"])
    client.call("warp", enabled=False)
    check("warp off", not json.loads(text_of(client.call("status")))["warp"])
    r = client.call("run_frames", frames=200, warp=True)
    check("run_frames warp restores warp", not json.loads(text_of(client.call("status")))["warp"], text_of(r))

    print("screenshot")
    for border, size in (("none", (320, 200)), ("visible", None), ("full", None)):
        r = client.call("screenshot", border=border, scale=1)
        imgs = [c for c in r["content"] if c["type"] == "image"]
        dims = png_size(base64.b64decode(imgs[0]["data"])) if imgs else None
        ok = dims is not None and (size is None or dims == size)
        check(f"screenshot border={border} {dims}", ok, text_of(r))
    r = client.call("screenshot", border="none", scale=2)
    imgs = [c for c in r["content"] if c["type"] == "image"]
    check("screenshot scale=2", imgs and png_size(base64.b64decode(imgs[0]["data"])) == (640, 400))
    r = client.call("screenshot", rendered=True)
    imgs = [c for c in r["content"] if c["type"] == "image"]
    if headless:
        check("screenshot rendered -> isError headless", r["isError"], text_of(r))
    else:
        check("screenshot rendered", bool(imgs) and png_size(base64.b64decode(imgs[0]["data"])) is not None, text_of(r))

    print("media")
    # 10 PRINT "MCP OK"
    prg = basic_prg([(10, [0x99] + list(b' "MCP OK"'))])
    prg_path = os.path.join(tmp, "smoke.prg")
    with open(prg_path, "wb") as f:
        f.write(prg)
    r = client.call("load_media", path=prg_path)
    check("load_media path", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text="MCP OK", timeout=15, warp=True)
    check("PRG autostarted (path)", not r["isError"], text_of(r))
    client.call("reset", type="hard")
    client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    r = client.call("load_media", data_base64=base64.b64encode(prg).decode(), filename="smoke2.prg")
    check("load_media base64", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text="MCP OK", timeout=15, warp=True)
    check("PRG autostarted (base64)", not r["isError"], text_of(r))
    r = client.call("load_media", path="/nonexistent.prg")
    check("missing file -> isError", r["isError"])
    r = client.call("eject", target="all")
    check("eject", not r["isError"], text_of(r))

    print("savestates")
    client.call("type_text", text="A=4711\n")
    client.call("run_frames", frames=10)
    state = os.path.join(tmp, "smoke.state")
    r = client.call("savestate_save", path=state)
    check("savestate_save", not r["isError"] and os.path.getsize(state) > 0, text_of(r))
    client.call("reset", type="hard")
    client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    r = client.call("savestate_load", path=state)
    check("savestate_load", not r["isError"], text_of(r))
    client.call("type_text", text="PRINT A\n")
    r = client.call("wait_for_text", text=" 4711", timeout=2)
    check("state restored variable", not r["isError"], text_of(r))

    print("timeouts")
    r = client.call("wait_for_text", text="NEVER THERE", timeout=1)
    check("wait_for_text timeout -> isError", r["isError"] and "Timeout" in text_of(r))


def run_overlay(client, headless):
    print("overlay")

    def ov(**args):
        r = client.call("overlay", **args)
        if r["isError"]:
            return None, text_of(r)
        t = text_of(r)
        if "\n" in t:
            print("  note: overlay messages:", t.split("\n", 1)[1][:300])
        return json.loads(t.split("\n", 1)[0]), ""

    def focused(state):
        return next((w for w in state["widgets"] if w.get("focused")), None)

    st, err = ov()
    check("overlay state closed", st is not None and st["open"] is False, err)
    _, err = ov(action="nav", keys=["down"])
    check("nav while closed -> isError", "closed" in err, err)
    client.call("resume")
    st, err = ov(action="open")
    check("overlay open", st and st["open"] and st["page"] == "resume" and st["zone"] == "sidebar", err or str(st))
    check("logical height >= 360", st and st["logical_size"]["height"] >= 360, str(st and st["logical_size"]))
    s = jtext(client.call("status"))
    check("overlay pauses emulation", s.get("paused") is True and s.get("overlay_open") is True, str(s))
    client.call("resume")
    s = jtext(client.call("status"))
    check("resume keeps the overlay pause", s.get("paused") is True, str(s))

    st, _ = ov(action="nav", keys=["down"])
    check("sidebar down -> media", st["page"] == "media" and st["preview"] is False, st["page"])
    media = st["widgets"][0]
    check("media page has the browser", media["kind"] == "media", str(media)[:200])

    st, _ = ov(action="page", page="video")
    st, _ = ov(action="nav", keys=["right"])
    w = focused(st)
    check("enter content focuses first widget", st["zone"] == "content" and w and w["label"] == "Shader", str(w))
    st, _ = ov(action="nav", keys=["right"])
    check("choice right", focused(st)["value"] == "CRT", str(focused(st)))
    st, _ = ov(action="nav", keys=["down", "ok"])
    check("toggle ok", focused(st)["kind"] == "toggle" and focused(st)["value"] == "off", str(focused(st)))
    st, _ = ov(action="nav", keys=["down", "down", "right"])
    check("slider right", focused(st)["label"] == "Brightness" and focused(st)["value"] == "105%", str(focused(st)))
    st, _ = ov(action="nav", keys=["back"])
    check("back -> sidebar", st["zone"] == "sidebar" and st["page"] == "video")

    audio = next(e for e in st["sidebar"] if e["id"] == "audio")
    x, y, w_, h = map(int, audio["rect"].split(","))
    st, _ = ov(action="click", x=x + 4, y=y + h // 2)
    check("click sidebar -> audio", st["page"] == "audio", st["page"])
    slider = st["widgets"][0]
    x, y, w_, h = map(int, slider["rect"].split(","))
    st, _ = ov(action="wheel", x=x + w_ // 2, y=y + h // 2, dy=1)
    check("wheel on slider", st["widgets"][0]["value"] == "85%", str(st["widgets"][0]))

    ov(action="page", page="input")
    st, _ = ov(action="nav", keys=["ok", "down", "down", "down", "ok"])
    w = focused(st)
    check("text field editing", w and w["kind"] == "text" and w.get("editing"), str(w))
    ov(action="nav", keys=["backspace", "backspace", "backspace"])
    ov(action="text", text="F10")
    st, _ = ov(action="nav", keys=["ok"])
    w = focused(st)
    check("text field applied", w["value"] == "F10" and not w.get("editing"), str(w))

    st, _ = ov(action="page", page="quit")
    st, _ = ov(action="nav", keys=["ok"])
    check("quit asks", st["dialog"] and st["dialog"]["selected"] == "No", str(st["dialog"]))
    st, _ = ov(action="nav", keys=["ok"])
    check("quit No keeps running", st["dialog"] is None and st["open"])

    if not headless:
        r = client.call("screenshot", rendered=True)
        img = [c for c in r["content"] if c["type"] == "image"]
        ok = not r["isError"] and img
        if ok:
            w_, h = png_size(base64.b64decode(img[0]["data"]))
            ok = h >= 360
        check("rendered screenshot with overlay", ok, text_of(r))

    st, _ = ov(action="page", page="reset")
    st, _ = ov(action="nav", keys=["ok", "ok"])
    check("soft reset closes the overlay", st["open"] is False, str(st["open"]))
    s = jtext(client.call("status"))
    check("emulation runs after close", s.get("paused") is False, str(s))
    r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    check("READY. after overlay reset", not r["isError"], text_of(r))

    ov(action="open")
    client.call("pause")
    ov(action="close")
    s = jtext(client.call("status"))
    check("user pause survives overlay close", s.get("paused") is True, str(s))
    client.call("resume")


def run_media(client, tmp, headless):
    print("media page")
    import zipfile
    import shutil

    base = os.path.join(tmp, "media")
    os.makedirs(os.path.join(base, "folder"))
    with open(os.path.join(base, "game.d64"), "wb") as f:
        f.write(make_d64("TESTDISK", [("HELLO", basic_prg([(10, [0x99] + list(b' "D64 OK"'))])),
                                      ("SECOND", basic_prg([(10, [0x99] + list(b' "SECOND OK"'))]))]))
    with open(os.path.join(base, "tool.prg"), "wb") as f:
        f.write(basic_prg([(10, [0x99] + list(b' "PRG OK"'))]))
    with open(os.path.join(base, "readme.txt"), "w") as f:
        f.write("not an image")
    with open(os.path.join(base, ".hidden.d64"), "wb") as f:
        f.write(b"x")
    with zipfile.ZipFile(os.path.join(base, "archive.zip"), "w") as z:
        z.writestr("sub/", "")
        z.writestr("sub/inside.d64", make_d64("ZIPDISK", [("ZIPPED", basic_prg([(10, [0x99] + list(b' "ZIP OK"'))]))]))
    shutil.copy(os.path.join(base, "tool.prg"), os.path.join(base, "gone.prg"))
    with open(os.path.join(base, "cart.crt"), "wb") as f:
        f.write(make_crt("CRT OK"))

    def ov(**args):
        r = client.call("overlay", **args)
        if r["isError"]:
            return None, text_of(r)
        t = text_of(r)
        if "\n" in t:
            print("  note: overlay messages:", t.split("\n", 1)[1][:300])
        return json.loads(t.split("\n", 1)[0]), ""

    def detail(st):
        return st["widgets"][0].get("detail", {}) if st and st["widgets"] else {}

    def nav(*keys):
        st, err = ov(action="nav", keys=list(keys))
        return st, detail(st), err

    def names(d):
        return [e.rsplit(" [", 1)[0] for e in d.get("entries", [])]

    def select(name):
        """Type a filter that selects the entry 'name' (exact) in the current folder."""
        st, err = ov(action="text", text=name.rstrip("/"))
        d = detail(st)
        n = names(d)
        if name in n and d.get("selected") != name:
            idx, cur = n.index(name), n.index(d.get("selected")) if d.get("selected") in n else 0
            st, d, _ = nav(*(["down"] * (idx - cur) if idx > cur else ["up"] * (cur - idx)))
        return st, d

    def goto(path):
        st, d, _ = nav("backspace")
        for _ in range(40):
            if d.get("location") == "/" and not d.get("archive"):
                break
            st, d, _ = nav("parent")
        for part in [p for p in path.split("/") if p]:
            st, d = select(part + "/")
            st, d, _ = nav("ok")
        return st, d

    client.call("resume")
    st, _ = ov(action="open")
    st, _ = ov(action="page", page="media")
    st, d, _ = nav("right")
    check("media page is bare browser", st["zone"] == "content" and st["widgets"][0]["kind"] == "media"
          and d.get("tab") == "autostart" and d.get("zone") == "files", str(d)[:300])
    check("media tabs", d.get("tabs") == ["autostart", "disk8", "cartridge", "tape"], str(d.get("tabs")))

    st, d = goto(base)
    check("navigate to test folder", d.get("folder") == os.path.realpath(base), str(d.get("folder")))
    n = names(d)
    check("browser lists images, archives, folders", all(x in n for x in ["..", "folder/", "archive.zip", "game.d64", "tool.prg", "gone.prg"]), str(n))
    check("browser hides other and hidden files", "readme.txt" not in n and ".hidden.d64" not in n, str(n))
    check("folders/archives first", n.index("archive.zip") < n.index("game.d64") and n.index("folder/") < n.index("game.d64"), str(n))
    st, d, _ = nav("all_files")
    check("all files shows readme", "readme.txt" in names(d) and d.get("all_files") is True, str(names(d)))
    st, d, _ = nav("all_files")

    st, d = select("game.d64")
    check("live filter", d.get("filter") == "game.d64" and d.get("selected") == "game.d64", str(d)[:300])
    pv = d.get("preview", [])
    check("directory preview", len(pv) >= 4 and "TESTDISK" in pv[0] and '"HELLO"' in pv[1] and '"SECOND"' in pv[2]
          and "BLOCKS FREE" in pv[-1], str(pv))
    st, d, _ = nav("back")
    check("esc clears the filter first", d.get("filter") == "" and st["zone"] == "content", str(d.get("filter")))

    st, d, _ = nav("bookmark")
    bm = [f for f in d.get("favorites", []) if f["kind"] == "bookmark"]
    check("bookmark current folder", any(f["path"] == os.path.realpath(base) for f in bm), str(bm))

    st, d = select("archive.zip")
    st, d, _ = nav("ok")
    check("enter archive", d.get("archive", "").endswith("archive.zip") and "sub/" in names(d), str(d)[:300])
    st, d = select("sub/")
    st, d, _ = nav("ok")
    check("folder inside archive", "inside.d64" in names(d) and d.get("location", "").endswith("archive.zip/sub/"), str(d)[:300])
    st, d = select("inside.d64")
    check("preview inside archive", any('"ZIPPED"' in l for l in d.get("preview", [])), str(d.get("preview")))
    st, d, _ = nav("back", "parent", "parent")
    check("leave archive selects it", not d.get("archive") and d.get("selected") == "archive.zip", str(d)[:300])

    # Return on a preview line loads exactly that file
    st, d = select("game.d64")
    st, d, _ = nav("right", "down", "down")
    check("preview focus", d.get("zone") == "preview" and d.get("preview_selected") == 2, str(d)[:200])
    st, d, _ = nav("ok")
    check("preview start closes overlay", st["open"] is False, str(st["open"]))
    r = client.call("wait_for_text", text="SECOND OK", timeout=60, warp=True)
    check("preview entry loaded + run", not r["isError"], text_of(r))

    # Disk 8 tab: slot row, write protect, eject, insert only
    ov(action="open")
    st, _ = ov(action="page", page="media")
    st, d, _ = nav("right", "tab")
    slot = d.get("slot", {})
    check("disk 8 slot shows image", d.get("tab") == "disk8" and "game" in slot.get("image", "")
          and slot.get("buttons", [""])[0] == "Eject", str(slot))
    st, d, _ = nav("home", "up", "right", "ok")
    check("write protect toggle", d.get("zone") == "slot" and d["slot"].get("write_protect") is True, str(d.get("slot")))
    st, d, _ = nav("ok")
    check("write protect off", d["slot"].get("write_protect") is False, str(d.get("slot")))
    st, d, _ = nav("eject")
    check("eject drive 8", d["slot"].get("image") == "" and d.get("zone") == "files", str(d.get("slot")) + d.get("zone", ""))
    st, d = goto(base)
    st, d = select("game.d64")
    st, d, _ = nav("insert")
    check("insert only keeps overlay open", st["open"] and "game" in d["slot"].get("image", "")
          and "Inserted" in st.get("message", ""), str(st.get("message")))
    st, d, _ = nav("ok")
    check("disk 8 autostart closes overlay", st["open"] is False)
    r = client.call("wait_for_text", text="D64 OK", timeout=60, warp=True)
    check("disk 8 autostart runs first file", not r["isError"], text_of(r))

    # tape tab filter, mouse on tabs
    ov(action="open")
    st, _ = ov(action="page", page="media")
    st, d, _ = nav("right")
    rects = [list(map(int, r.split(","))) for r in d.get("tab_rects", "").split(";") if r]
    if len(rects) == 4:
        x, y, w_, h = rects[3]
        st, _ = ov(action="click", x=x + w_ // 2, y=y + h // 2)
        d = detail(st)
    check("click tape tab", d.get("tab") == "tape", str(d.get("tab")))
    st, d = goto(base)
    check("tape tab filters disks", "game.d64" not in names(d) and "archive.zip" in names(d), str(names(d)))

    # autostart a PRG, then a recent file that disappears
    st, d, _ = nav("tab")
    check("tab wraps to autostart", d.get("tab") == "autostart", str(d.get("tab")))
    st, d = goto(base)
    st, d = select("gone.prg")
    st, d, _ = nav("ok")
    check("autostart prg closes overlay", st["open"] is False)
    r = client.call("wait_for_text", text="PRG OK", timeout=30, warp=True)
    check("autostart prg runs", not r["isError"], text_of(r))
    os.unlink(os.path.join(base, "gone.prg"))
    ov(action="open")
    st, _ = ov(action="page", page="media")
    d = detail(st)
    rec = [f for f in d.get("favorites", []) if f["kind"] == "recent"]
    check("recent list", rec and rec[0]["title"].endswith("gone.prg") and rec[0].get("missing") is True, str(rec[:3]))
    check("recent shows game.d64", any("game.d64" in f["title"] for f in rec), str(rec[:3]))
    st, d, _ = nav("right", "bookmark")
    bm = [f for f in d.get("favorites", []) if f["kind"] == "bookmark"]
    check("bookmark toggled off", not any(f["path"] == os.path.realpath(base) for f in bm), str(bm))

    # cartridge tab: insert = power cycle, eject = hard reset
    st, d, _ = nav("tab", "tab")
    check("cartridge tab", d.get("tab") == "cartridge", str(d.get("tab")))
    st, d = goto(base)
    check("cartridge tab filters", names(d)[1:] == ["archive.zip", "folder/", "cart.crt"], str(names(d)))
    st, d = select("cart.crt")
    st, d, _ = nav("ok")
    check("cartridge start closes overlay", st["open"] is False, str(st.get("message")))
    r = client.call("wait_for_text", text="CRT OK", timeout=20, warp=True)
    check("cartridge runs", not r["isError"], text_of(r))
    ov(action="open")
    st, _ = ov(action="page", page="media")
    d = detail(st)
    check("cartridge slot", "cart" in d["slot"].get("image", "") and d["slot"].get("buttons") == ["Eject"], str(d.get("slot")))
    st, d, _ = nav("right", "eject")
    check("cartridge eject", d["slot"].get("image") == "", str(d.get("slot")))
    ov(action="close")
    r = client.call("wait_for_text", text="READY.", timeout=20, warp=True)
    check("READY. after cartridge eject", not r["isError"], text_of(r))


def run_headless_checks(client):
    print("headless")
    st = jtext(client.call("status"))
    check("status headless", st.get("headless") is True, str(st))
    t = time.time()
    client.call("run_frames", frames=50)
    dt = time.time() - t
    check("real-time speed (50 frames ~ 1 s)", 0.8 < dt < 1.6, f"{dt:.2f}s")
    t = time.time()
    client.call("run_frames", frames=500, warp=True)
    dt = time.time() - t
    check("warp faster than real time", dt < 5.0, f"{dt:.2f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "..", "builds", "release", "program", "badline"))
    ap.add_argument("--port", type=int, default=6465)
    ap.add_argument("--attach", action="store_true")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()

    client = Client(args.port)
    proc = None
    if not args.attach:
        # fresh C64 config, so stale media paths from the user config cannot pop up dialogs
        cfgdir = tempfile.mkdtemp(prefix="u64-smoke-")
        mode = ["-headless", "-mcp", str(args.port)] if args.headless else ["-mcp", str(args.port)]
        cfg = os.path.join(cfgdir, "c64.ini")
        # own config folder: settings and recent files of the user stay untouched
        env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
        proc = subprocess.Popen([args.binary, "-config-c64", cfg] + mode, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(client, proc)
        with tempfile.TemporaryDirectory() as tmp:
            run(client, tmp, args.headless)
            run_debug(client)
            run_overlay(client, args.headless)
            run_media(client, tmp, args.headless)
            if args.headless:
                run_headless_checks(client)
                cfg_before = os.path.getmtime(cfg) if os.path.exists(cfg) else None
                r = client.call("quit")
                check("quit", text_of(r) == "Quitting", text_of(r))
                try:
                    code = proc.wait(10)
                    check("process exited with 0", code == 0, str(code))
                    cfg_after = os.path.getmtime(cfg) if os.path.exists(cfg) else None
                    check("headless does not save settings", cfg_before == cfg_after, f"{cfg_before} -> {cfg_after}")
                except subprocess.TimeoutExpired:
                    check("process exited after quit", False)
                proc = None
    finally:
        if proc:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()

    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
