// A1-95 — the sustained leak soak for R2-05 and R2-06.
//
// The gap-register row (gap-register.md:629) reads:
//   "[UNVERIFIABLE] R2-05/R2-06 (High) render-error pixmap/device leak +
//    per-thread context clone leak — evidence missing: no ASan soak result
//    committed"
//
// Two things about that row turned out to be wrong, and both are recorded here
// because the fix for each is in this file.
//
// 1. THE PRESCRIBED INSTRUMENT DOES NOT EXIST ON THIS PLATFORM.
//    An "ASan soak" for a LEAK requires LeakSanitizer. Measured, not assumed:
//    a deliberate unreachable allocation compiled with cl /fsanitize=address
//    and run under ASAN_OPTIONS=detect_leaks=1 gives
//
//        ==17512==AddressSanitizer: detect_leaks is not supported on this platform.
//
//    and exits 1 before any user code runs. LSan is absent from the MSVC ASan
//    runtime. The project's only ASan lane
//    (.github/workflows/windows-sanitizer.yml) therefore pins detect_leaks=0 —
//    which its comment used to explain as leak checking being "too noisy for
//    Qt/MuPDF", a rationale that cannot ever have been exercised, because the
//    setting that would produce the noise aborts the process. No amount of
//    running that lane could have produced the evidence this row asks for.
//    Corrected in the workflow; pinned by
//    SanitizerGateFaultInjection.TheSanitizerLaneNeverEnablesUnsupportedLeakDetection.
//
// 2. THE DEFECTS THEMSELVES ARE NOT UNVERIFIED — only the SOAK was missing.
//    status-register.md:278-279 records both as VERIFIED-CLOSED with named
//    fixes and named tests: TileRendererLeakTest.ThrowingRasterDoesNotLeak-
//    PixmapOrDevice (50 faulted renders through a counting allocator) and
//    MuPdfContextCloneLifecycle.* (3 cases on clone accounting). Those are
//    stronger instruments than LSan for this question — they count live bytes
//    and live clones directly rather than inferring reachability at exit.
//
//    What was genuinely absent is what the word "soak" means: SUSTAINED
//    operation. Fifty renders and three four-thread waves demonstrate that the
//    fix works; they do not demonstrate that nothing accumulates over a long
//    session, which is the failure mode a user meets and the one R2-05/R2-06
//    were originally filed as. This file supplies that, at roughly an order of
//    magnitude more cycles, and records the measured growth into the JUnit XML
//    via RecordProperty so the numbers are committed evidence rather than
//    terminal output nobody kept.
//
// ORACLE. Both halves are bounded-growth oracles, not return-to-exact-zero.
// MuPDF legitimately caches, and a strict equality would be flaky. A real
// per-cycle leak is LINEAR in the cycle count (~N tiles, ~N clones); a healthy
// run is flat after the first pass. The thresholds sit far below N and above
// the jitter, so the two are never confusable. The non-vacuity test at the
// bottom proves the oracle can actually go red by leaking a known quantity.

#include <gtest/gtest.h>

#include <mupdf/fitz.h>

#include "rendering/TileRenderInternal.h"
#include "rendering/Tile.h"
#include "core/MuPdfContext.h"

#include "support/CountingFzAllocator.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace pdfe::rendering;
using namespace pdfe::testsupport;
using pdfe::core::MuPdfContext;

namespace {

// Soak scale. Chosen so a genuine per-cycle leak is unmistakable: 400 faulted
// renders leaking a ~1 MB tile each would be ~400 MB of growth against a budget
// of one tile. Kept low enough that the case stays well under a second of CPU
// in a Debug build — a soak nobody runs is not a soak.
constexpr int kRenderCycles = 400;
constexpr int kCloneWaves   = 40;
constexpr int kCloneThreadsPerWave = 4;

// Render once through the production body, optionally faulting mid-raster.
// Mirrors the helper in test_tile_renderer_leak.cpp; kept local because the
// soak arms the gauge every pass rather than once.
bool renderOnce(fz_context* ctx, fz_display_list* list, bool fault,
                int64_t* grossOut = nullptr)
{
    g_grossSinceArm.store(0, std::memory_order_relaxed);
    if (grossOut && !fault) {
        g_faultFloorBytes = std::numeric_limits<int64_t>::max();  // measure only
    }
    g_faultArmed.store(fault || grossOut != nullptr, std::memory_order_relaxed);
    volatile bool threw = false;
    fz_try(ctx) {
        fz_pixmap* p = detail::renderTileToPixmap(ctx, list, fz_identity);
        fz_drop_pixmap(ctx, p);  // reached only if it did NOT throw
    }
    fz_catch(ctx) {
        threw = true;
    }
    g_faultArmed.store(false, std::memory_order_relaxed);
    if (grossOut) *grossOut = g_grossSinceArm.load(std::memory_order_relaxed);
    return threw;
}

// Establish a thread-local clone and exit WITHOUT releasing it — the
// QThreadPool idle-expiry path that leaked clones before the R2-06 fix.
void cloneAndExitWithoutRelease(MuPdfContext& ctx, std::atomic<int>& cloned)
{
    fz_context* clone = ctx.threadLocalClone();
    if (clone != nullptr) {
        cloned.fetch_add(1, std::memory_order_relaxed);
        ctx.execute([](fz_context*) { /* no-op MuPDF work */ });
    }
}

} // namespace

// ── R2-05, sustained: 400 throwing renders must not accumulate ──────────────
TEST(LeakSoakR205R206, ThrowingRasterSoakStaysBounded)
{
    g_counters.reset();
    g_faultArmed.store(false, std::memory_order_relaxed);
    g_faultFloorBytes = std::numeric_limits<int64_t>::max();

    const std::string pdfBytes = buildRenderablePdf();
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "pdfe_a195_render_soak.pdf";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "cannot open temp fixture " << tmp.string();
        out.write(pdfBytes.data(), static_cast<std::streamsize>(pdfBytes.size()));
        out.close();
        ASSERT_TRUE(out.good()) << "failed to write temp fixture";
    }

    fz_context* ctx = fz_new_context(&g_countingAlloc, nullptr, 64u << 20);
    ASSERT_NE(ctx, nullptr) << "fz_new_context with counting allocator failed";

    fz_document* doc = nullptr;
    fz_page* page = nullptr;
    fz_display_list* list = nullptr;

    bool setupOk = false;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, tmp.string().c_str());
        page = fz_load_page(ctx, doc, 0);
        list = fz_new_display_list_from_page(ctx, page);
        setupOk = true;
    }
    fz_catch(ctx) {
        setupOk = false;
    }
    ASSERT_TRUE(setupOk) << "fixture must open and build a display list cleanly";
    ASSERT_NE(list, nullptr);

    const int64_t oneTile = static_cast<int64_t>(TILE_SIZE) * TILE_SIZE * 4;  // ~1 MB

    // Calibrate the fault floor from one full successful render, so the OOM
    // fires after the tile pixmap + draw device exist. Without this guard the
    // fault could land before the leak window and the soak would be vacuous.
    fz_empty_store(ctx);
    int64_t totalGross = 0;
    ASSERT_FALSE(renderOnce(ctx, list, /*fault=*/false, &totalGross))
        << "calibration render of a valid page must not throw";
    ASSERT_GT(totalGross, oneTile + 65536)
        << "render must allocate meaningfully more than the tile pixmap so the "
           "OOM fault can fire mid-raster (got gross=" << totalGross << ")";
    g_faultFloorBytes = oneTile + (totalGross - oneTile) / 2;

    fz_empty_store(ctx);
    const int64_t baseBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    const int64_t baseBlocks = g_counters.liveBlocks.load(std::memory_order_relaxed);

    // Sample the midpoint too. A leak that is linear and one that plateaus after
    // a warm-up are both "growth" at the end; only the midpoint distinguishes
    // them, and the distinction is the whole claim a soak makes.
    int64_t midBytes = 0;
    int threwCount = 0;
    for (int i = 0; i < kRenderCycles; ++i) {
        fz_empty_store(ctx);
        if (renderOnce(ctx, list, /*fault=*/true)) ++threwCount;
        if (i == kRenderCycles / 2 - 1) {
            fz_empty_store(ctx);
            midBytes = g_counters.liveBytes.load(std::memory_order_relaxed) - baseBytes;
        }
    }
    fz_empty_store(ctx);

    const int64_t finalBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    const int64_t finalBlocks = g_counters.liveBlocks.load(std::memory_order_relaxed);
    const int64_t bytesGrowth = finalBytes - baseBytes;
    const int64_t blocksGrowth = finalBlocks - baseBlocks;

    // Tear down BEFORE asserting so a RED run still frees everything.
    fz_drop_display_list(ctx, list);
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // Committed evidence: these land in gtest-results.xml, which is the gate
    // artifact, so "results recorded" is satisfied by the run itself rather
    // than by a number retyped into a document.
    RecordProperty("soak_render_cycles", kRenderCycles);
    RecordProperty("soak_render_throws", threwCount);
    RecordProperty("soak_render_bytes_growth", static_cast<int>(bytesGrowth));
    RecordProperty("soak_render_bytes_growth_at_half", static_cast<int>(midBytes));
    RecordProperty("soak_render_blocks_growth", static_cast<int>(blocksGrowth));
    RecordProperty("soak_render_one_tile_bytes", static_cast<int>(oneTile));

    ASSERT_EQ(threwCount, kRenderCycles)
        << "only " << threwCount << " of " << kRenderCycles << " renders threw — "
           "the OOM fault stopped firing inside the raster path, so the rest of "
           "the soak measured the SUCCESS path and proves nothing about R2-05";

    EXPECT_LT(bytesGrowth, oneTile)
        << "live allocation grew by " << bytesGrowth << " bytes over "
        << kRenderCycles << " throwing renders (~" << (oneTile ? bytesGrowth / oneTile : 0)
        << " leaked tiles); expected < 1 tile (" << oneTile << "). R2-05.";
    EXPECT_LT(blocksGrowth, static_cast<int64_t>(kRenderCycles))
        << "live blocks grew by " << blocksGrowth << " over " << kRenderCycles
        << " throwing renders — a per-throw leak of MuPDF objects.";

    // The soak-specific claim: growth is not merely small, it is not GROWING.
    // A linear leak doubles between the halfway mark and the end.
    EXPECT_LT(bytesGrowth - midBytes, oneTile)
        << "live allocation grew a further " << (bytesGrowth - midBytes)
        << " bytes in the SECOND half of the soak (first half: " << midBytes
        << "). Flat-then-flat is healthy caching; still climbing is a leak.";
}

// ── R2-06, sustained: 160 worker threads across 40 waves ────────────────────
TEST(LeakSoakR205R206, ThreadCloneSoakReturnsToBaselineEveryWave)
{
    const size_t baseline = MuPdfContext::liveThreadCloneCount();
    MuPdfContext ctx;

    size_t worstWave = 0;
    int totalClones = 0;

    for (int wave = 0; wave < kCloneWaves; ++wave) {
        std::atomic<int> cloned{0};
        std::vector<std::thread> workers;
        workers.reserve(kCloneThreadsPerWave);
        for (int i = 0; i < kCloneThreadsPerWave; ++i) {
            workers.emplace_back(cloneAndExitWithoutRelease, std::ref(ctx), std::ref(cloned));
        }
        for (auto& t : workers) t.join();

        totalClones += cloned.load();
        const size_t live = MuPdfContext::liveThreadCloneCount();
        if (live > worstWave) worstWave = live;

        ASSERT_EQ(cloned.load(), kCloneThreadsPerWave)
            << "wave " << wave << " did not actually create its clones — the rest "
               "of the soak would measure nothing";
        ASSERT_EQ(live, baseline)
            << "live thread-clone count is " << live << " after wave " << wave
            << " (baseline " << baseline << "). Clones are accumulating across "
               "waves: every worker exited without releasing, and the thread_local "
               "destructor must reclaim each one (R2-06).";
    }

    RecordProperty("soak_clone_waves", kCloneWaves);
    RecordProperty("soak_clone_threads_total", totalClones);
    RecordProperty("soak_clone_peak_live", static_cast<int>(worstWave));
    RecordProperty("soak_clone_baseline", static_cast<int>(baseline));

    EXPECT_EQ(totalClones, kCloneWaves * kCloneThreadsPerWave);
    EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline)
        << "clone count did not return to baseline after the full soak";
}

// ── NON-VACUITY: the byte oracle must be able to fail ───────────────────────
//
// Every assertion above is satisfied by an oracle that always reads zero — a
// mis-wired allocator, a context that never routed through it, counters reset
// at the wrong moment. Leak a known quantity through the SAME counters and
// require the same comparison to reject it.
TEST(LeakSoakR205R206, TheByteOracleDetectsAnInjectedLeak)
{
    g_counters.reset();
    g_faultArmed.store(false, std::memory_order_relaxed);
    g_faultFloorBytes = std::numeric_limits<int64_t>::max();

    fz_context* ctx = fz_new_context(&g_countingAlloc, nullptr, 8u << 20);
    ASSERT_NE(ctx, nullptr);

    const int64_t baseBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    const int64_t oneTile = static_cast<int64_t>(TILE_SIZE) * TILE_SIZE * 4;

    // Deliberately orphan one tile-sized pixmap per cycle, exactly as the
    // unpatched raster path did on every throw.
    constexpr int kLeakCycles = 8;
    std::vector<fz_pixmap*> leaked;
    for (int i = 0; i < kLeakCycles; ++i) {
        fz_pixmap* p = fz_new_pixmap(ctx, fz_device_rgb(ctx), TILE_SIZE, TILE_SIZE, nullptr, 1);
        ASSERT_NE(p, nullptr);
        leaked.push_back(p);  // never dropped inside the measured window
    }

    const int64_t growth = g_counters.liveBytes.load(std::memory_order_relaxed) - baseBytes;

    EXPECT_GE(growth, oneTile)
        << "the counting allocator did not observe " << kLeakCycles
        << " orphaned tile pixmaps (growth=" << growth << "). The byte oracle "
           "used by the soaks above cannot see a leak, so their PASS means nothing.";

    // The soak's own comparison, applied to a known-leaking run, must reject it.
    EXPECT_FALSE(growth < oneTile)
        << "the soak threshold (growth < one tile) accepted a run that leaked "
        << kLeakCycles << " tiles";

    for (fz_pixmap* p : leaked) fz_drop_pixmap(ctx, p);
    fz_drop_context(ctx);
}
