# Tap macros

Daemon's agent can select a track and load a plugin onto it by driving MPC's own screens, through the remote addin
(jacob-sabella/mpc-addin-remote, MCP on `127.0.0.1:6720`). Nothing in the commander addin can do this, and MPC has no API for it.

Touch positions depend on the device and its screen layout, so they are **data you record on your own device**, in
`macros.json` next to `daemon.conf`. Without that file the agent is simply not offered the two tools. **Status: the engine is tested offline against a fake remote addin, and `import_clip` has been recorded and run on a Force (MPC OS 3.9.1, 2026-10-12, `docs/examples/macros.force-mpc-os-3.9.1.json`). `select_track` and `load_efx` were recorded 2026-10-12 (load_efx verified end to end: "load the plugin AW Tape - TapeHack2 on track 3" -> plan -> RUN -> TapeHack2 in track 3 insert 1, vision `find_tap` included).**

## Format
```json
{
  "version": 1,
  "screen": [1280, 800],
  "macros": {
    "select_track": {
      "params": { "track": { "type": "int", "min": 1, "max": 16 } },
      "steps": [
        { "press": "TRACK" },
        { "tap": ["100 + 140*(track-1)", 300], "require_change": true, "label": "track cell" }
      ]
    },
    "load_efx": {
      "params": { "track": { "type": "int", "min": 1, "max": 16 }, "plugin": { "type": "string" } },
      "steps": [
        { "call": "select_track" },
        { "tap": [640, 400], "require_change": true },
        { "find_tap": "{plugin}", "hint": "the list of plugins" }
      ]
    }
  }
}
```
(The numbers above are placeholders, not a recording.) `select_track(track)`, `load_efx(track, plugin)` and `import_clip(track, folder)` are the names
the agent uses; each takes the parameters shown. `import_clip` is run by Daemon after it saves the part: `folder` is the saved-parts folder
(`clips_dir`, a folder directly on the drive, `Daemon` by default).

- **`screen`**: the size the macros were recorded for. They refuse to run on a screen of another size.
- **Parameters**: `int` (with `min`/`max`) or `string` (1 to 80 characters). Missing, mistyped or out-of-range values stop the macro.
- **Coordinates** are numbers or expressions in the parameters: `+ - * /`, brackets, unary minus. They must land on the screen.
- **Steps**, exactly one action each:
  `tap`, `double_tap`, `long_press` (`[x, y]`, optional `hold_ms`), `press` (a hardware button name from the remote addin's
  `list_buttons`), `wait` (ms, 0 to 5000), `wait_stable` / `wait_change` (timeout ms), `scroll` (`[x, y]` with `direction`
  and `amount`), `find_tap` (see below), `find_empty` (see below), `call` (another macro; at most 4 deep), `repeat` (a count up to 20 with `steps`).
  `{param}` inside a text value is replaced by the parameter's value.
- **`require_change`**: stop if the screen did not change after the touch, which catches a coordinate that no longer hits
  anything. Set it on every touch that must do something. A `find_tap` has it on by default.
- At most 200 steps run per macro run, and `label` is only for you.

## Fingerprints: check the screen without a model
A fingerprint is a 16 x 8 grid of average brightness of a screen region. A step `{ "expect": "page_name" }` takes a fresh screenshot of just
that region, compares it with the stored grid, and waits up to `timeout_ms` (default 2000) for it to match, because screens take a
moment to settle. If it never matches, the macro stops and says how far off the screen was. Use it after every step that opens a page,
and after a `find_tap`, so a wrong tap is caught before the next one. Nothing here calls a model.
```json
"fingerprints": {
  "track_view": { "region": [0, 90, 1280, 120], "thumb": "<256 hex digits>", "tolerance": 12 }
}
```
`tools/macro_fp <port> <name> x y w h` prints the entry for the screen as it is now (build line in its header). Choose a region of fixed
furniture (titles, tabs, labels), not meters, a playhead or a blinking cursor. `tolerance` is the largest average brightness difference
(0 to 255) still accepted: small live changes pass, a different page is far over it.

## find_empty: the first free cell, without a model
`{ "find_empty": { "origin": [x, y], "step": [dx, dy], "size": [w, h], "count": 8, "like": "empty_slot" } }` checks up to `count` cells (centres
`origin + k*step`, coordinates may use the parameters) against the fingerprint `like`, taken from an empty cell, and taps the first that matches. If none
does it stops with "no empty slot among the cells checked". Pick `size` inside the cell so selection outlines do not count, and a
tight `tolerance` (5) for the empty-cell fingerprint, since an occupied cell differs only by about 15. `import_clip` uses it on the clip matrix
to find the next free slot of a track.

## Recorded on a Force (2026-10-12): things that mattered
- The browser's **folder-up** button (path line, top left) repeated 8 times always lands at the drive root, and refreshes a stale listing; tapping the drive's name does
  not go to the root when already on that drive. The list is sorted by modified date, newest first, so the folder just written to is the first row. A fingerprint of the
  folder-name part of the path (region `[150,40,140,26]`, tolerance 4; a wider region barely differs between folders) stops the macro if it is not the folder expected.
- The browser's file-type filter hides MIDI files unless the MIDI icon is on, and a LOAD does nothing unless a clip slot is selected first.
- The remote addin's `scroll` takes an `amount` of 20 to 2000 (pixels), not a count. The swipe starts at centre minus amount/2 and must start **inside** the list (a list at y 300-740 needs the centre near y 520 and an amount of about 400, repeated); a longer swipe starts above the list and does nothing. After folder-up the browser scrolls to the folder it came from, so always scroll to the top before tapping a row. The browser also remembers its last tab (Places or Content): tap Places and check it with a fingerprint first.
- The remote addin's `scroll` takes an `amount` of 20 to 2000 (pixels), not a count. Fingerprint regions of animated things (a playing clip, the scene column) never settle: use the
  empty lower rows of the matrix and the black bottom bar.

## Recorded: select_track and load_efx (Force, MPC OS 3.9.1)
- `select_track` goes through the **Mixer** (MIXER button; the clip matrix and its MATRIX button exist on the Force only): the strip headers are at x = 80 + 160*(track-1), y = 40 (8 tracks fit). On the Force's clip matrix the headers are at x = 72 + 146*(track-1) instead; a double tap on a header opens Track Settings, whose Plugin
  field only shows the instrument and does not open a list. Effects load from the **Mixer, EFFECTS tab**: the insert slots of a track strip sit at x = 80 + 160*(track-1),
  y = 370 + 86*(slot-1); tapping an empty one opens the plugin picker (TYPE tab; the VST group is the last row and stays expanded between openings), SELECT is at (960, 770).
- Step options added for this: `skip_if` (a fingerprint name; the step is skipped when the screen already looks like it, used so the VST group is not collapsed by a second tap)
  and, on `find_tap`, `tries` (0 to 8) with `scroll_at` [x, y] and `amount`: scroll the list and ask the model again.
- The bottom `+` / `-` buttons of the Track Settings dialog step to the next / previous track (past the last one they reach the submixes): do not use them to change values.

## load_efx vs load_instrument
`load_efx(track, plugin)` puts an effect into the first free insert slot (Mixer, EFFECTS tab). `load_instrument(track, plugin)` replaces a Plugin track's instrument: Mixer, **double tap the
track title** (Track Settings opens), **double tap the Plugin field** (it shows the current instrument; a single tap or long press does nothing), scroll the picker to its top and
`find_tap` the name (it opens scrolled to the current instrument), SELECT (960, 770), close the dialog with its X at (1152, 86). The agent is offered each tool only when its macro exists (`has_ui` bit 2 and 8).

## The vision step
`find_tap` takes a screenshot, asks Gemini where the named button, tile or list entry is (a point in 0 to 1000 of the image, converted
to screen pixels), and taps it. It is how `load_efx` can pick "9W9" out of a list without knowing its position. It only taps when
the model says it is clearly there and unique; otherwise the macro stops and says why. Use `hint` to say what screen should be showing.

## Safety
Every screen action is a **changing** tool, so it appears in the plan and runs only when you press RUN. If a macro step fails, the
run stops there, because the screen is in an unknown state and later steps could land on the wrong thing. The agent never taps
by itself outside a macro.

## Recording one
Check the file with `tools/macro_check macros.json` (build line in its header). To record, use the remote addin's web page or MCP
with `screenshot grid=true` to read coordinates, perform each step once, and write them down; add `require_change` to each touch,
then run the macro from the agent and watch the screen. Re-record when the MPC OS or the screen layout changes.

## load_preset
`load_preset(track, preset, folder?)` (agent tool bit 16): Mixer, EFFECTS tab, double tap the track title (Track Settings), double tap the PRESET field (940, 598). The preset list opens as a panel on the left; stock instruments such as Solina keep presets in folders (a row with an arrow), a preset row loads it and returns to Track Settings, then the X (1152, 86) closes. `folder` is optional: a macro parameter with `"optional": true`, and a step with `"if_param": "folder"` runs only when it was given. Plugins without presets (Dexed, Chordsmith, Clementine-XT here) have a PRESET field that does not open. Recorded 2026-10-14 on a Force (MPC OS 3.9.1) with Solina; not run end to end through the agent yet.
