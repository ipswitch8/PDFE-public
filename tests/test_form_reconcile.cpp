// -*- coding: utf-8 -*-
//
// tests/test_form_reconcile.cpp
//
// Pipeline form-reconcile, phase 1: tests for the points where origin/master's
// form-field fixes (afb57e4c, 596d613f) and PR #462's (GitHub #458/#459/#460)
// were reconciled into one implementation. The decisions are numbered in
// .claude/pipelines/form-reconcile/evidence/phase-1/notes.md.
//
//   FormFieldReconcileOverlay -- decision 2. master's at-rest overlay keeps
//       its tint on every unselected widget, but strokes its outline only
//       where the widget has no border of its own: a widget with an /MK /BC
//       colour and a non-zero border width (every field the app creates
//       except a Signature, GitHub #458) is not outlined a second time; a
//       widget with no /MK /BC, or a zero-width border, and every Signature,
//       is.
//   FormFieldReconcileStyle -- decision 4. Bold/italic base-14 variant keys
//       are refused by the one setter, with the document byte-identical, and
//       the reason is pinned: MuPDF 1.26.10 renders a /HeBo /DA exactly like
//       /Helv and reads it back as "Helv".
//   FormFieldReconcileCommand -- decision 1. The one command never merges,
//       and a refused request reports failed(), so it never becomes an undo
//       step.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker.
//
// fz_try discipline: every MuPdfContext::execute lambda below declares only
// trivially-destructible locals and writes through raw pointers
// (tools/gate/fz_try_raii_scan.py).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "support/FormDocumentBytes.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRectF>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using pdfe::commands::UndoManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldOwnDefaultAppearance;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;
using pdfe::forms::SetFieldAppearanceCommand;

namespace {

constexpr const char* kR = "pipeline form-reconcile: ";
constexpr float kZoom = 2.0f;
constexpr QPointF kOrigin{0.0, 0.0};

std::filesystem::path scratchDir() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pdfe_form_reconcile";
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

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

pdf_obj* firstWidgetObj(MuPdfContext* ctx, const Page* page) {
    return static_cast<pdf_obj*>(FormManager::firstWidgetObjForPageSeam(ctx, page));
}

// The screen rect FormTool::onPaintOverlay draws a widget at, for kZoom and
// kOrigin (the same mapping as tests/test_form_tool_overlay_render.cpp).
QRect screenRectFor(const QRectF& r) {
    return QRect(static_cast<int>(kOrigin.x() + r.x() * kZoom),
                 static_cast<int>(kOrigin.y() + r.y() * kZoom),
                 static_cast<int>(r.width() * kZoom),
                 static_cast<int>(r.height() * kZoom));
}

int maxAlphaIn(const QImage& img, const QRect& box) {
    const QRect r = box.intersected(img.rect());
    int best = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            best = std::max(best, qAlpha(img.pixel(x, y)));
    return best;
}

// The strongest overlay ink on the WEAKEST of the four edges of `box`: each
// edge is a band 2 px either side of the rect's boundary line. An outline
// stroked around the rect lights all four; a fill alone leaves them at the
// fill's alpha.
int weakestEdgeAlpha(const QImage& img, const QRect& box) {
    const int b = 2;
    const QRect top(box.left() - b, box.top() - b, box.width() + 2 * b, 2 * b + 1);
    const QRect bottom(box.left() - b, box.top() + box.height() - b,
                       box.width() + 2 * b, 2 * b + 1);
    const QRect left(box.left() - b, box.top() - b, 2 * b + 1, box.height() + 2 * b);
    const QRect right(box.left() + box.width() - b, box.top() - b, 2 * b + 1,
                      box.height() + 2 * b);
    return std::min({maxAlphaIn(img, top), maxAlphaIn(img, bottom),
                     maxAlphaIn(img, left), maxAlphaIn(img, right)});
}

// The strongest overlay ink on ANY of the four edges.
int strongestEdgeAlpha(const QImage& img, const QRect& box) {
    const int b = 2;
    const QRect outer = box.adjusted(-b, -b, b, b);
    const QRect inner = box.adjusted(b + 1, b + 1, -(b + 1), -(b + 1));
    const QRect r = outer.intersected(img.rect());
    int best = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (!inner.contains(x, y)) best = std::max(best, qAlpha(img.pixel(x, y)));
    return best;
}

// Fraction of pixels in `box` that carry any overlay ink (master's oracle).
double overlayFractionIn(const QImage& img, const QRect& box) {
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long lit = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (qAlpha(img.pixel(x, y)) != 0) ++lit;
    return static_cast<double>(lit) / (static_cast<double>(r.width()) * r.height());
}

QImage paintOverlayUnselected(FormManager& mgr, Page* page) {
    UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(page);
    // Interaction mode, nothing selected: any ink is the at-rest cue.
    tool.setCreationFieldType(FormFieldType::Unknown);
    QImage canvas(1300, 1700, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        tool.onPaintOverlay(painter, page->index(), kZoom, kOrigin, 792.0f);
    }
    return canvas;
}

// Outline pen alpha is 200; the tint brush alpha is 20. These thresholds sit
// well clear of both.
constexpr int kOutlineAlpha = 150;
constexpr int kNoOutlineAlpha = 60;

class BlankDoc : public ::testing::Test {
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

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path path_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<UndoManager> undo_;
};

// ═══════════════════════════════════════════════════════════════════════════
// FormFieldReconcileOverlay -- decision 2
// ═══════════════════════════════════════════════════════════════════════════

class FormFieldReconcileOverlay : public BlankDoc {};

// Every type the app gives /MK and /BS (GitHub #458): MuPDF already draws its
// border, so the overlay adds only the tint -- no second rectangle.
TEST_F(FormFieldReconcileOverlay, OutlineIsNotDrawnOverAnMkBorder) {
    const FormFieldType types[] = {FormFieldType::Text, FormFieldType::Checkbox,
                                   FormFieldType::RadioButton, FormFieldType::ComboBox,
                                   FormFieldType::ListBox, FormFieldType::PushButton};
    float y = 60.0f;
    for (FormFieldType t : types) {
        ASSERT_GE(manager_->createFormField(page_.get(), t, QRectF(80, y, 160, 40),
                                            "mk_" + std::to_string(static_cast<int>(t))),
                  0)
            << "harness: field not created";
        y += 90.0f;
    }
    const std::vector<FormFieldData> fields = manager_->getFormFields(page_.get());
    ASSERT_EQ(fields.size(), 6u) << "harness";

    const QImage canvas = paintOverlayUnselected(*manager_, page_.get());
    for (const FormFieldData& f : fields) {
        ASSERT_GT(f.borderAppearance.borderColorComponents, 0)
            << "harness: " << f.fieldName << " has no /MK /BC";
        const QRect box = screenRectFor(f.rect);
        EXPECT_LT(strongestEdgeAlpha(canvas, box), kNoOutlineAlpha)
            << kR << f.fieldName
            << ": the at-rest overlay stroked an outline over a widget that "
               "already draws its own /MK border (double border).";
        EXPECT_GT(overlayFractionIn(canvas, box.adjusted(3, 3, -3, -3)), 0.02)
            << kR << f.fieldName << ": the at-rest tint is missing (master bug 1 cue).";
    }
}

// Loaded widgets with no /MK /BC (and /BS /W 0): no border of their own, so
// the overlay outline is their at-rest cue. Covers Text, Checkbox, ComboBox,
// ListBox, multiline Text and Signature.
TEST_F(FormFieldReconcileOverlay, OutlineIsDrawnForAWidgetWithoutAnMkBorder) {
    const auto fx = fixture("phase3_form_all_field_types.pdf");
    ASSERT_TRUE(std::filesystem::exists(fx)) << "fixture missing: " << fx;
    auto doc = Document::open(ctx_, fx);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    FormManager mgr(ctx_);
    const std::vector<FormFieldData> fields = mgr.getFormFields(page.get());
    ASSERT_GE(fields.size(), 5u) << "harness";

    const QImage canvas = paintOverlayUnselected(mgr, page.get());
    for (const FormFieldData& f : fields) {
        ASSERT_EQ(f.borderAppearance.borderColorComponents, 0)
            << "harness: " << f.fieldName << " unexpectedly has /MK /BC";
        EXPECT_GE(weakestEdgeAlpha(canvas, screenRectFor(f.rect)), kOutlineAlpha)
            << kR << f.fieldName
            << ": a widget with no border of its own got no overlay outline.";
    }
}

// A zero-width border draws nothing, so the outline comes back even though
// /MK /BC is present.
TEST_F(FormFieldReconcileOverlay, OutlineIsDrawnWhenTheMkBorderHasZeroWidth) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        QRectF(80, 80, 200, 40), "zero_width"),
              0);
    pdf_obj* obj = firstWidgetObj(ctx_.get(), page_.get());
    ASSERT_NE(obj, nullptr);
    ctx_->execute([obj](fz_context* c) {
        pdf_obj* bs = pdf_dict_get(c, obj, PDF_NAME(BS));
        pdf_dict_put_real(c, bs, PDF_NAME(W), 0.0);
    });
    const FormFieldData f = manager_->getFormField(page_.get(), 0);
    ASSERT_GT(f.borderAppearance.borderColorComponents, 0) << "harness";
    ASSERT_TRUE(f.borderAppearance.hasBorderStyle) << "harness";
    ASSERT_EQ(f.borderAppearance.borderWidth, 0.0f) << "harness";

    const QImage canvas = paintOverlayUnselected(*manager_, page_.get());
    EXPECT_GE(weakestEdgeAlpha(canvas, screenRectFor(f.rect)), kOutlineAlpha)
        << kR << "an /MK border of width 0 is invisible, so the outline must be drawn.";
}

// A Signature is always outlined: MuPDF's unsigned signature appearance
// ignores /MK, so even an /MK /BC on it draws no border.
TEST_F(FormFieldReconcileOverlay, SignatureIsOutlinedWithOrWithoutAnMkBorder) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Signature,
                                        QRectF(80, 80, 200, 60), "sig"),
              0);
    FormFieldData f = manager_->getFormField(page_.get(), 0);
    ASSERT_EQ(f.type, FormFieldType::Signature) << "harness";
    ASSERT_EQ(f.borderAppearance.borderColorComponents, 0)
        << "harness: GitHub #458 gives a Signature no /MK";
    {
        const QImage canvas = paintOverlayUnselected(*manager_, page_.get());
        EXPECT_GE(weakestEdgeAlpha(canvas, screenRectFor(f.rect)), kOutlineAlpha)
            << kR << "a Signature without /MK got no overlay outline.";
    }

    pdf_obj* obj = firstWidgetObj(ctx_.get(), page_.get());
    ASSERT_NE(obj, nullptr);
    ctx_->execute([obj](fz_context* c) {
        pdf_obj* mk = pdf_dict_put_dict(c, obj, PDF_NAME(MK), 1);
        pdf_obj* bc = pdf_dict_put_array(c, mk, PDF_NAME(BC), 1);
        pdf_array_push_real(c, bc, 0.0);
    });
    f = manager_->getFormField(page_.get(), 0);
    ASSERT_EQ(f.borderAppearance.borderColorComponents, 1) << "harness: /MK /BC not read";
    const QImage canvas = paintOverlayUnselected(*manager_, page_.get());
    EXPECT_GE(weakestEdgeAlpha(canvas, screenRectFor(f.rect)), kOutlineAlpha)
        << kR << "a Signature with an /MK /BC must still be outlined: its "
                 "unsigned appearance ignores /MK.";
}

// ═══════════════════════════════════════════════════════════════════════════
// FormFieldReconcileStyle -- decision 4
// ═══════════════════════════════════════════════════════════════════════════

using pdfe::testsupport::documentBytes;

class FormFieldReconcileStyle : public BlankDoc {
protected:
    static constexpr float kScale = 3.0f;
    const QRectF kRect{60, 60, 300, 50};

    // The widget's appearance as MuPDF renders it, cropped to the field.
    QImage renderField() {
        QImage raw = page_->render(page_->boundingBox(), kScale);
        if (raw.isNull()) return raw;
        QImage out(raw.size(), QImage::Format_RGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        p.drawImage(0, 0, raw);
        p.end();
        const QRect box(static_cast<int>(kRect.x() * kScale),
                        static_cast<int>(kRect.y() * kScale),
                        static_cast<int>(kRect.width() * kScale),
                        static_cast<int>(kRect.height() * kScale));
        return out.copy(box);
    }

    // Write a raw /DA (no validation; the undo path) and regenerate the
    // appearance, then render.
    QImage renderWithDa(const char* da) {
        FormFieldOwnDefaultAppearance s;
        s.hasDa = true;
        s.da = da;
        EXPECT_TRUE(manager_->restoreFieldOwnDefaultAppearance(page_.get(), 0, s))
            << "harness: raw /DA not written";
        return renderField();
    }
};

TEST_F(FormFieldReconcileStyle, StyleVariantKeysAreRejectedAndTheDocumentIsUnchanged) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kRect, "style"), 0);
    manager_->setFieldValue(page_.get(), 0, "Bold?");
    const std::string before = documentBytes(ctx_.get(), page_.get());
    ASSERT_FALSE(before.empty()) << "harness: the document did not serialise";
    ASSERT_EQ(before, documentBytes(ctx_.get(), page_.get()))
        << "harness: serialising twice gave different bytes";
    for (const char* key : {"HeBo", "HeOb", "HeBO", "TiBo", "TiIt", "TiBI",
                            "CoBo", "CoOb", "CoBO"}) {
        EXPECT_FALSE(pdfe::forms::isBase14DefaultAppearanceFont(key)) << kR << key;
        EXPECT_FALSE(manager_->setFieldDefaultAppearance(page_.get(), 0, key, 14.0f,
                                                         QColor(Qt::black)))
            << kR << key << " was accepted, but MuPDF 1.26.10 renders it as the regular face";
        EXPECT_EQ(documentBytes(ctx_.get(), page_.get()), before)
            << kR << "a refused " << key << " request modified the document";
    }
    // The command refuses it the same way, and stays off the undo stack.
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "HeBo", 14.0f, QColor(Qt::black)));
    EXPECT_FALSE(undo_->canUndo()) << kR << "a refused HeBo command became an undo step";
    EXPECT_EQ(documentBytes(ctx_.get(), page_.get()), before) << kR;
}

// The REASON bold/italic are out: MuPDF 1.26.10 maps every /DA font other
// than Helv/TiRo/Cour/Symb/ZaDb to Helvetica, both when it parses the /DA
// (pdf_parse_default_appearance) and when it loads the font to draw it
// (full_font_name). If a future MuPDF renders the variants, this goes red and
// decision 4 should be revisited.
TEST_F(FormFieldReconcileStyle, MuPdfRendersAStyleVariantDaAsTheRegularFace) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kRect, "style"), 0);
    manager_->setFieldValue(page_.get(), 0, "WWWW MMMM");

    const QImage regular = renderWithDa("/Helv 24 Tf 0 g");
    const QImage serif = renderWithDa("/TiRo 24 Tf 0 g");
    ASSERT_FALSE(regular.isNull());
    // Control: the oracle can tell two faces apart.
    ASSERT_NE(regular, serif) << "harness: Helv and TiRo rendered identically";

    for (const char* da : {"/HeBo 24 Tf 0 g", "/HeOb 24 Tf 0 g", "/HeBO 24 Tf 0 g"}) {
        const QImage variant = renderWithDa(da);
        EXPECT_EQ(variant, regular)
            << kR << da << " rendered differently from /Helv: MuPDF now renders "
                           "style variants, so revisit decision 4";
        EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Helv")
            << kR << da << " did not read back as Helv";
    }
    for (const char* da : {"/TiBo 24 Tf 0 g", "/TiIt 24 Tf 0 g", "/CoBo 24 Tf 0 g"}) {
        EXPECT_EQ(renderWithDa(da), regular)
            << kR << da << " did not fall back to regular Helvetica";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FormFieldReconcileCommand -- decision 1
// ═══════════════════════════════════════════════════════════════════════════

class FormFieldReconcileCommand : public BlankDoc {};

TEST_F(FormFieldReconcileCommand, RefusedRequestReportsFailedAndIsNotAnUndoStep) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        QRectF(60, 60, 300, 40), "cmd"),
              0);
    // An accepted change first, so the stack is not empty.
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 14.0f, QColor(255, 0, 0)));
    ASSERT_EQ(undo_->undoStackSize(), 1);

    auto bad = std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Arial", 14.0f, QColor(255, 0, 0));
    SetFieldAppearanceCommand* raw = bad.get();
    EXPECT_FALSE(raw->failed()) << kR << "failed() before execute";
    undo_->execute(std::move(bad));
    EXPECT_EQ(undo_->undoStackSize(), 1)
        << kR << "a refused request became an undo step that does nothing";

    // Undo reverts the accepted change, not a no-op.
    undo_->undo();
    EXPECT_EQ(manager_->getFieldDefaultAppearance(page_.get(), 0), "/Helv 12 Tf 0 g") << kR;
}

TEST_F(FormFieldReconcileCommand, ConsecutiveTweaksToOneFieldAreSeparateUndoSteps) {
    // Decision 1: master merged consecutive tweaks (a family change and a
    // later colour change on the same field would undo together). The
    // reconciled command never merges; the panel already turns a spin-box
    // drag into one request (editingFinished), so each undo reverts exactly
    // one committed change.
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        QRectF(60, 60, 300, 40), "cmd"),
              0);
    const std::string d0 = manager_->getFieldDefaultAppearance(page_.get(), 0);
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 12.0f, QColor(0, 0, 0)));
    const std::string d1 = manager_->getFieldDefaultAppearance(page_.get(), 0);
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 12.0f, QColor(255, 0, 0)));
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 30.0f, QColor(255, 0, 0)));
    EXPECT_EQ(undo_->undoStackSize(), 3) << kR << "consecutive tweaks were merged";
    undo_->undo();
    undo_->undo();
    EXPECT_EQ(manager_->getFieldDefaultAppearance(page_.get(), 0), d1)
        << kR << "two undos did not land on the family change";
    undo_->undo();
    EXPECT_EQ(manager_->getFieldDefaultAppearance(page_.get(), 0), d0) << kR;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Phase-1 claim-check remediation (appended). Pins for claims that no test
// pinned before:
//   FormFieldReconcilePanel          -- decision 7 / security review: a
//       read-only field's font controls are disabled AND every emission path
//       (family, size commit, colour button) emits nothing; the colour picker
//       is never even opened for it.
//   FormFieldReconcileNeedAppearances -- decision 3: the NeedAppearances
//       fail-safe kept in the one setter fires when MuPDF cannot generate the
//       appearance (a /RV nested deeper than its XML parser allows).
//   FormFieldReconcileCommand (more) -- decision 1's motivating hazard:
//       UndoManager::execute runs a declined merge candidate's execute() twice.
//   FormFieldReconcileData           -- decision 8: FormFieldData carries no
//       uncapped raw /DA copy (master's `defaultAppearance` member).
// ═══════════════════════════════════════════════════════════════════════════

#include "ui/FormFieldPropertiesPanel.h"

#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>

namespace {

using pdfe::ui::FormFieldFontRequest;
using pdfe::ui::FormFieldPropertiesPanel;

class FormFieldReconcilePanel : public ::testing::Test {
protected:
    void SetUp() override {
        panel_ = std::make_unique<FormFieldPropertiesPanel>();
        combo_ = panel_->findChild<QComboBox*>(QStringLiteral("formFieldFontCombo"));
        spin_ = panel_->findChild<QSpinBox*>(QStringLiteral("formFieldFontSizeSpin"));
        color_ = panel_->findChild<QPushButton*>(QStringLiteral("formFieldFontColorButton"));
        ASSERT_NE(combo_, nullptr);
        ASSERT_NE(spin_, nullptr);
        ASSERT_NE(color_, nullptr);
        panel_->setColorPickerForTesting([this](const QColor&, QWidget*) {
            ++pickerCalls_;
            return QColor(200, 0, 0);
        });
        QObject::connect(panel_.get(), &FormFieldPropertiesPanel::fieldFontChangeRequested,
                         [this](FormFieldData, FormFieldFontRequest) { ++emissions_; });
    }

    void show(bool readOnly) {
        FormFieldData d;
        d.type = FormFieldType::Text;
        d.fieldName = readOnly ? "ro" : "rw";
        d.widgetIndex = 0;  // a valid index, so only the read-only rule can refuse
        d.pageIndex = 0;
        d.isReadOnly = readOnly;
        d.daFontName = "Helv";
        d.daFontSize = 12.0f;
        panel_->setFormField(d);
    }

    // Every emission path, driven at the signal the panel listens to (a
    // disabled widget ignores user input, so the slots are reached directly:
    // this is the "programmatic trigger" the read-only rule must also refuse).
    void driveAllThreePaths() {
        combo_->setCurrentIndex(combo_->findData(QStringLiteral("TiRo")));  // family
        spin_->setValue(30);
        emit spin_->editingFinished();                                       // size commit
        emit color_->clicked(false);                                         // colour
    }

    std::unique_ptr<FormFieldPropertiesPanel> panel_;
    QComboBox* combo_ = nullptr;
    QSpinBox* spin_ = nullptr;
    QPushButton* color_ = nullptr;
    int emissions_ = 0;
    int pickerCalls_ = 0;
};

TEST_F(FormFieldReconcilePanel, ReadOnlyFieldEmitsNothingOnAnyFontPath) {
    show(true);
    EXPECT_FALSE(combo_->isEnabled()) << kR;
    EXPECT_FALSE(spin_->isEnabled()) << kR;
    EXPECT_FALSE(color_->isEnabled()) << kR << "the colour button must be disabled";

    driveAllThreePaths();
    EXPECT_EQ(emissions_, 0)
        << kR << "a read-only field's font was changed through the panel";
    EXPECT_EQ(pickerCalls_, 0)
        << kR << "the colour dialog was opened for a read-only field";
}

// Control: the same three drives on an editable field DO emit, once each, so
// the zero above is the read-only rule and not a dead harness.
TEST_F(FormFieldReconcilePanel, EditableFieldEmitsOncePerPathUnderTheSameDrive) {
    show(false);
    EXPECT_TRUE(color_->isEnabled()) << kR;
    driveAllThreePaths();
    EXPECT_EQ(emissions_, 3) << kR << "harness: the drive did not reach the slots";
    EXPECT_EQ(pickerCalls_, 1) << kR;
}

// ─── NeedAppearances fail-safe in the one setter ────────────────────────────

class FormFieldReconcileNeedAppearances : public BlankDoc {
protected:
    // Author a /RV (rich value) nested deeper than MuPDF 1.26.10's XML parser
    // accepts ("syntax error: too deep xml element nesting"), and drop the
    // widget's /AP, as a loaded field may have none. MuPDF's Tx appearance
    // writer renders /RV through its HTML engine, which then throws;
    // pdf_update_appearance swallows that ("cannot create appearance stream")
    // and the widget is left with no /AP at all.
    void makeAppearanceUngeneratable() {
        rv_.clear();
        for (int i = 0; i < 20000; ++i) rv_ += "<b>";
        rv_ += "x";
        pdf_obj* obj = firstWidgetObj(ctx_.get(), page_.get());
        ASSERT_NE(obj, nullptr);
        const char* rv = rv_.c_str();
        ctx_->execute([obj, rv](fz_context* c) {
            pdf_dict_put_text_string(c, obj, PDF_NAME(RV), rv);
            pdf_dict_del(c, obj, PDF_NAME(AP));
        });
    }

    bool widgetHasApN() {
        pdf_obj* obj = firstWidgetObj(ctx_.get(), page_.get());
        if (!obj) return false;
        int has = 0;
        int* out = &has;
        ctx_->execute([obj, out](fz_context* c) {
            *out = pdf_dict_getp(c, obj, "AP/N") ? 1 : 0;
        });
        return has != 0;
    }

    std::string rv_;
};

TEST_F(FormFieldReconcileNeedAppearances, SetterRaisesNeedAppearancesWhenTheAppearanceCannotBeGenerated) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        QRectF(60, 60, 300, 40), "na"),
              0);
    ASSERT_FALSE(manager_->needAppearances(doc_.get())) << "harness: flag already set";
    makeAppearanceUngeneratable();

    EXPECT_TRUE(manager_->setFieldDefaultAppearance(page_.get(), 0, "TiRo", 12.0f,
                                                    QColor(Qt::black)))
        << kR << "a valid request was refused";
    ASSERT_FALSE(widgetHasApN())
        << "harness: MuPDF generated an appearance, so this input no longer "
           "exercises the fail-safe";
    EXPECT_TRUE(manager_->needAppearances(doc_.get()))
        << kR << "the field has no appearance and /NeedAppearances is not "
                 "true: the setter's fail-safe did not fire";
}

// Control: an appearance that CAN be generated does not raise the flag, so
// the fail-safe is conditional, not unconditional.
TEST_F(FormFieldReconcileNeedAppearances, SetterLeavesTheFlagAloneWhenTheAppearanceIsGenerated) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        QRectF(60, 60, 300, 40), "na"),
              0);
    ASSERT_FALSE(manager_->needAppearances(doc_.get())) << "harness";
    EXPECT_TRUE(manager_->setFieldDefaultAppearance(page_.get(), 0, "TiRo", 12.0f,
                                                    QColor(Qt::black)));
    EXPECT_TRUE(widgetHasApN()) << kR;
    EXPECT_FALSE(manager_->needAppearances(doc_.get())) << kR;
}

// ─── Decision 1: the UndoManager hazard that motivates never-merge ──────────

class CountingMergeCommand : public pdfe::commands::Command {
public:
    explicit CountingMergeCommand(int* executes) : executes_(executes) {}
    void execute() override { ++*executes_; }
    void undo() override {}
    [[nodiscard]] std::string description() const override { return "probe"; }
    bool mergeWith(const pdfe::commands::Command*) override { return false; }  // declines
    [[nodiscard]] int mergeId() const override { return 4242; }

private:
    int* executes_;
};

TEST_F(FormFieldReconcileCommand, UndoManagerExecutesADeclinedMergeCandidateTwice) {
    // Why SetFieldAppearanceCommand has mergeId 0: when a candidate shares the
    // top command's mergeId and the merge is declined, UndoManager::execute
    // has already run candidate->execute() once and runs it AGAIN. A command
    // that takes its undo snapshot in execute() would then snapshot its own
    // result. If UndoManager is ever changed so this no longer happens, this
    // test goes red and decision 1's second reason should be revisited.
    int topExecutes = 0;
    int candidateExecutes = 0;
    undo_->execute(std::make_unique<CountingMergeCommand>(&topExecutes));
    undo_->execute(std::make_unique<CountingMergeCommand>(&candidateExecutes));
    EXPECT_EQ(topExecutes, 1);
    EXPECT_EQ(candidateExecutes, 2)
        << kR << "a declined merge candidate was not executed twice";

    // The one /DA command opts out of that path entirely.
    SetFieldAppearanceCommand cmd(manager_.get(), page_.get(), 0, "TiRo", 12.0f,
                                  QColor(Qt::black));
    EXPECT_EQ(cmd.mergeId(), 0) << kR;
}

// ─── Decision 8: no uncapped raw /DA copy on FormFieldData ──────────────────

template <typename T>
concept HasUncappedRawDaCopy = requires(T d) { d.defaultAppearance; };

TEST(FormFieldReconcileData, FormFieldDataHasNoUncappedRawDaCopy) {
    // master's FormFieldData::defaultAppearance was an uncapped copy of the
    // file's /DA, made for every widget on every overlay repaint. The one
    // /DA string is defaultAppearanceString, capped at
    // kMaxCapturedDefaultAppearanceBytes. Re-adding the member fails here.
    static_assert(!HasUncappedRawDaCopy<FormFieldData>,
                  "FormFieldData regained an uncapped raw /DA copy");
    EXPECT_FALSE(HasUncappedRawDaCopy<FormFieldData>) << kR;
    EXPECT_EQ(pdfe::forms::kMaxCapturedDefaultAppearanceBytes, 4096u) << kR;
}

}  // namespace
