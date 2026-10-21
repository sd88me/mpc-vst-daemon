# Daemon

**Your AI generative assistant for the Akai MPC and Force.** Say or type what you hear in your head, and Daemon writes it, plays it and
puts it in your project: chord progressions, basslines, melodies and beats, then the tracks, sounds, effects and presets around them.

Generative sequencers give you dice to roll. Daemon gives you a collaborator who understands what you ask for. "Dubby minor chords
on track 4 with a delay and a reverb" becomes a MIDI part in your key, a clip in your clip matrix, and AIR Reverb and AIR Delay in the
track's inserts. All of it is planned in front of you, and none of it runs until you press **RUN**.

Daemon is a native instrument plugin that runs inside MPC OS standalone, with no computer attached. It uses Google Gemini or Anthropic Claude
(your choice, on the SETTINGS tab) for its musical and visual understanding, so the device needs a network connection and an API key.

> **Version 1.1.0.** Runs on first-generation MPCs and the Force (MPC OS 2.x and 3.x) and on second-generation MPCs (MPC OS 3.x).
> Developed and verified on an Akai Force with MPC OS 3.9.1; the other devices and MPC OS 2.x are supported by the build but not yet
> tested on hardware. The screen automations (see [Build the session around it](#build-the-session-around-it-agent-mode)) are recorded
> per device; a set for the Force on MPC OS 3.9.1 ships with it.

### New in 1.1.0
- **Claude as a second AI provider.** Choose Gemini or Claude on the SETTINGS tab; each has its own key and model list.
- **RUN is greyed out** until there is a plan ready to run.
- **The plan shows one step per line**, so longer plans are readable (four lines, then a count of the rest).
- **Unsaved projects are spotted.** If the open project is new or unsaved, the agent will not guess track numbers and asks you to save first.
- **RUN is more forgiving:** a waiting plan still runs after you press other buttons, and a plan that has run is cleared.
- **Clearer labels:** the agent switch is CONTROL MPC, the play switch is LOOP PART.
- **MPC OS 2.x builds** for first-generation MPCs, and Gen 2 (aarch64) builds.

---

## What Daemon can do

### Generate parts
- **Chords, melodies, basslines and beats** from a sentence: "four bars of jazzy minor 9ths", "a rolling acid bassline", "a lazy
  boom-bap beat with ghost notes".
- Every part follows your **key, scale and length**, and plays immediately, in time with MPC's transport, through Daemon's own MIDI port
  to whichever instrument you point it at.
- **REGEN** asks for another take of the same idea. **SAVE** writes the part as a standard MIDI file you can drop into any clip.
- **Voice:** tap TALK and speak instead of typing (with the Daemon Listen effect on your mic track).

### Build the session around it (Agent mode)
Switch **CONTROL MPC** on (agent mode), and Daemon can work your MPC for you:

| Ask for | Daemon does |
|---|---|
| "put it in a clip on track 3" | saves the part and loads it into the first free clip slot of that track |
| "add a track" | adds a new Plugin track at the end of the project |
| "load Solina on track 4" | loads an instrument onto a plugin track |
| "use the Rich Strings preset from the Modern folder" | opens the preset list and picks it, folders included |
| "add a reverb and a delay to track 5" | loads effects into the track's free insert slots, choosing from what is actually installed |
| "make it a plucky deadmau5 lead" | reads the synth's parameters and sets cutoff, envelopes and the rest |
| "start playback", "stop" | presses transport for you |

- It knows **your device**: the project's tracks and their plugins, every plugin's parameters, and every installed effect and
  instrument. That covers MPC's own (AIR Reverb, AIR Delay, AIR Multi-Chorus, compressors, filters, tape and lo-fi) as well as your third-party VSTs.
- It picks the **obvious, musical choice** (a reverb called reverb, a delay called delay) and tells you plainly when nothing installed
  fits, instead of guessing.
- **Style-aware:** "dubby", "ambient", "lo-fi" or "huge" bring the effects that style needs.
- **Safe by design:** everything is shown as a numbered plan first. Steps that replace an instrument or record over something need RUN
  pressed **twice**. Nothing is ever deleted or renamed.

---

## The screen

Daemon has two tabs at the bottom of its screen: **DAEMON** and **SETTINGS**.

### DAEMON tab

**Read-outs**
| | |
|---|---|
| **STATUS** | what Daemon is doing: READY, TYPING, LISTENING, THINKING, PLAN *n* STEPS, RUNNING, RAN *n* STEPS, or an ERROR with the reason |
| **YOU** | what you typed or said |
| **DAEMON** | Daemon's answer: a short description of the part or the plan, or a reply when there is nothing to do |
| **PLAN** | the numbered steps Agent mode will carry out when you press RUN, one per line (up to four lines; a longer plan says how many more) |
| **Face** (top right) | Daemon's mood at a glance: idle, listening, thinking, done, typing, or error (the crossed-out eyes) |
| **Add-ins line** (top right) | whether the two helper add-ins are reachable (see [Requirements](#requirements)) |

**Musical settings**
| | |
|---|---|
| **TYPE** | what to make: Monophony (single lines, basslines), Melody, Chords, Melody+Chords, Beats |
| **KEY** | the root, C to B |
| **SCALE** | Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Harmonic Minor, Minor Pentatonic, Blues |
| **BARS** | length of the part, 1 to 8 bars |
| **CH** | the MIDI channel Daemon plays on (1 to 16): set it to the channel your instrument listens to |

Your words win over the menus. "In E phrygian" or "16 bars" overrides KEY, SCALE and BARS for that request.

**Buttons**
| | |
|---|---|
| **[TALK]** | tap, speak, tap again to send. Greyed out while the Daemon Listen effect isn't detected; tapping it then tells you what is missing |
| **[TYPE]** | type with a USB or Bluetooth keyboard. **Enter** sends, **Esc** cancels. Wait for "TYPING: Enter sends" before you start |
| **[KEYS]** | an on-screen keyboard, no hardware needed: **SEND** asks, **BACK** deletes, **CLEAR** empties, **HIDE** closes |
| **[RUN]** | carries out the plan shown under PLAN (twice for plans flagged as replacing something). Greyed out until there is a plan ready to run |
| **CONTROL MPC** | the agent switch. Off: every request makes a part. On: requests can also change your session (tracks, plugins, effects, presets, parameters, clips) |
| **[REGEN]** | another take of the last request |
| **LOOP PART** | on: the current part loops in time with MPC through Daemon's MIDI port. Off: it stays silent (SAVE and clips still work) |
| **[SAVE]** | write the current part to `<drive>/Daemon/<name>.mid` |

**Q-Links** on this tab: TALK, TYPE, RUN, CONTROL MPC, TYPE (part type), KEY, SCALE, BARS.

### SETTINGS tab
| | |
|---|---|
| **AI PROVIDER** / **[SWITCH]** | Gemini (Google) or Claude (Anthropic). Saved straight away; the model list follows. Claude does not take audio, so with Claude a spoken request still goes to Gemini (TALK needs `api_key` too) |
| **API KEY** | whether the active provider's key is set, showing only its last four characters. Keys live in `engine/daemon.conf` |
| **MODEL** | the model Daemon uses with that provider, and its place in the list |
| **[< PREV] / [NEXT >]** | choose another model. Saved straight away, used from the next request, no reload needed |
| **[REFRESH LIST]** | asks the provider which models your key may use (for Gemini, text models only; image, speech and transcription models are left out) |
| **ADD-INS** | the state of the commander and remote add-ins |
| **DAEMON LISTEN** | whether the voice-input effect is found |

**Q-Links** on this tab: SWITCH, PREV, NEXT, REFRESH.

If a request fails with "quota exceeded", "high demand" or "overloaded", step to another model or switch provider here. Daemon already retries busy answers
three times on its own.

---

## Requirements
- An Akai MPC OS standalone device:

  | Device | Download | MPC OS |
  |---|---|---|
  | Force and first-generation MPCs (Gen 1, armv7 CPU) | `Daemon-1.1.0-mpc-armv7.zip`, `Daemon-Listen-1.1.0-mpc-armv7.zip` | 2.x and 3.x |
  | Second-generation MPCs (Gen 2, aarch64 CPU) | `Daemon-1.1.0-mpc-aarch64.zip`, `Daemon-Listen-1.1.0-mpc-aarch64.zip` | 3.x |

  Not sure which you have? Over ssh, `uname -m` prints `armv7l` on Gen 1 and `aarch64` on Gen 2. A device on MPC OS 2.x is Gen 1.
- Network access and a **Gemini API key** (free keys work, with daily limits) or an **Anthropic API key** for Claude (voice input still needs the Gemini key).
- For **Agent mode**, two add-ins on the device:
  - **mpc-addin-commander** (port 6730): reads the project, plugins and parameters, sets parameters and transport.
  - **mpc-addin-remote** (port 6720): screen, touch and buttons, used to add tracks, load plugins and presets, and import clips.
  Daemon checks both every ten seconds. Without them, making parts, voice, typing and saving all still work.
- For **voice**, the **Daemon Listen** effect (included) on the audio track that carries your mic.

## Install
1. Download the two zips for your device from the [Releases page](https://github.com/sd88me/mpc-vst-daemon/releases). Unzip each one
   on your computer, copy the folder to the device and run its `install.sh` as root over ssh (it stops MPC, installs the plugin, backs up MPC.settings and starts MPC
   again). Each zip's `INSTALL.md` also explains how to install by hand.
2. Create `engine/daemon.conf` inside Daemon's plugin folder. `engine/daemon.conf.example` shows every option, and the file is kept
   when you upgrade:
   ```
   api_key=YOUR_GEMINI_KEY
   model=gemini-3-flash-preview     # optional: any model from the SETTINGS tab
   claude_key=YOUR_ANTHROPIC_KEY    # optional: to use Claude
   provider=claude                  # optional: gemini (the default) or claude; also set on the SETTINGS tab
   claude_model=claude-sonnet-5-5   # optional: any Claude model from the SETTINGS tab
   clips_dir=Daemon                 # optional: where SAVE writes, a folder at the top of the drive
   ```
   Daemon never shows or logs the key.
   On MPC OS 2.x the add-ins that Agent mode needs may not be available; making parts, voice, typing and saving work without them.
3. For Agent mode, put the screen automations for your device in `engine/macros.json`. For a Force on MPC OS 3.9.1, copy
   `docs/examples/macros.force-mpc-os-3.9.1.json`. Other devices: see `docs/MACROS.md` to record your own. Without the file the agent
   simply does not offer the screen actions.
4. In **Preferences > MIDI**, switch on Global, Control and Track for **Daemon MIDI Out**. Give the instrument track you want
   to hear an input of All Ports (or Daemon MIDI Out), and set Daemon's **CH** to that instrument's channel.

## First steps
1. Add **Daemon** as the instrument of a track and open its screen (Main mode, select its track, then the arrow next to the plugin name).
2. Tap **[TYPE]** or **[KEYS]**, type "dark minor chords, slow" and send. The part plays at once.
3. Switch **CONTROL MPC** on and try "put that in a clip on track 2", or "add a reverb and a delay to track 2". Read the PLAN, then press **[RUN]**.
4. After a plan that adds tracks or plugins, **save the project**. The status line reminds you. The agent reads the saved project,
   so this keeps its picture of your session current.

## Tips
- Name the track and the instrument: "load Profit-8 on track 3" works better than "load a synth".
- Ask for one musical job at a time, then build: part first, then clip, then sound, then effects.
- A screen action can take a few seconds per step. Afterwards MPC shows the Mixer; reopen Daemon to read the result.
- A plan saved with the project comes back after a reload. Read it before you press RUN.
- Typed prompts also arrive through a file: write one line to `engine/prompt.txt` (over the network share or ssh) and Daemon picks it up.

## Troubleshooting
| Message | What to do |
|---|---|
| ERROR no api_key / no claude_key | add the key for the chosen provider to `engine/daemon.conf` |
| ERROR no mic | insert Daemon Listen on the mic's audio track |
| ERROR 429 / 503 | quota used up or the provider busy: pick another model or switch provider on the SETTINGS tab |
| voice needs api_key (Gemini) | with Claude, TALK still uses Gemini to hear you: add `api_key=` too, or type |
| stopped in *macro* at step *n* | the screen was not where the automation expected (another page open, a dialog, a different MPC OS version). Close dialogs, try again, or see `docs/MACROS.md` |
| the agent doesn't know a new track | save the project, then ask again |
| letters missing at the start of a typed prompt | wait for "TYPING: Enter sends" before typing |

Logs for the curious: `engine/daemon.log` (state, MIDI, transport; rewritten twice a second) and `engine/agent.log` (every agent lookup and any failed step).

## Efficiency
- **Planning stays small.** The agent sees a compact view of your project (real tracks only, about 0.5 KB instead of 9 KB), installed
  effects as short name lists grouped the way MPC's picker groups them, and only the parameters that match what it is looking for.
  Planning is capped at six rounds per request.
- **Screen actions check themselves.** Fixed screen fingerprints confirm every page before the next touch, and the model is only
  asked when a name has to be found (a plugin, a preset, a folder), using a half-size screenshot.
- **Audio stays light.** Generation, networking and screen work run on a background thread. The audio callback only plays
  notes and, while you talk, copies the mic.

## For developers
Daemon is built on the shared VST2 wrapper from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).
- `src/daemon_engine.c`: the engine (buttons, state, worker thread). `daemon_core.c`: part requests, reply parsing, MIDI file, mic capture.
- `src/daemon_claude.c`: Claude behind the Gemini wire format (requests and replies translated).
- `src/daemon_agent.c`: the function-calling agent and its tools. `daemon_cmdr.c`: the commander client.
- `src/daemon_macro.c`, `daemon_fp.c`, `daemon_mcp.c`: screen automations, screen fingerprints, the remote add-in client.
- `listen/`, `src/daemon_tap_engine.c`: the Daemon Listen effect. `vst/`: parameters, layout and skin.
- `test/run.sh` runs every offline suite under ASan/UBSan (set `MPC_VST` to an mpc-vst-plugins checkout). `tools/build_port.sh vst/vst.json armv7`
  (from that checkout) builds the device plugin.
- `docs/MACROS.md` documents the automation format; `docs/HANDOFF.md` is the development log with verified device facts.

## Licence
GPL-3.0-or-later (see `LICENSE`). The shared wrapper is MIT; libcurl and libasound are loaded at run time, not linked. If you
distribute a modified version, make its source available under the same licence. "Daemon" is a working name; the licence grants
no right to the name or branding.
