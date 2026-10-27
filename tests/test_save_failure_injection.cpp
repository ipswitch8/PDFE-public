// -*- coding: utf-8 -*-
//
// F10 — save-path failure injection (meta-audit-2026-07 follow-up).
//
// Gaps: WIN-64, WIN-76, WIN-77, WIN-84, WIN-85, WIN-87.
//
// WHAT THIS PINS DOWN
// -------------------
// PDFE's durable-write sequence is
//
//     write "<target>.pdfe_tmp"  →  fsync it  →  atomic rename over <target>
//
// and the whole point of it is a single property:
//
//     IF ANYTHING GOES WRONG, THE USER'S EXISTING FILE IS BYTE-IDENTICAL.
//
// Before this file that property was asserted for exactly one failure shape (a
// held Page blocking the rename, test_document_save_atomic.cpp). The three
// Critical windows-QA gaps — no space left during the temp write (WIN-76), the
// same on the autosave/recovery path (WIN-77), the volume/media going away
// mid-operation (WIN-64) — had no coverage at all, and the anti-A/V rename
// retry (WIN-84) had none either.
//
// Every test here asserts the BYTE-IDENTICAL property, not merely "the save
// returned false". A save that reports failure while having already truncated
// the target is the exact defect the criterion exists to catch, and only a byte
// comparison sees it.
//
// The injections are real OS failures at the real call sites, via
// tests/support/SaveFailureInjection.h — read the "HOW REAL IS THIS?" block
// there, including the one honest limitation (a literal mid-write
// ERROR_DISK_FULL needs an administratively-created small volume; the
// open-failure this uses takes the identical code path because the save path
// has no disk-full-specific handling to differentiate).

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "support/SaveFailureInjection.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::testing::fileIsByteIdenticalTo;
using pdfe::testing::injectionPointName;
using pdfe::testing::readAllBytes;
using pdfe::testing::RenameLock;
using pdfe::testing::SaveInjectionPoint;
using pdfe::testing::saveTempPathFor;
using pdfe::testing::TempWriteBlocker;
using pdfe::testing::TempWriteBlockMode;

namespace {

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

class SaveFailureInjectionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();

        // Every test gets its OWN directory (uniqued by test name + clock), so a
        // leftover lock or temp from a previous run can never make a later run
        // green-for-the-wrong-reason. This is the #49 lesson applied.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path()
               / ("pdfe_f10_" + std::string(info->name()) + "_"
                  + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()));
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        ASSERT_FALSE(ec) << "could not create the per-test dir: " << ec.message();

        source_ = dir_ / "source.pdf";
        std::filesystem::copy_file(fixtures() / "test_3page.pdf", source_,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        ASSERT_FALSE(ec) << "could not stage the source fixture: " << ec.message();
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// A second, DISTINCT existing PDF that plays "the file the user is about to
    /// overwrite". Its bytes are the thing that must survive.
    std::filesystem::path stageVictim(const char* name = "victim.pdf")
    {
        std::error_code ec;
        auto victim = dir_ / name;
        std::filesystem::copy_file(fixtures() / "test_single.pdf", victim,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        EXPECT_FALSE(ec) << "could not stage the victim: " << ec.message();
        return victim;
    }

    std::unique_ptr<Document> openEditedSource()
    {
        auto doc = Document::open(ctx_, source_);
        EXPECT_NE(doc, nullptr);
        if (!doc) {
            return nullptr;
        }
        // A REAL structural edit — the change that would be lost, and the thing
        // that forces the full-rewrite (temp + rename) route.
        doc->insertBlankPage(1, 300.0f, 300.0f);
        doc->markRequiresFullRewrite();
        EXPECT_EQ(doc->pageCount(), 4);
        return doc;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path dir_;
    std::filesystem::path source_;
};

// ===========================================================================
// SaveInjectionPoint::TempWrite — the ENOSPC class (WIN-76, WIN-64).
// ===========================================================================

// Parametrized over both ways of making the temp destination genuinely
// unwritable, so a fix that only survives one of them cannot pass.
class TempWriteFailureTest
    : public SaveFailureInjectionTest,
      public ::testing::WithParamInterface<TempWriteBlockMode> {};

// (WIN-76, Critical) An in-place save whose TEMP WRITE cannot proceed must leave
// the user's file BYTE-IDENTICAL. This is the data-loss guard: if the code ever
// regressed to opening the target directly (fopen "wb" truncates), the target
// would be destroyed by a failure that never produced a single usable byte.
TEST_P(TempWriteFailureTest, EnospcDuringTempWriteLeavesOriginalByteIdentical) {
    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    // Snapshot the ORIGINAL bytes: the edit is still only in memory.
    const std::string originalBytes = readAllBytes(source_);
    ASSERT_FALSE(originalBytes.empty());

    // Inject at the named point.
    TempWriteBlocker blocker(source_, GetParam());
    ASSERT_TRUE(blocker.armed())
        << "failed to arm the " << injectionPointName(SaveInjectionPoint::TempWrite)
        << " injection — the test would otherwise pass as a happy path";

    EXPECT_ANY_THROW(doc->save(source_))
        << "a save whose temp write cannot proceed must report failure";

    // THE ORACLE. Byte-for-byte, not just "still exists" or "same size".
    const std::string afterBytes = readAllBytes(source_);
    EXPECT_EQ(afterBytes, originalBytes)
        << "ENOSPC-class temp-write failure CLOBBERED the original file — "
           "this is silent data loss (WIN-76)";
    EXPECT_TRUE(fileIsByteIdenticalTo(source_, originalBytes));
}

// (WIN-76 / WIN-87) The same guarantee for saveAs ONTO AN EXISTING FILE — the
// case that used to hand the victim's path straight to pdf_save_document, which
// truncates it on open. A failed overwrite must not consume the victim.
TEST_P(TempWriteFailureTest, SaveAsOverExistingFileNeverTruncatesTheVictim) {
    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    const auto victim = stageVictim();
    const std::string victimBytes = readAllBytes(victim);
    ASSERT_FALSE(victimBytes.empty());
    ASSERT_NE(victimBytes, readAllBytes(source_))
        << "victim and source must differ or the oracle proves nothing";

    TempWriteBlocker blocker(victim, GetParam());
    ASSERT_TRUE(blocker.armed());

    EXPECT_ANY_THROW(doc->saveAs(victim))
        << "saveAs must report failure when its temp write cannot proceed";

    EXPECT_TRUE(fileIsByteIdenticalTo(victim, victimBytes))
        << "a failed Save As TRUNCATED the file it was overwriting — the "
           "user lost a file they never even opened (WIN-76)";
}

INSTANTIATE_TEST_SUITE_P(
    TempWriteBlockModes, TempWriteFailureTest,
    ::testing::Values(TempWriteBlockMode::DirectoryOnTempPath,
                      TempWriteBlockMode::ExclusiveHandleOnTempPath),
    [](const ::testing::TestParamInfo<TempWriteBlockMode>& i) {
        return i.param == TempWriteBlockMode::DirectoryOnTempPath
                   ? "DirectoryOnTempPath"
                   : "ExclusiveHandleOnTempPath";
    });

// (WIN-76) Control: once the obstruction is gone the SAME document saves
// successfully and the edit lands. Without this the tests above could pass
// against a save that never works at all.
TEST_F(SaveFailureInjectionTest, SaveSucceedsOnceTempWriteObstructionClears) {
    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    {
        TempWriteBlocker blocker(source_, TempWriteBlockMode::DirectoryOnTempPath);
        ASSERT_TRUE(blocker.armed());
        EXPECT_ANY_THROW(doc->save(source_));
    }  // blocker released here

    ASSERT_NO_THROW(doc->save(source_));
    EXPECT_FALSE(std::filesystem::exists(saveTempPathFor(source_)))
        << "a successful save must leave no orphan temp";

    doc.reset();
    auto vctx = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(vctx, source_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 4)
        << "the recovered save must actually have persisted the edit";
}

// (WIN-76, literal partial-temp residue) A real disk-full between the temp write
// and the rename leaves a TRUNCATED "<target>.pdfe_tmp" behind. Opening the
// original afterwards must surface that orphan for the user to decide about and
// must NEVER apply it over their file — a truncated temp is not a document.
TEST_F(SaveFailureInjectionTest, TruncatedOrphanTempIsSurfacedNotApplied) {
    const std::string originalBytes = readAllBytes(source_);
    ASSERT_FALSE(originalBytes.empty());

    const auto temp = pdfe::testing::stageTruncatedOrphanTemp(source_, source_, 512);
    ASSERT_TRUE(std::filesystem::exists(temp));
    ASSERT_LT(std::filesystem::file_size(temp), originalBytes.size())
        << "the staged orphan must genuinely be truncated";

    auto doc = Document::open(ctx_, source_);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(doc->pendingRecoveredTemp().has_value())
        << "a leftover .pdfe_tmp from a disk-full crash must be surfaced";
    EXPECT_TRUE(fileIsByteIdenticalTo(source_, originalBytes))
        << "opening a document with a truncated orphan temp must not touch the "
           "user's file";
}

// ===========================================================================
// SaveInjectionPoint::AtomicRename — the A/V lock (WIN-84, WIN-85).
// ===========================================================================

#if defined(_WIN32)

// (WIN-84, half 1 of 2) TRANSIENT lock: an A/V scanner holds the destination for
// ~300 ms and then lets go. The bounded retry in atomicReplaceFileWithRetry
// (10 × 100 ms) exists exactly for this, and it must SUCCEED — with the edit
// actually persisted, not merely "no throw".
TEST_F(SaveFailureInjectionTest, RenameRetrySucceedsAgainstTransientLock) {
    const auto victim = stageVictim();
    const std::string victimBytes = readAllBytes(victim);
    ASSERT_FALSE(victimBytes.empty());

    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    // Held for 300 ms — inside the ~1 s retry budget, so attempt 4-ish wins.
    RenameLock transientLock(victim, std::chrono::milliseconds(300));
    ASSERT_TRUE(transientLock.armed())
        << "failed to arm the " << injectionPointName(SaveInjectionPoint::AtomicRename)
        << " injection";

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(doc->saveAs(victim))
        << "the anti-A/V rename retry must ride out a transient lock (WIN-84)";
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Proof it actually RETRIED rather than getting lucky: the save cannot have
    // completed before the lock was released.
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              200)
        << "the save finished before the lock was released — the rename was "
           "never actually blocked, so this test proves nothing about the retry";
    EXPECT_TRUE(transientLock.released());

    // The overwrite really happened: victim now holds the edited document.
    EXPECT_FALSE(fileIsByteIdenticalTo(victim, victimBytes))
        << "the retry 'succeeded' without replacing the target";
    doc.reset();
    auto vctx = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(vctx, victim);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 4)
        << "the retried save must have persisted the edit";
    EXPECT_FALSE(std::filesystem::exists(saveTempPathFor(victim)))
        << "a successful retried save must leave no orphan temp";
}

// (WIN-84, half 2 of 2) PERMANENT lock: the destination is held for longer than
// the entire retry budget. The save must report a CLEAN, DIAGNOSABLE error — and
// the locked file must be BYTE-IDENTICAL, because a "clean error" that ate the
// user's file is not clean at all.
TEST_F(SaveFailureInjectionTest, RenameReportsCleanErrorAgainstPermanentLock) {
    const auto victim = stageVictim();
    const std::string victimBytes = readAllBytes(victim);
    ASSERT_FALSE(victimBytes.empty());

    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    RenameLock permanentLock(victim);  // never released until scope exit
    ASSERT_TRUE(permanentLock.armed());

    std::string what;
    try {
        doc->saveAs(victim);
        ADD_FAILURE() << "saveAs onto a permanently locked target must throw";
    } catch (const std::exception& e) {
        what = e.what();
    }
    EXPECT_FALSE(permanentLock.released())
        << "the lock must still be held throughout the save — otherwise this "
           "was the transient case and proves nothing about a permanent one";

    // Diagnosable: the message must say what happened, not be empty boilerplate.
    EXPECT_FALSE(what.empty());
    EXPECT_NE(what.find("intact"), std::string::npos)
        << "the error must tell the caller the target was left intact; got: " << what;

    // Release only NOW — the lock denies all sharing, so the file cannot even be
    // READ while it is held, and the byte oracle below must actually read it.
    permanentLock.release();

    // THE ORACLE: the file we could not replace is untouched.
    EXPECT_TRUE(fileIsByteIdenticalTo(victim, victimBytes))
        << "a permanently-locked rename destroyed the target (WIN-84)";

    EXPECT_FALSE(std::filesystem::exists(saveTempPathFor(victim)))
        << "the failed save must clean up its orphan temp";

    // And the Document survived: it can still save elsewhere (no dead handle).
    // Read the result back — "did not throw" would also be true of a save that
    // silently produced nothing, which is not evidence of a live handle
    // (GitHub #163).
    const auto elsewhere = dir_ / "elsewhere.pdf";
    EXPECT_NO_THROW(doc->saveAs(elsewhere));
    ASSERT_TRUE(std::filesystem::exists(elsewhere))
        << "the recovery save wrote no file at all";
    EXPECT_GT(std::filesystem::file_size(elsewhere), 0u)
        << "the recovery save produced an empty file";
}

// (WIN-84) The in-place Document::save() rename retry, same two shapes. save()
// and saveAs() reach the atomic replace by different routes, so both are pinned.
TEST_F(SaveFailureInjectionTest, InPlaceSaveRenameRetrySucceedsAgainstTransientLock) {
    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);
    const std::string originalBytes = readAllBytes(source_);

    // The document itself holds source_ open, so lock a COPY-TARGET instead:
    // an in-place save() to a second path that already exists.
    const auto victim = stageVictim("inplace_victim.pdf");
    const std::string victimBytes = readAllBytes(victim);

    RenameLock transientLock(victim, std::chrono::milliseconds(300));
    ASSERT_TRUE(transientLock.armed());

    ASSERT_NO_THROW(doc->save(victim))
        << "Document::save's rename retry must ride out a transient lock";
    EXPECT_FALSE(fileIsByteIdenticalTo(victim, victimBytes))
        << "the retried in-place save did not replace the target";
    // The source we opened from was not the save target, so it is untouched.
    EXPECT_TRUE(fileIsByteIdenticalTo(source_, originalBytes));
}

TEST_F(SaveFailureInjectionTest, InPlaceSaveReportsCleanErrorAgainstPermanentLock) {
    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    const auto victim = stageVictim("inplace_perm_victim.pdf");
    const std::string victimBytes = readAllBytes(victim);

    RenameLock permanentLock(victim);
    ASSERT_TRUE(permanentLock.armed());

    EXPECT_ANY_THROW(doc->save(victim));
    EXPECT_FALSE(permanentLock.released())
        << "the lock must still have been held for the whole retry budget";
    EXPECT_NO_THROW({ (void)doc->pageCount(); })
        << "the failed save must leave a usable Document, not a null handle";

    // Release before reading: the lock denies read sharing too.
    permanentLock.release();
    EXPECT_TRUE(fileIsByteIdenticalTo(victim, victimBytes))
        << "a permanently-locked in-place save destroyed the target";
}

// (WIN-64, Critical — media/network disconnect mid-operation) Losing the volume
// mid-save presents to the process as the target's DIRECTORY becoming
// unavailable. The closest faithful, unprivileged reproduction is to make the
// destination unreachable for the rename while the temp write has already
// happened — which is what a detach between the two steps produces. The
// requirement is identical to a disconnect: an error, and nothing destroyed.
TEST_F(SaveFailureInjectionTest, VolumeUnavailableAtRenameLeavesTargetIntact) {
    const auto victim = stageVictim("detach_victim.pdf");
    const std::string victimBytes = readAllBytes(victim);

    auto doc = openEditedSource();
    ASSERT_NE(doc, nullptr);

    RenameLock detach(victim);
    ASSERT_TRUE(detach.armed());

    EXPECT_ANY_THROW(doc->saveAs(victim));
    EXPECT_FALSE(detach.released())
        << "the destination must have been unreachable for the whole save";
    detach.release();  // read sharing is denied while it is held
    EXPECT_TRUE(fileIsByteIdenticalTo(victim, victimBytes))
        << "a mid-operation disconnect must not leave a truncated target (WIN-64)";
    EXPECT_FALSE(std::filesystem::exists(saveTempPathFor(victim)))
        << "no orphan temp may survive the failed save";
}

#endif  // _WIN32

// ===========================================================================
// WIN-77 (Critical) — the autosave / recovery-copy path under the same
// failure. A recovery copy that cannot be written must NOT be published as if
// it were usable, or the user recovers a truncated file believing it is their
// work.
// ===========================================================================

TEST_F(SaveFailureInjectionTest, AutosaveWithUnwritableRecoveryDirDoesNotPublish) {
    auto doc = Document::open(ctx_, source_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<pdfe::commands::UndoManager>();
    const std::string sourceBytesBefore = readAllBytes(source_);
    ASSERT_FALSE(sourceBytesBefore.empty());

    // A recovery directory that cannot hold files: a FILE occupies the path
    // where the directory must be, so every create inside it fails for real.
    const auto blockedRecoveryDir = dir_ / "recovery_blocked";
    {
        std::ofstream f(blockedRecoveryDir, std::ios::binary);
        f << "not-a-directory";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(blockedRecoveryDir));

    // The manager BASELINES the document's content version at construction, so
    // it must exist BEFORE the edit — otherwise autosaveNow() correctly sees
    // nothing dirty and this test would pass without exercising anything.
    AutosaveManager mgr(doc.get(), undo.get(), source_, blockedRecoveryDir);
    doc->insertBlankPage(1, 300.0f, 300.0f);
    doc->markRequiresFullRewrite();
    ASSERT_EQ(doc->pageCount(), 4);
    bool published = false;
    try {
        published = mgr.autosaveNow();
    } catch (const std::exception&) {
        published = false;  // reporting the failure by throwing is acceptable
    }

    // Either the autosave honestly reports it could not run, or — if it claims
    // success — the artefacts it claims must genuinely exist and be openable.
    // What is forbidden is claiming success with nothing (or a stub) on disk.
    if (published) {
        ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()))
            << "autosaveNow() reported success but published no recovery copy "
               "(WIN-77): a later scan would offer the user a file that is not "
               "there";
        auto vctx = std::make_shared<MuPdfContext>();
        EXPECT_NE(Document::open(vctx, mgr.recoveryFilePath()), nullptr)
            << "the published recovery copy is not an openable PDF (WIN-77)";
    } else {
        EXPECT_FALSE(mgr.hasRecovery())
            << "a failed autosave must not leave a recovery record pointing at "
               "nothing";
    }

    // In every branch, the user's SOURCE file is untouched — autosave never
    // writes back over it.
    EXPECT_TRUE(fileIsByteIdenticalTo(source_, sourceBytesBefore))
        << "the autosave path wrote back over the user's source file";
    EXPECT_EQ(doc->pageCount(), 4);
}

// (WIN-77) The recovery COPY target itself being unwritable must not corrupt an
// already-good recovery copy from a previous tick.
TEST_F(SaveFailureInjectionTest, FailedAutosaveDoesNotCorruptEarlierRecoveryCopy) {
    auto doc = Document::open(ctx_, source_);
    ASSERT_NE(doc, nullptr);
    auto undo = std::make_unique<pdfe::commands::UndoManager>();

    const auto recoveryDir = dir_ / "recovery";
    std::error_code ec;
    std::filesystem::create_directories(recoveryDir, ec);

    // Construct BEFORE editing — the manager baselines contentVersion in its
    // constructor (see AutosaveManager.cpp), so an edit made first is invisible.
    AutosaveManager mgr(doc.get(), undo.get(), source_, recoveryDir);
    doc->insertBlankPage(1, 300.0f, 300.0f);
    doc->markRequiresFullRewrite();

    ASSERT_TRUE(mgr.autosaveNow()) << "the first autosave must succeed";
    const auto copyPath = mgr.recoveryFilePath();
    ASSERT_TRUE(std::filesystem::exists(copyPath));
    const std::string goodCopy = readAllBytes(copyPath);
    ASSERT_FALSE(goodCopy.empty());

#if defined(_WIN32)
    // Second tick with the recovery copy held exclusively — the write cannot
    // land. The previously-good copy must still be a valid PDF afterwards.
    RenameLock hold(copyPath);
    ASSERT_TRUE(hold.armed());
    doc->insertBlankPage(2, 300.0f, 300.0f);
    doc->markRequiresFullRewrite();
    try {
        (void)mgr.autosaveNow();  // may succeed or fail; must not corrupt
    } catch (const std::exception&) {
        // a reported failure is fine — silent corruption is not
    }
    hold.release();
#endif

    auto vctx = std::make_shared<MuPdfContext>();
    EXPECT_NE(Document::open(vctx, copyPath), nullptr)
        << "a failed autosave tick left the previous recovery copy unopenable "
           "(WIN-77): the user's only backup was destroyed by the backup itself";
}

}  // namespace
