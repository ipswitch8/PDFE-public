// -*- coding: utf-8 -*-
//
// A1-63 (meta-audit-2026-07 status register, docs/audit/oracle-audit-ui.md:311)
// — "no form test renders a field appearance".
//
// WHAT WAS MISSING
// ----------------
// Every form test in this suite reads /V, or (in test_form_appearance_streams)
// the raw /AP /N BYTES. Neither sees the page. A widget whose appearance
// stream is regenerated but placed at the wrong coordinates, drawn with a
// zero-size font, clipped away by its own /BBox, or painted in white-on-white
// passes both of those oracles and ships a form that LOOKS empty — the exact
// "my form saved empty" complaint /AP regeneration exists to prevent.
//
// WHAT THIS FILE ASSERTS
// ----------------------
// The field's value is VISIBLY DRAWN, in the widget's own rectangle:
//   1. the widget rect the product reports matches the AUTHORED /Rect of
//      tests/fixtures/test_form.pdf, mapped into render space;
//   2. that rectangle carries NO ink before the field is filled;
//   3. after the fill it carries real ink;
//   4. the new ink is CONFINED to that rectangle — nothing else on the page
//      changed;
//   5. the ink survives save + reopen (an /AP written to memory but not to
//      disk renders on the second open as an empty box);
//   6. clearing the value removes the ink again (the appearance is
//      regenerated on EVERY write, not just the first).
//
// MUTATIONS KILLED
//   * delete `pdf_update_annot(ctx, widget)` from FormManager::setFieldValue
//     -> no ink ever appears (3) and nothing survives the round trip (5).
//   * regenerate the appearance but leave the old one in /AP (a "write once"
//     cache) -> (6) fails: the cleared field still shows the old text.
//   * emit the appearance at page coordinates instead of /BBox-local ones,
//     or drop the widget's /Matrix -> (4) fails: ink lands outside the rect.
//   * y-flip regression in FormManager's rect read (returning raw PDF /Rect
//     instead of the page-transformed rect FormTool.cpp:282 relies on)
//     -> (1) fails, and with it the form-field selection highlight and
//     hit-testing in the UI.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QRectF>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;

namespace {

// ── AUTHORED ground truth, read from the committed fixture bytes, never
// from the product. tests/fixtures/test_form.pdf, page 0:
//     MediaBox  [0 0 612 792], /Rotate absent
//     FirstName /Rect [180 690 380 710]  (PDF user space, Y-up)
// Mapped into MuPDF page/render space (Y-down, origin top-left) that is
//     x 180..380,  y (792-710)..(792-690) = 82..102
// which is what FormFieldData::rect must report — FormTool.cpp:282 converts
// it to screen coordinates with "both Y-down, no flip needed".
constexpr double kPageHeightPt = 792.0;
constexpr double kFieldPdfX0 = 180.0;
constexpr double kFieldPdfY0 = 690.0;
constexpr double kFieldPdfX1 = 380.0;
constexpr double kFieldPdfY1 = 710.0;
const char* const kFieldName = "FirstName";

// Long enough that the drawn glyphs cover a measurable share of the 200x20pt
// widget box, short enough to fit inside it at the field's authored 10pt /DA.
const char* const kFilledValue = "PDFE-APPEARANCE-INK-0123456789";

constexpr float kScale = 3.0f;

// Expected widget rect in render space, in POINTS (multiply by kScale for
// pixels).
QRectF expectedFitzRect() {
    return QRectF(kFieldPdfX0,
                  kPageHeightPt - kFieldPdfY1,
                  kFieldPdfX1 - kFieldPdfX0,
                  kFieldPdfY1 - kFieldPdfY0);
}

// Rasterize a page over opaque white.
//
// Page::render() goes through fz_new_pixmap_from_page(..., alpha=1) and MuPDF
// clears an alpha pixmap to TRANSPARENT BLACK before compositing. Reading the
// raw RGB would report every untouched pixel as black and make "is there ink
// here" meaningless, so flatten onto white first.
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

// Fraction of pixels inside `box` that carry ink.
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

// Count pixels that differ between two rasters, split by whether they fall
// inside `box`. `inside`/`outside` are absolute pixel counts.
void diffSplitByBox(const QImage& before, const QImage& after,
                    const QRect& box, long& inside, long& outside) {
    inside = 0;
    outside = 0;
    if (before.isNull() || after.isNull()) return;
    if (before.size() != after.size()) return;
    for (int y = 0; y < before.height(); ++y) {
        const QRgb* rb = reinterpret_cast<const QRgb*>(before.constScanLine(y));
        const QRgb* ra = reinterpret_cast<const QRgb*>(after.constScanLine(y));
        for (int x = 0; x < before.width(); ++x) {
            if (std::abs(luminance(ra[x]) - luminance(rb[x])) <= 8) continue;
            if (box.contains(x, y)) ++inside;
            else ++outside;
        }
    }
}

}  // namespace

class FormAppearanceRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "committed fixture missing: " << fixture_;
        tmpDir_ = std::filesystem::temp_directory_path() /
                  "pdfe_form_appearance_render";
        std::filesystem::create_directories(tmpDir_);
    }

    // Locate the authored field and return its widget index, asserting the
    // reported rect against the authored geometry on the way through.
    int locateField(FormManager& mgr, Page* page) {
        auto fields = mgr.getFormFields(page);
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].fieldName != kFieldName) continue;
            EXPECT_EQ(fields[i].type, FormFieldType::Text);
            const QRectF expected = expectedFitzRect();
            EXPECT_NEAR(fields[i].rect.x(), expected.x(), 1.0)
                << "widget rect X drifted from the authored /Rect.";
            EXPECT_NEAR(fields[i].rect.y(), expected.y(), 1.0)
                << "widget rect Y must be the PAGE-TRANSFORMED (Y-down) "
                   "coordinate FormTool.cpp:282 relies on. Reporting the raw "
                   "PDF /Rect Y (" << kFieldPdfY0 << ") here would put the "
                   "selection highlight and every hit test "
                << (kFieldPdfY0 - expected.y()) << "pt off.";
            EXPECT_NEAR(fields[i].rect.width(), expected.width(), 1.0);
            EXPECT_NEAR(fields[i].rect.height(), expected.height(), 1.0);
            return static_cast<int>(i);
        }
        return -1;
    }

    // The widget box in image pixels, grown by a 2px margin so anti-aliased
    // edge pixels are not mistaken for ink escaping the field.
    static QRect widgetPixelBox() {
        const QRectF r = expectedFitzRect();
        return QRect(static_cast<int>(r.left() * kScale) - 2,
                     static_cast<int>(r.top() * kScale) - 2,
                     static_cast<int>(r.width() * kScale) + 4,
                     static_cast<int>(r.height() * kScale) + 4);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tmpDir_;
};

// (1)(2)(3)(4) — the filled value is drawn, in the widget rect, and nowhere
// else on the page.
TEST_F(FormAppearanceRenderTest, FilledValueIsDrawnInsideTheWidgetRect) {
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    const QRect box = widgetPixelBox();

    // Baseline raster of the pristine, unfilled form.
    QImage before = renderOverWhite(doc.get(), 0, kScale);
    ASSERT_FALSE(before.isNull()) << "page did not rasterize";
    ASSERT_TRUE(box.intersects(before.rect()))
        << "the authored widget box falls outside the rendered page";

    const double inkBefore = inkFractionIn(before, box);
    // GROUND TRUTH CORRECTION (F17): the authored widget is NOT bare. Its
    // object in test_form.pdf carries `/MK << /BC [0 0 0] /BG [1 1 1] >>`
    // plus `/BS << /S /S /W 1 >>`, i.e. a 1pt black border, and an /AP that
    // draws it. So the pristine box legitimately contains border ink (~10%
    // of the box area at this scale) and "blank box" was never the fixture's
    // truth. What IS true is that no VALUE is drawn: /V is empty. The oracle
    // therefore measures the box INTERIOR, inset past the border, which must
    // start blank and must gain ink once the value is set.
    const QRect interior = box.adjusted(6, 6, -6, -6);
    ASSERT_FALSE(interior.isEmpty());
    const double interiorBefore = inkFractionIn(before, interior);
    EXPECT_LT(interiorBefore, 0.01)
        << "the authored fixture leaves " << kFieldName
        << " with an EMPTY /V, so the interior of its box (inside the authored "
           "1pt /MK border) must start blank; measured ink fraction "
        << interiorBefore << " (whole box including border: " << inkBefore
        << ")";

    // Fill the field through the production API.
    {
        auto pagePtr = doc->page(0);
        ASSERT_NE(pagePtr, nullptr);
        FormManager mgr(ctx_);
        const int idx = locateField(mgr, pagePtr.get());
        ASSERT_GE(idx, 0) << "fixture must contain the authored " << kFieldName
                          << " text field";
        mgr.setFieldValue(pagePtr.get(), idx, kFilledValue);
        ASSERT_EQ(mgr.getFormField(pagePtr.get(), idx).value,
                  std::string(kFilledValue))
            << "precondition: the value itself must be stored";
    }

    QImage after = renderOverWhite(doc.get(), 0, kScale);
    ASSERT_FALSE(after.isNull());

    // (3) The value is VISIBLE.
    const double inkAfter = inkFractionIn(after, box);
    EXPECT_GT(inkAfter, inkBefore)
        << "filling the field must add ink inside its own rectangle.";
    EXPECT_GT(inkFractionIn(after, interior), 0.01)
        << "the filled value must rasterize INSIDE the border, not merely "
           "redraw the border; interior ink went from " << interiorBefore
        << " to " << inkFractionIn(after, interior);
    EXPECT_GT(inkAfter, 0.01)
        << "the filled value must actually rasterize inside the widget box "
           "(measured ink fraction " << inkAfter
        << "). /V and the /AP bytes can both be correct while nothing "
           "reaches the page.";

    // (4) And ONLY there.
    long inside = 0, outside = 0;
    diffSplitByBox(before, after, box, inside, outside);
    EXPECT_GT(inside, 100)
        << "expected a substantial number of changed pixels inside the widget "
           "rect; got " << inside;
    // A misplaced appearance stream moves the ENTIRE drawn string outside the
    // rect — thousands of pixels. The small allowance below only absorbs
    // anti-aliasing on the box edge, and is two orders of magnitude below
    // what a real misplacement produces.
    EXPECT_LE(outside, 20)
        << "filling a form field changed " << outside
        << " pixels OUTSIDE its own rectangle — the appearance stream is "
           "being placed in the wrong coordinate space.";
}

// (5) — the drawn appearance survives save + reopen.
TEST_F(FormAppearanceRenderTest, DrawnValueSurvivesSaveAndReopen) {
    const auto out = tmpDir_ / "form_appearance_filled.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    QImage pristine;
    {
        auto doc = Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        pristine = renderOverWhite(doc.get(), 0, kScale);
        ASSERT_FALSE(pristine.isNull());

        auto pagePtr = doc->page(0);
        ASSERT_NE(pagePtr, nullptr);
        FormManager mgr(ctx_);
        const int idx = locateField(mgr, pagePtr.get());
        ASSERT_GE(idx, 0);
        mgr.setFieldValue(pagePtr.get(), idx, kFilledValue);
        pagePtr.reset();  // release the page before the full-rewrite save
        doc->saveAs(out);
    }

    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    QImage reloaded = renderOverWhite(reopened.get(), 0, kScale);
    ASSERT_FALSE(reloaded.isNull());

    const QRect box = widgetPixelBox();
    EXPECT_GT(inkFractionIn(reloaded, box), 0.01)
        << "the field's value must still be VISIBLE after save + reopen. An "
           "appearance regenerated in memory but not serialised renders as an "
           "empty box on the next open — the classic 'my form saved empty'.";

    long inside = 0, outside = 0;
    diffSplitByBox(pristine, reloaded, box, inside, outside);
    EXPECT_GT(inside, 100)
        << "the reopened page must differ from the pristine one inside the "
           "widget rect.";
    // This diff spans a save + reopen, so a handful of unrelated pixels can
    // move as MuPDF serialises appearance streams it synthesised for the
    // OTHER widgets. Requiring the change to be dominated by the widget rect
    // still catches an appearance written at the wrong coordinates.
    EXPECT_LE(outside, inside / 10)
        << "the saved appearance leaked " << outside
        << " changed pixels outside the widget rect (vs " << inside
        << " inside).";
}

// (6) — the appearance is regenerated on EVERY write: clearing the value must
// remove the ink, not leave the previous render cached in /AP.
TEST_F(FormAppearanceRenderTest, ClearingValueRemovesTheDrawnInk) {
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    const QRect box = widgetPixelBox();
    QImage pristine = renderOverWhite(doc.get(), 0, kScale);
    ASSERT_FALSE(pristine.isNull());
    const double inkPristine = inkFractionIn(pristine, box);

    auto pagePtr = doc->page(0);
    ASSERT_NE(pagePtr, nullptr);
    FormManager mgr(ctx_);
    const int idx = locateField(mgr, pagePtr.get());
    ASSERT_GE(idx, 0);

    mgr.setFieldValue(pagePtr.get(), idx, kFilledValue);
    const double inkFilled = inkFractionIn(
        renderOverWhite(doc.get(), 0, kScale), box);
    ASSERT_GT(inkFilled, 0.01) << "precondition: the value must be drawn first";

    mgr.setFieldValue(pagePtr.get(), idx, "");
    const double inkCleared = inkFractionIn(
        renderOverWhite(doc.get(), 0, kScale), box);

    // The box's baseline ink is the authored 1pt /MK border (see the ground
    // truth note in FilledValueIsDrawnInsideTheWidgetRect), so "cleared" means
    // "back to the pristine border-only render", not "near zero". Comparing to
    // the pristine measurement rather than to a fraction of the filled value
    // is also the stronger oracle: it fails on ANY residue, not just on more
    // than a quarter of it.
    EXPECT_NEAR(inkCleared, inkPristine, 0.005)
        << "a cleared field must render like the pristine empty one; measured "
        << inkCleared << " vs pristine " << inkPristine << " (filled was "
        << inkFilled << "). A stale appearance stream leaves the old text on "
           "screen while /V reads empty — the value the user sees and the "
           "value the document carries disagree.";
    EXPECT_LT(inkCleared, inkFilled)
        << "clearing must remove ink, not add it";
}
