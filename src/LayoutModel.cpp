#include "LayoutModel.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// JSON key names are the on-disk contract; `cls` maps to `class`.
template <> struct glz::meta<LayoutRestore::SEntry> {
  static constexpr auto value = std::make_tuple(
      &LayoutRestore::SEntry::cls, &LayoutRestore::SEntry::title,
      &LayoutRestore::SEntry::workspace, &LayoutRestore::SEntry::monitor,
      &LayoutRestore::SEntry::x, &LayoutRestore::SEntry::y,
      &LayoutRestore::SEntry::width, &LayoutRestore::SEntry::height,
      &LayoutRestore::SEntry::floating,
      &LayoutRestore::SEntry::workspaceSpecial,
      &LayoutRestore::SEntry::timestamp, &LayoutRestore::SEntry::cmdline,
      &LayoutRestore::SEntry::order, &LayoutRestore::SEntry::pid,
      &LayoutRestore::SEntry::noRestore);

  static constexpr auto keys = std::array<std::string_view, 15>{
      "class",     "title",   "workspace", "monitor",  "x",
      "y",         "width",   "height",    "floating", "workspaceSpecial",
      "timestamp", "cmdline", "order",     "pid",      "norestore"};
};

std::optional<std::string> LayoutRestore::serialize(const SLayoutFile &file) {
  const auto result = glz::write_json(file);
  if (!result)
    return glz::format_error(result.error());

  return *result;
}

std::expected<LayoutRestore::SLayoutFile, std::string>
LayoutRestore::deserialize(const std::string &json) {
  const auto result = glz::read_json<SLayoutFile>(json);
  if (!result)
    return std::unexpected(glz::format_error(result.error()));

  auto file = *result;
  // A missing or pre-versioned file reads as 0: normalize it to the current
  // schema so every downstream version comparison sees a real version.
  if (file.version <= 0)
    file.version = SCHEMA_VERSION;

  return file;
}

std::vector<std::string> LayoutRestore::splitCmdline(const std::string &cmd) {
  // Splits on single spaces, so consecutive spaces and a leading space produce
  // empty tokens that the caller filters. The command string is written by
  // joinCmdline as argv joined with single spaces, so this round-trips.
  std::vector<std::string> out;

  size_t start = 0;
  while (true) {
    const size_t SPACE = cmd.find(' ', start);
    if (SPACE == std::string::npos) {
      out.emplace_back(cmd.substr(start));
      break;
    }

    out.emplace_back(cmd.substr(start, SPACE - start));
    start = SPACE + 1;
  }

  return out;
}

std::string LayoutRestore::joinCmdline(const std::string &raw) {
  // /proc/<pid>/cmdline is argv with NUL separators and a trailing NUL.
  // Empty argv elements are preserved as-is: dropping one would shift the rest
  // of the command line and restore the wrong arguments.
  std::string out;
  out.reserve(raw.size());

  size_t i = 0;
  while (i < raw.size()) {
    size_t end = raw.find('\0', i);
    if (end == std::string::npos)
      end = raw.size();

    // argv[0] must exist: an empty one leaves nothing to re-exec. Later
    // argv elements are kept even when empty so arguments don't shift.
    if (i == 0 && i == end)
      return "";

    if (!out.empty())
      out += ' ';

    out.append(raw, i, end - i);
    i = end + 1;
  }

  return out;
}

bool LayoutRestore::isRelaunchable(const SEntry &entry) {
  return !entry.cmdline.empty();
}

std::vector<size_t> LayoutRestore::relaunchOrder(const SLayoutFile &file,
                                                 const std::string &workspace) {
  std::vector<size_t> out;

  for (size_t i = 0; i < file.entries.size(); ++i) {
    const auto &entry = file.entries[i];

    if (entry.workspace != workspace)
      continue;

    if (!isRelaunchable(entry))
      continue;

    out.emplace_back(i);
  }

  // Replay in saved insertion order. Unknown order (-1) goes last: we can't
  // place it relative to the others, so it is appended rather than moved to the
  // front. stable_sort keeps file order for equal orders.
  std::ranges::stable_sort(out, [&file](size_t a, size_t b) {
    const auto ORDER_A = file.entries[a].order, ORDER_B = file.entries[b].order;
    if (ORDER_A < 0)
      return false;
    if (ORDER_B < 0)
      return true;
    return ORDER_A < ORDER_B;
  });

  return out;
}

std::optional<size_t> LayoutRestore::matchEntryIndex(const SLayoutFile &file,
                                                     const std::string &cls,
                                                     const std::string &title,
                                                     int64_t now,
                                                     int64_t stalenessSeconds) {
  if (cls.empty())
    return std::nullopt;

  std::optional<size_t> best;

  for (size_t i = 0; i < file.entries.size(); ++i) {
    const auto &entry = file.entries[i];

    if (entry.cls != cls)
      continue;

    if (!entry.title.empty() && entry.title != title)
      continue;

    if (stalenessSeconds > 0 && now - entry.timestamp > stalenessSeconds)
      continue;

    if (best && entry.timestamp < file.entries[*best].timestamp)
      continue;

    best = i;
  }

  return best;
}

std::optional<LayoutRestore::SEntry>
LayoutRestore::matchEntry(const SLayoutFile &file, const std::string &cls,
                          const std::string &title, int64_t now,
                          int64_t stalenessSeconds) {
  const auto index = matchEntryIndex(file, cls, title, now, stalenessSeconds);
  if (!index)
    return std::nullopt;

  return file.entries[*index];
}

LayoutRestore::SLayoutFile
LayoutRestore::filterStale(const SLayoutFile &file, int64_t now,
                           int64_t stalenessSeconds) {
  if (stalenessSeconds <= 0)
    return file;

  SLayoutFile out;
  out.version = file.version;
  out.timestamp = file.timestamp;

  for (const auto &entry : file.entries) {
    if (now - entry.timestamp > stalenessSeconds)
      continue;

    out.entries.push_back(entry);
  }

  return out;
}

std::expected<void, std::string>
LayoutRestore::writeFile(const std::string &path, const SLayoutFile &file) {
  const auto json = serialize(file);
  if (!json)
    return std::unexpected(*json);

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    return std::unexpected("failed to open " + path + " for writing");

  stream << *json;
  stream.flush();
  if (!stream)
    return std::unexpected("failed to write " + path);

  return {};
}

std::expected<LayoutRestore::SLayoutFile, std::string>
LayoutRestore::readFile(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return std::unexpected("failed to open " + path);

  std::string json((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  return deserialize(json);
}

std::vector<size_t> LayoutRestore::matchEntryIndexByIdentity(
    const SLayoutFile &file, const int64_t pid, const std::string &cls,
    const int64_t now, const int64_t stalenessSeconds) {
  if (pid == 0)
    return {};

  std::vector<size_t> out;
  for (size_t i = 0; i < file.entries.size(); ++i) {
    const auto &entry = file.entries[i];

    if (entry.pid != pid)
      continue;

    if (entry.cls != cls)
      continue;

    if (stalenessSeconds > 0 && now - entry.timestamp > stalenessSeconds)
      continue;

    out.push_back(i);
  }

  return out;
}

void LayoutRestore::consumeEntries(SLayoutFile &file,
                                   const std::vector<size_t> &indices) {
  if (indices.empty())
    return;

  // Highest first: each erase shifts only indices above it, and those have
  // already been removed, so the remaining targets stay valid.
  std::vector<size_t> sorted = indices;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  for (size_t i = sorted.size(); i-- > 0;) {
    if (sorted[i] >= file.entries.size())
      continue;

    file.entries.erase(file.entries.begin() +
                       static_cast<std::ptrdiff_t>(sorted[i]));
  }
}

std::vector<size_t> LayoutRestore::restorableEntries(const SLayoutFile &file) {
  std::vector<size_t> out;
  for (size_t i = 0; i < file.entries.size(); ++i) {
    if (!file.entries[i].noRestore)
      out.push_back(i);
  }

  return out;
}

LayoutRestore::SClassIndex
LayoutRestore::buildClassIndex(const SLayoutFile &file) {
  SClassIndex index;
  for (size_t i = 0; i < file.entries.size(); ++i)
    index.byClass[file.entries[i].cls].push_back(i);

  return index;
}

const LayoutRestore::SEntry *
LayoutRestore::matchEntryIn(const SLayoutFile &file, const std::string &cls,
                            const std::string &title, const int64_t now,
                            const int64_t stalenessSeconds) {
  if (cls.empty())
    return nullptr;

  const LayoutRestore::SEntry *best = nullptr;

  for (const auto &entry : file.entries) {
    if (entry.cls != cls)
      continue;

    if (!entry.title.empty() && entry.title != title)
      continue;

    if (stalenessSeconds > 0 && now - entry.timestamp > stalenessSeconds)
      continue;

    if (best && entry.timestamp < best->timestamp)
      continue;

    best = &entry;
  }

  return best;
}

std::optional<size_t> LayoutRestore::matchEntryIndexFrom(
    const SLayoutFile &file, const SClassIndex &index, const std::string &cls,
    const std::string &title, const int64_t now,
    const int64_t stalenessSeconds) {
  if (cls.empty())
    return std::nullopt;

  const auto it = index.byClass.find(cls);
  if (it == index.byClass.end())
    return std::nullopt;

  std::optional<size_t> best;

  for (const size_t i : it->second) {
    const auto &entry = file.entries[i];

    if (!entry.title.empty() && entry.title != title)
      continue;

    if (stalenessSeconds > 0 && now - entry.timestamp > stalenessSeconds)
      continue;

    if (best && entry.timestamp < file.entries[*best].timestamp)
      continue;

    best = i;
  }

  return best;
}
