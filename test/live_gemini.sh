#!/bin/sh
# One live Gemini call with a typed prompt, then the reply goes through the same parser the plugin uses.
# Key: $GEMINI_API_KEY, else ~/.gemini_api_key, else the Windows copy the Max for Live device saves. Never printed.
set -e
cd "$(dirname "$0")/.."
KEYFILE=
for f in "$HOME/.gemini_api_key" /mnt/c/Users/*/.gemini_api_key; do [ -f "$f" ] && KEYFILE=$f && break; done
[ -n "$GEMINI_API_KEY" ] || { [ -n "$KEYFILE" ] || { echo "no key"; exit 2; }; GEMINI_API_KEY=$(tr -d '\r\n ' < "$KEYFILE"); }
MODEL=${MODEL:-gemini-2.5-flash}
gcc -std=gnu11 -O1 src/daemon_core.c test/live_main.c -lm -o /tmp/daemon_live
/tmp/daemon_live req "${1:-4 bar chord progression with the feel of faxing berlin by deadmau5}" > /tmp/daemon_live_req.json
curl -sS -m 60 -o /tmp/daemon_live_resp.json -w 'http %{http_code}\n' \
  -H "x-goog-api-key: $GEMINI_API_KEY" -H 'Content-Type: application/json' \
  -d @/tmp/daemon_live_req.json "https://generativelanguage.googleapis.com/v1beta/models/$MODEL:generateContent"
/tmp/daemon_live parse /tmp/daemon_live_resp.json
