// -*- coding: utf-8 -*-
//
// Save crops what hangs off the page (ui-tweaks phase-4).
//
// Phase-3 shades the part of a placed image that lies outside its page and
// says, in kImageOverflowCropTooltip, that it "will be cropped when the
// document is saved". These tests hold the save to that:
//
//   1. GEOMETRY. pageOverflowCropRect, the crop the save applies: none for an
//      image on the page, at the exact page edge, wholly off the page or in a
//      Form XObject; the page edge for an upright overhang.
//   2. ROUND TRIP. A file with overhanging placements is opened in a real
//      MainWindow and saved through the real Save / Save As actions; the SAVED
//      file is then reopened from disk and checked:
//        * every placement's visible quad (pageCtm applied to its unit crop)
//          lies inside the page box (exactly for upright, 90-degree and
//          flipped placements; for 30 degrees see below);
//        * no image pixel is drawn outside the page box: the page content is
//          run through MuPDF's draw device onto a canvas larger than the page
//          (no page-box clip), so whatever lies outside the page shows up;
//        * every in-page pixel is what it was before the save (the image is a
//          colour gradient, so a moved, flipped or re-scaled image would not
//          match);
//        * rotation and flips are kept;
//        * a placement already inside the page keeps its exact content-stream
//          bytes.
//   3. STATE. The crop is an undoable edit of the live document (in memory ==
//      saved; undo brings the overhang back), and the image tool's overflow
//      region and tooltip are gone after the save.
//
// A 30-degree placement cannot be cut along the page edge by a rectangular
// crop in the image's own axes. The save takes the smallest such crop that
// keeps every on-page pixel (see pageOverflowCropRect) and clips what is
// still outside the page with a page-rect clip (pageClipNeeded /
// ImageContentEditor::clipImageToPage); the test checks the crop against a
// dense sample of the image, that no pixel is left outside the page, and that
// no on-page pixel is lost.
//
// 4. OTHER SAVE PATHS. The redaction save and the pre-sign Save As crop too; a
//    document that already carries a digital signature is left alone (a
//    content rewrite would invalidate the signature); the page box is the
//    CropBox.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "image/ImagePlacement.h"
#include "image/ImagePlacementScanner.h"
#include "image/ImageTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"

#include <QAbstractButton>
#include <QAction>
#include <QMessageBox>
#include <QTimer>
#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;
namespace pl = pdfe::image::placement;

namespace {

constexpr double kPi = 3.14159265358979323846;

void ensureQApplication()
{
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = {nullptr};
        static QApplication app(argc, argv);
    }
}

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

std::string fixture(const char* name)
{
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

// A placement in the RENDERED fitz frame (unit square -> Y-down page), the
// frame of PageImageInfo::pageCtm.
//
// The upright image on rect r: u runs left to right, v bottom to top (v = 0 is
// the image's bottom row as drawn).
pl::Affine upright(const QRectF& r)
{
    return pl::Affine{r.width(), 0, 0, -r.height(), r.x(), r.y() + r.height()};
}
// Mirrored left-right on rect r.
pl::Affine mirroredX(const QRectF& r)
{
    return pl::Affine{-r.width(), 0, 0, -r.height(), r.x() + r.width(), r.y() + r.height()};
}
// Mirrored top-bottom on rect r.
pl::Affine mirroredY(const QRectF& r)
{
    return pl::Affine{r.width(), 0, 0, r.height(), r.x(), r.y()};
}
// The upright image on rect r turned by `degCW` (visually clockwise) about
// r's centre.
pl::Affine turned(const QRectF& r, double degCW)
{
    const double t = degCW * kPi / 180.0;  // Y-down: positive math = clockwise
    const double cs = std::cos(t), sn = std::sin(t);
    pl::Affine m = upright(r);
    auto rot = [&](double x, double y) { return QPointF(x * cs - y * sn, x * sn + y * cs); };
    const QPointF ax = rot(m.a, m.b), ay = rot(m.c, m.d);
    m.a = ax.x(); m.b = ax.y(); m.c = ay.x(); m.d = ay.y();
    const QPointF c = r.center();
    m.e = c.x() - 0.5 * (m.a + m.c);
    m.f = c.y() - 0.5 * (m.b + m.d);
    return m;
}

// Fixture numbers are written as a third-party producer might write them -
// fixed point with four decimals ("200.0000") - which is NOT how PDFE's
// placement writer formats them (pl::formatNumber, "200"). So a rewrite of a
// placement, even one that changes nothing, changes its bytes, and the
// byte-identical checks below can see it.
std::string num(double v)
{
    if (std::abs(v) < 5e-5) v = 0.0;
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << v;
    return o.str();
}

// The visible quad of a placement: pageCtm applied to the corners of its crop.
std::array<QPointF, 4> visibleQuad(const PageImageInfo& i)
{
    const auto& m = i.pageCtm;
    const pl::Affine P{m[0], m[1], m[2], m[3], m[4], m[5]};
    return {P.apply(i.crop[0], i.crop[1]), P.apply(i.crop[2], i.crop[1]),
            P.apply(i.crop[2], i.crop[3]), P.apply(i.crop[0], i.crop[3])};
}

bool quadInside(const PageImageInfo& i, const QRectF& page, double tol, std::string* why)
{
    for (const QPointF& p : visibleQuad(i)) {
        if (p.x() < page.left() - tol || p.x() > page.right() + tol ||
            p.y() < page.top() - tol || p.y() > page.bottom() + tol) {
            if (why) {
                std::ostringstream o;
                o << "corner (" << p.x() << ", " << p.y() << ") is outside the page ("
                  << page.left() << ", " << page.top() << ", " << page.right() << ", "
                  << page.bottom() << ")";
                *why = o.str();
            }
            return false;
        }
    }
    return true;
}

// The page content drawn onto a canvas `margin` points larger than the page
// on every side, with no page-box clip. Pixel (0, 0) is fitz point
// (bounds.x0 - margin, bounds.y0 - margin); one pixel per point.
struct Canvas {
    QImage image;
    QRectF page;  // page bounds, fitz
    int originX = 0, originY = 0;
};

// Byte range of the n-th Do of `name` (its whole wrapper) in page 0's content.
struct SiteBytes {
    std::string bytes;
    bool found = false;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Geometry of the save-time crop.
// ---------------------------------------------------------------------------

namespace {
PageImageInfo infoFor(const pl::Affine& P, std::array<float, 4> crop = {0, 0, 1, 1})
{
    PageImageInfo i;
    i.xobjectName = "Im0";
    i.pageCtm = P.toArray();
    i.crop = crop;
    i.cropped = !(crop[0] == 0 && crop[1] == 0 && crop[2] == 1 && crop[3] == 1);
    return i;
}
const QRectF kPage(0, 0, 600, 800);
} // namespace

TEST(ImageSaveCropGeometryTest, PlacementInsideThePageIsLeftAlone)
{
    EXPECT_FALSE(pageOverflowCropRect(infoFor(upright(QRectF(100, 100, 200, 120))), kPage));
    EXPECT_FALSE(pageOverflowCropRect(infoFor(turned(QRectF(200, 300, 200, 120), 30)), kPage));
}

TEST(ImageSaveCropGeometryTest, PlacementAtTheExactPageEdgeIsLeftAlone)
{
    // A full-page image (a scan): touching every edge is not hanging off it.
    EXPECT_FALSE(pageOverflowCropRect(infoFor(upright(kPage)), kPage));
    // Less than the tolerance over the edge.
    EXPECT_FALSE(pageOverflowCropRect(
        infoFor(upright(QRectF(400.0 + kOverflowCropTolerancePt / 4, 100, 200, 120))), kPage));
}

TEST(ImageSaveCropGeometryTest, UprightOverhangIsCutAtThePageEdge)
{
    // 200x120 at x = 520: 120 pt hang off the right edge (x = 600).
    const auto r = pageOverflowCropRect(infoFor(upright(QRectF(520, 100, 200, 120))), kPage);
    ASSERT_TRUE(r);
    EXPECT_NEAR(r->left(), 520.0, 1e-6);
    EXPECT_NEAR(r->right(), 600.0, 1e-6);
    EXPECT_NEAR(r->top(), 100.0, 1e-6);
    EXPECT_NEAR(r->bottom(), 220.0, 1e-6);
}

TEST(ImageSaveCropGeometryTest, PlacementWhollyOffThePageIsLeftAlone)
{
    EXPECT_FALSE(pageOverflowCropRect(infoFor(upright(QRectF(700, 100, 200, 120))), kPage));
}

TEST(ImageSaveCropGeometryTest, FormXObjectPlacementIsLeftAlone)
{
    PageImageInfo i = infoFor(upright(QRectF(520, 100, 200, 120)));
    i.inFormXObject = true;
    EXPECT_FALSE(pageOverflowCropRect(i, kPage)) << "a Form XObject placement is not editable";
}

// ---------------------------------------------------------------------------
// 2./3. Round trip through MainWindow's Save and Save As.
// ---------------------------------------------------------------------------

class ImageSaveCropTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
        // A colour gradient: every pixel position has its own colour, so a
        // moved, scaled or flipped image cannot render the same.
        png_ = path("gradient.png");
        QImage g(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) g.setPixel(x, y, qRgb(x * 4, y * 4, 160));
        ASSERT_TRUE(g.save(QString::fromStdString(png_)));
    }

    std::string path(const char* leaf) const { return tmp_.filePath(leaf).toStdString(); }

    // test_single.pdf with the gradient as an Image XObject, its page content
    // replaced by one `q M cm /Name Do Q` per placement (fitz-frame matrices,
    // converted to user space here). Written to `leaf`; returns the path.
    //
    // cropInset > 0 gives the page a CropBox that much inside its MediaBox on
    // every side (the placements are then in the CropBox's frame, as
    // Page::boundingBox reports it). page_ is the built page's
    // Page::boundingBox().
    std::string build(const char* leaf, const std::vector<pl::Affine>& placements,
                      const std::vector<std::array<double, 4>>& crops = {},
                      double cropInset = 0.0)
    {
        std::string out = path(leaf);
        {
            auto doc = Document::open(context_, base_);
            EXPECT_NE(doc, nullptr);
            if (!doc) return {};
            auto page = doc->page(0);
            page_ = page->boundingBox();
            InsertImageOptions o;
            o.filePath = png_;
            o.targetRect = QRectF(100, 100, 64, 64);
            o.maintainAspectRatio = false;
            auto r = manager_->insertImage(page.get(), doc.get(), o);
            EXPECT_TRUE(r.success) << r.errorMessage;
            name_ = r.imageInfo.xobjectName;
            page.reset();
            doc->saveAs(path("with-image.pdf"));
        }
        const std::string src = path("with-image.pdf");
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, src.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
            if (cropInset > 0) {
                // The page's (untransformed, possibly inherited) MediaBox, inset.
                const fz_rect m = pdf_to_rect(
                    ctx, pdf_dict_get_inheritable(ctx, pp->obj, PDF_NAME(MediaBox)));
                const fz_rect cb{static_cast<float>(m.x0 + cropInset),
                                 static_cast<float>(m.y0 + cropInset),
                                 static_cast<float>(m.x1 - cropInset),
                                 static_cast<float>(m.y1 - cropInset)};
                pdf_dict_put_rect(ctx, pp->obj, PDF_NAME(CropBox), cb);
                mb = cb;
            }
            std::ostringstream c;
            for (std::size_t k = 0; k < placements.size(); ++k) {
                const pl::Affine& P = placements[k];
                // fitz (Y-down from the top) -> user space (Y-up).
                c << "q " << num(P.a) << " " << num(-P.b) << " " << num(P.c) << " "
                  << num(-P.d) << " " << num(mb.x0 + P.e) << " " << num(mb.y1 - P.f) << " cm ";
                if (k < crops.size()) {
                    const auto& u = crops[k];
                    c << num(u[0]) << " " << num(u[1]) << " " << num(u[2] - u[0]) << " "
                      << num(u[3] - u[1]) << " re W n ";
                }
                c << "/" << name_ << " Do Q\n";
            }
            const std::string text = c.str();
            fz_buffer* buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
            pdf_obj* stm = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
            fz_drop_buffer(ctx, buf);
            pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
            pdf_drop_obj(ctx, stm);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, out.c_str(), &wo);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        if (auto doc = Document::open(context_, out)) {
            auto page = doc->page(0);
            page_ = page->boundingBox();
        }
        return out;
    }

    // Number of `re W n` clips in page 0's content.
    int clipCount(const std::string& file)
    {
        const std::string c = pageContent(file);
        int n = 0;
        for (size_t p = c.find("re W n"); p != std::string::npos; p = c.find("re W n", p + 1)) ++n;
        return n;
    }

    // The overflow region the image tool shows for `info`'s placement on page 0
    // of `file`, read from the file (a fresh tool on a fresh document).
    QPainterPath toolOverflow(const std::string& file, int occurrence)
    {
        auto doc = Document::open(context_, file);
        if (!doc) {
            ADD_FAILURE() << "cannot open " << file;
            return {};
        }
        auto page = doc->page(0);
        ImageTool tool(manager_.get());
        tool.setPage(page.get(), doc.get());
        if (!tool.selectPlacement(name_, occurrence)) {
            ADD_FAILURE() << "placement not found";
            return {};
        }
        return tool.overflowRegion();
    }

    // Placements of page 0 of `file`, read back from disk.
    std::vector<PageImageInfo> imagesIn(const std::string& file)
    {
        auto doc = Document::open(context_, file);
        EXPECT_NE(doc, nullptr);
        if (!doc) return {};
        auto page = doc->page(0);
        return manager_->enumerateImages(page.get(), doc.get());
    }

    // Page 0 of `file` drawn with `margin` points to spare on every side, no
    // page-box clip (see Canvas).
    Canvas draw(const std::string& file, int margin = 160)
    {
        Canvas cv;
        std::vector<unsigned char> samples;
        int w = 0, h = 0, stride = 0;
        fz_rect bounds{};
        fz_rect pageBox{};
        context_->execute([&](fz_context* ctx) {
            // MuPDF clips the page content to a CropBox that differs from the
            // MediaBox, which would hide exactly the content this oracle looks
            // for. So the CropBox is dropped (in memory only) and the page is
            // drawn in its MediaBox frame; "the page" is then the CropBox
            // mapped into that frame. Without a CropBox nothing changes.
            pdf_document* d = pdf_open_document(ctx, file.c_str());
            pdf_obj* pobj = pdf_lookup_page_obj(ctx, d, 0);
            pdf_obj* cbObj = pdf_dict_get(ctx, pobj, PDF_NAME(CropBox));
            const fz_rect cb = pdf_to_rect(ctx, cbObj);
            const fz_rect mbu =
                pdf_to_rect(ctx, pdf_dict_get_inheritable(ctx, pobj, PDF_NAME(MediaBox)));
            const bool hasCropBox = cbObj != nullptr;
            if (hasCropBox) pdf_dict_del(ctx, pobj, PDF_NAME(CropBox));
            fz_page* p = fz_load_page(ctx, &d->super, 0);
            bounds = fz_bound_page(ctx, p);
            pageBox = hasCropBox ? fz_rect{cb.x0 - mbu.x0, mbu.y1 - cb.y1, cb.x1 - mbu.x0,
                                           mbu.y1 - cb.y0}
                                 : bounds;
            fz_irect ib{static_cast<int>(std::floor(bounds.x0)) - margin,
                        static_cast<int>(std::floor(bounds.y0)) - margin,
                        static_cast<int>(std::ceil(bounds.x1)) + margin,
                        static_cast<int>(std::ceil(bounds.y1)) + margin};
            fz_pixmap* pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), ib, nullptr, 1);
            fz_clear_pixmap(ctx, pix);
            fz_device* dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_page_contents(ctx, p, dev, fz_identity, nullptr);
            fz_close_device(ctx, dev);
            fz_drop_device(ctx, dev);
            w = pix->w;
            h = pix->h;
            stride = static_cast<int>(pix->stride);
            samples.assign(pix->samples, pix->samples + static_cast<std::size_t>(stride) * h);
            cv.originX = ib.x0;
            cv.originY = ib.y0;
            fz_drop_pixmap(ctx, pix);
            fz_drop_page(ctx, p);
            pdf_drop_document(ctx, d);
        });
        cv.page = QRectF(QPointF(pageBox.x0, pageBox.y0), QPointF(pageBox.x1, pageBox.y1));
        cv.image = QImage(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y)
            std::copy_n(samples.data() + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(w) * 4, cv.image.scanLine(y));
        return cv;
    }

    // Pixel (x, y) of a canvas lies wholly inside / wholly outside the page.
    static bool pixelInPage(const Canvas& c, int x, int y)
    {
        const double fx = c.originX + x, fy = c.originY + y;
        return fx >= c.page.left() && fx + 1 <= c.page.right() && fy >= c.page.top() &&
               fy + 1 <= c.page.bottom();
    }
    static bool pixelOffPage(const Canvas& c, int x, int y)
    {
        const double fx = c.originX + x, fy = c.originY + y;
        return fx + 1 <= c.page.left() || fx >= c.page.right() || fy + 1 <= c.page.top() ||
               fy >= c.page.bottom();
    }

    // Image pixels (alpha > 0) drawn wholly outside the page.
    static int pixelsOffPage(const Canvas& c)
    {
        int n = 0;
        for (int y = 0; y < c.image.height(); ++y)
            for (int x = 0; x < c.image.width(); ++x)
                if (pixelOffPage(c, x, y) && qAlpha(c.image.pixel(x, y)) > 0) ++n;
        return n;
    }

    // In-page pixels that differ by more than `tol` in any channel.
    // With opaqueOnly, only pixels fully covered in both (alpha 255) count.
    // Pixels overlapping `ignore` (fitz page frame, as Canvas::page) are skipped.
    static int inPageDifferences(const Canvas& a, const Canvas& b, int tol = 8,
                                 bool opaqueOnly = false, const QRectF& ignore = QRectF())
    {
        if (a.image.size() != b.image.size() || a.originX != b.originX ||
            a.originY != b.originY)
            return -1;
        int n = 0;
        for (int y = 0; y < a.image.height(); ++y)
            for (int x = 0; x < a.image.width(); ++x) {
                if (!pixelInPage(a, x, y)) continue;
                const QRgb p = a.image.pixel(x, y), q = b.image.pixel(x, y);
                if (opaqueOnly && (qAlpha(p) < 255 || qAlpha(q) < 255)) continue;
                if (!ignore.isNull() &&
                    ignore.intersects(QRectF(a.originX + x, a.originY + y, 1, 1)))
                    continue;
                if (std::abs(qRed(p) - qRed(q)) > tol || std::abs(qGreen(p) - qGreen(q)) > tol ||
                    std::abs(qBlue(p) - qBlue(q)) > tol || std::abs(qAlpha(p) - qAlpha(q)) > tol)
                    ++n;
            }
        return n;
    }

    // In-page image pixels (alpha > 0) of a canvas.
    static int inPageImagePixels(const Canvas& c)
    {
        int n = 0;
        for (int y = 0; y < c.image.height(); ++y)
            for (int x = 0; x < c.image.width(); ++x)
                if (pixelInPage(c, x, y) && qAlpha(c.image.pixel(x, y)) > 0) ++n;
        return n;
    }

    // The bytes of the n-th Do of `name` - its whole `q ... Do Q` wrapper -
    // in page 0's content stream, as the placement scanner finds them.
    SiteBytes siteBytes(const std::string& file, const std::string& name, int occurrence)
    {
        SiteBytes out;
        std::vector<pl::DoSite> sites;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            sites = pl::scanPage(ctx, pp, false);
            for (const auto& s : sites) {
                if (s.formDepth != 0 || s.name != name || s.occurrence != occurrence) continue;
                pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
                pdf_obj* st = pdf_is_array(ctx, c) ? pdf_array_get(ctx, c, s.stream) : c;
                fz_buffer* b = pdf_load_stream(ctx, st);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                if (d && s.replaceEnd <= n && s.replaceStart < s.replaceEnd) {
                    out.bytes.assign(reinterpret_cast<const char*>(d) + s.replaceStart,
                                     s.replaceEnd - s.replaceStart);
                    out.found = true;
                }
                fz_drop_buffer(ctx, b);
                break;
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return out;
    }

    // The whole decoded content of page 0.
    std::string pageContent(const std::string& file)
    {
        std::string out;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
            auto add = [&](pdf_obj* st) {
                fz_buffer* b = pdf_load_stream(ctx, st);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                if (d && n) out.append(reinterpret_cast<const char*>(d), n);
                out += '\n';
                fz_drop_buffer(ctx, b);
            };
            if (pdf_is_array(ctx, c)) {
                for (int i = 0, n = pdf_array_len(ctx, c); i < n; ++i) add(pdf_array_get(ctx, c, i));
            } else if (c) {
                add(c);
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return out;
    }

    // Open `file` in a real MainWindow and save it in place with File > Save
    // (MainWindow::onSaveFile). `before`, if set, runs on the open window
    // first; `after` runs on it after the save.
    void saveInMainWindow(const std::string& file,
                          const std::function<void(pdfe::ui::MainWindow&)>& before = {},
                          const std::function<void(pdfe::ui::MainWindow&)>& after = {})
    {
        pdfe::ui::MainWindow mw;
        settle();
        mw.openFile(QString::fromStdString(file));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr) << "MainWindow did not open " << file;
        if (before) before(mw);
        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr) << "saveAction must exist";
        save->trigger();
        settle();
        if (after) after(mw);
    }

    static PageImageInfo find(const std::vector<PageImageInfo>& v, const std::string& name,
                              int occurrence)
    {
        for (const auto& i : v)
            if (i.xobjectName == name && i.occurrence == occurrence) return i;
        ADD_FAILURE() << name << "#" << occurrence << " not on the page";
        return {};
    }

    // The checks every overhang case shares, on the file as saved.
    void expectNothingOffPageAndInPagePixelsKept(const Canvas& before, const Canvas& after)
    {
        ASSERT_GT(pixelsOffPage(before), 1000)
            << "oracle: before the save the overhang must be drawn outside the page "
               "(the canvas has no page-box clip)";
        EXPECT_EQ(pixelsOffPage(after), 0)
            << "the saved file still draws image pixels outside the page box";
        EXPECT_GT(inPageImagePixels(after), 1000) << "the in-page part of the image is gone";
        EXPECT_EQ(inPageDifferences(before, after), 0)
            << "in-page pixels changed: the crop must keep the visible part exactly";
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string base_;
    std::string png_;
    std::string name_;
    QRectF page_;  // page 0 bounds, fitz
};

// Upright image hanging 120 pt off the right edge, and a second copy of the
// same image wholly inside the page.
TEST_F(ImageSaveCropTest, SaveCropsUprightOverhangAndKeepsInPagePixels)
{
    const std::string file = build("upright.pdf", {});  // sets page_
    ASSERT_FALSE(page_.isEmpty());
    const QRectF over(page_.right() - 80, 200, 200, 120);
    const QRectF inside(60, 500, 160, 100);
    build("upright.pdf", {upright(over), upright(inside)});
    const Canvas before = draw(file);
    const SiteBytes insideBefore = siteBytes(file, name_, 1);
    ASSERT_TRUE(insideBefore.found);

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 2u);
    const PageImageInfo o = find(imgs, name_, 0);
    std::string why;
    EXPECT_TRUE(quadInside(o, page_, 0.05, &why)) << why;
    EXPECT_TRUE(o.cropped);
    EXPECT_NEAR(o.boundingBox.right(), page_.right(), 0.05) << "cut at the page edge";
    EXPECT_NEAR(o.boundingBox.left(), over.left(), 0.05) << "the in-page part kept";
    EXPECT_NEAR(o.displayWidth, over.width(), 0.05) << "the placement itself is not rescaled";
    EXPECT_NEAR(o.rotationRadians, 0.0f, 1e-4f);
    EXPECT_FALSE(o.flippedX || o.flippedY);

    const SiteBytes insideAfter = siteBytes(file, name_, 1);
    ASSERT_TRUE(insideAfter.found);
    EXPECT_EQ(insideAfter.bytes, insideBefore.bytes)
        << "the placement inside the page was rewritten";
    EXPECT_FALSE(find(imgs, name_, 1).cropped);

    expectNothingOffPageAndInPagePixelsKept(before, draw(file));

    // A second save has nothing left to crop: the cropped wrapper's bytes stay.
    const SiteBytes cropped = siteBytes(file, name_, 0);
    saveInMainWindow(file);
    EXPECT_EQ(siteBytes(file, name_, 0).bytes, cropped.bytes) << "a second save re-cropped";
}

// A 90-degree turn: the page edge runs along the image's own (now vertical)
// axis, so the crop follows it exactly.
TEST_F(ImageSaveCropTest, SaveCropsRotated90OverhangExactly)
{
    const std::string file = build("rot90.pdf", {});
    // 200x120 turned 90 degrees clockwise: 120 wide, 200 tall on the page,
    // centred 40 pt below the top edge, so 60 pt hang off the top.
    const QRectF r(200, -60, 200, 120);  // upright rect; centre (300, 0) ...
    const QRectF centred(r.translated(0, 40));  // ... moved to centre (300, 40)
    build("rot90.pdf", {turned(centred, 90)});
    const PageImageInfo pre = imagesIn(file).at(0);
    ASSERT_NEAR(std::abs(pre.rotationRadians), kPi / 2, 1e-3);
    const Canvas before = draw(file);

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 1u);
    const PageImageInfo o = imgs[0];
    std::string why;
    EXPECT_TRUE(quadInside(o, page_, 0.05, &why)) << why;
    EXPECT_NEAR(o.rotationRadians, pre.rotationRadians, 1e-4f) << "the rotation changed";
    EXPECT_EQ(o.flippedX, pre.flippedX);
    EXPECT_EQ(o.flippedY, pre.flippedY);
    // On the page: x 240..360, y from the top edge to 140.
    double top = 1e9;
    for (const QPointF& p : visibleQuad(o)) top = std::min(top, p.y());
    EXPECT_NEAR(top, page_.top(), 0.05) << "cut at the top edge";
    expectNothingOffPageAndInPagePixelsKept(before, draw(file));
}

// Mirrored images keep their mirror: one flipped left-right hanging off the
// left edge, one flipped top-bottom hanging off the bottom.
TEST_F(ImageSaveCropTest, SaveCropsFlippedOverhangsAndKeepsTheFlips)
{
    const std::string file = build("flip.pdf", {});
    const QRectF left(page_.left() - 90, 150, 200, 120);
    const QRectF bottom(250, page_.bottom() - 50, 200, 120);
    build("flip.pdf", {mirroredX(left), mirroredY(bottom)});
    const auto pre = imagesIn(file);
    ASSERT_EQ(pre.size(), 2u);
    ASSERT_TRUE(find(pre, name_, 0).flippedX);
    ASSERT_TRUE(find(pre, name_, 1).flippedY);
    const Canvas before = draw(file);

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 2u);
    const PageImageInfo a = find(imgs, name_, 0), b = find(imgs, name_, 1);
    std::string why;
    EXPECT_TRUE(quadInside(a, page_, 0.05, &why)) << "mirrored X: " << why;
    EXPECT_TRUE(quadInside(b, page_, 0.05, &why)) << "mirrored Y: " << why;
    EXPECT_TRUE(a.flippedX) << "the left-right mirror was lost";
    EXPECT_TRUE(b.flippedY) << "the top-bottom mirror was lost";
    EXPECT_NEAR(a.boundingBox.left(), page_.left(), 0.05);
    EXPECT_NEAR(b.boundingBox.bottom(), page_.bottom(), 0.05);
    // The matrices themselves are untouched; only the unit crop changed.
    for (int k = 0; k < 6; ++k) {
        EXPECT_NEAR(a.pageCtm[k], find(pre, name_, 0).pageCtm[k], 0.01) << "matrix entry " << k;
        EXPECT_NEAR(b.pageCtm[k], find(pre, name_, 1).pageCtm[k], 0.01) << "matrix entry " << k;
    }
    expectNothingOffPageAndInPagePixelsKept(before, draw(file));
}

// 30 degrees with the centre 30 pt from the right edge: a wide band of the
// image is off the page. A rectangular crop in the image's axes is cut as far
// as it can go without losing an on-page pixel, and the rest is clipped.
TEST_F(ImageSaveCropTest, SaveCropsAndClipsRotated30Overhang)
{
    const std::string file = build("rot30.pdf", {});
    const QRectF r(page_.right() - 30 - 100, 300, 200, 120);
    const pl::Affine P = turned(r, 30);
    build("rot30.pdf", {P});
    const Canvas before = draw(file);
    const int offBefore = pixelsOffPage(before);

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 1u);
    const PageImageInfo o = imgs[0];
    EXPECT_TRUE(o.cropped) << "nothing was cropped";
    for (int k = 0; k < 6; ++k)
        EXPECT_NEAR(o.pageCtm[k], P.toArray()[k], 0.01) << "matrix entry " << k << " changed";

    // The crop is the smallest unit rect holding every on-page point of the
    // image: sample the unit square densely, independently of the clipper.
    double u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    const int n = 800;
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j) {
            const double u = double(i) / n, v = double(j) / n;
            if (page_.contains(P.apply(u, v))) {
                u0 = std::min(u0, u); u1 = std::max(u1, u);
                v0 = std::min(v0, v); v1 = std::max(v1, v);
            }
        }
    const double step = 1.5 / n;
    EXPECT_NEAR(o.crop[0], u0, step);
    EXPECT_NEAR(o.crop[1], v0, step);
    EXPECT_NEAR(o.crop[2], u1, step);
    EXPECT_NEAR(o.crop[3], v1, step);
    EXPECT_LT(o.crop[2], 0.9f) << "the band beyond the right edge was not cut";

    const Canvas after = draw(file);
    // Every fully covered on-page pixel is unchanged. Along the image's
    // slanted edges the clip mask now decides the anti-aliasing, so a
    // partially covered edge pixel may differ in coverage - nowhere else.
    EXPECT_EQ(inPageDifferences(before, after, 8, /*opaqueOnly=*/true), 0)
        << "an on-page pixel was lost or moved";
    EXPECT_EQ(inPageImagePixels(after), inPageImagePixels(before))
        << "the on-page footprint of the image changed";
    // What the crop could not remove is clipped at the page edge.
    ASSERT_GT(offBefore, 1000) << "oracle: the overhang is drawn before the save";
    EXPECT_EQ(pixelsOffPage(after), 0)
        << "the saved file still draws image pixels outside the page box";
    EXPECT_FALSE(o.pageClip.isNull()) << "no clip around the rotated placement";
    EXPECT_TRUE(page_.adjusted(-0.05, -0.05, 0.05, 0.05).contains(o.pageClip))
        << "the clip is not within the page";
    EXPECT_TRUE(toolOverflow(file, 0).isEmpty())
        << "red overflow / crop tooltip still shown for the saved, clipped image";

    // A second save changes nothing: no further crop, no second clip.
    const std::string saved = pageContent(file);
    EXPECT_EQ(clipCount(file), 2) << "expected exactly the crop clip and the page clip";
    saveInMainWindow(file);
    EXPECT_EQ(pageContent(file), saved) << "a second save changed the page content";
    EXPECT_EQ(clipCount(file), 2) << "clips stacked on a second save";
}

// A document whose images are all on the page is saved without touching their
// placements: the page content is byte-identical.
TEST_F(ImageSaveCropTest, SaveLeavesImagesInsideThePageByteIdentical)
{
    const std::string file = build("inside.pdf", {});
    build("inside.pdf",
          {upright(QRectF(60, 80, 200, 120)), turned(QRectF(250, 300, 200, 120), 30),
           mirroredX(QRectF(80, 500, 160, 100)), upright(page_)});
    const std::string content = pageContent(file);
    const auto pre = imagesIn(file);
    ASSERT_EQ(pre.size(), 4u);

    saveInMainWindow(file);

    EXPECT_EQ(pageContent(file), content) << "a placement inside the page was rewritten";
    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 4u);
    for (std::size_t k = 0; k < imgs.size(); ++k) {
        EXPECT_FALSE(imgs[k].cropped) << "placement " << k;
        for (int e = 0; e < 6; ++e) EXPECT_EQ(imgs[k].pageCtm[e], pre[k].pageCtm[e]);
    }
}

// An existing crop is kept where it is on the page: the save-time crop
// narrows it, never widens it.
TEST_F(ImageSaveCropTest, SaveKeepsAnExistingCrop)
{
    const std::string file = build("precropped.pdf", {});
    const QRectF over(page_.right() - 80, 200, 200, 120);
    // Already cropped to the image's left 3/4 and middle half (v 0.25..0.75).
    build("precropped.pdf", {upright(over)}, {{0.0, 0.25, 0.75, 0.75}});
    const Canvas before = draw(file);

    saveInMainWindow(file);

    const PageImageInfo o = imagesIn(file).at(0);
    EXPECT_NEAR(o.crop[0], 0.0f, 1e-4f);
    EXPECT_NEAR(o.crop[1], 0.25f, 1e-4f) << "the existing crop was widened";
    EXPECT_NEAR(o.crop[3], 0.75f, 1e-4f) << "the existing crop was widened";
    EXPECT_NEAR(o.crop[2], 80.0f / 200.0f, 1e-4f) << "not cut at the page edge";
    expectNothingOffPageAndInPagePixelsKept(before, draw(file));
}

// File > Save As crops too, and writes the crop to the NEW file.
TEST_F(ImageSaveCropTest, SaveAsCropsTheOverhang)
{
    const std::string file = build("saveas-src.pdf", {});
    build("saveas-src.pdf", {upright(QRectF(page_.right() - 80, 200, 200, 120))});
    const Canvas before = draw(file);
    const std::string target = path("saveas-out.pdf");
    {
        pdfe::ui::MainWindow mw;
        settle();
        mw.openFile(QString::fromStdString(file));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);
        pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(target));
        auto* saveAs = mw.findChild<QAction*>(QStringLiteral("saveAsAction"));
        ASSERT_NE(saveAs, nullptr);
        saveAs->trigger();  // MainWindow::onSaveAsFile
        settle();
    }
    ASSERT_TRUE(std::filesystem::exists(target)) << "Save As wrote nothing";
    const PageImageInfo o = imagesIn(target).at(0);
    std::string why;
    EXPECT_TRUE(quadInside(o, page_, 0.05, &why)) << why;
    expectNothingOffPageAndInPagePixelsKept(before, draw(target));
}

// The crop is an edit of the LIVE document on the undo stack: after the save
// the open document is what was written, and Undo brings the overhang back.
TEST_F(ImageSaveCropTest, SaveCropIsUndoableAndInMemoryMatchesTheFile)
{
    const std::string file = build("undo.pdf", {});
    const QRectF over(page_.right() - 80, 200, 200, 120);
    build("undo.pdf", {upright(over)});

    saveInMainWindow(file, {}, [&](pdfe::ui::MainWindow& mw) {
        Document* doc = mw.documentForTesting();
        ASSERT_NE(doc, nullptr);
        ImageManager mgr(doc->context());
        {
            auto page = doc->page(0);
            const auto live = mgr.enumerateImages(page.get(), doc);
            ASSERT_EQ(live.size(), 1u);
            const PageImageInfo onDisk = imagesIn(file).at(0);
            for (int k = 0; k < 4; ++k)
                EXPECT_NEAR(live[0].crop[k], onDisk.crop[k], 1e-6f)
                    << "the open document and the saved file disagree (crop " << k << ")";
            EXPECT_TRUE(live[0].cropped);
        }
        auto* undo = mw.findChild<QAction*>(QStringLiteral("undoAction"));
        ASSERT_NE(undo, nullptr);
        ASSERT_TRUE(undo->isEnabled()) << "the save-time crop left nothing to undo";
        undo->trigger();
        settle();
        auto page = doc->page(0);
        const auto undone = mgr.enumerateImages(page.get(), doc);
        ASSERT_EQ(undone.size(), 1u);
        EXPECT_FALSE(undone[0].cropped) << "Undo did not bring the overhang back";
        EXPECT_NEAR(undone[0].boundingBox.right(), over.right(), 0.05);
    });
}

// The image tool's red overflow and its "will be cropped" tooltip are gone
// after the save, because the image is cropped - and the tool reads the saved
// page, not the Page the save released.
TEST_F(ImageSaveCropTest, ImageToolOverflowStateClearsAfterSave)
{
    const std::string file = build("tool.pdf", {});
    const QRectF over(page_.right() - 80, 200, 200, 120);
    build("tool.pdf", {upright(over)});
    const QPointF offPage(page_.right() + 40, over.center().y());

    saveInMainWindow(
        file,
        [&](pdfe::ui::MainWindow& mw) {
            auto* act = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
            ASSERT_NE(act, nullptr);
            act->trigger();
            settle();
            ImageTool* tool = mw.canvasForTesting()->imageTool();
            ASSERT_NE(tool, nullptr);
            ASSERT_TRUE(tool->selectPlacement(name_, 0));
            ASSERT_FALSE(tool->overflowRegion().isEmpty()) << "oracle: overflow before save";
            ASSERT_EQ(tool->tooltipAt(offPage), QString::fromUtf8(kImageOverflowCropTooltip));
        },
        [&](pdfe::ui::MainWindow& mw) {
            ImageTool* tool = mw.canvasForTesting()->imageTool();
            ASSERT_NE(tool, nullptr);
            EXPECT_EQ(tool->pageIndex(), 0);
            ASSERT_TRUE(tool->hasSelection()) << "the save dropped the selection";
            EXPECT_TRUE(tool->selectedImage().cropped);
            EXPECT_TRUE(tool->overflowRegion().isEmpty())
                << "red overflow still shown for an image the save cropped";
            EXPECT_TRUE(tool->tooltipAt(offPage).isEmpty());
        });
}

// ---------------------------------------------------------------------------
// The save-time clip decision (pageClipNeeded).
// ---------------------------------------------------------------------------

TEST(ImageSaveCropGeometryTest, PageClipNeededOnlyForWhatACropCannotKeepOnThePage)
{
    // On the page: no clip.
    EXPECT_FALSE(pageClipNeeded(infoFor(upright(QRectF(100, 100, 200, 120))), kPage));
    EXPECT_FALSE(pageClipNeeded(infoFor(turned(QRectF(200, 300, 200, 120), 30)), kPage));
    // A rotated quad over the right edge: a clip is needed.
    const PageImageInfo rotated = infoFor(turned(QRectF(470, 300, 200, 120), 30));
    EXPECT_TRUE(pageClipNeeded(rotated, kPage));
    // ... but not once it is inside a clip (clips never stack).
    PageImageInfo clipped = rotated;
    clipped.pageClip = kPage;
    EXPECT_FALSE(pageClipNeeded(clipped, kPage));
    // Wholly off the page, or in a Form XObject: left alone.
    EXPECT_FALSE(pageClipNeeded(infoFor(turned(QRectF(800, 300, 200, 120), 30)), kPage));
    PageImageInfo form = rotated;
    form.inFormXObject = true;
    EXPECT_FALSE(pageClipNeeded(form, kPage));
}

// ---------------------------------------------------------------------------
// More round trips.
// ---------------------------------------------------------------------------

// The rotated case of ImageToolOverflowStateClearsAfterSave: the crop alone
// cannot clear the overflow, the clip does.
TEST_F(ImageSaveCropTest, ImageToolOverflowStateClearsAfterSaveRotated)
{
    const std::string file = build("tool-rot.pdf", {});
    const QRectF r(page_.right() - 30 - 100, 300, 200, 120);
    build("tool-rot.pdf", {turned(r, 30)});
    const QPointF offPage(page_.right() + 20, r.center().y());

    saveInMainWindow(
        file,
        [&](pdfe::ui::MainWindow& mw) {
            auto* act = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
            ASSERT_NE(act, nullptr);
            act->trigger();
            settle();
            ImageTool* tool = mw.canvasForTesting()->imageTool();
            ASSERT_NE(tool, nullptr);
            ASSERT_TRUE(tool->selectPlacement(name_, 0));
            ASSERT_FALSE(tool->overflowRegion().isEmpty()) << "oracle: overflow before save";
            ASSERT_EQ(tool->tooltipAt(offPage), QString::fromUtf8(kImageOverflowCropTooltip))
                << "oracle: the probe point is in the overflow";
        },
        [&](pdfe::ui::MainWindow& mw) {
            ImageTool* tool = mw.canvasForTesting()->imageTool();
            ASSERT_NE(tool, nullptr);
            ASSERT_TRUE(tool->hasSelection()) << "the save dropped the selection";
            EXPECT_FALSE(tool->selectedImage().pageClip.isNull()) << "the tool sees no clip";
            EXPECT_TRUE(tool->overflowRegion().isEmpty())
                << "red overflow still shown for a rotated image the save clipped";
            EXPECT_TRUE(tool->tooltipAt(offPage).isEmpty());
        });
}

// The clip is an undoable edit like the crop: Undo removes the clip, a second
// Undo the crop, and the in-memory document matches the file before that.
TEST_F(ImageSaveCropTest, SaveClipIsUndoable)
{
    const std::string file = build("clip-undo.pdf", {});
    const QRectF r(page_.right() - 30 - 100, 300, 200, 120);
    build("clip-undo.pdf", {turned(r, 30)});

    saveInMainWindow(file, {}, [&](pdfe::ui::MainWindow& mw) {
        Document* doc = mw.documentForTesting();
        ASSERT_NE(doc, nullptr);
        ImageManager mgr(doc->context());
        auto live = [&]() {
            auto page = doc->page(0);
            const auto v = mgr.enumerateImages(page.get(), doc);
            EXPECT_EQ(v.size(), 1u);
            return v.empty() ? PageImageInfo{} : v[0];
        };
        const PageImageInfo onDisk = imagesIn(file).at(0);
        const PageImageInfo now = live();
        EXPECT_FALSE(now.pageClip.isNull()) << "the open document has no clip";
        EXPECT_EQ(now.pageClip, onDisk.pageClip) << "the open document and the file disagree";
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(now.crop[k], onDisk.crop[k], 1e-6f);

        auto* undo = mw.findChild<QAction*>(QStringLiteral("undoAction"));
        ASSERT_NE(undo, nullptr);
        ASSERT_TRUE(undo->isEnabled());
        undo->trigger();
        settle();
        const PageImageInfo unclipped = live();
        EXPECT_TRUE(unclipped.pageClip.isNull()) << "Undo did not remove the clip";
        EXPECT_TRUE(unclipped.cropped) << "the first Undo must take only the clip";
        ASSERT_TRUE(undo->isEnabled());
        undo->trigger();
        settle();
        EXPECT_FALSE(live().cropped) << "the second Undo did not remove the crop";
    });
}

// Removing a clipped image takes its clip along: no empty clip is left.
TEST_F(ImageSaveCropTest, RemovingAClippedImageRemovesItsClip)
{
    const std::string file = build("clip-remove.pdf", {});
    const QRectF r(page_.right() - 30 - 100, 300, 200, 120);
    build("clip-remove.pdf", {turned(r, 30)});
    saveInMainWindow(file);
    ASSERT_EQ(clipCount(file), 2) << "premise: the save cropped and clipped";

    const std::string out = path("clip-removed.pdf");
    {
        auto doc = Document::open(context_, file);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_TRUE(manager_->removeImage(page.get(), doc.get(), 0).success);
        page.reset();
        doc->saveAs(out);
    }
    const std::string c = pageContent(out);
    EXPECT_EQ(c.find("Do"), std::string::npos) << "the image is still drawn";
    EXPECT_EQ(clipCount(out), 0) << "an empty clip wrapper was left behind: " << c;
}

// The page box is the MediaBox (Page::mediaBox), not the CropBox: on a page
// whose CropBox is smaller than its MediaBox, an image hanging off the
// CropBox - but still inside the MediaBox - is left exactly as it was on save,
// while one hanging off the MediaBox is cut at the MediaBox edge. (This
// replaces SaveCropsToTheCropBox, which pinned the opposite rule; the user
// reversed that decision for ui-tweaks-2.)
TEST_F(ImageSaveCropTest, SaveKeepsWhatIsPastTheCropBoxButInsideTheMediaBox)
{
    const double inset = 40.0;
    const std::string file = build("cropbox.pdf", {}, {}, inset);
    ASSERT_FALSE(page_.isEmpty());
    // In the CropBox's fitz frame the MediaBox extends `inset` past every side.
    const QRectF media = page_.adjusted(-inset, -inset, inset, inset);
    // 30 pt past the CropBox's right edge, 10 pt short of the MediaBox's.
    const QRectF inside(page_.right() - 60, 200, 90, 120);
    // 30 pt past the MediaBox's right edge.
    const QRectF past(media.right() - 60, 400, 90, 120);
    build("cropbox.pdf", {upright(inside), upright(past)}, {}, inset);
    const SiteBytes insideBefore = siteBytes(file, name_, 0);
    ASSERT_FALSE(insideBefore.bytes.empty());

    saveInMainWindow(file);

    EXPECT_EQ(siteBytes(file, name_, 0).bytes, insideBefore.bytes)
        << "a placement inside the MediaBox was rewritten (cut at the CropBox?)";
    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 2u);
    EXPECT_NEAR(imgs.at(0).boundingBox.right(), inside.right(), 0.05)
        << "cut, though it is inside the MediaBox";
    std::string why;
    EXPECT_TRUE(quadInside(imgs.at(1), media, 0.05, &why)) << "not cut at the MediaBox: " << why;
    EXPECT_NEAR(imgs.at(1).boundingBox.right(), media.right(), 0.05)
        << "cut, but not at the MediaBox";
}

// A document that carries a digital signature is saved without the crop: the
// edit would force a full rewrite that invalidates the signature.
TEST_F(ImageSaveCropTest, SaveLeavesSignedDocumentsAlone)
{
    const std::string signedFixture = fixture("sig_clean_signed.pdf");
    ASSERT_FALSE(signedFixture.empty()) << "fixture sig_clean_signed.pdf not found";
    const std::string file = path("signed-overhang.pdf");
    std::filesystem::copy_file(signedFixture, file,
                               std::filesystem::copy_options::overwrite_existing);
    // Draw the gradient off the right edge of page 0, appended incrementally
    // so the existing signature stays applied.
    const std::string png = png_;
    QRectF pageBox;
    context_->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, file.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, 0);
        const fz_rect b = pdf_bound_page(ctx, pp, FZ_CROP_BOX);
        pageBox = QRectF(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1));
        fz_image* img = fz_new_image_from_file(ctx, png.c_str());
        pdf_obj* ref = pdf_add_image(ctx, pdf, img);
        fz_drop_image(ctx, img);
        pdf_obj* res = pdf_dict_get(ctx, pp->obj, PDF_NAME(Resources));
        if (!res) res = pdf_dict_put_dict(ctx, pp->obj, PDF_NAME(Resources), 2);
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        if (!xo) xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1);
        pdf_dict_puts(ctx, xo, "ImSaveCrop", ref);
        pdf_drop_obj(ctx, ref);
        const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
        char text[160];
        std::snprintf(text, sizeof(text), "q 200 0 0 120 %.2f %.2f cm /ImSaveCrop Do Q\n",
                      double(mb.x1 - 80), double(mb.y1 - 320));
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(text), std::strlen(text));
        pdf_obj* stm = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
        fz_drop_buffer(ctx, buf);
        pdf_obj* old = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
        pdf_obj* arr = pdf_new_array(ctx, pdf, 4);
        if (pdf_is_array(ctx, old)) {
            for (int i = 0, n = pdf_array_len(ctx, old); i < n; ++i)
                pdf_array_push(ctx, arr, pdf_array_get(ctx, old, i));
        } else if (old) {
            pdf_array_push(ctx, arr, old);
        }
        pdf_array_push(ctx, arr, stm);
        pdf_drop_obj(ctx, stm);
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), arr);
        pdf_drop_obj(ctx, arr);
        pdf_write_options wo = pdf_default_write_options;
        wo.do_incremental = 1;
        pdf_save_document(ctx, pdf, file.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });
    int signaturesBefore = 0;
    {
        auto doc = Document::open(context_, file);
        ASSERT_NE(doc, nullptr);
        signaturesBefore = doc->appliedSignatureCount();
    }
    ASSERT_GE(signaturesBefore, 1) << "premise: the document carries a signature";
    name_ = "ImSaveCrop";
    const PageImageInfo pre = find(imagesIn(file), name_, 0);
    ASSERT_GT(pre.boundingBox.right(), double(pageBox.right()) + 50) << "premise: it overhangs";
    const std::string content = pageContent(file);

    saveInMainWindow(file);

    const PageImageInfo o = find(imagesIn(file), name_, 0);
    EXPECT_FALSE(o.cropped) << "a signed document was cropped";
    EXPECT_TRUE(o.pageClip.isNull()) << "a signed document was clipped";
    EXPECT_EQ(pageContent(file), content) << "a signed document's page content was rewritten";
    auto doc = Document::open(context_, file);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->appliedSignatureCount(), signaturesBefore);
}

// The pre-sign Save As of the signing flow crops, and the new signature then
// covers the cropped content.
TEST_F(ImageSaveCropTest, SignFlowCropsBeforeSigning)
{
    const std::string pfx = fixture("test_signer.pfx");
    ASSERT_FALSE(pfx.empty()) << "fixture test_signer.pfx not found";
    const std::string file = build("sign-src.pdf", {});
    build("sign-src.pdf", {upright(QRectF(page_.right() - 80, 200, 200, 120))});
    const Canvas before = draw(file);
    const std::string out = path("sign-out.pdf");
    {
        pdfe::ui::MainWindow mw;
        settle();
        mw.openFile(QString::fromStdString(file));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);
        pdfe::ui::MainWindow::SignFlowTestInputs inputs;
        inputs.dialogAccepted = true;
        inputs.pfxPath = QString::fromStdString(pfx);
        inputs.pfxPassword = QStringLiteral("test123");
        inputs.savePath = QString::fromStdString(out);
        inputs.suppressErrorDialogs = true;
        mw.setSignFlowTestInputsForTesting(inputs);
        mw.triggerSignDocumentForTesting();
        settle();
        ASSERT_TRUE(mw.lastSigningErrorForTesting().isEmpty())
            << "premise: the sign must succeed; got: "
            << mw.lastSigningErrorForTesting().toStdString();
        mw.setSignFlowTestInputsForTesting(std::nullopt);
    }
    ASSERT_TRUE(std::filesystem::exists(out));
    const PageImageInfo o = find(imagesIn(out), name_, 0);
    std::string why;
    EXPECT_TRUE(quadInside(o, page_, 0.05, &why)) << why;
    expectNothingOffPageAndInPagePixelsKept(before, draw(out));
    auto doc = Document::open(context_, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->appliedSignatureCount(), 1) << "the signed output lost its signature";
}

// Apply Redactions saves the document in place; it crops like File > Save.
TEST_F(ImageSaveCropTest, RedactionSaveCropsTheOverhang)
{
    const std::string file = build("redact.pdf", {});
    build("redact.pdf", {upright(QRectF(page_.right() - 80, 200, 200, 120))});
    const Canvas before = draw(file);

    std::vector<std::string> boxes;
    {
        pdfe::ui::MainWindow mw;
        settle();
        mw.openFile(QString::fromStdString(file));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr);
        // A redaction mark in the top-left corner, away from the image,
        // through the same CreateRedactionCommand path as a user's drag.
        ASSERT_TRUE(mw.canvasForTesting()->addRedactionMarkAtPdfRect(0, QRectF(10, 10, 30, 30)));
        settle();
        auto* apply = mw.findChild<QAction*>(QStringLiteral("applyRedactionsAction"));
        ASSERT_NE(apply, nullptr);
        // onApplyRedactions asks for confirmation and reports the result in
        // static QMessageBoxes; answer each one (Yes, else OK) from inside its
        // own event loop, recording its title.
        QTimer clicker;
        clicker.setInterval(30);
        QObject::connect(&clicker, &QTimer::timeout, [&boxes]() {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(w);
                if (!box || !box->isVisible()) continue;
                QAbstractButton* b = box->button(QMessageBox::Yes);
                if (!b) b = box->button(QMessageBox::Ok);
                if (!b) continue;
                boxes.push_back(box->windowTitle().toStdString());
                b->click();
            }
        });
        clicker.start();
        ASSERT_TRUE(apply->isEnabled()) << "premise: the document has a pending redaction";
        apply->trigger();  // MainWindow::onApplyRedactions
        settle();
        clicker.stop();
    }
    ASSERT_FALSE(boxes.empty()) << "premise: the confirmation was asked and answered";
    for (const std::string& t : boxes) {
        EXPECT_EQ(t.find("Error"), std::string::npos) << "redaction save reported: " << t;
        EXPECT_EQ(t.find("Failed"), std::string::npos) << "redaction save reported: " << t;
    }
    // The redaction save renames the image resource, so take the one image.
    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 1u);
    std::string why;
    EXPECT_TRUE(quadInside(imgs[0], page_, 0.05, &why)) << why;
    const Canvas after = draw(file);
    ASSERT_GT(pixelsOffPage(before), 1000) << "oracle: the overhang is drawn before the save";
    EXPECT_EQ(pixelsOffPage(after), 0)
        << "the saved file still draws image pixels outside the page box";
    // In-page pixels are unchanged except where the redaction itself painted.
    EXPECT_EQ(inPageDifferences(before, after, 8, false, QRectF(9, 9, 32, 32)), 0)
        << "in-page pixels changed outside the redaction mark";
}
