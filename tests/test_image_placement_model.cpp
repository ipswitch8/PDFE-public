// -*- coding: utf-8 -*-
//
// Image placement model, phase 2 of pipeline image-placement-state
// (docs/image-placement-model.md). The phase-1 probes
// (test_image_placement_state.cpp, test_image_editor_state_probes.cpp) pin the
// defects; these pin the rules the fix decided:
//
//   * a second crop is the INTERSECTION of the visible region and the new
//     rect (crops compose; a crop never brings back a cropped-away part);
//   * repeated edits replace PDFE's wrapper instead of nesting a new one, so
//     no dead `re W n` fragment accumulates;
//   * an edit touches only the targeted placement's bytes (name + occurrence);
//   * a legacy page-space crop is read and converted to the unit-space form;
//   * flips are reported as flips, not as a 180 degree rotation;
//   * images inside a Form XObject are enumerated but not editable;
//   * undo restores the ExtGState resource and the translucency of a
//     gs-wrapped image (E11 control), and undo/redo of a crop restores the
//     exact content bytes;
//   * a form placement can never alias a page placement of the same name;
//   * the content reader survives inline images whose data holds "EI"/"Do",
//     /Contents arrays with q/Q split across streams, a shared content
//     stream, and malformed content;
//   * a third-party rotated clip is not "converted" as a legacy crop;
//   * a refused edit leaves no undo entry and no dirty page.
//
// Facts are read back from the page (enumeration, render pixels, raw content
// bytes of a recovery copy), never from a tool's cache.
#include <gtest/gtest.h>
#include "image/ImageCommands.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "image/ImageTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include <QAction>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QApplication>
#include <QImage>
#include <QRectF>
#include <QTemporaryDir>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
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

namespace {

void ensureQApplication() {
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = { nullptr };
        static QApplication app(argc, argv);
    }
}

std::string fixture(const char* name) {
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

// Upright, 200x120 at (150, 200), fitz Y-down.
const QRectF kRect(150.0, 200.0, 200.0, 120.0);

int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

struct Geom {
    float x0 = 0, y1 = 0;
    std::string place(const QRectF& r) const {
        std::ostringstream o;
        o << r.width() << " 0 0 " << r.height() << " " << (x0 + r.x()) << " "
          << (y1 - r.y() - r.height());
        return o.str();
    }
    std::string re(const QRectF& r) const {
        std::ostringstream o;
        o << (x0 + r.x()) << " " << (y1 - r.y() - r.height()) << " " << r.width() << " "
          << r.height();
        return o.str();
    }
};

} // namespace

class ImagePlacementModelBase : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
        png_ = path("red.png");
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        ASSERT_TRUE(red.save(QString::fromStdString(png_)));
    }
    void TearDown() override {
        page_.reset();
        doc_.reset();
    }

    std::string path(const char* leaf) { return tmp_.filePath(leaf).toStdString(); }

    // Insert the red square at kRect into test_single.pdf; keeps it open.
    void openWithInsert() {
        doc_ = Document::open(context_, base_);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        name_ = r.imageInfo.xobjectName;
        reload();
    }

    using Rewrite = std::function<std::string(fz_context*, pdf_document*, pdf_page*,
                                              const Geom&, const std::string&)>;

    // Insert the red square, then replace page 0's whole content with the
    // text `rewrite` returns, and open the result.
    void build(const Rewrite& rewrite) {
        openWithInsert();
        page_.reset();
        const std::string a = path("a.pdf");
        doc_->saveAs(a);
        doc_.reset();
        const std::string out = path("built.pdf");
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, a.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
            Geom g;
            g.x0 = mb.x0;
            g.y1 = mb.y1;
            const std::string text = rewrite(ctx, pdf, pp, g, name_);
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
        doc_ = Document::open(context_, out);
        ASSERT_NE(doc_, nullptr);
        reload();
    }

    void reload() { page_ = doc_->page(0); }

    // Insert the red square, save, let `mutate` change the saved file freely
    // through raw MuPDF (contents, resources, pages), save and open it.
    using Mutate = std::function<void(fz_context*, pdf_document*, pdf_page*, const Geom&,
                                      const std::string&)>;
    void buildCustom(const Mutate& mutate) {
        openWithInsert();
        page_.reset();
        const std::string a = path("a.pdf");
        doc_->saveAs(a);
        doc_.reset();
        const std::string out = path("custom.pdf");
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, a.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
            Geom g;
            g.x0 = mb.x0;
            g.y1 = mb.y1;
            mutate(ctx, pdf, pp, g, name_);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, out.c_str(), &wo);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        doc_ = Document::open(context_, out);
        ASSERT_NE(doc_, nullptr);
        reload();
    }
    static pdf_obj* addStream(fz_context* ctx, pdf_document* pdf, const std::string& text) {
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
        pdf_obj* stm = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
        fz_drop_buffer(ctx, buf);
        return stm;
    }
    // Content of page `index` of a file (decoded, streams concatenated).
    std::string contentOfPage(const std::string& file, int index) {
        std::string out;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, index);
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
    QImage renderPage() {
        reload();
        return page_->render(QRectF(), 1.0f);
    }
    // Pixels whose colour differs by more than 60 in any channel.
    static int differingPixels(const QImage& a, const QImage& b) {
        if (a.size() != b.size()) return a.width() * a.height();
        int n = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                if (std::abs(qRed(p) - qRed(q)) > 60 || std::abs(qGreen(p) - qGreen(q)) > 60 ||
                    std::abs(qBlue(p) - qBlue(q)) > 60 || std::abs(qAlpha(p) - qAlpha(q)) > 60)
                    ++n;
            }
        return n;
    }
    std::vector<PageImageInfo> images() {
        reload();
        return manager_->enumerateImages(page_.get(), doc_.get());
    }
    PageImageInfo only() {
        auto v = images();
        EXPECT_EQ(v.size(), 1u);
        return v.empty() ? PageImageInfo{} : v[0];
    }
    QRgb pixel(QPointF p) {
        reload();
        const QImage img = page_->render(QRectF(), 1.0f);
        const int x = static_cast<int>(std::lround(p.x()));
        const int y = static_cast<int>(std::lround(p.y()));
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return qRgb(0, 0, 255);
        return img.pixel(x, y);
    }
    bool isRed(QPointF p) {
        const QRgb c = pixel(p);
        return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90;
    }
    bool isWhite(QPointF p) {
        const QRgb c = pixel(p);
        return qAlpha(c) < 30 || (qRed(c) > 230 && qGreen(c) > 230 && qBlue(c) > 230);
    }

    // Decoded page-0 content of a PDF file, all streams concatenated.
    std::string contentOf(const std::string& file) {
        std::string out;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
            auto add = [&](pdf_obj* s) {
                fz_buffer* b = pdf_load_stream(ctx, s);
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
    // The live document's page-0 content, through a recovery copy (which does
    // not disturb the editing session).
    std::string liveContent(const char* leaf) {
        const std::string f = path(leaf);
        doc_->writeRecoveryCopy(f);
        return contentOf(f);
    }
    bool fileHasExtGState(const std::string& file, const char* gs) {
        bool has = false;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* eg = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(ExtGState));
            has = eg && pdf_dict_gets(ctx, eg, gs) != nullptr;
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return has;
    }
    static void addGraphicsState(fz_context* ctx, pdf_document* pdf, pdf_page* pp, const char* gs,
                                 float opacity) {
        pdf_obj* res = pdf_page_resources(ctx, pp);
        pdf_obj* eg = pdf_dict_get(ctx, res, PDF_NAME(ExtGState));
        if (!eg) {
            eg = pdf_new_dict(ctx, pdf, 1);
            pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), eg);
        }
        pdf_obj* d = pdf_new_dict(ctx, pdf, 2);
        pdf_dict_puts_drop(ctx, d, "CA", pdf_new_real(ctx, opacity));
        pdf_dict_puts_drop(ctx, d, "ca", pdf_new_real(ctx, opacity));
        pdf_dict_puts_drop(ctx, eg, gs, d);
    }

    // Image > Crop style: a fraction of the CURRENT visible region.
    bool cropFraction(const PageImageInfo& img, double fx, double fy, double fw, double fh) {
        const QRectF b = img.boundingBox;
        return editor_->cropImage(page_.get(), doc_.get(), img.xobjectName,
                                  QRectF(b.x() + b.width() * fx, b.y() + b.height() * fy,
                                         b.width() * fw, b.height() * fh),
                                  img.displayWidth, img.displayHeight,
                                  float(b.x()), float(b.y()));
    }

    QTemporaryDir tmp_;
    std::string base_, png_, name_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
};

// ---------------------------------------------------------------------------
// Second crop: crops compose by intersection.
// ---------------------------------------------------------------------------
class ImagePlacementStateCropTest : public ImagePlacementModelBase {};

TEST_F(ImagePlacementStateCropTest, SecondCropIntersectsTheFirst) {
    openWithInsert();
    ASSERT_TRUE(cropFraction(only(), 0.25, 0.25, 0.5, 0.5));  // centre half
    const PageImageInfo first = only();
    ASSERT_NEAR(first.boundingBox.width(), 100.0, 0.5) << "precondition: first crop took effect";
    // Keep the LEFT half of what is visible now, with a rect that reaches
    // well OUTSIDE the first crop (40pt left, 20pt above and below): under
    // "replace" semantics the crop would grow to this rect; under the
    // decided intersection it is the left half of the first crop.
    {
        const QRectF b = first.boundingBox;
        ASSERT_TRUE(editor_->cropImage(page_.get(), doc_.get(), name_,
                                       QRectF(b.x() - 40.0, b.y() - 20.0,
                                              b.width() * 0.5 + 40.0, b.height() + 40.0),
                                       first.displayWidth, first.displayHeight,
                                       float(b.x()), float(b.y())));
    }
    const PageImageInfo second = only();
    EXPECT_NEAR(second.crop[0], 0.25f, 1e-4f);
    EXPECT_NEAR(second.crop[2], 0.50f, 1e-4f) << "the second crop replaced the first instead of composing";
    EXPECT_NEAR(second.crop[1], 0.25f, 1e-4f);
    EXPECT_NEAR(second.crop[3], 0.75f, 1e-4f);
    EXPECT_NEAR(second.boundingBox.width(), 50.0, 0.5);
    EXPECT_NEAR(second.boundingBox.height(), 60.0, 0.5);
    EXPECT_NEAR(second.boundingBox.center().x(), kRect.x() + 0.375 * kRect.width(), 0.5);
    EXPECT_NEAR(second.displayWidth, kRect.width(), 0.05) << "a crop must not rescale the image";
    // Render: left quarter of the centre is drawn, the right of it is not.
    EXPECT_TRUE(isRed(QPointF(kRect.x() + 0.375 * kRect.width(), kRect.center().y())));
    EXPECT_TRUE(isWhite(QPointF(kRect.x() + 0.625 * kRect.width(), kRect.center().y())))
        << "the part the second crop removed is still drawn";
    EXPECT_TRUE(isWhite(kRect.topLeft() + QPointF(10, 10)))
        << "the part the first crop removed came back";
    // Inside the second rect but outside the first crop: must stay hidden.
    EXPECT_TRUE(isWhite(QPointF(kRect.x() + 0.15 * kRect.width(), kRect.center().y())))
        << "the second crop replaced the first instead of intersecting with it";
}

TEST_F(ImagePlacementStateCropTest, CropCannotBringBackACroppedAwayPart) {
    openWithInsert();
    ASSERT_TRUE(cropFraction(only(), 0.25, 0.25, 0.5, 0.5));
    const PageImageInfo first = only();
    // A rect covering the whole ORIGINAL image: the intersection is the
    // current visible region, so nothing changes.
    ASSERT_TRUE(editor_->cropImage(page_.get(), doc_.get(), name_, kRect.adjusted(-20, -20, 20, 20),
                                   first.displayWidth, first.displayHeight,
                                   float(first.boundingBox.x()), float(first.boundingBox.y())));
    const PageImageInfo again = only();
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(again.crop[i], first.crop[i], 1e-4f);
    EXPECT_TRUE(isWhite(kRect.topLeft() + QPointF(10, 10)))
        << "a larger crop rect brought back a cropped-away part";
}

TEST_F(ImagePlacementStateCropTest, CropOutsideTheVisibleRegionChangesNothing) {
    openWithInsert();
    const std::string before = liveContent("before.pdf");
    EXPECT_FALSE(editor_->cropImage(page_.get(), doc_.get(), name_,
                                    QRectF(10, 10, 30, 30), 0, 0, 0, 0))
        << "an empty intersection must be refused";
    EXPECT_EQ(liveContent("after.pdf"), before) << "a refused crop changed the page";
}

// ---------------------------------------------------------------------------
// The wrapper is replaced, never nested; the clip lives in unit space.
// ---------------------------------------------------------------------------
class ImagePlacementStateWrapperTest : public ImagePlacementModelBase {};

TEST_F(ImagePlacementStateWrapperTest, RepeatedEditsLeaveOneWrapperAndOneClip) {
    openWithInsert();
    ASSERT_TRUE(cropFraction(only(), 0.25, 0.25, 0.5, 0.5));
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 30.0f, float(c.x()),
                                     float(c.y()), float(kRect.width()), float(kRect.height())));
    PageImageInfo i = only();
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth,
                                     i.displayHeight, float(i.placementBox.x() + 15.0),
                                     float(i.placementBox.y() + 9.0), -i.rotationRadians * 57.29578f));
    i = only();
    ASSERT_TRUE(cropFraction(i, 0.1, 0.1, 0.8, 0.8));
    const std::string content = liveContent("wrap.pdf");
    EXPECT_EQ(count(content, "/" + name_ + " Do"), 1) << content;
    EXPECT_EQ(count(content, "re W n"), 1) << "a dead clip fragment was left behind:\n" << content;
    // The clip sits INSIDE the cm, in the unit square: `cm <u0 v0 du dv> re W n /Im Do Q`.
    const size_t cm = content.find(" cm ");
    const size_t re = content.find("re W n");
    const size_t d = content.find("/" + name_ + " Do Q");
    ASSERT_NE(cm, std::string::npos);
    ASSERT_NE(d, std::string::npos);
    EXPECT_LT(cm, re);
    EXPECT_LT(re, d);
    std::istringstream nums(content.substr(cm + 4, re - cm - 4));
    double u0 = -1, v0 = -1, du = -1, dv = -1;
    nums >> u0 >> v0 >> du >> dv;
    EXPECT_GE(u0, 0.0);
    EXPECT_LE(u0 + du, 1.0 + 1e-6) << "the clip is not in the image's unit square";
    EXPECT_GE(v0, 0.0);
    EXPECT_LE(v0 + dv, 1.0 + 1e-6);
    // Two compositions of the centre half: 0.25 + 0.1 * 0.5 = 0.3 .. 0.7.
    EXPECT_NEAR(u0, 0.3, 1e-4);
    EXPECT_NEAR(du, 0.4, 1e-4);
    EXPECT_NEAR(std::abs(only().rotationRadians), 30.0 * 3.14159265 / 180.0, 1e-3)
        << "the rotation did not survive the move and the second crop";
}

TEST_F(ImagePlacementStateWrapperTest, LegacyPageSpaceCropIsReadAndConvertedOnTheNextEdit) {
    // The crop the editor wrote before the fix: page-space clip OUTSIDE the cm.
    const QRectF keep(kRect.x() + 50, kRect.y() + 30, 100, 60);  // the centre half
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "q " + g.re(keep) + " re W n " + g.place(kRect) + " cm /" + n + " Do Q\n";
    });
    PageImageInfo legacy = only();
    EXPECT_TRUE(legacy.cropped) << "a legacy page-space crop is not read as a crop";
    EXPECT_NEAR(legacy.crop[0], 0.25f, 1e-3f);
    EXPECT_NEAR(legacy.crop[2], 0.75f, 1e-3f);
    EXPECT_NEAR(legacy.boundingBox.width(), 100.0, 0.5);
    EXPECT_NEAR(legacy.boundingBox.center().x(), kRect.center().x(), 0.5);
    ASSERT_TRUE(isWhite(kRect.topLeft() + QPointF(10, 10))) << "oracle: the legacy crop renders";

    const QPointF d(40, 25);
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, legacy.displayWidth,
                                     legacy.displayHeight, float(legacy.placementBox.x() + d.x()),
                                     float(legacy.placementBox.y() + d.y())));
    const std::string content = liveContent("legacy.pdf");
    EXPECT_EQ(count(content, "re W n"), 1) << content;
    EXPECT_LT(content.find(" cm "), content.find("re W n"))
        << "the legacy page-space clip was not converted to the unit-space form:\n" << content;
    const PageImageInfo moved = only();
    EXPECT_NEAR(moved.crop[0], 0.25f, 1e-3f);
    EXPECT_NEAR(moved.crop[3], 0.75f, 1e-3f);
    EXPECT_TRUE(isWhite(kRect.topLeft() + d + QPointF(10, 10))) << "the crop was dropped by the move";
    EXPECT_TRUE(isRed(kRect.center() + d));
}

// ---------------------------------------------------------------------------
// Identity: name + occurrence.
// ---------------------------------------------------------------------------
class ImagePlacementStateIdentityTest : public ImagePlacementModelBase {};

TEST_F(ImagePlacementStateIdentityTest, EditingTheSecondPlacementLeavesEveryOtherByte) {
    const QRectF second = kRect.translated(0.0, 250.0);
    const std::string tail = "0 0 1 rg 20 20 30 30 re f\n";
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "q " + g.place(kRect) + " cm /" + n + " Do Q\nq " + g.place(second) + " cm /" + n +
               " Do Q\n" + tail;
    });
    const auto imgs = images();
    ASSERT_EQ(imgs.size(), 2u);
    EXPECT_EQ(imgs[0].occurrence, 0);
    EXPECT_EQ(imgs[1].occurrence, 1);
    const std::string before = liveContent("before.pdf");

    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), PlacementId{name_, 1},
                                     imgs[1].displayWidth, imgs[1].displayHeight,
                                     float(second.x() + 30.0), float(second.y())));
    const std::string after = liveContent("after.pdf");
    // Everything before the second placement and everything after it is
    // byte-for-byte what it was.
    const size_t start = before.find("\nq ") + 1;  // start of the 2nd placement
    const size_t end = before.find(tail);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(after.substr(0, start), before.substr(0, start))
        << "the first placement's bytes changed";
    EXPECT_EQ(after.substr(after.size() - (before.size() - end)), before.substr(end))
        << "the content after the edited placement changed";
    const auto now = images();
    ASSERT_EQ(now.size(), 2u);
    EXPECT_NEAR(now[0].boundingBox.x(), kRect.x(), 0.01) << "the first placement moved";
    EXPECT_NEAR(now[1].boundingBox.x(), second.x() + 30.0, 0.01) << "the second placement did not move";
    EXPECT_TRUE(isRed(kRect.center()));
    EXPECT_TRUE(isRed(second.center() + QPointF(30.0, 0.0)));
}

TEST_F(ImagePlacementStateIdentityTest, RemovePlacementRemovesOnlyThatCopy) {
    const QRectF second = kRect.translated(0.0, 250.0);
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "q " + g.place(kRect) + " cm /" + n + " Do Q\nq " + g.place(second) + " cm /" + n +
               " Do Q\n";
    });
    ASSERT_EQ(images().size(), 2u);
    auto r = manager_->removePlacement(page_.get(), doc_.get(), name_, 0);
    ASSERT_TRUE(r.success) << r.errorMessage;
    const auto left = images();
    ASSERT_EQ(left.size(), 1u) << "removing one placement removed the other copy too";
    EXPECT_NEAR(left[0].boundingBox.center().y(), second.center().y(), 0.01);
    EXPECT_TRUE(isWhite(kRect.center()));
    EXPECT_TRUE(isRed(second.center())) << "the remaining copy lost its image resource";
}

TEST_F(ImagePlacementStateIdentityTest, ImageInsideAFormIsEnumeratedButNotEditable) {
    build([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g, const std::string& n) {
        pdf_obj* res = pdf_page_resources(ctx, pp);
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        pdf_obj* img = pdf_keep_obj(ctx, pdf_dict_gets(ctx, xo, n.c_str()));
        const std::string body = "q " + g.place(kRect) + " cm /" + n + " Do Q\n";
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(body.data()), body.size());
        pdf_obj* form = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
        fz_drop_buffer(ctx, buf);
        pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), fz_make_rect(g.x0, g.y1 - 842, g.x0 + 596, g.y1));
        // Shift the form by 10 points right: enumeration composes the matrix.
        pdf_dict_put_matrix(ctx, form, PDF_NAME(Matrix), fz_translate(10, 0));
        pdf_obj* fres = pdf_new_dict(ctx, pdf, 1);
        pdf_obj* fxo = pdf_new_dict(ctx, pdf, 1);
        pdf_dict_puts(ctx, fxo, n.c_str(), img);
        pdf_dict_put_drop(ctx, fres, PDF_NAME(XObject), fxo);
        pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), fres);
        pdf_drop_obj(ctx, img);
        pdf_dict_dels(ctx, xo, n.c_str());
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        return std::string("/Fm0 Do\n");
    });
    const PageImageInfo i = only();
    EXPECT_TRUE(i.inFormXObject);
    EXPECT_NEAR(i.boundingBox.center().x(), kRect.center().x() + 10.0, 0.01)
        << "the form's /Matrix was not composed into the placement";
    EXPECT_TRUE(isRed(kRect.center() + QPointF(10.0, 0.0))) << "oracle: rendered through the form";
    const std::string before = liveContent("before.pdf");
    EXPECT_FALSE(editor_->resizeImage(page_.get(), doc_.get(), name_, 50, 50, 10, 10))
        << "an image inside a Form XObject must not be edited through the page";
    EXPECT_EQ(liveContent("after.pdf"), before);
}

// ---------------------------------------------------------------------------
// Flips are flips.
// ---------------------------------------------------------------------------
class ImagePlacementStateFlipTest : public ImagePlacementModelBase {};

TEST_F(ImagePlacementStateFlipTest, MirrorAndVerticalFlipAreNotReadAsHalfTurns) {
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        std::ostringstream o;
        o << "q " << -kRect.width() << " 0 0 " << kRect.height() << " "
          << (g.x0 + kRect.x() + kRect.width()) << " " << (g.y1 - kRect.y() - kRect.height())
          << " cm /" << n << " Do Q\n";
        return o.str();
    });
    PageImageInfo m = only();
    EXPECT_TRUE(m.flippedX);
    EXPECT_FALSE(m.flippedY);
    EXPECT_NEAR(m.rotationRadians, 0.0f, 1e-4f) << "a mirror read as a rotation";
    EXPECT_NEAR(m.boundingBox.center().x(), kRect.center().x(), 0.01);

    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        std::ostringstream o;
        o << "q " << kRect.width() << " 0 0 " << -kRect.height() << " " << (g.x0 + kRect.x())
          << " " << (g.y1 - kRect.y()) << " cm /" << n << " Do Q\n";
        return o.str();
    });
    PageImageInfo v = only();
    EXPECT_TRUE(v.flippedY);
    EXPECT_FALSE(v.flippedX);
    EXPECT_NEAR(v.rotationRadians, 0.0f, 1e-4f) << "a vertical flip read as a half turn";

    // Rotating a mirrored image keeps the mirror.
    const QPointF c = kRect.center();
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        std::ostringstream o;
        o << "q " << -kRect.width() << " 0 0 " << kRect.height() << " "
          << (g.x0 + kRect.x() + kRect.width()) << " " << (g.y1 - kRect.y() - kRect.height())
          << " cm /" << n << " Do Q\n";
        return o.str();
    });
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 30.0f, float(c.x()),
                                     float(c.y()), float(kRect.width()), float(kRect.height())));
    const PageImageInfo r = only();
    EXPECT_TRUE(r.flippedX || r.flippedY) << "rotating a mirrored image dropped the mirror";
    EXPECT_LT(r.ctm[0] * r.ctm[3] - r.ctm[1] * r.ctm[2], 0.0f) << "the determinant changed sign";
}

// ---------------------------------------------------------------------------
// Undo.
// ---------------------------------------------------------------------------
class ImagePlacementStateUndoTest : public ImagePlacementModelBase {};

// E11 control (docs/image-placement-model.md): edit + undo of a gs-wrapped
// image brings back its translucency and its ExtGState resource.
TEST_F(ImagePlacementStateUndoTest, UndoOfAnEditRestoresTranslucencyAndExtGState) {
    build([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g, const std::string& n) {
        addGraphicsState(ctx, pdf, pp, "GS1", 0.5f);
        return "q /GS1 gs " + g.place(kRect) + " cm /" + n + " Do Q\n";
    });
    auto alphaAt = [&](QPointF p) { return qAlpha(pixel(p)); };
    ASSERT_GT(alphaAt(kRect.center()), 90);
    ASSERT_LT(alphaAt(kRect.center()), 170) << "oracle: translucent";
    const PageImageInfo i = only();
    auto cmdPage = doc_->page(0);
    ResizeImageCommand cmd(manager_.get(), editor_.get(), cmdPage.get(), doc_.get(), name_,
                           i.displayWidth, i.displayHeight, float(i.boundingBox.x() + 120.0),
                           float(i.boundingBox.y() + 200.0));
    cmd.execute();
    ASSERT_GT(alphaAt(kRect.center() + QPointF(120.0, 200.0)), 90) << "oracle: moved";
    cmd.undo();
    const int a = alphaAt(kRect.center());
    EXPECT_GT(a, 90) << "undo did not bring the image back";
    EXPECT_LT(a, 170) << "undo brought the image back fully opaque";
    const std::string saved = path("undone.pdf");
    doc_->writeRecoveryCopy(saved);
    EXPECT_TRUE(fileHasExtGState(saved, "GS1")) << "undo lost the ExtGState resource";
}

// restoreImage puts the snapshot's ExtGState back even when the live page
// lost it after the snapshot was taken (the mechanism the control relies on).
TEST_F(ImagePlacementStateUndoTest, RestoreImagePutsBackTheExtGStateResource) {
    build([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g, const std::string& n) {
        addGraphicsState(ctx, pdf, pp, "GS1", 0.5f);
        return "q /GS1 gs " + g.place(kRect) + " cm /" + n + " Do Q\n";
    });
    void* snap = manager_->saveContentsRef(page_.get(), doc_.get());
    ASSERT_NE(snap, nullptr);
    // A copy of the page with the ExtGState gone.
    page_.reset();
    const std::string withGs = path("with.pdf");
    doc_->writeRecoveryCopy(withGs);
    const std::string without = path("without.pdf");
    context_->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, withGs.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, 0);
        pdf_dict_del(ctx, pdf_page_resources(ctx, pp), PDF_NAME(ExtGState));
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, pdf, without.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });
    ASSERT_FALSE(fileHasExtGState(without, "GS1"));
    doc_ = Document::open(context_, without);
    ASSERT_NE(doc_, nullptr);
    reload();
    auto r = manager_->restoreImage(page_.get(), doc_.get(), {}, nullptr, snap);
    EXPECT_TRUE(r.success);
    manager_->releaseRef(snap);
    const std::string restored = path("restored.pdf");
    doc_->writeRecoveryCopy(restored);
    EXPECT_TRUE(fileHasExtGState(restored, "GS1")) << "restoreImage did not restore the ExtGState";
    const int a = qAlpha(pixel(kRect.center()));
    EXPECT_GT(a, 90);
    EXPECT_LT(a, 170) << "the restored image is not translucent";
}

TEST_F(ImagePlacementStateUndoTest, UndoAndRedoOfCropRestoreTheExactContentBytes) {
    openWithInsert();
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 30.0f, float(c.x()),
                                     float(c.y()), float(kRect.width()), float(kRect.height())));
    const PageImageInfo i = only();
    const std::string before = liveContent("b0.pdf");
    auto cmdPage = doc_->page(0);
    const QRectF b = i.boundingBox;
    CropImageCommand cmd(manager_.get(), editor_.get(), cmdPage.get(), doc_.get(), name_,
                         QRectF(b.x() + 20, b.y() + 10, b.width() - 60, b.height() - 30),
                         i.displayWidth, i.displayHeight, float(b.x()), float(b.y()));
    cmd.execute();
    const std::string cropped = liveContent("b1.pdf");
    ASSERT_NE(cropped, before) << "oracle: the crop changed the content";
    ASSERT_TRUE(only().cropped);
    cmd.undo();
    EXPECT_EQ(liveContent("b2.pdf"), before) << "undo of a crop did not restore the exact content";
    EXPECT_FALSE(only().cropped);
    cmd.redo();
    EXPECT_EQ(liveContent("b3.pdf"), cropped) << "redo of a crop did not reproduce the exact content";
    cmd.undo();
    EXPECT_EQ(liveContent("b4.pdf"), before) << "a second undo differs from the first";
}

// ---------------------------------------------------------------------------
// karen phase-2 findings: identity across forms, robustness, legacy scope,
// refused edits.
// ---------------------------------------------------------------------------

namespace {
// A Form XObject drawing image `img` under the name `n` at `r`; `ownResources`
// false makes it inherit the page's resources.
pdf_obj* makeForm(fz_context* ctx, pdf_document* pdf, const Geom& g, const std::string& n,
                  pdf_obj* img, const QRectF& r, bool ownResources) {
    const std::string body = "q " + g.place(r) + " cm /" + n + " Do Q\n";
    fz_buffer* buf = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(body.data()), body.size());
    pdf_obj* form = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
    fz_drop_buffer(ctx, buf);
    pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
    pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), fz_make_rect(g.x0, g.y1 - 842, g.x0 + 596, g.y1));
    if (ownResources) {
        pdf_obj* fres = pdf_new_dict(ctx, pdf, 1);
        pdf_obj* fxo = pdf_new_dict(ctx, pdf, 1);
        pdf_dict_puts(ctx, fxo, n.c_str(), img);
        pdf_dict_put_drop(ctx, fres, PDF_NAME(XObject), fxo);
        pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), fres);
    }
    return form;
}
} // namespace

// The page draws /Im0, and a form draws ITS OWN /Im0 (same name, the form's
// own resources). The form entry must not alias the page placement: editing
// or removing it is refused and changes nothing; editing the page one works
// and leaves the form one alone.
TEST_F(ImagePlacementStateIdentityTest, AFormImageWithThePageImagesNameDoesNotAliasIt) {
    const QRectF inForm = kRect.translated(0.0, 250.0);
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        pdf_obj* img = pdf_dict_gets(ctx, xo, n.c_str());
        pdf_obj* form = makeForm(ctx, pdf, g, n, img, inForm, /*ownResources=*/true);
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        pdf_obj* stm = addStream(ctx, pdf, "q " + g.place(kRect) + " cm /" + n + " Do Q\n/Fm0 Do\n");
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
    });
    const auto imgs = images();
    ASSERT_EQ(imgs.size(), 2u);
    const PageImageInfo& pageImg = imgs[0].inFormXObject ? imgs[1] : imgs[0];
    const PageImageInfo& formImg = imgs[0].inFormXObject ? imgs[0] : imgs[1];
    ASSERT_FALSE(pageImg.inFormXObject);
    ASSERT_TRUE(formImg.inFormXObject);
    EXPECT_EQ(pageImg.xobjectName, formImg.xobjectName) << "oracle: the names collide";
    EXPECT_EQ(pageImg.occurrence, 0);
    EXPECT_EQ(formImg.occurrence, -1) << "a form placement has an addressable page identity";
    const int formIndex = formImg.imageIndex;

    const std::string before = liveContent("before.pdf");
    const QImage beforeRender = renderPage();
    EXPECT_FALSE(editor_->resizeImage(page_.get(), doc_.get(),
                                      PlacementId{formImg.xobjectName, formImg.occurrence},
                                      50, 50, 10, 10));
    EXPECT_FALSE(editor_->cropImage(page_.get(), doc_.get(),
                                    PlacementId{formImg.xobjectName, formImg.occurrence},
                                    formImg.boundingBox.adjusted(10, 10, -10, -10)));
    EXPECT_FALSE(manager_->removePlacement(page_.get(), doc_.get(), formImg.xobjectName,
                                           formImg.occurrence).success);
    EXPECT_FALSE(manager_->removeImage(page_.get(), doc_.get(), formIndex).success)
        << "removeImage(index of the form entry) removed something";
    EXPECT_EQ(liveContent("after.pdf"), before) << "a refused form edit changed the page content";
    EXPECT_EQ(differingPixels(beforeRender, renderPage()), 0);
    EXPECT_EQ(images().size(), 2u);

    // The page's own placement is editable, and the form's stays put.
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), PlacementId{pageImg.xobjectName, 0},
                                     pageImg.displayWidth, pageImg.displayHeight,
                                     float(kRect.x() + 60.0), float(kRect.y())));
    EXPECT_TRUE(isRed(kRect.center() + QPointF(60.0, 0.0)));
    EXPECT_TRUE(isRed(inForm.center())) << "editing the page placement moved the form's";
}

// A form that INHERITS the page's resources draws the same /Im0: removing the
// page's own placement must keep the XObject entry the form still needs.
TEST_F(ImagePlacementStateIdentityTest, RemovingThePagePlacementKeepsAResourceAFormStillUses) {
    const QRectF inForm = kRect.translated(0.0, 250.0);
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        pdf_obj* form = makeForm(ctx, pdf, g, n, nullptr, inForm, /*ownResources=*/false);
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        pdf_obj* stm = addStream(ctx, pdf, "q " + g.place(kRect) + " cm /" + n + " Do Q\n/Fm0 Do\n");
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
    });
    ASSERT_EQ(images().size(), 2u);
    ASSERT_TRUE(isRed(inForm.center())) << "oracle: the form draws through inherited resources";
    auto r = manager_->removePlacement(page_.get(), doc_.get(), name_, 0);
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_TRUE(isWhite(kRect.center()));
    EXPECT_TRUE(isRed(inForm.center())) << "the XObject the form still draws was deleted";
}

class ImagePlacementStateRobustnessTest : public ImagePlacementModelBase {};

// Inline image data holding "EI", "/Im0 Do" and NUL bytes, before the target.
TEST_F(ImagePlacementStateRobustnessTest, InlineImageDataWithEiAndDoBytesIsSkipped) {
    // 16 gray samples: ASCII that looks like operators, plus binary.
    const std::string samples("a EI q /Im0 Do Q\x00\xff", 18);
    std::string inlineImg = "q 40 0 0 10 20 20 cm BI /W 18 /H 1 /BPC 8 /CS /G ID ";
    inlineImg += samples;
    inlineImg += "\nEI Q\n";
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* stm = addStream(ctx, pdf, inlineImg + "q " + g.place(kRect) + " cm /" + n + " Do Q\n");
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
    });
    const PageImageInfo i = only();
    EXPECT_EQ(i.occurrence, 0) << "a Do inside the inline image data was counted";
    EXPECT_NEAR(i.boundingBox.center().x(), kRect.center().x(), 0.01);
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth, i.displayHeight,
                                     float(kRect.x() + 40.0), float(kRect.y())));
    const std::string content = liveContent("inline.pdf");
    EXPECT_NE(content.find(inlineImg), std::string::npos) << "the inline image bytes were changed";
    EXPECT_TRUE(isRed(kRect.center() + QPointF(40.0, 0.0)));
    EXPECT_EQ(only().occurrence, 0);
}

// /Contents is an array; the target is in the 2nd stream; a q ... Q with a
// translation is split across the two streams.
TEST_F(ImagePlacementStateRobustnessTest, ContentsArrayWithQSplitAcrossStreams) {
    const std::string first = "q 1 0 0 1 10 -20 cm\n";
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* arr = pdf_new_array(ctx, pdf, 2);
        pdf_obj* s0 = addStream(ctx, pdf, first);
        pdf_obj* s1 = addStream(ctx, pdf, "q " + g.place(kRect) + " cm /" + n + " Do Q\nQ\n");
        pdf_array_push_drop(ctx, arr, s0);
        pdf_array_push_drop(ctx, arr, s1);
        pdf_dict_put_drop(ctx, pp->obj, PDF_NAME(Contents), arr);
    });
    // The split translation moves the image 10 right and 20 down (fitz).
    const PageImageInfo i = only();
    EXPECT_NEAR(i.boundingBox.center().x(), kRect.center().x() + 10.0, 0.01);
    EXPECT_NEAR(i.boundingBox.center().y(), kRect.center().y() + 20.0, 0.01);
    ASSERT_TRUE(isRed(kRect.center() + QPointF(10.0, 20.0)));
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth, i.displayHeight,
                                     float(kRect.x() + 100.0), float(kRect.y() + 50.0)));
    const PageImageInfo moved = only();
    EXPECT_NEAR(moved.boundingBox.x(), kRect.x() + 100.0, 0.01) << "the outer CTM was not compensated";
    EXPECT_NEAR(moved.boundingBox.y(), kRect.y() + 50.0, 0.01);
    EXPECT_TRUE(isRed(kRect.center() + QPointF(100.0, 50.0)));
    const std::string content = liveContent("arr.pdf");
    EXPECT_EQ(content.substr(0, first.size()), first) << "the first stream changed";
}

// Two pages share ONE content stream object: editing page 1 leaves page 2.
TEST_F(ImagePlacementStateRobustnessTest, SharedContentStreamIsNotChangedOnTheOtherPage) {
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* stm = addStream(ctx, pdf, "q " + g.place(kRect) + " cm /" + n + " Do Q\n");
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        fz_buffer* empty = fz_new_buffer(ctx, 1);
        pdf_obj* p2 = pdf_add_page(ctx, pdf, pdf_bound_page(ctx, pp, FZ_MEDIA_BOX), 0,
                                   pdf_page_resources(ctx, pp), empty);
        fz_drop_buffer(ctx, empty);
        pdf_dict_put(ctx, p2, PDF_NAME(Contents), stm);  // the SAME stream object
        pdf_insert_page(ctx, pdf, -1, p2);
        pdf_drop_obj(ctx, p2);
        pdf_drop_obj(ctx, stm);
    });
    ASSERT_EQ(doc_->pageCount(), 2);
    const std::string page2Before = contentOfPage(path("custom.pdf"), 1);
    const PageImageInfo i = only();
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth, i.displayHeight,
                                     float(kRect.x() + 70.0), float(kRect.y())));
    const std::string f = path("shared.pdf");
    doc_->writeRecoveryCopy(f);
    EXPECT_EQ(contentOfPage(f, 1), page2Before) << "editing page 1 rewrote page 2's shared content";
    EXPECT_NE(contentOfPage(f, 0), page2Before) << "oracle: page 1 changed";
    auto page2 = doc_->page(1);
    const auto onPage2 = manager_->enumerateImages(page2.get(), doc_.get());
    ASSERT_EQ(onPage2.size(), 1u);
    EXPECT_NEAR(onPage2[0].boundingBox.x(), kRect.x(), 0.01) << "page 2's image moved";
}

// Unbalanced q/Q, a Do with no name, a Do with an unresolvable name, and a
// truncated stream: no crash; the resolvable image is still editable; the
// unresolvable name is refused and changes nothing.
TEST_F(ImagePlacementStateRobustnessTest, MalformedContentIsSurvivedAndBadTargetsRefused) {
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        const std::string text = "Q Q\n5 Do\nDo\nq /Nope Do\nq " + g.place(kRect) + " cm /" + n +
                                 " Do Q\nq 1 0 0 1 0 0 cm (unterminated string \\) q 200 0 0 12";
        pdf_obj* stm = addStream(ctx, pdf, text);
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
    });
    const PageImageInfo i = only();
    EXPECT_NEAR(i.boundingBox.center().x(), kRect.center().x(), 0.01);
    const std::string before = liveContent("m0.pdf");
    EXPECT_FALSE(editor_->resizeImage(page_.get(), doc_.get(), "Nope", 50, 50, 10, 10))
        << "an unresolvable name was edited";
    EXPECT_FALSE(manager_->removePlacement(page_.get(), doc_.get(), "Nope", 0).success);
    EXPECT_EQ(liveContent("m1.pdf"), before);
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth, i.displayHeight,
                                     float(kRect.x() + 30.0), float(kRect.y())));
    EXPECT_TRUE(isRed(kRect.center() + QPointF(30.0, 0.0)));
    const std::string after = liveContent("m2.pdf");
    EXPECT_NE(after.find("(unterminated string"), std::string::npos) << "the truncated tail was lost";
    EXPECT_NE(after.find("/Nope Do"), std::string::npos);
}

// A third-party clip in page space around a ROTATED placement is not a PDFE
// legacy crop: it is left untouched as context, and an identity edit does
// not change what is drawn.
TEST_F(ImagePlacementStateWrapperTest, RotatedThirdPartyClipIsLeftUntouched) {
    const QRectF clip(kRect.x() + 40, kRect.y() - 20, 90, 120);
    std::string clipText;
    build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        // 30 degree rotated placement of the 200x120 image about kRect's corner.
        const double t = 30.0 * 3.14159265358979 / 180.0;
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(6);
        o << 200 * std::cos(t) << " " << 200 * std::sin(t) << " " << -120 * std::sin(t) << " "
          << 120 * std::cos(t) << " " << (g.x0 + kRect.x() + 40) << " " << (g.y1 - kRect.bottom());
        clipText = g.re(clip) + " re W n ";
        return "q " + clipText + o.str() + " cm /" + n + " Do Q\n";
    });
    const PageImageInfo i = only();
    EXPECT_FALSE(i.cropped) << "a rotated third-party clip was read as a PDFE crop";
    const QImage before = renderPage();
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, i.displayWidth, i.displayHeight,
                                     float(i.placementBox.x()), float(i.placementBox.y()),
                                     -i.rotationRadians * 57.29577951f));
    const QImage after = renderPage();
    EXPECT_LE(differingPixels(before, after), 20) << "an identity edit changed what is drawn";
    const std::string content = liveContent("tp.pdf");
    EXPECT_NE(content.find("q " + clipText), std::string::npos)
        << "the third-party clip was rewritten:\n" << content;
    EXPECT_FALSE(only().cropped);
}

// A refused edit leaves no undo entry, no dirty page and no content change.
TEST_F(ImagePlacementStateUndoTest, RefusedEditLeavesNoUndoEntryAndACleanPage) {
    openWithInsert();
    const PageImageInfo i = only();
    const std::string before = liveContent("r0.pdf");
    const uint64_t version = doc_->contentVersion();
    int dirtied = 0;
    const auto token = doc_->addPageDirtyListener([&](int) { ++dirtied; });
    pdfe::commands::UndoManager undo;
    auto cmdPage = doc_->page(0);

    auto crop = std::make_unique<CropImageCommand>(manager_.get(), editor_.get(), cmdPage.get(),
                                                   doc_.get(), name_, QRectF(5, 5, 20, 20),
                                                   i.displayWidth, i.displayHeight, 0.0f, 0.0f);
    CropImageCommand* cropRaw = crop.get();
    undo.execute(std::move(crop));
    EXPECT_FALSE(undo.canUndo()) << "a refused crop left an undo entry";
    (void)cropRaw;

    undo.execute(std::make_unique<ResizeImageCommand>(manager_.get(), editor_.get(), cmdPage.get(),
                                                      doc_.get(), name_, 50.0f, 50.0f, 10.0f, 10.0f,
                                                      0.0f, /*occurrence=*/-1));
    undo.execute(std::make_unique<RotateImageCommand>(manager_.get(), editor_.get(), cmdPage.get(),
                                                      doc_.get(), "Nope", 30.0f, 100.0f, 100.0f,
                                                      50.0f, 50.0f));
    undo.execute(std::make_unique<RemoveImageCommand>(manager_.get(), cmdPage.get(), doc_.get(), 7));
    EXPECT_FALSE(undo.canUndo()) << "a refused edit left an undo entry";
    EXPECT_EQ(dirtied, 0) << "a refused edit marked the page dirty";
    EXPECT_EQ(doc_->contentVersion(), version) << "a refused edit bumped the content version";
    EXPECT_EQ(liveContent("r1.pdf"), before);

    // Control: a real edit does get an entry and does dirty the page.
    undo.execute(std::make_unique<ResizeImageCommand>(manager_.get(), editor_.get(), cmdPage.get(),
                                                      doc_.get(), name_, i.displayWidth,
                                                      i.displayHeight, float(kRect.x() + 10.0),
                                                      float(kRect.y())));
    EXPECT_TRUE(undo.canUndo());
    EXPECT_GT(dirtied, 0);
    undo.clear();
    doc_->removePageDirtyListener(token);
}

// ---------------------------------------------------------------------------
// claim-check phase-2: occurrence > 0 through the commands and MainWindow.
// ---------------------------------------------------------------------------
class ImagePlacementStateCommandOccurrenceTest : public ImagePlacementModelBase {
protected:
    const QRectF second_ = kRect.translated(0.0, 250.0);
    void buildTwoCopies() {
        build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
            return "q " + g.place(kRect) + " cm /" + n + " Do Q\nq " + g.place(second_) + " cm /" +
                   n + " Do Q\n";
        });
        const auto imgs = images();
        ASSERT_EQ(imgs.size(), 2u);
        ASSERT_EQ(imgs[0].occurrence, 0);
        ASSERT_EQ(imgs[1].occurrence, 1);
        ASSERT_NEAR(imgs[1].boundingBox.y(), second_.y(), 0.01);
    }
    // The first placement's bytes: everything before the second "\nq ".
    static std::string firstPart(const std::string& content) {
        return content.substr(0, content.find("\nq ") + 1);
    }
    // Run `cmd` through an UndoManager targeting copy 2 and check copy 1 is
    // untouched, `changed` holds for copy 2, undo restores the exact bytes
    // and redo reapplies the exact bytes.
    void runOnSecondCopy(std::unique_ptr<pdfe::commands::Command> cmd,
                         const std::function<bool(const std::vector<PageImageInfo>&)>& changed,
                         const char* what) {
        const auto before = images();
        const std::string bytesBefore = liveContent("c0.pdf");
        pdfe::commands::UndoManager undo;
        undo.execute(std::move(cmd));
        ASSERT_TRUE(undo.canUndo()) << what << ": the command failed";
        const auto after = images();
        const std::string bytesAfter = liveContent("c1.pdf");
        EXPECT_TRUE(changed(after)) << what << ": copy 2 (occurrence 1) did not change";
        const std::string copy1 = firstPart(bytesBefore);
        ASSERT_FALSE(copy1.empty());
        EXPECT_EQ(bytesAfter.substr(0, copy1.size()), copy1)
            << what << ": copy 1's bytes changed";
        ASSERT_GE(after.size(), 1u);
        for (int k = 0; k < 6; ++k)
            EXPECT_NEAR(after[0].ctm[k], before[0].ctm[k], 1e-4f) << what << ": copy 1 moved";
        EXPECT_FALSE(after[0].cropped) << what << ": copy 1 was cropped";
        undo.undo();
        EXPECT_EQ(liveContent("c2.pdf"), bytesBefore) << what << ": undo did not restore";
        undo.redo();
        EXPECT_EQ(liveContent("c3.pdf"), bytesAfter) << what << ": redo did not reapply";
        undo.clear();
    }
};

TEST_F(ImagePlacementStateCommandOccurrenceTest, ResizeCommandEditsTheSecondCopyOnly) {
    buildTwoCopies();
    const PageImageInfo c2 = images()[1];
    auto cmdPage = doc_->page(0);
    runOnSecondCopy(std::make_unique<ResizeImageCommand>(
                        manager_.get(), editor_.get(), cmdPage.get(), doc_.get(), name_,
                        c2.displayWidth, c2.displayHeight, float(second_.x() + 80.0),
                        float(second_.y()), 0.0f, /*occurrence=*/1),
                    [&](const std::vector<PageImageInfo>& v) {
                        return v.size() == 2 && std::abs(v[1].boundingBox.x() - (second_.x() + 80.0)) < 0.01;
                    },
                    "resize");
}

TEST_F(ImagePlacementStateCommandOccurrenceTest, RotateCommandEditsTheSecondCopyOnly) {
    buildTwoCopies();
    const PageImageInfo c2 = images()[1];
    auto cmdPage = doc_->page(0);
    runOnSecondCopy(std::make_unique<RotateImageCommand>(
                        manager_.get(), editor_.get(), cmdPage.get(), doc_.get(), name_, 30.0f,
                        float(second_.center().x()), float(second_.center().y()),
                        c2.displayWidth, c2.displayHeight, /*occurrence=*/1),
                    [&](const std::vector<PageImageInfo>& v) {
                        return v.size() == 2 && std::abs(v[1].rotationRadians) > 0.5f &&
                               std::abs(v[0].rotationRadians) < 1e-4f;
                    },
                    "rotate");
}

TEST_F(ImagePlacementStateCommandOccurrenceTest, CropCommandEditsTheSecondCopyOnly) {
    buildTwoCopies();
    const PageImageInfo c2 = images()[1];
    auto cmdPage = doc_->page(0);
    const QRectF b = c2.boundingBox;
    runOnSecondCopy(std::make_unique<CropImageCommand>(
                        manager_.get(), editor_.get(), cmdPage.get(), doc_.get(), name_,
                        QRectF(b.x() + 20, b.y() + 10, b.width() * 0.5, b.height() * 0.5),
                        c2.displayWidth, c2.displayHeight, float(b.x()), float(b.y()),
                        /*occurrence=*/1),
                    [&](const std::vector<PageImageInfo>& v) {
                        return v.size() == 2 && v[1].cropped && !v[0].cropped;
                    },
                    "crop");
}

TEST_F(ImagePlacementStateCommandOccurrenceTest, RemoveCommandRemovesTheSecondCopyOnly) {
    buildTwoCopies();
    auto cmdPage = doc_->page(0);
    runOnSecondCopy(std::make_unique<RemoveImageCommand>(manager_.get(), cmdPage.get(), doc_.get(),
                                                         /*imageIndex=*/1),
                    [&](const std::vector<PageImageInfo>& v) {
                        return v.size() == 1 && std::abs(v[0].boundingBox.y() - kRect.y()) < 0.01;
                    },
                    "remove");
    EXPECT_TRUE(isRed(kRect.center())) << "removing copy 2 lost copy 1's image resource";
}

// The image tool's form-drag guard: a placement inside a Form XObject is
// selectable, and dragging it commits nothing.
TEST_F(ImagePlacementStateCommandOccurrenceTest, DraggingAFormPlacementCommitsNothing) {
    buildCustom([&](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                    const std::string& n) {
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        pdf_obj* img = pdf_dict_gets(ctx, xo, n.c_str());
        pdf_obj* form = makeForm(ctx, pdf, g, n, img, kRect, /*ownResources=*/true);
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        pdf_obj* stm = addStream(ctx, pdf, std::string("/Fm0 Do\n"));
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
    });
    ImageTool tool(manager_.get());
    tool.setPage(page_.get(), doc_.get());
    ASSERT_TRUE(tool.selectImage(name_));
    ASSERT_TRUE(tool.selectedImage().inFormXObject);
    int emitted = 0;
    QObject::connect(&tool, &ImageTool::resizeRequested,
                     [&](const std::string&, float, float, float, float, float) { ++emitted; });
    const QPointF c0 = tool.selectedImage().boundingBox.center();
    auto mouse = [&](QEvent::Type t, QPointF pt) {
        QMouseEvent e(t, pt, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool.mousePressEvent(&e, pt, 1.0f);
        else if (t == QEvent::MouseMove) tool.mouseMoveEvent(&e, pt, 1.0f);
        else tool.mouseReleaseEvent(&e, pt, 1.0f);
    };
    mouse(QEvent::MouseButtonPress, c0);
    mouse(QEvent::MouseMove, c0 + QPointF(50.0, 30.0));
    mouse(QEvent::MouseButtonRelease, c0 + QPointF(50.0, 30.0));
    EXPECT_EQ(emitted, 0) << "a drag of a form placement emitted a resize";
    EXPECT_NEAR(tool.selectedImage().boundingBox.center().x(), c0.x(), 0.01)
        << "the overlay was left at the dragged position";
    EXPECT_TRUE(isRed(kRect.center()));
}

// MainWindow passes the SELECTED placement's occurrence to the command: with
// copy 2 selected, a resizeRequested through the window moves copy 2 only.
TEST_F(ImagePlacementStateCommandOccurrenceTest, MainWindowEditsTheSelectedSecondCopy) {
    buildTwoCopies();
    const PageImageInfo c2 = images()[1];
    // Hand the file to the window; release this fixture's handle first.
    page_.reset();
    doc_.reset();
    const std::string file = path("built.pdf");

    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(file));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    Document* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    auto* toolAction = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
    ASSERT_NE(toolAction, nullptr);
    ASSERT_TRUE(toolAction->isEnabled());
    toolAction->trigger();
    QCoreApplication::processEvents();
    auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
    ASSERT_NE(canvas, nullptr);
    ImageTool* tool = canvas->imageTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_TRUE(tool->selectPlacement(name_, 1)) << "copy 2 is not selectable by identity";
    ASSERT_EQ(tool->selectedImage().occurrence, 1);

    ImageManager mgr(doc->context());
    auto onPage = [&]() {
        auto page = doc->page(0);
        return mgr.enumerateImages(page.get(), doc);
    };
    const auto before = onPage();
    ASSERT_EQ(before.size(), 2u);
    emit tool->resizeRequested(name_, c2.displayWidth, c2.displayHeight,
                               float(second_.x() + 90.0), float(second_.y()), 0.0f);
    QCoreApplication::processEvents();
    const auto after = onPage();
    ASSERT_EQ(after.size(), 2u);
    EXPECT_NEAR(after[1].boundingBox.x(), second_.x() + 90.0, 0.01)
        << "the selected copy 2 did not move";
    EXPECT_NEAR(after[0].boundingBox.x(), kRect.x(), 0.01)
        << "MainWindow edited copy 1 although copy 2 was selected";
    for (int k = 0; k < 6; ++k) EXPECT_NEAR(after[0].ctm[k], before[0].ctm[k], 1e-4f);
}
