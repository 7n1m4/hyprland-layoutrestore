# AGENTS.md

## What this repo is

`layoutrestore` is an **out-of-tree Hyprland plugin**: a shared library that the compositor
`dlopen()`s. It adds `hyprctl layoutsave` / `hyprctl layoutrestore [arm]` to persist and restore a
window layout — geometry, workspace, monitor and floating state, keyed by app-id/class — plus
autosave and relaunch of windows that no longer exist.

It is not part of the Hyprland source tree. Nothing here builds with Hyprland's build system, and
**the Hyprland repo rules do not apply here** (no Hypr org contribution policy, no `plugin:` access
to `src/`, no ABI guarantee beyond what the installed headers declare).

## Repo layout

| Path | Role |
|---|---|
| `main.cpp` | Plugin entrypoints, hyprctl handlers, event listeners, save/restore/relaunch logic |
| `globals.hpp` | Plugin ABI includes, config value names, command names |
| `src/LayoutModel.{hpp,cpp}` | **Hyprland-independent**: JSON schema (glaze), matching, staleness, cmdline join, order sorting |
| `tests/plugins/LayoutModel.cpp` | gtest over `LayoutModel` — includes the `.cpp` directly, no linking |
| `hyprtester/plugins/` | The integration test + notes, meant to be copied into a Hyprland checkout |
| `Makefile` | `all` / `test` / `clean` |
| `hyprpm.toml` | hyprpm manifest (`hyprpm add <url>` / `hyprpm enable layoutrestore`) |

## The one architectural rule

> `src/LayoutModel.cpp` **must not include any Hyprland header.**

`main.cpp` holds everything that needs the compositor (`PHLWINDOW`, config values, the event bus,
`g_layoutManager`). `LayoutModel` holds everything that doesn't, which is what makes it unit
testable without running a compositor — the test compiles the `.cpp` into itself and runs in
milliseconds. Keep it that way. If you're about to add logic that needs `PHLWINDOW`, it belongs in
`main.cpp`; if it needs neither, it belongs in `LayoutModel`.

## Conventions

- C++26, single-line `if`/`else` without braces. There is no `.clang-format` here; formatting follows
  Hyprland's style, so run `clang-format` with Hyprland's config when you have a checkout available
  (e.g. `clang-format --style=file` inside one).
- JSON key names are the on-disk contract and live in `glz::meta<SEntry>` in
  `LayoutModel.cpp`; `cls` maps to `class`. Changing them is a **file-format break** — bump
  `SLayoutFile::version` and make `deserialize` tolerate the old shape.
- Every config value name starts with `plugin:` (Hyprland requirement).
- Static file-scope state (`g_*`) is deliberate: the plugin has no object to hang state off, and
  `globals.hpp` holds the shared constants.

## Build & test

```sh
make all   # -> layoutrestore.so, against the installed Hyprland headers
make test  # gtest over LayoutModel, needs a Hyprland checkout to resolve <hyprland/src/...> includes
make clean
```

The plugin links against no Hyprland library — the executable's symbols resolve when it is
`dlopen()`ed. Building against the wrong headers is the usual failure: `hyprpm update` (or
`make installheaders` in the Hyprland tree) after any Hyprland upgrade, then rebuild.

## Working on it

**Development happens in a Hyprland checkout** (`plugins/layoutrestore/`, `tests/plugins/`), because
`make test` needs `<hyprland/src/...>` and the gtest binary is built by Hyprland's CMake. Changes
made here at `~/hyprland-layoutrestore` need those same two paths mirrored back into the checkout,
and the include paths adjusted, to be built and tested locally.

To publish: build + run `make test`, then update this repo's `README.md` (usage, config table,
matching rules, file format) when behavior changes, bump the version string in
`PLUGIN_DESCRIPTION_INFO`, and `git push`. Tags are the release mechanism for hyprpm.

## Testing constraints in the dev environment

- `make test` covers the model layer and is fast and reliable.
- `hyprtester/plugins/layoutrestore.cpp` is the end-to-end test. **It cannot run on the dev
  machine**: no DRM seat or GBM buffer is available while a live compositor holds them, so the nested
  Hyprland dies before the test body executes. This is environmental — the in-tree reference
  plugin crashes identically. Do not try to run it locally; use CI or a machine with a free GPU.
- Do **not** launch nested Hyprland runs to smoke-test: it steals focus and disrupts the live
  session. See issue #1 for the runtime-verification path.
