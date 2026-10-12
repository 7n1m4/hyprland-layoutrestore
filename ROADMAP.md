# Roadmap

Enhancement ideas, in rough priority order. Nothing here is committed work: each item states
**why it's broken or limited today** and **what "done" looks like**, so it can be picked up without
re-deriving the reasoning.

Two entries are real bugs found while writing this and are worth taking before new features:

---

## Known issues

### 1. ~~Relaunch can double-spawn, and `arm` never disarms~~ — CLOSED

`g_deferred` was a counter that only ever incremented and could never be cleared, so an arm persisted
for the rest of the session, and every `arm` spawned a fresh wave of relaunched windows.

Closed by replacing it with an explicit `g_armed` state bounded three ways:

- a 30-second deadline (`ARM_DEADLINE_S`), checked at the top of `onWindowOpenLate`;
- an auto-disarm once every relaunched process has matched and no entry is left to place;
- `hyprctl layoutrestore disarm`, which drops the state immediately.

A re-arm while already armed reloads the file but sets `FRESH_ARM = false`, so it does not spawn a
second wave.

### 2. ~~Autosave will overwrite a good layout during relaunch~~ — CLOSED

Closed by `g_restorePending`, a gate at the top of `onTick()`. It is set when a relaunch is actually
in flight (not merely armed) and cleared when the deferred state ends, so autosave never rewrites the
file a pending restore is still consuming. It is a gate rather than a latch, so it cannot wedge
autosave permanently.

### 3. ~~A non-saved window can steal a relaunched sibling's entry~~ — CLOSED

Closed by recording the saved window's `pid` on each entry. An already-open process claims its own
entries by pid first via `matchEntryIndexByIdentity` + `consumeEntries`, and only falls back to
class/title matching when no pid was captured (a process that was reaped at save time).

Also fixed while closing this: `matchEntryIndexFrom` returned a **cached index into
`file.entries`**, which goes stale (and out of bounds) the moment the restore loop consumed an entry.
Multi-window restores read past the end of the vector and aborted. It is replaced by `matchEntryIn`,
which returns a pointer resolved against the live vector, so it cannot drift.

---

## Features

### 4. ~~Layered / removed-window handling~~ — CLOSED

`readFile` read the whole JSON into memory and `matchEntryIndex` was a linear scan, so save/restore
was O(windows²). Closed by `buildClassIndex` + `matchEntryIndexFrom`, which scan only a window's own
class bucket. (That path then got bypassed — see item 3 — so the remaining cost is a linear scan of
just the matching class, which is fine at realistic window counts.)

### 5. Workspace- and monitor-scoped restore

Right now `layoutrestore` rebuilds every saved entry onto whatever workspace exists. A multi-monitor
user cannot restore workspace 2 to DP-2 without pulling everything else along.

**Done when:** `layoutrestore <workspace>` / `layoutrestore monitor:<name>` restore a subset, and
entries from other workspaces are left untouched.

### 6. Per-window overrides — HALF CLOSED

The opt-out half is done: `plugin:layoutrestore:no_restore` saves a window with `"norestore":true`,
and `loadLayoutFile()` drops those entries through `restorableEntries()` before any restore path sees
them.

Still open: forced geometry for a tiled window. Forcing it is a special case the current design
deliberately avoids — a tiled window's geometry is recomputed by the layout on the next recalculate,
so a restored value survives only until something touches the layout.

**Done when:** a per-window fixed geometry is honored for tiled windows when explicitly requested.

### 7. Ordering that survives a changed window set

Order restore assumes the same window set comes back. Open an extra terminal of the same class and
the indices shift, so a scrolling layout ends up with a different arrangement than saved.

**Partially mitigated** by item 3's pid-based claiming: a window that was relaunched with the same
pid now claims its own entry, so an unrelated window of the same class cannot displace it. The
remaining exposure is windows whose pid could not be captured at save time.

**Done when:** entries are matched to windows by identity where possible, with order only breaking
ties, so an extra window doesn't displace the rest.

### 8. Save on exit, not just on a timer — CLOSED

The autosave was a 5s poll, so a clean shutdown could lose up to 5 seconds of layout changes. Closed
by a listener on `Event::bus()->m_events.exit` that saves once more, gated on `g_dirty` so an
unchanged layout is not rewritten, and controlled by `plugin:layoutrestore:save_on_exit` (default
true). `layoutsave` remains the manual override.

### 9. hyprtester coverage

The integration test exists in `hyprtester/plugins/` but has never run — no GPU/DRM seat on the dev
machine. Everything above is verified only through the `LayoutModel` unit tests.

The test now also asserts the two behaviours that were previously undefined: that a window opened
after `disarm` keeps its own state, and that re-arming does not spawn a second wave.

**Done when:** the test runs in CI (NixOS VM with virtio-gpu) and asserts relaunch, order restore,
and geometry precedence end-to-end.

---

## Non-goals

- Restoring geometry for arbitrary tiled windows by force. The layout owns that; restoring insertion
  order is the correct mechanism. Per-window overrides (item 6) are the exception.
- Any change under Hyprland's `src/`. This is an out-of-tree plugin and must stay one.
