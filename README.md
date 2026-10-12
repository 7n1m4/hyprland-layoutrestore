# layoutrestore

A Hyprland plugin that saves window layout and restores it: geometry, workspace, monitor and
floating state, keyed by app-id/class.

| | |
|---|---|
| Commands | `hyprctl layoutsave`, `hyprctl layoutrestore [arm]` |
| Autosave | On by default; only writes when something changed and the interval has elapsed |
| Restore model | Deferred — entries are re-applied to windows as they open, so it survives shutdown and crash |

## Why deferred

When the compositor shuts down or crashes, every client window dies with it. `layoutrestore`
cannot reposition windows that no longer exist, so instead of restoring on load it arms a
matcher: each window that opens is matched against the saved entries by class and title, and
immediately gets its saved workspace, monitor, floating state and geometry back.

## Installing

```sh
hyprpm add https://github.com/7n1m4/hyprland-layoutrestore
hyprpm enable layoutrestore
hyprctl reload
```

A compositor is only able to load plugins built against its exact headers, so build the plugin
against the Hyprland you run (run `hyprpm update` after upgrading Hyprland).

## Usage

```sh
hyprctl layoutsave          # write the layout to disk now
hyprctl layoutrestore       # apply saved entries to windows that are open
hyprctl layoutrestore arm   # also restore windows that open later
```

A typical crash-recovery flow: `hyprctl layoutrestore arm` early in your session (e.g. from an
`exec-once` or a keybind). Saved windows that are gone come back on their own, and the matching
placement below runs as each one opens.

## Restoring windows, not just positions

`layoutrestore arm` does two things:

1. **Re-execs the saved commands** for windows that no longer exist. Each saved entry stores the
   `argv` it was captured from, so the same process comes back with the same arguments.
2. **Places windows as they open.** When a window maps, `window.openLate` matches it to its saved
   entry and applies workspace, monitor, floating state and (for floating only) geometry.

Relaunched commands are spawned per workspace in **saved insertion order**. That order is what a
derived layout (scrolling, master-stack, dwindle) turns into geometry — so restoring the order
restores the tiled positions without forcing geometry onto the tiling algorithm, which would
immediately override it.

A window is never relaunched when it has no captured command: kernel threads, `/proc` entries whose
cmdline is unreadable, and **compositor-side processes** (XWayland's server, compositor children) are
skipped, since spawning those from a restore is not something you want.

## Configuration

| Value | Type | Default | Meaning |
|---|---|---|---|
| `plugin:layoutrestore:file` | string | `~/.local/share/hyprland/layoutrestore.json` | Where the layout is written |
| `plugin:layoutrestore:autosave` | bool | `true` | Autosave on layout changes |
| `plugin:layoutrestore:autosave_interval` | int | `5000` | Minimum ms between autosaves |
| `plugin:layoutrestore:staleness_seconds` | int | `0` | Drop entries older than this (`0` = never expire) |

## Matching rules

- A window's class (app-id) must match a saved entry's class. Windows without a class never match.
- An empty saved title is a wildcard. A non-empty saved title must match the window's title exactly.
- Among the candidates, the newest entry wins; on equal timestamps the later one in the file wins.
- Each saved entry is consumed by at most one window, so two windows of the same class each get
  their own saved state.
- Entries older than `staleness_seconds` are ignored at load time.

Geometry is recorded for every window, but only restored for floating ones — a tiled window's
position is owned by the tiling algorithm, which would immediately override it. For tiled windows
the insertion order is restored instead, which reproduces the position under a deterministic layout.

## File format

```json
{"version":2,"timestamp":1770000000,"entries":[
  {"class":"kitty","title":"nvim","workspace":"2","monitor":"DP-1",
   "x":100,"y":50,"width":800,"height":600,"floating":true,
   "workspaceSpecial":false,"timestamp":1770000000,"cmdline":"/home/me/.local/bin/kitty --directory=/src","order":0}
]}
```

No two fields are required, and older files still load: an entry with no `cmdline` can only be
matched, not relaunched, and an entry with no `order` is placed after every known-order entry.

## Building

```sh
make
```

Needs the Hyprland headers for your running compositor (`hyprpm update`, or
`make installheaders` in a Hyprland checkout) plus the usual Wayland dev packages.

## Tests

```sh
make test        # unit tests: JSON round-trip, matching, staleness, file IO
```

The plugin logic that does not need a compositor lives in `src/LayoutModel.{hpp,cpp}` and is
covered by `tests/plugins/LayoutModel.cpp` (gtest).

There is also a `hyprtester` integration test under `hyprtester/plugins/` driving the real
compositor over its socket: `hyprctl layoutsave`, respawn the window, `hyprctl layoutrestore`,
then assert the window is back on its saved monitor/workspace with its saved geometry and
floating state. See the issue tracker for its status — it needs an environment with a usable
GPU and DRM seat.

## License

BSD-3-Clause, matching Hyprland. See [LICENSE](LICENSE).
