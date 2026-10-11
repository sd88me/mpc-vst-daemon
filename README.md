# Daemon (working name)

Voice and keyboard prompts to MIDI parts for Akai MPC OS standalone devices, as a native VST2 effect plugin
(mic in on an audio track) that plays through its own MIDI port. Modelled on a Max for Live generator device:
prompt + type + key/scale + length in, notes out. Skin theme: amber phosphor, cyan accent.

Status: the core (`src/daemon_core.c`: reply checking, loop player, `.mid` writer, Gemini request) passes
`test/run.sh` offline; one live Gemini call (`test/live_gemini.sh`) parses. No plugin wrapper, skin or device test yet.
