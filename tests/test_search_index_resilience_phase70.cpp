// -*- coding: utf-8 -*-
//
// Phase-17 (follow-up phase-70) — WIN-88, and the mutation that survived.
//
// WIN-88 (`windows-qa-gaps.md:2067`): "Search index in the system temp dir is
// untested against external deletion, A/V lock, or a temp-cleaner purge
// mid-session ... assert graceful rebuild rather than crash or
// empty-result-without-error." Re-verified as still open before writing this:
// nothing in `tests/` deleted or locked the index directory mid-session.
//
// It converged with a mutation that SURVIVED phase-70's first harness run:
//
//     "ensureBuilt sets built_ even when buildFromScratch refuses"  -> 0 red
//
// That mutation is not behaviourally neutral, but it looked neutral to every
// assertion I had written. With `built_` set and `index_` null,
// `documentCount()` returns 0 and `searchPages()` returns {} — exactly what the
// fail-closed case already asserted. The difference is INVISIBLE AT THE MOMENT
// OF FAILURE and only appears afterwards: with `built_` false the next search
// retries the build, so removing the obstruction restores search; with `built_`
// true the failure is permanent for the object's lifetime.
//
// So the oracle that kills the mutation is the same one WIN-88 asks for:
// **does search come back once the obstruction goes away?** A fail-closed guard
// that latches is not fail-closed, it is broken — and the only way to tell them
// apart is to remove the obstruction and look again.
//
// Every case below therefore has two halves: the failure, and the RECOVERY.
// Asserting only the failure is what let the mutation through.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "search/DocumentSearchIndex.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::search::DocumentSearchIndex;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

const char* kTerm = "title";

#if defined(_WIN32)
bool plantJunction(const std::filesystem::path& link,
                   const std::filesystem::path& target)
{
    const std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() +
                             L"\" \"" + target.wstring() + L"\" >nul 2>&1";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 30000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    const DWORD attrs = ::GetFileAttributesW(link.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}
#endif

} // namespace

#if defined(_WIN32)

TEST(SearchIndexResilience, SearchRecoversOnceAnUnprotectableDirectoryIsCleared)
{
    // THE CASE THAT KILLS THE SURVIVING MUTATION.
    //
    // Plant a junction so the ACL check refuses; confirm search is closed; then
    // remove the junction and confirm search WORKS AGAIN. The recovery half is
    // the whole point: it is the only observable that distinguishes "refused
    // this time" from "latched off permanently".
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);

    DocumentSearchIndex idx(doc.get());
    const auto dir = idx.indexDir();
    idx.invalidate();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto target =
        std::filesystem::temp_directory_path() / "pdfe_resilience_target";
    std::filesystem::remove_all(target, ec);
    ASSERT_TRUE(std::filesystem::create_directories(target, ec)) << ec.message();
    std::filesystem::create_directories(dir.parent_path(), ec);
    ASSERT_TRUE(plantJunction(dir, target))
        << "could not plant the junction; nothing below is being tested";

    EXPECT_TRUE(idx.searchPages(kTerm, 100).empty())
        << "search succeeded through a directory that could not be secured";

    // Clear the obstruction. Removing the junction unlinks it, not the target.
    ASSERT_TRUE(std::filesystem::remove(dir, ec)) << ec.message();
    ASSERT_EQ(::GetFileAttributesW(dir.wstring().c_str()),
              INVALID_FILE_ATTRIBUTES);

    EXPECT_FALSE(idx.searchPages(kTerm, 100).empty())
        << "search stayed dead after the obstruction was removed. built_ was "
           "latched on a refusal, so the object never retries the build — a "
           "fail-closed guard that latches is not fail-closed, it is broken.";
    EXPECT_GT(idx.documentCount(), 0);

    std::filesystem::remove_all(target, ec);
}

TEST(SearchIndexResilience, IndexDirectoryDeletedMidSessionDoesNotCrashAndRecovers)
{
    // WIN-88 proper: a temp cleaner, an A/V quarantine or a user with a broom
    // removes the index directory while the session is live.
    //
    // The gap row's acceptance is "graceful rebuild rather than crash or
    // empty-result-without-error". Both failure modes are asserted, and the
    // recovery is asserted through invalidate(), which is the product's own
    // documented way of saying "the on-disk state is no longer trustworthy".
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);

    DocumentSearchIndex idx(doc.get());
    idx.invalidate();
    ASSERT_FALSE(idx.searchPages(kTerm, 100).empty())
        << "no baseline hits, so the deletion below removes nothing that was "
           "working and the case proves nothing";
    const auto dir = idx.indexDir();
    ASSERT_TRUE(std::filesystem::is_directory(dir));

    // Best-effort deletion. Tantivy holds files open, and on Windows an open
    // file without FILE_SHARE_DELETE cannot be unlinked — so a partial removal
    // is the REALISTIC outcome of a temp cleaner meeting a live index, and it
    // is the more interesting state anyway: a half-deleted index directory.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Deliberately NO assertion on the result here.
    //
    // Whether a partially-deleted index still answers is not a property worth
    // pinning — it depends on which files Tantivy had open. The no-crash oracle
    // is structural: a crash fails the shard. Writing `EXPECT_TRUE(x || !x)` to
    // express "we reached this line" is a tautology, and the project's own
    // OracleStandard.NoSelfNegatingTautologies guard caught exactly that here on
    // the first evidence run. The count is recorded instead, so the behaviour is
    // observable in the XML without being asserted.
    const auto during = idx.searchPages(kTerm, 100);
    RecordProperty("hits_while_index_partially_deleted",
                   static_cast<int>(during.size()));

    // And the session must be recoverable rather than permanently degraded.
    idx.invalidate();
    EXPECT_FALSE(idx.searchPages(kTerm, 100).empty())
        << "search did not come back after invalidate() following an external "
           "deletion — an index that vanishes mid-session leaves the feature "
           "dead for the rest of the session, which is WIN-88's exact concern";
    EXPECT_GT(idx.documentCount(), 0);
}

TEST(SearchIndexResilience, RepeatedRefusalsDoNotAccumulateStateThatBlocksRecovery)
{
    // The control for the two cases above: recovery must not be a one-shot.
    // A guard that recovers once but latches on the second refusal would pass
    // both cases above and still strand a real session.
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);

    DocumentSearchIndex idx(doc.get());
    const auto dir = idx.indexDir();
    const auto target =
        std::filesystem::temp_directory_path() / "pdfe_resilience_target2";
    std::error_code ec;

    for (int round = 0; round < 3; ++round) {
        idx.invalidate();
        std::filesystem::remove_all(dir, ec);
        std::filesystem::remove_all(target, ec);
        ASSERT_TRUE(std::filesystem::create_directories(target, ec));
        std::filesystem::create_directories(dir.parent_path(), ec);
        ASSERT_TRUE(plantJunction(dir, target)) << "round " << round;

        EXPECT_TRUE(idx.searchPages(kTerm, 100).empty())
            << "round " << round << ": search succeeded through a junction";

        ASSERT_TRUE(std::filesystem::remove(dir, ec)) << "round " << round;
        EXPECT_FALSE(idx.searchPages(kTerm, 100).empty())
            << "round " << round << ": search did not recover";
    }

    std::filesystem::remove_all(target, ec);
}

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 3 — the delete itself, not the scan in front of it
//
// Added after the phase-17 `validator` gate FAILED this criterion. The gate was
// right: the criterion says the check-then-act window must be hardened "so it
// cannot be raced", and the phase had only NARROWED who could win the race (via
// the owner-only ACL) while leaving `remove_all()` behind a separate scan.
//
// `removeTreeUnraceable` closes it by acting on the object it inspected: every
// entry is opened with FILE_FLAG_OPEN_REPARSE_POINT, classified from the
// HANDLE, matched by FILE ID against the id the directory enumeration reported,
// and deleted BY HANDLE.
//
// These cases exercise the DELETE DIRECTLY. Going through sweepStaleIndexes()
// cannot reach it — the sweep's redirection scan refuses such a candidate
// first — so the delete's safety property would otherwise be untestable, and a
// guard nobody can exercise is a guard nobody can show works.
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchIndexUnraceableDelete, AJunctionInsideTheTreeIsUnlinkedAndItsTargetSurvives)
{
    const auto base = std::filesystem::temp_directory_path() / "pdfe_unraceable";
    const auto tree = base / "candidate";
    const auto victim = base / "victim";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(tree / "sub", ec));
    ASSERT_TRUE(std::filesystem::create_directories(victim, ec));

    // Something precious behind the junction. If the delete ever descends, this
    // is what it destroys — so its survival is the assertion that matters.
    { std::ofstream f(victim / "precious.txt"); f << "must survive"; }
    { std::ofstream f(tree / "ordinary.txt"); f << "delete me"; }

    ASSERT_TRUE(plantJunction(tree / "sub" / "link", victim))
        << "could not plant the junction; nothing below is being tested";

    const bool removed =
        DocumentSearchIndex::removeTreeUnraceable(tree);

    // Either outcome is acceptable for the TREE — refusing is the safe
    // direction and the contract says so. What is NOT acceptable is the victim
    // losing anything.
    EXPECT_TRUE(std::filesystem::exists(victim))
        << "the delete followed the junction and removed its target directory";
    EXPECT_TRUE(std::filesystem::exists(victim / "precious.txt"))
        << "the delete descended into the junction and destroyed the file "
           "behind it — the link was traversed instead of unlinked";

    if (removed) {
        EXPECT_FALSE(std::filesystem::exists(tree))
            << "reported success but the tree is still there";
    }

    std::filesystem::remove_all(base, ec);
}

TEST(SearchIndexUnraceableDelete, AnOrdinaryTreeIsFullyRemoved)
{
    // The control. A delete that refused everything would satisfy the case
    // above while making the sweep useless — stale plaintext would never be
    // purged, which is the whole point of F24.
    const auto base = std::filesystem::temp_directory_path() / "pdfe_unraceable2";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(base / "a" / "b" / "c", ec));
    { std::ofstream f(base / "a" / "one.txt"); f << "x"; }
    { std::ofstream f(base / "a" / "b" / "two.txt"); f << "y"; }
    { std::ofstream f(base / "a" / "b" / "c" / "three.txt"); f << "z"; }

    EXPECT_TRUE(DocumentSearchIndex::removeTreeUnraceable(base))
        << "an ordinary nested tree could not be removed; the sweep would now "
           "never purge a stale index";
    EXPECT_FALSE(std::filesystem::exists(base));
}

TEST(SearchIndexUnraceableDelete, AReparsePointAtTheROOTOfTheWalkIsRefusedNotFollowed)
{
    // A candidate that IS a junction must be refused outright rather than
    // unlinked as routine cleanup: the caller enumerated it as an index
    // directory, and one that turns out to be a link is a finding.
    const auto base = std::filesystem::temp_directory_path() / "pdfe_unraceable3";
    const auto link = base / "candidate";
    const auto victim = base / "victim";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    ASSERT_TRUE(std::filesystem::create_directories(victim, ec));
    std::filesystem::create_directories(base, ec);
    { std::ofstream f(victim / "precious.txt"); f << "must survive"; }
    ASSERT_TRUE(plantJunction(link, victim));

    EXPECT_FALSE(DocumentSearchIndex::removeTreeUnraceable(link))
        << "a junction handed in as the walk root was accepted";
    EXPECT_TRUE(std::filesystem::exists(victim / "precious.txt"))
        << "the target behind the root junction was destroyed";

    std::filesystem::remove(link, ec);
    std::filesystem::remove_all(base, ec);
}
