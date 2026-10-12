#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace LayoutRestore {

    struct SEntry {
        std::string cls              = "";
        std::string title            = "";
        std::string workspace        = "";
        std::string monitor          = "";
        int         x                = 0;
        int         y                = 0;
        int         width            = 0;
        int         height           = 0;
        bool        floating         = false;
        bool        workspaceSpecial = false;
        int64_t     timestamp        = 0;
    };

    struct SLayoutFile {
        int                 version   = 1;
        int64_t             timestamp = 0;
        std::vector<SEntry> entries;
    };

    std::optional<std::string>                     serialize(const SLayoutFile& file);
    static std::expected<SLayoutFile, std::string> deserialize(const std::string& json);
    // Index of the best matching entry, so callers can consume it from the vector.
    // Returns nullopt when nothing matches.
    std::optional<size_t>                   matchEntryIndex(const SLayoutFile& file, const std::string& cls, const std::string& title, int64_t now, int64_t stalenessSeconds);
    std::optional<SEntry>                   matchEntry(const SLayoutFile& file, const std::string& cls, const std::string& title, int64_t now, int64_t stalenessSeconds);
    SLayoutFile                             filterStale(const SLayoutFile& file, int64_t now, int64_t stalenessSeconds);
    std::expected<void, std::string>        writeFile(const std::string& path, const SLayoutFile& file);
    std::expected<SLayoutFile, std::string> readFile(const std::string& path);

}
