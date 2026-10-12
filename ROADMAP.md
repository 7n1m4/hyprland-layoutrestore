# Roadmap

Enhancement ideas, in rough priority order. Nothing here is committed work: each item states
**why it's broken or limited today** and **what "done" looks like**, so it can be picked up without
re-deriving the reasoning.

Two entries are real bugs found while writing this and are worth taking before new features:

---

## Known issues

### 1. Relaunch can double-spawn, and `arm` never disarms

`g_deferred` is a counter that only ever increments (`++g_deferred` on every `arm`) and is
decremented per successfully matched window. It never reaches zero in practice, and there is no way
to disarm: once armed, every window that opens for the rest of the session is matched against the
saved file.

Worse, `arm` **relaunches immediately and re-arms forever**. Two `arm` calls spawn the same
commands twice, and a second `arm` after the layout has repopulated will relaunch windows that are
already open.

**Done when:** `arm` takes a bounded lifetime (a deadline, or disarms after N matched windows /
a timeout), repeated `arm` does not double-spawn, and there's an explicit way to stop
(`layoutrestore disarm`, or `arm` re-arming cleanly replaces the previous state).

### 2. Autosave will overwrite a good layout during relaunch

Relaunch spawns N processes; each new window sets `g_dirty`, and `onTick` autosaves after the
interval. So a restore in progress gets written over mid-flight, and the relaunch window is lost
from the very next restore. `layoutrestore arm` reads the file *after* `restoreOpenWindows` has
already consumed entries from its own copy — a race with the autosave writing a different file.

**Done when:** arming suspends autosave until the restore settles (or at minimum, autosave never
overwrites a file that a pending restore is still consuming).

### 3. `restoreOpenWindows` applies entries to windows regardless of whether they were relaunched

Two windows of the same class that were both open at save time and both still open at restore will
both be matched — fine. But a window that was **not** saved (opened after the save) can consume a
saved entry meant for a relaunched sibling, and the relaunched window then finds nothing.

**Done when:** entries are claimed by identity (pid or a save-time window id), not just by
class/title order, or relaunched entries are reserved until their window maps.

---

## Features

### 4. Layered / removed-window handling

`readFile` reads the whole JSON into memory and `matchEntryIndex` is a linear scan, so save/restore
is O(windows²) and a large layout is slow. Fine at 20 windows, painful at 200+.

**Done when:** matching is a class → entries index built once per restore, or entries are keyed.

### 5. Workspace- and monitor-scoped restore

Right now `layoutrestore` rebuilds every saved entry onto whatever workspace exists. A multi-monitor
user cannot restore workspace 2 to DP-2 without pulling everything else along.

**Done when:** `layoutrestore <workspace>` / `layoutrestore monitor:<name>` restore a subset, and
entries from other workspaces are left untouched.

### 6. Per-window overrides

Some windows should not be restored (a transient confirm dialog), and some need a fixed position
that tiling would never produce. Today there's no way to express either.

**Done when:** entries can carry a `noRestore` flag, and a fixed geometry is honored for tiled
windows when explicitly requested (this is the one case where forcing geometry is correct).

### 7. Ordering that survives a changed window set

Order restore assumes the same window set comes back. Open an extra terminal of the same class and
the indices shift, so a scrolling layout ends up with a different arrangement than saved.

**Done when:** entries are matched to windows by identity where possible, with order only
break ties, so an extra window doesn't displace the rest.

### 8. Save on exit, not just on a timer

The current autosave is a 5s poll, so a clean shutdown can lose up to 5 seconds of layout changes.
There's no `configReloaded`/pre-exit hook wired for a final save.

**Done when:** a final save runs on configured shutdown, and `layoutsave` remains the manual
override.

### 9. hyprtester coverage

The integration test exists in `hyprtester/plugins/` but has never run — no GPU/DRM seat on the dev
machine. Everything above is verified only through the `LayoutModel` unit tests.

**Done when:** the test runs in CI (NixOS VM with virtio-gpu) and asserts relaunch, order restore,
and geometry precedence end-to-end.

---

## Non-goals

- Restoring geometry for arbitrary tiled windows by force. The layout owns that; restoring insertion
  order is the correct mechanism. Per-window overrides (item 6) are the exception.
- Any change under Hyprland's `src/`. This is an out-of-tree plugin and must stay one.
