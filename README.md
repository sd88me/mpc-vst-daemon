# Daemon (working name)

Voice and keyboard prompts to MIDI parts for Akai MPC OS standalone devices, as a native VST2 effect plugin
(mic in on an audio track) that plays through its own MIDI port. Modelled on a Max for Live generator device:
prompt + type + key/scale + length in, notes out. Skin theme: amber phosphor, cyan accent.

Status (offline only; nothing has run on a device yet):
- `src/daemon_core.c`, `daemon_json.c`: reply checking (object or compact `[pitch,start,dur,vel]` notes), loop player, `.mid` writer,
  Gemini request, mic capture (44.1 kHz stereo -> 16 kHz WAV), a small JSON reader/writer.
- `src/daemon_engine.c`: the engine for the shared wrapper: TALK (mic), TYPE (USB or Bluetooth keyboard), REGEN, SAVE, project state,
  and AGENT mode: plan with read-only tools, press RUN to apply the changes. Worker thread for every request.
- `src/daemon_kbd.c`: keyboard entry from `/dev/input/event*` (grabbed only while typing; picks up keyboards that connect later).
- `src/daemon_cmdr.c`, `daemon_agent.c`: a WebSocket client for the commander addin (`docs/PROTOCOL.md` of
  jacob-sabella/mpc-addin-commander) and the Gemini function-calling agent (tools: get_project, list_plugins, get_plugin_params,
  set_param, transport, play_part).
- `src/daemon_mcp.c`, `daemon_macro.c`: an MCP client for the remote addin and the tap-macro engine (`docs/MACROS.md`): select a track and
  load a plugin by replaying macros recorded on the device, with a Gemini vision step to find a plugin by name. `tools/macro_check` validates a file.
- `src/io_device.c`: MIDI port (libasound) and HTTPS (libcurl), both loaded at run time.
- `test/run.sh`: core, keyboard, engine (fake network, fake MIDI port, fake keyboard device, fake commander, fake remote addin), commander client,
  agent, MCP client and macro tests under ASan/UBSan. Needs mpc-vst-plugins with the `HAS_SONG_POS` hook (PR #385): set `MPC_VST` to a checkout that has it.
- `test/live_*.sh`: real Gemini calls (typed prompt, a spoken WAV played into `process()`, the agent against a fake commander). Key from
  `$GEMINI_API_KEY` or `~/.gemini_api_key`; it is written to a temp config and never printed. A free key hits its quota quickly.
- `vst/`: `vst.json`, `params.json`, `layout.conf`, `skin.css`, `art/make_face.py`. `tools/test_port.sh vst/vst.json` (from
  mpc-vst-plugins) passes; `tools/build_port.sh` builds the armhf `.so`.

Setup on a device: `daemon.conf` next to the plugin's `engine/` folder with `api_key=...` (optional `model=`, `commander=127.0.0.1:6730`).
Agent mode needs the commander addin installed. Loading a plugin / choosing a track needs a `macros.json` recorded on the device and the remote addin (docs/MACROS.md); none is recorded yet.
