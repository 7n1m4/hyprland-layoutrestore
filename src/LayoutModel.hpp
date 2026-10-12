#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace LayoutRestore {

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

} // namespace LayoutRestore
