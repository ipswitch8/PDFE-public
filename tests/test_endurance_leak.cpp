// -*- coding: utf-8 -*-
//
// C-02 (coverage-expansion register) — endurance / soak as a GATED leak oracle.
//
// WHY THIS CLASS EXISTS
// ---------------------
// tests/test_ui_journey_soak.py and tests/test_journey_op99_crash.cpp exercise
// a long op sequence and have caught real CRASHES, but neither is a GATED
// memory/handle-leak oracle: nothing asserts that per-operation resource growth
// stays under a threshold across a long editing session. A ~1-per-op leak
// (memory, a GDI/USER object, or a kernel/file handle) accumulates over hours of
// real editing and crashes the app — exactly the class the ad-hoc soak surfaced
// by hand. This test turns that into a bounded, deterministic, committed gate.
//
// WHAT IT DRIVES (headless, real production paths)
// ------------------------------------------------
// A fixed op sequence per iteration, all through the SAME code the UI drives:
//   open(fixture) -> annotate (CreateAnnotationCommand + setContents)
//                 -> redact  (CreateRedactionCommand)
//                 -> rotate  (RotatePageCommand)                 [the "edit"]
//                 -> undo x3 -> redo x3                          [do/undo churn]
//                 -> saveAs(temp)                                [full rewrite]
//                 -> reopen(temp) + verify pageCount            [save->reopen]
//                 -> destroy doc / undo stack / page wrappers
// Each iteration always opens the SAME pristine staged fixture (edits are never
// fed back), so a healthy iteration is steady-state: growth after warm-up is
// jitter, a real per-op leak is linear.
//
// ORACLE — warm-up + bounded-growth (same methodology as R3-18
// test_open_close_leak_cycle.cpp and test_ui_journey_soak.py):
//   1. WARM-UP iterations first — let MuPDF/Qt one-time caches (fonts, glyph,
//      pixmap) fill so their one-shot growth is not misread as a leak.
//   2. Snapshot the baseline AFTER warm-up.
//   3. Run K measured iterations.
//   4. Assert each measured resource — process commit (PrivateUsage), kernel
//      handles, GDI objects, USER objects — grew by LESS THAN a bounded budget.
// A real ~per-iteration leak grows a counter linearly (~K); a healthy iteration
// stays flat after warm-up. The budgets sit between: comfortably above warm-up
// jitter, far below a linear leak.
//
// NON-VACUITY PROOF (the oracle can actually FAIL on a leak). A deliberate
// per-iteration leak — a 2 MiB heap block plus one kernel handle + one GDI
// object + one USER object, none ever freed — is COMPILED IN but INERT by
// default, armed only when PDFE_ENDURANCE_INJECT_LEAK=1. Armed, commit climbs
// ~2 MiB/iter (~2*K MiB over the loop) and each handle counter climbs ~1/iter,
// so every product assertion below goes RED. Unset (the committed default) the
// real sequence is leak-free and the test is GREEN. This lets one build
// demonstrate BOTH the green pass and the red catch without weakening the
// oracle. (Proof run recorded in docs/coverage-expansion/phase-1-report.md.)
//
// BOUNDED TIME: warm-up + K iterations of a tiny 3-page fixture — a few seconds.
// Windows-only (the handle/GDI/USER counters and commit query are Win32); on
// other platforms the body is a trivially-passing skip, matching R3-18.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/PageInfo.h"
#include "core/UndoManager.h"
#include "core/PageCommands.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationType.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"

#include <QCoreApplication>
#include <QRectF>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <cstdlib>
#  pragma comment(lib, "psapi.lib")
#endif

using namespace pdfe::core;
using pdfe::commands::UndoManager;
using pdfe::commands::RotatePageCommand;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::CreateAnnotationCommand;
using pdfe::annotations::AnnotationType;
using pdfe::redaction::RedactionManager;
using pdfe::redaction::CreateRedactionCommand;

#ifdef _WIN32

namespace {

struct ResourceSample {
    long long privateBytes = 0;  // commit charge (PROCESS_MEMORY_COUNTERS_EX.PrivateUsage)
    long long workingSet = 0;    // resident working set (RSS)
    long handles = 0;            // kernel handles
    long gdi = 0;                // live GDI objects
    long user = 0;               // live USER objects
};

ResourceSample rawSample() {
    ResourceSample s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        s.privateBytes = static_cast<long long>(pmc.PrivateUsage);
        s.workingSet = static_cast<long long>(pmc.WorkingSetSize);
    }
    DWORD h = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &h)) s.handles = static_cast<long>(h);
    s.gdi = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    s.user = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    return s;
}

// Pump deferred deletes, then take the steady (minimum) reading over a few
// samples so momentary spikes (thread-pool wakeups, lazy cache fills) are shed.
ResourceSample steadySample() {
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ResourceSample best = rawSample();
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        ResourceSample s = rawSample();
        best.privateBytes = std::min(best.privateBytes, s.privateBytes);
        best.workingSet = std::min(best.workingSet, s.workingSet);
        best.handles = std::min(best.handles, s.handles);
        best.gdi = std::min(best.gdi, s.gdi);
        best.user = std::min(best.user, s.user);
    }
    return best;
}

// One full editing iteration against the pristine staged fixture. `outPath`
// alternates between two temp files across iterations so a just-saved file is
// never reopened while still locked. When `injectLeak` is true a deliberate
// per-iteration leak is added (non-vacuity proof, off by default).
void runIteration(const std::shared_ptr<MuPdfContext>& ctx,
                  const std::filesystem::path& fixture,
                  const std::filesystem::path& outPath,
                  int seed,
                  bool injectLeak) {
    std::unique_ptr<Document> doc = Document::open(ctx, fixture);
    ASSERT_NE(doc, nullptr);
    const int pc = doc->pageCount();
    ASSERT_GT(pc, 0);

    // Page wrappers stay alive for the whole iteration (the commands hold them
    // by raw pointer); destroyed at scope exit with the doc, not mid-iteration.
    std::vector<std::unique_ptr<Page>> pages;
    UndoManager undo;
    AnnotationManager annots(ctx);
    RedactionManager redactions(ctx);

    // annotate
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pages.push_back(std::move(page));
        Page* p = pages.back().get();
        QRectF rect(80 + (seed % 30), 90 + (seed % 20), 120, 26);
        undo.execute(std::make_unique<CreateAnnotationCommand>(
            &annots, p, AnnotationType::Text, rect));
        const auto created = annots.getAnnotations(p);
        if (!created.empty()) {
            annots.setContents(p, created.back().annotIndex, "endurance note");
        }
    }
    // redact
    {
        auto page = doc->page(1 % pc);
        ASSERT_NE(page, nullptr);
        pages.push_back(std::move(page));
        Page* p = pages.back().get();
        undo.execute(std::make_unique<CreateRedactionCommand>(
            &redactions, p, QRectF(100, 300, 120, 18)));
    }
    // edit (structural): rotate page 0.
    undo.execute(std::make_unique<RotatePageCommand>(doc.get(), 0, 90));

    // do/undo churn: fully unwind, then fully reapply.
    for (int u = 0; u < 3 && undo.canUndo(); ++u) undo.undo();
    for (int r = 0; r < 3 && undo.canRedo(); ++r) undo.redo();

    // save to temp (full rewrite path).
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    ASSERT_NO_THROW(doc->saveAs(outPath));
    ASSERT_TRUE(std::filesystem::exists(outPath));

    // reopen + verify (the save->reopen leg, and the save-with-verify oracle).
    {
        std::unique_ptr<Document> reopened = Document::open(ctx, outPath);
        ASSERT_NE(reopened, nullptr);
        EXPECT_EQ(reopened->pageCount(), pc)
            << "reopened saved document lost/gained pages";
        // reopened destroyed here.
    }

    // doc / undo / managers / pages destroyed at scope exit.
    doc.reset();

    if (injectLeak) {
        // Deliberate per-iteration leak — memory + one of each Windows counter
        // category, so every product assertion is proven catchable. Inert
        // unless armed by PDFE_ENDURANCE_INJECT_LEAK=1.
        void* blk = std::malloc(2u * 1024u * 1024u);  // 2 MiB commit / iter
        if (blk) std::memset(blk, 0xEE, 2u * 1024u * 1024u);  // fault it in
        (void)CreateEventW(nullptr, FALSE, FALSE, nullptr);   // kernel handle
        (void)CreateSolidBrush(RGB(3, 2, 1));                 // GDI object
        (void)CreateMenu();                                   // USER object
        // Intentionally never released.
    }

    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::filesystem::remove(outPath, ec);
}

}  // namespace

TEST(EnduranceLeakTest, SteadyStateResourceDeltaBoundedOverLongSession) {
    const std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "required fixture missing: " << fixture.string();

    // Stage a pristine private copy so the committed fixture is never mutated
    // and every iteration opens byte-identical input.
    const std::filesystem::path staged =
        std::filesystem::temp_directory_path() / "pdfe_endurance_fixture.pdf";
    std::error_code ec;
    std::filesystem::copy_file(
        fixture, staged, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "failed to stage fixture: " << ec.message();

    const std::filesystem::path outA =
        std::filesystem::temp_directory_path() / "pdfe_endurance_out_a.pdf";
    const std::filesystem::path outB =
        std::filesystem::temp_directory_path() / "pdfe_endurance_out_b.pdf";

    const bool injectLeak = [] {
        const char* v = std::getenv("PDFE_ENDURANCE_INJECT_LEAK");
        return v && v[0] == '1';
    }();

    auto ctx = std::make_shared<MuPdfContext>();

    constexpr int kWarmup = 12;
    constexpr int kMeasured = 100;

    // Budgets. A ~2 MiB/iter commit leak grows ~200 MiB over 100 iters; a
    // healthy post-warm-up iteration plateaus far below. 40 MiB sits between:
    // above allocator/cache jitter, well under the linear leak.
    constexpr long long kCommitBudget = 40ll * 1024 * 1024;  // 40 MiB
    // A ~1/iter handle/GDI/USER leak grows ~100; healthy stays flat. K/3 == 33.
    constexpr long kHandleBudget = kMeasured / 3;

    // --- Warm-up: fill one-time MuPDF/Qt caches.
    for (int i = 0; i < kWarmup; ++i) {
        runIteration(ctx, staged, (i % 2) ? outB : outA, i, /*injectLeak=*/false);
        if (::testing::Test::HasFatalFailure()) return;
    }

    const ResourceSample base = steadySample();

    ResourceSample mid{};
    for (int i = 0; i < kMeasured; ++i) {
        runIteration(ctx, staged, (i % 2) ? outB : outA, i, injectLeak);
        if (::testing::Test::HasFatalFailure()) return;
        if (i == kMeasured / 2) mid = steadySample();
    }

    const ResourceSample fin = steadySample();

    const long long dCommit = fin.privateBytes - base.privateBytes;
    const long long dWorking = fin.workingSet - base.workingSet;
    const long dHandles = fin.handles - base.handles;
    const long dGdi = fin.gdi - base.gdi;
    const long dUser = fin.user - base.user;

    std::printf(
        "[C-02 endurance] injectLeak=%d  warmup=%d measured=%d\n"
        "  commit(PrivateUsage): base=%lld mid=%lld final=%lld  (growth %+lld B, budget %lld)\n"
        "  workingSet(RSS)     : base=%lld mid=%lld final=%lld  (growth %+lld B)\n"
        "  handles             : base=%ld mid=%ld final=%ld  (growth %+ld, budget %ld)\n"
        "  gdi                 : base=%ld mid=%ld final=%ld  (growth %+ld, budget %ld)\n"
        "  user                : base=%ld mid=%ld final=%ld  (growth %+ld, budget %ld)\n",
        injectLeak ? 1 : 0, kWarmup, kMeasured,
        base.privateBytes, mid.privateBytes, fin.privateBytes, dCommit, kCommitBudget,
        base.workingSet, mid.workingSet, fin.workingSet, dWorking,
        base.handles, mid.handles, fin.handles, dHandles, kHandleBudget,
        base.gdi, mid.gdi, fin.gdi, dGdi, kHandleBudget,
        base.user, mid.user, fin.user, dUser, kHandleBudget);
    std::fflush(stdout);

    ctx.reset();
    std::filesystem::remove(staged, ec);
    std::filesystem::remove(outA, ec);
    std::filesystem::remove(outB, ec);

    if (injectLeak) {
        std::printf(
            "[C-02 endurance] injection armed (PDFE_ENDURANCE_INJECT_LEAK=1): the "
            "product assertions below MUST fail (RED), proving the oracle catches "
            "a per-op leak.\n");
        std::fflush(stdout);
    }

    // --- Product assertions: no per-iteration leak. Each resource must stay
    // within its bounded budget after warm-up. Under the injection env var
    // these deliberately fail, proving the oracle is not vacuous.
    EXPECT_LT(dCommit, kCommitBudget)
        << "process commit grew by " << dCommit << " bytes over " << kMeasured
        << " edit/save/reopen iterations (base=" << base.privateBytes
        << " final=" << fin.privateBytes << "); a ~2 MiB/iter leak grows ~"
        << (2ll * 1024 * 1024 * kMeasured) << ". Budget=" << kCommitBudget << ".";
    EXPECT_LT(dHandles, kHandleBudget)
        << "kernel handle count grew by " << dHandles << " over " << kMeasured
        << " iterations (base=" << base.handles << " final=" << fin.handles
        << "). Budget=" << kHandleBudget << ".";
    EXPECT_LT(dGdi, kHandleBudget)
        << "GDI object count grew by " << dGdi << " over " << kMeasured
        << " iterations. Budget=" << kHandleBudget << ".";
    EXPECT_LT(dUser, kHandleBudget)
        << "USER object count grew by " << dUser << " over " << kMeasured
        << " iterations. Budget=" << kHandleBudget << ".";
}

#else  // !_WIN32

TEST(EnduranceLeakTest, SteadyStateResourceDeltaBoundedOverLongSession) {
    GTEST_SKIP() << "C-02 endurance leak oracle uses Win32 commit/handle/GDI/USER "
                    "counters; the leak class it guards is Windows-process-specific.";
}

#endif  // _WIN32
