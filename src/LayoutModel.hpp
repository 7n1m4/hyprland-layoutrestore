#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace LayoutRestore {

// Current on-disk schema version. Written by serialize(); deserialize()
// rewrites anything <= 0 to this so a missing/zero version field is treated
// as the current shape.
inline constexpr int SCHEMA_VERSION = 2;

struct SEntry {
  std::string cls = "";
  std::string title = "";
  std::string workspace = "";
  std::string monitor = "";
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool floating = false;
  bool workspaceSpecial = false;
  int64_t timestamp = 0;
  // Null-terminated argv as read from /proc/<pid>/cmdline. Empty when the
  // command could not be captured, in which case the entry can only be
  // matched against a window that is already open.
  std::string cmdline = "";
  // Position of the window within its workspace's layout. A derived layout
  // (scrolling, dwindle) computes geometry from this, so restoring the
  // order reproduces the saved geometry without forcing it. -1 = unknown.
  int order = -1;
  // The saved window's pid, 0 when unknown. Lets a window that still exists
  // claim its own entry by identity instead of by class/title guessing.
  int64_t pid = 0;
  // Explicit opt-out: the entry is persisted for its geometry but must not
  // be restored (its window relaunched) on the next session.
  bool noRestore = false;
};

struct SLayoutFile {
  int version = 2;
  int64_t timestamp = 0;
  std::vector<SEntry> entries;
};

// Splits a raw /proc/<pid>/cmdline blob into argv and re-joins it into a
// shell-free command string. Empty output means "not capturable".
std::string joinCmdline(const std::string &raw);

// Inverse of joinCmdline: splits the stored command string back into argv.
// CProcess takes argv and there is no shell, so the stored string has to be
// split before spawning or the whole line becomes argv[0].
std::vector<std::string> splitCmdline(const std::string &cmd);

// Whether an entry's window can be brought back by re-executing its command.
bool isRelaunchable(const SEntry &entry);

// Indices into `entries` of the relaunchable windows of one workspace, in
// saved insertion order. Indices let the caller act on the entries directly.
std::vector<size_t> relaunchOrder(const SLayoutFile &file,
                                  const std::string &workspace);

std::optional<std::string> serialize(const SLayoutFile &file);
std::expected<SLayoutFile, std::string> deserialize(const std::string &json);
// Index of the best matching entry, so callers can consume it from the vector.
// Returns nullopt when nothing matches.
std::optional<size_t> matchEntryIndex(const SLayoutFile &file,
                                      const std::string &cls,
                                      const std::string &title, int64_t now,
                                      int64_t stalenessSeconds);
std::optional<SEntry> matchEntry(const SLayoutFile &file,
                                 const std::string &cls,
                                 const std::string &title, int64_t now,
                                 int64_t stalenessSeconds);
SLayoutFile filterStale(const SLayoutFile &file, int64_t now,
                        int64_t stalenessSeconds);
std::expected<void, std::string> writeFile(const std::string &path,
                                           const SLayoutFile &file);
std::expected<SLayoutFile, std::string> readFile(const std::string &path);

// Indices of the entries saved for the same process: exact pid match AND the
// same class. Empty when pid == 0, since 0 means "unknown", not "this window".
std::vector<size_t> matchEntryIndexByIdentity(const SLayoutFile &file,
                                              int64_t pid,
                                              const std::string &cls,
                                              int64_t now,
                                              int64_t stalenessSeconds);

// Removes the given indices from `file`. Indices are applied highest-first so
// each erase leaves the remaining indices valid; duplicates and unsorted input
// are tolerated.
void consumeEntries(SLayoutFile &file, const std::vector<size_t> &indices);

// Indices of the entries that are allowed to be restored, in saved order.
std::vector<size_t> restorableEntries(const SLayoutFile &file);

// Entries bucketed by class, so a class-only query scans a subset instead of
// the whole vector. Indices refer to `file.entries` and stay ascending.
struct SClassIndex {
  std::unordered_map<std::string, std::vector<size_t>> byClass;
};
SClassIndex buildClassIndex(const SLayoutFile &file);

// matchEntryIndex() restricted to a class: same result for the same inputs,
// without scanning unrelated entries.
//
// Returns a pointer into `file.entries`, not an index. The class index caches
// absolute indices, which go stale the moment the caller consumes an entry
// (consumeEntries/erase shift everything above it), so a returned index would
// be off by one or out of bounds on the next query. A pointer is resolved
// against the live vector in the same expression, so it cannot go stale.
const LayoutRestore::SEntry *
matchEntryIn(const LayoutRestore::SLayoutFile &file, const std::string &cls,
             const std::string &title, int64_t now, int64_t stalenessSeconds);

// matchEntryIndex() restricted to the classes in `index`: same result for the
// same inputs, without scanning unrelated entries. Requires index to have been
// built from `file`.
//
// The class index caches absolute indices into `file.entries`, so this is only
// safe to call while `file.entries` is not being mutated. Callers that consume
// entries use matchEntryIn() instead.
std::optional<size_t>
matchEntryIndexFrom(const LayoutRestore::SLayoutFile &file,
                    const SClassIndex &index, const std::string &cls,
                    const std::string &title, int64_t now,
                    int64_t stalenessSeconds);

} // namespace LayoutRestore
