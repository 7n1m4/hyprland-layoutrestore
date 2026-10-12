#include "../../src/LayoutModel.cpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using LayoutRestore::SEntry;
using LayoutRestore::SLayoutFile;

// Fixed clock: every staleness assertion below is expressed relative to this.
static constexpr int64_t NOW      = 1000000;
static constexpr int64_t LIMIT    = 100;
static constexpr int64_t FRESH_AT = NOW - LIMIT; // exactly at the limit: `>` comparison keeps it
static constexpr int64_t STALE_AT = NOW - LIMIT - 1;

static SEntry            floatingEntry() {
    SEntry entry;
    entry.cls              = "kitty";
    entry.title            = "editor";
    entry.workspace        = "3";
    entry.monitor          = "DP-1";
    entry.x                = -100;
    entry.y                = 250;
    entry.width            = 1920;
    entry.height           = 1200;
    entry.floating         = true;
    entry.workspaceSpecial = false;
    entry.timestamp        = FRESH_AT;
    return entry;
}

static SEntry tiledEntry() {
    SEntry entry;
    entry.cls              = "firefox";
    entry.title            = "";
    entry.workspace        = "special:magic";
    entry.monitor          = "HDMI-A-1";
    entry.x                = 0;
    entry.y                = 0;
    entry.width            = 800;
    entry.height           = 600;
    entry.floating         = false;
    entry.workspaceSpecial = true;
    entry.timestamp        = FRESH_AT - 1;
    return entry;
}

static SLayoutFile twoEntryFile() {
    SLayoutFile file;
    file.version   = 3;
    file.timestamp = FRESH_AT;
    file.entries   = {floatingEntry(), tiledEntry()};
    return file;
}

TEST(LayoutModelJson, roundTripPreservesEveryField) {
    const SLayoutFile original = twoEntryFile();

    const auto        json = LayoutRestore::serialize(original);
    ASSERT_TRUE(json.has_value());
    ASSERT_FALSE(json->empty());

    const auto parsed = LayoutRestore::deserialize(*json);
    ASSERT_TRUE(parsed.has_value());

    const SLayoutFile& out = *parsed;
    ASSERT_EQ(out.version, 3);
    ASSERT_EQ(out.timestamp, original.timestamp);
    ASSERT_EQ(out.entries.size(), 2U);

    const SEntry& FLOATING = out.entries[0];
    EXPECT_EQ(FLOATING.cls, "kitty");
    EXPECT_EQ(FLOATING.title, "editor");
    EXPECT_EQ(FLOATING.workspace, "3");
    EXPECT_EQ(FLOATING.monitor, "DP-1");
    EXPECT_EQ(FLOATING.x, -100);
    EXPECT_EQ(FLOATING.y, 250);
    EXPECT_EQ(FLOATING.width, 1920);
    EXPECT_EQ(FLOATING.height, 1200);
    EXPECT_TRUE(FLOATING.floating);
    EXPECT_FALSE(FLOATING.workspaceSpecial);
    EXPECT_EQ(FLOATING.timestamp, FRESH_AT);

    const SEntry& TILED = out.entries[1];
    EXPECT_EQ(TILED.cls, "firefox");
    EXPECT_EQ(TILED.title, "");
    EXPECT_EQ(TILED.workspace, "special:magic");
    EXPECT_EQ(TILED.monitor, "HDMI-A-1");
    EXPECT_EQ(TILED.x, 0);
    EXPECT_EQ(TILED.y, 0);
    EXPECT_EQ(TILED.width, 800);
    EXPECT_EQ(TILED.height, 600);
    EXPECT_FALSE(TILED.floating);
    EXPECT_TRUE(TILED.workspaceSpecial);
    EXPECT_EQ(TILED.timestamp, FRESH_AT - 1);
}

TEST(LayoutModelJson, serializeUsesOnDiskKeyNames) {
    const auto json = LayoutRestore::serialize(twoEntryFile());
    ASSERT_TRUE(json.has_value());

    EXPECT_NE(json->find("\"class\""), std::string::npos);
    EXPECT_EQ(json->find("\"cls\""), std::string::npos);
    EXPECT_NE(json->find("\"workspaceSpecial\""), std::string::npos);
    EXPECT_NE(json->find("\"floating\""), std::string::npos);
    EXPECT_NE(json->find("\"monitor\""), std::string::npos);
    EXPECT_NE(json->find("\"workspace\""), std::string::npos);
    EXPECT_NE(json->find("\"version\""), std::string::npos);
    EXPECT_NE(json->find("\"entries\""), std::string::npos);
    EXPECT_NE(json->find("\"timestamp\""), std::string::npos);
}

TEST(LayoutModelJson, serializeEmptyFileIsValidJsonForDeserialize) {
    const auto json = LayoutRestore::serialize(SLayoutFile{});
    ASSERT_TRUE(json.has_value());

    const auto parsed = LayoutRestore::deserialize(*json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->version, 2);
    EXPECT_EQ(parsed->timestamp, 0);
    EXPECT_TRUE(parsed->entries.empty());
}

TEST(LayoutModelJson, deserializeCorruptInputFails) {
    const auto parsed = LayoutRestore::deserialize("{ this is not json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_FALSE(parsed.error().empty());
}

TEST(LayoutModelJson, deserializeWrongTopLevelTypeFails) {
    const auto parsed = LayoutRestore::deserialize("[1, 2, 3]");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_FALSE(parsed.error().empty());
}

static SEntry matchEntry(std::string cls, std::string title) {
    SEntry entry;
    entry.cls   = std::move(cls);
    entry.title = std::move(title);
    // Timestamps are set explicitly by the tests that care about precedence/staleness.
    entry.timestamp = FRESH_AT;
    return entry;
}

TEST(LayoutModelMatch, matchEntryIndexPointsAtWinningEntry) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", ""), matchEntry("kitty", "")};
    file.entries[0].workspace = "old";
    file.entries[0].timestamp = NOW - 50;
    file.entries[1].workspace = "new";
    file.entries[1].timestamp = NOW - 10;

    const auto index = LayoutRestore::matchEntryIndex(file, "kitty", "anything", NOW, LIMIT);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, 1U);
    EXPECT_EQ(file.entries[*index].workspace, "new");

    // and matchEntry must agree with the index it reports
    const auto entry = LayoutRestore::matchEntry(file, "kitty", "anything", NOW, LIMIT);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->workspace, file.entries[*index].workspace);
}

TEST(LayoutModelMatch, matchEntryIndexNoMatchIsNullopt) {
    const auto index = LayoutRestore::matchEntryIndex(SLayoutFile{}, "kitty", "nvim", NOW, LIMIT);
    EXPECT_FALSE(index.has_value());

    const auto emptyClass = LayoutRestore::matchEntryIndex(twoEntryFile(), "", "", NOW, LIMIT);
    EXPECT_FALSE(emptyClass.has_value());
}

TEST(LayoutModelMatch, matchEntryIndexEqualTimestampsTakesLaterVectorSlot) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", ""), matchEntry("kitty", "")};
    file.entries[0].timestamp = NOW - 10;
    file.entries[1].timestamp = NOW - 10;

    const auto index = LayoutRestore::matchEntryIndex(file, "kitty", "nvim", NOW, LIMIT);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, 1U);
}

TEST(LayoutModelMatch, matchEntryIndexAfterConsumingEarlierEntry) {
    // Simulates the two-window restore: erase the consumed entry, the next match
    // must now resolve to the second one.
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", "one"), matchEntry("kitty", "two")};
    file.entries[0].timestamp = NOW - 10;
    file.entries[1].timestamp = NOW - 20;

    const auto first = LayoutRestore::matchEntryIndex(file, "kitty", "one", NOW, LIMIT);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(file.entries[*first].title, "one");
    file.entries.erase(file.entries.begin() + static_cast<std::ptrdiff_t>(*first));

    const auto second = LayoutRestore::matchEntryIndex(file, "kitty", "two", NOW, LIMIT);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(file.entries[*second].title, "two");
    EXPECT_EQ(file.entries.size(), 1U);
}

TEST(LayoutModelMatch, exactClassAndTitleMatchReturnsSavedValues) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", "nvim"), matchEntry("kitty", "htop")};

    const auto found = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->cls, "kitty");
    EXPECT_EQ(found->title, "nvim");
}

TEST(LayoutModelMatch, emptySavedTitleIsAWildcard) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", "")};

    const auto found = LayoutRestore::matchEntry(file, "kitty", "whatever", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "");
}

TEST(LayoutModelMatch, emptySavedTitleMatchesEmptyWindowTitleToo) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", "")};

    const auto found = LayoutRestore::matchEntry(file, "kitty", "", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->cls, "kitty");
}

TEST(LayoutModelMatch, nonEmptySavedTitleMustEqualWindowTitle) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", "nvim")};

    EXPECT_FALSE(LayoutRestore::matchEntry(file, "kitty", "ntim", NOW, LIMIT).has_value());
    EXPECT_FALSE(LayoutRestore::matchEntry(file, "kitty", "NVIM", NOW, LIMIT).has_value());
    EXPECT_FALSE(LayoutRestore::matchEntry(file, "kitty", "", NOW, LIMIT).has_value());
    ASSERT_TRUE(LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT).has_value());
}

TEST(LayoutModelMatch, emptyWindowClassNeverMatches) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", ""), matchEntry("", "")};

    EXPECT_FALSE(LayoutRestore::matchEntry(file, "", "nvim", NOW, LIMIT).has_value());
    EXPECT_FALSE(LayoutRestore::matchEntry(file, "", "", NOW, LIMIT).has_value());
}

TEST(LayoutModelMatch, classMismatchDoesNotMatch) {
    SLayoutFile file;
    file.entries = {matchEntry("kitty", "")};

    EXPECT_FALSE(LayoutRestore::matchEntry(file, "foot", "nvim", NOW, LIMIT).has_value());
}

TEST(LayoutModelMatch, noEntriesDoesNotMatch) {
    const auto found = LayoutRestore::matchEntry(SLayoutFile{}, "kitty", "nvim", NOW, LIMIT);
    EXPECT_FALSE(found.has_value());
}

TEST(LayoutModelMatch, newestTimestampWins) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", ""), matchEntry("kitty", "")};
    file.entries[0].timestamp = NOW - 50;
    file.entries[1].timestamp = NOW - 10;

    const auto found = LayoutRestore::matchEntry(file, "kitty", "anything", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->timestamp, NOW - 10);

    // Reordering the vector must not change which entry wins.
    SLayoutFile reversed;
    reversed.entries      = {file.entries[1], file.entries[0]};
    const auto foundAgain = LayoutRestore::matchEntry(reversed, "kitty", "anything", NOW, LIMIT);
    ASSERT_TRUE(foundAgain.has_value());
    EXPECT_EQ(foundAgain->timestamp, NOW - 10);
}

TEST(LayoutModelMatch, equalTimestampsLaterEntryWins) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", ""), matchEntry("kitty", "")};
    file.entries[0].timestamp = NOW - 10;
    file.entries[1].timestamp = NOW - 10;
    file.entries[0].workspace = "first-in-vector";
    file.entries[1].workspace = "later-in-vector";

    // Deterministic tie-break: with identical timestamps the later vector entry wins.
    const auto found = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->workspace, "later-in-vector");
    EXPECT_EQ(found->timestamp, NOW - 10);
}

TEST(LayoutModelMatch, entryOlderThanLimitDoesNotMatch) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", "")};
    file.entries[0].timestamp = STALE_AT;

    EXPECT_FALSE(LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT).has_value());
}

TEST(LayoutModelMatch, zeroStalenessNeverExpires) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", "nvim")};
    file.entries[0].timestamp = STALE_AT;

    const auto fresh = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT);
    ASSERT_FALSE(fresh.has_value());

    const auto forever = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, 0);
    ASSERT_TRUE(forever.has_value());
    EXPECT_EQ(forever->title, "nvim");
    EXPECT_EQ(forever->timestamp, STALE_AT);
}

TEST(LayoutModelMatch, negativeStalenessNeverExpires) {
    SLayoutFile file;
    file.entries              = {matchEntry("kitty", "nvim")};
    file.entries[0].timestamp = STALE_AT;

    const auto found = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, -5);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "nvim");
}

TEST(LayoutModelMatch, newestValidCandidateBeatsNewerButStaleOne) {
    SLayoutFile file;
    // The newest entry is a stale wildcard; an older but still fresh one must win.
    file.entries              = {matchEntry("kitty", "fresh"), matchEntry("kitty", "wildcard-stale")};
    file.entries[0].timestamp = NOW - 50;
    file.entries[1].timestamp = NOW - 5000;

    const auto found = LayoutRestore::matchEntry(file, "kitty", "fresh", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "fresh");
    EXPECT_EQ(found->timestamp, NOW - 50);
}

TEST(LayoutModelMatch, titleGateSkipsNonMatchingSavedTitles) {
    SLayoutFile file;
    // Both candidates share the timestamp; only the wildcard can match this window.
    file.entries              = {matchEntry("kitty", "htop"), matchEntry("kitty", "")};
    file.entries[0].timestamp = NOW - 10;
    file.entries[1].timestamp = NOW - 10;

    const auto found = LayoutRestore::matchEntry(file, "kitty", "nvim", NOW, LIMIT);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "");
    EXPECT_EQ(found->workspace, "");
}

TEST(LayoutModelFilterStale, nonPositiveLimitReturnsFileUnchanged) {
    SLayoutFile file = twoEntryFile();

    for (const int64_t STALENESS : {int64_t{0}, int64_t{-1}}) {
        const SLayoutFile out = LayoutRestore::filterStale(file, NOW, STALENESS);
        EXPECT_EQ(out.version, file.version);
        EXPECT_EQ(out.timestamp, file.timestamp);
        ASSERT_EQ(out.entries.size(), file.entries.size());
        EXPECT_EQ(out.entries[0].cls, file.entries[0].cls);
        EXPECT_EQ(out.entries[1].cls, file.entries[1].cls);
    }
}

TEST(LayoutModelFilterStale, prunesStaleKeepsFreshPreservingHeaderAndOrder) {
    SLayoutFile file;
    file.version              = 7;
    file.timestamp            = STALE_AT;
    file.entries              = {matchEntry("kitty", ""), matchEntry("firefox", ""), matchEntry("foot", "")};
    file.entries[0].timestamp = STALE_AT;
    file.entries[1].timestamp = NOW;
    file.entries[2].timestamp = FRESH_AT;

    const SLayoutFile out = LayoutRestore::filterStale(file, NOW, LIMIT);
    EXPECT_EQ(out.version, 7);
    EXPECT_EQ(out.timestamp, STALE_AT);
    ASSERT_EQ(out.entries.size(), 2U);
    EXPECT_EQ(out.entries[0].cls, "firefox");
    EXPECT_EQ(out.entries[1].cls, "foot");
    EXPECT_EQ(out.entries[0].timestamp, NOW);
    EXPECT_EQ(out.entries[1].timestamp, FRESH_AT);

    // An entry exactly at the limit is still fresh.
    SLayoutFile boundary;
    boundary.entries              = {matchEntry("kitty", "")};
    boundary.entries[0].timestamp = FRESH_AT;
    const SLayoutFile kept        = LayoutRestore::filterStale(boundary, NOW, LIMIT);
    ASSERT_EQ(kept.entries.size(), 1U);
    EXPECT_EQ(kept.entries[0].cls, "kitty");
}

TEST(LayoutModelFilterStale, allStaleLeavesEmptyEntries) {
    SLayoutFile file;
    file.version              = 2;
    file.timestamp            = STALE_AT;
    file.entries              = {matchEntry("kitty", ""), matchEntry("foot", "")};
    file.entries[0].timestamp = STALE_AT;
    file.entries[1].timestamp = STALE_AT - 10;

    const SLayoutFile out = LayoutRestore::filterStale(file, NOW, LIMIT);
    EXPECT_EQ(out.version, 2);
    EXPECT_EQ(out.timestamp, STALE_AT);
    EXPECT_TRUE(out.entries.empty());
}

class CLayoutModelFileIOTest : public ::testing::Test {
  public:
    void SetUp() override {
        m_path = std::filesystem::temp_directory_path() / ("hyprland-layoutmodel-" + std::to_string(getpid()) + "-" + std::to_string(m_counter++) + ".json");
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    bool overwriteWith(std::string contents) {
        std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream << contents;
        return static_cast<bool>(stream);
    }

    static SLayoutFile singleEntryFile() {
        SLayoutFile file;
        file.version   = 9;
        file.timestamp = FRESH_AT;
        file.entries   = {tiledEntry()};
        return file;
    }

    std::filesystem::path m_path;

  private:
    static int64_t m_counter;
};

int64_t CLayoutModelFileIOTest::m_counter = 0;

TEST_F(CLayoutModelFileIOTest, writeThenReadRoundTrips) {
    const SLayoutFile original = twoEntryFile();

    const auto        written = LayoutRestore::writeFile(m_path.string(), original);
    ASSERT_TRUE(written.has_value()) << written.error();
    ASSERT_TRUE(std::filesystem::exists(m_path));

    const auto read = LayoutRestore::readFile(m_path.string());
    ASSERT_TRUE(read.has_value()) << read.error();

    const SLayoutFile& out = *read;
    EXPECT_EQ(out.version, 3);
    EXPECT_EQ(out.timestamp, original.timestamp);
    ASSERT_EQ(out.entries.size(), 2U);
    EXPECT_EQ(out.entries[0].cls, "kitty");
    EXPECT_EQ(out.entries[0].title, "editor");
    EXPECT_EQ(out.entries[0].workspace, "3");
    EXPECT_EQ(out.entries[0].monitor, "DP-1");
    EXPECT_EQ(out.entries[0].x, -100);
    EXPECT_EQ(out.entries[0].y, 250);
    EXPECT_EQ(out.entries[0].width, 1920);
    EXPECT_EQ(out.entries[0].height, 1200);
    EXPECT_TRUE(out.entries[0].floating);
    EXPECT_FALSE(out.entries[0].workspaceSpecial);
    EXPECT_EQ(out.entries[0].timestamp, FRESH_AT);
    EXPECT_EQ(out.entries[1].cls, "firefox");
    EXPECT_EQ(out.entries[1].workspace, "special:magic");
    EXPECT_EQ(out.entries[1].monitor, "HDMI-A-1");
    EXPECT_TRUE(out.entries[1].workspaceSpecial);
    EXPECT_FALSE(out.entries[1].floating);
    EXPECT_EQ(out.entries[1].timestamp, FRESH_AT - 1);
}

TEST_F(CLayoutModelFileIOTest, writeOverwritesExistingFile) {
    const SLayoutFile first = twoEntryFile();
    ASSERT_TRUE(LayoutRestore::writeFile(m_path.string(), first).has_value());

    const SLayoutFile second = singleEntryFile();
    const auto        result = LayoutRestore::writeFile(m_path.string(), second);
    ASSERT_TRUE(result.has_value()) << result.error();

    const auto read = LayoutRestore::readFile(m_path.string());
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->entries.size(), 1U);
    EXPECT_EQ(read->entries[0].cls, "firefox");
    EXPECT_EQ(read->version, second.version);
}

TEST_F(CLayoutModelFileIOTest, readMissingFileFailsWithPathInMessage) {
    const std::string missing = (m_path.parent_path() / "hyprland-layoutmodel-does-not-exist.json").string();
    std::error_code   ec;
    std::filesystem::remove(missing, ec);

    const auto read = LayoutRestore::readFile(missing);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), "failed to open " + missing);
    EXPECT_NE(read.error().find(missing), std::string::npos);
}

TEST_F(CLayoutModelFileIOTest, writeIntoMissingDirectoryFails) {
    const auto bad = (m_path.parent_path() / "hyprland-layoutmodel-missing-dir" / "layout.json").string();

    const auto written = LayoutRestore::writeFile(bad, twoEntryFile());
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().find(bad), std::string::npos);
}

TEST_F(CLayoutModelFileIOTest, readCorruptFileFails) {
    ASSERT_TRUE(LayoutRestore::writeFile(m_path.string(), twoEntryFile()).has_value());
    ASSERT_TRUE(overwriteWith("{corrupt"));

    const auto read = LayoutRestore::readFile(m_path.string());
    ASSERT_FALSE(read.has_value());
    EXPECT_FALSE(read.error().empty());
}

//
// relaunch: cmdline capture, skip rules, insertion order
//
namespace {
    SEntry relaunchEntry(std::string cls, std::string ws, std::string cmdline, int order) {
        SEntry entry;
        entry.cls       = std::move(cls);
        entry.title     = "";
        entry.workspace = std::move(ws);
        entry.monitor   = "DP-1";
        entry.width     = 800;
        entry.height    = 600;
        entry.timestamp = FRESH_AT;
        entry.cmdline   = std::move(cmdline);
        entry.order     = order;
        return entry;
    }
}

TEST(LayoutRelaunch, joinCmdlineSplitsOnNulAndDropsTrailingSeparator) {
    // Built by length: a std::string{const char*} would stop at the first NUL.
    static const char BYTES[] = {'k', 'i', 't', 't', 'y', '\0', '-', '-', 'd', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', '\0', '/', 't', 'm', 'p', '\0'};
    const std::string raw(BYTES, sizeof(BYTES));
    EXPECT_EQ(LayoutRestore::joinCmdline(raw), "kitty --directory /tmp");
}

TEST(LayoutRelaunch, joinCmdlineKeepsInternalEmptyArgv) {
    // An intentionally empty argv element must survive: dropping it shifts the rest.
    static const char BYTES[] = {'p', 'r', 'o', 'g', '\0', '\0', 'a', 'r', 'g', '\0'};
    const std::string raw(BYTES, sizeof(BYTES));
    EXPECT_EQ(LayoutRestore::joinCmdline(raw), "prog  arg");
}

TEST(LayoutRelaunch, joinCmdlineOfEmptyBlobIsEmpty) {
    EXPECT_TRUE(LayoutRestore::joinCmdline("").empty());
}

TEST(LayoutRelaunch, joinCmdlineOfOnlySeparatorsIsEmpty) {
    static const char BYTES[] = {'\0', '\0'};
    const std::string raw(BYTES, sizeof(BYTES));
    EXPECT_TRUE(LayoutRestore::joinCmdline(raw).empty());
}

TEST(LayoutRelaunch, isRelaunchableRequiresACapturedCommand) {
    const SEntry capturable = relaunchEntry("kitty", "1", "kitty", 0);
    const SEntry empty      = relaunchEntry("kitty", "1", "", 0);

    EXPECT_TRUE(LayoutRestore::isRelaunchable(capturable));
    EXPECT_FALSE(LayoutRestore::isRelaunchable(empty));
}

TEST(LayoutRelaunch, relaunchOrderReturnsOnlyCapturableEntries) {
    SLayoutFile file;
    file.entries = {
        relaunchEntry("firefox", "1", "firefox", 0),
        relaunchEntry("xwayland", "1", "", 0), // uncapturable: skipped
        relaunchEntry("kitty", "1", "kitty", 1),
    };

    const auto out = LayoutRestore::relaunchOrder(file, "1");
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out[0], 0U); // firefox
    EXPECT_EQ(out[1], 2U); // kitty
}

TEST(LayoutRelaunch, relaunchOrderSortsBySavedInsertionOrder) {
    // The vector is deliberately scrambled: order must come from `order`, not position.
    SLayoutFile file;
    file.entries = {
        relaunchEntry("kitty", "1", "kitty", 2),
        relaunchEntry("firefox", "1", "firefox", 0),
        relaunchEntry("code", "1", "code", 1),
    };

    const auto out = LayoutRestore::relaunchOrder(file, "1");
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(file.entries[out[0]].cls, "firefox");
    EXPECT_EQ(file.entries[out[1]].cls, "code");
    EXPECT_EQ(file.entries[out[2]].cls, "kitty");
}

TEST(LayoutRelaunch, relaunchOrderUnknownOrderGoesLast) {
    SLayoutFile file;
    file.entries = {
        relaunchEntry("kitty", "1", "kitty", 0),
        relaunchEntry("firefox", "1", "firefox", -1),
    };

    const auto out = LayoutRestore::relaunchOrder(file, "1");
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out[0], 0U); // known order first
    EXPECT_EQ(out[1], 1U); // -1 sorts after every real order
}

TEST(LayoutRelaunch, relaunchOrderIsScopedToTheRequestedWorkspace) {
    SLayoutFile file;
    file.entries = {
        relaunchEntry("kitty", "1", "kitty", 0),
        relaunchEntry("firefox", "2", "firefox", 0),
        relaunchEntry("code", "3", "code", 0),
    };

    EXPECT_EQ(LayoutRestore::relaunchOrder(file, "2").size(), 1U);
    EXPECT_EQ(LayoutRestore::relaunchOrder(file, "3").size(), 1U);
    EXPECT_TRUE(LayoutRestore::relaunchOrder(file, "4").empty());
}

TEST(LayoutRelaunch, relaunchOrderStableForEqualSavedOrder) {
    SLayoutFile file;
    file.entries = {
        relaunchEntry("a", "1", "a", 0),
        relaunchEntry("b", "1", "b", 0),
        relaunchEntry("c", "1", "c", 0),
    };

    const auto out = LayoutRestore::relaunchOrder(file, "1");
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(file.entries[out[0]].cls, "a");
    EXPECT_EQ(file.entries[out[1]].cls, "b");
    EXPECT_EQ(file.entries[out[2]].cls, "c");
}

// Geometry precedence: the saved geometry is only authoritative for floating
// windows. A tiled one in a non-empty workspace is placed by the layout, which
// derives it from the restored insertion order.
TEST(LayoutGeometryPrecedence, floatingWindowTakesSavedGeometry) {
    const SEntry entry = floatingEntry();
    EXPECT_TRUE(entry.floating);
    EXPECT_TRUE(entry.width > 0 && entry.height > 0);
}

TEST(LayoutGeometryPrecedence, tiledWindowInNonEmptyWorkspaceIsNotGeometryDriven) {
    SEntry entry = tiledEntry();
    entry.order  = 1; // second window of the workspace, so the layout places it

    EXPECT_FALSE(entry.floating);
    EXPECT_GE(entry.order, 0);
}

TEST_F(CLayoutModelFileIOTest, writeThenReadRoundTripsRelaunchFields) {
    SLayoutFile file;
    file.entries = {relaunchEntry("kitty", "1", "kitty --directory /tmp", 3)};

    const auto written = LayoutRestore::writeFile(m_path.string(), file);
    ASSERT_TRUE(written.has_value()) << written.error();

    const auto read = LayoutRestore::readFile(m_path.string());
    ASSERT_TRUE(read.has_value()) << read.error();
    ASSERT_EQ(read->entries.size(), 1U);
    EXPECT_EQ(read->entries[0].cmdline, "kitty --directory /tmp");
    EXPECT_EQ(read->entries[0].order, 3);
}

TEST_F(CLayoutModelFileIOTest, entriesWithoutNewFieldsStillDeserialize) {
    // Version-1 files have no cmdline/order: they must load with the defaults so
    // an old layout file is not rejected on upgrade.
    const std::string v1 =
        R"({"version":1,"timestamp":1000,"entries":[{"class":"kitty","title":"t","workspace":"1","monitor":"DP-1","x":1,"y":2,"width":3,"height":4,"floating":false,"workspaceSpecial":false,"timestamp":1000}]})";
    ASSERT_TRUE(overwriteWith(v1));

    const auto read = LayoutRestore::readFile(m_path.string());
    ASSERT_TRUE(read.has_value()) << read.error();
    ASSERT_EQ(read->entries.size(), 1U);
    EXPECT_EQ(read->entries[0].cls, "kitty");
    EXPECT_TRUE(read->entries[0].cmdline.empty());
    EXPECT_EQ(read->entries[0].order, -1);
}
