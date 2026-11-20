// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=18 no_throw=0  (2026-11-17)
//
// RED probes for the image editor beyond the placement-state set
// (tests/test_image_placement_state.cpp), pipeline image-placement-state,
// phase 1 (audit; nothing under src/ changes). Each probe asserts the CORRECT
// behaviour, so on today's code it fails for the reason named below. Setup
// preconditions are ASSERTs, so a red probe is never a setup failure.
//
// Placement facts are read back from the page (ImageManager::enumerateImages,
// Page::render pixels, raw MuPDF on a saved copy), never from a tool's cache.
// Page::render keeps alpha: unpainted = transparent.
//
//   E1  flips and skew: a placement with CTM [w 0 0 -h] (vertical flip),
//       [-w 0 0 h] (mirror) or a shear keeps its pixel orientation / its
//       a,b,c,d after a move through the production resizeImage arguments.
//       (E1_VerticalFlipSurvivesMove, E1_MirrorSurvivesMove,
//        E1_SkewSurvivesMove)
//   E2  an ExtGState opacity wrapper (`q /GS1 gs ... cm /Im Do Q`, CA/ca 0.5)
//       survives a move: the rendered pixel stays translucent.
//       (E2_OpacityWrapperSurvivesMove)
//   E3  stacking order: an image drawn BEFORE an opaque rectangle stays
//       beneath it after an edit (pixel under the rectangle keeps its colour).
//       (E3_ImageStaysBeneathLaterContentAfterEdit)
//   E4  marked content: `/Figure <</MCID 0>> BDC ... EMC` and `/OC /oc1 BDC ...
//       EMC` still enclose the `Do` in the saved content stream after an edit.
//       (E4_McidMarkedContentSurvivesEdit, E4_OptionalContentSurvivesEdit)
//   E5  MainWindow's crop handler re-syncs the image tool: after Image > Crop
//       the tool's selection equals the page's placement.
//       (ImageEditorStateWindowTest.E5_CropResyncsTheImageTool)
//   E6  Rotate 180 / Rotate 270 are reachable: actions imageRotate180Action and
//       imageRotate270Action exist and rotate relatively.
//       (ImageEditorStateWindowTest.E6_*)
//   E7  dragging the right-edge handle of an image rotated 90 degrees outward
//       (in its own frame) grows its own displayWidth.
//       (E7_RightHandleDragOfRotatedImageGrowsItsOwnWidth)
//   E8  InsertImageCommand::undo removes the image from the page AND from the
//       page's /XObject resources.
//       (E8_InsertUndoRemovesImageAndItsResource)
//   E9  insertImage honours jpegQuality; a JPEG with EXIF Orientation=6 is
//       inserted upright (rotated), not sideways.
//       (E9_JpegQualityChangesTheEncodedImage, E9_ExifOrientationIsApplied)
//       stripExif: NOT probed red, see findings-editor-extras.md.
//   E10 compressionString: JPX -> "JPEG 2000", JBIG2 -> "JBIG2", not "Lossless".
//       (ImageEditorStateDataTest.E10_JpxAndJbig2AreNotReportedAsLossless)
//   E11 NOT a defect: a trial probe (edit + undo of a gs-wrapped image) was
//       green - the ExtGState resource and the translucent render both come
//       back - so no probe is kept. See findings-editor-extras.md.
//   E12 on a page with /Rotate 90 the enumerated box (rotated by
//       -rotationRadians about its centre, the documented convention) is where
//       the image is rendered.
//       (E12_EnumeratedBoxMatchesRenderOnRotatedPage)
//   E13 an image drawn inside a Form XObject (`/Fm0 Do`) is enumerated.
//       (E13_ImageInsideFormXObjectIsEnumerated)
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageCommands.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"
#include "support/ImageWindowFixture.h"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImageReader>
#include <QImage>
#include <QMouseEvent>
#include <QRectF>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
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

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

float angularDistance(float a, float b) {
    float d = std::fmod(std::abs(a - b), 2.0f * kPi);
    return d > kPi ? 2.0f * kPi - d : d;
}

float normDeg(float d) {
    d = std::fmod(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}

QPointF rotateAbout(QPointF p, QPointF c, float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

// The image rect used throughout: upright, 200x120 at (150, 200), fitz Y-down.
const QRectF kRect(150.0, 200.0, 200.0, 120.0);

// One character per colour family; '.' = transparent, '?' = anything else.
char classify(QRgb c) {
    const int a = qAlpha(c), r = qRed(c), g = qGreen(c), b = qBlue(c);
    if (a < 30) return '.';
    if (a < 200) return '?';
    if (r > 200 && g < 90 && b < 90) return 'R';
    if (g > 200 && r < 90 && b < 90) return 'G';
    if (b > 200 && r < 90 && g < 90) return 'B';
    if (r > 200 && g > 200 && b < 90) return 'Y';
    if (r > 230 && g > 230 && b > 230) return 'W';
    return '?';
}

// Geometry of the raw page, for writing user-space operators.
struct Geom {
    float x0 = 0, y1 = 0;
    // "w 0 0 h e f" for a fitz Y-down rect.
    std::string place(const QRectF& r) const {
        std::ostringstream o;
        o << r.width() << " 0 0 " << r.height() << " " << (x0 + r.x()) << " "
          << (y1 - r.y() - r.height());
        return o.str();
    }
    // fitz rect -> "x y w h" user-space operands of `re`.
    std::string re(const QRectF& r) const {
        std::ostringstream o;
        o << (x0 + r.x()) << " " << (y1 - r.y() - r.height()) << " " << r.width() << " " << r.height();
        return o.str();
    }
};

// ---- raw content-stream inspection ----------------------------------------

// Operand text of every marked-content opener still open at `/name Do`.
std::vector<std::string> openMarkedContentAtDo(const std::string& content, const std::string& name) {
    std::vector<std::string> tokens;
    {
        std::istringstream in(content);
        std::string t;
        while (in >> t) tokens.push_back(t);
    }
    auto isOperator = [](const std::string& t) {
        if (t.empty()) return false;
        const char c = t[0];
        if (c == '/' || c == '<' || c == '[' || c == '(' || c == '-' || c == '.' || c == '%' ||
            (c >= '0' && c <= '9')) return false;
        return t != "true" && t != "false" && t != "null";
    };
    std::vector<std::string> stack;
    std::string operands;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "Do" && i > 0 && tokens[i - 1] == "/" + name) return stack;
        if (!isOperator(t)) {
            operands += (operands.empty() ? "" : " ") + t;
            continue;
        }
        if (t == "BDC" || t == "BMC") stack.push_back(operands);
        else if (t == "EMC" && !stack.empty()) stack.pop_back();
        operands.clear();
    }
    return {"<Do not found in content>"};
}

} // namespace

class ImageEditorStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
    }
    void TearDown() override {
        tool_.reset();
        page_.reset();
        doc_.reset();
    }

    std::string path(const char* leaf) { return tmp_.filePath(leaf).toStdString(); }

    // 64x64 quadrants: TL red, TR green, BL blue, BR yellow.
    std::string quadPng() {
        QImage img(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                img.setPixel(x, y, y < 32 ? (x < 32 ? qRgb(255, 0, 0) : qRgb(0, 255, 0))
                                          : (x < 32 ? qRgb(0, 0, 255) : qRgb(255, 255, 0)));
        const std::string p = path("quad.png");
        EXPECT_TRUE(img.save(QString::fromStdString(p)));
        return p;
    }

    // Insert `png` into a fresh copy of test_single.pdf at kRect and save it.
    // Leaves nothing open; returns the XObject name.
    std::string insertInto(const std::string& png, const std::string& out) {
        auto doc = Document::open(context_, base_);
        EXPECT_NE(doc, nullptr);
        if (!doc) return {};
        auto page = doc->page(0);
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page.get(), doc.get(), o);
        EXPECT_TRUE(r.success) << r.errorMessage;
        page.reset();
        doc->saveAs(out);
        return r.imageInfo.xobjectName;
    }

    using Rewrite = std::function<std::string(fz_context*, pdf_document*, pdf_page*,
                                              const Geom&, const std::string& imgName)>;

    // Raw MuPDF: replace page 0's whole content with the text `rewrite`
    // returns (it may also edit resources / the page dict), save to `dst`.
    void rawRewrite(const std::string& src, const std::string& dst, const std::string& name,
                    const Rewrite& rewrite) {
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, src.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
            Geom g;
            g.x0 = mb.x0;
            g.y1 = mb.y1;
            const std::string text = rewrite(ctx, pdf, pp, g, name);
            fz_buffer* buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
            pdf_obj* stm = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
            fz_drop_buffer(ctx, buf);
            pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
            pdf_drop_obj(ctx, stm);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, dst.c_str(), &wo);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
    }

    // Build a one-image page whose content is `rewrite`'s text and open it.
    bool build(const Rewrite& rewrite, const std::string& png = std::string()) {
        const std::string a = path("a.pdf");
        name_ = insertInto(png.empty() ? quadPng() : png, a);
        if (name_.empty()) return false;
        built_ = path("built.pdf");
        rawRewrite(a, built_, name_, rewrite);
        doc_ = Document::open(context_, built_);
        if (!doc_) return false;
        reload();
        return page_ != nullptr;
    }

    void reload() { page_ = doc_->page(0); }
    std::vector<PageImageInfo> images() {
        reload();
        return manager_->enumerateImages(page_.get(), doc_.get());
    }
    PageImageInfo only() {
        auto v = images();
        EXPECT_EQ(v.size(), 1u);
        return v.empty() ? PageImageInfo{} : v[0];
    }
    QImage render() {
        reload();
        return page_->render(QRectF(), 1.0f);
    }
    static QRgb pixelAt(const QImage& img, QPointF p) {
        const int x = static_cast<int>(std::lround(p.x()));
        const int y = static_cast<int>(std::lround(p.y()));
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return qRgb(0, 0, 255);
        return img.pixel(x, y);
    }
    // Colour families at the four quadrant centres of `r`: TL TR BL BR.
    std::string quads(const QRectF& r) {
        const QImage img = render();
        std::string s;
        for (auto p : {QPointF(r.x() + r.width() * 0.25, r.y() + r.height() * 0.25),
                       QPointF(r.x() + r.width() * 0.75, r.y() + r.height() * 0.25),
                       QPointF(r.x() + r.width() * 0.25, r.y() + r.height() * 0.75),
                       QPointF(r.x() + r.width() * 0.75, r.y() + r.height() * 0.75)})
            s += classify(pixelAt(img, p));
        return s;
    }
    int alphaAt(QPointF p) { return qAlpha(pixelAt(render(), p)); }

    // The production move: the enumerated placement's own values, shifted.
    bool moveByProduction(const PageImageInfo& i, double dx, double dy) {
        reload();
        return editor_->resizeImage(page_.get(), doc_.get(), i.xobjectName,
                                    i.displayWidth, i.displayHeight,
                                    float(i.boundingBox.x() + dx), float(i.boundingBox.y() + dy),
                                    visualClockwiseDegrees(i));
    }

    // Save the open document to a copy (pages released first) and return it.
    std::string saveCopy(const char* leaf) {
        page_.reset();
        const std::string out = path(leaf);
        doc_->saveAs(out);
        return out;
    }

    // ---- raw reads of a saved file ----
    std::string contentBytes(const std::string& file) {
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
    std::vector<std::string> xobjectNames(const std::string& file) {
        std::vector<std::string> names;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
            for (int i = 0, n = xo ? pdf_dict_len(ctx, xo) : 0; i < n; ++i)
                names.push_back(pdf_to_name(ctx, pdf_dict_get_key(ctx, xo, i)));
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return names;
    }
    bool hasExtGState(const std::string& file, const char* gs) {
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
    // Raw (encoded) byte length of the first XObject of page 0.
    size_t firstXObjectRawLength(const std::string& file) {
        size_t len = 0;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, file.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
            if (xo && pdf_dict_len(ctx, xo) > 0) {
                pdf_obj* o = pdf_dict_get_val(ctx, xo, 0);
                if (pdf_is_stream(ctx, o)) {
                    fz_buffer* b = pdf_load_raw_stream(ctx, o);
                    unsigned char* d = nullptr;
                    len = fz_buffer_storage(ctx, b, &d);
                    fz_drop_buffer(ctx, b);
                }
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        return len;
    }

    // ---- resource builders for rewrite callbacks ----
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

    QTemporaryDir tmp_;
    std::string base_, built_, name_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
};

// ---------------------------------------------------------------------------
// E1: flips and skew survive a move.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E1_VerticalFlipSurvivesMove) {
    ASSERT_TRUE(build([](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        // [w 0 0 -h e f+h]: the image's top row lands at the bottom.
        std::ostringstream o;
        o << "q " << kRect.width() << " 0 0 " << -kRect.height() << " " << (g.x0 + kRect.x()) << " "
          << (g.y1 - kRect.y()) << " cm /" << n << " Do Q\n";
        return o.str();
    }));
    const PageImageInfo before = only();
    ASSERT_LT(before.ctm[3], 0.0f) << "oracle: the placement is vertically flipped";
    ASSERT_EQ(quads(kRect), "BYRG") << "oracle: a vertical flip renders top row at the bottom";
    ASSERT_TRUE(moveByProduction(before, 40.0, 30.0));
    EXPECT_EQ(quads(kRect.translated(40.0, 30.0)), "BYRG")
        << "a move changed the orientation of a vertically flipped image (CTM d sign lost)";
}

TEST_F(ImageEditorStateTest, E1_MirrorSurvivesMove) {
    ASSERT_TRUE(build([](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        // [-w 0 0 h e+w f]: the image's left column lands at the right.
        std::ostringstream o;
        o << "q " << -kRect.width() << " 0 0 " << kRect.height() << " "
          << (g.x0 + kRect.x() + kRect.width()) << " " << (g.y1 - kRect.y() - kRect.height())
          << " cm /" << n << " Do Q\n";
        return o.str();
    }));
    const PageImageInfo before = only();
    ASSERT_LT(before.ctm[0], 0.0f) << "oracle: the placement is mirrored";
    ASSERT_EQ(quads(kRect), "GRYB") << "oracle: a mirror renders left column at the right";
    ASSERT_TRUE(moveByProduction(before, 40.0, 30.0));
    EXPECT_EQ(quads(kRect.translated(40.0, 30.0)), "GRYB")
        << "a move changed the orientation of a mirrored image (CTM a sign lost)";
}

TEST_F(ImageEditorStateTest, E1_SkewSurvivesMove) {
    ASSERT_TRUE(build([](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        std::ostringstream o;  // [200 0 40 120 e f]: shear along x
        o << "q " << kRect.width() << " 0 40 " << kRect.height() << " " << (g.x0 + kRect.x()) << " "
          << (g.y1 - kRect.y() - kRect.height()) << " cm /" << n << " Do Q\n";
        return o.str();
    }));
    const PageImageInfo before = only();
    ASSERT_NEAR(before.ctm[2], 40.0f, 1e-3f) << "oracle: the placement is sheared";
    ASSERT_TRUE(moveByProduction(before, 40.0, 30.0));
    const PageImageInfo after = only();
    EXPECT_NEAR(after.ctm[2], before.ctm[2], 0.05f) << "a move dropped the skew (CTM c) from the image";
    EXPECT_NEAR(after.ctm[3], before.ctm[3], 0.05f) << "a move changed the CTM d of a sheared image";
    EXPECT_NEAR(after.ctm[0], before.ctm[0], 0.05f);
    EXPECT_NEAR(after.ctm[1], before.ctm[1], 0.05f);
}

// ---------------------------------------------------------------------------
// E2: an opacity wrapper survives a move.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E2_OpacityWrapperSurvivesMove) {
    ASSERT_TRUE(build([](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                         const std::string& n) {
        addGraphicsState(ctx, pdf, pp, "GS1", 0.5f);
        return "q /GS1 gs " + g.place(kRect) + " cm /" + n + " Do Q\n";
    }));
    const int before = alphaAt(kRect.center());
    ASSERT_GT(before, 90) << "oracle: image is drawn";
    ASSERT_LT(before, 170) << "oracle: image is translucent (ca 0.5)";
    ASSERT_TRUE(moveByProduction(only(), 40.0, 30.0));
    const int after = alphaAt(kRect.center() + QPointF(40.0, 30.0));
    EXPECT_GT(after, 90) << "oracle: the moved image is drawn";
    EXPECT_LT(after, 170) << "a move made the image fully opaque (alpha " << after
                          << "): the /GS1 gs opacity wrapper was lost";
}

// ---------------------------------------------------------------------------
// E3: stacking order.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E3_ImageStaysBeneathLaterContentAfterEdit) {
    const QRectF cover(kRect.x() + 60, kRect.y() + 30, 80, 60);
    ASSERT_TRUE(build([&](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "q " + g.place(kRect) + " cm /" + n + " Do Q\n0 1 0 rg " + g.re(cover) + " re f\n";
    }));
    ASSERT_EQ(classify(pixelAt(render(), cover.center())), 'G')
        << "oracle: the later opaque rectangle covers the image";
    ASSERT_EQ(quads(kRect), "RGBY") << "oracle: the image shows outside the rectangle";
    const PageImageInfo before = only();
    ASSERT_TRUE(moveByProduction(before, 0.0, 0.0));
    EXPECT_EQ(quads(kRect), "RGBY") << "the image is no longer drawn where it was";
    EXPECT_EQ(classify(pixelAt(render(), cover.center())), 'G')
        << "after an edit the image is drawn ON TOP of the rectangle it was beneath: "
           "the editor appended the placement last";
}

// ---------------------------------------------------------------------------
// E4: marked content.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E4_McidMarkedContentSurvivesEdit) {
    ASSERT_TRUE(build([](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "/Figure <</MCID 0>> BDC q " + g.place(kRect) + " cm /" + n + " Do Q EMC\n";
    }));
    auto marks = openMarkedContentAtDo(contentBytes(built_), name_);
    ASSERT_EQ(marks.size(), 1u) << "oracle: the Do starts inside one BDC";
    ASSERT_NE(marks[0].find("MCID"), std::string::npos);
    ASSERT_TRUE(moveByProduction(only(), 12.0, 7.0));
    const std::string saved = saveCopy("e4a.pdf");
    marks = openMarkedContentAtDo(contentBytes(saved), name_);
    bool inFigure = false;
    for (const auto& m : marks) inFigure = inFigure || m.find("MCID") != std::string::npos;
    EXPECT_TRUE(inFigure) << "after an edit the Do is no longer inside /Figure <</MCID 0>> BDC ... EMC; "
                             "open markers at the Do: " << marks.size();
}

TEST_F(ImageEditorStateTest, E4_OptionalContentSurvivesEdit) {
    ASSERT_TRUE(build([](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                         const std::string& n) {
        pdf_obj* res = pdf_page_resources(ctx, pp);
        pdf_obj* props = pdf_new_dict(ctx, pdf, 1);
        pdf_obj* ocg = pdf_add_new_dict(ctx, pdf, 2);
        pdf_dict_put(ctx, ocg, PDF_NAME(Type), PDF_NAME(OCG));
        pdf_dict_put_text_string(ctx, ocg, PDF_NAME(Name), "Layer");
        pdf_dict_puts(ctx, props, "oc1", ocg);
        pdf_drop_obj(ctx, ocg);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Properties), props);
        return "/OC /oc1 BDC q " + g.place(kRect) + " cm /" + n + " Do Q EMC\n";
    }));
    auto marks = openMarkedContentAtDo(contentBytes(built_), name_);
    ASSERT_EQ(marks.size(), 1u) << "oracle: the Do starts inside one BDC";
    ASSERT_NE(marks[0].find("/OC"), std::string::npos);
    ASSERT_TRUE(moveByProduction(only(), 12.0, 7.0));
    const std::string saved = saveCopy("e4b.pdf");
    marks = openMarkedContentAtDo(contentBytes(saved), name_);
    bool inOc = false;
    for (const auto& m : marks)
        inOc = inOc || (m.find("/OC") != std::string::npos && m.find("oc1") != std::string::npos);
    EXPECT_TRUE(inOc) << "after an edit the Do is no longer inside /OC /oc1 BDC ... EMC; "
                         "open markers at the Do: " << marks.size();
}

// ---------------------------------------------------------------------------
// E7: handle resize of a rotated image grows the image's OWN width.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E7_RightHandleDragOfRotatedImageGrowsItsOwnWidth) {
    ASSERT_TRUE(build([](fz_context*, pdf_document*, pdf_page*, const Geom& g, const std::string& n) {
        return "q " + g.place(kRect) + " cm /" + n + " Do Q\n";
    }));
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 90.0f, float(c.x()), float(c.y()),
                                     float(kRect.width()), float(kRect.height())));
    const PageImageInfo rotated = only();
    ASSERT_NEAR(std::abs(rotated.rotationRadians), kPi / 2.0f, 1e-3f) << "oracle: rotated a quarter turn";
    ASSERT_NEAR(rotated.displayWidth, kRect.width(), 0.05f);

    tool_ = std::make_unique<ImageTool>(manager_.get());
    tool_->setPage(page_.get(), doc_.get());
    ASSERT_TRUE(tool_->selectImage(name_));
    int fires = 0;
    float w = 0, h = 0, x = 0, y = 0, angle = 0;
    QObject::connect(tool_.get(), &ImageTool::resizeRequested,
        [&](const std::string&, float nw, float nh, float nx, float ny, float na) {
            w = nw; h = nh; x = nx; y = ny; angle = na; ++fires;
        });

    // The right-edge handle sits at local (right, middle); drag it 40 points
    // OUTWARD along the image's own +x axis. In page space that is rotated
    // with the image (local -> page is a rotation of MINUS rotationRadians).
    const QRectF bb = rotated.boundingBox;
    const QPointF handleLocal(bb.right(), bb.center().y());
    const QPointF from = rotateAbout(handleLocal, bb.center(), -rotated.rotationRadians);
    const QPointF to = rotateAbout(handleLocal + QPointF(40.0, 0.0), bb.center(), -rotated.rotationRadians);
    auto mouse = [&](QEvent::Type t, QPointF p) {
        QMouseEvent e(t, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool_->mousePressEvent(&e, p, 1.0f);
        else if (t == QEvent::MouseMove) tool_->mouseMoveEvent(&e, p, 1.0f);
        else tool_->mouseReleaseEvent(&e, p, 1.0f);
    };
    mouse(QEvent::MouseButtonPress, from);
    mouse(QEvent::MouseMove, to);
    mouse(QEvent::MouseButtonRelease, to);
    ASSERT_EQ(fires, 1) << "oracle: the drag grabbed the handle and committed one resize";

    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_, w, h, x, y, angle));
    const PageImageInfo after = only();
    EXPECT_NEAR(after.displayWidth, rotated.displayWidth + 40.0f, 1.0f)
        << "dragging the right handle outward along the image's own x axis did not grow its "
           "width (200 -> " << after.displayWidth << "): the page-space drag delta was applied "
           "in the un-rotated frame";
    EXPECT_NEAR(after.displayHeight, rotated.displayHeight, 1.0f)
        << "the height changed on a pure right-handle drag";
}

// ---------------------------------------------------------------------------
// E8: undo of an insert removes the image and its resource.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E8_InsertUndoRemovesImageAndItsResource) {
    const std::string png = path("red.png");
    QImage red(64, 64, QImage::Format_RGB32);
    red.fill(QColor(255, 0, 0));
    ASSERT_TRUE(red.save(QString::fromStdString(png)));
    ASSERT_TRUE(xobjectNames(base_).empty()) << "oracle: test_single.pdf has no XObjects";
    doc_ = Document::open(context_, base_);
    ASSERT_NE(doc_, nullptr);
    page_ = doc_->page(0);
    InsertImageOptions o;
    o.filePath = png;
    o.targetRect = kRect;
    o.maintainAspectRatio = false;
    {
        // The command keeps a raw Page*; enumeration below reloads page_, so
        // give the command its own page (MainWindow keeps one in commandPages_).
        auto cmdPage = doc_->page(0);
        InsertImageCommand cmd(manager_.get(), cmdPage.get(), doc_.get(), o);
        cmd.execute();
        ASSERT_TRUE(cmd.result().success) << cmd.result().errorMessage;
        ASSERT_EQ(images().size(), 1u) << "oracle: the insert placed one image";
        cmd.undo();
    }
    EXPECT_EQ(images().size(), 0u) << "undo left the image on the page";
    const std::string saved = saveCopy("e8.pdf");
    EXPECT_TRUE(xobjectNames(saved).empty())
        << "undo left the inserted image in the page's /XObject resources (removeImage was "
           "called with index " << -1 << ")";
}

// ---------------------------------------------------------------------------
// E9: jpegQuality and EXIF orientation.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E9_JpegQualityChangesTheEncodedImage) {
    QImage noise(128, 128, QImage::Format_RGB32);
    quint32 s = 12345;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            s = s * 1664525u + 1013904223u;
            noise.setPixel(x, y, qRgb((s >> 8) & 255, (s >> 16) & 255, (s >> 24) & 255));
        }
    const std::string jpg = path("noise.jpg");
    ASSERT_TRUE(noise.save(QString::fromStdString(jpg), "JPEG", 95));

    size_t len[2] = {0, 0};
    const int qualities[2] = {10, 95};
    for (int i = 0; i < 2; ++i) {
        auto doc = Document::open(context_, base_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        InsertImageOptions o;
        o.filePath = jpg;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        o.jpegQuality = qualities[i];
        auto r = manager_->insertImage(page.get(), doc.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        page.reset();
        const std::string out = path(i == 0 ? "q10.pdf" : "q95.pdf");
        doc->saveAs(out);
        len[i] = firstXObjectRawLength(out);
        ASSERT_GT(len[i], 0u) << "oracle: the image stream was written";
    }
    EXPECT_LT(len[0], len[1])
        << "jpegQuality 10 and 95 produced streams of " << len[0] << " and " << len[1]
        << " bytes: the option is accepted and ignored";
}

TEST_F(ImageEditorStateTest, E9_ExifOrientationIsApplied) {
    // 64 wide x 32 high: left half red, right half blue.
    QImage wide(64, 32, QImage::Format_RGB32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) wide.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
    QByteArray jpeg;
    {
        QBuffer buf(&jpeg);
        ASSERT_TRUE(buf.open(QIODevice::WriteOnly));
        ASSERT_TRUE(wide.save(&buf, "JPEG", 100));
    }
    ASSERT_GE(jpeg.size(), 4);
    ASSERT_EQ(static_cast<uchar>(jpeg[0]), 0xFF);
    ASSERT_EQ(static_cast<uchar>(jpeg[1]), 0xD8);
    // APP1 "Exif" with one IFD0 entry: Orientation (0x0112) = 6 (rotate 90 CW to view).
    const unsigned char app1[] = {
        0xFF, 0xE1, 0x00, 0x22,
        'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00};
    jpeg.insert(2, reinterpret_cast<const char*>(app1), sizeof(app1));
    const std::string file = path("exif6.jpg");
    {
        QFile f(QString::fromStdString(file));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        ASSERT_EQ(f.write(jpeg), jpeg.size());
    }
    // Oracle: Qt itself reads the orientation tag (so it is a valid APP1).
    {
        QImageReader rd(QString::fromStdString(file));
        ASSERT_EQ(rd.transformation(), QImageIOHandler::TransformationRotate90)
            << "oracle: the hand-written EXIF segment is parsed as Orientation 6";
    }

    auto doc = Document::open(context_, base_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    InsertImageOptions o;
    o.filePath = file;
    o.targetRect = kRect;
    o.maintainAspectRatio = false;
    auto r = manager_->insertImage(page.get(), doc.get(), o);
    ASSERT_TRUE(r.success) << r.errorMessage;
    page.reset();
    doc_ = std::move(doc);
    const PageImageInfo info = only();
    EXPECT_EQ(info.pixelWidth, 32) << "a JPEG with EXIF Orientation 6 was stored sideways: pixel size "
                                   << info.pixelWidth << "x" << info.pixelHeight
                                   << " (a viewer shows it 32x64); the EXIF is stripped, so the "
                                      "orientation is lost";
    EXPECT_EQ(info.pixelHeight, 64);
    const QImage img = render();
    EXPECT_EQ(classify(pixelAt(img, QPointF(kRect.center().x(), kRect.y() + kRect.height() * 0.2))), 'R')
        << "after rotating 90 CW the red (left) half must be on top";
}

// ---------------------------------------------------------------------------
// E12: page /Rotate.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E12_EnumeratedBoxMatchesRenderOnRotatedPage) {
    ASSERT_TRUE(build([](fz_context* ctx, pdf_document*, pdf_page* pp, const Geom& g,
                         const std::string& n) {
        pdf_dict_put_int(ctx, pp->obj, PDF_NAME(Rotate), 90);
        return "q " + g.place(kRect) + " cm /" + n + " Do Q\n";
    }));
    const QImage img = render();
    ASSERT_GT(img.width(), img.height()) << "oracle: /Rotate 90 turned the page to landscape";
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) > 200) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    ASSERT_GE(maxX, 0) << "oracle: the image is painted";
    const QRectF drawn(minX, minY, maxX - minX + 1, maxY - minY + 1);

    const PageImageInfo info = only();
    QRectF extent;
    {
        const QRectF b = info.boundingBox;
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (auto corner : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()}) {
            const QPointF p = rotateAbout(corner, b.center(), -info.rotationRadians);
            x0 = std::min(x0, p.x()); x1 = std::max(x1, p.x());
            y0 = std::min(y0, p.y()); y1 = std::max(y1, p.y());
        }
        extent = QRectF(x0, y0, x1 - x0, y1 - y0);
    }
    EXPECT_NEAR(extent.center().x(), drawn.center().x(), 2.0)
        << "enumerated centre x " << extent.center().x() << " but the image is rendered at "
        << drawn.center().x() << " on a /Rotate 90 page: enumeration ignores the page transform";
    EXPECT_NEAR(extent.center().y(), drawn.center().y(), 2.0)
        << "enumerated centre y " << extent.center().y() << " but rendered at " << drawn.center().y();
    EXPECT_NEAR(extent.width(), drawn.width(), 2.0);
    EXPECT_NEAR(extent.height(), drawn.height(), 2.0);
}

// ---------------------------------------------------------------------------
// E13: an image inside a Form XObject.
// ---------------------------------------------------------------------------
TEST_F(ImageEditorStateTest, E13_ImageInsideFormXObjectIsEnumerated) {
    ASSERT_TRUE(build([](fz_context* ctx, pdf_document* pdf, pdf_page* pp, const Geom& g,
                         const std::string& n) {
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
        pdf_obj* fres = pdf_new_dict(ctx, pdf, 1);
        pdf_obj* fxo = pdf_new_dict(ctx, pdf, 1);
        pdf_dict_puts(ctx, fxo, n.c_str(), img);
        pdf_dict_put_drop(ctx, fres, PDF_NAME(XObject), fxo);
        pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), fres);
        pdf_drop_obj(ctx, img);
        pdf_dict_dels(ctx, xo, n.c_str());           // the page itself no longer names the image
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        return std::string("/Fm0 Do\n");
    }));
    ASSERT_EQ(quads(kRect), "RGBY") << "oracle: the image is rendered through the form";
    const auto found = images();
    ASSERT_GE(found.size(), 1u)
        << "an image drawn inside a Form XObject is not enumerated: the page shows it, the "
           "editor does not know it exists";
    EXPECT_NEAR(found[0].boundingBox.center().x(), kRect.center().x(), 0.5);
    EXPECT_NEAR(found[0].boundingBox.center().y(), kRect.center().y(), 0.5);
}

// ---------------------------------------------------------------------------
// E10: data-level.
// ---------------------------------------------------------------------------
TEST(ImageEditorStateDataTest, E10_JpxAndJbig2AreNotReportedAsLossless) {
    PageImageInfo plain;
    ASSERT_EQ(plain.compressionString(), "Lossless") << "oracle: the default is Lossless";
    PageImageInfo jpeg;
    jpeg.isJpeg = true;
    ASSERT_EQ(jpeg.compressionString(), "JPEG") << "oracle: DCT is JPEG";
    PageImageInfo jpx;
    jpx.isJpx = true;
    EXPECT_EQ(jpx.compressionString(), "JPEG 2000")
        << "a JPXDecode image is reported as '" << jpx.compressionString() << "'";
    PageImageInfo jbig2;
    jbig2.isJbig2 = true;
    EXPECT_EQ(jbig2.compressionString(), "JBIG2")
        << "a JBIG2Decode image is reported as '" << jbig2.compressionString() << "'";
}

// ---------------------------------------------------------------------------
// Window-level probes (E5, E6).
// ---------------------------------------------------------------------------
namespace {

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

using pdfe::testsupport::WindowFixture;

} // namespace

// E5: after Image > Crop the tool's selection is what the page now holds.
TEST(ImageEditorStateWindowTest, E5_CropResyncsTheImageTool) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    emit f.tool->rotateRequested(f.name, 30.0f);
    settle();
    const PageImageInfo rotated = f.onPage();
    ASSERT_GT(std::abs(rotated.rotationRadians), 0.4f) << "oracle: the rotate committed";
    ASSERT_NEAR(f.tool->selectedImage().rotationRadians, rotated.rotationRadians, 1e-4f)
        << "oracle: the tool agrees with the page before the crop";
    auto* crop = f.mw.findChild<QAction*>(QStringLiteral("imageCropAction"));
    ASSERT_NE(crop, nullptr);
    ASSERT_TRUE(crop->isEnabled());
    crop->trigger();
    settle();
    ASSERT_TRUE(f.tool->hasSelection());
    const PageImageInfo page = f.onPage();
    const PageImageInfo tool = f.tool->selectedImage();
    EXPECT_NEAR(tool.rotationRadians, page.rotationRadians, 1e-3f)
        << "after Image > Crop the tool shows rotation " << tool.rotationRadians
        << " but the page holds " << page.rotationRadians << ": the crop handler did not re-read the tool";
    EXPECT_NEAR(tool.displayWidth, page.displayWidth, 0.5f)
        << "tool and page disagree on the width after the crop";
    EXPECT_NEAR(tool.displayHeight, page.displayHeight, 0.5f);
    EXPECT_NEAR(tool.boundingBox.center().x(), page.boundingBox.center().x(), 0.5);
    EXPECT_NEAR(tool.boundingBox.center().y(), page.boundingBox.center().y(), 0.5);
}

// E6: Rotate 180 / 270 actions exist and rotate.
TEST(ImageEditorStateWindowTest, E6_Rotate180ActionRotatesHalfTurn) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    ASSERT_NEAR(f.onPage().rotationRadians, 0.0f, 1e-3f) << "oracle: the inserted image is upright";
    auto* a = f.mw.findChild<QAction*>(QStringLiteral("imageRotate180Action"));
    ASSERT_NE(a, nullptr) << "there is no Rotate 180 action: onImageRotate180 is not connected to anything";
    ASSERT_TRUE(a->isEnabled());
    a->trigger();
    settle();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(f.onPage())), 180.0f, 0.1f);
}

TEST(ImageEditorStateWindowTest, E6_Rotate270ActionRotatesThreeQuarterTurn) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    ASSERT_NEAR(f.onPage().rotationRadians, 0.0f, 1e-3f) << "oracle: the inserted image is upright";
    auto* a = f.mw.findChild<QAction*>(QStringLiteral("imageRotate270Action"));
    ASSERT_NE(a, nullptr) << "there is no Rotate 270 action: onImageRotate270 is not connected to anything";
    ASSERT_TRUE(a->isEnabled());
    a->trigger();
    settle();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(f.onPage())), 270.0f, 0.1f);
}

TEST(ImageEditorStateWindowTest, E6_RotateActionsAreRelative) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    auto* r90 = f.mw.findChild<QAction*>(QStringLiteral("imageRotateAction"));
    ASSERT_NE(r90, nullptr);
    r90->trigger();
    settle();
    ASSERT_NEAR(normDeg(visualClockwiseDegrees(f.onPage())), 90.0f, 0.1f) << "oracle: first quarter turn";
    auto* a = f.mw.findChild<QAction*>(QStringLiteral("imageRotate180Action"));
    ASSERT_NE(a, nullptr) << "there is no Rotate 180 action";
    a->trigger();
    settle();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(f.onPage())), 270.0f, 0.1f)
        << "Rotate 180 after a quarter turn must give 270 (relative), not 180";
}
