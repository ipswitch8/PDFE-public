// -*- coding: utf-8 -*-
//
// form-field-bugs pipeline, phase-3 — FormTool overlay render oracles.
//
// Two product defects are pinned here, both in FormTool::onPaintOverlay:
//
//   Bug 1 — placed form fields are INVISIBLE at rest. Before the fix,
//   onPaintOverlay drew an overlay ONLY for the single SELECTED widget
//   (selectedWidget_ >= 0, else early-return), so every UNSELECTED field got
//   no overlay; a freshly placed / empty field whose MuPDF /AP is blank or
//   transparent rendered nothing at all. The fix draws a light outline + tint
//   for EVERY widget on the page, independent of selection.
//
//   Bug 2 — text typed into a form field is INVISIBLE until defocus. The
//   on-screen glyphs come from MuPDF's /AP, which only regenerates in
//   commitEdit(); during an edit, editBuffer_ changed per keystroke but
//   nothing painted it. The fix renders the live editBuffer_ over the field
//   rect each repaint.
//
// ORACLE DESIGN (follows test_form_tool_document_closing.cpp and
// test_form_appearance_render.cpp): construct a real FormTool over a real
// fixture page, paint its overlay onto a QImage, and assert on the ACTUAL
// pixels produced — not on whether a method was called.
//
//   * At-rest: paint with NOTHING selected and assert each widget rect the
//     product reports (getFormFields) carries overlay ink. Uses the
//     all-field-types fixture (text, checkbox, combobox, listbox, multiline
//     text, signature) so the type-agnostic fix is exercised across every
//     type, plus the radio-group fixture for RadioButton. Deleting the
//     at-rest enumeration loop reddens this: an unselected field paints
//     nothing.
//   * Live-typing: select a Text field, type into it WITHOUT committing, and
//     assert the field interior gains dark ink that was not there while the
//     edit buffer was empty. Deleting the live-text drawText reddens this:
//     the typed characters never reach the screen before commit.

#include <gtest/gtest.h>

#include <QFont>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRectF>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"

using pdfe::commands::UndoManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;

namespace {

constexpr float kZoom = 2.0f;
constexpr QPointF kOrigin{0.0, 0.0};

// Convert a widget's page-space (fitz, Y-down) rect into the SCREEN rect
// FormTool::onPaintOverlay draws it at, for the same zoom/origin.
QRect screenRectFor(const QRectF& fitzRect) {
    const double x = kOrigin.x() + fitzRect.x() * kZoom;
    const double y = kOrigin.y() + fitzRect.y() * kZoom;
    const double w = fitzRect.width() * kZoom;
    const double h = fitzRect.height() * kZoom;
    return QRect(static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(w), static_cast<int>(h));
}

int luminance(QRgb c) {
    return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
}

// Fraction of pixels inside `box` that are non-transparent (any overlay ink).
double overlayFractionIn(const QImage& img, const QRect& box) {
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long lit = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (qAlpha(img.pixel(x, y)) != 0) ++lit;
    return static_cast<double>(lit) /
           (static_cast<double>(r.width()) * r.height());
}

// Fraction of pixels inside `box` that carry DARK ink (over an opaque white
// base) — used to detect rendered text as distinct from the light tint.
double darkFractionIn(const QImage& img, const QRect& box) {
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long dark = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (luminance(img.pixel(x, y)) < 120) ++dark;
    return static_cast<double>(dark) /
           (static_cast<double>(r.width()) * r.height());
}

class FormToolOverlayRenderTest : public ::testing::Test {
protected:
    void SetUp() override { ctx_ = std::make_shared<MuPdfContext>(); }

    std::filesystem::path fixture(const char* name) const {
        return std::filesystem::path(TEST_FIXTURES_DIR) / name;
    }

    std::shared_ptr<MuPdfContext> ctx_;
};

// ── Bug 1: every field is visible at rest, for every field type ─────────────
//
// Paint with nothing selected and assert each reported widget rect carries
// overlay ink. The fixture holds one of each major field type; this therefore
// exercises Text, Checkbox, ComboBox, ListBox, multiline Text and Signature in
// a single assertion sweep.
TEST_F(FormToolOverlayRenderTest, EveryFieldTypeIsOutlinedAtRestUnselected) {
    const auto fx = fixture("phase3_form_all_field_types.pdf");
    ASSERT_TRUE(std::filesystem::exists(fx)) << "fixture missing: " << fx;

    auto doc = Document::open(ctx_, fx);
    ASSERT_NE(doc, nullptr);
    auto pageOwner = doc->page(0);
    ASSERT_NE(pageOwner, nullptr);

    FormManager mgr(ctx_);
    const std::vector<FormFieldData> fields = mgr.getFormFields(pageOwner.get());
    ASSERT_GE(fields.size(), 5u)
        << "the all-field-types fixture must expose its widgets";

    UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(pageOwner.get());
    ASSERT_LT(tool.selectedFieldIndex(), 0)
        << "precondition: nothing is selected, so any ink is the AT-REST cue, "
           "not a selection highlight";

    QImage canvas(700, 950, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        tool.onPaintOverlay(painter, 0, kZoom, kOrigin, 842.0f);
    }

    // Every field, regardless of type, must carry overlay ink in its OWN rect.
    for (size_t i = 0; i < fields.size(); ++i) {
        const QRect box = screenRectFor(fields[i].rect);
        // Inset so a neighbouring field's border cannot be mistaken for this
        // field's own ink (the fixture lays the widgets out well apart).
        const QRect interior = box.adjusted(3, 3, -3, -3);
        ASSERT_FALSE(interior.isEmpty())
            << "field " << i << " (" << fields[i].fieldName
            << ") has a degenerate rect";
        EXPECT_GT(overlayFractionIn(canvas, interior), 0.02)
            << "field " << i << " '" << fields[i].fieldName << "' (type "
            << static_cast<int>(fields[i].type)
            << ") drew NO at-rest overlay while unselected — it is invisible "
               "until clicked (bug 1).";
    }
}

// RadioButton is not present in the all-field-types fixture; cover it from the
// dedicated radio-group fixture so the audited type set is complete.
TEST_F(FormToolOverlayRenderTest, RadioButtonIsOutlinedAtRestUnselected) {
    const auto fx = fixture("form_radio_group.pdf");
    ASSERT_TRUE(std::filesystem::exists(fx)) << "fixture missing: " << fx;

    auto doc = Document::open(ctx_, fx);
    ASSERT_NE(doc, nullptr);
    auto pageOwner = doc->page(0);
    ASSERT_NE(pageOwner, nullptr);

    FormManager mgr(ctx_);
    const std::vector<FormFieldData> fields = mgr.getFormFields(pageOwner.get());
    ASSERT_FALSE(fields.empty()) << "radio fixture must expose widgets";

    UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(pageOwner.get());

    QImage canvas(700, 950, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        tool.onPaintOverlay(painter, 0, kZoom, kOrigin, 842.0f);
    }

    int radioChecked = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type != FormFieldType::RadioButton) continue;
        ++radioChecked;
        const QRect interior = screenRectFor(fields[i].rect).adjusted(2, 2, -2, -2);
        if (interior.isEmpty()) continue;
        EXPECT_GT(overlayFractionIn(canvas, interior), 0.02)
            << "radio widget " << i << " drew no at-rest overlay (bug 1).";
    }
    EXPECT_GT(radioChecked, 0)
        << "fixture was expected to contain at least one RadioButton widget";
}

// The selected field must still be drawn (and remain distinct) — the at-rest
// pass must not have REPLACED the selection highlight.
TEST_F(FormToolOverlayRenderTest, SelectedFieldStillDrawnAlongsideAtRestPass) {
    const auto fx = fixture("phase3_form_all_field_types.pdf");
    ASSERT_TRUE(std::filesystem::exists(fx)) << "fixture missing: " << fx;

    auto doc = Document::open(ctx_, fx);
    ASSERT_NE(doc, nullptr);
    auto pageOwner = doc->page(0);
    ASSERT_NE(pageOwner, nullptr);

    FormManager mgr(ctx_);
    const std::vector<FormFieldData> fields = mgr.getFormFields(pageOwner.get());
    ASSERT_FALSE(fields.empty());

    UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(pageOwner.get());

    // Select the first field by hit-testing its centre through the real press
    // path (event pointer is unused for the select path, so nullptr is safe).
    const QRectF fr = fields[0].rect;
    const QPointF centre(fr.center());
    tool.onMousePress(nullptr, 0, centre);
    ASSERT_GE(tool.selectedFieldIndex(), 0)
        << "hit-test must select the field under the click";

    QImage canvas(700, 950, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        tool.onPaintOverlay(painter, 0, kZoom, kOrigin, 842.0f);
    }

    const QRect selBox = screenRectFor(fields[tool.selectedFieldIndex()].rect)
                             .adjusted(3, 3, -3, -3);
    EXPECT_GT(overlayFractionIn(canvas, selBox), 0.02)
        << "the selected field must still paint its highlight.";
}

// ── Bug 2: typed text renders live, before the edit is committed ────────────
TEST_F(FormToolOverlayRenderTest, TypedTextRendersLiveBeforeCommit) {
    const auto fx = fixture("test_form.pdf");
    ASSERT_TRUE(std::filesystem::exists(fx)) << "fixture missing: " << fx;

    auto doc = Document::open(ctx_, fx);
    ASSERT_NE(doc, nullptr);
    auto pageOwner = doc->page(0);
    ASSERT_NE(pageOwner, nullptr);

    FormManager mgr(ctx_);
    const std::vector<FormFieldData> fields = mgr.getFormFields(pageOwner.get());

    // Find an empty, editable Text field.
    int textIdx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == FormFieldType::Text && !fields[i].isReadOnly &&
            fields[i].value.empty()) {
            textIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "fixture must contain an empty editable Text field";

    const QRectF fr = fields[textIdx].rect;
    const QRect interior = screenRectFor(fr).adjusted(3, 3, -3, -3);
    ASSERT_FALSE(interior.isEmpty());

    UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(pageOwner.get());

    // Enter edit mode on the text field via the real press path.
    tool.onMousePress(nullptr, 0, fr.center());
    ASSERT_EQ(tool.selectedFieldIndex(), textIdx)
        << "click must select the targeted text field";
    ASSERT_TRUE(tool.isEditingField())
        << "clicking an editable text field must start editing";
    ASSERT_TRUE(tool.editBufferForTesting().empty())
        << "precondition: the edit buffer starts empty for an empty field";

    auto paintOverWhite = [&]() -> QImage {
        QImage img(700, 300, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QPainter painter(&img);
        tool.onPaintOverlay(painter, 0, kZoom, kOrigin, 792.0f);
        return img;
    };

    // While the buffer is empty, the interior is a clean edit surface: no
    // rendered glyphs (at most the thin caret line).
    const double darkEmpty = darkFractionIn(paintOverWhite(), interior);

    // Type a run of printable characters WITHOUT committing.
    const std::string typed = "LIVE-FORM-TEXT-0123456789";
    for (char ch : typed) {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier,
                     QString(QChar::fromLatin1(ch)));
        tool.onKeyPress(&ev);
    }
    ASSERT_EQ(tool.editBufferForTesting(), typed)
        << "the typed characters must land in the edit buffer";
    ASSERT_TRUE(tool.isEditingField())
        << "the edit must NOT have been committed by typing";

    const double darkTyped = darkFractionIn(paintOverWhite(), interior);

    EXPECT_GT(darkTyped, darkEmpty + 0.01)
        << "typed text must appear in the field BEFORE commit: dark-ink "
           "fraction went from " << darkEmpty << " (empty buffer) to "
        << darkTyped << " (after typing) — if these are equal the live "
           "editBuffer_ is not being painted (bug 2).";
    EXPECT_GT(darkTyped, 0.01)
        << "the live edit buffer must rasterize visible glyphs in the field "
           "interior while editing (measured " << darkTyped << ").";
}

}  // namespace
