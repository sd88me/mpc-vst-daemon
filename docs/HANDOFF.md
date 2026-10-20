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

## 2026-10-14: instrument + Daemon Listen
- Daemon is a VST2 instrument (`vst/vst.json` effect false). **Daemon Listen** (`listen/`, `src/daemon_tap_engine.c`) is an effect for the mic's audio track: passes audio through and writes a copy into `/dev/shm/daemon_tap` (`src/daemon_tap.h`); Daemon's `render()` reads it while LISTENING. MPC gives instruments no audio and no routing (DaemonProbe experiment: valid but silent input buffers).
- Deployed to the Force; `MPC.settings` edited (backup `MPC.settings.bak-daemon-listen`): Daemon now an instrument with no inputs, Daemon Listen added, DaemonProbe removed (folder deleted). MPC restarted. New plugins need a settings entry + restart; a `.so` swap does not.
- Drawn keyboard widened (keys 100 px, pitch 104). Add-in probe feeds the agent (remote missing drops the screen tools). Not yet seen on the device.
- Verified on the Force (2026-10-14): voice end to end through Daemon Listen, the drawn keyboard (full-width 112x68 keys), and a Bluetooth keyboard via TYPE.
- Plans that replace an instrument (per the commander's last-saved project) or record need RUN pressed twice (`agent_risk`). Unsaved new tracks are not flagged.
- Release dry run (nothing published): `vst.json` and `listen/vst.json` now list `targets` armv7 + aarch64; both build. `tools/release.py --repo sd88me/mpc-vst-daemon --license GPL-3.0-or-later [--user-data engine]` makes Daemon and Daemon Listen zips for each CPU; `catalog_check.py --catalog` OK (one expected warning: network/dlopen). Not done: Gen2 device test, bench, GitHub release, trademark check of the name "Daemon".

## 2026-10-15: load_preset (built, deployed, NOT yet run end to end)
- Macro `load_preset(track, preset, folder?)`: Mixer → EFFECTS tab → double tap track title → double tap PRESET (940,598) → optional folder → preset by vision → close X (1152,86). Mixer only, no Matrix button (Force-specific). Agent tool uses `has_ui` bit 16 when the macro exists. Optional macro params and `if_param` steps are tested (9 suites pass). Recorded by hand on Solina (AIR); Dexed/Chordsmith/Clementine-XT have no presets.
- Deployed: `daemon.so` md5 d42147487adaeaa6d9225cca96c5c616 and `engine/macros.json` (prior `macros.json.bak2`). The new `.so` loads on project reload.
- To test next: reload the project, open Daemon, AGENT on, prompt "load the Rich Strings preset from the Modern folder on track 4" (track 4 = Solina, unsaved, a reload removes it, so re-add Solina first), press RUN, check `engine/agent.log`/`daemon.log`.
- Still open: A/B audition and prompt recipes, GitHub draft release (confirm the name "Daemon", ask before publishing; no Gen2 test, bench or trademark check), keyboard responsiveness (120 ms same-key debounce) if reported.

## 2026-10-20: load_preset checked by hand, agent path blocked by an unsaved project
- Followed the `load_preset` steps by hand with the MCP tools (macro_run has no vision, so `find_tap` cannot run there): Mixer, EFFECTS tab, double tap track 4's title (560,40), double tap PRESET (940,598), folder Modern, row Rich Strings. Solina went from "Back To The 80s" to "Rich Strings" and the X (1152,86) closed Track Settings. The coordinates and list layout in the macro are right.
- Opening Daemon's editor: select its track in the Mixer, MENU (press ~300 ms) > MAIN MODE > the **arrow button (1040,478)** beside the plugin name (it takes a moment). The pencil at (1222,470) is TRACK EDIT (copy/duplicate/DELETE track): do not use it. The editor does not repaint while it is up: bounce out and back in to read the plan text.
- Agent path **not** verified: the prompt "load the Rich Strings preset from the Modern folder on track 4" got "Track 4 is a submix track..." because `get_project` reads the last-saved project on disk (`Mlo.xpj`), not the open "untitled" one. Save the open project first, then retry; the commander cannot see unsaved tracks.

## 2026-10-20 (later): add_track, save prompt, effects guidance (offline-tested, NOT deployed)
- New macro `add_track` (docs/examples/macros.force-mpc-os-3.9.1.json, agent tool bit 32): taps **+** by vision, then Plugin. Recorded by hand (made track 5 "Plugin 003" in the open untitled project; delete it if unwanted). Not yet run through the macro engine or agent.
- After a run with add_track/load_instrument/load_efx the status reads "RAN n STEPS: now SAVE the project so Daemon sees it" (the commander only reads the saved project). The model is told a new track is the next number and to ask for a save.
- System prompt now says when to use load_efx for a style (dub: delay + reverb, etc.). 9 suites pass.
- To deploy: the armv7 build failed with "exec format error" because QEMU binfmt is not registered in this WSL (`/proc/sys/fs/binfmt_misc` has no qemu-arm). Register it, build `tools/build_port.sh vst/vst.json armv7`, copy `daemon.so` and the new `engine/macros.json` to the Force (stage `.new` then `mv`, back up the old macros.json), reload the project, save it, then try "make dubby chords on track 5 with a delay and reverb".
- 2026-10-20: QEMU was registered again with `docker run --rm --privileged multiarch/qemu-user-static --reset -p yes` (tonistiivi/binfmt was not pullable; registration is lost when WSL restarts). armv7 build OK. Deployed to the Force: `daemon.so` md5 7b2ba290d8638275eb423c6cf191594f (backup `daemon.so.bak3`) and `engine/macros.json` md5 d14c1a25616477605d628f3e4949397d (backup `macros.json.bak3`). Takes effect on a project reload (macros re-read per plan). Next: user saves the open project, reloads it, then test the `add_track` agent tool and "dubby chords".

## 2026-10-20 (evening): load_preset verified through the agent
- Restarted MPC (user asked), reopened `daemon-test`. "load the Rich Viola preset from the Modern folder on track 4" -> PLAN 1 STEPS -> RUN -> `RAN 1 STEPS`; Solina on track 4 read Rich Viola afterwards.
- **Why the agent said "Track 4 is a submix":** the commander addin snapshots `recentProject1` from `MPC.settings`, and MPC only rewrites that file on some events (not on save, not on a systemctl restart). It still said `Mlo.xpj`, so saving did not help. Worked around by editing `recentProject1` to `daemon-test.xpj` (backup `MPC.settings.bak-daemon-recent` next to it); the commander picked it up at once, no restart. **Open:** make this automatic (the commander could take the newest-mtime project among recentProject1..9, or Daemon could ask for a given project); until then re-point `recentProject1` after opening another project. The "SAVE the project" prompt alone is not enough.
- macro fix: `load_preset` opens its list inside the current preset's folder, so a step now taps the back arrow (54,54) before looking for the folder (the old macro failed with "stopped in load_preset at step 11" when the preset was already in the target folder). `engine/macros.json` deployed (md5 2072ed0f...). `daemon.so` rebuild for the `RUN FAILED` line in agent.log (full failure text) is committed but not deployed.
- Not yet tested on the device: `add_track` through the agent, and the dubby-chords effects prompt.

## 2026-10-20 (night): project tracking fixed, dubby chords verified end to end
- **Commander fix** (`~/mpc-addin-commander-up`, commit 4a16d01, local only, fork of jacob-sabella's): `recent_project()` now picks the newest-saved `.xpj` among `recentProject1..9` and their folders (test added, all pass). Deployed `mpc_commander_addin.so` md5 e28e5b35... (backup `.bak-recent`), `MPC.settings` restored from `MPC.settings.bak-daemon-recent` (recentProject1 = Mlo again) and the commander still reported daemon-test. MPC also keeps `CurrentProject` (`lastProjectPath`) next to `MPC.settings`: a cleaner source if the newest-mtime rule ever picks wrong (e.g. another project saved more recently than the open one).
- **list_plugins** now also returns `installed_effects` / `installed_instruments` read from `KNOWNPLUGINS` in MPC.settings (VST format only; DAEMON_MPC_SETTINGS overrides the path, tests added). The agent prompt says to pick effects only from that list. Before this it guessed "AIR Delay", which is not in the VST picker (load_efx stopped at find_tap step 11).
- **import_clip** now finds the saved-parts folder by name (`find_tap` with new `double: 1`): the root listing is sorted by modification time, and a plugin deploy had pushed `Synths` to the top (the old "first row" double tap entered Synths).
- Agent planning retries 429/500/503 three times. `gemini-3.1-flash-lite` kept returning 503/timeouts today, so `engine/daemon.conf` now has `model=gemini-3.5-flash` (backup `daemon.conf.bak-model`). `daemon.conf` is read at load only; a model change needs a project reload.
- **Verified:** "make dubby chords on track 4 with a delay and a reverb" -> PLAN 4 STEPS (Machinemodule Tap FX, AW Reverb - Galactic3, part "Dub Chords", clip into track 4) -> RUN -> `RAN 4 STEPS: now SAVE the project so Daemon sees it`; Tap FX and Galactic3 sit in track 4's inserts and Dub_Chords is in its first clip slot. No set_param steps yet (the effects were not loaded when the plan was made): a second prompt such as "set the delay to feedback 60%" would do it.
- Not yet tested: `add_track` through the agent (macro recorded by hand only); `set_param` on the freshly loaded effects.
- **Habits that cost time:** wait for the daemon.log status "TYPING: Enter sends" (not "no keyboard yet") before type_text or the first characters are lost; the editor does not repaint while open (bounce MENU > MAIN MODE > arrow to read the plan); reloading a project = MENU > NEW PROJECT > DON'T SAVE > pick the project (the app restarts, takes ~1.5 min); never `pkill -f` with a pattern in your own command line; no `sleep` in the foreground here, use `run_in_background`.
- Unsaved state in the open `daemon-test`: the two effects, the Dub_Chords clip and the part file `Daemon/Dub_Chords.mid`. Save the project on the device if you want to keep them, or reload with DON'T SAVE.

## 2026-10-20 (late): settings tab, factory effects, TALK greying, several device fixes
- **SETTINGS tab** (standard second tab; `qlinks` lines must sit inside their own `[tab]` section, a misplaced one gave 8 Q-Link warnings): key status (last 4 chars), model prev/next/refresh (ListModels via GET: `http_post` with a NULL body; filter drops image/tts/embedding/live/transcribe/banana...), add-ins, Daemon Listen state. Text readouts need `"display":"string"` in params.json. Model choice is written to `engine/daemon.conf` and used at once. **TALK** is greyed with a second `button img=btn_talk_dim.svg when=mic:off` (a `picture` overlay is drawn under buttons: useless). Momentary buttons fire twice per tap: model_prev/next, models_refresh, regen and save are now debounced like talk/typing.
- **Factory plugins:** `list_plugins` also returns installed_effects/instruments: VST entries of KNOWNPLUGINS in MPC.settings, plus the built-in set from `/usr/share/Akai/Content/Synths/<vendor> - MPC - <name>` (about 100, presets inside each), `Synths` and `Force Documents/Plugin Presets`, plus a table of the picker's verified Delay/Reverb and Harmonic groups. Each effect carries the picker **group**; `load_efx(track, plugin, group)` expands that group first (`find_tap` + `optional`, skipped when the group is already open: the picker remembers the last one). Groups for Dynamics, EQ/Filter, Modulation, Vocal, Vintage Effects come from name heuristics and are **not verified** (an error "could not find on screen" would mean a wrong group).
- **Fixes found on the device:** insert slots sit on a **159 px** column pitch, not 160 (`find_empty` origin was drifting, track 5 failed); vision lookup retries 429/500/503; a **double unlock** of the engine mutex in the model refresh hung MPC at "Loading clips to row 1" (killed MPC with `kill -TERM`; `systemctl` was unreachable with "Transport endpoint is not connected"); the virtual keyboard device only appears after the first `type_text`, so the first characters are lost unless a space is typed first and "TYPING: Enter sends" has appeared.
- **Verified end to end:** "add a reverb and a delay to track 5" -> AIR Reverb + AIR Delay in track 5's inserts (model gemini-3-flash-preview). The default model in `daemon.conf` is now gemini-3-flash-preview (3.5-flash hit its free quota; 3.1-flash-lite was timing out).
- Not done: verify the other picker groups; the project still needs saving by hand after structural changes (the commander picks the newest-saved project); `add_track` not yet run through the agent; Gen2 build untested.

## 2026-10-20 (night 2): efficiency review, release README
- Token diet: get_project result is compacted in daemon_agent.c (real tracks, name/type/plugin/preset/mute/solo only): 8722 -> 470 bytes on daemon-test; installed plugins are `{"group":[names]}` lists; vision screenshots at zoom 0.5 (verified on the device: AIR Chorus found and loaded, so the Modulation group guess is right).
- load_efx now scrolls the picker to the top before the group step (the picker reopens where the last pick was; AIR Reverb was above the view). Deployed (macros.json ff4eacf4...), NOT yet re-run: gemini-3-flash-preview hit its free quota (429) too. Next test: "add a reverb to track 5" (track 5 holds AIR Chorus, unsaved).
- `engine/prompt.txt` works for driving tests without the screen.
- README rewritten as a first-release README.
