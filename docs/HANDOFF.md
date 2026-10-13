# Daemon handoff (2026-10-12)

Daemon = voice/keyboard AI MIDI generator + agent, a native VST2 **effect** for Akai MPC/Force (repo `~/mpc-vst-daemon`, local git only, GPL-3.0-or-later,
no GitHub repo yet). Built with the shared wrapper from `~/mpc-vst-plugins` (use a worktree of origin/main: `MPC_VST=<worktree> bash test/run.sh`, all 8
suites pass; `tools/build_port.sh vst/vst.json armv7`). Never commit/push main in `~/mpc-vst-plugins` (another session uses it); `gh pr merge` is blocked.

## What works on the Force (MPC OS 3.9.1, 2026-10-12)
- Plugin loads as an insert effect (track 2 in the user's `daemon-test` project), skin v2 (full width) draws well.
- Typed prompt through `engine/prompt.txt` (drop file, read once and deleted) -> Gemini -> part (`DONE n NOTES`). Retries 429/500/503 three times.
  `engine/daemon.conf` holds `api_key` (never print it), `model=gemini-3.1-flash-lite` (gemini-3.8-flash was overloaded), optional `clips_dir=`.
- Playback: Daemon opens its own ALSA seq client "Daemon MIDI Out" (works for an effect; the VST host is not used for MIDI). MPC keeps the port's
  Global/Control/Track switches only if the ALSA client number is stable: the client is created once per MPC session and its handle is kept in the
  process env `DAEMON_ALSA_SEQ` (no library pinning, so a new daemon.so loads on a project reload). The user's JV-880 on track 1 answers on **channel 1**
  (the Channel knob defaults to 1 now; it is 1-based).
- `[SAVE]` writes `<drive root>/Daemon/<clip>.mid` (valid SMF type 0, 480 ppq). Import by hand works (select slot, MENU > BROWSER, MIDI filter, LOAD).
- Diagnostics: `engine/daemon.log` (rewritten twice a second: transport callbacks, notes sent, `midi_bad`, status).
- Agent toggle is now saved in the project state (needs a project reload to take effect on old projects).

## In progress: the `import_clip` tap macro (agent tool `import_clip(track)`)
Files: `docs/examples/macros.force-mpc-os-3.9.1.json` (installed as `engine/macros.json`), `docs/MACROS.md`, step `find_empty` (first free clip slot by
fingerprint, no model), `tools/macro_run.c` (runs a macro through an SSH tunnel to the remote addin: `ssh -f -N -L 16720:127.0.0.1:6720 root@<force>`),
`tools/macro_fp.c` (capture a fingerprint, no screenshot cost), `tools/macro_check.c`.
- Verified end to end once (43 steps, clip landed in the next free slot of track 1) when the browser was already on its **Places** tab.
- **Fixed 2026-10-12:** the failure was the browser opening on its Content tab and a scroll that never started inside the list. The macro now taps Places and checks it,
  folder-up x8, scrolls to the top with four 400-pixel swipes centred at (850,520), and checks the narrow folder-name fingerprints. Verified from a fresh state (53 steps):
  the part `dub_chords` landed in track 1's first free slot. The macro file is re-read at the start of each plan and run (no project reload needed).
- Other facts (all in docs/MACROS.md): folder-up button (44,54) x8 reaches the root and refreshes a stale listing; newest folder sorts first; `scroll` amount
  must be 20-2000; fingerprint only static regions; LOAD needs a selected slot; the drive named `662522` is external, the internal one is `User`.
- Not recorded yet: `select_track`, `load_efx`, instrument parameter editing via the commander (commander `set_param` works; selecting a track /
  loading a plugin needs macros). The agent only offers screen tools whose macro exists (`has_ui` mask).

## Device rules to keep
- Force is the user's live instrument: ask before restarting MPC (a restart closes the open project; `daemon-test` is saved), tell the user before taps,
  back up MPC.settings before editing, stage files as `x.new` then `mv`. Install: `release.py ... --user-data engine`, extract, tar over ssh to
  `/tmp/daemon-install*/`, `sh install.sh -y` (restarts MPC). Replacing daemon.so alone: `cat daemon.so | ssh ... 'cat > daemon.so.new && mv'` then the user reloads the project.
- `ssh -o BatchMode=yes root@192.168.1.187` (address may change; device is BusyBox: no `timeout`/`unzip`, `od` is limited).
- Never `pkill -f` with a pattern that appears in your own command line (it kills your shell); `sleep` with a long wait is blocked by the harness here.
- Screenshots cost many tokens: prefer `macro_fp`/ssh checks; the mcp `scroll` tool needs amount >= 20.
- Reading the Gemini key: only inside scripts/ssh pipes, never print it (`/mnt/c/Users/Sam/.gemini_api_key`, or `~/.gemini_api_key`).

## Not tested on the device yet
Mic capture, a real Bluetooth keyboard (the remote add-in's virtual keyboard `event3` works: TYPE -> text -> Enter, 2026-10-12; `engine/kbd.log` lists each device scan outcome; momentary buttons deliver `on` twice per tap, debounced 400 ms in set_param), commander transport state (verified 2026-10-12 through an SSH tunnel with test/live_cmdr: playing true/false, tempo, bar/beat, source mmc; MPC Commander In needs Sync on and Send MMC on; the client only reads pushes while a request waits),
(`find_tap` and `load_efx` are now verified, see docs/MACROS.md). Skin: live text is MPC's own font, not monospace.

## Before any public release
Add aarch64 target (Gen2 default per repo policy), create the GitHub repo (ask the user first), catalog entry PR + `catalog_check.py --catalog`, `tested.json`,
docs/NOTES.md entries in `mpc-vst-plugins` with dates (verified device facts above), optional DCO sign-off, trademark check for the name "Daemon".

## Verified 2026-10-12 (late)
Typed prompt (remote virtual keyboard) -> agent plan (make part + clip into track 3) -> RUN -> `RAN 2 STEPS`; Dark_Chords.mid saved and the clip appeared on track 3 (a new Drum track) in the clip matrix. Left in the user's project.

## Verified 2026-10-12 (night)
`load_instrument` ("load the instrument profit-08 on track 3" -> RAN 1 STEPS, Profit-08 id 23, 282 params) and sound shaping with `set_param` ("plucky deadmau5 lead": agent used `get_plugin_params` with `match`, 6 set_param steps: Osc2 Fine +10, FEnv Amt +77, short AEnv/FEnv decay). Pitfalls met: a prompt that does not name a loaded synth got "load Profit-08 on track 1" (would have replaced the JV-880: always read the plan before RUN); the project state restores the last plan (a stale "Bassline" plan showed after a reload: do not RUN it); after a macro the screen is the Mixer, reopen Daemon (tap its insert) before TYPE; Agent toggle defaults to on after a reload. `engine/agent.log` has each read-only tool call. Test clutter left: an unsaved Plugin track 3 with Profit-08 (reload `daemon-test` to drop it).

## 2026-10-13: drawn keyboard, and independence findings
- **KEYS button** (590,656) shows a drawn keypad (skin buttons `k_*` with `when=kb:on`, images from `vst/art/make_keys.py`), like poloq's mpc-vst-manager. SEND asks, HIDE/KEYS closes. Needs no hardware keyboard. Deployed, **not yet tried on the device**: reload the project, tap KEYS, check `engine/daemon.log` status. Same-key repeats within 120 ms are dropped (unknown whether keys double-fire like TALK; if letters double, raise it).
- **Mic from an instrument build:** MPC holds the ADA2 capture PCM (`pcmC2D0c`, 2 ch S32 44.1k, owner = MPC's pid) open and running, so a plugin cannot open it again exclusively; a loopback/dsnoop would be needed. An instrument gets no audio input from the host. Not yet tried: whether MPC passes inputs to a VST2 with numInputs > 0 and isSynth.
- **No Remote add-in needed for taps/screen:** a plain root process on the device (built from the Remote add-in's `touch.c` + `capture.c`) opened `/dev/input/event0`, read the 1280x800 frame, injected a tap and saw the frame change. Daemon already runs as root inside MPC, so the macro engine can use those directly (replacing the MCP client in `daemon_mcp.c`); Remote then becomes optional. `/data/midi.in` (Commander's `surface`) does not exist on this device; `/dev/uinput` does.
