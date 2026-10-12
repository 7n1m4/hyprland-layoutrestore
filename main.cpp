#include <cstdlib>
#include <ctime>

#include <chrono>
#include <expected>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

#include <hyprutils/os/Process.hpp>
#include <hyprutils/string/String.hpp>

#include "globals.hpp"

// config values must outlive pluginInit
static SP<Config::Values::CStringValue> g_cfgFilePath = nullptr;
static SP<Config::Values::CBoolValue> g_cfgAutosave = nullptr;
static SP<Config::Values::CIntValue> g_cfgInterval = nullptr;
static SP<Config::Values::CIntValue> g_cfgStaleness = nullptr;
static SP<Config::Values::CBoolValue> g_cfgNoRestore = nullptr;
static SP<Config::Values::CBoolValue> g_cfgSaveOnExit = nullptr;

static SP<IPC::Socket1::SCommand> g_saveCmd = nullptr;
static SP<IPC::Socket1::SCommand> g_restoreCmd = nullptr;

// listeners must be retained: they disconnect when destroyed
static std::vector<CHyprSignalListener> g_listeners;

static Time::steady_tp g_lastSave = Time::steady_tp{};
static bool g_dirty = false;

// Deferred restore state, bounded in three ways so arming can never leak:
// g_armed covers a window that opens, g_armDeadline expires the arm, and
// g_pendingRelaunch counts the relaunched processes that have not matched yet.
static constexpr int ARM_DEADLINE_S = 30;
static bool g_armed = false;
static Time::steady_tp g_armDeadline = Time::steady_tp{};
static size_t g_pendingRelaunch = 0;

// True while a restore is in flight: the deferred matcher works on a snapshot
// of the file, so autosaving now would rewrite the file the next restore reads.
static bool g_restorePending = false;

// The file is loaded once when arming and consumed per window, so a concurrent
// autosave cannot swap entries out from under an in-flight restore.
static LayoutRestore::SLayoutFile g_deferredFile;
static bool g_deferredLoaded = false;

static std::string expandPath(const std::string &path) {
  if (path.size() < 2 || path[0] != '~')
    return path;

  if (path[1] != '/')
    return path;

  const char *const HOME = getenv("HOME");
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

// Reads /proc/<pid>/cmdline. Returns an empty string when it can't be captured
// (process reaped, permission denied, kernel thread, or a command with no
// argv[0]).
static std::string readProcCmdline(pid_t pid) {
  if (pid <= 0)
    return "";

  std::ifstream stream(std::format("/proc/{}/cmdline", pid), std::ios::binary);
  if (!stream)
    return "";

  std::string raw((std::istreambuf_iterator<char>(stream)),
                  std::istreambuf_iterator<char>());
  if (raw.empty())
    return "";

  return LayoutRestore::joinCmdline(raw);
}

// A window's own process is not something we bring back: relocating it would
// fight whatever manages the window's app. Covers XWayland (pid of the X
// server), the Wayland shm backend helpers and compositor children.
static bool isCompositorSideProcess(pid_t pid, pid_t selfPid) {
  return pid <= 0 || pid == selfPid;
}

static bool autosaveEnabled() {
  return g_cfgAutosave && g_cfgAutosave->value();
}

static bool noRestoreAll() { return g_cfgNoRestore && g_cfgNoRestore->value(); }

static bool saveOnExitEnabled() {
  return g_cfgSaveOnExit && g_cfgSaveOnExit->value();
}

static uint64_t autosaveIntervalMs() {
  if (!g_cfgInterval)
    return 5000;

  const Config::INTEGER MS = g_cfgInterval->value();
  return MS <= 0 ? 0 : sc<uint64_t>(MS);
}

static void markDirty() { g_dirty = true; }

//
// capture + write
//
static LayoutRestore::SLayoutFile captureCurrentWindows() {
  LayoutRestore::SLayoutFile file;

  file.version = LayoutRestore::SCHEMA_VERSION;
  file.timestamp = sc<int64_t>(time(nullptr));

  for (const auto &window : Desktop::windowState()->windows()) {
    if (!window)
      continue;

    if (!window->mapped())
      continue;

    const auto MONITOR = window->m_monitor.lock();
    if (!MONITOR)
      continue;

    const auto &META = window->metadata();

    // an unnamed window can never be matched back
    if (META.appID().empty())
      continue;

    LayoutRestore::SEntry entry;
    entry.cls = META.appID();
    entry.title = META.title();
    entry.workspace =
        window->m_workspace ? window->m_workspace->addressableName() : "";
    entry.monitor = MONITOR->m_name;
    entry.floating = window->isFloating();
    // Floating geometry is directly restorable; tiled geometry is recorded for
    // diagnostics/inspection because the tiling algorithm will override it.
    const auto BOX = window->getWindowMainSurfaceBox();
    entry.x = sc<int>(BOX.x);
    entry.y = sc<int>(BOX.y);
    entry.width = sc<int>(BOX.w);
    entry.height = sc<int>(BOX.h);
    entry.workspaceSpecial =
        window->m_workspace &&
        window->m_workspace->type() == Workspace::eWorkspaceType::SPECIAL;
    entry.timestamp = file.timestamp;

    // Only the client's own process is a relaunch candidate. Capturing a
    // compositor-side pid would have us spawn a compositor on restore.
    const pid_t PID = window->backend().pid();
    if (!isCompositorSideProcess(PID, getpid())) {
      entry.cmdline = readProcCmdline(PID);
      // The pid lets a restore claim a relaunched window by identity, before
      // any class/title match is attempted.
      entry.pid = sc<int64_t>(PID);
    }

    // opt-out: record the window, but never place it on restore.
    entry.noRestore = noRestoreAll();

    file.entries.emplace_back(std::move(entry));
  }

  // Number each workspace's windows in window order. Derived layouts
  // (scrolling, dwindle, master-stack) place windows from this, so replaying
  // the order reproduces the saved geometry without forcing it onto the tiling
  // algorithm.
  size_t cursor = 0;
  for (size_t i = 0; i < file.entries.size(); ++i) {
    if (i > 0 && file.entries[i].workspace == file.entries[i - 1].workspace)
      ++cursor;
    else
      cursor = 0;

    file.entries[i].order = sc<int>(cursor);
  }

  return file;
}

static std::expected<void, std::string> doSave() {
  const auto RET =
      LayoutRestore::writeFile(saveFilePath(), captureCurrentWindows());
  if (!RET)
    return RET;

  g_lastSave = Time::steadyNow();
  g_dirty = false;

  return {};
}

//
// deferred restore
//
static PHLWORKSPACE findOrCreateWorkspace(const std::string &address,
                                          const std::string &monitorName,
                                          bool special) {
  if (address.empty())
    return nullptr;

  const auto TARGET =
      State::Workspace::resolver()->getWorkspaceTargetFromString(
          special && !address.starts_with("special:")
              ? Workspace::specialWorkspaceAddressFromName(address)
              : address);

  if (!TARGET.valid())
    return nullptr;

  auto workspace = State::Workspace::state()->find(TARGET);
  if (workspace)
    return workspace;

  auto monitor = Desktop::focusState()->monitor();
  for (const auto &mon : State::monitorState()->monitors()) {
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

static void applyEntry(PHLWINDOW window, const LayoutRestore::SEntry &entry) {
  if (!window)
    return;

  // 1. workspace, on the saved monitor
  const auto WORKSPACE = findOrCreateWorkspace(entry.workspace, entry.monitor,
                                               entry.workspaceSpecial);
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
  if (entry.floating && entry.width > 0 && entry.height > 0 &&
      window->isFloating())
    g_layoutManager->setTargetGeom(
        CBox{sc<double>(entry.x), sc<double>(entry.y), sc<double>(entry.width),
             sc<double>(entry.height)},
        TARGET);

  // 4. consumed: LayoutModel matching is stateless by design (newest entry wins
  // on
  //    re-match), so there is nothing to mark here.
}

//
// relaunch: bring back windows that no longer exist
//
// Re-executes the saved command for each entry that has one. The compositor
// then opens the window and the deferred matcher (window.openLate) applies the
// saved state to it, so there is exactly one code path that positions windows.
static size_t relaunchSavedWindows(LayoutRestore::SLayoutFile &file) {
  // Group by workspace so the commands of one workspace are spawned in saved
  // order, which is what a derived layout turns back into geometry.
  std::vector<std::string> workspaces;
  for (const auto &entry : file.entries) {
    if (!LayoutRestore::isRelaunchable(entry))
      continue;

    if (std::ranges::find(workspaces, entry.workspace) == workspaces.end())
      workspaces.emplace_back(entry.workspace);
  }

  size_t spawned = 0;

  for (const auto &workspace : workspaces) {
    for (const auto index : LayoutRestore::relaunchOrder(file, workspace)) {
      const std::string &CMDLINE = file.entries[index].cmdline;
      if (CMDLINE.empty())
        continue;

      // CProcess takes argv, not a command line, and there is no shell: the
      // saved string has to be split or the whole thing becomes argv[0] and
      // every relaunch with arguments silently fails.
      std::vector<std::string> argv = LayoutRestore::splitCmdline(CMDLINE);
      std::erase_if(argv, [](const auto &arg) { return arg.empty(); });
      if (argv.empty())
        continue;

      // runAsync() detaches, so a restored window is not killed with the
      // compositor and the compositor does not block waiting for it.
      Hyprutils::OS::CProcess proc(
          argv[0], std::vector<std::string>(argv.begin() + 1, argv.end()));
      if (proc.runAsync())
        ++spawned;
    }
  }

  return spawned;
}

static LayoutRestore::SLayoutFile loadLayoutFile() {
  const auto READ = LayoutRestore::readFile(saveFilePath());
  if (!READ)
    return {};

  auto file = LayoutRestore::filterStale(*READ, sc<int64_t>(time(nullptr)),
                                         stalenessSeconds());

  // noRestore entries are recorded but never placed, so drop them here and
  // both restore paths inherit the exclusion.
  const auto KEEP = LayoutRestore::restorableEntries(file);
  if (KEEP.size() == file.entries.size())
    return file;

  LayoutRestore::SLayoutFile restorable;
  restorable.version = file.version;
  restorable.timestamp = file.timestamp;
  restorable.entries.reserve(KEEP.size());
  for (const auto index : KEEP)
    restorable.entries.emplace_back(std::move(file.entries[index]));

  return restorable;
}

// applies the saved entry for a window. Returns true if one was applied.
static bool restoreWindow(PHLWINDOW window, LayoutRestore::SLayoutFile &file) {
  if (!window)
    return false;

  if (!window->mapped())
    return false;

  const auto &META = window->metadata();

  if (META.appID().empty())
    return false;

  const auto NOW = sc<int64_t>(time(nullptr));

  // Identity first: a relaunched window owns its saved entry by pid, so a
  // same-class window that merely opened first cannot steal it.
  const pid_t PID = window->backend().pid();
  if (!isCompositorSideProcess(PID, getpid())) {
    const auto IDENTITY = LayoutRestore::matchEntryIndexByIdentity(
        file, sc<int64_t>(PID), META.appID(), NOW, stalenessSeconds());
    if (!IDENTITY.empty()) {
      // A pid can hold several saved entries (one window per client process
      // of a single-instance app), so take all of them.
      applyEntry(window, file.entries[IDENTITY.front()]);
      LayoutRestore::consumeEntries(file, IDENTITY);
      return true;
    }
  }

  const auto ENTRY = LayoutRestore::matchEntryIndex(
      file, META.appID(), META.title(), NOW, stalenessSeconds());
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

  // A restore in flight works on a snapshot of the file; rewriting it now
  // would change what the deferred matcher and the next restore read.
  if (g_restorePending)
    return;

  if (!g_dirty)
    return;

  if (Time::steadyNow() - g_lastSave <
      std::chrono::milliseconds{autosaveIntervalMs()})
    return;

  (void)doSave();
}

static void disarm() {
  g_armed = false;
  g_armDeadline = Time::steady_tp{};
  g_pendingRelaunch = 0;
  g_restorePending = false;
  g_deferredLoaded = false;
}

static void onExit() {
  // Only when something changed: rewriting an identical file on every exit
  // would clobber a good file's timestamp for nothing.
  if (!saveOnExitEnabled())
    return;

  if (!g_dirty)
    return;

  (void)doSave();
}

static void onWindowOpenLate(PHLWINDOW window) {
  if (!g_armed)
    return;

  const auto NOW = Time::steadyNow();
  if (NOW >= g_armDeadline) {
    // Bound: an arm that outlived its deadline disarms itself.
    disarm();
    return;
  }

  if (!g_deferredLoaded) {
    g_deferredFile = loadLayoutFile();
    g_deferredLoaded = true;
  }

  if (restoreWindow(window, g_deferredFile) && g_pendingRelaunch > 0)
    --g_pendingRelaunch; // one relaunched window accounted for

  // Done when every relaunched process has matched and no entry is left to
  // place: keeping the arm alive past that would let an unrelated window of
  // the same class be moved.
  if (g_pendingRelaunch == 0 && g_deferredFile.entries.empty())
    disarm();
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

static size_t restoreOpenWindows(LayoutRestore::SLayoutFile &file) {
  if (file.entries.empty())
    return 0;

  size_t applied = 0;

  // Window-outer so each saved entry is consumed by at most one window:
  // with two kitty windows and two saved kitty entries, both get their own
  // state.
  for (const auto &window : Desktop::windowState()->windows()) {
    if (!window)
      continue;

    if (!window->mapped())
      continue;

    const auto &META = window->metadata();
    if (META.appID().empty())
      continue;

    // Identity first: an already-matching process claims its own entries by
    // pid, so a window that merely opened first cannot steal them.
    const pid_t PID = window->backend().pid();
    if (!isCompositorSideProcess(PID, getpid())) {
      const auto IDENTITY = LayoutRestore::matchEntryIndexByIdentity(
          file, sc<int64_t>(PID), META.appID(), sc<int64_t>(time(nullptr)),
          stalenessSeconds());
      if (!IDENTITY.empty()) {
        applyEntry(window, file.entries[IDENTITY.front()]);
        // consume, so a later window of the same class takes the next entry
        LayoutRestore::consumeEntries(file, IDENTITY);
        ++applied;
        continue;
      }
    }

    // matchEntryIn returns a pointer, resolved against the live vector, so it
    // stays valid across the consuming erases in this loop. An index would go
    // stale after the first one.
    const auto *const PENTRY = LayoutRestore::matchEntryIn(
        file, META.appID(), META.title(), sc<int64_t>(time(nullptr)),
        stalenessSeconds());
    if (PENTRY) {
      applyEntry(window, *PENTRY);

      // consume, so a later window of the same class takes the next entry
      file.entries.erase(file.entries.begin() +
                         sc<int64_t>(PENTRY - file.entries.data()));
      ++applied;
    }
  }

  return applied;
}

static std::string handleRestore(const std::string &args) {
  // "layoutrestore"         -> apply to open windows
  // "layoutrestore arm"     -> also arm the deferred matcher for windows that
  //                            open later ("deferred"/"all" are aliases)
  // "layoutrestore disarm"  -> drop the armed state
  if (!args.empty() && args != "arm" && args != "deferred" && args != "all" &&
      args != "disarm")
    return std::format(
        "layoutrestore: unknown argument \"{}\" (expected \"arm\")", args);

  if (args == "disarm") {
    disarm();
    return "ok: disarmed";
  }

  const bool ARM = !args.empty();

  // Re-arm while already armed is idempotent: it reloads entries but must not
  // spawn a second wave of the same relaunchable windows.
  const bool FRESH_ARM = ARM && !g_armed;

  if (FRESH_ARM) {
    // Arming is bounded by three explicit bounds: it expires after
    // ARM_DEADLINE_S, auto-disarms once every relaunched process matched and
    // no entry is left, and can be dropped with "layoutrestore disarm".
    g_armed = true;
    g_armDeadline = Time::steadyNow() + std::chrono::seconds{ARM_DEADLINE_S};
  }

  if (ARM)
    g_deferredLoaded = false; // reload from disk, see the current file

  // A relaunch in flight is a gate on autosave, not a latch: it drops when the
  // deferred state ends. A fresh arm clears it, a re-arm keeps it.
  if (FRESH_ARM)
    g_restorePending = false;

  auto FILE = loadLayoutFile();
  auto APPLIED = restoreOpenWindows(FILE);

  // Bring back the windows that are gone. Only spawned on a fresh arm: an
  // immediate restore would have to race the compositor to place them, whereas
  // arming lets window.openLate do it deterministically as each one maps.
  size_t RELAUNCHED = 0;
  if (FRESH_ARM) {
    // relaunchSavedWindows needs the untouched file: entries are matched by
    // the deferred matcher, not consumed here.
    LayoutRestore::SLayoutFile RELAUNCH_FILE = loadLayoutFile();
    RELAUNCHED = relaunchSavedWindows(RELAUNCH_FILE);
    g_pendingRelaunch = RELAUNCHED;
    // Nothing to wait for means nothing is pending.
    g_restorePending = RELAUNCHED > 0;
  }

  if (APPLIED == 0 && RELAUNCHED == 0 && !ARM)
    return "layoutrestore: no open window matched a saved entry";

  if (ARM)
    return std::format("ok: {} open window(s) placed, {} relaunched", APPLIED,
                       RELAUNCHED);

  return "ok";
}

//
// plugin entry
//
APICALL EXPORT std::string pluginAPIVersion() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
  PHANDLE = handle;

  g_cfgFilePath = makeShared<Config::Values::CStringValue>(
      CFG_FILE.c_str(), "path to the layout file",
      Config::STRING{"~/.local/share/hyprland/layoutrestore.json"},
      Config::Values::SStringValueOptions{});
  g_cfgAutosave = makeShared<Config::Values::CBoolValue>(
      CFG_AUTOSAVE.c_str(), "save the layout automatically", Config::BOOL{true},
      Config::Values::SBoolValueOptions{});
  g_cfgInterval = makeShared<Config::Values::CIntValue>(
      CFG_INTERVAL.c_str(), "autosave interval in ms", Config::INTEGER{5000},
      Config::Values::SIntValueOptions{});
  g_cfgStaleness = makeShared<Config::Values::CIntValue>(
      CFG_STALENESS.c_str(),
      "drop saved entries older than this many seconds (0 = never)",
      Config::INTEGER{0}, Config::Values::SIntValueOptions{});
  g_cfgNoRestore = makeShared<Config::Values::CBoolValue>(
      CFG_NO_RESTORE.c_str(), "skip restoring this window", Config::BOOL{false},
      Config::Values::SBoolValueOptions{});
  g_cfgSaveOnExit = makeShared<Config::Values::CBoolValue>(
      CFG_SAVE_ON_EXIT.c_str(), "save the layout when the compositor exits",
      Config::BOOL{true}, Config::Values::SBoolValueOptions{});

  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgFilePath);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgAutosave);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgInterval);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgStaleness);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgNoRestore);
  HyprlandAPI::addConfigValueV2(PHANDLE, g_cfgSaveOnExit);

  g_saveCmd = HyprlandAPI::registerHyprCtlCommand(
      PHANDLE,
      IPC::Socket1::SCommand{
          .name = CMD_SAVE,
          .match = IPC::Socket1::COMMAND_MATCH_EXACT,
          .handler =
              [](const IPC::Socket1::SRequest &) -> IPC::Socket1::SResponse {
            return IPC::Socket1::SResponse{handleSave()};
          },
      });

  g_restoreCmd = HyprlandAPI::registerHyprCtlCommand(
      PHANDLE, IPC::Socket1::SCommand{
                   .name = CMD_RESTORE,
                   .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
                   .handler = [](const IPC::Socket1::SRequest &request)
                       -> IPC::Socket1::SResponse {
                     const std::string_view REST =
                         request.command.starts_with(CMD_RESTORE)
                             ? request.command.substr(CMD_RESTORE.size())
                             : std::string_view{};
                     return IPC::Socket1::SResponse{handleRestore(
                         std::string{Hyprutils::String::trim(REST)})};
                   },
               });

  g_dirty = false;
  g_lastSave = Time::steadyNow();

  auto &BUS = *Event::bus();

  g_listeners.emplace_back(BUS.m_events.tick.listen([] { onTick(); }));

  // Emitted before stopCompositor(), so the final layout is captured while
  // the windows are still mapped.
  g_listeners.emplace_back(BUS.m_events.exit.listen([] { onExit(); }));

  g_listeners.emplace_back(
      BUS.m_events.window.create.listen([](PHLWINDOW) { markDirty(); }));
  g_listeners.emplace_back(
      BUS.m_events.window.close.listen([](PHLWINDOW) { markDirty(); }));
  g_listeners.emplace_back(BUS.m_events.window.moveToWorkspace.listen(
      [](PHLWINDOW, PHLWORKSPACE) { markDirty(); }));
  g_listeners.emplace_back(
      BUS.m_events.window.floating.listen([](PHLWINDOW) { markDirty(); }));
  g_listeners.emplace_back(
      BUS.m_events.monitor.added.listen([](PHLMONITOR) { markDirty(); }));

  g_listeners.emplace_back(BUS.m_events.window.openLate.listen(
      [](PHLWINDOW window) { onWindowOpenLate(window); }));

  return PLUGIN_DESCRIPTION_INFO{
      "layoutrestore", "restore window layout by app-id/class", "omp", "0.1.0"};
}

APICALL EXPORT void pluginExit() {
  if (g_saveCmd)
    HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_saveCmd);

  if (g_restoreCmd)
    HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_restoreCmd);

  g_saveCmd = nullptr;
  g_restoreCmd = nullptr;

  g_listeners.clear();

  g_cfgFilePath = nullptr;
  g_cfgAutosave = nullptr;
  g_cfgInterval = nullptr;
  g_cfgStaleness = nullptr;
  g_cfgNoRestore = nullptr;
  g_cfgSaveOnExit = nullptr;
}
