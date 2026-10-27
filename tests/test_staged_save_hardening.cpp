// -*- coding: utf-8 -*-
// ===========================================================================
// Meta-audit follow-up F58-a / F58-b / F58-d / F58-f — phase-53.
//
// WHAT THIS FILE MEASURES, AND WHY IT IS NOT THE SAME QUESTION F58 ASKED.
//
// F58 (phase-52) asked "can a mid-write failure destroy the user's signed
// document?" and answered it by staging the append. tests/test_dss_mid_append_kill
// .cpp is the control for that, and it still passes. This file asks the question
// the adversarial review of that work asked instead:
//
//     the staging file is now a NEW file, at a predictable path, in the user's
//     directory, with no handle held on it — is it easier to attack than the
//     document it protects?
//
// It was. Both halves were demonstrated end-to-end, not theorised:
//
//   * F58-a (CRITICAL). A separate unprivileged process polled for
//     "<target>.pdfe_tmp" and stamped 64 bytes over offset 0 — a region MuPDF
//     had already passed, because an incremental append seeks to EOF. PDFE
//     atomically published the attacker's bytes over the user's signed document
//     and LtvDssWriter::write() returned TRUE. The published file began
//     "PWNED-BY-CONCURRENT-PROCESS-…" and was not a PDF.
//
//   * F58-b (HIGH). CopyFileW does not copy the security descriptor, so the
//     staged copy took the DIRECTORY's inheritable ACEs and MoveFileExW carried
//     them onto the destination. An owner-only document in a folder granting
//     Users:(OI)(CI)(M) came out of ONE save as Users:(I)(M) — permanently.
//     That is not pre-existing: the old in-place append never created a file, so
//     it preserved the ACL exactly. It also downgraded F58-a from "needs
//     directory write" to "needs nothing at all, on the second save".
//
// THE ORACLES ARE THE ATTACKER'S OWN INSTRUMENTS, NOT A PROXY. The TOCTOU test
// runs a real concurrent writer against a real save and asserts (a) that it SAW
// the staging file — so a green result cannot come from "the window never
// happened" — and (b) that every attempt to open it for WRITING was refused. The
// ACL test compares SDDL strings of the same file before and after, so it cannot
// pass by measuring something other than the published permissions.
// ===========================================================================

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/AtomicFileReplace.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "support/SaveFailureInjection.h"

#include <QRectF>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

// F65 (follow-up phase-67). These three helpers used to be defined AGAIN in
// this file, byte-for-byte alongside the copies in tests/support/. Two
// implementations of "read/apply a DACL" in one suite is the sibling-drift
// shape DCA 0015 records — and they had already drifted in TEXT
// (`static_cast<size_t>(n - 1)` here vs a bare `n - 1` there), which is what
// made the duplication visible. See the disposition for why that particular
// drift was harmless and why that does not make the duplication harmless.
#if defined(_WIN32)
using pdfe::testing::applyProtectedDacl;
using pdfe::testing::currentUserSidString;
using pdfe::testing::daclSddl;
#endif

using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::StagingFile;

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

/// A private working directory for one test.
std::filesystem::path makeWorkDir(const char* tag)
{
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path()
             / (std::string("pdfe_f58h_") + tag + "_"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(d, ec);
    return d;
}

/// The source document. Deliberately one of the larger real-world fixtures: the
/// staging window is proportional to how long the copy-plus-append takes, and a
/// 3 KB document would give the concurrent writer so little time that "it never
/// got in" could be mistaken for "it was refused". The test asserts the writer
/// SAW the file, so a too-small window fails loudly rather than passing falsely —
/// but there is no reason to make it fight for the observation.
std::filesystem::path sourceFixture()
{
    for (const char* rel : {"realworld/irs_i1040.pdf", "realworld/irs_p17.pdf",
                            "realworld/arxiv_attention.pdf", "test_3page.pdf"}) {
        const auto p = fixtures() / rel;
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    return {};
}

/// One purely ADDITIVE edit — the edit class the incremental-append path exists
/// for. Returns false if the document could not take one.
bool addAnnotation(const std::shared_ptr<MuPdfContext>& ctx, Document* doc)
{
    try {
        AnnotationManager am(ctx);
        auto page = doc->page(0);
        if (!page) return false;
        am.createAnnotation(page.get(), AnnotationType::Square,
                            QRectF(50, 50, 120, 60));
        return true;
    } catch (...) {
        return false;
    }
}

/// Every sibling of `target` that is one of OUR staging files.
std::vector<std::filesystem::path> stagingResidue(
    const std::filesystem::path& target)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(target.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (StagingFile::isStagingResidueName(target.filename(),
                                              it->path().filename())) {
            out.push_back(it->path());
        }
    }
    return out;
}

#if defined(_WIN32)




/// Give `dir` an inheritable ACE granting BUILTIN\Users Modify — the permissive
/// containing folder half of the F58-b scenario. The test user keeps full
/// control explicitly so the directory stays usable whatever groups it is in.
bool grantUsersModifyInheritable(const std::filesystem::path& dir,
                                 const std::string& userSid)
{
    const std::wstring me(userSid.begin(), userSid.end());
    const std::wstring sddl =
        L"D:(A;OICI;FA;;;" + me + L")"      // the test user, full
        L"(A;OICI;FA;;;SY)"                 // SYSTEM, full
        L"(A;OICI;0x1301bf;;;BU)";          // BUILTIN\Users, Modify, inheritable
    return applyProtectedDacl(dir, sddl);
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// F58-a — the exploit, re-run against the fix.
// ===========================================================================
//
// THE ATTACKER. A thread standing in for the separate unprivileged process the
// review used (a thread is if anything a HARDER test: it polls in the same
// address space with no process-launch latency). It does exactly what the
// original exploit did — enumerate the target's directory for any new sibling,
// open it for WRITING, stamp a marker over offset 0 — and it records both what
// it saw and what it managed to open.
//
// WHY BOTH COUNTERS MATTER. `sawStaging` is what stops this test passing for the
// wrong reason: without it, a save that never staged at all, or one whose window
// closed before the poller looked, would produce "no tamper" just as loudly as a
// correct fix. With it, the test asserts the attacker was PRESENT AT THE SCENE
// and was REFUSED.
//
// KILLS: reverting to std::filesystem::copy_file + "<target>.pdfe_tmp" with no
// handle held (`openedForWrite` becomes non-zero and the published prefix stops
// matching the original); dropping CREATE_NEW; widening dwShareMode to include
// FILE_SHARE_WRITE or FILE_SHARE_DELETE; publishing by path instead of through
// the handle (re-opens the name, re-opening the window).
TEST(StagedSaveTocttou, ConcurrentWriterCannotTamperWithTheStagedBytes)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty()) << "no usable source fixture";

    const auto dir = makeWorkDir("tocttou");
    const auto target = dir / "victim.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::string before = readAll(target);
    ASSERT_FALSE(before.empty());

    std::atomic<bool> stop{false};
    std::atomic<int> sawStaging{0};
    std::atomic<int> openedForWrite{0};
    std::atomic<int> tamperedBytes{0};
    std::string firstStagingName;
    std::mutex nameMutex;

    std::thread attacker([&] {
        static const char kPayload[] =
            "PWNED-BY-CONCURRENT-PROCESS-0123456789ABCDEF0123456789ABCDEF";
        while (!stop.load(std::memory_order_relaxed)) {
            std::error_code it_ec;
            for (std::filesystem::directory_iterator it(dir, it_ec), end;
                 !it_ec && it != end; it.increment(it_ec)) {
                const auto name = it->path().filename();
                if (name == target.filename()) {
                    continue;
                }
                // Anything that is not the victim itself is a staging artifact.
                // The attacker does NOT get to know the naming scheme — this is
                // the strongest form of the original attack, which only had to
                // guess "<target>.pdfe_tmp".
                sawStaging.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(nameMutex);
                    if (firstStagingName.empty()) {
                        firstStagingName = name.string();
                    }
                }
                HANDLE h = ::CreateFileW(
                    it->path().native().c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    openedForWrite.fetch_add(1, std::memory_order_relaxed);
                    DWORD wrote = 0;
                    ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
                    if (::WriteFile(h, kPayload,
                                    static_cast<DWORD>(sizeof(kPayload) - 1),
                                    &wrote, nullptr)) {
                        tamperedBytes.fetch_add(static_cast<int>(wrote),
                                                std::memory_order_relaxed);
                    }
                    ::FlushFileBuffers(h);
                    ::CloseHandle(h);
                }
            }
        }
    });

    bool saveThrew = false;
    std::string saveError;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(addAnnotation(ctx, doc.get()))
            << "could not make an additive edit on the fixture";
        try {
            doc->saveIncrementalStaged();
        } catch (const std::exception& e) {
            saveThrew = true;
            saveError = e.what();
        }
    }
    stop.store(true, std::memory_order_relaxed);
    attacker.join();

    EXPECT_FALSE(saveThrew) << "the staged save failed: " << saveError;

    // (a) The attacker was present at the scene.
    EXPECT_GT(sawStaging.load(), 0)
        << "the concurrent writer never observed a staging file, so NOTHING was "
           "attacked and a clean result here would prove nothing. Either the "
           "save did not stage, or the window closed before the poller looked.";

    // (b) …and was refused, every time.
    EXPECT_EQ(openedForWrite.load(), 0)
        << "a concurrent unprivileged writer OPENED the staging file for "
           "writing. That is the F58-a exploit: the next step is stamping bytes "
           "over a region the writer has already passed, and PDFE then "
           "atomically publishes them over the user's signed document and "
           "reports success. Staging file seen: " << firstStagingName;
    EXPECT_EQ(tamperedBytes.load(), 0)
        << "bytes were written into the staged copy by another writer";

    // (c) The published document really is the original plus an append.
    const std::string after = readAll(target);
    ASSERT_GT(after.size(), before.size())
        << "the save reported success but appended nothing, so (b) proves "
           "nothing";
    EXPECT_EQ(after.compare(0, before.size(), before), 0)
        << "the published document does not start with the original bytes — "
           "either the append landed on a tampered copy or the prefix guarantee "
           "is broken";
    EXPECT_EQ(after.compare(0, 5, "%PDF-"), 0)
        << "the published file is not a PDF";

    // (d) No residue.
    EXPECT_TRUE(stagingResidue(target).empty())
        << "an uninterrupted staged save must leave no staging file behind";

    std::filesystem::remove_all(dir, ec);
}

// The name itself is part of the fix, and it is also the F58-d contract: a
// staged APPEND residue must never be confusable with a full-rewrite temp,
// because the two mean opposite things to crash recovery (possibly truncated vs
// complete).
//
// KILLS: reverting to the fixed "<target>.pdfe_tmp"; seeding the suffix from
// anything with fewer than 128 bits; reusing one name across saves.
TEST(StagedSaveTocttou, StagingNameIsUnpredictableAndNotTheRewriteTempName)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("names");
    const auto target = dir / "names.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    // The FULL-REWRITE name, deliberately: this test proves the STAGED path
    // never uses it. Matching with the residue predicate instead would assert
    // the opposite of the intent and could never fail.
    const auto rewriteTemp = pdfe::core::fullRewriteTempPathFor(target);

    std::atomic<bool> stop{false};
    std::vector<std::string> observed;
    std::mutex m;
    std::atomic<int> rewriteTempSeen{0};

    std::thread watcher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::error_code it_ec;
            for (std::filesystem::directory_iterator it(dir, it_ec), end;
                 !it_ec && it != end; it.increment(it_ec)) {
                const auto name = it->path().filename().string();
                if (name == target.filename().string()) continue;
                if (name == rewriteTemp.filename().string()) {
                    rewriteTempSeen.fetch_add(1, std::memory_order_relaxed);
                }
                std::lock_guard<std::mutex> lk(m);
                if (std::find(observed.begin(), observed.end(), name)
                    == observed.end()) {
                    observed.push_back(name);
                }
            }
        }
    });

    // Two saves, so the "one fixed name" mutation is caught as well.
    for (int i = 0; i < 2; ++i) {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(addAnnotation(ctx, doc.get()));
        ASSERT_NO_THROW(doc->saveIncrementalStaged());
    }
    stop.store(true, std::memory_order_relaxed);
    watcher.join();

    ASSERT_FALSE(observed.empty())
        << "no staging file was ever observed, so this test measured nothing";
    EXPECT_EQ(rewriteTempSeen.load(), 0)
        << "the staged incremental append used the FULL-REWRITE temp name "
           "\"<target>.pdfe_tmp\" — predictable, and indistinguishable from the "
           "complete-file artifact crash recovery is allowed to offer a user "
           "(F58-a + F58-d)";

    for (const auto& name : observed) {
        EXPECT_TRUE(StagingFile::isStagingResidueName(
            target.filename(), std::filesystem::path(name)))
            << "unexpected sibling created during a staged save: " << name;
    }
    // Unconditional, and this is the point: an implementation with ONE fixed
    // staging name produces exactly one observed name, so a
    // `if (observed.size() >= 2)` guard around the inequality would let that
    // mutation through in silence. Two saves must yield two names.
    ASSERT_GE(observed.size(), 2u)
        << "two consecutive staged saves produced only " << observed.size()
        << " distinct staging name(s) (" << observed.front()
        << ") — the name is a constant, not drawn fresh from the CSPRNG, and "
           "is therefore predictable to an attacker polling the directory";
    EXPECT_NE(observed[0], observed[1]);

    std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// F58-b — the published document must not come out with a wider DACL.
// ===========================================================================
//
// THE SCENARIO, exactly as measured in the review: an owner-only document
// (inheritance broken, one ACE granting the owner) inside a folder that grants
// BUILTIN\Users Modify with (OI)(CI). Before the fix, one staged save published
// a file carrying the folder's inherited Users:(I)(M).
//
// THE ORACLE is the SDDL of the same path before and after — not "does it still
// open", not "is the owner still there".
//
// KILLS: removing the preserveDestinationDacl() call from atomicReplaceFile /
// StagingFile::commitOver; applying the DACL without
// PROTECTED_DACL_SECURITY_INFORMATION (the inherited Users ACE would come back);
// applying it AFTER the rename to the wrong path.
TEST(StagedSaveDacl, PublishedDocumentKeepsTheOriginalProtectedDacl)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());
    const std::string me = currentUserSidString();
    ASSERT_FALSE(me.empty()) << "could not resolve the current user SID";

    const auto dir = makeWorkDir("dacl");
    ASSERT_TRUE(grantUsersModifyInheritable(dir, me))
        << "could not make the containing folder permissive; without that this "
           "test cannot reproduce the widening it exists to catch";

    const auto target = dir / "ownersonly.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    // Lock it to the owner alone, inheritance broken.
    const std::wstring meW = std::wstring(me.begin(), me.end());
    ASSERT_TRUE(applyProtectedDacl(target, L"D:(A;;FA;;;" + meW + L")"))
        << "could not lock the document to owner-only";

    const std::string beforeSddl = daclSddl(target);
    ASSERT_NE(beforeSddl.find("(A;;FA;;;" + me + ")"), std::string::npos)
        << "the owner-only DACL did not take: " << beforeSddl;

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(addAnnotation(ctx, doc.get()));
        ASSERT_NO_THROW(doc->saveIncrementalStaged());
    }

    const std::string afterSddl = daclSddl(target);
    EXPECT_EQ(beforeSddl, afterSddl)
        << "\n=== ORIGINAL DACL === " << beforeSddl
        << "\n=== PUBLISHED DACL === " << afterSddl
        << "\nOne /DSS-shaped save WIDENED the document's permissions. "
           "CopyFileW does not copy the security descriptor, so the staged copy "
           "took the folder's inheritable ACEs and the rename carried them onto "
           "the destination (F58-b).";
    // Say the failure in the specific terms of the measured incident too.
    EXPECT_EQ(afterSddl.find(";;;BU)"), std::string::npos)
        << "the published document now grants BUILTIN\\Users: " << afterSddl;

    std::filesystem::remove_all(dir, ec);
}

// The same widening applies to every OTHER stage-and-rename site, because they
// all commit through atomicReplaceFileWithRetry(). This test proves the fix went
// into the SHARED helper rather than only into the signature path: it drives
// Document::save()'s full-rewrite branch, which never touches
// saveIncrementalStaged().
//
// KILLS: fixing the DACL only inside StagingFile::commitOver.
TEST(StagedSaveDacl, FullRewriteSaveAlsoKeepsTheOriginalProtectedDacl)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());
    const std::string me = currentUserSidString();
    ASSERT_FALSE(me.empty());

    const auto dir = makeWorkDir("daclrw");
    ASSERT_TRUE(grantUsersModifyInheritable(dir, me));

    const auto target = dir / "ownersonly_rw.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    const std::wstring meW = std::wstring(me.begin(), me.end());
    ASSERT_TRUE(applyProtectedDacl(target, L"D:(A;;FA;;;" + meW + L")"));
    const std::string beforeSddl = daclSddl(target);

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        // markRequiresFullRewrite() forces save() off the incremental branch and
        // onto the stage-then-atomicReplaceFileWithRetry() branch.
        doc->markRequiresFullRewrite();
        ASSERT_TRUE(addAnnotation(ctx, doc.get()));
        ASSERT_NO_THROW(doc->save(target));
    }

    const std::string afterSddl = daclSddl(target);
    EXPECT_EQ(beforeSddl, afterSddl)
        << "\n=== ORIGINAL DACL === " << beforeSddl
        << "\n=== PUBLISHED DACL === " << afterSddl
        << "\nDocument::save()'s full-rewrite branch widened the document's "
           "permissions. Every stage-and-rename site in the product commits "
           "through atomicReplaceFileWithRetry(), so the fix belongs there.";

    std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// F58-f — a failed re-open must not leave a lying Document, and must not be
// reported as a failed save.
// ===========================================================================
//
// HOW THE RE-OPEN IS MADE TO FAIL DETERMINISTICALLY. Not by racing: a helper
// thread waits until the staging file has actually GROWN past the original's
// size — i.e. the copy is done and the append is under way — and only then
// applies a PROTECTED DACL to the target that DENIES the current user
// FILE_GENERIC_READ while still allowing everything else. That is a legitimate
// concurrent permissions change, and it is observed rather than timed.
//
// The rest follows mechanically: commitOver() carries that DACL onto the staging
// file (F58-b) and publishes it, so the revision IS on disk and correct — and
// fz_open_document() on it then fails with access denied, which is precisely the
// shape an A/V scanner or the search indexer produces on a just-renamed file.
//
// (Windows always grants the OWNER READ_CONTROL and WRITE_DAC regardless of the
// DACL, which is what lets the DACL be read for preservation and restored for
// cleanup while FILE_READ_DATA stays denied.)
//
// KILLS: removing the `pImpl->cachedPageCount = 0` when the handle is dropped;
// removing the null-handle guards from pageCount()/page(); removing the
// try/catch that distinguishes "publish failed" from "published, re-open
// failed".
TEST(StagedSaveReopen, PublishedButReopenFailedIsDistinctAndLeavesNoLyingDocument)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());
    const std::string me = currentUserSidString();
    ASSERT_FALSE(me.empty());
    const std::wstring meW = std::wstring(me.begin(), me.end());

    const auto dir = makeWorkDir("reopen");
    const auto target = dir / "reopen.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);
    const std::string before = readAll(target);
    const auto originalSize = static_cast<std::uintmax_t>(before.size());

    std::atomic<bool> stop{false};
    std::atomic<bool> flipped{false};
    std::thread flipper([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (const auto& s : stagingResidue(target)) {
                std::error_code se;
                const auto sz = std::filesystem::file_size(s, se);
                if (!se && sz > originalSize) {
                    // Deny read, allow everything else, inheritance broken.
                    // The DENY ace is ordered first, as Windows evaluates.
                    if (applyProtectedDacl(
                            target,
                            L"D:(D;;FR;;;" + meW + L")(A;;FA;;;" + meW + L")")) {
                        flipped.store(true, std::memory_order_relaxed);
                    }
                    return;
                }
            }
        }
    });

    int pageCountAfter = -1;
    bool pageThrew = false;
    std::string pageError;
    bool threw = false;
    std::string message;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        const int pagesBefore = doc->pageCount();
        ASSERT_GT(pagesBefore, 0);
        ASSERT_TRUE(addAnnotation(ctx, doc.get()));

        bool reloaded = false;
        bool publishedButReopenFailed = false;
        try {
            doc->saveIncrementalStaged(&reloaded, &publishedButReopenFailed);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        stop.store(true, std::memory_order_relaxed);
        flipper.join();

        ASSERT_TRUE(flipped.load())
            << "the permissions flip never fired, so the re-open never failed "
               "and this test measured nothing";
        ASSERT_TRUE(threw) << "the re-open was expected to fail";

        EXPECT_TRUE(publishedButReopenFailed)
            << "the revision WAS published (the rename went through) but the "
               "caller was told nothing to distinguish that from a failed save. "
               "LtvDssWriter::write() then reports 'the /DSS upgrade failed' "
               "about an upgrade that succeeded. Message was: " << message;
        EXPECT_NE(message.find("WAS published"), std::string::npos)
            << "the exception text does not say the save succeeded: " << message;
        EXPECT_TRUE(reloaded);

        // The zombie-Document half.
        pageCountAfter = doc->pageCount();
        try {
            auto p = doc->page(0);
            (void)p;
        } catch (const std::exception& e) {
            pageThrew = true;
            pageError = e.what();
        }
    }
    // Restore access so the directory can be cleaned up.
    applyProtectedDacl(target, L"D:(A;;FA;;;" + meW + L")");

    EXPECT_EQ(pageCountAfter, 0)
        << "pageCount() still reports " << pageCountAfter
        << " pages for a Document whose fz_document handle was dropped and "
           "never re-opened — a stale cached int describing a document that no "
           "longer exists (F58-f)";
    // PREMISE CORRECTION (phase-53). The review called this a null dereference:
    // "page(int) range-checks against the stale count and calls
    // fz_load_page(ctx, nullptr, index) — a null deref". It is not. MuPDF 1.26.10
    // is defensive on that path — fz_count_chapters/fz_count_chapter_pages both
    // test `doc &&` and fall through, so fz_load_page(ctx, NULL, i) reaches
    // `fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid page number: %d")`. So asserting
    // only "it threw" is DECORATION: it is true with the guard and true without.
    //
    // The real, mutation-sensitive defect is that the diagnosis is wrong. Without
    // the guard the caller is told "invalid page number", which sends whoever
    // debugs it hunting an index bug in their own code; the actual cause is that
    // the Document has no open handle because a save dropped it and the re-open
    // failed. Assert the message names that.
    EXPECT_TRUE(pageThrew) << "page(0) did not fail at all on a handle-less "
                              "Document";
    EXPECT_NE(pageError.find("no open handle"), std::string::npos)
        << "page(0) failed, but blamed the wrong thing: \"" << pageError
        << "\". With the null-handle guard removed the range check passes "
           "against a stale cached count and MuPDF reports \"invalid page "
           "number\" — a diagnosis that points at the caller's index instead of "
           "at the dropped handle (F58-f). drainRebindAroundWrite does exactly "
           "pageCount() then page(idx).";

    // The published bytes really are correct — that is what makes reporting a
    // plain failure wrong.
    const std::string after = readAll(target);
    EXPECT_GT(after.size(), before.size());
    EXPECT_EQ(after.compare(0, before.size(), before), 0);

    std::filesystem::remove_all(dir, ec);
}

#endif  // _WIN32

// ===========================================================================
// F58-d — the crash-recovery contract must not offer a truncated append.
// ===========================================================================
//
// Document's constructor recorded ANY regular file at "<filePath>.pdfe_tmp" into
// pendingRecoveredTemp_, under a comment asserting it was "a COMPLETE,
// pdf_save_document-finished file". There was no validation of any kind — not
// size, not "%PDF-", not startxref. A real TerminateProcess mid-append left a
// 46 MB residue whose only startxref is the ORIGINAL's; offered as "your
// unsaved changes", it opens as the PRE-save document with 46 MB of junk past
// the last %%EOF — for a signed document, the classic tampering shape.
//
// WHAT THE FIX IS, AND WHY IT IS NOT "STOP SURFACING IT". The review asked for
// the orphan to be validated BEFORE being recorded. That would have broken two
// pre-existing controls that deliberately require the opposite —
// DocumentSaveAtomicTest.OrphanTempRecoveryDetectedWithoutClobber and
// SaveFailureInjectionTest.TruncatedOrphanTempIsSurfacedNotApplied both demand
// that residue be shown to the user rather than silently hidden — and silently
// dropping a user's only copy of a half-written save is its own data-loss bug.
// So surfacing is unchanged and the FALSE PROMISE is what got fixed:
// pendingRecoveredTempIsComplete() now answers the question the old comment
// merely asserted.
//
// KILLS: removing orphanTempLooksComplete(); weakening it to an existence check;
// hard-coding it to true.
TEST(OrphanTempValidation, TruncatedRewriteTempIsReportedAsIncomplete)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("orphan");
    const auto target = dir / "orphan.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    const std::string whole = readAll(target);
    ASSERT_GT(whole.size(), 4096u);

    // The FULL-REWRITE name: this test plants both a truncated and a complete
    // one and pins how the R2-04 recovery scan reports each. It must be the
    // recoverable shape — a ".pdfe_part.<hex>" would be swept, not surfaced,
    // which is the distinction under test.
    const auto temp = pdfe::core::fullRewriteTempPathFor(target);

    // (1) A TRUNCATED temp — a real half-written save — must be surfaced but
    //     must NOT be described as complete.
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(whole.data(), static_cast<std::streamsize>(whole.size() / 2));
    }
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        // Surfaced — that contract is unchanged and other controls depend on it.
        ASSERT_TRUE(doc->pendingRecoveredTemp().has_value());
        // …but NOT as a complete document.
        EXPECT_FALSE(doc->pendingRecoveredTempIsComplete())
            << "a truncated .pdfe_tmp (no %%EOF after the last startxref) is "
               "reported as a COMPLETE, finished save — that is the false "
               "promise a recovery UI would act on (F58-d)";
    }

    // (2) A COMPLETE one still is — otherwise (1) would pass by the validator
    //     simply rejecting everything, and R2-04's recovery contract would be
    //     silently dead.
    {
        // A real complete save, produced the same way a crashed one would have
        // been: full-rewrite into the temp path, then leave it there.
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(addAnnotation(ctx, doc.get()));
        doc->markRequiresFullRewrite();
        ASSERT_NO_THROW(doc->saveAs(temp));
    }
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(doc->pendingRecoveredTemp().has_value())
            << "a COMPLETE .pdfe_tmp is no longer surfaced — the R2-04 recovery "
               "contract has been broken, and test (1) above now proves nothing";
        EXPECT_EQ(*doc->pendingRecoveredTemp(), temp);
        EXPECT_TRUE(doc->pendingRecoveredTempIsComplete())
            << "a genuinely finished save is reported as incomplete — the "
               "validator rejects everything, so (1) passes for the wrong "
               "reason and R2-04 recovery is silently dead";
    }

    std::filesystem::remove_all(dir, ec);
}

// A staged-append residue is a DIFFERENT kind of artifact and must never reach
// the recovery path at all — that is what the distinguishable suffix buys.
//
// KILLS: giving staged appends the ".pdfe_tmp" suffix; dropping the sweep.
TEST(OrphanTempValidation, StagedAppendResidueIsNeverOfferedForRecovery)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());

    const auto dir = makeWorkDir("residue");
    const auto target = dir / "residue.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    // Exactly what a TerminateProcess mid-append leaves: the original bytes plus
    // a partial revision, under a staging name.
    const std::string whole = readAll(target);
    auto residue = target;
    residue += std::string(StagingFile::kSuffixMarker)
             + "0123456789abcdef0123456789abcdef";
    {
        std::ofstream out(residue, std::ios::binary | std::ios::trunc);
        out.write(whole.data(), static_cast<std::streamsize>(whole.size()));
        const std::string partial(4096, 'X');
        out.write(partial.data(), static_cast<std::streamsize>(partial.size()));
    }
    ASSERT_TRUE(std::filesystem::exists(residue));

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        EXPECT_FALSE(doc->pendingRecoveredTemp().has_value())
            << "a possibly-truncated staged APPEND residue was offered to the "
               "user as a recoverable complete save (F58-d)";
    }
    EXPECT_FALSE(std::filesystem::exists(residue))
        << "the staged-append residue was neither recovered nor swept — a 46 MB "
           "orphan per killed save accumulates in the user's folder";

    std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// F58-c — the measurement behind Document::save()'s incremental routing.
// ===========================================================================
//
// Not a pass/fail gate: a MEASUREMENT, printed, so the routing decision recorded
// in Document.h rests on numbers from this machine rather than on an estimate.
// It times the same additive edit saved through the unstaged in-place append and
// through the staged one, on the largest XFA fixture in the corpus.
//
// Gated on PDFE_RUN_SLOW_SAVE_TESTS for the same reason
// IncrementalSaveRoutingTest.FormFillSaveIsFastOnXfa is: it is a multi-second
// real save of an 8 MB document, not a unit test.
TEST(StagedIncrementalXfaBenchmark, MeasureUnstagedVersusStagedAppend)
{
    const char* v = std::getenv("PDFE_RUN_SLOW_SAVE_TESTS");
    if (!v || std::string(v) == "0") {
        GTEST_SKIP() << "set PDFE_RUN_SLOW_SAVE_TESTS=1 to run the XFA "
                        "staged-vs-unstaged append measurement";
    }
    const auto fixture = fixtures() / "realworld" / "ssa_sf86.pdf";
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "ssa_sf86.pdf fixture not available";
    }

    const auto dir = makeWorkDir("xfabench");
    std::error_code ec;

    auto timeOne = [&](const char* name, bool staged) -> long long {
        const auto work = dir / (std::string(name) + ".pdf");
        std::filesystem::remove(work, ec);
        std::filesystem::copy_file(fixture, work, ec);
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, work);
        if (!doc) return -1;
        if (!addAnnotation(ctx, doc.get())) return -1;
        const auto t0 = std::chrono::steady_clock::now();
        if (staged) {
            doc->saveIncrementalStaged();
        } else {
            doc->saveIncremental();
        }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
            .count();
    };

    const long long unstaged = timeOne("unstaged", false);
    const long long staged = timeOne("staged", true);
    ASSERT_GE(unstaged, 0);
    ASSERT_GE(staged, 0);

    const auto bytes = std::filesystem::file_size(fixture, ec);
    std::cout << "[ MEASURED  ] ssa_sf86.pdf (" << bytes << " bytes)\n"
              << "[ MEASURED  ]   saveIncremental()       = " << unstaged
              << " ms\n"
              << "[ MEASURED  ]   saveIncrementalStaged() = " << staged
              << " ms\n"
              << "[ MEASURED  ]   staging overhead        = "
              << (staged - unstaged) << " ms" << std::endl;

    // The claim the routing decision rests on is that the staging overhead is a
    // FILE COPY — proportional to SIZE, not to XFA complexity. An 8 MB copy that
    // took longer than a second would falsify that and the decision would have
    // to be revisited, so assert it rather than only printing it.
    EXPECT_LT(staged - unstaged, 2000)
        << "staging an 8 MB document cost " << (staged - unstaged)
        << " ms — that is not a file copy, and the routing rationale recorded "
           "in Document.h needs re-deriving";

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// F71 — the staging file must carry the DESTINATION's DACL for the WHOLE
// append, not just from publication onwards.
//
// The two StagedSaveDacl cases above check the PUBLISHED file, and both passed
// throughout the window this defect was open: commitOver() has always applied
// the destination's DACL before renaming, so the end state was already right.
// What neither could see is the TRANSIENT state in between. Until F71 the
// staging file was created with no security descriptor, so it wore the
// CONTAINING DIRECTORY's inherited ACEs from CreateFileW until commit — and a
// full plaintext copy of an owner-only document sat readable by every principal
// that directory admits for as long as the save took. Seconds, on a large file.
//
// So this observes the staging file WHILE THE HANDLE IS STILL OPEN, which is
// the only moment the property is falsifiable. It drives StagingFile directly
// rather than a save, because a save closes the window it needs to look into.
//
// KILLS: reverting either factory to create-then-DACL-at-publish. The published
// file would still be correct and the two cases above would still pass.
// ---------------------------------------------------------------------------
TEST(StagedSaveDacl, StagingFileCarriesTheDestinationDaclBeforeAnyBytesAreWritten)
{
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty());
    const std::string me = currentUserSidString();
    ASSERT_FALSE(me.empty()) << "could not resolve the current user SID";

    const auto dir = makeWorkDir("dacl_staging");
    ASSERT_TRUE(grantUsersModifyInheritable(dir, me))
        << "could not make the containing folder permissive; without that this "
           "test cannot reproduce the exposure it exists to catch — the staging "
           "file would inherit nothing interesting";

    const auto target = dir / "ownersonly.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec);

    const std::wstring meW = std::wstring(me.begin(), me.end());
    ASSERT_TRUE(applyProtectedDacl(target, L"D:(A;;FA;;;" + meW + L")"))
        << "could not lock the document to owner-only";

    const std::string targetSddl = daclSddl(target);
    ASSERT_NE(targetSddl.find("(A;;FA;;;" + me + ")"), std::string::npos)
        << "the owner-only DACL did not take: " << targetSddl;
    ASSERT_EQ(targetSddl.find(";;;BU)"), std::string::npos)
        << "the destination itself already grants BUILTIN\Users, so this test "
           "cannot distinguish inherited-from-directory from copied-from-target: "
        << targetSddl;

    for (const bool fullRewrite : {false, true}) {
        std::error_code sfEc;
        auto sf = fullRewrite
            ? pdfe::core::StagingFile::createFullRewriteTempFor(target, sfEc)
            : pdfe::core::StagingFile::createBeside(target, sfEc);
        ASSERT_NE(sf, nullptr)
            << (fullRewrite ? "createFullRewriteTempFor" : "createBeside")
            << " failed: " << sfEc.message();

        // The handle is still open and NOTHING has been written yet. This is
        // precisely the instant the old code left the file wearing the
        // directory's ACEs.
        const std::string stagingSddl = daclSddl(sf->path());

        EXPECT_EQ(stagingSddl.find(";;;BU)"), std::string::npos)
            << "\n=== factory        === "
            << (fullRewrite ? "createFullRewriteTempFor" : "createBeside")
            << "\n=== staging DACL   === " << stagingSddl
            << "\n=== destination    === " << targetSddl
            << "\nThe staging file grants BUILTIN\Users before a single byte is "
               "written. It inherited the containing directory's ACEs instead of "
               "taking the destination's, so a plaintext copy of an owner-only "
               "document is readable by every principal that folder admits for "
               "the duration of the save (F71).";

        EXPECT_NE(stagingSddl.find("(A;;FA;;;" + me + ")"), std::string::npos)
            << "the staging file does not carry the destination's owner ACE: "
            << stagingSddl;
    }

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// F71, CLASS LEVEL. The case above proves the two factories that exist TODAY
// carry the destination's DACL. This one is about the class: it pins how many
// staging factories `StagingFile` declares, so adding a THIRD without giving it
// the same treatment fails here rather than shipping a new exposure quietly.
//
// The count is read from the header rather than hard-coded in a comment,
// because a number in prose beside four machine-checked numbers is exactly the
// thing this repository has watched drift before.
//
// KILLS: adding a staging factory that creates with no security descriptor.
// ---------------------------------------------------------------------------
TEST(StagedSaveDacl, EveryDeclaredStagingFactoryIsCoveredByTheDaclCase)
{
    const auto header = std::filesystem::path(PDFE_REPO_ROOT)
        / "src" / "core" / "AtomicFileReplace.h";
    std::ifstream in(header, std::ios::binary);
    ASSERT_TRUE(in) << "cannot read " << header.string();
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // Every `static std::unique_ptr<StagingFile> <name>(` declaration.
    size_t declared = 0;
    const std::string needle = "static std::unique_ptr<StagingFile>";
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++declared;
    }

    // The DACL case above exercises exactly these two by name.
    const size_t coveredByTheDaclCase = 2;

    EXPECT_EQ(declared, coveredByTheDaclCase)
        << "StagingFile declares " << declared << " factory function(s), but "
        << coveredByTheDaclCase << " are exercised by "
           "StagedSaveDacl.StagingFileCarriesTheDestinationDaclBeforeAnyBytes"
           "AreWritten. "
           "A staging factory that creates its file with no "
           "security descriptor gives it the CONTAINING DIRECTORY's inherited "
           "ACEs, which is F71: a plaintext copy of an owner-only document "
           "readable by every principal that folder admits, for the whole "
           "append. "
           "Add the new factory to that test's loop (and call "
           "preserveDaclImpl immediately after CreateFileW) rather than "
           "raising this number.";
}
