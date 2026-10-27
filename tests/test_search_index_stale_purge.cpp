// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F24 — item 2: stale plaintext search
// indexes are never purged.
//
// F2 closed the leak that wrote a plaintext Tantivy index to %TEMP% for a
// copy-restricted document (the PERMISSION GATE in DocumentSearchIndex.cpp).
// It stopped NEW leaks and left every index a pre-fix build had already
// written sitting on disk — a searchable plaintext copy of restricted page
// text, on every machine that ever ran one. A fix that stops the bleeding and
// leaves the spill is half a fix.
//
// The sweep is DocumentSearchIndex::sweepStaleIndexes(). Its hazard is the
// mirror image of the leak: deleting a CONCURRENTLY-RUNNING instance's live
// index would corrupt that instance's search mid-session, which is worse than
// the leak it fixes. Two INDEPENDENT gates must both pass before anything is
// removed:
//
//   (a) OWNERSHIP — a live index carries a sibling "<key>.pdfe_index_lock"
//       holding the owning process id, held open for the index's lifetime. A
//       directory whose lock names a LIVE process is never touched. Anything
//       undecidable is treated as alive (fail safe).
//   (b) AGE — the directory must be older than the caller's threshold.
//
// Every test below asserts one gate with the other held constant, so neither
// can be passing by accident, and the "another instance is running" case has
// its own test with a dead-pid control.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "search/DocumentSearchIndex.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using pdfe::search::DocumentSearchIndex;
using pdfe::search::StaleIndexSweepReport;

namespace {

bool listed(const std::vector<std::filesystem::path>& v,
            const std::filesystem::path& p) {
    for (const auto& e : v) {
        if (e == p) return true;
    }
    return false;
}

class StaleIndexSweepTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        // The sweep refuses any root not named pdfe_search_indexes (it deletes
        // directory trees), so the fake root must carry the real name.
        root_ = std::filesystem::path(tmp_.path().toStdString())
              / "pdfe_search_indexes";
        std::filesystem::create_directories(root_);
    }

    /// Plant an index directory holding a plaintext page-text file — the shape
    /// of the actual leak, so the assertion is "the spilled text is gone", not
    /// "an empty directory is gone".
    std::filesystem::path plant(const char* key) const {
        const auto dir = root_ / key;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "meta.json") << "{\"segments\":[]}";
        std::ofstream(dir / "leaked.txt")
            << "RESTRICTED-PAGE-TEXT-SENTINEL";
        return dir;
    }

    /// Write the sibling ownership lock for `dir` naming `pid`.
    void writeLock(const std::filesystem::path& dir, unsigned long pid) const {
        std::filesystem::path lock = dir;
        lock += ".pdfe_index_lock";
        std::ofstream(lock) << pid << "\n";
    }

    std::filesystem::path lockOf(const std::filesystem::path& dir) const {
        std::filesystem::path lock = dir;
        lock += ".pdfe_index_lock";
        return lock;
    }

    QTemporaryDir tmp_;
    std::filesystem::path root_;
};

/// A process id that is guaranteed NOT to be running: spawn a trivial process,
/// wait for it to exit, and use its pid. Fabricating a "probably unused" number
/// would make the control depend on luck.
unsigned long deadProcessId() {
    QProcess p;
#ifdef _WIN32
    p.start("cmd.exe", {"/c", "exit"});
#else
    p.start("/bin/sh", {"-c", "exit"});
#endif
    if (!p.waitForStarted(10000)) return 0;
    const auto pid = static_cast<unsigned long>(p.processId());
    p.waitForFinished(10000);
    return pid;
}

unsigned long liveProcessId() {
    // Our own pid: unambiguously alive for the duration of the test.
    return static_cast<unsigned long>(QCoreApplication::applicationPid());
}

} // namespace

// ===========================================================================
// 1. THE HEADLINE — a planted stale index is removed; a current-session index
//    is not. Both directories are in the same root and swept in one call, so
//    the sweep has to DISCRIMINATE, not just "delete everything" or "delete
//    nothing".
// ===========================================================================

TEST_F(StaleIndexSweepTest, StalePlantedIndexRemovedCurrentSessionIndexKept) {
    // Stale: the pre-fix leak shape — no ownership lock at all, because the
    // builds that wrote these never had one.
    const auto stale = plant("stale_pre_fix_build");
    ASSERT_FALSE(std::filesystem::exists(lockOf(stale)));

    // Current session: same directory shape, but owned by a live process.
    const auto live = plant("current_session");
    writeLock(live, liveProcessId());

    const auto report = DocumentSearchIndex::sweepStaleIndexes(
        root_, std::chrono::seconds(0));
    EXPECT_FALSE(report.refusedRoot);

    EXPECT_FALSE(std::filesystem::exists(stale))
        << "the stale plaintext index must be REMOVED";
    EXPECT_TRUE(listed(report.removed, stale));

    EXPECT_TRUE(std::filesystem::exists(live))
        << "the current session's live index must be KEPT";
    EXPECT_TRUE(std::filesystem::exists(live / "leaked.txt"))
        << "the live index's contents must be intact, not merely its directory";
    EXPECT_TRUE(listed(report.kept, live));
}

// ===========================================================================
// 2. THE CONCURRENCY REQUIREMENT — a directory owned by ANOTHER live process
//    is never deleted, however old it is. Control: the identical directory
//    whose lock names a process that has exited IS deleted, so the protection
//    is the liveness check and not a refusal to delete anything with a lock.
// ===========================================================================

TEST_F(StaleIndexSweepTest, LiveOtherInstanceIndexSurvivesDeadOwnerIsPurged) {
    const auto liveOwned = plant("owned_by_live_instance");
    writeLock(liveOwned, liveProcessId());

    const unsigned long dead = deadProcessId();
    ASSERT_NE(dead, 0u) << "could not spawn a process to obtain a dead pid";
    ASSERT_FALSE(DocumentSearchIndex::processIsAlive(dead))
        << "precondition: pid " << dead << " must have exited";
    ASSERT_TRUE(DocumentSearchIndex::processIsAlive(liveProcessId()))
        << "precondition: our own pid must read as alive";

    const auto deadOwned = plant("owned_by_dead_instance");
    writeLock(deadOwned, dead);

    // Threshold 0 => the age gate cannot be what saves anything here; only
    // ownership can.
    const auto report = DocumentSearchIndex::sweepStaleIndexes(
        root_, std::chrono::seconds(0));

    EXPECT_TRUE(std::filesystem::exists(liveOwned))
        << "deleting a concurrently-running instance's live index is a worse "
           "bug than the leak being fixed";
    EXPECT_TRUE(std::filesystem::exists(liveOwned / "leaked.txt"));
    EXPECT_TRUE(listed(report.kept, liveOwned));

    EXPECT_FALSE(std::filesystem::exists(deadOwned))
        << "CONTROL: a lock naming an EXITED process must not protect the "
           "directory, otherwise the ownership gate would keep everything "
           "forever";
    EXPECT_TRUE(listed(report.removed, deadOwned));
}

// ===========================================================================
// 3. THE AGE GATE IS REAL — the same unowned directory survives a threshold it
//    is younger than and is removed by one it is older than. This is the belt
//    that also covers a concurrently-running PRE-FIX build, which has no lock
//    file to find but does have a freshly-touched directory.
// ===========================================================================

TEST_F(StaleIndexSweepTest, FreshUnownedIndexSurvivesTheAgeThreshold) {
    const auto fresh = plant("fresh_unowned");

    const auto kept = DocumentSearchIndex::sweepStaleIndexes(
        root_, std::chrono::hours(24));
    EXPECT_TRUE(std::filesystem::exists(fresh))
        << "a directory younger than the threshold must be kept even with no "
           "ownership lock — that is what protects a running pre-fix build";
    EXPECT_TRUE(listed(kept.kept, fresh));

    const auto swept = DocumentSearchIndex::sweepStaleIndexes(
        root_, std::chrono::seconds(0));
    EXPECT_FALSE(std::filesystem::exists(fresh))
        << "CONTROL: past the threshold the same directory must be removed, "
           "otherwise the previous assertion would hold for a sweep that never "
           "deletes anything";
    EXPECT_TRUE(listed(swept.removed, fresh));
}

// ===========================================================================
// 4. An unparseable lock file is respected — the fail-safe direction.
// ===========================================================================

TEST_F(StaleIndexSweepTest, UnreadableLockIsTreatedAsLiveNotAsAbsent) {
    const auto garbled = plant("garbled_lock");
    std::ofstream(lockOf(garbled)) << "not-a-pid\n";

    const auto report = DocumentSearchIndex::sweepStaleIndexes(
        root_, std::chrono::seconds(0));
    EXPECT_TRUE(std::filesystem::exists(garbled))
        << "a lock we cannot read is a lock we must respect; guessing wrong "
           "costs a running instance its index";
    EXPECT_TRUE(listed(report.kept, garbled));
}

// ===========================================================================
// 5. The safety rail — the sweep refuses a root that is not an index root.
//    It deletes directory trees; a future caller must not be able to aim it
//    somewhere else.
// ===========================================================================

TEST_F(StaleIndexSweepTest, RefusesARootThatIsNotAnIndexRoot) {
    const auto wrong = std::filesystem::path(tmp_.path().toStdString())
                     / "my_documents";
    std::filesystem::create_directories(wrong / "important");
    std::ofstream(wrong / "important" / "thesis.txt") << "do not delete";

    const auto report = DocumentSearchIndex::sweepStaleIndexes(
        wrong, std::chrono::seconds(0));
    EXPECT_TRUE(report.refusedRoot);
    EXPECT_TRUE(report.removed.empty());
    EXPECT_TRUE(std::filesystem::exists(wrong / "important" / "thesis.txt"))
        << "the sweep must never delete outside a PDFE index root";
}

// ===========================================================================
// 6. The root the production sweep targets is the root the product actually
//    writes indexes into. Without this the sweep could be pointed at a
//    directory the leak never used and every test above would still pass.
// ===========================================================================

TEST_F(StaleIndexSweepTest, ProductionSweepRootIsWhereIndexesAreActuallyCreated) {
    const auto root = DocumentSearchIndex::indexRoot();
    EXPECT_EQ(root.filename().string(), std::string("pdfe_search_indexes"));

    // Ask the product where it would put THIS document's index. Comparing that
    // against the sweep root is the non-tautological half: it is derived by the
    // constructor, not restated from the same constant.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(
        ctx, std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf");
    ASSERT_NE(doc, nullptr);
    DocumentSearchIndex idx(doc.get());
    EXPECT_EQ(idx.indexDir().parent_path(), root)
        << "the purge must target the directory the product materialises page "
           "text into — index dir was " << idx.indexDir().string();

    // …and that root lives under %TEMP%, which is where the pre-fix builds
    // spilled. `equivalent` rather than `==` because temp_directory_path()
    // carries a trailing separator on Windows and parent_path() does not.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::equivalent(
        root.parent_path(), std::filesystem::temp_directory_path(), ec))
        << "sweep root parent " << root.parent_path().string()
        << " is not the system temp directory";
}
