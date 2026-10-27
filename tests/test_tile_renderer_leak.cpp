// Allocation-balance leak oracle for TileRenderer raster (remediation R2-05).
//
// Defect: TileRenderer::RenderTask::run()'s inner render lambda created a
// pixmap + draw device with NO fz_try/fz_always guard. When rasterization
// fz_throws, MuPDF longjmps to MuPdfContext::execute()'s fz_catch — unwinding
// past fz_close_device/fz_drop_device and orphaning the pixmap, so BOTH leak
// (~1 MB tile pixmap + draw device). Tiles are re-requested on every
// scroll/zoom, so the leak is unbounded.
//
// This oracle drives the EXACT production raster sequence
// (pdfe::rendering::detail::renderTileToPixmap, factored out of run()) N times,
// through a custom counting fz allocator, and asserts live allocation returns
// to baseline (bounded). RED before the fz_try/fz_always fix (live count grows
// ~N tiles), GREEN after (flat).
//
// Forcing the raster throw:
//   A corrupt embedded image does NOT work — MuPDF's image decoders treat a bad
//   stream as a recoverable FORMAT error ("treating as end of file") and keep
//   rendering, so fz_run_display_list never throws. Instead we inject a one-shot
//   OUT-OF-MEMORY fault through the counting allocator, mid-raster. OOM
//   (fz_throw FZ_ERROR_MEMORY) is NOT recovered — it propagates out of
//   fz_run_display_list exactly like a real raster failure, and is itself a
//   production-relevant R2-05 scenario (a large/complex tile exhausting memory
//   during rasterization).
//
//   The fixture draws a large VALID image. Its decode (a big, single, uncached
//   allocation performed lazily at raster) makes per-render allocation both
//   large and repeatable once the resource store is emptied each pass. An
//   adaptive byte floor — calibrated from one full successful render — places
//   the fault after the tile pixmap + draw device are allocated but before the
//   render completes, which is precisely the leak window the fix guards.

#include <gtest/gtest.h>

#include <mupdf/fitz.h>

#include "rendering/TileRenderInternal.h"
#include "rendering/Tile.h"

#include "support/CountingFzAllocator.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace pdfe::rendering;

namespace {

// The counting fz allocator with one-shot OOM fault injection used to be
// defined here. It moved to tests/support/CountingFzAllocator.h (A1-95) so the
// leak SOAK in tests/test_leak_soak_r205_r206.cpp drives the same instrument
// instead of a second copy that could drift from this one. Behaviour is
// unchanged; the names below now resolve into pdfe::testsupport.
using namespace pdfe::testsupport;

// The renderable-PDF fixture builder that used to sit here moved to
// tests/support/CountingFzAllocator.h alongside the allocator, for the same
// reason: the A1-95 soak needs the identical page to make the OOM fault land
// in the same window, and two copies of a calibration-sensitive fixture would
// drift. buildRenderablePdf/kImgW/kImgH now resolve into pdfe::testsupport.

// Render once through the production body. If `fault` is true the injected OOM
// is armed for exactly this render (fires once gross allocation since arm
// crosses g_faultFloorBytes). If `grossOut` is set, arms the gauge with an
// unreachable floor purely to MEASURE the render's gross allocation.
// Returns true iff the render fz_threw.
bool renderOnce(fz_context* ctx, fz_display_list* list, bool fault, int64_t* grossOut = nullptr) {
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

} // namespace

// ---------------------------------------------------------------------------
// The oracle. N throwing (OOM-faulted) renders must not grow live allocation
// beyond a single tile. Unpatched (no fz_try/fz_always guard) each throw leaks
// a full ~1 MB pixmap + draw device -> ~N tiles of growth (RED). Patched -> flat.
// ---------------------------------------------------------------------------
TEST(TileRendererLeakTest, ThrowingRasterDoesNotLeakPixmapOrDevice) {
    g_counters.reset();
    g_faultArmed.store(false, std::memory_order_relaxed);
    g_faultFloorBytes = std::numeric_limits<int64_t>::max();

    // Write the renderable fixture to a temp file (hermetic — no fixture-floor
    // dependency, cannot silently skip).
    const std::string pdfBytes = buildRenderablePdf();
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "pdfe_r205_render.pdf";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "cannot open temp fixture " << tmp.string();
        out.write(pdfBytes.data(), static_cast<std::streamsize>(pdfBytes.size()));
        out.close();
        ASSERT_TRUE(out.good()) << "failed to write temp fixture";
    }

    // Dedicated context wired to the counting allocator (64 MB store).
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

    // --- Calibrate: gross allocation of one full SUCCESSFUL render (store
    // emptied first so the image re-decodes). The fault floor is placed halfway
    // between "tile pixmap allocated" (oneTile) and full render, guaranteeing
    // the fault fires inside fz_run_display_list AFTER the pixmap + draw device.
    fz_empty_store(ctx);
    int64_t totalGross = 0;
    ASSERT_FALSE(renderOnce(ctx, list, /*fault=*/false, &totalGross))
        << "calibration render of a valid page must not throw";
    ASSERT_GT(totalGross, oneTile + 65536)
        << "render must allocate meaningfully more than the tile pixmap so the "
           "OOM fault can fire mid-raster (got gross=" << totalGross << ")";
    g_faultFloorBytes = oneTile + (totalGross - oneTile) / 2;

    // Baseline after emptying the store: only genuinely-live allocations remain
    // (the retained display list + document/page). Leaks in the loop show up as
    // growth ON TOP of this baseline.
    fz_empty_store(ctx);
    const int64_t baseBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    const int64_t baseBlocks = g_counters.liveBlocks.load(std::memory_order_relaxed);

    // --- The measured loop: N OOM-faulted renders. Empty the store each pass so
    // the image re-decodes and gross reliably reaches the fault floor. Emptying
    // reclaims legitimately-cached resources but NOT a leaked (orphaned) pixmap
    // or device — those are exactly what the oracle measures.
    const int N = 50;
    for (int i = 0; i < N; ++i) {
        fz_empty_store(ctx);
        ASSERT_TRUE(renderOnce(ctx, list, /*fault=*/true))
            << "faulted render iteration " << i << " did not throw — the OOM "
               "fault did not fire inside the raster path (floor="
            << g_faultFloorBytes << ", gross=" << totalGross << ")";
    }
    fz_empty_store(ctx);

    const int64_t finalBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    const int64_t finalBlocks = g_counters.liveBlocks.load(std::memory_order_relaxed);
    const int64_t bytesGrowth = finalBytes - baseBytes;
    const int64_t blocksGrowth = finalBlocks - baseBlocks;

    // Teardown BEFORE asserting so a RED run still frees the fixture/context.
    fz_drop_display_list(ctx, list);
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // Bounded-allocation oracle: growth over N throwing renders must stay below
    // a single tile. Unpatched leaks ~N tiles.
    EXPECT_LT(bytesGrowth, oneTile)
        << "live allocation grew by " << bytesGrowth << " bytes (~"
        << (oneTile ? bytesGrowth / oneTile : 0) << " leaked tiles) over " << N
        << " throwing renders; expected < 1 tile (" << oneTile
        << " bytes). Draw device + pixmap leak on every raster throw (R2-05).";
    EXPECT_LT(blocksGrowth, static_cast<int64_t>(N))
        << "live allocation blocks grew by " << blocksGrowth << " over " << N
        << " throwing renders — a per-throw leak of MuPDF objects.";
}
