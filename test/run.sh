#!/bin/sh
# Offline test of src/daemon_core.c on x86 under ASan+UBSan.
set -e
cd "$(dirname "$0")/.."
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer src/daemon_core.c src/daemon_json.c test/core_test.c -lm -o /tmp/daemon_core_test
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined src/daemon_kbd.c test/kbd_test.c -lpthread -o /tmp/daemon_kbd_test
/tmp/daemon_kbd_test
/tmp/daemon_core_test
python3 -c "import json;d=json.load(open('/tmp/daemon_req.json'));assert d['contents'][0]['parts'][0]['inline_data']['data'];print('request is valid JSON,', len(open('/tmp/daemon_req.json').read()), 'bytes')"
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -I"${MPC_VST:-$HOME/mpc-vst-plugins}/wrapper" \
  src/daemon_engine.c src/daemon_core.c src/daemon_json.c src/daemon_kbd.c src/daemon_agent.c src/daemon_cmdr.c test/io_fake.c test/engine_test.c -lm -lpthread -o /tmp/daemon_engine_test
/tmp/daemon_engine_test
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined src/daemon_cmdr.c src/daemon_json.c test/cmdr_test.c -o /tmp/daemon_cmdr_test
timeout 60 /tmp/daemon_cmdr_test
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined src/daemon_agent.c src/daemon_cmdr.c src/daemon_core.c src/daemon_json.c test/agent_test.c -lm -o /tmp/daemon_agent_test
timeout 60 /tmp/daemon_agent_test
