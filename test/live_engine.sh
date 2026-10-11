#!/bin/sh
# Live check of the whole engine with a typed prompt. Key as in live_gemini.sh; it is written to a temp daemon.conf, never printed.
set -e
cd "$(dirname "$0")/.."
KEYFILE=
for f in "$HOME/.gemini_api_key" /mnt/c/Users/*/.gemini_api_key; do [ -f "$f" ] && KEYFILE=$f && break; done
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
[ -n "$GEMINI_API_KEY" ] || { [ -n "$KEYFILE" ] || { echo "no key"; exit 2; }; GEMINI_API_KEY=$(tr -d '\r\n ' < "$KEYFILE"); }
umask 077; printf 'api_key=%s\nmodel=%s\n' "$GEMINI_API_KEY" "${MODEL:-gemini-3.8-flash}" > "$D/daemon.conf"
gcc -std=gnu11 -O1 -Isrc -I"${MPC_VST:-$HOME/mpc-vst-plugins}/wrapper" src/daemon_engine.c src/daemon_core.c src/io_device.c test/live_engine.c -lm -ldl -lpthread -o /tmp/daemon_live_engine
/tmp/daemon_live_engine "$D" "${1:-a rolling bassline in the style of deadmau5}"
ls -la "$D/clips" 2>/dev/null | tail -n +4
