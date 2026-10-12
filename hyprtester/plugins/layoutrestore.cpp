#include "tests.hpp"

#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Hyprutils::Memory::sc;
using Hyprutils::Utils::CScopeGuard;

// Absolute, build-independent: hyprtester is a local test tool and the plugin is built alongside the tree.
static constexpr const char* LAYOUTRESTORE_SO   = "/home/vi/hyprland/plugins/layoutrestore/layoutrestore.so";
static constexpr const char* LAYOUTRESTORE_FILE = "/tmp/hyprtester-layoutrestore.json";

// A monitor of our own, so the test asserts a cross-monitor restore without touching the shared HEADLESS-* ones.
static constexpr const char* TEST_MONITOR = "HYPRTEST-LAYOUTRESTORE";

static constexpr const char* TEST_CLASS     = "lr_restore";
static constexpr const char* TEST_WORKSPACE = "name:lr_layout_saved";
// What /clients reports for `workspace:`: the addressable name (no `name:` prefix).
static constexpr const char* TEST_WORKSPACE_NAME = "lr_layout_saved";

// Distinctive, deterministic floats that stay well inside a 1920x1080 monitor at 0x0.
static constexpr int TEST_POS_X  = 140;
static constexpr int TEST_POS_Y  = 180;
static constexpr int TEST_SIZE_W = 600;
static constexpr int TEST_SIZE_H = 400;

// Floating geometry is restored through the layout algorithm, so allow a couple of px of slack.
static constexpr int GEOM_DELTA = 2;

/// Reads a file into a string. Empty when it can't be opened.
static std::string readFileContents(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return "";

    return std::string{(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
}

/// Parses an "X,Y" attribute as returned by Tests::getAttribute for "at" and "size".
/// Sets `ok` to false when the value is malformed.
static std::pair<int, int> parseXY(const std::string& value, bool& ok) {
    ok               = false;
    const auto COMMA = value.find(',');
    if (COMMA == std::string::npos)
        return {0, 0};

    try {
        const int x = std::stoi(value.substr(0, COMMA));
        const int y = std::stoi(value.substr(COMMA + 1));
        ok          = true;
        return {x, y};
    } catch (...) { return {0, 0}; }
}

static bool near(int actual, int expected, int delta) {
    return std::abs(actual - expected) <= delta;
}

/// Waits (up to ~5s) until the single open window's saved workspace, monitor, floating state and
/// geometry are applied. The explicit path applies them from a socket command, the deferred path
/// from `window.openLate`, i.e. after the window is already mapped, so poll rather than assert once.
static bool waitForRestoredWindow(int expectedMonitorId) {
    for (size_t i = 0; i < 50; ++i) {
        const auto ACTIVE = getFromSocket("/activewindow");
        if (!ACTIVE.empty() && Tests::getAttribute(ACTIVE, "class") == TEST_CLASS) {
            bool okAt = false, okSize = false;
            auto at   = parseXY(Tests::getAttribute(ACTIVE, "at"), okAt);
            auto size = parseXY(Tests::getAttribute(ACTIVE, "size"), okSize);

            if (okAt && okSize && Tests::getAttribute(ACTIVE, "floating") == "1" && Tests::getAttribute(ACTIVE, "workspace").starts_with(TEST_WORKSPACE_NAME) &&
                Tests::getAttribute(ACTIVE, "monitor") == std::to_string(expectedMonitorId) && near(at.first, TEST_POS_X, GEOM_DELTA) && near(at.second, TEST_POS_Y, GEOM_DELTA) &&
                near(size.first, TEST_SIZE_W, GEOM_DELTA) && near(size.second, TEST_SIZE_H, GEOM_DELTA))
                return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Tests::sync();
    }

    return false;
}

/// Resolves a monitor's ID (`monitor:` field of /clients) from `/monitors all`. A freshly created
/// output may not be registered yet, so poll instead of assuming it is available immediately.
static std::string monitorIdOf(const std::string& monitorName) {
    for (size_t i = 0; i < 30; ++i) {
        const auto MONITORS = getFromSocket("/monitors all");
        const auto PREFIX   = std::string{monitorName} + " (ID ";
        const auto POS      = MONITORS.find(PREFIX);
        if (POS != std::string::npos) {
            const auto ID_START = POS + PREFIX.size();
            const auto ID_END   = MONITORS.find(')', ID_START);
            if (ID_END != std::string::npos) {
                const auto ID = MONITORS.substr(ID_START, ID_END - ID_START);
                if (!ID.empty())
                    return ID;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return "";
}

TEST_CASE(pluginLayoutRestoreSaveAndRestore) {
    // --- 0. Isolation: point the plugin at a temp file so the real user layout is never touched.
    std::error_code ec;
    std::filesystem::remove(LAYOUTRESTORE_FILE, ec);

    // --- 1. Load the freshly built plugin.
    const auto LOADED = getFromSocket(std::format("/plugin load {}", LAYOUTRESTORE_SO));
    if (LOADED != "ok")
        FAIL_TEST("Failed to load {}: got \"{}\"", LAYOUTRESTORE_SO, LOADED);
    LOG_OK("Loaded {}", LAYOUTRESTORE_SO);

    // --- 2. Configure the plugin to a unique temp file.
    // Plugin config values are registered in the lua config manager under ':'->'.' (see
    // CConfigManager::registerPluginValue + luaConfigValueName), so hl.config can set them at runtime
    // exactly like any other value; that is also the only socket-exposed setter.
    const auto CONFIGURED = getFromSocket(std::format("/eval hl.config({{ plugin = {{ layoutrestore = {{ file = '{}' }} }}}})", LAYOUTRESTORE_FILE));
    if (CONFIGURED != "ok")
        FAIL_TEST("Failed to set plugin:layoutrestore:file to {}: got \"{}\"", LAYOUTRESTORE_FILE, CONFIGURED);
    LOG_OK("plugin:layoutrestore:file = {}", LAYOUTRESTORE_FILE);

    // --- 3. A dedicated monitor for cross-monitor placement, removed again at the end.
    getFromSocket(std::format("/output remove {}", TEST_MONITOR));
    OK(getFromSocket(std::format("/eval hl.monitor({{ output = 'HEADLESS-2', mode = '1920x1080@60', position = '0x0', scale = '1' }})")));
    OK(getFromSocket(std::format("/eval hl.monitor({{ output = '{}', mode = '1920x1080@60', position = '1920x0', scale = '1' }})", TEST_MONITOR)));
    const auto CREATED = getFromSocket(std::format("/output create headless {}", TEST_MONITOR));
    if (CREATED != "ok" && CREATED != "Name already taken")
        FAIL_TEST("Failed to create monitor {}: got \"{}\"", TEST_MONITOR, CREATED);

    CScopeGuard guard = {[&]() {
        getFromSocket(std::format("/output remove {}", TEST_MONITOR));
        std::error_code ignore;
        std::filesystem::remove(LAYOUTRESTORE_FILE, ignore);
    }};

    const auto  MONITOR_ID = monitorIdOf(TEST_MONITOR);
    if (MONITOR_ID.empty())
        FAIL_TEST("Monitor {} was not created", TEST_MONITOR);
    ASSERT(Tests::windowCount(), 0);

    // --- 4. Spawn the window and give it a distinctive, deterministic state.
    OK(getFromSocket("/dispatch hl.dsp.focus({ monitor = 'HEADLESS-2' })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })"));
    SPAWN_KITTY(TEST_CLASS);
    Tests::waitUntilWindowsN(1);
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'class:{}' }})", TEST_CLASS)));

    OK(getFromSocket(std::format("/dispatch hl.dsp.window.float({{ action = 'set', window = 'class:{}' }})", TEST_CLASS)));
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.resize({{ x = {}, y = {}, window = 'class:{}' }})", TEST_SIZE_W, TEST_SIZE_H, TEST_CLASS)));
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ x = {}, y = {}, window = 'class:{}' }})", TEST_POS_X, TEST_POS_Y, TEST_CLASS)));

    // Move the workspace (and thus the window) to the second monitor: the plugin records the monitor
    // the window physically lives on, and restore must put it back there.
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ workspace = '{}', window = 'class:{}' }})", TEST_WORKSPACE, TEST_CLASS)));
    OK(getFromSocket(std::format("/dispatch hl.dsp.workspace.move({{ monitor = '{}', workspace = '{}' }})", TEST_MONITOR, TEST_WORKSPACE)));
    Tests::sync();

    // Sanity: the state we're about to save is really the state we asked for.
    {
        const auto ACTIVE = getFromSocket("/activewindow");
        bool       okAt   = false;
        bool       okSize = false;
        const auto at     = parseXY(Tests::getAttribute(ACTIVE, "at"), okAt);
        const auto size   = parseXY(Tests::getAttribute(ACTIVE, "size"), okSize);
        ASSERT(Tests::getAttribute(ACTIVE, "class"), TEST_CLASS);
        ASSERT(Tests::getAttribute(ACTIVE, "monitor"), MONITOR_ID);
        ASSERT(Tests::getAttribute(ACTIVE, "workspace").starts_with(TEST_WORKSPACE_NAME), true);
        ASSERT(Tests::getAttribute(ACTIVE, "floating"), "1");
        if (!okAt || !okSize)
            FAIL_TEST("Malformed geometry: at=\"{}\" size=\"{}\"", Tests::getAttribute(ACTIVE, "at"), Tests::getAttribute(ACTIVE, "size"));
        if (!near(at.first, TEST_POS_X, GEOM_DELTA) || !near(at.second, TEST_POS_Y, GEOM_DELTA))
            FAIL_TEST("Pre-save position was {},{} but expected {},{}", at.first, at.second, TEST_POS_X, TEST_POS_Y);
        if (!near(size.first, TEST_SIZE_W, GEOM_DELTA) || !near(size.second, TEST_SIZE_H, GEOM_DELTA))
            FAIL_TEST("Pre-save size was {},{} but expected {},{}", size.first, size.second, TEST_SIZE_W, TEST_SIZE_H);
        LOG_OK("Window state before save: monitor {} ws lr_layout_saved floating 1 at {},{} size {},{}", MONITOR_ID, at.first, at.second, size.first, size.second);
    }

    // --- 5. Save the layout.
    OK(getFromSocket("/layoutsave"));

    // --- 6. Assert the file on disk contains the saved state.
    const auto JSON = readFileContents(LAYOUTRESTORE_FILE);
    if (JSON.empty())
        FAIL_TEST("{} is empty or missing after layoutsave", LAYOUTRESTORE_FILE);

    // glaze emits compact JSON with no space after ':' — assert exactly that form.
    ASSERT_CONTAINS(JSON, std::format("\"class\":\"{}\"", TEST_CLASS));
    ASSERT_CONTAINS(JSON, std::format("\"workspace\":\"{}\"", TEST_WORKSPACE_NAME));
    ASSERT_CONTAINS(JSON, std::format("\"monitor\":\"{}\"", TEST_MONITOR));
    ASSERT_CONTAINS(JSON, "\"floating\":true");
    ASSERT_CONTAINS(JSON, std::format("\"x\":{}", TEST_POS_X));
    ASSERT_CONTAINS(JSON, std::format("\"y\":{}", TEST_POS_Y));
    ASSERT_CONTAINS(JSON, std::format("\"width\":{}", TEST_SIZE_W));
    ASSERT_CONTAINS(JSON, std::format("\"height\":{}", TEST_SIZE_H));
    LOG_OK("Saved layout file contains class {}, workspace {}, monitor {}, floating true and geometry {},{} -> {},{}", TEST_CLASS, TEST_WORKSPACE_NAME, TEST_MONITOR, TEST_POS_X,
           TEST_POS_Y, TEST_SIZE_W, TEST_SIZE_H);

    // --- 7. Kill the window, confirm it's gone, then respawn the same class (fresh state).
    if (!Tests::killAllWindows())
        FAIL_TEST("Failed to kill all windows before restore");
    ASSERT(Tests::windowCount(), 0);

    SPAWN_KITTY(TEST_CLASS);
    Tests::waitUntilWindowsN(1);
    Tests::sync();

    // The fresh window must NOT already be in the saved state, otherwise the restore below proves nothing.
    {
        const auto ACTIVE = getFromSocket("/activewindow");
        ASSERT(Tests::getAttribute(ACTIVE, "class"), TEST_CLASS);
        ASSERT(Tests::getAttribute(ACTIVE, "workspace").starts_with(TEST_WORKSPACE_NAME), false);
        ASSERT_NOT(Tests::getAttribute(ACTIVE, "monitor"), MONITOR_ID);
    }

    // --- 8. Restore and assert the window came back exactly where it was saved.
    OK(getFromSocket("/layoutrestore"));
    OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'class:{}' }})", TEST_CLASS)));
    Tests::sync();

    if (!waitForRestoredWindow(std::stoi(MONITOR_ID)))
        FAIL_TEST("Explicit layoutrestore did not restore the window: expected class {} floating, workspace {}, monitor {}, "
                  "at {},{} size {},{}; /activewindow was:\n{}",
                  TEST_CLASS, TEST_WORKSPACE_NAME, MONITOR_ID, TEST_POS_X, TEST_POS_Y, TEST_SIZE_W, TEST_SIZE_H, getFromSocket("/activewindow"));
    LOG_OK("Explicit layoutrestore restored workspace {}, monitor {}, floating state 1 and geometry {},{} -> {},{}", TEST_WORKSPACE, MONITOR_ID, TEST_POS_X, TEST_POS_Y,
           TEST_SIZE_W, TEST_SIZE_H);

    // --- 9. Deferred path: arm, respawn without calling restore, expect it to be restored on open.
    // Re-save so the file holds an entry for the (still open) window, then kill it.
    OK(getFromSocket("/layoutsave"));
    if (!Tests::killAllWindows())
        FAIL_TEST("Failed to kill all windows before the deferred restore");
    ASSERT(Tests::windowCount(), 0);

    OK(getFromSocket("/layoutrestore arm"));

    SPAWN_KITTY(TEST_CLASS);
    Tests::waitUntilWindowsN(1);
    Tests::sync();

    if (!waitForRestoredWindow(std::stoi(MONITOR_ID)))
        FAIL_TEST("Deferred (armed) restore did not restore the newly opened window: expected class {} floating, workspace {}, "
                  "monitor {}, at {},{} size {},{}; /activewindow was:\n{}",
                  TEST_CLASS, TEST_WORKSPACE_NAME, MONITOR_ID, TEST_POS_X, TEST_POS_Y, TEST_SIZE_W, TEST_SIZE_H, getFromSocket("/activewindow"));
    LOG_OK("Deferred layoutrestore restored the newly opened window on monitor {} with geometry {},{} -> {},{}", MONITOR_ID, TEST_POS_X, TEST_POS_Y, TEST_SIZE_W, TEST_SIZE_H);

    // --- 10. Cleanup.
    if (!Tests::killAllWindows())
        FAIL_TEST("Failed to kill all windows at the end of the test");
    ASSERT(Tests::windowCount(), 0);
    std::filesystem::remove(LAYOUTRESTORE_FILE, ec);
}
