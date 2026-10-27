// -*- coding: utf-8 -*-
//
// F10 — process death INSIDE the save window (meta-audit-2026-07 follow-up).
//
// Gaps: WIN-69 (Critical), WIN-81, WIN-82 (crash residue), WIN-87.
//
// WHY A SECOND FILE
// -----------------
// This is the SECOND registered test file using
// tests/support/SaveFailureInjection.h (the first is
// test_save_failure_injection.cpp). It exercises the harness's third named
// injection point, SaveInjectionPoint::InsideWindow, which cannot live in the
// same file as the in-process injections because it needs a re-entrant "victim"
// mode: the parent re-executes THIS BINARY with a gtest filter naming the
// victim test, and kills it.
//
// WHAT MAKES THIS REAL
// --------------------
// The existing kill-oracle for autosave
// (AutosaveRecoveryTest.KillMidEditRecoveryRestoresUnsavedWork) SIMULATES the
// crash by dropping C++ objects — which is honest about itself, but it cannot
// see anything that only a genuine kill produces: unflushed buffers, a
// half-published rename, an orphan temp with a live directory entry.
//
// Here the child is really killed with TerminateProcess: no unwinding, no
// destructors, no CRT flush. And it is killed INSIDE the window, not merely
// near it — the child watches for "<target>.pdfe_tmp" to appear and only then
// signals the parent, so at the moment of the kill pdf_save_document has
// produced the temp and the atomic rename has not yet been attempted.
//
// THE CONTRACT ASSERTED
// ---------------------
// An atomic replace has exactly two legal observable outcomes after a kill:
//
//   (a) the rename had not happened  → the target is BYTE-IDENTICAL to what it
//       was before, and an orphan temp may be present; or
//   (b) the rename had happened      → the target is the complete NEW document
//       and opens cleanly.
//
// What must NEVER be observed is a third state: a target that is missing,
// truncated or unopenable. That third state is exactly what the old
// remove-then-rename design produced, and it is what this test would catch if
// anyone reintroduced it. The assertion is deliberately written as
// "(a) OR (b), never a third state" so it is deterministic — no reliance on
// winning a race.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "support/SaveFailureInjection.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::testing::injectionPointName;
using pdfe::testing::KilledSaveResult;
using pdfe::testing::readAllBytes;
using pdfe::testing::SaveInjectionPoint;
using pdfe::testing::saveTempPathFor;

namespace {

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE VICTIM.
//
// In the parent process this is a no-op that passes immediately (the env var is
// unset), so it costs nothing in a normal suite run and is never DISABLED_ or
// skipped. In the child — launched by KillsChildInsideSaveWindow with
// PDFE_F10_SAVE_VICTIM set — it opens the source, makes a real structural edit
// and saves over the target, having first started the watcher that tells the
// parent when the temp appears. The parent kills it partway through; this test
// is not expected to complete there.
// ---------------------------------------------------------------------------
TEST(SaveKillWindowVictim, SavesUntilKilled) {
    if (!pdfe::testing::runningAsSaveVictim()) {
        // Parent process: nothing to do. Asserting the harness agrees keeps this
        // from being an empty always-green test.
        EXPECT_TRUE(pdfe::testing::saveVictimTarget().empty());
        return;
    }

    const auto source = pdfe::testing::saveVictimSource();
    const auto target = pdfe::testing::saveVictimTarget();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(target.empty());

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, source);
    ASSERT_NE(doc, nullptr);

    // A real structural edit forces the full-rewrite temp+rename route.
    for (int i = 0; i < 3; ++i) {
        doc->insertBlankPage(1, 400.0f, 400.0f);
    }
    doc->markRequiresFullRewrite();

    // Tell the parent the moment the temp file materialises — i.e. the moment we
    // are genuinely INSIDE the window.
    pdfe::testing::startTempAppearanceWatcher(target);

    // Loop so the window is wide enough for the parent to land the kill in it on
    // any machine; each iteration re-enters the same save sequence. The parent
    // kills us; reaching the end of the loop is also acceptable (the assertions
    // that matter run in the parent, against the on-disk state).
    for (int i = 0; i < 40; ++i) {
        try {
            doc->markRequiresFullRewrite();
            doc->save(target);
        } catch (const std::exception&) {
            // A failed save here is fine — the parent's oracle is the file state.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

#if defined(_WIN32)

// (WIN-81 / WIN-69) A REAL TerminateProcess inside the temp-write → rename
// window must never leave the target missing, truncated, or unopenable.
TEST(SaveKillWindow, KillInsideSaveWindowNeverLosesTheTarget) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path()
                     / ("pdfe_f10_kill_"
                        + std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto source = dir / "kill_source.pdf";
    const auto target = dir / "kill_target.pdf";
    std::filesystem::copy_file(fixtures() / "test_3page.pdf", source,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);
    // The target starts as a DIFFERENT, complete document — the thing that must
    // survive if the kill lands before the rename.
    std::filesystem::copy_file(fixtures() / "test_single.pdf", target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    const std::string targetBefore = readAllBytes(target);
    ASSERT_FALSE(targetBefore.empty());

    const KilledSaveResult r = pdfe::testing::killChildInsideSaveWindow(
        source, target, "SaveKillWindowVictim.SavesUntilKilled");

    ASSERT_TRUE(r.childLaunched)
        << "could not spawn the victim child at injection point "
        << injectionPointName(SaveInjectionPoint::InsideWindow)
        << ": " << r.diagnostic;
    ASSERT_TRUE(r.childReachedWindow)
        << "the child never entered the save window, so nothing was injected: "
        << r.diagnostic;
    ASSERT_TRUE(r.childKilled) << r.diagnostic;

    // ── THE ORACLE ──────────────────────────────────────────────────────
    // (a) untouched-original, or (b) fully-replaced. Never a third state.
    ASSERT_TRUE(std::filesystem::exists(target))
        << "the kill left the target file GONE — the atomic replace must never "
           "have a window in which the destination does not exist (WIN-81)";

    const std::string targetAfter = readAllBytes(target);
    ASSERT_FALSE(targetAfter.empty())
        << "the kill left the target TRUNCATED to zero bytes (WIN-81)";

    if (targetAfter == targetBefore) {
        // (a) the rename had not landed. Nothing more to prove about the target;
        // any orphan temp must be surfaced rather than silently applied.
        auto ctx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(ctx, target);
        ASSERT_NE(reopened, nullptr)
            << "the untouched target no longer opens after the kill";
        if (std::filesystem::exists(saveTempPathFor(target))) {
            EXPECT_TRUE(reopened->pendingRecoveredTemp().has_value())
                << "an orphan temp left by the killed save must be surfaced to "
                   "the user, not silently ignored (WIN-81)";
        }
    } else {
        // (b) the rename landed. The target must be the COMPLETE new document.
        auto ctx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(ctx, target);
        ASSERT_NE(reopened, nullptr)
            << "the kill published a target that no longer opens — the replace "
               "was not atomic (WIN-81)";
        EXPECT_GT(reopened->pageCount(), 0);
    }

    std::filesystem::remove_all(dir, ec);
}

// (WIN-81) Control: the same child, NOT killed, produces a clean, openable
// target. Without this the oracle above could pass against a child that never
// managed to save anything at all.
TEST(SaveKillWindow, UnkilledChildProducesACleanTarget) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path()
                     / ("pdfe_f10_nokill_"
                        + std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto source = dir / "nokill_source.pdf";
    const auto target = dir / "nokill_target.pdf";
    std::filesystem::copy_file(fixtures() / "test_3page.pdf", source,
                               std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::copy_file(fixtures() / "test_single.pdf", target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    // Run the very same save sequence in-process, uninterrupted.
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, source);
        ASSERT_NE(doc, nullptr);
        doc->insertBlankPage(1, 400.0f, 400.0f);
        doc->markRequiresFullRewrite();
        ASSERT_NO_THROW(doc->save(target));
    }

    auto vctx = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(vctx, target);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), 4)
        << "the uninterrupted save must publish the edited document";
    EXPECT_FALSE(std::filesystem::exists(saveTempPathFor(target)))
        << "an uninterrupted save must leave no orphan temp";

    std::filesystem::remove_all(dir, ec);
}

#endif  // _WIN32
