#!/bin/sh
# Offline test of src/daemon_core.c on x86 under ASan+UBSan.
set -e
cd "$(dirname "$0")/.."
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer src/daemon_core.c test/core_test.c -lm -o /tmp/daemon_core_test
/tmp/daemon_core_test
python3 -c "import json;d=json.load(open('/tmp/daemon_req.json'));assert d['contents'][0]['parts'][0]['inline_data']['data'];print('request is valid JSON,', len(open('/tmp/daemon_req.json').read()), 'bytes')"
gcc -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -I"${MPC_VST:-$HOME/mpc-vst-plugins}/wrapper" \
  src/daemon_engine.c src/daemon_core.c test/io_fake.c test/engine_test.c -lm -lpthread -o /tmp/daemon_engine_test
/tmp/daemon_engine_test
