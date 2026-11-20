// -*- coding: utf-8 -*-
//
// tests/test_form_textfield_state.cpp
//
// Pipeline form-textfield, phase 1: RED probes for three form text-field bugs.
//
//   GitHub #458 (B1) — a text field placed with the form tool is invisible once
//       it is deselected. FormManager::createFormField writes /FT and /DA but no
//       /MK (/BC, /BG) and no /BS, and MuPDF draws a widget's border and
//       background ONLY from /MK, so the empty field's appearance stream draws
//       nothing. restoreFormField (undo/redo) rebuilds through the same path.
//
//   GitHub #459 (B2) — typed text is invisible while editing. FormTool keeps
//       the typed text in editBuffer_ and writes the field only on commit; its
//       overlay paints a selection rectangle and an approximated caret, never
//       the buffer or the IME preedit (deferral IME-FORM-PREEDIT-PAINT-01).
//
//   GitHub #460 (B3) — font settings are ignored. Nothing writes a widget /DA
//       after creation, FormFieldPropertiesPanel has no font controls, the
//       text-edit toolbar stays visible (and wired to a destroyed tool) under
//       the form tool, and restoreFormField rebuilds /DA hard-coded.
//
// HOW EACH PROBE IS RED, AND WHY THAT IS THE DOCUMENTED REASON
// ------------------------------------------------------------
// Every assertion message starts with the bug it belongs to. No probe carries
// DISABLED_, GTEST_SKIP or an expected-fail marker, and every assertion runs
// unconditionally.
//
// FormManager::setFieldDefaultAppearance does not exist yet. Rather than
// introduce a declaration-only stub under src/ (forbidden in this phase), the
// probes reach it through a C++20 detection idiom (HasSetFieldDefaultAppearance
// below). The build stays green either way; while the method is absent the
// helper reports "not implemented" and the probe FAILS at run time with
// "setFieldDefaultAppearance not implemented (GitHub #460)". Once phase 2 adds
// the method with the signature
//     setFieldDefaultAppearance(const Page*, int widgetIndex,
//                               const std::string& font, <float|double> size,
//                               const QColor& color)
// the same probes exercise it with no edit here.
//
// The /DA survival probes (undo, redo, delete+undo) do NOT depend on that
// method: they author the /DA directly in the widget dictionary, exactly as a
// field loaded from a file carries one, so they are red today for the restore
// path's hard-coded "/Helv 12 Tf 0 g" — the actual third cause of #460 — and
// not merely for a missing API.
//
// fz_try discipline: every MuPdfContext::execute lambda below declares only
// trivially-destructible locals, writes results into POD out-structs through a
// raw pointer, and never throws (tools/gate/fz_try_raii_scan.py).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "ui/FormFieldPropertiesPanel.h"
#include "ui/MainWindow.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QToolBar>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using pdfe::commands::UndoManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::forms::CreateFormFieldCommand;
using pdfe::forms::DeleteFormFieldCommand;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;

namespace {

// Bug tags. Every assertion message begins with one of these.
constexpr const char* kB1 =
    "GitHub #458 (form fields invisible after placement): ";
constexpr const char* kB2 =
    "GitHub #459 (typed text invisible while editing a form text field): ";
constexpr const char* kB3 =
    "GitHub #460 (font settings ignored for form text fields): ";
constexpr const char* kB3NotImplemented =
    "GitHub #460: setFieldDefaultAppearance not implemented (GitHub #460) -- "
    "FormManager has no way to write a widget /DA after creation";

// ─── Raster helpers ──────────────────────────────────────────────────────────
// renderOverWhite / luminance / inkFractionIn are copied verbatim from
// tests/test_form_appearance_render.cpp (they live in that file's anonymous
// namespace and cannot be linked to).

// Page::render() goes through fz_new_pixmap_from_page(..., alpha=1), which
// clears to TRANSPARENT BLACK; flatten onto white so "ink" means something.
QImage renderOverWhite(Document* doc, int pageIdx, float scale) {
    if (!doc) return QImage();
    auto p = doc->page(pageIdx);
    if (!p) return QImage();
    QImage raw = p->render(p->boundingBox(), scale);
    if (raw.isNull()) return raw;
    QImage out(raw.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, raw);
    painter.end();
    return out;
}

int luminance(QRgb c) {
    return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
}

// Fraction of pixels inside `box` that carry ink (luminance < 160).
double inkFractionIn(const QImage& img, const QRect& box) {
    if (img.isNull()) return 0.0;
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long dark = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x)
            if (luminance(row[x]) < 160) ++dark;
    }
    return static_cast<double>(dark) /
           (static_cast<double>(r.width()) * r.height());
}

// Fraction of pixels inside `box` that are SHADED: visibly not white, but too
// light to be ink. A field background such as /BG [0.9 0.9 0.9] lands here
// (luminance ~230); bare paper (255) and black ink (< 160) do not.
double shadeFractionIn(const QImage& img, const QRect& box) {
    if (img.isNull()) return 0.0;
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long shaded = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const int l = luminance(row[x]);
            if (l >= 160 && l <= 250) ++shaded;
        }
    }
    return static_cast<double>(shaded) /
           (static_cast<double>(r.width()) * r.height());
}

// The LEAST-inked of the four edge strips of `box`, each `band` pixels deep.
// A visible border is a closed ring, so the weakest side is the oracle: a
// field drawn with only a top rule, or a shading with no outline, scores ~0.
double weakestEdgeInk(const QImage& img, const QRect& box, int band) {
    const QRect top(box.left(), box.top(), box.width(), band);
    const QRect bottom(box.left(), box.bottom() - band + 1, box.width(), band);
    const QRect left(box.left(), box.top(), band, box.height());
    const QRect right(box.right() - band + 1, box.top(), band, box.height());
    return std::min({inkFractionIn(img, top), inkFractionIn(img, bottom),
                     inkFractionIn(img, left), inkFractionIn(img, right)});
}

// Dark (luminance < 110) pixels inside `box`, skipping the columns
// [excludeX0, excludeX1]. The threshold sits below FormTool's blue selection
// pen (luminance ~94 is excluded by insetting past it, see callers) and far
// below its translucent selection brush over white (~236).
long darkPixelsIn(const QImage& img, const QRect& box, int excludeX0,
                  int excludeX1) {
    const QRect r = box.intersected(img.rect());
    long dark = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            if (x >= excludeX0 && x <= excludeX1) continue;
            if (luminance(row[x]) < 110) ++dark;
        }
    }
    return dark;
}

struct InkExtent {
    int top = -1;
    int bottom = -1;
    long inkPixels = 0;
    long redPixels = 0;
    int height() const { return top < 0 ? 0 : bottom - top + 1; }
};

// Vertical extent of ink (luminance < 160) inside `box`, plus how much of that
// ink is RED (r > 150, g < 100, b < 100).
InkExtent inkExtentIn(const QImage& img, const QRect& box) {
    InkExtent e;
    const QRect r = box.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb c = row[x];
            if (luminance(c) >= 160) continue;
            ++e.inkPixels;
            if (qRed(c) > 150 && qGreen(c) < 100 && qBlue(c) < 100) ++e.redPixels;
            if (e.top < 0) e.top = y;
            e.bottom = y;
        }
    }
    return e;
}

// ─── MuPDF readback (POD out-structs; nothing RAII inside the lambdas) ───────

struct DaReadback {
    bool present = false;
    char font[64] = {};
    float size = 0.0f;
    int n = 0;
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct MkBsReadback {
    bool hasMk = false;
    int bcLen = 0;
    int bgLen = 0;
    bool hasBs = false;
    float bsWidth = 0.0f;
};

pdf_obj* firstWidgetObj(MuPdfContext* ctx, const Page* page) {
    return static_cast<pdf_obj*>(FormManager::firstWidgetObjForPageSeam(ctx, page));
}

// Parse the first widget's (inheritable) /DA without font-name mapping, so
// "TiRo" reads back as "TiRo".
DaReadback readFirstWidgetDa(MuPdfContext* ctx, const Page* page) {
    DaReadback out;
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return out;
    DaReadback* o = &out;
    ctx->execute([obj, o](fz_context* c) {
        pdf_obj* daObj = pdf_dict_get_inheritable(c, obj, PDF_NAME(DA));
        const char* da = pdf_to_text_string(c, daObj);
        if (!da || !da[0]) return;
        pdf_parse_default_appearance_unmapped(c, da, o->font,
                                              static_cast<int>(sizeof(o->font)),
                                              &o->size, &o->n, o->color);
        o->present = true;
    });
    return out;
}

MkBsReadback readFirstWidgetMkBs(MuPdfContext* ctx, const Page* page) {
    MkBsReadback out;
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return out;
    MkBsReadback* o = &out;
    ctx->execute([obj, o](fz_context* c) {
        pdf_obj* mk = pdf_dict_get(c, obj, PDF_NAME(MK));
        if (pdf_is_dict(c, mk)) {
            o->hasMk = true;
            o->bcLen = pdf_array_len(c, pdf_dict_get(c, mk, PDF_NAME(BC)));
            o->bgLen = pdf_array_len(c, pdf_dict_get(c, mk, PDF_NAME(BG)));
        }
        pdf_obj* bs = pdf_dict_get(c, obj, PDF_NAME(BS));
        if (pdf_is_dict(c, bs)) {
            o->hasBs = true;
            pdf_obj* w = pdf_dict_get(c, bs, PDF_NAME(W));
            // PDF 32000-1 table 166: /W defaults to 1 when absent.
            o->bsWidth = w ? pdf_to_real(c, w) : 1.0f;
        }
    });
    return out;
}

// Author a /DA directly on the first widget, as a field loaded from a file
// carries one. Used by the survival probes so they test the restore path, not
// the (still missing) setter.
void authorFirstWidgetDa(MuPdfContext* ctx, const Page* page, const char* da) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, da](fz_context* c) {
        pdf_dict_put_text_string(c, obj, PDF_NAME(DA), da);
    });
}

// ─── The not-yet-existing API, reached without breaking the build ────────────

template <typename M>
concept HasSetFieldDefaultAppearance =
    requires(M& m, const Page* p, int idx, const std::string& font, float size,
             const QColor& color) {
        m.setFieldDefaultAppearance(p, idx, font, size, color);
    };

// Returns false (and does nothing) while FormManager lacks the method.
template <typename M>
bool setFieldDefaultAppearanceIfImplemented(M& m, const Page* p, int idx,
                                            const std::string& font, float size,
                                            const QColor& color) {
    if constexpr (HasSetFieldDefaultAppearance<M>) {
        static_cast<void>(m.setFieldDefaultAppearance(p, idx, font, size, color));
        return true;
    } else {
        static_cast<void>(m);
        static_cast<void>(p);
        static_cast<void>(idx);
        static_cast<void>(font);
        static_cast<void>(size);
        static_cast<void>(color);
        return false;
    }
}

// ─── Fixture plumbing ────────────────────────────────────────────────────────

std::filesystem::path scratchDir() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pdfe_form_textfield_state";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string currentTestSlug() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string s = std::string(info->test_suite_name()) + "_" + info->name();
    for (char& ch : s)
        if (ch == '/' || ch == '\\') ch = '_';
    return s;
}

const char* typeName(FormFieldType t) {
    switch (t) {
        case FormFieldType::Text: return "Text";
        case FormFieldType::ComboBox: return "ComboBox";
        case FormFieldType::ListBox: return "ListBox";
        default: return "Other";
    }
}

// Field geometry on a blank Letter page, in MuPDF page space (Y-down, points).
const QRectF kFieldRect(100.0, 100.0, 240.0, 40.0);
constexpr float kRenderScale = 3.0f;

QRect fieldPixelBox(const QRectF& r, float scale) {
    return QRect(static_cast<int>(std::lround(r.left() * scale)),
                 static_cast<int>(std::lround(r.top() * scale)),
                 static_cast<int>(std::lround(r.width() * scale)),
                 static_cast<int>(std::lround(r.height() * scale)));
}

// A blank one-page document with a FormManager and UndoManager.
class BlankFormDocTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        path_ = scratchDir() / (currentTestSlug() + ".pdf");
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        doc_ = Document::createBlank(ctx_, path_);
        ASSERT_NE(doc_, nullptr) << "harness: Document::createBlank failed";
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr) << "harness: blank page 0 missing";
        manager_ = std::make_unique<FormManager>(ctx_);
        undo_ = std::make_unique<UndoManager>();
    }

    void TearDown() override {
        undo_.reset();
        manager_.reset();
        page_.reset();
        doc_.reset();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    // Create a field through the undoable command, as MainWindow does.
    int createViaCommand(FormFieldType type, const std::string& name) {
        undo_->execute(std::make_unique<CreateFormFieldCommand>(
            manager_.get(), page_.get(), type, kFieldRect, name));
        const auto fields = manager_->getFormFields(page_.get());
        return fields.empty() ? -1 : static_cast<int>(fields.size()) - 1;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path path_;
    std::unique_ptr<Document> doc_;
    std::shared_ptr<Page> page_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<UndoManager> undo_;
};

// ════════════════════════════════════════════════════════════════════════════
// B1 — GitHub #458: placed fields are invisible.
// ════════════════════════════════════════════════════════════════════════════
//
// Check, radio and push buttons are NOT parameterised here: MuPDF 1.26.10's
// pdf_write_widget_appearance (pdf-appearance.c) throws for /FT /Btn, so their
// appearance is not synthesised from /MK by the path #458 is about. Text,
// combo box and list box all go through pdf_write_tx_widget_appearance, which
// paints /MK /BG and the /MK /BC border at /BS /W — the fix's whole surface.

class FormTextfieldAppearanceState
    : public BlankFormDocTest,
      public ::testing::WithParamInterface<FormFieldType> {
protected:
    // The visibility oracle, shared by the three lifecycle probes.
    void expectVisibleField(const QImage& img, const char* when) {
        ASSERT_FALSE(img.isNull()) << kB1 << "harness: page did not rasterize ("
                                   << when << ")";
        const QRect box = fieldPixelBox(kFieldRect, kRenderScale);
        // A 1pt border is 3px at this scale; a 4px strip tolerates rounding.
        const double ring = weakestEdgeInk(img, box, 4);
        EXPECT_GT(ring, 0.40)
            << kB1 << typeName(GetParam()) << " field " << when
            << ": the weakest side of the border ring carries ink fraction "
            << ring << " -- no visible outline. createFormField writes no /MK "
               "/BC and no /BS, so MuPDF's appearance stream draws no border.";
        // Interior: 3pt inside the edge, past any border.
        const QRect interior = box.adjusted(9, 9, -9, -9);
        const double shade = shadeFractionIn(img, interior);
        EXPECT_GT(shade, 0.90)
            << kB1 << typeName(GetParam()) << " field " << when
            << ": only " << shade << " of the interior is shaded -- the field "
               "background is bare paper. createFormField writes no /MK /BG.";
        EXPECT_LT(inkFractionIn(img, interior), 0.02)
            << kB1 << typeName(GetParam()) << " field " << when
            << ": the interior of an EMPTY field must stay light enough to read "
               "typed text over (no dark fill).";
    }
};

TEST_P(FormTextfieldAppearanceState, FreshFieldShowsBorderRingAndShadedInterior) {
    const int idx = createViaCommand(GetParam(), "b1_fresh");
    ASSERT_EQ(idx, 0) << kB1 << "harness: field was not created";
    expectVisibleField(renderOverWhite(doc_.get(), 0, kRenderScale),
                       "freshly created");
}

TEST_P(FormTextfieldAppearanceState, FieldStaysVisibleAfterUndoRedo) {
    ASSERT_EQ(createViaCommand(GetParam(), "b1_undo_redo"), 0)
        << kB1 << "harness: field was not created";
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << kB1 << "harness: undo did not remove the field";
    undo_->redo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << kB1 << "harness: redo did not restore the field";
    expectVisibleField(renderOverWhite(doc_.get(), 0, kRenderScale),
                       "after undo+redo (restoreFormField path)");
}

TEST_P(FormTextfieldAppearanceState, FieldStaysVisibleAfterSaveAndReopen) {
    ASSERT_EQ(createViaCommand(GetParam(), "b1_save"), 0)
        << kB1 << "harness: field was not created";
    const auto out = scratchDir() / (currentTestSlug() + "_saved.pdf");
    std::error_code ec;
    std::filesystem::remove(out, ec);
    undo_.reset();   // commands hold raw Page*; drop them before the rewrite
    page_.reset();   // release the page before the full-rewrite save
    doc_->saveAs(out);

    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr) << kB1 << "harness: saved file did not reopen";
    {
        auto p = reopened->page(0);
        ASSERT_NE(p, nullptr) << kB1 << "harness: reopened page missing";
        ASSERT_EQ(manager_->getFormFields(p.get()).size(), 1u)
            << kB1 << "harness: the field did not survive the save";
    }
    expectVisibleField(renderOverWhite(reopened.get(), 0, kRenderScale),
                       "after save+reopen");
    reopened.reset();
    std::filesystem::remove(out, ec);
}

class FormTextfieldAppearanceStructure
    : public BlankFormDocTest,
      public ::testing::WithParamInterface<FormFieldType> {};

TEST_P(FormTextfieldAppearanceStructure, CreatedFieldCarriesMkBcBgAndBs) {
    ASSERT_EQ(createViaCommand(GetParam(), "b1_structure"), 0)
        << kB1 << "harness: field was not created";
    const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
    EXPECT_TRUE(mk.hasMk)
        << kB1 << typeName(GetParam())
        << " widget has no /MK dictionary; MuPDF reads border and background "
           "colours only from /MK.";
    EXPECT_GE(mk.bcLen, 1)
        << kB1 << typeName(GetParam())
        << " widget has no /MK /BC (border colour) array.";
    EXPECT_GE(mk.bgLen, 1)
        << kB1 << typeName(GetParam())
        << " widget has no /MK /BG (background colour) array.";
    EXPECT_TRUE(mk.hasBs)
        << kB1 << typeName(GetParam()) << " widget has no /BS border style.";
    EXPECT_GT(mk.bsWidth, 0.0f)
        << kB1 << typeName(GetParam()) << " widget border width /BS /W is "
        << mk.bsWidth << "; a zero-width border is invisible.";
}

std::string fieldTypeParamName(const ::testing::TestParamInfo<FormFieldType>& i) {
    return typeName(i.param);
}

// The instantiation prefix is "FormTextfield" so that every generated name
// ("FormTextfield/FormTextfieldAppearanceState.X/Text") matches the gate's
// "FormTextfield*" filter in both directions.
INSTANTIATE_TEST_SUITE_P(FormTextfield, FormTextfieldAppearanceState,
                         ::testing::Values(FormFieldType::Text,
                                           FormFieldType::ComboBox,
                                           FormFieldType::ListBox),
                         fieldTypeParamName);

INSTANTIATE_TEST_SUITE_P(FormTextfield, FormTextfieldAppearanceStructure,
                         ::testing::Values(FormFieldType::Text,
                                           FormFieldType::ComboBox,
                                           FormFieldType::ListBox),
                         fieldTypeParamName);

// ════════════════════════════════════════════════════════════════════════════
// B2 — GitHub #459: typed text is not painted while editing.
// ════════════════════════════════════════════════════════════════════════════
//
// Paint harness after tests/test_form_tool_document_closing.cpp: drive a real
// FormTool, call onPaintOverlay into an off-screen image, and inspect pixels.
// The overlay is all the user sees of the field while typing — the page
// raster underneath comes from the appearance stream, which is stale until
// commit — so the typed text must be in the OVERLAY.

class FormTextfieldEditPaintState : public BlankFormDocTest {
protected:
    static constexpr float kZoom = 2.0f;

    void SetUp() override {
        BlankFormDocTest::SetUp();
        if (HasFatalFailure()) return;
        idx_ = manager_->createFormField(page_.get(), FormFieldType::Text,
                                         kFieldRect, "b2_field");
        ASSERT_EQ(idx_, 0) << kB2 << "harness: text field was not created";
        tool_ = std::make_unique<FormTool>(manager_.get(), nullptr, undo_.get());
        tool_->activate();
        tool_->setPage(page_.get());
        click(kFieldRect.center());
        ASSERT_TRUE(tool_->isEditingField())
            << kB2 << "harness: clicking the text field did not start an edit";
    }

    void TearDown() override {
        tool_.reset();
        BlankFormDocTest::TearDown();
    }

    void click(QPointF pdfPoint) {
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, pdfPoint);
    }

    void type(const QString& s) {
        for (const QChar ch : s) {
            QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
            tool_->onKeyPress(&ev);
        }
    }

    QImage paint() {
        QImage canvas(1000, 500, QImage::Format_ARGB32);
        canvas.fill(Qt::white);
        QPainter painter(&canvas);
        tool_->onPaintOverlay(painter, 0, kZoom, QPointF(0, 0), 792.0f);
        painter.end();
        return canvas;
    }

    // The field rect on screen, inset past the 2px selection pen.
    static QRect screenInterior() {
        return fieldPixelBox(kFieldRect, kZoom).adjusted(4, 4, -4, -4);
    }

    // Dark pixels inside the field that are NOT the caret.
    long textInk(const QImage& img) {
        QRectF caret;
        const bool haveCaret = tool_->caretScreenRect(caret);
        const int cx = haveCaret ? static_cast<int>(std::floor(caret.left())) : -100;
        return darkPixelsIn(img, screenInterior(), cx - 3, cx + 3);
    }

    double caretX() {
        paint();
        QRectF caret;
        EXPECT_TRUE(tool_->caretScreenRect(caret))
            << kB2 << "harness: no caret was painted for the active edit";
        return caret.left();
    }

    int idx_ = -1;
    std::unique_ptr<FormTool> tool_;
};

TEST_F(FormTextfieldEditPaintState, TypedTextIsPaintedInsideTheFieldBeyondTheCaret) {
    type(QStringLiteral("WWWW"));
    ASSERT_EQ(tool_->editBufferForTesting(), "WWWW")
        << kB2 << "harness: keystrokes did not reach the edit buffer";
    const long ink = textInk(paint());
    EXPECT_GT(ink, 60)
        << kB2 << "after typing \"WWWW\" the field interior holds " << ink
        << " dark pixels outside the caret column -- the overlay paints the "
           "caret but never editBuffer_, so the typed text is invisible until "
           "commit.";
}

TEST_F(FormTextfieldEditPaintState, PaintedInkGrowsWithMoreCharacters) {
    type(QStringLiteral("WW"));
    const long two = textInk(paint());
    type(QStringLiteral("WWWW"));
    const long six = textInk(paint());
    EXPECT_GT(two, 20)
        << kB2 << "two typed characters painted " << two << " dark pixels.";
    EXPECT_GT(six, 2 * two)
        << kB2 << "six characters painted " << six << " dark pixels vs " << two
        << " for two -- the overlay ink does not follow the edit buffer.";
}

TEST_F(FormTextfieldEditPaintState, PreeditTextIsPainted) {
    tool_->setPreeditText(QStringLiteral("WWWW"));
    ASSERT_TRUE(tool_->hasPreedit())
        << kB2 << "harness: the composition was not stored";
    ASSERT_TRUE(tool_->editBufferForTesting().empty())
        << kB2 << "harness: the preedit leaked into editBuffer_";
    const long ink = textInk(paint());
    EXPECT_GT(ink, 60)
        << kB2 << "an in-flight IME composition \"WWWW\" painted " << ink
        << " dark pixels -- the preedit is never drawn "
           "(IME-FORM-PREEDIT-PAINT-01).";
}

TEST_F(FormTextfieldEditPaintState, CaretXMatchesTheTextAdvance) {
    const double x0 = caretX();          // empty buffer: caret at text origin
    type(QStringLiteral("WWWW"));
    const double x4 = caretX();

    // The field /DA is "/Helv 12 Tf 0 g"; at zoom 2 that is a 24px Helvetica.
    QFont font(QStringLiteral("Helvetica"));
    font.setStyleHint(QFont::Helvetica);
    font.setPixelSize(24);
    const double expected = QFontMetricsF(font).horizontalAdvance(QStringLiteral("WWWW"));
    // Cross-check against the base-14 Helvetica AFM width of W (944/1000 em),
    // so a QFont substitution that is not Helvetica-metric shows up here.
    const double afm = 4 * 0.944 * 24.0;
    EXPECT_NEAR(expected, afm, 2.0)
        << kB2 << "harness: QFont(\"Helvetica\") on this host is not "
           "Helvetica-metric (advance " << expected << " vs AFM " << afm << ").";
    EXPECT_NEAR(x4 - x0, expected, 1.0)
        << kB2 << "the caret moved " << (x4 - x0) << "px for \"WWWW\" but the "
           "text advance is " << expected << "px -- the caret uses a uniform "
           "field-width/(n+1) approximation, not the field font's metrics.";
}

// CONTROL (green on current code, by design). Fix option B — writing the
// value on every keystroke — would make the text visible and break undo/JS
// semantics; this pins that the fix must not take it.
TEST_F(FormTextfieldEditPaintState, DocumentValueIsUnchangedWhileEditing) {
    type(QStringLiteral("WWWW"));
    paint();
    EXPECT_EQ(manager_->getFormField(page_.get(), idx_).value, "")
        << kB2 << "the document value changed before commit; the fix must "
           "paint the edit buffer, not write the field per keystroke.";
    EXPECT_FALSE(undo_->canUndo())
        << kB2 << "an undo command was pushed while typing, before commit.";
}

// CONTROL (green on current code, by design): commit still writes the value.
TEST_F(FormTextfieldEditPaintState, CommitStillWritesTheValue) {
    type(QStringLiteral("WWWW"));
    paint();
    click(QPointF(5.0, 5.0));   // outside every field: commits the edit
    EXPECT_FALSE(tool_->isEditingField())
        << kB2 << "clicking outside the field did not end the edit.";
    EXPECT_EQ(manager_->getFormField(page_.get(), idx_).value, "WWWW")
        << kB2 << "commit did not write the typed value.";
}

// ════════════════════════════════════════════════════════════════════════════
// B3 — GitHub #460: font settings are ignored.
// ════════════════════════════════════════════════════════════════════════════

class FormTextfieldFontState : public BlankFormDocTest {};

TEST_F(FormTextfieldFontState, SetDefaultAppearanceReadsBackTiRo18Red) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        kFieldRect, "b3_da"), 0)
        << kB3 << "harness: text field was not created";
    ASSERT_TRUE(setFieldDefaultAppearanceIfImplemented(
        *manager_, page_.get(), 0, "TiRo", 18.0f, QColor(255, 0, 0)))
        << kB3NotImplemented;
    const DaReadback da = readFirstWidgetDa(ctx_.get(), page_.get());
    ASSERT_TRUE(da.present) << kB3 << "the widget has no /DA at all.";
    EXPECT_STREQ(da.font, "TiRo")
        << kB3 << "/DA font did not read back as the base-14 key TiRo.";
    EXPECT_NEAR(da.size, 18.0f, 0.01f) << kB3 << "/DA size did not read back as 18.";
    EXPECT_EQ(da.n, 3) << kB3 << "/DA colour is not an RGB triple.";
    EXPECT_NEAR(da.color[0], 1.0f, 0.01f) << kB3 << "/DA colour red component.";
    EXPECT_NEAR(da.color[1], 0.0f, 0.01f) << kB3 << "/DA colour green component.";
    EXPECT_NEAR(da.color[2], 0.0f, 0.01f) << kB3 << "/DA colour blue component.";
}

TEST_F(FormTextfieldFontState, RenderedInkHeightScalesWithSizeAndIsRed) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        kFieldRect, "b3_ink"), 0)
        << kB3 << "harness: text field was not created";
    // Interior only (3pt in), so a border added by the #458 fix is not ink.
    const QRect interior = fieldPixelBox(kFieldRect, kRenderScale).adjusted(9, 9, -9, -9);

    ASSERT_TRUE(setFieldDefaultAppearanceIfImplemented(
        *manager_, page_.get(), 0, "Helv", 10.0f, QColor(255, 0, 0)))
        << kB3NotImplemented;
    manager_->setFieldValue(page_.get(), 0, "HHHH");
    const InkExtent small = inkExtentIn(renderOverWhite(doc_.get(), 0, kRenderScale), interior);

    ASSERT_TRUE(setFieldDefaultAppearanceIfImplemented(
        *manager_, page_.get(), 0, "Helv", 20.0f, QColor(255, 0, 0)))
        << kB3NotImplemented;
    manager_->setFieldValue(page_.get(), 0, "HHHHH");   // force regeneration
    manager_->setFieldValue(page_.get(), 0, "HHHH");
    const InkExtent large = inkExtentIn(renderOverWhite(doc_.get(), 0, kRenderScale), interior);

    ASSERT_GT(small.height(), 0) << kB3 << "no ink drawn for the 10pt value.";
    const double ratio = static_cast<double>(large.height()) / small.height();
    EXPECT_NEAR(ratio, 2.0, 0.3)
        << kB3 << "ink height went " << small.height() << "px -> "
        << large.height() << "px for /DA size 10 -> 20; the field ignores its size.";
    const double redShare = large.inkPixels
        ? static_cast<double>(large.redPixels) / large.inkPixels : 0.0;
    EXPECT_GT(redShare, 0.8)
        << kB3 << "only " << redShare << " of the value's ink is red although "
           "the /DA colour is 1 0 0 rg.";
}

TEST_F(FormTextfieldFontState, DefaultAppearanceSurvivesUndoAndRedoOfCreation) {
    ASSERT_EQ(createViaCommand(FormFieldType::Text, "b3_undo"), 0)
        << kB3 << "harness: text field was not created";
    authorFirstWidgetDa(ctx_.get(), page_.get(), "/TiRo 18 Tf 1 0 0 rg");
    const DaReadback before = readFirstWidgetDa(ctx_.get(), page_.get());
    ASSERT_STREQ(before.font, "TiRo") << kB3 << "harness: authored /DA not in place.";

    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << kB3 << "harness: undo did not remove the field";
    undo_->redo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << kB3 << "harness: redo did not restore the field";

    const DaReadback after = readFirstWidgetDa(ctx_.get(), page_.get());
    EXPECT_STREQ(after.font, "TiRo")
        << kB3 << "undo+redo rebuilt the field with font '" << after.font
        << "': restoreFormField re-creates /DA hard-coded as /Helv 12 Tf 0 g.";
    EXPECT_NEAR(after.size, 18.0f, 0.01f)
        << kB3 << "undo+redo lost the /DA size (now " << after.size << ").";
    EXPECT_NEAR(after.color[0], 1.0f, 0.01f)
        << kB3 << "undo+redo lost the /DA red colour.";
}

TEST_F(FormTextfieldFontState, DefaultAppearanceSurvivesDeleteAndUndo) {
    ASSERT_EQ(createViaCommand(FormFieldType::Text, "b3_delete"), 0)
        << kB3 << "harness: text field was not created";
    authorFirstWidgetDa(ctx_.get(), page_.get(), "/TiRo 18 Tf 1 0 0 rg");

    undo_->execute(std::make_unique<DeleteFormFieldCommand>(
        manager_.get(), page_.get(), 0));
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << kB3 << "harness: delete did not remove the field";
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << kB3 << "harness: undo of delete did not restore the field";

    const DaReadback after = readFirstWidgetDa(ctx_.get(), page_.get());
    EXPECT_STREQ(after.font, "TiRo")
        << kB3 << "delete+undo restored the field with font '" << after.font
        << "': restoreFormField drops the field's /DA.";
    EXPECT_NEAR(after.size, 18.0f, 0.01f)
        << kB3 << "delete+undo lost the /DA size (now " << after.size << ").";
    EXPECT_NEAR(after.color[0], 1.0f, 0.01f)
        << kB3 << "delete+undo lost the /DA red colour.";
}

// A font change while typing must keep the typed text, and the commit must
// write the value under the NEW /DA.
TEST_F(FormTextfieldFontState, MidEditFontChangeKeepsTheBufferAndCommitUsesNewDa) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        kFieldRect, "b3_midedit"), 0)
        << kB3 << "harness: text field was not created";
    FormTool tool(manager_.get(), nullptr, undo_.get());
    tool.activate();
    tool.setPage(page_.get());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool.onMousePress(&press, 0, kFieldRect.center());
    ASSERT_TRUE(tool.isEditingField()) << kB3 << "harness: edit did not start";
    for (const QChar ch : QStringLiteral("ABC")) {
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
        tool.onKeyPress(&ev);
    }
    ASSERT_EQ(tool.editBufferForTesting(), "ABC") << kB3 << "harness: typing failed";

    const bool applied = setFieldDefaultAppearanceIfImplemented(
        *manager_, page_.get(), 0, "TiRo", 18.0f, QColor(255, 0, 0));
    EXPECT_TRUE(applied) << kB3NotImplemented;
    EXPECT_TRUE(tool.isEditingField())
        << kB3 << "the font change ended the edit in progress.";
    EXPECT_EQ(tool.editBufferForTesting(), "ABC")
        << kB3 << "the font change discarded the typed text.";

    QMouseEvent outside(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool.onMousePress(&outside, 0, QPointF(5.0, 5.0));
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "ABC")
        << kB3 << "commit after a mid-edit font change did not write the value.";
    const DaReadback da = readFirstWidgetDa(ctx_.get(), page_.get());
    EXPECT_STREQ(da.font, "TiRo")
        << kB3 << "after commit the field /DA font is '" << da.font
        << "', not the TiRo chosen mid-edit.";
    EXPECT_NEAR(da.size, 18.0f, 0.01f)
        << kB3 << "after commit the field /DA size is " << da.size << ", not 18.";
}

// The properties panel must offer font controls, findable by objectName and
// announced to assistive technology by accessibleName. The names below are
// the contract phase 2 implements.
TEST(FormTextfieldPanelState, PropertiesPanelExposesFontControls) {
    ASSERT_NE(QApplication::instance(), nullptr) << kB3 << "harness: no QApplication";
    pdfe::ui::FormFieldPropertiesPanel panel;
    panel.show();
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "b3_panel";
    data.rect = kFieldRect;
    data.pageIndex = 0;
    data.widgetIndex = 0;
    panel.setFormField(data);
    QCoreApplication::processEvents();

    for (const char* name : {"formFieldFontCombo", "formFieldFontSizeSpin",
                             "formFieldFontColorButton"}) {
        auto* w = panel.findChild<QWidget*>(QString::fromLatin1(name));
        EXPECT_NE(w, nullptr)
            << kB3 << "FormFieldPropertiesPanel has no '" << name
            << "' control; a form field's font cannot be changed from the panel.";
        EXPECT_FALSE(w ? w->accessibleName().isEmpty() : true)
            << kB3 << "'" << name << "' has no accessibleName.";
        EXPECT_TRUE(w ? w->isVisibleTo(&panel) : false)
            << kB3 << "'" << name << "' is not shown for a Text field.";
    }
}

// MainWindow: switching from Edit Text to the Form tool must not leave the
// text-edit toolbar visible — its signals are wired to the destroyed
// TextEditTool, so every change made there goes nowhere.
TEST(FormTextfieldToolbarState, TextEditToolbarIsHiddenUnderTheFormTool) {
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << kB3 << "harness: committed fixture test_form.pdf missing";

    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(fixture.string()));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ASSERT_NE(mw.documentForTesting(), nullptr) << kB3 << "harness: fixture did not open";

    auto* textAct = mw.findChild<QAction*>(QStringLiteral("textEditToolAction"));
    auto* formAct = mw.findChild<QAction*>(QStringLiteral("formToolAction"));
    auto* toolbar = mw.findChild<QToolBar*>(QStringLiteral("textEditToolBar"));
    ASSERT_NE(textAct, nullptr) << kB3 << "harness: textEditToolAction missing";
    ASSERT_NE(formAct, nullptr) << kB3 << "harness: formToolAction missing";
    ASSERT_NE(toolbar, nullptr) << kB3 << "harness: textEditToolBar missing";

    textAct->trigger();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ASSERT_TRUE(toolbar->isVisibleTo(&mw))
        << kB3 << "harness precondition: the Edit Text tool must show the "
           "text-edit toolbar, otherwise the assertion below passes vacuously.";

    formAct->trigger();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_FALSE(toolbar->isVisibleTo(&mw))
        << kB3 << "the text-edit toolbar is still visible under the Form tool; "
           "the form branch of onAnnotationToolSelected never hides it and its "
           "connections target the destroyed TextEditTool.";
}

}  // namespace
