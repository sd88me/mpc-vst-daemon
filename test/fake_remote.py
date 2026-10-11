#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 sd88me
"""A stand-in for the remote addin's MCP endpoint (POST /mcp, JSON reply per request), standard library only. Prints its port first, then
one line per tool call: "CALL <name> <args json>". A tap with y >= 700 reports no screen change (a stale coordinate); every other touch
reports a change. screenshot returns a 1x1 PNG. Exits when stdin closes."""
import base64, json, os, socket, sys, threading

PNG = base64.b64encode(bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360f8cfc0000003010100c9fe92ef0000000049454e44ae426082")).decode()

def result(text, image=False, error=False):
    c = [{"type": "text", "text": text}]
    if image: c.append({"type": "image", "data": PNG, "mimeType": "image/png"})
    return {"content": c, "isError": error}

def call(name, a):
    print("CALL %s %s" % (name, json.dumps(a, separators=(",", ":"), sort_keys=True)), flush=True)
    if name == "get_screen_info": return result('{"width":1280,"height":800,"display":"ok","touchscreen":"/dev/input/event1"}\nCoordinates for every touch tool: x 0-1279, y 0-799, origin top left.')
    if name in ("tap", "double_tap", "long_press"):
        if a.get("y", 0) >= 700: return result("Tapped (%d, %d). The screen did not change." % (a["x"], a["y"]))
        return result("Tapped (%d, %d). The screen changed in x 10-90, y 20-60." % (a["x"], a["y"]), image=a.get("screenshot", "half") != "none")
    if name == "press_button":
        if a.get("name") == "NOPE": return result("No button called NOPE on this device.", error=True)
        return result("Pressed %s. The screen changed in x 0-1279, y 0-799." % a["name"])
    if name == "wait_for_screen": return result("The screen is still.")
    if name == "scroll": return result("Scrolled %s. The screen changed in x 0-300, y 100-600." % a["direction"])
    if name == "screenshot": return result("Screen 1280x800.", image=True)
    return result("unknown tool", error=True)

def handle(c):
    d = b""
    while b"\r\n\r\n" not in d: 
        k = c.recv(4096)
        if not k: return
        d += k
    head, _, rest = d.partition(b"\r\n\r\n")
    n = int([l.split(b":")[1] for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0])
    while len(rest) < n: rest += c.recv(4096)
    m = json.loads(rest[:n])
    if "id" not in m: c.sendall(b"HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); c.close(); return
    if m["method"] == "initialize": r = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "fake"}}
    elif m["method"] == "tools/call": r = call(m["params"]["name"], m["params"].get("arguments", {}))
    else: c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"); c.close(); return
    b = json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": r}).encode()
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % len(b) + b); c.close()

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
while True:
    c, _ = s.accept(); threading.Thread(target=handle, args=(c,), daemon=True).start()
