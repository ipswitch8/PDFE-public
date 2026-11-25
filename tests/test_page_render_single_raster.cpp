// -*- coding: utf-8 -*-
//
// ui-tweaks-3 phase-2: Page::render draws ONE raster.
//
// Before: Page::render asked MuPDF for a pixmap (fz_new_pixmap_from_page) and
// then allocated a QImage of the same size and copied it row by row while the
// pixmap was still alive, so a whole-page render held two full rasters at
// once. For the 14400 x 14400 pt sheet in the large-PDF repro at scale 2 that
// is 2 x 3.09 GB. The copy loop also computed `samples + y * stride` with two
// ints, which is signed overflow once y * stride passes INT_MAX (from row 18642
// of a 28800-pixel-wide RGBA raster).
//
// After: Page::render allocates the QImage and has MuPDF draw straight into
// its bits (fz_new_pixmap_with_bbox_and_data over the QImage buffer), the same
// steps fz_new_pixmap_from_page_with_separations takes (MuPDF 1.26
// source/fitz/util.c:203) with no second buffer and no copy.
//
// ORACLES.
//   * Byte equality with MuPDF's own fz_new_pixmap_from_page on the same
//     fz_page, same ctm, every pixel byte, on pages built here with explicit
//     geometry: no CropBox, /Rotate 90 and 270, a non-zero MediaBox origin, a
//     CropBox inset with an offset origin; each carrying an opaque gradient
//     image and a TRANSPARENT (alpha) image near the bottom of the page, plus
//     the scanned fixture. Several scales, including fractional ones whose
//     bounds round.
//   * Failure paths: a raster too big to allocate gives a null QImage; a
//     MuPDF error mid-draw (injected through the test seam) still surfaces as
//     MuPdfException as before, and the next render is byte-identical again
//     (nothing leaked into the fz error stack or the page).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "render_band_reference.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {

std::string fixture(const char* name)
{
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

struct Layout {
    const char* name;
    std::array<float, 4> media;
    std::optional<std::array<float, 4>> crop;
    int rotate;
};

const Layout kLayouts[] = {
    {"Plain", {0, 0, 612, 792}, std::nullopt, 0},
    {"Rotate90", {0, 0, 612, 792}, std::nullopt, 90},
    {"Rotate270CropInset", {0, 0, 612, 792}, std::array<float, 4>{40, 50, 572, 742}, 270},
    {"NonZeroMediaBoxOrigin", {100, 200, 712, 992}, std::nullopt, 0},
    {"CropBoxOffsetOrigin", {100, 200, 712, 992}, std::array<float, 4>{150, 260, 650, 940}, 0},
};

const float kScales[] = {0.5f, 1.0f, 1.3333f, 2.0f, 2.37f};

// MuPDF's own whole-page render of the same fz_page: the reference.
struct RefRaster {
    int w = 0, h = 0;
    std::vector<unsigned char> bytes;  // w * 4 per row, RGBA
};

RefRaster mupdfReference(MuPdfContext& ctx, Page& page, float scale)
{
    RefRaster r;
    ctx.execute([&](fz_context* c) {
        fz_pixmap* pix = fz_new_pixmap_from_page(c, PageRenderTestAccess::fzPage(page), fz_scale(scale, scale),
                                                 fz_device_rgb(c), 1);
        // Accessors, not fields: fz_pixmap's layout is MuPDF-internal.
        r.w = fz_pixmap_width(c, pix);
        r.h = fz_pixmap_height(c, pix);
        const unsigned char* samples = fz_pixmap_samples(c, pix);
        const ptrdiff_t stride = fz_pixmap_stride(c, pix);
        r.bytes.resize(static_cast<size_t>(r.w) * 4 * r.h);
        for (int y = 0; y < r.h; ++y) {
            std::memcpy(r.bytes.data() + static_cast<size_t>(y) * r.w * 4,
                        samples + static_cast<ptrdiff_t>(y) * stride,
                        static_cast<size_t>(r.w) * 4);
        }
        fz_drop_pixmap(c, pix);
    });
    return r;
}

// Every pixel byte; on a mismatch, where.
::testing::AssertionResult sameRaster(const QImage& img, const RefRaster& ref)
{
    if (img.isNull()) return ::testing::AssertionFailure() << "Page::render returned null";
    if (img.format() != QImage::Format_RGBA8888)
        return ::testing::AssertionFailure() << "format " << int(img.format());
    if (img.width() != ref.w || img.height() != ref.h)
        return ::testing::AssertionFailure() << "size " << img.width() << "x" << img.height()
                                             << " vs MuPDF " << ref.w << "x" << ref.h;
    int badRows = 0, firstBad = -1;
    for (int y = 0; y < ref.h; ++y) {
        if (std::memcmp(img.constScanLine(y), ref.bytes.data() + static_cast<size_t>(y) * ref.w * 4,
                        static_cast<size_t>(ref.w) * 4) != 0) {
            if (firstBad < 0) firstBad = y;
            ++badRows;
        }
    }
    if (badRows)
        return ::testing::AssertionFailure() << badRows << " rows differ, first " << firstBad;
    return ::testing::AssertionSuccess();
}

// Count of pixels with any alpha: a raster that is blank everywhere would make
// equality vacuous, so each fixture must draw something.
long inkedPixels(const RefRaster& r)
{
    long n = 0;
    for (size_t i = 3; i < r.bytes.size(); i += 4) n += r.bytes[i] != 0;
    return n;
}

long inkedRowsInBottomQuarter(const RefRaster& r)
{
    long rows = 0;
    for (int y = r.h * 3 / 4; y < r.h; ++y) {
        const unsigned char* row = r.bytes.data() + static_cast<size_t>(y) * r.w * 4;
        for (int x = 0; x < r.w; ++x) {
            if (row[x * 4 + 3]) {
                ++rows;
                break;
            }
        }
    }
    return rows;
}

}  // namespace

class PageRenderSingleRasterTest : public ::testing::TestWithParam<Layout> {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";

        gradient_ = path("gradient.png");
        QImage g(64, 48, QImage::Format_RGB32);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) g.setPixel(x, y, qRgb(x * 4, y * 5, 160));
        ASSERT_TRUE(g.save(QString::fromStdString(gradient_)));

        // Transparent: alpha ramps 0..255 across, colour down.
        alpha_ = path("alpha.png");
        QImage a(80, 40, QImage::Format_ARGB32);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x) a.setPixel(x, y, qRgba(200, y * 6, 40, x * 255 / 79));
        ASSERT_TRUE(a.save(QString::fromStdString(alpha_)));
    }

    std::string path(const char* leaf) const { return tmp_.filePath(leaf).toStdString(); }

    // test_single.pdf with the page's boxes and /Rotate set from `lay`, then
    // (through ImageManager, on the page as Page sees it) an opaque gradient
    // near the top and a TRANSPARENT image in the bottom fifth of the visible
    // page, both placed relative to that page's own boundingBox() so every
    // layout draws them, last rows included. Returns the saved path.
    std::string build(const Layout& lay)
    {
        const std::string boxed = path((std::string(lay.name) + "-boxes.pdf").c_str());
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, base_.c_str());
            pdf_obj* pobj = pdf_lookup_page_obj(ctx, pdf, 0);
            const auto& m = lay.media;
            pdf_dict_put_rect(ctx, pobj, PDF_NAME(MediaBox), fz_make_rect(m[0], m[1], m[2], m[3]));
            if (lay.crop) {
                const auto& c = *lay.crop;
                pdf_dict_put_rect(ctx, pobj, PDF_NAME(CropBox), fz_make_rect(c[0], c[1], c[2], c[3]));
            } else {
                pdf_dict_del(ctx, pobj, PDF_NAME(CropBox));
            }
            pdf_dict_put_int(ctx, pobj, PDF_NAME(Rotate), lay.rotate);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, boxed.c_str(), &wo);
            pdf_drop_document(ctx, pdf);
        });
        const std::string out = path((std::string(lay.name) + ".pdf").c_str());
        auto doc = Document::open(context_, boxed);
        EXPECT_NE(doc, nullptr);
        if (!doc) return {};
        auto page = doc->page(0);
        const QRectF bb = page->boundingBox();
        const QRectF top(bb.x() + bb.width() * 0.2, bb.y() + bb.height() * 0.1,
                         bb.width() * 0.3, bb.height() * 0.15);
        const QRectF bottom(bb.x() + bb.width() * 0.45, bb.bottom() - bb.height() * 0.18,
                            bb.width() * 0.4, bb.height() * 0.15);
        for (const auto& [file, rect] : {std::pair{gradient_, top}, std::pair{alpha_, bottom}}) {
            InsertImageOptions o;
            o.filePath = file;
            o.targetRect = rect;
            o.maintainAspectRatio = false;
            auto r = manager_->insertImage(page.get(), doc.get(), o);
            EXPECT_TRUE(r.success) << r.errorMessage;
        }
        page.reset();
        doc->saveAs(out);
        return out;
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string base_, gradient_, alpha_;
};

TEST_P(PageRenderSingleRasterTest, IsByteIdenticalToMuPdfsOwnPageRender)
{
    const Layout& lay = GetParam();
    const std::string file = build(lay);
    ASSERT_FALSE(file.empty());
    auto doc = Document::open(context_, file);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    for (float s : kScales) {
        SCOPED_TRACE(std::string(lay.name) + " scale " + std::to_string(s));
        const RefRaster ref = mupdfReference(*context_, *page, s);
        ASSERT_GT(inkedPixels(ref), 1000) << "precondition: the page draws something";
        const QImage img = page->render(page->boundingBox(), s);
        EXPECT_TRUE(sameRaster(img, ref));
    }
}

// The transparent image sits in the bottom fifth of every layout's visible
// page, so the byte comparison above covers inked last rows too, not only
// blank paper.
TEST_P(PageRenderSingleRasterTest, TheComparedRasterHasInkInItsLastRows)
{
    const Layout& lay = GetParam();
    const std::string file = build(lay);
    auto doc = Document::open(context_, file);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const RefRaster ref = mupdfReference(*context_, *page, 1.0f);
    EXPECT_GT(inkedRowsInBottomQuarter(ref), 20) << lay.name;
}

INSTANTIATE_TEST_SUITE_P(Layouts, PageRenderSingleRasterTest, ::testing::ValuesIn(kLayouts),
                         [](const ::testing::TestParamInfo<Layout>& i) {
                             return std::string(i.param.name);
                         });

class PageRenderSingleRasterFixtureTest : public ::testing::Test {
protected:
    void SetUp() override { context_ = std::make_shared<MuPdfContext>(); }
    std::shared_ptr<MuPdfContext> context_;
};

TEST_F(PageRenderSingleRasterFixtureTest, TheScannedFixtureIsByteIdentical)
{
    const std::string f = fixture("test_scanned.pdf");
    ASSERT_FALSE(f.empty());
    auto doc = Document::open(context_, f);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    for (float s : {0.75f, 1.0f, 1.5f}) {
        SCOPED_TRACE(s);
        const RefRaster ref = mupdfReference(*context_, *page, s);
        ASSERT_GT(inkedPixels(ref), 1000);
        EXPECT_TRUE(sameRaster(page->render(page->boundingBox(), s), ref));
    }
}

// The large-PDF repro checks regions of a 3 GB raster against a band render
// (render_band_reference.h). That oracle is only as good as its agreement with
// a whole-page render, so check it here on every layout's pages, at the
// regions holding the two inserted images and the page's last rows.
TEST_P(PageRenderSingleRasterTest, ABandRenderMatchesTheWholePageRender)
{
    const Layout& lay = GetParam();
    const std::string file = build(lay);
    auto doc = Document::open(context_, file);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    for (float s : {1.0f, 2.0f, 2.37f}) {
        SCOPED_TRACE(std::string(lay.name) + " scale " + std::to_string(s));
        const QImage img = page->render(page->boundingBox(), s);
        ASSERT_FALSE(img.isNull());
        const QRect bands[] = {
            QRect(0, 0, img.width(), img.height()),
            QRect(img.width() / 5, img.height() / 9, img.width() / 2, img.height() / 4),
            QRect(0, img.height() - 64, img.width(), 64),
            QRect(img.width() / 3, img.height() * 3 / 4, img.width() / 2, img.height() / 4),
        };
        // Exact at integer scales, the ones the repro uses. At a fractional
        // scale MuPDF's image scaler picks its source subarea from the clip,
        // so a band's image pixels can differ slightly from a whole-page
        // render (measured: 436 of 340025 pixels, channel delta <= 5, in the
        // gradient's band at 2.37): bounded there, not exact.
        const bool integerScale = s == 1.0f || s == 2.0f;
        long inked = 0;
        for (const QRect& b : bands) {
            const auto cmp = pdfe::testing::compareBandToReference(*context_, *page, s, img, b);
            if (integerScale) {
                EXPECT_TRUE(cmp.ok()) << cmp.describe();
            } else {
                EXPECT_TRUE(cmp.error.empty() && cmp.pixels > 0) << cmp.describe();
                EXPECT_LE(cmp.differing, cmp.pixels / 200) << cmp.describe();
                EXPECT_LE(cmp.maxDelta, 8) << cmp.describe();
            }
            inked += cmp.inked;
        }
        EXPECT_GT(inked, 1000);
    }
}

// Too large to allocate: a null image, not a crash and not an exception.
// (Before, the pixmap allocation threw; and when the pixmap fit but the second,
// same-sized QImage did not, the copy wrote through a null scanLine: the
// large-PDF repro capped at 5.7 GiB of address space died on signal 11.)
TEST_F(PageRenderSingleRasterFixtureTest, ARasterTooLargeToAllocateIsNull)
{
    const std::string f = fixture("test_single.pdf");
    auto doc = Document::open(context_, f);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    // fz_round_rect clamps the bounds to MuPDF's safe integer range (2^24),
    // so 1e5 gives a 16777216-pixel-square raster: ~1.1e15 bytes, which no
    // allocation satisfies.
    QImage huge;
    EXPECT_NO_THROW(huge = page->render(page->boundingBox(), 1.0e5f));
    EXPECT_TRUE(huge.isNull());
    // And the page still renders normally afterwards.
    const RefRaster ref = mupdfReference(*context_, *page, 1.0f);
    EXPECT_TRUE(sameRaster(page->render(page->boundingBox(), 1.0f), ref));
}

// A MuPDF error mid-draw: as before the change, Page::render throws
// MuPdfException (MuPdfContext::execute's translation). The device and the
// pixmap header are dropped on that path (fz_always), the fz error stack is
// balanced, and the next render is byte-identical to MuPDF's.
TEST_F(PageRenderSingleRasterFixtureTest, AMuPdfErrorMidDrawThrowsAndLeavesThePageUsable)
{
    const std::string f = fixture("test_scanned.pdf");
    auto doc = Document::open(context_, f);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    Page::failNextRenderForTest();
    EXPECT_THROW((void)page->render(page->boundingBox(), 1.0f), MuPdfException);
    // One-shot: the seam is spent.
    const RefRaster ref = mupdfReference(*context_, *page, 1.0f);
    EXPECT_TRUE(sameRaster(page->render(page->boundingBox(), 1.0f), ref));
    // Repeatedly: the error path is repeatable and leaves the page rendering
    // exactly. (Whether the device and pixmap header are freed on that path
    // is not measured here; Page::render drops them in fz_always.)
    for (int i = 0; i < 50; ++i) {
        Page::failNextRenderForTest();
        EXPECT_THROW((void)page->render(page->boundingBox(), 0.5f), MuPdfException);
    }
    EXPECT_TRUE(sameRaster(page->render(page->boundingBox(), 1.0f), ref));
}

// ===========================================================================
// THE MEMORY PROPERTY, as a control (claim-check, ui-tweaks-3 phase-2): a
// whole-page render must fit in ONE raster plus slack. Linux-only by #if, as
// test_preflight_sandbox_posix.cpp: the mechanism is RLIMIT_AS on a forked
// child, so the cap never touches the test process.
//
// The child opens test_single.pdf, renders it once at scale 1 (fonts and
// caches loaded), reads its address-space size, caps RLIMIT_AS at that plus
// 1.5 rasters of the page at scale 14 (8568 x 11088 RGBA, 380 MB), and runs
// one render. Page::render must succeed there. As controls under the same
// cap, the OLD shape (fz_new_pixmap_from_page, then a same-sized QImage while
// the pixmap is alive) must fail at its SECOND allocation, and under a cap of
// 2.5 rasters it must succeed: so the 1.5-raster cap, not something else, is
// what separates one raster from two.
// ===========================================================================
#if defined(__linux__)

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>

namespace {

enum BudgetResult : int {
    kBudgetOk = 0,
    kBudgetNull = 10,          // Page::render returned a null image
    kBudgetWrongSize = 11,
    kBudgetThrew = 12,
    kBudgetSetupFailed = 13,
    kBudgetFirstAllocFailed = 14,   // control: the pixmap itself did not fit
    kBudgetSecondAllocFailed = 15,  // control: the pixmap fit, the copy did not
    kBudgetNoChild = 99,
};

constexpr float kBudgetScale = 14.0f;

long long addressSpaceBytes()
{
    long long pages = -1;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%lld", &pages) != 1) pages = -1;
        std::fclose(f);
    }
    return pages < 0 ? -1 : pages * static_cast<long long>(sysconf(_SC_PAGESIZE));
}

// Run @p body in a forked child under RLIMIT_AS = (address space after a warm
// render) + rasters * (one raster at kBudgetScale). Returns the child's code.
template <class Body>
int runUnderBudget(double rasters, Body body)
{
    const std::string file = fixture("test_single.pdf");
    if (file.empty()) return kBudgetSetupFailed;
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return kBudgetNoChild;
    if (pid == 0) {
        int code = kBudgetSetupFailed;
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, file);
            auto page = doc ? doc->page(0) : nullptr;
            if (page) {
                (void)page->render(page->boundingBox(), 1.0f);  // warm: fonts, store
                const QRectF bb = page->boundingBox();
                const long long w = static_cast<long long>(std::ceil(bb.width() * kBudgetScale));
                const long long h = static_cast<long long>(std::ceil(bb.height() * kBudgetScale));
                const long long raster = w * h * 4;
                const long long now = addressSpaceBytes();
                if (now > 0) {
                    const rlim_t cap = static_cast<rlim_t>(now + rasters * raster);
                    rlimit lim{cap, cap};
                    if (setrlimit(RLIMIT_AS, &lim) == 0) code = body(*ctx, *page, w, h);
                }
            }
        } catch (...) {
            code = kBudgetThrew;
        }
        _exit(code);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return kBudgetNoChild;
    return WEXITSTATUS(status);
}

int renderWithPage(MuPdfContext&, Page& page, long long w, long long h)
{
    try {
        const QImage img = page.render(page.boundingBox(), kBudgetScale);
        if (img.isNull()) return kBudgetNull;
        return (img.width() == w && img.height() == h) ? kBudgetOk : kBudgetWrongSize;
    } catch (...) {
        return kBudgetThrew;
    }
}

// The pre-ui-tweaks-3 shape: MuPDF's pixmap, then a same-sized QImage while
// the pixmap is still alive (the old copy). Nothing is written through a null
// image here; the point is only which allocation fails.
int renderTwoBuffers(MuPdfContext& ctx, Page& page, long long, long long)
{
    fz_pixmap* pix = nullptr;
    try {
        pix = ctx.executeReturn<fz_pixmap*>([&](fz_context* c) {
            return fz_new_pixmap_from_page(c, PageRenderTestAccess::fzPage(page),
                                           fz_scale(kBudgetScale, kBudgetScale),
                                           fz_device_rgb(c), 1);
        });
    } catch (...) {
        return kBudgetFirstAllocFailed;
    }
    int code = kBudgetOk;
    {
        int w = 0, h = 0;
        ctx.execute([&](fz_context* c) {
            w = fz_pixmap_width(c, pix);
            h = fz_pixmap_height(c, pix);
        });
        QImage copy(w, h, QImage::Format_RGBA8888);
        if (copy.isNull()) code = kBudgetSecondAllocFailed;
        else std::memset(copy.bits(), 0, static_cast<size_t>(copy.sizeInBytes()));
    }
    ctx.execute([&](fz_context* c) { fz_drop_pixmap(c, pix); });
    return code;
}

}  // namespace

TEST(PageRenderMemoryBudgetTest, RendersInsideOneAndAHalfRasters)
{
    EXPECT_EQ(runUnderBudget(1.5, renderWithPage), kBudgetOk)
        << "Page::render needs more than 1.5 rasters of address space: is a second "
           "full-size buffer back? (10 = null image, 12 = threw)";
}

TEST(PageRenderMemoryBudgetTest, TheTwoBufferShapeFailsAtItsSecondAllocationUnderTheSameCap)
{
    EXPECT_EQ(runUnderBudget(1.5, renderTwoBuffers), kBudgetSecondAllocFailed)
        << "control: under 1.5 rasters the pixmap must fit and its copy must not";
}

TEST(PageRenderMemoryBudgetTest, TheTwoBufferShapeFitsInTwoAndAHalfRasters)
{
    EXPECT_EQ(runUnderBudget(2.5, renderTwoBuffers), kBudgetOk)
        << "control: with room for two rasters the two-buffer shape must succeed";
}

#endif  // __linux__
