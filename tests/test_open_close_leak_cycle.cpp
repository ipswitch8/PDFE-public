// R3-18 (Medium) — isolated OPEN/CLOSE leak-cycle oracle for Windows
// GDI / USER / kernel-handle counts.
//
// The gap (round-3 register R3-18): no ISOLATED test repeatedly opens AND
// closes documents *and* dialogs while asserting that the process-wide Windows
// resource counters (GDI objects, USER objects, kernel handles) return to a
// bounded neighbourhood of a post-warm-up baseline. The existing coverage —
// tests/test_ui_leak_detection.py (interactive psutil soak, skips without
// pywinauto) and tests/test_ui_journey_soak.py (RSS/handle TREND oracle on ONE
// long-lived document) — never isolates a repeated open->work->close cycle, so
// a per-cycle handle/GDI/USER leak (a dialog that is constructed but never
// deleted, a QPixmap/HDC orphaned per open, a document handle not released)
// would go unmeasured.
//
// This is a headless GoogleTest that runs INSIDE the existing suite (test_main
// already creates the QApplication). Each cycle:
//   * opens a real multi-page document (Document::open on test_10page.pdf),
//   * does representative work (renders two pages through the production
//     Page::render path, enumerates every page's info),
//   * destroys the Document,
//   * constructs AND destroys a representative Qt dialog (AboutDialog — the
//     simplest real PDFE QDialog that constructs headlessly with no live
//     document tree; it builds a QTextBrowser + QTextEdit, exercising Qt's
//     font/pixmap/GDI caches the way a real dialog open does),
//   * pumps the Qt event loop (processEvents) so deferred deletes run.
//
// ORACLE — warm-up + bounded-growth (methodology mirrors
// test_ui_journey_soak.py:assert_resource_bounded). Qt lazily populates font,
// pixmap, icon and glyph caches on the first few dialog/render cycles, so a
// strict return-to-exact-baseline is flaky (the caches legitimately grow once
// and then plateau). We therefore:
//   1. run WARM-UP cycles first to let those one-time caches fill,
//   2. snapshot the counters AFTER warm-up as the baseline,
//   3. run the measured N cycles,
//   4. assert each of {handles, GDI, USER} grew by LESS THAN a bounded
//      threshold.
// A genuine ~1-per-cycle leak manifests as ~N growth (unbounded, linear); a
// healthy cycle stays flat (within cache jitter) after warm-up. The threshold
// (kGrowthBudget = N/3) sits well below a real linear leak (~N) yet above the
// small jitter a healthy post-warm-up cycle produces, so it catches a real
// leak while tolerating warm-up noise. Counters are sampled robustly (idle the
// event loop, then take the steady reading) to shed transient handles.
//
// NON-VACUITY PROOF (mutation-survivor equivalent). The oracle is only
// meaningful if it can actually FAIL on a leak. A deliberate per-cycle leak —
// one leaked kernel handle (CreateEventW), one leaked GDI object
// (CreateSolidBrush) and one leaked USER object (CreateMenu) per cycle — is
// compiled in but INERT by default; it is armed only when the environment
// variable PDFE_R318_INJECT_LEAK=1 is set. With it armed each of the three
// counters climbs ~1 per cycle (~N over the loop) and every assertion goes RED;
// unset, the real cycle is leak-free and the test is GREEN. This lets a single
// build demonstrate both the GREEN pass and the RED catch (see the test's
// report), proving the assertion is not vacuous without weakening it.
//
// Windows-only: the counters are Win32 APIs. On non-Windows the body is a
// no-op that trivially passes (the leak class this guards is Windows-specific).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageInfo.h"

#include <QCoreApplication>
#include <QImage>
#include <QRectF>

#include <filesystem>
#include <memory>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <cstdlib>

#  include "ui/AboutDialog.h"
#endif

using namespace pdfe::core;

#ifdef _WIN32

namespace {

// A single snapshot of the three process-wide Windows resource counters.
struct ResourceCounts {
    long handles = 0;  // kernel handles (GetProcessHandleCount)
    long gdi = 0;      // live GDI objects (GetGuiResources / GR_GDIOBJECTS)
    long user = 0;     // live USER objects (GetGuiResources / GR_USEROBJECTS)
};

ResourceCounts rawSample() {
    ResourceCounts c;
    DWORD h = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &h)) {
        c.handles = static_cast<long>(h);
    }
    c.gdi = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    c.user = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    return c;
}

// Idle the Qt event loop so any queued deleteLater() runs and transient
// handles drain, THEN take a steady reading. Sampling several times and
// keeping the minimum sheds momentary spikes from background bookkeeping
// (thread-pool wakeups, font-config), giving the true resident level.
ResourceCounts steadySample() {
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ResourceCounts best = rawSample();
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        ResourceCounts s = rawSample();
        best.handles = std::min(best.handles, s.handles);
        best.gdi = std::min(best.gdi, s.gdi);
        best.user = std::min(best.user, s.user);
    }
    return best;
}

// One open/work/close cycle: open the document, render two pages through the
// production Page::render path, enumerate every page, destroy the document,
// then construct and destroy a representative dialog. When `injectLeak` is
// true a deliberate per-cycle leak of one kernel handle + one GDI object + one
// USER object is added (non-vacuity proof, off by default).
void runCycle(const std::shared_ptr<MuPdfContext>& context,
              const std::filesystem::path& pdfPath,
              bool injectLeak) {
    {
        std::unique_ptr<Document> doc = Document::open(context, pdfPath);
        ASSERT_NE(doc, nullptr);
        const int pages = doc->pageCount();
        ASSERT_GT(pages, 0);

        // Representative work: render the first two pages (real raster path)
        // and enumerate every page's info.
        for (int i = 0; i < pages && i < 2; ++i) {
            std::unique_ptr<Page> pg = doc->page(i);
            ASSERT_NE(pg, nullptr);
            QImage img = pg->render(pg->boundingBox(), 0.5f);
            EXPECT_FALSE(img.isNull());
        }
        for (int i = 0; i < pages; ++i) {
            PageInfo info = doc->pageInfo(i);
            (void)info;
        }
        // doc destroyed here (end of scope) — close.
    }

    // Dialog open/close: construct and destroy a real PDFE QDialog headlessly.
    {
        pdfe::ui::AboutDialog dlg(nullptr);
        // Touch it enough to force layout/text population (populates Qt's
        // font + pixmap caches, the warm-up noise the oracle tolerates).
        dlg.ensurePolished();
        (void)dlg.versionString();
        (void)dlg.sizeHint();
        // dlg destroyed here — close.
    }

    if (injectLeak) {
        // Deliberate per-cycle leak — one of EACH counter category, so all
        // three assertions are proven catchable. Inert unless armed by env.
        HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // kernel handle
        (void)h;
        HGDIOBJ brush = CreateSolidBrush(RGB(1, 2, 3));           // GDI object
        (void)brush;
        HMENU menu = CreateMenu();                                // USER object
        (void)menu;
        // Intentionally never released.
    }

    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}  // namespace

TEST(OpenCloseLeakCycleTest, ResourceCountsBoundedAfterWarmup) {
    std::filesystem::path pdfPath =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_10page.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdfPath))
        << "required fixture missing: " << pdfPath.string();

    const bool injectLeak = [] {
        const char* v = std::getenv("PDFE_R318_INJECT_LEAK");
        return v && v[0] == '1';
    }();

    auto context = std::make_shared<MuPdfContext>();

    constexpr int kWarmupCycles = 8;
    constexpr int kMeasuredCycles = 45;
    // A real ~1-per-cycle leak grows a counter by ~kMeasuredCycles (45). A
    // healthy post-warm-up cycle stays flat (small cache jitter). The budget
    // sits between: N/3 == 15. Catches the linear leak (>=45 >> 15) yet
    // tolerates warm-up/jitter noise.
    constexpr long kGrowthBudget = kMeasuredCycles / 3;

    // --- Warm-up: fill Qt's one-time font/pixmap/glyph caches so their growth
    // is not misattributed to a leak.
    for (int i = 0; i < kWarmupCycles; ++i) {
        runCycle(context, pdfPath, /*injectLeak=*/false);
    }

    const ResourceCounts base = steadySample();

    // --- Measured loop.
    ResourceCounts mid{};
    for (int i = 0; i < kMeasuredCycles; ++i) {
        runCycle(context, pdfPath, injectLeak);
        if (i == kMeasuredCycles / 2) {
            mid = steadySample();
        }
    }

    const ResourceCounts fin = steadySample();

    const long dHandles = fin.handles - base.handles;
    const long dGdi = fin.gdi - base.gdi;
    const long dUser = fin.user - base.user;

    // Human-readable evidence in the test log (warm-up baseline vs final).
    std::printf(
        "[R3-18] injectLeak=%d  warmup=%d measured=%d budget=%ld\n"
        "        handles: base=%ld mid=%ld final=%ld  (growth %+ld)\n"
        "        gdi    : base=%ld mid=%ld final=%ld  (growth %+ld)\n"
        "        user   : base=%ld mid=%ld final=%ld  (growth %+ld)\n",
        injectLeak ? 1 : 0, kWarmupCycles, kMeasuredCycles, kGrowthBudget,
        base.handles, mid.handles, fin.handles, dHandles,
        base.gdi, mid.gdi, fin.gdi, dGdi,
        base.user, mid.user, fin.user, dUser);
    std::fflush(stdout);

    context.reset();

    if (injectLeak) {
        // Non-vacuity proof mode (opt-in via PDFE_R318_INJECT_LEAK=1): the
        // deliberate per-cycle leak makes each counter grow ~1/cycle, so the
        // SAME product EXPECT_LT assertions below MUST fail (the test goes
        // RED). This proves the oracle is not vacuous — it actually catches a
        // real per-cycle handle/GDI/USER leak. Inert (never taken) by default.
        std::printf(
            "[R3-18] injection armed: expecting the product assertions below "
            "to FAIL (RED) — this demonstrates the oracle catches a leak.\n");
        std::fflush(stdout);
    }

    // --- Product assertion: no per-cycle leak. Each counter must stay within
    // the bounded budget after warm-up. (Under PDFE_R318_INJECT_LEAK=1 these
    // deliberately fail, proving the oracle can catch a leak.)
    EXPECT_LT(dHandles, kGrowthBudget)
        << "kernel handle count grew by " << dHandles << " over "
        << kMeasuredCycles << " open/close cycles (base=" << base.handles
        << " final=" << fin.handles << "); a ~1-per-cycle handle leak grows ~"
        << kMeasuredCycles << ". Budget=" << kGrowthBudget << ".";
    EXPECT_LT(dGdi, kGrowthBudget)
        << "GDI object count grew by " << dGdi << " over " << kMeasuredCycles
        << " open/close cycles (base=" << base.gdi << " final=" << fin.gdi
        << "). Budget=" << kGrowthBudget << ".";
    EXPECT_LT(dUser, kGrowthBudget)
        << "USER object count grew by " << dUser << " over " << kMeasuredCycles
        << " open/close cycles (base=" << base.user << " final=" << fin.user
        << "). Budget=" << kGrowthBudget << ".";
}

#else  // !_WIN32

TEST(OpenCloseLeakCycleTest, ResourceCountsBoundedAfterWarmup) {
    GTEST_SKIP() << "R3-18 GDI/USER/handle leak-cycle oracle is Windows-only "
                    "(the resource-counter class it guards is Win32-specific).";
}

#endif  // _WIN32
