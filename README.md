# Daemon (working name)

Voice and keyboard prompts to MIDI parts for Akai MPC OS standalone devices, as a native VST2 effect plugin
(mic in on an audio track) that plays through its own MIDI port. Modelled on a Max for Live generator device:
prompt + type + key/scale + length in, notes out. Skin theme: amber phosphor, cyan accent.

Status (offline only; nothing has run on a device yet):
- `src/daemon_core.c`: reply checking, loop player, `.mid` writer, Gemini request, mic capture (44.1 kHz stereo -> 16 kHz WAV).
- `src/daemon_engine.c`: the engine for the shared wrapper (talk, regen, save, project state; worker thread for the request).
- `src/io_device.c`: MIDI port (libasound) and HTTPS (libcurl), both loaded at run time.
- `test/run.sh`: core and engine tests under ASan/UBSan with a fake network and MIDI port. Needs mpc-vst-plugins with the
  `HAS_SONG_POS` hook (PR #385): set `MPC_VST` to a checkout that has it.
- `test/live_*.sh`: real Gemini calls (typed prompt, and a spoken WAV played into `process()`). Key from `$GEMINI_API_KEY`
  or `~/.gemini_api_key`; it is written to a temp config and never printed.
- `vst/`: `vst.json` and `params.json`. `tools/test_port.sh vst/vst.json` (from mpc-vst-plugins) passes.

Setup on a device: `daemon.conf` next to the plugin's `engine/` folder with `api_key=...` (optional `model=`).
