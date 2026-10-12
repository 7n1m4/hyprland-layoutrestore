#pragma once

// The plugin ABI: APICALL/EXPORT/HANDLE, PLUGIN_DESCRIPTION_INFO, HyprlandAPI.
#include <hyprland/src/plugins/PluginAPI.hpp>

// Serialization + matching (no hyprland headers).
#include "src/LayoutModel.hpp"

inline HANDLE PHANDLE = nullptr;

// Config value names: all must start with "plugin:".
inline const std::string CFG_FILE      = "plugin:layoutrestore:file";
inline const std::string CFG_AUTOSAVE  = "plugin:layoutrestore:autosave";
inline const std::string CFG_INTERVAL  = "plugin:layoutrestore:autosave_interval";
inline const std::string CFG_STALENESS = "plugin:layoutrestore:staleness_seconds";

// hyprctl command names.
inline const std::string CMD_SAVE    = "layoutsave";
inline const std::string CMD_RESTORE = "layoutrestore";
