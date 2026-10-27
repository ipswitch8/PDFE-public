// -*- coding: utf-8 -*-
//
// GitHub #152 — the search index leaves every page's plaintext in %TEMP% for
// 24 h or more after the app closes, because the destructor does not remove it
// and the only cleanup is an age-gated sweep that fires opportunistically at
// the NEXT index construction. On a machine where PDFE is used once and not
// reopened, nothing ever sweeps.
//
// The fix is sweepOwnIndexesAtShutdown(). The whole risk in it is the opposite
// of the retention problem: a sweep that deletes an index another live instance
// is using is a cross-instance data-loss bug — the class this project already
// hit once with autosave sidecars, and the reason
// AutosaveManager::isForeignLiveInstance exists. So every ownership gate gets
// its own test with the others held constant:
//
//   * the happy path removes OUR index;
//   * a lock file naming ANOTHER pid is refused (the two-instances-same-
//     document case, where the second claim overwrote the first);
//   * a directory with a LIVE DocumentSearchIndex still bound to it is refused;
//   * the destructor is confirmed to still LEAVE the files, because that is the
//     within-session amortisation #152 deliberately preserved — and because it
//     is the fact the file's own comment used to get wrong.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "search/DocumentSearchIndex.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

using pdfe::search::DocumentSearchIndex;
using pdfe::search::StaleIndexSweepReport;

namespace {

bool listed(const std::vector<std::filesystem::path>& v,
            const std::filesystem::path& p)
{
    for (const auto& e : v) {
        if (e == p) return true;
    }
    return false;
}

unsigned long selfPid()
{
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

class ShutdownSweepTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture))
            << "tracked fixture missing: " << fixture.string();

        // A PRIVATE COPY per test, under a unique name. The index directory is
        // keyed by a hash of the document's absolute path, so a private copy
        // gives this test its own index directory and cannot collide with a
        // neighbouring test's live index.
        work_ = std::filesystem::temp_directory_path()
              / ("pdfe_issue152_" + std::to_string(counter_++));
        std::filesystem::create_directories(work_);
        docPath_ = work_ / "doc.pdf";
        std::filesystem::copy_file(
            fixture, docPath_,
            std::filesystem::copy_options::overwrite_existing);

        doc_ = pdfe::core::Document::open(ctx_, docPath_);
        ASSERT_NE(doc_, nullptr);
    }

    void TearDown() override
    {
        doc_.reset();
        std::error_code ec;
        std::filesystem::remove_all(work_, ec);
    }

    /// Build a real on-disk index for doc_ and return where it landed.
    /// Asserts the index actually materialised — a sweep test whose subject
    /// never existed passes for the wrong reason.
    std::filesystem::path buildIndex(DocumentSearchIndex& idx)
    {
        (void)idx.searchPages("page", 5);
        idx.waitForPendingReindex();
        const auto dir = idx.indexDir();
        EXPECT_TRUE(std::filesystem::exists(dir))
            << "no index directory was created at " << dir.string();
        return dir;
    }

    static std::filesystem::path lockFor(const std::filesystem::path& dir)
    {
        std::filesystem::path lock = dir;
        lock += DocumentSearchIndex::kLockSuffix;
        return lock;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::filesystem::path work_;
    std::filesystem::path docPath_;
    static int counter_;
};

int ShutdownSweepTest::counter_ = 0;

// -------------------------------------------------------------------------
// The behaviour the misleading comment described, and the one it actually had.
// -------------------------------------------------------------------------
TEST_F(ShutdownSweepTest, DestructorStillLeavesTheIndexForWithinSessionReuse)
{
    std::filesystem::path dir;
    {
        DocumentSearchIndex idx(doc_.get());
        dir = buildIndex(idx);
        ASSERT_FALSE(dir.empty());
    }

    // NOT a defect — the deliberate amortisation, restated here so that a
    // future change to the destructor cannot quietly land without a decision.
    // The PERMISSION GATE comment in DocumentSearchIndex.cpp used to claim the
    // destructor removed these files; it never did, and this is the assertion
    // that pins which of the two is true.
    EXPECT_TRUE(std::filesystem::exists(dir))
        << "the destructor removed the index directory — that is a behaviour "
           "change from what #152 decided (keep within-session, sweep at "
           "shutdown), and the comment at the permission gate now disagrees "
           "with the code again";
}

// -------------------------------------------------------------------------
// Gate: happy path.
// -------------------------------------------------------------------------
TEST_F(ShutdownSweepTest, ShutdownSweepRemovesThisProcessesOwnIndex)
{
    std::filesystem::path dir;
    {
        DocumentSearchIndex idx(doc_.get());
        dir = buildIndex(idx);
    }
    ASSERT_TRUE(std::filesystem::exists(dir));
    ASSERT_TRUE(std::filesystem::exists(lockFor(dir)))
        << "no ownership lock was written, so the sweep has nothing to verify "
           "ownership against";

    const StaleIndexSweepReport report =
        DocumentSearchIndex::sweepOwnIndexesAtShutdown();

    EXPECT_TRUE(listed(report.removed, dir))
        << "the shutdown sweep did not report removing " << dir.string();
    EXPECT_FALSE(std::filesystem::exists(dir))
        << "every page's plaintext is still in %TEMP% after this process swept "
           "at shutdown (GitHub #152)";
    EXPECT_FALSE(std::filesystem::exists(lockFor(dir)))
        << "the ownership lock outlived the index it locked";
}

// -------------------------------------------------------------------------
// Gate: "ITS OWN" — a lock naming another process is a refusal, not a cleanup.
// -------------------------------------------------------------------------
TEST_F(ShutdownSweepTest, ShutdownSweepRefusesAnIndexClaimedByAnotherProcess)
{
    std::filesystem::path dir;
    {
        DocumentSearchIndex idx(doc_.get());
        dir = buildIndex(idx);
    }
    ASSERT_TRUE(std::filesystem::exists(dir));

    // Simulate the real case: a SECOND PDFE instance opened the same document,
    // derived the same index key, and overwrote the lock with its own pid. That
    // instance is the live owner; this one must leave the directory alone.
    const unsigned long other = selfPid() + 1;
    {
        std::ofstream out(lockFor(dir), std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << other << "\n";
    }

    const StaleIndexSweepReport report =
        DocumentSearchIndex::sweepOwnIndexesAtShutdown();

    EXPECT_FALSE(listed(report.removed, dir))
        << "the shutdown sweep deleted an index whose lock names pid " << other
        << ", not this process (" << selfPid() << ") — that is the "
           "cross-instance data-loss bug, not a cleanup";
    EXPECT_TRUE(listed(report.kept, dir));
    EXPECT_TRUE(std::filesystem::exists(dir))
        << "another instance's live plaintext index was destroyed";

    // Clean up what the sweep correctly refused to touch.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(lockFor(dir), ec);
}

// -------------------------------------------------------------------------
// Gate: a directory a live DocumentSearchIndex is still bound to.
// -------------------------------------------------------------------------
TEST_F(ShutdownSweepTest, ShutdownSweepRefusesAnIndexStillInUseInThisProcess)
{
    DocumentSearchIndex live(doc_.get());
    const auto dir = buildIndex(live);
    ASSERT_TRUE(std::filesystem::exists(dir));

    const StaleIndexSweepReport report =
        DocumentSearchIndex::sweepOwnIndexesAtShutdown();

    EXPECT_FALSE(listed(report.removed, dir))
        << "the sweep pulled the index directory out from under a live "
           "DocumentSearchIndex";
    EXPECT_TRUE(listed(report.kept, dir));
    EXPECT_TRUE(std::filesystem::exists(dir));

    // Still searchable, which is the property the gate exists to protect.
    EXPECT_GT(live.documentCount(), 0)
        << "the live index lost its contents to the shutdown sweep";
}

// -------------------------------------------------------------------------
// Idempotence — the handler is registered with atexit AND callable directly.
// -------------------------------------------------------------------------
TEST_F(ShutdownSweepTest, ShutdownSweepIsIdempotent)
{
    std::filesystem::path dir;
    {
        DocumentSearchIndex idx(doc_.get());
        dir = buildIndex(idx);
    }

    const auto first = DocumentSearchIndex::sweepOwnIndexesAtShutdown();
    ASSERT_TRUE(listed(first.removed, dir));

    const auto second = DocumentSearchIndex::sweepOwnIndexesAtShutdown();
    EXPECT_FALSE(listed(second.removed, dir))
        << "a second sweep reported removing an already-removed directory";
    EXPECT_FALSE(listed(second.kept, dir))
        << "the registry still holds an entry for a directory that is gone";
}

}  // namespace
