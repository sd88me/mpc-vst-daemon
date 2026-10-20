# Daemon (working name)

Daemon turns a spoken or typed idea into music on an Akai MPC / Force. It is a native VST2 **instrument** for MPC OS standalone devices:
you say or type "dark minor chords, 4 bars" and it writes a MIDI part and plays it out of its own MIDI port. Switch on **AGENT** and it can
also run the instrument for you: load a synth or effect, pick a preset, set parameters, add a track and drop the new clip into the clip matrix. Everything
the agent wants to change is shown as a plan first, and nothing happens until you press **RUN**.

It talks to Google Gemini for the musical part, so it needs a network connection and a Gemini API key. Skin theme: amber phosphor with a cyan accent.
Verified on a Force running MPC OS 3.9.1 (the other standalone devices should work, but the screen recordings are Force-specific).

## What it does
**Parts from a prompt** (works on its own, no add-ins needed)
- **TYPE** a prompt on a USB or Bluetooth keyboard, or with the on-screen **KEYS** keypad. **TALK** speaks it instead (needs the Daemon Listen effect, see below).
- Choose **Type** (Chords, Bassline, Melody, Drums ...), **Key**, **Scale**, **Bars** and the MIDI **Channel**. Daemon returns a part and loops it live.
- **REGEN** asks for another take. **SAVE** writes a standard MIDI file (`<drive>/Daemon/<name>.mid`) you can load into any clip.

**Agent mode** (switch AGENT on; needs the two add-ins below)
- Reads the project, the loaded plugins and the plugins installed on the device, and sees their parameters.
- Plans actions from a sentence, shows them in the PLAN box, and runs them when you press RUN. A plan that would replace an instrument or record over something needs **RUN pressed twice**.
- Tools: set plugin parameters, press transport buttons, make a part, and by driving the MPC screen: select a track, add a Plugin track, load an effect, load an instrument,
  choose a preset (including folders), and import the part into the first free clip slot of a track.
- Style requests use effects: "dubby chords" can load a delay and a reverb from the effects you have installed, then set their mix and feedback.
- After steps that change the project's structure, Daemon reminds you to save the project (the agent reads the saved file).

Things to say: "make dubby chords on track 4 with a delay and a reverb", "load the Rich Strings preset from the Modern folder on track 4", "load Profit-8 on track 3 and make it a plucky lead",
"add a track", "start playback".

## Requirements
- An Akai MPC OS standalone device (developed on a Force). Daemon is built for the device's CPU (armv7, and aarch64 for Gen2) with the shared wrapper from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).
- A Gemini API key and network access. Free keys hit their quota fast; if you see 429 or 503 Daemon retries three times, and you can pick another model.
- For agent mode, two add-ins on the device (Daemon checks both ports every ten seconds and the skin shows which one is missing):
  - **mpc-addin-commander** (port 6730): reads the project, plugins and parameters, and sets parameters and transport. Without it the plugin and project tools are off.
  - **mpc-addin-remote** (port 6720): screen, touch and hardware buttons. Without it the screen tools (select track, add track, load plugin, preset, import clip) are off.
- For voice: the **Daemon Listen** effect on the audio track that carries the microphone.

## Install and set up
1. Copy the Daemon plugin folder to the device (`/sdcard/Synths/...`) and register it in `MPC.settings` (new plugins need a settings entry and an MPC restart; replacing `daemon.so` only needs a project reload). `tools/release.py` builds installable zips for each CPU.
2. Create `engine/daemon.conf` inside the plugin folder (it survives upgrades):
   ```
   api_key=YOUR_GEMINI_KEY
   model=gemini-3.1-flash-lite     # optional
   clips_dir=Daemon                # optional, a folder directly on the drive
   commander=127.0.0.1:6730        # optional
   ```
   Never share the key; Daemon never prints it.
3. For agent mode, record or copy a `macros.json` for your screen into `engine/` (`docs/examples/macros.force-mpc-os-3.9.1.json` is a ready set for a Force on MPC OS 3.9.1;
   see `docs/MACROS.md`). It is re-read on every plan, so no restart is needed. Without it the agent simply is not offered the screen tools.
4. In MPC's Preferences > MIDI switch on **Global, Control and Track** for the port "Daemon MIDI Out", and give the instrument track you want to hear an input of All Ports.
   Set Daemon's **Channel** to the channel that instrument listens on (1-based, default 1).

## Using it
1. Add Daemon as an instrument on a track and open its screen (Main mode, select its track, then the arrow button next to the plugin name).
2. Tap **TYPE** and type a prompt, **Enter** sends. (Wait for the status line to say TYPING before typing.) Or tap **KEYS** and use the drawn keypad, or **TALK** to speak.
3. Without AGENT, Daemon makes a part and plays it. With **AGENT** on it answers with a plan, or with a short reply if there is nothing to do.
4. Read the PLAN line, then press **RUN**. Steps that touch the screen take a few seconds each, and the screen shows the Mixer afterwards: reopen Daemon to read the result.
5. Press **SAVE** to write the part as a `.mid` file.

Notes: a plan is restored with the project (a stale plan can show after a reload: do not RUN it). The agent reads the project **file**, so save the project after adding tracks or plugins.
Typed prompts also work without a keyboard: write a line to `engine/prompt.txt` (ssh or a file share); Daemon reads and deletes it.

## Troubleshooting
- `engine/daemon.log` (rewritten twice a second): transport callbacks, notes sent, last status. `engine/agent.log`: every agent tool call and each failed run.
- "no mic": insert Daemon Listen on the mic's audio track. "no api_key": check `engine/daemon.conf`.
- A step stops with "stopped in <macro> at step N": the screen was not where the recording expects. See `docs/MACROS.md` to re-record it.
- The device clock and the drive names differ between devices: the saved-parts folder is on the same drive as the plugin (`User` is the internal drive).

## How it is built
- `src/daemon_core.c`, `daemon_json.c`: reply checking (object or compact `[pitch,start,dur,vel]` notes), loop player, `.mid` writer, Gemini request, mic capture (44.1 kHz stereo to 16 kHz WAV), a small JSON reader/writer.
- `src/daemon_engine.c`: the engine for the shared wrapper (TALK, TYPE, REGEN, SAVE, project state, AGENT) with a worker thread for every request. `daemon_kbd.c`: keyboard entry from `/dev/input/event*`.
- `src/daemon_cmdr.c`, `daemon_agent.c`: a WebSocket client for the commander add-in and the Gemini function-calling agent (get_project, list_plugins with installed plugins, get_plugin_params, set_param, transport, play_part, select_track, add_track, load_efx, load_instrument, load_preset, import_clip).
- `src/daemon_mcp.c`, `daemon_macro.c`, `daemon_fp.c`: an MCP client for the remote add-in, the tap-macro engine and screen fingerprints; a vision step finds a plugin or preset by name on the screen.
- `src/io_device.c`: MIDI (libasound) and HTTPS (libcurl), loaded at run time. `listen/`, `src/daemon_tap_engine.c`: the Daemon Listen effect.
- `vst/`, `listen/`: `vst.json`, `params.json`, `layout.conf`, `skin.css`, art. `tools/`: macro runner, fingerprint capture, macro checker, release packaging.
- `test/run.sh`: core, keyboard, engine (fake network, MIDI port, keyboard, commander and remote add-in), commander client, agent, MCP client and macro tests under ASan/UBSan.
  Set `MPC_VST` to a checkout of mpc-vst-plugins. `test/live_*.sh` make real Gemini calls (key from `$GEMINI_API_KEY` or `~/.gemini_api_key`, never printed).
- `docs/MACROS.md` (tap macros), `docs/HANDOFF.md` (development log and verified device facts).

## Licence
GPL-3.0-or-later (see `LICENSE`; every source file carries an SPDX tag). Daemon is built on the shared wrapper from
[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (MIT), which is compatible; libcurl and libasound are loaded at run time and
not linked. If you distribute a modified version, you must make its source available under the same licence.

"Daemon" is a working name, and the licence does not grant any right to the name or branding of the project.
