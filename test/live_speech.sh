#!/bin/sh
# Live check with a WAV as the mic: test/live_speech.sh speech.wav. Key as in live_gemini.sh; it is written to a temp daemon.conf, never printed.
set -e
cd "$(dirname "$0")/.."
KEYFILE=
for f in "$HOME/.gemini_api_key" /mnt/c/Users/*/.gemini_api_key; do [ -f "$f" ] && KEYFILE=$f && break; done
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
[ -n "$GEMINI_API_KEY" ] || { [ -n "$KEYFILE" ] || { echo "no key"; exit 2; }; GEMINI_API_KEY=$(tr -d '\r\n ' < "$KEYFILE"); }
umask 077; printf 'api_key=%s\nmodel=%s\n' "$GEMINI_API_KEY" "${MODEL:-gemini-3.8-flash}" > "$D/daemon.conf"
gcc -std=gnu11 -O1 -Isrc -I"${MPC_VST:-$HOME/mpc-vst-plugins}/wrapper" src/daemon_engine.c src/daemon_core.c src/daemon_json.c src/daemon_kbd.c src/io_device.c test/live_speech.c -lm -ldl -lpthread -o /tmp/daemon_live_speech
/tmp/daemon_live_speech "$D" "$1"
