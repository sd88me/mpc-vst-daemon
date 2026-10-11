#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 sd88me
"""A stand-in for the commander addin's WebSocket (docs/PROTOCOL.md of jacob-sabella/mpc-addin-commander), standard library only.
Prints its port on the first line of stdout, then one line per client message ("MSG {json}"). Exits when stdin closes."""
import base64, hashlib, json, os, socket, struct, sys, threading, time

PLUGINS = [
    {"id": 3, "name": "Chordsmith", "vendor": "jacob-sabella", "product": "Chordsmith", "uid": "4368536d", "so": "/x/c.so", "skin": True, "synth": True,
     "params": [{"i": 0, "name": "Key", "label": "", "value": 0.0, "text": "C"}, {"i": 1, "name": "Mode", "label": "", "value": 0.5, "text": "Dorian"}]},
    {"id": 5, "name": "Profit-8", "vendor": "sd88me", "product": "Profit-8", "uid": "50723038", "so": "/x/p.so", "skin": True, "synth": True,
     "params": [{"i": 0, "name": "Cutoff", "label": "", "value": 0.7, "text": "70%"}, {"i": 1, "name": "Resonance", "label": "", "value": 0.2, "text": "20%"},
                {"i": 2, "name": "Attack", "label": "", "value": 0.0, "text": "0 ms"}]},
]
PROJECT = {"t": "project", "name": "Jam 1", "tempo": 124.0, "key": "F Minor", "current_track": 1,
           "tracks": [{"index": 1, "name": "Drums", "type": "drum", "plugin": None}, {"index": 2, "name": "Keys", "type": "plugin", "plugin": {"name": "Chordsmith"}}]}

def frame(obj, mask=False):
    b = json.dumps(obj).encode(); n = len(b)
    h = bytes([0x81])
    if n < 126: h += bytes([n])
    elif n < 65536: h += bytes([126]) + struct.pack(">H", n)
    else: h += bytes([127]) + struct.pack(">Q", n)
    return h + b

def recv_exact(c, n):
    d = b""
    while len(d) < n:
        k = c.recv(n - len(d))
        if not k: raise EOFError
        d += k
    return d

def read_frame(c):
    h = recv_exact(c, 2); op = h[0] & 15; n = h[1] & 0x7f
    if n == 126: n = struct.unpack(">H", recv_exact(c, 2))[0]
    elif n == 127: n = struct.unpack(">Q", recv_exact(c, 8))[0]
    mask = recv_exact(c, 4) if h[1] & 0x80 else b"\0\0\0\0"
    d = bytearray(recv_exact(c, n))
    for i in range(n): d[i] ^= mask[i & 3]
    return op, bytes(d)

def serve(c):
    req = b""
    while b"\r\n\r\n" not in req: req += c.recv(1024)
    key = [l.split(b": ")[1] for l in req.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0].strip()
    acc = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
    c.sendall(frame({"t": "hello", "protocol": 1, "addin": "fake", "device": {"model": "test", "mpc": "0"}, "poll_ms": 100, "text_ms": 500, "midi": {"client": 130}}))
    c.sendall(frame({"t": "plugins", "plugins": PLUGINS}))
    c.sendall(frame({"t": "transport", "playing": False, "recording": False, "tempo": 124.0, "bar": 1, "beat": 1, "tick": 0, "source": "clock"}))
    try:
        while True:
            op, d = read_frame(c)
            if op == 8: return
            if op != 1: continue
            m = json.loads(d); print("MSG " + json.dumps(m, separators=(",", ":")), flush=True)
            t = m.get("t")
            if t == "set":
                p = next((p for p in PLUGINS if p["id"] == m["id"]), None)
                if not p or not (0 <= m["i"] < len(p["params"])): c.sendall(frame({"t": "error", "msg": "no such parameter"})); continue
                p["params"][m["i"]]["value"] = m["value"]; p["params"][m["i"]]["text"] = "%d%%" % round(m["value"] * 100)
                c.sendall(frame({"t": "values", "id": 5 if False else m["id"], "v": [[m["i"], m["value"], p["params"][m["i"]]["text"]]]}))
            elif t == "project": c.sendall(frame(PROJECT))
            elif t == "list": c.sendall(frame({"t": "plugins", "plugins": PLUGINS}))
            elif t == "ping": c.sendall(frame({"t": "pong"}))
    except EOFError:
        return

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(4)
print(s.getsockname()[1], flush=True)
threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
