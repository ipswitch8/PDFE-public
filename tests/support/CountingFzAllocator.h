// Counting fz allocator with one-shot OOM fault injection.
//
// Extracted from tests/test_tile_renderer_leak.cpp (remediation R2-05) so the
// A1-95 leak soak (tests/test_leak_soak_r205_r206.cpp) drives the SAME
// instrument rather than a second copy that could drift from it. Both include
// this header; neither owns it.
//
// Why an in-suite allocator rather than a sanitizer
// -------------------------------------------------
// The obvious instrument for a leak is LeakSanitizer, and A1-95 asks for
// exactly that ("no ASan soak result committed"). It does not exist here.
// Measured against the MSVC ASan runtime this project's sanitizer lane uses,
// a deliberate unreachable allocation under ASAN_OPTIONS=detect_leaks=1 gives:
//
//     ==NNNNN==AddressSanitizer: detect_leaks is not supported on this platform.
//
// and the process exits 1 before running any user code. detect_leaks=0 in
// .github/workflows/windows-sanitizer.yml is therefore not a noise-tuning
// choice — it is the only value that runs. No amount of soaking that lane can
// produce leak evidence.
//
// This allocator is a stronger instrument for the question anyway: it counts
// LIVE BYTES AND BLOCKS on a dedicated fz_context, so a leak is measured
// directly and deterministically rather than inferred from a reachability
// sweep at exit.
//
// Live counters: every malloc/realloc increments a live-byte/live-block
// counter; every free decrements it. A leak-free run returns the counters to
// their post-setup baseline; a leak leaves them elevated by (leaked bytes).
// The block size is stashed in a 16-byte header prefix (preserving payload
// alignment) so free/realloc adjust the byte counter without a side table.
//
// Fault gauge (independent of the live counters): when armed, each allocation
// adds its size to a "gross since arm" gauge; once the gauge reaches an
// adaptive floor the allocator disarms itself and returns NULL exactly once,
// which makes MuPDF fz_throw an out-of-memory error from inside the current
// operation. OOM is not a recoverable FORMAT error, so it propagates out of
// fz_run_display_list exactly like a real raster failure.
//
// THREADING NOTE. The counters and the gauge are process-global inline
// variables, because an fz_alloc_context passes only a single user pointer and
// both tests want a stable address for it. They are therefore shared by every
// test in a shard. GoogleTest runs cases sequentially, and each user calls
// reset()/rearm before measuring, so this is safe — but a test that allocates
// through this allocator on a background thread while another test measures
// would corrupt both readings. Do not do that.

#pragma once

#include <mupdf/fitz.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace pdfe::testsupport {

struct AllocCounters {
    std::atomic<int64_t> liveBytes{0};
    std::atomic<int64_t> liveBlocks{0};
    void reset() {
        liveBytes.store(0, std::memory_order_relaxed);
        liveBlocks.store(0, std::memory_order_relaxed);
    }
};

inline AllocCounters g_counters;

inline std::atomic<bool> g_faultArmed{false};
inline std::atomic<int64_t> g_grossSinceArm{0};
inline int64_t g_faultFloorBytes = std::numeric_limits<int64_t>::max();

inline constexpr size_t kHdr = 16;

// Returns true (and disarms) if this allocation should fault.
inline bool shouldFault(size_t size) {
    if (!g_faultArmed.load(std::memory_order_relaxed)) return false;
    const int64_t g =
        g_grossSinceArm.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed) +
        static_cast<int64_t>(size);
    if (g >= g_faultFloorBytes) {
        g_faultArmed.store(false, std::memory_order_relaxed);  // one-shot
        return true;
    }
    return false;
}

inline void* counting_malloc(void* user, size_t size) {
    if (shouldFault(size)) return nullptr;  // -> MuPDF fz_throw OOM
    auto* c = static_cast<AllocCounters*>(user);
    if (size == 0) size = 1;
    auto* base = static_cast<unsigned char*>(std::malloc(size + kHdr));
    if (!base) return nullptr;
    std::memcpy(base, &size, sizeof(size_t));
    c->liveBytes.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
    c->liveBlocks.fetch_add(1, std::memory_order_relaxed);
    return base + kHdr;
}

inline void counting_free(void* user, void* ptr) {
    if (!ptr) return;
    auto* c = static_cast<AllocCounters*>(user);
    auto* base = static_cast<unsigned char*>(ptr) - kHdr;
    size_t size = 0;
    std::memcpy(&size, base, sizeof(size_t));
    c->liveBytes.fetch_sub(static_cast<int64_t>(size), std::memory_order_relaxed);
    c->liveBlocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(base);
}

inline void* counting_realloc(void* user, void* old, size_t size) {
    if (!old) return counting_malloc(user, size);
    if (size == 0) {
        counting_free(user, old);
        return nullptr;
    }
    auto* c = static_cast<AllocCounters*>(user);
    auto* base = static_cast<unsigned char*>(old) - kHdr;
    size_t oldSize = 0;
    std::memcpy(&oldSize, base, sizeof(size_t));
    // Realloc growth counts toward the fault gauge.
    if (size > oldSize && shouldFault(size - oldSize)) return nullptr;
    auto* nb = static_cast<unsigned char*>(std::realloc(base, size + kHdr));
    if (!nb) return nullptr;
    std::memcpy(nb, &size, sizeof(size_t));
    c->liveBytes.fetch_add(static_cast<int64_t>(size) - static_cast<int64_t>(oldSize),
                           std::memory_order_relaxed);
    return nb + kHdr;
}

inline fz_alloc_context g_countingAlloc = {
    &g_counters, counting_malloc, counting_realloc, counting_free
};

// ---------------------------------------------------------------------------
// Fixture: a minimal, fully valid PDF whose single page draws a large raw
// (unfiltered) DeviceRGB image scaled to fill the tile. The image is decoded
// lazily at raster time — a big, single, uncached allocation — so per-render
// allocation is large and, once the resource store is emptied, repeatable.
//
// This matters for the fault gauge: the floor is calibrated from one full
// successful render, and only a page whose gross allocation meaningfully
// exceeds the tile pixmap lets the OOM fire AFTER the pixmap and draw device
// exist — which is the only window in which the R2-05 leak is observable. A
// text-only page allocates too little and would make the oracle vacuous.
// ---------------------------------------------------------------------------
inline constexpr int kImgW = 256;
inline constexpr int kImgH = 256;

inline std::string buildRenderablePdf() {
    // Raw RGB samples (a gradient, so it isn't degenerate). Decoding this at
    // raster allocates a fresh ~196 KB pixmap every render.
    std::string img;
    img.reserve(static_cast<size_t>(kImgW) * kImgH * 3);
    for (int y = 0; y < kImgH; ++y) {
        for (int x = 0; x < kImgW; ++x) {
            img.push_back(static_cast<char>(x & 0xFF));
            img.push_back(static_cast<char>(y & 0xFF));
            img.push_back(static_cast<char>((x + y) & 0xFF));
        }
    }

    const std::string content = "q 512 0 0 512 0 0 cm /Im0 Do Q";

    const std::string imgDict =
        "<< /Type /XObject /Subtype /Image /Width " + std::to_string(kImgW) +
        " /Height " + std::to_string(kImgH) +
        " /BitsPerComponent 8 /ColorSpace /DeviceRGB /Length " +
        std::to_string(img.size()) + " >>";
    const std::string contentDict = "<< /Length " + std::to_string(content.size()) + " >>";

    std::string pdf = "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(6, 0);

    auto emitObj = [&](int n, const std::string& dict, const std::string* stream) {
        offsets[static_cast<size_t>(n)] = pdf.size();
        pdf += std::to_string(n) + " 0 obj\n" + dict + "\n";
        if (stream) {
            pdf += "stream\n";
            pdf += *stream;
            pdf += "\nendstream\n";
        }
        pdf += "endobj\n";
    };

    emitObj(1, "<< /Type /Catalog /Pages 2 0 R >>", nullptr);
    emitObj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>", nullptr);
    emitObj(3,
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>",
            nullptr);
    emitObj(4, imgDict, &img);
    emitObj(5, contentDict, &content);

    const size_t xrefOff = pdf.size();
    pdf += "xref\n0 6\n";
    pdf += "0000000000 65535 f \n";
    for (int n = 1; n <= 5; ++n) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[static_cast<size_t>(n)]);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xrefOff) + "\n%%EOF\n";
    return pdf;
}

} // namespace pdfe::testsupport
