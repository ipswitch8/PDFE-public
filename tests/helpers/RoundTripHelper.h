// PHASE3_ROUND_TRIP_HELPER: shared inline helpers for the
// mutate -> save -> reopen -> assert testing pattern that phases 3-5
// adopt across annotations / forms / encryption / signatures /
// PDF/A / redaction / image / metadata round-trip tests.
//
// The audit found tests that mutate PDF state but never re-open the
// output and verify the observable property. This helper makes the
// re-open step a one-liner, so test authors don't have an excuse to
// skip it.
//
// SaveAndReopen
// -------------
// Saves the document to a fresh path under the test's temp dir, then
// closes and re-opens it in a fresh Document handle. The returned
// document is the test author's source of truth for "what actually
// landed on disk" — anything still queryable through it is a real
// roundtrip, not just internal state.
//
// Usage
// -----
//   #include "helpers/RoundTripHelper.h"
//
//   TEST_F(AnnotationRoundTripTest, StickyNoteSurvivesSave) {
//       auto a = manager.createAnnotation(page, ...);
//       manager.setContents(page, a.annotIndex, "Hello");
//
//       auto reopened = pdfe::test::SaveAndReopen(*document, ctx, tempDir());
//       auto reopenedAnnots = manager.getAnnotations(reopened->page(0).get());
//       ASSERT_EQ(reopenedAnnots.size(), 1u);
//       EXPECT_EQ(reopenedAnnots[0].contents, "Hello");
//   }
//
// All helpers live in pdfe::test to avoid colliding with production
// code names. Inline + header-only — no separate .cpp.

#pragma once

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRectF>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace pdfe::test {

inline std::filesystem::path UniqueTempPath(
    const std::filesystem::path& dir,
    const std::string& stem,
    const std::string& ext = ".pdf")
{
    std::filesystem::create_directories(dir);
    static std::atomic<int> counter{0};
    int n = counter.fetch_add(1, std::memory_order_relaxed);
    auto path = dir / (stem + "_" + std::to_string(n) + ext);
    // Best-effort: caller owns the lifetime; we don't pre-clean so
    // existing-file collision surfaces as a real test failure.
    return path;
}

// Saves *doc* to a fresh path and re-opens it. Closes the local handle
// to *doc* implicitly (caller still owns the original Document; only
// the on-disk file is the contract). Returns the freshly-opened
// Document handle pointing at the saved file. Never throws — failures
// surface via ASSERT_NE on the returned pointer.
inline std::unique_ptr<pdfe::core::Document> SaveAndReopen(
    pdfe::core::Document& doc,
    std::shared_ptr<pdfe::core::MuPdfContext> ctx,
    const std::filesystem::path& tempDir,
    const std::string& stem = "roundtrip")
{
    auto out = UniqueTempPath(tempDir, stem);
    doc.saveAs(out);
    if (!std::filesystem::exists(out) ||
        std::filesystem::file_size(out) == 0) {
        return nullptr;
    }
    return pdfe::core::Document::open(ctx, out);
}

// Variant that returns BOTH the reopened Document and the path it
// wrote to, for tests that want to assert against the file (e.g.,
// file_size, byte hashes) before / after reopen.
struct SaveAndReopenResult {
    std::unique_ptr<pdfe::core::Document> doc;
    std::filesystem::path path;
};

inline SaveAndReopenResult SaveAndReopenWithPath(
    pdfe::core::Document& doc,
    std::shared_ptr<pdfe::core::MuPdfContext> ctx,
    const std::filesystem::path& tempDir,
    const std::string& stem = "roundtrip")
{
    SaveAndReopenResult r;
    r.path = UniqueTempPath(tempDir, stem);
    doc.saveAs(r.path);
    if (!std::filesystem::exists(r.path) ||
        std::filesystem::file_size(r.path) == 0) {
        return r;  // doc remains null
    }
    r.doc = pdfe::core::Document::open(ctx, r.path);
    return r;
}

// Render a page to a QImage at the given DPI for pixel-level
// roundtrip assertions. Used by phase-5 redaction tests to confirm
// the redacted rectangle's pixels match the fill color.
//
// page: the Page to render. Caller must own and keep alive.
// dpi:  output DPI; default 72 produces 1 point = 1 pixel.
// region: PDF rect to render; pass an empty QRectF to render the
//         whole page (taken from the page's bounding box).
inline QImage RenderPage(pdfe::core::Page& page,
                         double dpi = 72.0,
                         QRectF region = {})
{
    if (region.isEmpty()) {
        region = page.boundingBox();
    }
    const float scale = static_cast<float>(dpi / 72.0);
    return page.render(region, scale);
}

} // namespace pdfe::test
