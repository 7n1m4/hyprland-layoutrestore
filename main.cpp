#include <cstdlib>
#include <ctime>

#include <chrono>
#include <string>
#include <vector>
#include <expected>

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/state/workspace/State.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/WorkspaceUtils.hpp>

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <hyprutils/string/String.hpp>

#include "globals.hpp"

// config values must outlive pluginInit
static SP<Config::Values::CStringValue> g_cfgFilePath  = nullptr;
static SP<Config::Values::CBoolValue>   g_cfgAutosave  = nullptr;
static SP<Config::Values::CIntValue>    g_cfgInterval  = nullptr;
static SP<Config::Values::CIntValue>    g_cfgStaleness = nullptr;

static SP<IPC::Socket1::SCommand>       g_saveCmd    = nullptr;
static SP<IPC::Socket1::SCommand>       g_restoreCmd = nullptr;

// listeners must be retained: they disconnect when destroyed
static std::vector<CHyprSignalListener> g_listeners;

static Time::steady_tp                  g_lastSave = Time::steady_tp{};
static bool                             g_dirty    = false;
static uint64_t                         g_deferred = 0; // 0 == deferred restore not armed

// Deferred restore state: the file is loaded once when arming and consumed per window,
// so a concurrent autosave cannot swap entries out from under an in-flight restore.
static LayoutRestore::SLayoutFile g_deferredFile;
static bool                       g_deferredLoaded = false;

static std::string                expandPath(const std::string& path) {
    if (path.size() < 2 || path[0] != '~')
        return path;

    if (path[1] != '/')
        return path;

    const char* const HOME = getenv("HOME");
    if (!HOME || !*HOME)
        return path;

    return std::string{HOME} + path.substr(1);
}

static std::string saveFilePath() {
    if (g_cfgFilePath) {
        const std::string PATH = expandPath(g_cfgFilePath->value());
        if (!PATH.empty())
            return PATH;
    }

    return expandPath("~/.local/share/hyprland/layoutrestore.json");
}

static int64_t stalenessSeconds() {
    if (!g_cfgStaleness)
        return 0;

    const Config::INTEGER SECONDS = g_cfgStaleness->value();
    return SECONDS <= 0 ? 0 : sc<int64_t>(SECONDS);
}

static bool autosaveEnabled() {
    return g_cfgAutosave && g_cfgAutosave->value();
}

static uint64_t autosaveIntervalMs() {
    if (!g_cfgInterval)
        return 5000;

    const Config::INTEGER MS = g_cfgInterval->value();
    return MS <= 0 ? 0 : sc<uint64_t>(MS);
}

static void markDirty() {
    g_dirty = true;
}

//
// capture + write
//
static LayoutRestore::SLayoutFile captureCurrentWindows() {
    LayoutRestore::SLayoutFile file;

    file.version   = 1;
    file.timestamp = sc<int64_t>(time(nullptr));

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window)
            continue;

        if (!window->mapped())
            continue;

        const auto MONITOR = window->m_monitor.lock();
        if (!MONITOR)
            continue;

        const auto& META = window->metadata();

        // an unnamed window can never be matched back
        if (META.appID().empty())
            continue;

        LayoutRestore::SEntry entry;
        entry.cls       = META.appID();
        entry.title     = META.title();
        entry.workspace = window->m_workspace ? window->m_workspace->addressableName() : "";
        entry.monitor   = MONITOR->m_name;
        entry.floating  = window->isFloating();
        // Floating geometry is directly restorable; tiled geometry is recorded for
        // diagnostics/inspection because the tiling algorithm will override it.
        const auto BOX         = window->getWindowMainSurfaceBox();
        entry.x                = sc<int>(BOX.x);
        entry.y                = sc<int>(BOX.y);
        entry.width            = sc<int>(BOX.w);
        entry.height           = sc<int>(BOX.h);
        entry.workspaceSpecial = window->m_workspace && window->m_workspace->type() == Workspace::eWorkspaceType::SPECIAL;
        entry.timestamp        = file.timestamp;

        file.entries.emplace_back(std::move(entry));
    }

    return file;
}

static std::expected<void, std::string> doSave() {
    const auto RET = LayoutRestore::writeFile(saveFilePath(), captureCurrentWindows());
    if (!RET)
        return RET;

    g_lastSave = Time::steadyNow();
    g_dirty    = false;

    return {};
}

//
// deferred restore
//
static PHLWORKSPACE findOrCreateWorkspace(const std::string& address, const std::string& monitorName, bool special) {
    if (address.empty())
        return nullptr;

    const auto TARGET =
        State::Workspace::resolver()->getWorkspaceTargetFromString(special && !address.starts_with("special:") ? Workspace::specialWorkspaceAddressFromName(address) : address);

    if (!TARGET.valid())
        return nullptr;

    auto workspace = State::Workspace::state()->find(TARGET);
    if (workspace)
        return workspace;

    auto monitor = Desktop::focusState()->monitor();
    for (const auto& mon : State::monitorState()->monitors()) {
        if (!mon)
            continue;

        if (!monitorName.empty() && mon->m_name != monitorName)
            continue;

        monitor = mon;
        break;
    }

    if (!monitor)
        return nullptr;

    return State::Workspace::state()->create(TARGET, monitor, false);
}

static void applyEntry(PHLWINDOW window, const LayoutRestore::SEntry& entry) {
    if (!window)
        return;

    // 1. workspace, on the saved monitor
    const auto WORKSPACE = findOrCreateWorkspace(entry.workspace, entry.monitor, entry.workspaceSpecial);
    if (WORKSPACE && WORKSPACE != window->m_workspace)
        window->moveToWorkspace(WORKSPACE);

    if (!g_layoutManager)
        return;

    const auto TARGET = window->layoutTarget();
    if (!TARGET)
        return;

    if (entry.floating) {
        // 2. floating
        if (g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();

        g_layoutManager->changeFloatingMode(TARGET);
    }

    // 3. geometry, floating only
    if (entry.floating && entry.width > 0 && entry.height > 0 && window->isFloating())
        g_layoutManager->setTargetGeom(CBox{sc<double>(entry.x), sc<double>(entry.y), sc<double>(entry.width), sc<double>(entry.height)}, TARGET);

    // 4. consumed: LayoutModel matching is stateless by design (newest entry wins on
    //    re-match), so there is nothing to mark here.
}

static LayoutRestore::SLayoutFile loadLayoutFile() {
    const auto READ = LayoutRestore::readFile(saveFilePath());
    if (!READ)
        return {};

    return LayoutRestore::filterStale(*READ, sc<int64_t>(time(nullptr)), stalenessSeconds());
}

// applies the saved entry for a window. Returns true if one was applied.
static bool restoreWindow(PHLWINDOW window, LayoutRestore::SLayoutFile& file) {
    if (!window)
        return false;

    if (!window->mapped())
        return false;

    const auto& META = window->metadata();

    if (META.appID().empty())
        return false;

    const auto NOW = sc<int64_t>(time(nullptr));

    const auto ENTRY = LayoutRestore::matchEntryIndex(file, META.appID(), META.title(), NOW, stalenessSeconds());
    if (!ENTRY)
        return false;

    applyEntry(window, file.entries[*ENTRY]);

    // consume: the same entry must not be handed to the next window of that class
    file.entries.erase(file.entries.begin() + sc<int64_t>(*ENTRY));

    return true;
}

static void onTick() {
    if (!autosaveEnabled())
        return;

    if (!g_dirty)
        return;

    if (Time::steadyNow() - g_lastSave < std::chrono::milliseconds{autosaveIntervalMs()})
        return;

    (void)doSave();
}

static void onWindowOpenLate(PHLWINDOW window) {
    if (!g_deferred)
        return;

    if (!g_deferredLoaded) {
        g_deferredFile   = loadLayoutFile();
        g_deferredLoaded = true;
    }

    if (restoreWindow(window, g_deferredFile))
        --g_deferred; // one pending window per armed entry
}

//
// hyprctl commands
//
static std::string handleSave() {
    const auto RET = doSave();
    if (!RET)
        return std::string{"layoutsave: "} + RET.error();

    return "ok";
}

static size_t restoreOpenWindows(LayoutRestore::SLayoutFile& file) {
    if (file.entries.empty())
        return 0;

    size_t applied = 0;

    // Window-outer so each saved entry is consumed by at most one window:
    // with two kitty windows and two saved kitty entries, both get their own state.
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window)
            continue;

        if (!window->mapped())
            continue;

        const auto& META = window->metadata();
        if (META.appID().empty())
            continue;

        const auto INDEX = LayoutRestore::matchEntryIndex(file, META.appID(), META.title(), sc<int64_t>(time(nullptr)), stalenessSeconds());
        if (!INDEX)
            continue;

        applyEntry(window, file.entries[*INDEX]);

        // consume, so a later window of the same class takes the next entry
        file.entries.erase(file.entries.begin() + sc<int64_t>(*INDEX));
        ++applied;
    }

    return applied;
}

static std::string handleRestore(const std::string& args) {
    // "layoutrestore"        -> apply to open windows
    // "layoutrestore arm"    -> also arm the deferred matcher for windows that open later
    if (!args.empty() && args != "arm" && args != "deferred" && args != "all")
        return std::format("layoutrestore: unknown argument \"{}\" (expected \"arm\")", args);

    const bool ARM = !args.empty();

    if (ARM) {
        // re-arm: discard any stale pending state and reload from disk
        ++g_deferred;
        g_deferredLoaded = false;
    }

    auto       FILE    = loadLayoutFile();
    const auto APPLIED = restoreOpenWindows(FILE);

    if (APPLIED == 0 && !ARM)
        return "layoutrestore: no open window matched a saved entry";

    return "ok";
}

//
// plugin entry
//
APICALL EXPORT std::string pluginAPIVersion() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
    PHANDLE = handle;

    g_cfgFilePath  = makeShared<Config::Values::CStringValue>(CFG_FILE.c_str(), "path to the layout file", Config::STRING{"~/.local/share/hyprland/layoutrestore.json"},
                                                              Config::Values::SStringValueOptions{});
    g_cfgAutosave  = makeShared<Config::Values::CBoolValue>(CFG_AUTOSAVE.c_str(), "save the layout automatically", Config::BOOL{true}, Config::Values::SBoolValueOptions{});
    g_cfgInterval  = makeShared<Config::Values::CIntValue>(CFG_INTERVAL.c_str(), "autosave interval in ms", Config::INTEGER{5000}, Config::Values::SIntValueOptions{});
    g_cfgStaleness = makeShared<Config::Values::CIntValue>(CFG_STALENESS.c_str(), "drop saved entries older than this many seconds (0 = never)", Config::INTEGER{0},
                                                           Config::Values::SIntValueOptions{});

    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgFilePath);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAutosave);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgInterval);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgStaleness);

    g_saveCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE,
                                                    IPC::Socket1::SCommand{
                                                        .name    = CMD_SAVE,
                                                        .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                                        .handler = [](const IPC::Socket1::SRequest&) -> IPC::Socket1::SResponse { return IPC::Socket1::SResponse{handleSave()}; },
                                                    });

    g_restoreCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE,
                                                       IPC::Socket1::SCommand{
                                                           .name    = CMD_RESTORE,
                                                           .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                           .handler = [](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse {
                                                               const std::string_view REST =
                                                                   request.command.starts_with(CMD_RESTORE) ? request.command.substr(CMD_RESTORE.size()) : std::string_view{};
                                                               return IPC::Socket1::SResponse{handleRestore(std::string{Hyprutils::String::trim(REST)})};
                                                           },
                                                       });

    g_dirty    = false;
    g_lastSave = Time::steadyNow();

    auto& BUS = *Event::bus();

    g_listeners.emplace_back(BUS.m_events.tick.listen([] { onTick(); }));

    g_listeners.emplace_back(BUS.m_events.window.create.listen([](PHLWINDOW) { markDirty(); }));
    g_listeners.emplace_back(BUS.m_events.window.close.listen([](PHLWINDOW) { markDirty(); }));
    g_listeners.emplace_back(BUS.m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { markDirty(); }));
    g_listeners.emplace_back(BUS.m_events.window.floating.listen([](PHLWINDOW) { markDirty(); }));
    g_listeners.emplace_back(BUS.m_events.monitor.added.listen([](PHLMONITOR) { markDirty(); }));

    g_listeners.emplace_back(BUS.m_events.window.openLate.listen([](PHLWINDOW window) { onWindowOpenLate(window); }));

    return PLUGIN_DESCRIPTION_INFO{"layoutrestore", "restore window layout by app-id/class", "omp", "0.1.0"};
}

APICALL EXPORT void pluginExit() {
    if (g_saveCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_saveCmd);

    if (g_restoreCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_restoreCmd);

    g_saveCmd    = nullptr;
    g_restoreCmd = nullptr;

    g_listeners.clear();

    g_cfgFilePath  = nullptr;
    g_cfgAutosave  = nullptr;
    g_cfgInterval  = nullptr;
    g_cfgStaleness = nullptr;
}
