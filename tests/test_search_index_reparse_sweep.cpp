// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F24 REMEDIATION — security-audit gate,
// finding 2: the stale-index sweep had no reparse-point defence.
//
// DocumentSearchIndex::sweepStaleIndexes() deletes directory TREES. It
// enumerated candidates with directory_iterator + entry.is_directory() and then
// called std::filesystem::remove_all(). Neither of those classifies the NAME:
//
//   * entry.is_directory() FOLLOWS a reparse point and answers true;
//   * MSVC's std::filesystem::is_symlink() recognises IO_REPARSE_TAG_SYMLINK
//     but NOT IO_REPARSE_TAG_MOUNT_POINT, which is what an NTFS JUNCTION uses.
//
// So a junction planted under %TEMP%\pdfe_search_indexes — writable by anything
// running as the same user, and creatable with `mklink /J`, which needs no
// privilege and no developer mode — with no adjacent .pdfe_index_lock and an
// old mtime passed BOTH existing gates. remove_all() was then aimed at whatever
// it pointed to. A deletion sweep that can be redirected is the highest
// blast-radius item in the phase; the existing root-name rail validates only
// the ROOT's filename and says nothing about what is inside it.
//
// The fix adds gate (0), REDIRECTION, which classifies by file ATTRIBUTES
// (FILE_ATTRIBUTE_REPARSE_POINT, covering both tags) rather than by
// is_symlink(), uses the non-following symlink_status when classifying, scans
// the whole candidate tree before deleting it (remove_all recurses, so a
// junction one level down is the same hazard), and fails safe toward KEEPING.
//
// THESE TESTS PLANT A REAL JUNCTION. They do not simulate one, and they do NOT
// silently skip if one cannot be created — a silent skip here would recreate
// exactly the false-green class this programme exists to remove. If the
// environment cannot create the redirection, the capability probe FAILS the
// test loudly.
//
// Every test carries a DISCRIMINATING CONTROL: an ordinary stale index
// directory in the same sweep, which must still be removed. Without it, a
// sweep that had simply stopped deleting anything would pass.

#include <gtest/gtest.h>

#include "search/DocumentSearchIndex.h"

#include <QTemporaryDir>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// Create a directory redirection at `link` pointing at `target`.
///
/// Windows: an NTFS JUNCTION via `mklink /J`, deliberately — a junction needs
/// no privilege and no developer mode, and it is the tag MSVC's is_symlink()
/// does NOT report, i.e. the exact shape of the hazard. Returns false if the
/// redirection could not be created; the caller must FAIL, never skip.
bool makeDirectoryRedirection(const std::filesystem::path& link,
                              const std::filesystem::path& target) {
#ifdef _WIN32
    const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \""
                          + target.string() + "\" >nul 2>&1";
    (void)std::system(cmd.c_str());
#else
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
#endif
    // Verify by OBSERVATION, not by the tool's exit code: the point is that a
    // redirection now exists and that the product's own predicate sees it.
    std::error_code ec2;
    if (!std::filesystem::exists(link, ec2)) return false;
    return DocumentSearchIndex::isRedirection(link);
}

class ReparseSweepTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        base_ = std::filesystem::path(tmp_.path().toStdString());
        // The sweep refuses any root not named pdfe_search_indexes, so the
        // fake root must carry the real name.
        root_ = base_ / "pdfe_search_indexes";
        std::filesystem::create_directories(root_);
    }

    void TearDown() override {
        // Remove any redirection we planted BEFORE QTemporaryDir's recursive
        // delete runs, so a test failure cannot let Qt's own cleanup follow a
        // junction into the canary. remove() unlinks the reparse point itself.
        for (const auto& l : planted_) {
            std::error_code ec;
            std::filesystem::remove(l, ec);
        }
    }

    /// An ordinary, stale, unowned index directory — the thing the sweep is
    /// SUPPOSED to delete. Used as the discriminating control in every test.
    std::filesystem::path plantOrdinaryStale(const char* key) const {
        const auto d = root_ / key;
        std::filesystem::create_directories(d);
        std::ofstream(d / "leaked.txt") << "RESTRICTED-PAGE-TEXT-SENTINEL";
        age(d);
        return d;
    }

    /// Backdate `p` well past any threshold the tests use.
    static void age(const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::last_write_time(
            p,
            std::filesystem::file_time_type::clock::now()
                - std::chrono::hours(24 * 30),
            ec);
    }

    /// A canary OUTSIDE the sweep root, holding a sentinel file. This is what
    /// a redirected remove_all() would destroy.
    std::filesystem::path makeCanary(const char* name) const {
        const auto c = base_ / name;
        std::filesystem::create_directories(c);
        std::ofstream(c / "PRECIOUS.txt") << "DO-NOT-DELETE-ME";
        return c;
    }

    std::filesystem::path track(std::filesystem::path link) {
        planted_.push_back(link);
        return link;
    }

    static StaleIndexSweepReport sweep(const std::filesystem::path& root) {
        return DocumentSearchIndex::sweepStaleIndexes(root,
                                                      std::chrono::seconds(1));
    }

    QTemporaryDir tmp_;
    std::filesystem::path base_;
    std::filesystem::path root_;
    std::vector<std::filesystem::path> planted_;
};

} // namespace

// ===========================================================================
// 0. THE CAPABILITY PROBE, asserted. If this environment cannot plant the
//    redirection the rest of the file tests nothing — and says so LOUDLY
//    rather than skipping, which is how a defence like this quietly rots.
// ===========================================================================

TEST_F(ReparseSweepTest, EnvironmentCanPlantADirectoryRedirection) {
    const auto canary = makeCanary("probe_canary");
    const auto link = track(root_ / "probe_link");

    ASSERT_TRUE(makeDirectoryRedirection(link, canary))
        << "Could not create a directory junction/symlink, so the reparse-point "
           "defence cannot be exercised at all. This test FAILS rather than "
           "skipping deliberately: a silent skip would report green for a "
           "security rail nothing had touched. On Windows `mklink /J` needs no "
           "privilege — investigate the environment, do not disable this test.";

    // The planted redirection must look like an ordinary directory to the API
    // the sweep used to rely on. If it did not, the hazard would not exist and
    // these tests would be shadow-boxing.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(link, ec))
        << "precondition: entry.is_directory() FOLLOWS the redirection — that "
           "is why the old sweep enumerated it as a candidate";
#ifdef _WIN32
    EXPECT_FALSE(std::filesystem::is_symlink(std::filesystem::symlink_status(link, ec)))
        << "precondition: MSVC's is_symlink() does NOT report an NTFS junction "
           "(IO_REPARSE_TAG_MOUNT_POINT). If this ever starts reporting true, "
           "the attribute-based check is still correct but this comment is not.";
#endif
}

// ===========================================================================
// 1. THE FINDING ITSELF — a junction planted under the sweep root, passing
//    both pre-existing gates (no lock file, old mtime), must NOT redirect the
//    delete. The canary and its sentinel survive.
// ===========================================================================

TEST_F(ReparseSweepTest, JunctionUnderTheRootDoesNotRedirectTheDelete) {
    const auto canary = makeCanary("canary");
    const auto sentinel = canary / "PRECIOUS.txt";
    const std::string sentinelBefore = readAll(sentinel);
    ASSERT_FALSE(sentinelBefore.empty());

    // Old mtime, no adjacent .pdfe_index_lock: passes gates (a) and (b).
    age(canary);
    const auto link = track(root_ / "redirected_index");
    ASSERT_TRUE(makeDirectoryRedirection(link, canary))
        << "cannot plant the junction — see the capability probe test";

    // DISCRIMINATING CONTROL: an ordinary stale index in the SAME sweep.
    const auto ordinary = plantOrdinaryStale("ordinary_stale");

    const auto report = sweep(root_);

    // The whole point.
    EXPECT_TRUE(std::filesystem::exists(canary))
        << "the sweep followed the junction and deleted a directory OUTSIDE "
           "its root";
    EXPECT_TRUE(std::filesystem::exists(sentinel))
        << "the canary directory survived but its contents did not — a "
           "redirected recursive delete";
    EXPECT_EQ(readAll(sentinel), sentinelBefore)
        << "the sentinel's contents were altered";

    // And the reason is recorded, not merely the outcome: an entry that
    // survived by accident (an unrelated error) would satisfy the assertions
    // above but not this one.
    EXPECT_TRUE(listed(report.skippedRedirection, link))
        << "the junction must be reported as skipped-for-redirection";
    EXPECT_TRUE(listed(report.kept, link));
    EXPECT_FALSE(listed(report.removed, link));

    // CONTROL: the sweep still does its job. Without this, a sweep that had
    // simply stopped deleting anything would pass every assertion above.
    EXPECT_FALSE(std::filesystem::exists(ordinary))
        << "CONTROL: an ordinary stale, unowned index must still be purged — "
           "otherwise this is not a redirection defence, it is a broken sweep";
    EXPECT_TRUE(listed(report.removed, ordinary));
    EXPECT_FALSE(listed(report.skippedRedirection, ordinary))
        << "CONTROL: an ordinary directory must not be misreported as a "
           "redirection";
}

// ===========================================================================
// 2. NESTED — remove_all() RECURSES, so a junction one level inside an
//    otherwise ordinary stale index directory is the same hazard. The
//    per-candidate check cannot see it; the tree scan must.
// ===========================================================================

TEST_F(ReparseSweepTest, JunctionNestedInsideACandidateBlocksTheWholeDelete) {
    const auto canary = makeCanary("nested_canary");
    const auto sentinel = canary / "PRECIOUS.txt";
    const std::string before = readAll(sentinel);

    const auto host = root_ / "host_index";
    std::filesystem::create_directories(host / "sub");
    std::ofstream(host / "leaked.txt") << "RESTRICTED-PAGE-TEXT-SENTINEL";
    const auto link = track(host / "sub" / "escape");
    ASSERT_TRUE(makeDirectoryRedirection(link, canary))
        << "cannot plant the nested junction — see the capability probe test";
    age(host);

    // DISCRIMINATING CONTROL.
    const auto ordinary = plantOrdinaryStale("ordinary_stale_2");

    const auto report = sweep(root_);

    EXPECT_TRUE(std::filesystem::exists(canary));
    EXPECT_TRUE(std::filesystem::exists(sentinel));
    EXPECT_EQ(readAll(sentinel), before);

    EXPECT_TRUE(std::filesystem::exists(host))
        << "a candidate containing a redirection must be kept whole — deleting "
           "'everything except the junction' still means walking a tree that "
           "was rigged";
    EXPECT_TRUE(listed(report.skippedRedirection, host));
    EXPECT_FALSE(listed(report.removed, host));

    EXPECT_FALSE(std::filesystem::exists(ordinary))
        << "CONTROL: an unrigged stale index in the same sweep is still purged";
}

// ===========================================================================
// 3. THE LOCK FILE — it is READ (its pid is parsed) and then DELETED. A lock
//    that is itself a redirection would have both of those aimed elsewhere.
// ===========================================================================

TEST_F(ReparseSweepTest, RedirectedLockFileDisqualifiesTheCandidate) {
    const auto canary = makeCanary("lock_canary");
    const auto sentinel = canary / "PRECIOUS.txt";
    const std::string before = readAll(sentinel);

    const auto dir = root_ / "locked_index";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "leaked.txt") << "RESTRICTED-PAGE-TEXT-SENTINEL";
    age(dir);

    // "<key>.pdfe_index_lock" as a redirection at the canary.
    auto lock = dir;
    lock += DocumentSearchIndex::kLockSuffix;
    ASSERT_TRUE(makeDirectoryRedirection(track(lock), canary))
        << "cannot plant the redirected lock — see the capability probe test";

    const auto ordinary = plantOrdinaryStale("ordinary_stale_3");

    const auto report = sweep(root_);

    EXPECT_TRUE(std::filesystem::exists(canary));
    EXPECT_TRUE(std::filesystem::exists(sentinel));
    EXPECT_EQ(readAll(sentinel), before);

    EXPECT_TRUE(std::filesystem::exists(dir))
        << "a candidate whose lock file is a redirection must be kept: the "
           "pid parse and the lock removal both follow that name";
    EXPECT_TRUE(listed(report.skippedRedirection, dir));
    EXPECT_FALSE(listed(report.removed, dir));

    EXPECT_FALSE(std::filesystem::exists(ordinary))
        << "CONTROL: an ordinary stale index with no lock is still purged";
}

// ===========================================================================
// 4. The predicate itself, addressed directly — it is the whole of the
//    defence, and it must discriminate rather than answer true for everything.
// ===========================================================================

TEST_F(ReparseSweepTest, IsRedirectionDiscriminatesBetweenLinksAndRealEntries) {
    const auto canary = makeCanary("pred_canary");
    const auto link = track(root_ / "pred_link");
    ASSERT_TRUE(makeDirectoryRedirection(link, canary));

    EXPECT_TRUE(DocumentSearchIndex::isRedirection(link));

    // CONTROLS: ordinary things are NOT redirections, or the sweep would keep
    // everything and the purge would be dead.
    const auto plain = plantOrdinaryStale("pred_plain");
    EXPECT_FALSE(DocumentSearchIndex::isRedirection(plain))
        << "CONTROL: an ordinary directory must not be classed as a redirection";
    EXPECT_FALSE(DocumentSearchIndex::isRedirection(plain / "leaked.txt"))
        << "CONTROL: an ordinary file must not be classed as a redirection";
    EXPECT_FALSE(DocumentSearchIndex::isRedirection(root_ / "no_such_entry"))
        << "CONTROL: a path that does not exist is not a redirection — "
           "answering true here would be a keep-everything sweep";

    EXPECT_TRUE(DocumentSearchIndex::treeContainsRedirection(root_))
        << "the tree scan must find the link planted under this root";
    EXPECT_FALSE(DocumentSearchIndex::treeContainsRedirection(plain))
        << "CONTROL: an unrigged tree must scan clean, or nothing is ever "
           "deleted";
}
