// -*- coding: utf-8 -*-
//
// The image page box is the MediaBox (ui-tweaks-2 phase-1).
//
// The image tool's drag clamp, its red overflow shading and the "will be
// cropped" tooltip, and the save-time crop/clip of images that hang off their
// page all use Page::mediaBox(): the page's MediaBox in the SAME fitz space as
// Page::boundingBox() (user space mapped by the page ctm of the CropBox
// frame). So on a page whose CropBox is smaller than its MediaBox (CropTool):
//   * an image past the CropBox but inside the MediaBox is not clamped, not
//     shaded, gets no tooltip, and is not cropped or clipped by the save;
//   * an image past the MediaBox still is - to the MediaBox edge.
//
//   1. ACCESSOR. mediaBox() == boundingBox() without a CropBox and with a
//      CropBox equal to the MediaBox; the expected fitz rect (worked out by
//      hand below, not read back from MuPDF) for a CropBox inset, a /Rotate 90
//      page, a non-zero MediaBox origin with an offset CropBox origin, and the
//      last two combined.
//   2. TOOL. Overflow region, tooltip and drag clamp against the MediaBox, on
//      each of those pages.
//   3. SAVE. A real MainWindow File > Save; the saved file is reopened from
//      disk: the placement inside the MediaBox keeps its exact content-stream
//      bytes, the one past it is cut at the MediaBox edge; a 30-degree
//      overhang is clipped to the MediaBox.
//   4. CLIP. ImageContentEditor::clipImageToPage clips to the MediaBox.
//
// Fixtures follow tests/test_image_save_crop.cpp: test_single.pdf with an
// Image XObject inserted by ImageManager, its page boxes set and its content
// replaced by `q M cm /Name Do Q` per placement (fitz-frame matrices taken to
// user space through the page transform).

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

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
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

// The upright image on rect r (rendered fitz frame): u left to right, v bottom
// to top.
pl::Affine upright(const QRectF& r)
{
    return pl::Affine{r.width(), 0, 0, -r.height(), r.x(), r.y() + r.height()};
}

// The upright image on rect r turned by `degCW` (visually clockwise) about its
// centre.
pl::Affine turned(const QRectF& r, double degCW)
{
    const double t = degCW * kPi / 180.0;
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

// Fixed point, four decimals: NOT PDFE's own number format, so any rewrite of
// a placement changes its bytes (see test_image_save_crop.cpp).
std::string num(double v)
{
    if (std::abs(v) < 5e-5) v = 0.0;
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << v;
    return o.str();
}

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
                o << "corner (" << p.x() << ", " << p.y() << ") is outside ("
                  << page.left() << ", " << page.top() << ", " << page.right() << ", "
                  << page.bottom() << ")";
                *why = o.str();
            }
            return false;
        }
    }
    return true;
}

double quadRight(const PageImageInfo& i)
{
    double r = -1e9;
    for (const QPointF& p : visibleQuad(i)) r = std::max(r, p.x());
    return r;
}

::testing::AssertionResult rectNear(const QRectF& a, const QRectF& b, double eps)
{
    if (std::abs(a.left() - b.left()) <= eps && std::abs(a.top() - b.top()) <= eps &&
        std::abs(a.right() - b.right()) <= eps && std::abs(a.bottom() - b.bottom()) <= eps)
        return ::testing::AssertionSuccess();
    std::ostringstream o;
    o << "(" << a.left() << ", " << a.top() << ", " << a.right() << ", " << a.bottom()
      << ") vs expected (" << b.left() << ", " << b.top() << ", " << b.right() << ", "
      << b.bottom() << ")";
    return ::testing::AssertionFailure() << o.str();
}

// A page geometry and what Page::boundingBox() / Page::mediaBox() must report
// for it (worked out by hand from pdf_page_obj_transform_box: fitz = user
// space flipped, turned by /Rotate, translated so the CropBox's top-left as
// rendered is (0, 0)).
struct Layout {
    const char* name;
    std::array<float, 4> media;            // user space [x0 y0 x1 y1]
    std::optional<std::array<float, 4>> crop;
    int rotate;
    QRectF expectBounding;                 // fitz
    QRectF expectMedia;                    // fitz
};

std::ostream& operator<<(std::ostream& os, const Layout& l) { return os << l.name; }

// CropBox 40 pt inside a Letter MediaBox on every side.
//   fitz = (ux - 40, 752 - uy): MediaBox -> x [-40, 572], y [-40, 752].
const Layout kInset{"CropBoxInset", {0, 0, 612, 792}, std::array<float, 4>{40, 40, 572, 752}, 0,
                    QRectF(0, 0, 532, 712), QRectF(-40, -40, 612, 792)};
// /Rotate 90 (clockwise as seen). fitz = (uy - 50, ux - 40): MediaBox ->
// x [-50, 742], y [-40, 572]; the CropBox renders 692 x 532.
const Layout kRot90{"Rotate90", {0, 0, 612, 792}, std::array<float, 4>{40, 50, 572, 742}, 90,
                    QRectF(0, 0, 692, 532), QRectF(-50, -40, 792, 612)};
// MediaBox origin (100, 200), CropBox origin (150, 260).
//   fitz = (ux - 150, 952 - uy): MediaBox -> x [-50, 562], y [-40, 752].
const Layout kOrigin{"NonZeroOrigin", {100, 200, 712, 992}, std::array<float, 4>{150, 260, 662, 952},
                     0, QRectF(0, 0, 512, 692), QRectF(-50, -40, 612, 792)};
// Both: fitz = (uy - 260, ux - 150): MediaBox -> x [-60, 732], y [-50, 562].
const Layout kOriginRot90{"NonZeroOriginRotate90", {100, 200, 712, 992},
                          std::array<float, 4>{150, 260, 662, 952}, 90, QRectF(0, 0, 692, 512),
                          QRectF(-60, -50, 792, 612)};
// No CropBox, and a CropBox equal to the MediaBox.
const Layout kNoCrop{"NoCropBox", {0, 0, 612, 792}, std::nullopt, 0, QRectF(0, 0, 612, 792),
                     QRectF(0, 0, 612, 792)};
const Layout kEqualCrop{"CropBoxEqualsMediaBox", {0, 0, 612, 792},
                        std::array<float, 4>{0, 0, 612, 792}, 0, QRectF(0, 0, 612, 792),
                        QRectF(0, 0, 612, 792)};

class MediaBoxFixture {
protected:
    void setUpFixture()
    {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
        png_ = path("gradient.png");
        QImage g(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) g.setPixel(x, y, qRgb(x * 4, y * 4, 160));
        ASSERT_TRUE(g.save(QString::fromStdString(png_)));
    }

    std::string path(const char* leaf) const { return tmp_.filePath(leaf).toStdString(); }

    // test_single.pdf with the gradient as an Image XObject, the page's boxes
    // and /Rotate set from `lay`, and its content one `q M cm /Name Do Q` per
    // placement (rendered fitz-frame matrices, taken to user space through
    // the page transform). Returns the written path.
    std::string build(const char* leaf, const Layout& lay,
                      const std::vector<pl::Affine>& placements)
    {
        const std::string withImage = path("with-image.pdf");
        {
            auto doc = Document::open(context_, base_);
            EXPECT_NE(doc, nullptr);
            if (!doc) return {};
            auto page = doc->page(0);
            InsertImageOptions o;
            o.filePath = png_;
            o.targetRect = QRectF(100, 100, 64, 64);
            o.maintainAspectRatio = false;
            auto r = manager_->insertImage(page.get(), doc.get(), o);
            EXPECT_TRUE(r.success) << r.errorMessage;
            name_ = r.imageInfo.xobjectName;
            page.reset();
            doc->saveAs(withImage);
        }
        const std::string out = path(leaf);
        bool ok = false;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, withImage.c_str());
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
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            const pl::Affine T = pl::pageTransform(ctx, pp);
            pl::Affine inv;
            if (pl::invert(T, inv)) {
                std::ostringstream c;
                for (const pl::Affine& P : placements) {
                    const pl::Affine U = pl::concat(P, inv);  // unit square -> user space
                    c << "q " << num(U.a) << " " << num(U.b) << " " << num(U.c) << " "
                      << num(U.d) << " " << num(U.e) << " " << num(U.f) << " cm /" << name_
                      << " Do Q\n";
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
                ok = true;
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        EXPECT_TRUE(ok) << "fixture page transform not invertible";
        return out;
    }

    std::vector<PageImageInfo> imagesIn(const std::string& file)
    {
        auto doc = Document::open(context_, file);
        EXPECT_NE(doc, nullptr);
        if (!doc) return {};
        auto page = doc->page(0);
        return manager_->enumerateImages(page.get(), doc.get());
    }

    static PageImageInfo find(const std::vector<PageImageInfo>& v, const std::string& name,
                              int occurrence)
    {
        for (const auto& i : v)
            if (i.xobjectName == name && i.occurrence == occurrence) return i;
        ADD_FAILURE() << name << "#" << occurrence << " not on the page";
        return {};
    }

    // The bytes of the n-th Do of `name` (its whole wrapper) in page 0's content.
    std::string siteBytes(const std::string& file, int occurrence)
    {
        std::string out;
        std::vector<pl::DoSite> sites;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            sites = pl::scanPage(ctx, pp, false);
            for (const auto& s : sites) {
                if (s.formDepth != 0 || s.name != name_ || s.occurrence != occurrence) continue;
                pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
                pdf_obj* st = pdf_is_array(ctx, c) ? pdf_array_get(ctx, c, s.stream) : c;
                fz_buffer* b = pdf_load_stream(ctx, st);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                if (d && s.replaceEnd <= n && s.replaceStart < s.replaceEnd)
                    out.assign(reinterpret_cast<const char*>(d) + s.replaceStart,
                               s.replaceEnd - s.replaceStart);
                fz_drop_buffer(ctx, b);
                break;
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return out;
    }

    // Number of `re W n` clips in page 0's content.
    int clipCount(const std::string& file)
    {
        std::string all;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
            auto add = [&](pdf_obj* st) {
                fz_buffer* b = pdf_load_stream(ctx, st);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                if (d && n) all.append(reinterpret_cast<const char*>(d), n);
                all += '\n';
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
        int n = 0;
        for (size_t p = all.find("re W n"); p != std::string::npos; p = all.find("re W n", p + 1))
            ++n;
        return n;
    }

    // Open `file` in a real MainWindow and save it in place (File > Save).
    void saveInMainWindow(const std::string& file)
    {
        pdfe::ui::MainWindow mw;
        settle();
        mw.openFile(QString::fromStdString(file));
        settle();
        ASSERT_NE(mw.documentForTesting(), nullptr) << "MainWindow did not open " << file;
        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr) << "saveAction must exist";
        save->trigger();
        settle();
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::string base_;
    std::string png_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// 1. The accessor.
// ---------------------------------------------------------------------------

class PageMediaBoxTest : public ::testing::Test, protected MediaBoxFixture {
protected:
    void SetUp() override { setUpFixture(); }

    void expectBoxes(const Layout& lay)
    {
        const std::string file = build((std::string(lay.name) + ".pdf").c_str(), lay, {});
        auto doc = Document::open(context_, file);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        EXPECT_TRUE(rectNear(page->boundingBox(), lay.expectBounding, 1e-3))
            << lay.name << ": precondition, boundingBox()";
        EXPECT_TRUE(rectNear(page->mediaBox(), lay.expectMedia, 1e-3))
            << lay.name << ": mediaBox() in boundingBox()'s fitz space";
    }
};

TEST_F(PageMediaBoxTest, EqualsBoundingBoxOnTheUnmodifiedFixture)
{
    auto doc = Document::open(context_, base_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(rectNear(page->mediaBox(), page->boundingBox(), 1e-6));
}

TEST_F(PageMediaBoxTest, EqualsBoundingBoxWithoutACropBox)
{
    expectBoxes(kNoCrop);
    auto doc = Document::open(context_, path("NoCropBox.pdf"));
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    EXPECT_TRUE(rectNear(page->mediaBox(), page->boundingBox(), 1e-6));
}

TEST_F(PageMediaBoxTest, EqualsBoundingBoxWhenTheCropBoxIsTheMediaBox)
{
    expectBoxes(kEqualCrop);
    auto doc = Document::open(context_, path("CropBoxEqualsMediaBox.pdf"));
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    EXPECT_TRUE(rectNear(page->mediaBox(), page->boundingBox(), 1e-6));
}

TEST_F(PageMediaBoxTest, IsTheMediaBoxInFitzSpaceForACropBoxInset) { expectBoxes(kInset); }
TEST_F(PageMediaBoxTest, IsTheMediaBoxInFitzSpaceOnARotate90Page) { expectBoxes(kRot90); }
TEST_F(PageMediaBoxTest, IsTheMediaBoxInFitzSpaceWithANonZeroOriginAndCropOffset)
{
    expectBoxes(kOrigin);
}
TEST_F(PageMediaBoxTest, IsTheMediaBoxInFitzSpaceWithANonZeroOriginOnARotate90Page)
{
    expectBoxes(kOriginRot90);
}

TEST_F(PageMediaBoxTest, BoundingBoxIsUnchangedAndInsideTheMediaBox)
{
    for (const Layout* lay : {&kInset, &kRot90, &kOrigin, &kOriginRot90}) {
        const std::string file = build((std::string(lay->name) + "-in.pdf").c_str(), *lay, {});
        auto doc = Document::open(context_, file);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        // boundingBox() is still fz_bound_page: the CropBox frame.
        QRectF fz;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            fz_page* p = fz_load_page(ctx, &pdf->super, 0);
            const fz_rect b = fz_bound_page(ctx, p);
            fz = QRectF(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1));
            fz_drop_page(ctx, p);
            pdf_drop_document(ctx, pdf);
        });
        EXPECT_TRUE(rectNear(page->boundingBox(), fz, 1e-6)) << lay->name;
        EXPECT_TRUE(page->mediaBox().contains(page->boundingBox())) << lay->name;
        EXPECT_GT(page->mediaBox().width(), page->boundingBox().width()) << lay->name;
    }
}

// ---------------------------------------------------------------------------
// 2-4. Tool, save and clip against the MediaBox, on each page geometry.
// ---------------------------------------------------------------------------

class ImageMediaBoxBoundsTest : public ::testing::TestWithParam<Layout>,
                                protected MediaBoxFixture {
protected:
    void SetUp() override
    {
        setUpFixture();
        lay_ = GetParam();
        crop_ = lay_.expectBounding;
        media_ = lay_.expectMedia;
        // Every layout leaves at least 40 pt between the CropBox and the
        // MediaBox on the right and bottom; the placements below rely on it.
        ASSERT_GE(media_.right() - crop_.right(), 40.0);
        ASSERT_GE(media_.bottom() - crop_.bottom(), 40.0);
    }

    // Past the CropBox's right and bottom edges by 20 pt, inside the MediaBox.
    QRectF pastCropInsideMedia() const
    {
        return QRectF(crop_.right() - 30, crop_.bottom() - 30, 50, 50);
    }
    // Hanging 50 pt past the MediaBox's right edge.
    QRectF pastMedia() const
    {
        return QRectF(media_.right() - 30, crop_.top() + 150, 80, 40);
    }

    // A fresh tool on a fresh document of `file`, placement `occ` selected.
    struct Bound {
        std::unique_ptr<Document> doc;
        std::unique_ptr<Page> page;
        std::unique_ptr<ImageTool> tool;
    };
    Bound bind(const std::string& file, int occ)
    {
        Bound b;
        b.doc = Document::open(context_, file);
        EXPECT_NE(b.doc, nullptr);
        if (!b.doc) return b;
        b.page = b.doc->page(0);
        b.tool = std::make_unique<ImageTool>(manager_.get());
        b.tool->setPage(b.page.get(), b.doc.get());
        EXPECT_TRUE(b.tool->selectPlacement(name_, occ)) << "placement " << occ;
        return b;
    }

    static void press(ImageTool& t, QPointF p)
    {
        QMouseEvent e(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        t.mousePressEvent(&e, p, 1.0f);
    }
    static void moveTo(ImageTool& t, QPointF p)
    {
        QMouseEvent e(QEvent::MouseMove, p, p, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        t.mouseMoveEvent(&e, p, 1.0f);
    }
    static void release(ImageTool& t, QPointF p)
    {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        t.mouseReleaseEvent(&e, p, 1.0f);
    }

    Layout lay_{};
    QRectF crop_, media_;
};

TEST_P(ImageMediaBoxBoundsTest, PastTheCropBoxInsideTheMediaBoxIsNotShadedAndHasNoTooltip)
{
    const QRectF r = pastCropInsideMedia();
    const std::string file = build("noshade.pdf", lay_, {upright(r)});
    Bound b = bind(file, 0);
    ASSERT_NE(b.tool, nullptr);
    ASSERT_TRUE(rectNear(b.tool->selectedImage().boundingBox, r, 0.05))
        << "precondition: placed where asked";
    EXPECT_TRUE(b.tool->overflowRegion().isEmpty())
        << "an image inside the MediaBox is shaded as overflowing";
    const QPointF beyondCrop(crop_.right() + 10, crop_.bottom() + 10);
    ASSERT_TRUE(r.contains(beyondCrop));
    EXPECT_TRUE(b.tool->tooltipAt(beyondCrop).isEmpty())
        << "the crop-on-save tooltip shows past the CropBox, inside the MediaBox";
}

TEST_P(ImageMediaBoxBoundsTest, PastTheMediaBoxIsShadedFromTheMediaBoxEdgeWithTooltip)
{
    const QRectF r = pastMedia();
    const std::string file = build("shade.pdf", lay_, {upright(r)});
    Bound b = bind(file, 0);
    ASSERT_NE(b.tool, nullptr);
    const QPainterPath over = b.tool->overflowRegion();
    ASSERT_FALSE(over.isEmpty()) << "an image past the MediaBox is not shaded";
    EXPECT_NEAR(over.boundingRect().left(), media_.right(), 0.05)
        << "the shading must start at the MediaBox edge";
    EXPECT_NEAR(over.boundingRect().right(), r.right(), 0.05);
    const QString want = QString::fromUtf8(kImageOverflowCropTooltip);
    EXPECT_EQ(b.tool->tooltipAt(QPointF(media_.right() + 20, r.center().y())), want);
    EXPECT_TRUE(b.tool->tooltipAt(QPointF(media_.right() - 10, r.center().y())).isEmpty())
        << "the part between the CropBox and the MediaBox is not overflow";
}

TEST_P(ImageMediaBoxBoundsTest, DragIsNotClampedAtTheCropBoxButIsAtTheMediaBox)
{
    // 40 x 80 pt: wide enough that a press at its centre is clear of the
    // resize handles (16 pt reach at zoom 1), and with its centre 15 pt past
    // the CropBox it still lies inside the MediaBox (at least 40 pt further
    // out).
    const QRectF r(crop_.left() + 100, crop_.top() + 100, 40, 80);
    const std::string file = build("drag.pdf", lay_, {upright(r)});
    Bound b = bind(file, 0);
    ASSERT_NE(b.tool, nullptr);
    ImageTool& t = *b.tool;
    float cx = -1;
    int fires = 0;
    QObject::connect(&t, &ImageTool::resizeRequested,
                     [&](const std::string&, float w, float, float x, float, float) {
                         ++fires;
                         cx = x + w / 2;
                     });
    const QPointF c0 = t.selectedImage().boundingBox.center();
    press(t, c0);
    ASSERT_TRUE(rectNear(t.selectedImage().boundingBox, r, 0.05)) << "precondition";
    // Centre to 15 pt past the CropBox's right edge: the image is then inside
    // the MediaBox, so the move follows the pointer exactly.
    const double dx = crop_.right() + 15 - c0.x();
    moveTo(t, c0 + QPointF(dx, 0));
    EXPECT_NEAR(t.selectedImage().boundingBox.center().x(), crop_.right() + 15, 1e-3)
        << "the move was clamped at the CropBox";
    EXPECT_TRUE(t.overflowRegion().isEmpty()) << "shaded while inside the MediaBox";
    // Far past: the centre stops on the MediaBox edge.
    moveTo(t, c0 + QPointF(4000, 0));
    EXPECT_NEAR(t.selectedImage().boundingBox.center().x(), media_.right(), 1e-3)
        << "the clamp is not the MediaBox";
    EXPECT_FALSE(t.overflowRegion().isEmpty());
    release(t, c0 + QPointF(4000, 0));
    ASSERT_EQ(fires, 1);
    EXPECT_NEAR(cx, media_.right(), 1e-2) << "the committed move is the MediaBox-clamped one";
}

TEST_P(ImageMediaBoxBoundsTest, SaveKeepsWhatIsInsideTheMediaBoxAndCutsWhatIsPastIt)
{
    const QRectF in = pastCropInsideMedia();
    const QRectF out = pastMedia();
    const std::string file = build("save.pdf", lay_, {upright(in), upright(out)});
    const std::string inBefore = siteBytes(file, 0);
    ASSERT_FALSE(inBefore.empty());

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);  // the SAVED file, from disk
    ASSERT_EQ(imgs.size(), 2u);
    const PageImageInfo i0 = find(imgs, name_, 0);
    const PageImageInfo i1 = find(imgs, name_, 1);
    EXPECT_FALSE(i0.cropped) << "an image inside the MediaBox was cropped on save";
    EXPECT_TRUE(i0.pageClip.isNull()) << "an image inside the MediaBox was clipped on save";
    EXPECT_EQ(siteBytes(file, 0), inBefore)
        << "the save rewrote a placement that lies inside the MediaBox";
    EXPECT_TRUE(rectNear(i0.boundingBox, in, 0.05));

    EXPECT_TRUE(i1.cropped) << "an image past the MediaBox was not cropped on save";
    std::string why;
    EXPECT_TRUE(quadInside(i1, media_, 0.05, &why)) << why;
    EXPECT_NEAR(quadRight(i1), media_.right(), 0.05) << "cut, but not at the MediaBox edge";
    EXPECT_NEAR(i1.boundingBox.left(), out.left(), 0.05) << "the on-page part moved";
    EXPECT_TRUE(i1.pageClip.isNull()) << "an upright overhang needs no page clip";
    // The crop's own clip, of the cut placement only.
    EXPECT_EQ(clipCount(file), 1);
}

TEST_P(ImageMediaBoxBoundsTest, SaveClipsARotatedOverhangToTheMediaBoxOnly)
{
    // 30 degrees: a unit-square crop cannot follow the page edge, so the save
    // also clips (pageClipNeeded / ClipImageToPageCommand).
    const QRectF inside(crop_.right() - 40, crop_.top() + 300, 50, 30);
    const QRectF over(media_.right() - 40, crop_.top() + 150, 80, 50);
    const std::string file = build("clip.pdf", lay_, {turned(inside, 30), turned(over, 30)});
    const std::string inBefore = siteBytes(file, 0);
    ASSERT_FALSE(inBefore.empty());

    saveInMainWindow(file);

    const auto imgs = imagesIn(file);
    ASSERT_EQ(imgs.size(), 2u);
    const PageImageInfo i0 = find(imgs, name_, 0);
    const PageImageInfo i1 = find(imgs, name_, 1);
    EXPECT_EQ(siteBytes(file, 0), inBefore)
        << "a turned image inside the MediaBox (past the CropBox) was rewritten";
    EXPECT_TRUE(i0.pageClip.isNull());
    EXPECT_FALSE(i0.cropped);
    ASSERT_FALSE(i1.pageClip.isNull()) << "the turned overhang was not clipped";
    EXPECT_TRUE(rectNear(i1.pageClip, media_, 0.05)) << "the save clip is not the MediaBox";
    EXPECT_TRUE(i1.cropped);
    // The overhang's crop clip and its page clip; nothing for placement 0.
    EXPECT_EQ(clipCount(file), 2);
}

TEST_P(ImageMediaBoxBoundsTest, ClipImageToPageClipsToTheMediaBox)
{
    const QRectF r(crop_.right() - 40, crop_.top() + 300, 50, 30);  // past the CropBox
    const std::string file = build("editor-clip.pdf", lay_, {turned(r, 30)});
    auto doc = Document::open(context_, file);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_TRUE(editor_->clipImageToPage(page.get(), doc.get(), PlacementId{name_, 0}));
    const auto imgs = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_EQ(imgs.size(), 1u);
    ASSERT_FALSE(imgs[0].pageClip.isNull());
    EXPECT_TRUE(rectNear(imgs[0].pageClip, media_, 0.05))
        << "clipImageToPage did not clip to the MediaBox";
    // So the clip cuts nothing of an image wholly inside the MediaBox.
    for (const QPointF& p : visibleQuad(imgs[0]))
        EXPECT_TRUE(imgs[0].pageClip.adjusted(-1e-3, -1e-3, 1e-3, 1e-3).contains(p));
}

INSTANTIATE_TEST_SUITE_P(PageGeometries, ImageMediaBoxBoundsTest,
                         ::testing::Values(kInset, kRot90, kOrigin, kOriginRot90),
                         [](const ::testing::TestParamInfo<Layout>& i) {
                             return std::string(i.param.name);
                         });

// ---------------------------------------------------------------------------
// 5. Accessor edge cases (claim-check phase-1, claims 3-5): UserUnit, a
//    degenerate MediaBox, and a non-PDF page.
// ---------------------------------------------------------------------------

class PageMediaBoxEdgeTest : public ::testing::Test, protected MediaBoxFixture {
protected:
    void SetUp() override { setUpFixture(); }

    // Copy page 0 of `src` to `leaf` with `fn` applied to the page object.
    std::string patchPage(const std::string& src, const char* leaf,
                          const std::function<void(fz_context*, pdf_obj*)>& fn)
    {
        const std::string out = path(leaf);
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, src.c_str());
            pdf_obj* pobj = pdf_lookup_page_obj(ctx, pdf, 0);
            fn(ctx, pobj);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, out.c_str(), &wo);
            pdf_drop_document(ctx, pdf);
        });
        return out;
    }

    struct Boxes {
        QRectF bounding, media;
    };
    Boxes boxesOf(const std::string& file)
    {
        Boxes b;
        auto doc = Document::open(context_, file);
        EXPECT_NE(doc, nullptr) << file;
        if (!doc) return b;
        auto page = doc->page(0);
        EXPECT_NE(page, nullptr);
        if (!page) return b;
        b.bounding = page->boundingBox();
        b.media = page->mediaBox();
        return b;
    }
};

// /UserUnit 2 on the CropBoxInset page (MediaBox [0 0 612 792], CropBox
// [40 40 572 752]). pdf_page_obj_transform_box scales user space by UserUnit
// before the flip and the CropBox translation:
//   fitz = (2 * (ux - 40), 2 * (752 - uy))
//   boundingBox: x [0, 1064],    y [0, 1424]
//   mediaBox:    x [-80, 1144],  y [-80, 1504]
// i.e. exactly twice the UserUnit-1 rects of kInset.
TEST_F(PageMediaBoxEdgeTest, UserUnitScalesTheMediaBoxLikeTheBoundingBox)
{
    const std::string plain = build("uu1.pdf", kInset, {});
    const Boxes one = boxesOf(plain);
    ASSERT_TRUE(rectNear(one.bounding, kInset.expectBounding, 1e-3)) << "precondition";
    ASSERT_TRUE(rectNear(one.media, kInset.expectMedia, 1e-3)) << "precondition";

    const std::string file = patchPage(plain, "uu2.pdf", [](fz_context* ctx, pdf_obj* pobj) {
        pdf_dict_put_real(ctx, pobj, PDF_NAME(UserUnit), 2.0);
    });
    const Boxes two = boxesOf(file);
    EXPECT_TRUE(rectNear(two.bounding, QRectF(0, 0, 1064, 1424), 1e-3))
        << "precondition: MuPDF applies /UserUnit to boundingBox()";
    EXPECT_TRUE(rectNear(two.media, QRectF(-80, -80, 1224, 1584), 1e-3))
        << "mediaBox() does not apply /UserUnit";
    // Consistent with boundingBox(): the same factor on both, and the CropBox
    // still inside the MediaBox.
    const QRectF halved(two.media.x() / 2, two.media.y() / 2, two.media.width() / 2,
                        two.media.height() / 2);
    EXPECT_TRUE(rectNear(halved, one.media, 1e-3));
    EXPECT_NEAR(two.media.width() / two.bounding.width(),
                one.media.width() / one.bounding.width(), 1e-6);
    EXPECT_TRUE(two.media.contains(two.bounding));
}

// A zero-area MediaBox: MuPDF renders the page as its default 612 x 792 box,
// which is what boundingBox() reports; mediaBox() must be the same rect, not
// the transformed empty box.
TEST_F(PageMediaBoxEdgeTest, ZeroAreaMediaBoxFallsBackToBoundingBox)
{
    const std::string plain = build("zero-src.pdf", kNoCrop, {});
    const std::string file = patchPage(plain, "zero.pdf", [](fz_context* ctx, pdf_obj* pobj) {
        pdf_dict_put_rect(ctx, pobj, PDF_NAME(MediaBox), fz_make_rect(0, 0, 0, 0));
    });
    const Boxes b = boxesOf(file);
    ASSERT_FALSE(b.bounding.isEmpty()) << "precondition: MuPDF substitutes a default box";
    EXPECT_TRUE(rectNear(b.media, b.bounding, 1e-6))
        << "a zero-area MediaBox must fall back to boundingBox()";
}

// Under 1 pt on a side: MuPDF substitutes the unit rect; mediaBox() again
// reports what boundingBox() does.
TEST_F(PageMediaBoxEdgeTest, SubPointMediaBoxFallsBackToBoundingBox)
{
    const std::string plain = build("tiny-src.pdf", kNoCrop, {});
    const std::string file = patchPage(plain, "tiny.pdf", [](fz_context* ctx, pdf_obj* pobj) {
        pdf_dict_put_rect(ctx, pobj, PDF_NAME(MediaBox), fz_make_rect(10, 10, 10.5f, 300));
    });
    const Boxes b = boxesOf(file);
    ASSERT_FALSE(b.bounding.isEmpty());
    EXPECT_TRUE(rectNear(b.media, b.bounding, 1e-6))
        << "a MediaBox under 1 pt wide must fall back to boundingBox()";
}

// A non-PDF page (an SVG opened through fitz, as test_image_insert_crash_guard
// does: the "%PDF-" comment satisfies Document::open's magic-byte gate, the
// handler is chosen by extension). There is no MediaBox; mediaBox() is
// boundingBox().
TEST_F(PageMediaBoxEdgeTest, NonPdfPageFallsBackToBoundingBox)
{
    const std::string svg = path("nonpdf.svg");
    {
        std::ofstream out(svg, std::ios::binary | std::ios::trunc);
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!-- %PDF-1.7 marker: satisfies PDFE open() magic-byte gate; "
               "this file is really an SVG, not a PDF -->\n"
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"90\" "
               "viewBox=\"0 0 120 90\">\n"
               "  <rect x=\"0\" y=\"0\" width=\"120\" height=\"90\" fill=\"#cccccc\"/>\n"
               "</svg>\n";
    }
    std::unique_ptr<Document> doc;
    ASSERT_NO_THROW(doc = Document::open(context_, svg));
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->isPdf()) << "precondition: the fixture must open as a non-PDF document";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF bounding = page->boundingBox();
    ASSERT_TRUE(rectNear(bounding, QRectF(0, 0, 120, 90), 0.5)) << "precondition: SVG page box";
    EXPECT_TRUE(rectNear(page->mediaBox(), bounding, 1e-6));
}

}  // namespace
