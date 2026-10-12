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
- **Current bug (the safety check caught it):** from a fresh state the macro stopped at step 19 (`expect browser_daemon` difference 3.9, allowed 2.5) because
  the first row at the drive root was not `Daemon` (the user saw it enter `mdp3`). Most likely cause: the browser had opened on its **Content** tab (left panel:
  Audio Clips/Instruments...) and my final macro lost the `tap [88,150]` (Places tab) that the working stage `p3` had. Fix to try first: add
  `{ "tap": [88,150] }` (Places) + a short wait before the folder-up taps, and capture a fingerprint of the left panel to confirm the Places tab.
  Also strengthen `browser_daemon`/`browser_user` (path text region is weak: "Daemon" vs "mdp3" differ only by ~4): capture a narrower region around the
  folder name (about `[150,40,140,26]`), and re-measure against another folder before trusting a tolerance.
- Other facts (all in docs/MACROS.md): folder-up button (44,54) x8 reaches the root and refreshes a stale listing; newest folder sorts first; `scroll` amount
  must be 20-2000; fingerprint only static regions; LOAD needs a selected slot; the drive named `662522` is external, the internal one is `User`.
- Not recorded yet: `select_track`, `load_plugin`, instrument parameter editing via the commander (commander `set_param` works; selecting a track /
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
Mic capture, USB/Bluetooth keyboard (evdev grab), agent plan -> RUN of `import_clip` end to end, commander transport state (needs MIDI port + Receive MMC),
vision step (`find_tap`). Skin: live text is MPC's own font, not monospace.

## Before any public release
Add aarch64 target (Gen2 default per repo policy), create the GitHub repo (ask the user first), catalog entry PR + `catalog_check.py --catalog`, `tested.json`,
docs/NOTES.md entries in `mpc-vst-plugins` with dates (verified device facts above), optional DCO sign-off, trademark check for the name "Daemon".
