# Tap macros

Daemon's agent can select a track and load a plugin onto it by driving MPC's own screens, through the remote addin
(jacob-sabella/mpc-addin-remote, MCP on `127.0.0.1:6720`). Nothing in the commander addin can do this, and MPC has no API for it.

Touch positions depend on the device and its screen layout, so they are **data you record on your own device**, in
`macros.json` next to `daemon.conf`. Without that file the agent is simply not offered the two tools. **Status: the engine is tested offline against a fake remote addin, and `import_clip` has been recorded and run on a Force (MPC OS 3.9.1, 2026-10-12, `docs/examples/macros.force-mpc-os-3.9.1.json`). `select_track` and `load_plugin` are not recorded yet.**

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
    "load_plugin": {
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
(The numbers above are placeholders, not a recording.) `select_track(track)`, `load_plugin(track, plugin)` and `import_clip(track, folder)` are the names
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
  path text (tolerance 2.5) stops the macro if it is not the folder expected.
- The browser's file-type filter hides MIDI files unless the MIDI icon is on, and a LOAD does nothing unless a clip slot is selected first.
- The remote addin's `scroll` takes an `amount` of 20 to 2000 (pixels), not a count. Fingerprint regions of animated things (a playing clip, the scene column) never settle: use the
  empty lower rows of the matrix and the black bottom bar.

## The vision step
`find_tap` takes a screenshot, asks Gemini where the named button, tile or list entry is (a point in 0 to 1000 of the image, converted
to screen pixels), and taps it. It is how `load_plugin` can pick "9W9" out of a list without knowing its position. It only taps when
the model says it is clearly there and unique; otherwise the macro stops and says why. Use `hint` to say what screen should be showing.

## Safety
Every screen action is a **changing** tool, so it appears in the plan and runs only when you press RUN. If a macro step fails, the
run stops there, because the screen is in an unknown state and later steps could land on the wrong thing. The agent never taps
by itself outside a macro.

## Recording one
Check the file with `tools/macro_check macros.json` (build line in its header). To record, use the remote addin's web page or MCP
with `screenshot grid=true` to read coordinates, perform each step once, and write them down; add `require_change` to each touch,
then run the macro from the agent and watch the screen. Re-record when the MPC OS or the screen layout changes.
