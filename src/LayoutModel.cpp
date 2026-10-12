#include "LayoutModel.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// JSON key names are the on-disk contract; `cls` maps to `class`.
template <>
struct glz::meta<LayoutRestore::SEntry> {
    static constexpr auto value =
        std::make_tuple(&LayoutRestore::SEntry::cls, &LayoutRestore::SEntry::title, &LayoutRestore::SEntry::workspace, &LayoutRestore::SEntry::monitor, &LayoutRestore::SEntry::x,
                        &LayoutRestore::SEntry::y, &LayoutRestore::SEntry::width, &LayoutRestore::SEntry::height, &LayoutRestore::SEntry::floating,
                        &LayoutRestore::SEntry::workspaceSpecial, &LayoutRestore::SEntry::timestamp, &LayoutRestore::SEntry::cmdline, &LayoutRestore::SEntry::order);

    static constexpr auto keys =
        std::array<std::string_view, 13>{"class", "title", "workspace", "monitor", "x", "y", "width", "height", "floating", "workspaceSpecial", "timestamp", "cmdline", "order"};
};

std::optional<std::string> LayoutRestore::serialize(const SLayoutFile& file) {
    const auto result = glz::write_json(file);
    if (!result)
        return glz::format_error(result.error());

    return *result;
}

std::expected<LayoutRestore::SLayoutFile, std::string> LayoutRestore::deserialize(const std::string& json) {
    const auto result = glz::read_json<SLayoutFile>(json);
    if (!result)
        return std::unexpected(glz::format_error(result.error()));

    return *result;
}

std::string LayoutRestore::joinCmdline(const std::string& raw) {
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

bool LayoutRestore::isRelaunchable(const SEntry& entry) {
    return !entry.cmdline.empty();
}

std::vector<size_t> LayoutRestore::relaunchOrder(const SLayoutFile& file, const std::string& workspace) {
    std::vector<size_t> out;

    for (size_t i = 0; i < file.entries.size(); ++i) {
        const auto& entry = file.entries[i];

        if (entry.workspace != workspace)
            continue;

        if (!isRelaunchable(entry))
            continue;

        out.emplace_back(i);
    }

    // Replay in saved insertion order. Unknown order (-1) goes last: we can't place
    // it relative to the others, so it is appended rather than moved to the front.
    // stable_sort keeps file order for equal orders.
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

std::optional<size_t> LayoutRestore::matchEntryIndex(const SLayoutFile& file, const std::string& cls, const std::string& title, int64_t now, int64_t stalenessSeconds) {
    if (cls.empty())
        return std::nullopt;

    std::optional<size_t> best;

    for (size_t i = 0; i < file.entries.size(); ++i) {
        const auto& entry = file.entries[i];

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

std::optional<LayoutRestore::SEntry> LayoutRestore::matchEntry(const SLayoutFile& file, const std::string& cls, const std::string& title, int64_t now, int64_t stalenessSeconds) {
    const auto index = matchEntryIndex(file, cls, title, now, stalenessSeconds);
    if (!index)
        return std::nullopt;

    return file.entries[*index];
}

LayoutRestore::SLayoutFile LayoutRestore::filterStale(const SLayoutFile& file, int64_t now, int64_t stalenessSeconds) {
    if (stalenessSeconds <= 0)
        return file;

    SLayoutFile out;
    out.version   = file.version;
    out.timestamp = file.timestamp;

    for (const auto& entry : file.entries) {
        if (now - entry.timestamp > stalenessSeconds)
            continue;

        out.entries.push_back(entry);
    }

    return out;
}

std::expected<void, std::string> LayoutRestore::writeFile(const std::string& path, const SLayoutFile& file) {
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

std::expected<LayoutRestore::SLayoutFile, std::string> LayoutRestore::readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::unexpected("failed to open " + path);

    std::string json((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return deserialize(json);
}
