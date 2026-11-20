// -*- coding: utf-8 -*-
//
// tests/test_form_textfield_model.cpp
//
// Pipeline form-textfield, phase 2: model tests for the GitHub #458 fix
// (form fields visible after placement). They complement the phase-1 probes in
// tests/test_form_textfield_state.cpp, which stay untouched.
//
//   FormFieldVisibilityModel  -- check box, radio button and push button get
//       the same /MK /BC, /MK /BG and /BS as text, combo and list fields, and
//       MuPDF 1.26.10's button writer (pdf_update_button_appearance) draws a
//       visible border and background from them; delete+undo keeps every
//       type's appearance; hand-built restore data gets the default; a
//       signature widget gets no /MK.
//   FormFieldLoadedAppearanceModel -- a field loaded from a file keeps its own
//       /MK and /BS through a value change, through delete+undo and through
//       save+reopen; a loaded field with no /MK is not given one.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker.
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
#include "support/FormDocumentBytes.h"
#include "forms/FormManager.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QRectF>

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
using pdfe::forms::FormFieldBorderAppearance;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;

namespace {

constexpr const char* kB1 =
    "GitHub #458 (form fields invisible after placement): ";

// ─── Raster helpers (same oracle as tests/test_form_textfield_state.cpp) ────

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

// Fraction of pixels in `box` that are clearly RED (a loaded field's border).
double redFractionIn(const QImage& img, const QRect& box) {
    if (img.isNull()) return 0.0;
    const QRect r = box.intersected(img.rect());
    if (r.width() <= 0 || r.height() <= 0) return 0.0;
    long red = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb c = row[x];
            if (qRed(c) > 150 && qGreen(c) < 100 && qBlue(c) < 100) ++red;
        }
    }
    return static_cast<double>(red) /
           (static_cast<double>(r.width()) * r.height());
}

double weakestEdgeInk(const QImage& img, const QRect& box, int band) {
    const QRect top(box.left(), box.top(), box.width(), band);
    const QRect bottom(box.left(), box.bottom() - band + 1, box.width(), band);
    const QRect left(box.left(), box.top(), band, box.height());
    const QRect right(box.right() - band + 1, box.top(), band, box.height());
    return std::min({inkFractionIn(img, top), inkFractionIn(img, bottom),
                     inkFractionIn(img, left), inkFractionIn(img, right)});
}

// ─── MuPDF readback / authoring (POD only inside the lambdas) ───────────────

struct MkBsReadback {
    bool hasMk = false;
    int bcLen = 0;
    float bc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int bgLen = 0;
    float bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool hasBs = false;
    float bsWidth = 0.0f;
    char bsStyle = '\0';
};

pdf_obj* firstWidgetObj(MuPdfContext* ctx, const Page* page) {
    return static_cast<pdf_obj*>(FormManager::firstWidgetObjForPageSeam(ctx, page));
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
            pdf_obj* bc = pdf_dict_get(c, mk, PDF_NAME(BC));
            o->bcLen = pdf_array_len(c, bc);
            for (int i = 0; i < o->bcLen && i < 4; ++i)
                o->bc[i] = pdf_array_get_real(c, bc, i);
            pdf_obj* bg = pdf_dict_get(c, mk, PDF_NAME(BG));
            o->bgLen = pdf_array_len(c, bg);
            for (int i = 0; i < o->bgLen && i < 4; ++i)
                o->bg[i] = pdf_array_get_real(c, bg, i);
        }
        pdf_obj* bs = pdf_dict_get(c, obj, PDF_NAME(BS));
        if (pdf_is_dict(c, bs)) {
            o->hasBs = true;
            pdf_obj* w = pdf_dict_get(c, bs, PDF_NAME(W));
            o->bsWidth = w ? pdf_to_real(c, w) : 1.0f;
            const char* s = pdf_to_name(c, pdf_dict_get(c, bs, PDF_NAME(S)));
            o->bsStyle = (s && s[0]) ? s[0] : '\0';
        }
    });
    return out;
}

// Make the first widget look like one authored by another tool: /MK with a
// red border and a yellow background, a 2pt dashed /BS -- or, with
// `withMk == false`, no /MK and no /BS at all. The stored appearance stream is
// dropped, so on reload MuPDF synthesises one from these entries.
void authorFirstWidgetLook(MuPdfContext* ctx, const Page* page, bool withMk) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, withMk](fz_context* c) {
        pdf_dict_del(c, obj, PDF_NAME(MK));
        pdf_dict_del(c, obj, PDF_NAME(BS));
        pdf_dict_del(c, obj, PDF_NAME(AP));
        if (withMk) {
            pdf_obj* mk = pdf_dict_put_dict(c, obj, PDF_NAME(MK), 2);
            pdf_obj* bc = pdf_dict_put_array(c, mk, PDF_NAME(BC), 3);
            pdf_array_push_real(c, bc, 1.0f);
            pdf_array_push_real(c, bc, 0.0f);
            pdf_array_push_real(c, bc, 0.0f);
            pdf_obj* bg = pdf_dict_put_array(c, mk, PDF_NAME(BG), 3);
            pdf_array_push_real(c, bg, 1.0f);
            pdf_array_push_real(c, bg, 1.0f);
            pdf_array_push_real(c, bg, 0.8f);
            pdf_obj* bs = pdf_dict_put_dict(c, obj, PDF_NAME(BS), 2);
            pdf_dict_put_real(c, bs, PDF_NAME(W), 2.0f);
            pdf_dict_put_name(c, bs, PDF_NAME(S), "D");
        }
    });
}

// ─── Fixture plumbing ────────────────────────────────────────────────────────

std::filesystem::path scratchDir() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pdfe_form_textfield_model";
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
        case FormFieldType::Checkbox: return "Checkbox";
        case FormFieldType::RadioButton: return "RadioButton";
        case FormFieldType::ComboBox: return "ComboBox";
        case FormFieldType::ListBox: return "ListBox";
        case FormFieldType::PushButton: return "PushButton";
        case FormFieldType::Signature: return "Signature";
        default: return "Other";
    }
}

std::string fieldTypeParamName(const ::testing::TestParamInfo<FormFieldType>& i) {
    return typeName(i.param);
}

// Text-like fields are wide; check boxes and radio buttons are square (MuPDF
// draws them in a min(w,h) square box that is then mapped onto /Rect).
const QRectF kWideRect(100.0, 100.0, 240.0, 40.0);
const QRectF kSquareRect(100.0, 100.0, 40.0, 40.0);
constexpr float kRenderScale = 3.0f;

QRectF rectFor(FormFieldType t) {
    return (t == FormFieldType::Checkbox || t == FormFieldType::RadioButton)
               ? kSquareRect
               : kWideRect;
}

QRect pixelBox(const QRectF& r, float scale) {
    return QRect(static_cast<int>(std::lround(r.left() * scale)),
                 static_cast<int>(std::lround(r.top() * scale)),
                 static_cast<int>(std::lround(r.width() * scale)),
                 static_cast<int>(std::lround(r.height() * scale)));
}

class BlankDocTest : public ::testing::Test {
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

    int createViaCommand(FormFieldType type, const QRectF& rect,
                         const std::string& name) {
        undo_->execute(std::make_unique<CreateFormFieldCommand>(
            manager_.get(), page_.get(), type, rect, name));
        const auto fields = manager_->getFormFields(page_.get());
        return fields.empty() ? -1 : static_cast<int>(fields.size()) - 1;
    }

    // The app default must be what the widget carries.
    void expectDefaultMkBs(FormFieldType type, const char* when) {
        const FormFieldBorderAppearance def = FormManager::defaultBorderAppearance(type);
        const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
        EXPECT_TRUE(mk.hasMk) << kB1 << typeName(type) << " " << when
                              << ": no /MK dictionary";
        EXPECT_EQ(mk.bcLen, 3) << kB1 << typeName(type) << " " << when
                               << ": /MK /BC is not an RGB colour";
        EXPECT_EQ(mk.bgLen, 3) << kB1 << typeName(type) << " " << when
                               << ": /MK /BG is not an RGB colour";
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(mk.bc[i], def.borderColor[i], 1e-4)
                << kB1 << typeName(type) << " " << when << ": /MK /BC[" << i << "]";
            EXPECT_NEAR(mk.bg[i], def.backgroundColor[i], 1e-4)
                << kB1 << typeName(type) << " " << when << ": /MK /BG[" << i << "]";
        }
        EXPECT_TRUE(mk.hasBs) << kB1 << typeName(type) << " " << when
                              << ": no /BS dictionary";
        EXPECT_FLOAT_EQ(mk.bsWidth, 1.0f) << kB1 << typeName(type) << " " << when
                                          << ": /BS /W is not 1";
        EXPECT_EQ(mk.bsStyle, 'S') << kB1 << typeName(type) << " " << when
                                   << ": /BS /S is not /S (solid)";
    }

    // Visible: a border on every side and a light shaded interior with no ink.
    void expectVisible(FormFieldType type, const QImage& img, const char* when) {
        ASSERT_FALSE(img.isNull()) << kB1 << "harness: page did not rasterize";
        const QRect box = pixelBox(rectFor(type), kRenderScale);
        if (type == FormFieldType::RadioButton) {
            // A circle inscribed in the square: ink at the four extreme points.
            const int cx = box.center().x();
            const int cy = box.center().y();
            const double top = inkFractionIn(img, QRect(cx - 2, box.top(), 5, 4));
            const double bottom =
                inkFractionIn(img, QRect(cx - 2, box.bottom() - 3, 5, 4));
            const double left = inkFractionIn(img, QRect(box.left(), cy - 2, 4, 5));
            const double right =
                inkFractionIn(img, QRect(box.right() - 3, cy - 2, 4, 5));
            const double ring = std::min({top, bottom, left, right});
            EXPECT_GT(ring, 0.40) << kB1 << "RadioButton " << when
                                  << ": the circle outline is missing (weakest "
                                     "extreme point ink "
                                  << ring << ")";
            const QRect centre(cx - 15, cy - 15, 31, 31);
            const double shade = shadeFractionIn(img, centre);
            EXPECT_GT(shade, 0.90) << kB1 << "RadioButton " << when
                                   << ": only " << shade
                                   << " of the circle centre is shaded";
            EXPECT_LT(inkFractionIn(img, centre), 0.02)
                << kB1 << "RadioButton " << when << ": dark fill in an Off radio";
            return;
        }
        const double ring = weakestEdgeInk(img, box, 4);
        EXPECT_GT(ring, 0.40) << kB1 << typeName(type) << " " << when
                              << ": weakest border side ink " << ring;
        // Push buttons draw a 2pt bevel inside the 1pt border: look past it.
        const int inset = (type == FormFieldType::PushButton) ? 12 : 9;
        const QRect interior = box.adjusted(inset, inset, -inset, -inset);
        const double shade = shadeFractionIn(img, interior);
        EXPECT_GT(shade, 0.90) << kB1 << typeName(type) << " " << when
                               << ": only " << shade << " of the interior is shaded";
        EXPECT_LT(inkFractionIn(img, interior), 0.02)
            << kB1 << typeName(type) << " " << when
            << ": the interior of an empty/Off field must stay light";
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path path_;
    std::unique_ptr<Document> doc_;
    std::shared_ptr<Page> page_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<UndoManager> undo_;
};

// ════════════════════════════════════════════════════════════════════════════
// Check box, radio button, push button: decided in phase 2 to get /MK + /BS.
// MuPDF 1.26.10 routes /FT /Btn widgets to pdf_update_button_appearance
// (pdf-appearance.c), whose draw_check_button / draw_radio_button /
// draw_push_button paint /MK /BG and the /MK /BC border at /BS /W.
// ════════════════════════════════════════════════════════════════════════════

class FormFieldVisibilityModel
    : public BlankDocTest,
      public ::testing::WithParamInterface<FormFieldType> {};

TEST_P(FormFieldVisibilityModel, CreatedFieldCarriesDefaultMkAndBs) {
    ASSERT_EQ(createViaCommand(GetParam(), rectFor(GetParam()), "vis_struct"), 0)
        << kB1 << "harness: field was not created";
    expectDefaultMkBs(GetParam(), "freshly created");
}

TEST_P(FormFieldVisibilityModel, CreatedFieldRendersBorderAndShadedInterior) {
    ASSERT_EQ(createViaCommand(GetParam(), rectFor(GetParam()), "vis_render"), 0)
        << kB1 << "harness: field was not created";
    expectVisible(GetParam(), renderOverWhite(doc_.get(), 0, kRenderScale),
                  "freshly created");
}

TEST_P(FormFieldVisibilityModel, FieldStaysVisibleAfterUndoRedoOfCreate) {
    ASSERT_EQ(createViaCommand(GetParam(), rectFor(GetParam()), "vis_redo"), 0)
        << kB1 << "harness: field was not created";
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << kB1 << "harness: undo did not remove the field";
    undo_->redo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << kB1 << "harness: redo did not restore the field";
    expectDefaultMkBs(GetParam(), "after undo+redo of create");
    expectVisible(GetParam(), renderOverWhite(doc_.get(), 0, kRenderScale),
                  "after undo+redo of create");
}

TEST_P(FormFieldVisibilityModel, FieldStaysVisibleAfterDeleteAndUndo) {
    ASSERT_EQ(createViaCommand(GetParam(), rectFor(GetParam()), "vis_delete"), 0)
        << kB1 << "harness: field was not created";
    undo_->execute(std::make_unique<DeleteFormFieldCommand>(
        manager_.get(), page_.get(), 0));
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << kB1 << "harness: delete did not remove the field";
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << kB1 << "harness: undo of delete did not restore the field";
    expectDefaultMkBs(GetParam(), "after delete+undo");
    expectVisible(GetParam(), renderOverWhite(doc_.get(), 0, kRenderScale),
                  "after delete+undo");
}

TEST_P(FormFieldVisibilityModel, HandBuiltRestoreDataGetsTheDefaultAppearance) {
    FormFieldData data;
    data.type = GetParam();
    data.rect = rectFor(GetParam());
    data.fieldName = "vis_handbuilt";
    ASSERT_FALSE(data.borderAppearanceCaptured)
        << "harness: a default-constructed FormFieldData must not claim a "
           "captured appearance";
    ASSERT_EQ(manager_->restoreFormField(page_.get(), data), 0)
        << kB1 << "harness: restoreFormField did not create the field";
    expectDefaultMkBs(GetParam(), "restored from hand-built data");
    expectVisible(GetParam(), renderOverWhite(doc_.get(), 0, kRenderScale),
                  "restored from hand-built data");
}

TEST_P(FormFieldVisibilityModel, DeletedFieldDataCapturesTheAppearance) {
    ASSERT_EQ(createViaCommand(GetParam(), rectFor(GetParam()), "vis_capture"), 0)
        << kB1 << "harness: field was not created";
    const FormFieldData data = manager_->deleteFormField(page_.get(), 0);
    const FormFieldBorderAppearance def =
        FormManager::defaultBorderAppearance(GetParam());
    EXPECT_TRUE(data.borderAppearanceCaptured) << kB1 << typeName(GetParam());
    EXPECT_EQ(data.borderAppearance.borderColorComponents, 3)
        << kB1 << typeName(GetParam());
    EXPECT_EQ(data.borderAppearance.backgroundColorComponents, 3)
        << kB1 << typeName(GetParam());
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(data.borderAppearance.borderColor[i], def.borderColor[i], 1e-4)
            << kB1 << typeName(GetParam()) << " BC[" << i << "]";
        EXPECT_NEAR(data.borderAppearance.backgroundColor[i],
                    def.backgroundColor[i], 1e-4)
            << kB1 << typeName(GetParam()) << " BG[" << i << "]";
    }
    EXPECT_TRUE(data.borderAppearance.hasBorderStyle) << kB1 << typeName(GetParam());
    EXPECT_FLOAT_EQ(data.borderAppearance.borderWidth, 1.0f)
        << kB1 << typeName(GetParam());
    EXPECT_EQ(data.borderAppearance.borderStyle, 'S') << kB1 << typeName(GetParam());
}

// Every type the form tool places, except Signature (see below).
INSTANTIATE_TEST_SUITE_P(AllPlaceableTypes, FormFieldVisibilityModel,
                         ::testing::Values(FormFieldType::Text,
                                           FormFieldType::Checkbox,
                                           FormFieldType::RadioButton,
                                           FormFieldType::ComboBox,
                                           FormFieldType::ListBox,
                                           FormFieldType::PushButton),
                         fieldTypeParamName);

// The default border must be ink and the default background must be shade
// under the same luminance bands the render oracle uses.
TEST(FormFieldVisibilityDefaults, DefaultColoursAreInkBorderAndLightShade) {
    const FormFieldBorderAppearance def =
        FormManager::defaultBorderAppearance(FormFieldType::Text);
    const auto lum = [](const std::array<float, 4>& c) {
        return 0.299 * c[0] * 255.0 + 0.587 * c[1] * 255.0 + 0.114 * c[2] * 255.0;
    };
    EXPECT_LT(lum(def.borderColor), 110.0)
        << kB1 << "the default border colour is too light to read as an outline";
    EXPECT_GT(lum(def.backgroundColor), 200.0)
        << kB1 << "the default background is too dark to read typed text over";
    EXPECT_LT(lum(def.backgroundColor), 250.0)
        << kB1 << "the default background is indistinguishable from paper";
}

// Signature: MuPDF draws an unsigned signature widget from its own display
// list (pdf_signature_appearance_unsigned) and ignores /MK, so the app writes
// none -- the field keeps MuPDF's "sign here" look.
class FormFieldSignatureModel : public BlankDocTest {};

TEST_F(FormFieldSignatureModel, SignatureFieldGetsNoMkOrBs) {
    ASSERT_EQ(createViaCommand(FormFieldType::Signature, kWideRect, "vis_sig"), 0)
        << kB1 << "harness: signature field was not created";
    const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
    EXPECT_FALSE(mk.hasMk) << kB1 << "Signature widget unexpectedly got /MK";
    EXPECT_FALSE(mk.hasBs) << kB1 << "Signature widget unexpectedly got /BS";
}

// ════════════════════════════════════════════════════════════════════════════
// Fields loaded from a file keep their own /MK and /BS.
// ════════════════════════════════════════════════════════════════════════════

class FormFieldLoadedAppearanceModel
    : public BlankDocTest,
      public ::testing::WithParamInterface<FormFieldType> {
protected:
    // Write a file whose single widget carries the look of another tool
    // (`withMk`) or no /MK at all, then reopen it as the document under test.
    void loadFieldAuthoredElsewhere(bool withMk) {
        ASSERT_EQ(manager_->createFormField(page_.get(), GetParam(), kWideRect,
                                            "loaded"),
                  0)
            << "harness: field was not created";
        authorFirstWidgetLook(ctx_.get(), page_.get(), withMk);
        saved_ = scratchDir() / (currentTestSlug() + "_authored.pdf");
        std::error_code ec;
        std::filesystem::remove(saved_, ec);
        undo_ = std::make_unique<UndoManager>();
        page_.reset();
        doc_->saveAs(saved_);
        doc_.reset();
        doc_ = Document::open(ctx_, saved_);
        ASSERT_NE(doc_, nullptr) << "harness: authored file did not reopen";
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr) << "harness: reopened page missing";
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
            << "harness: the authored field did not load";
    }

    void TearDown() override {
        BlankDocTest::TearDown();
        std::error_code ec;
        if (!saved_.empty()) std::filesystem::remove(saved_, ec);
    }

    void expectAuthoredLook(const char* when) {
        const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
        ASSERT_TRUE(mk.hasMk) << kB1 << typeName(GetParam()) << " " << when
                              << ": the loaded field lost its /MK";
        ASSERT_EQ(mk.bcLen, 3) << kB1 << when << ": /MK /BC changed length";
        ASSERT_EQ(mk.bgLen, 3) << kB1 << when << ": /MK /BG changed length";
        EXPECT_FLOAT_EQ(mk.bc[0], 1.0f) << kB1 << when << ": /MK /BC was replaced";
        EXPECT_FLOAT_EQ(mk.bc[1], 0.0f) << kB1 << when << ": /MK /BC was replaced";
        EXPECT_FLOAT_EQ(mk.bc[2], 0.0f) << kB1 << when << ": /MK /BC was replaced";
        EXPECT_FLOAT_EQ(mk.bg[0], 1.0f) << kB1 << when << ": /MK /BG was replaced";
        EXPECT_FLOAT_EQ(mk.bg[1], 1.0f) << kB1 << when << ": /MK /BG was replaced";
        EXPECT_NEAR(mk.bg[2], 0.8f, 1e-4) << kB1 << when << ": /MK /BG was replaced";
        EXPECT_TRUE(mk.hasBs) << kB1 << when << ": the loaded field lost its /BS";
        EXPECT_FLOAT_EQ(mk.bsWidth, 2.0f) << kB1 << when << ": /BS /W was replaced";
        EXPECT_EQ(mk.bsStyle, 'D') << kB1 << when << ": /BS /S was replaced";
        // And it renders with ITS border colour, not the app default.
        const QImage img = renderOverWhite(doc_.get(), 0, kRenderScale);
        const QRect box = pixelBox(kWideRect, kRenderScale);
        const QRect leftBand(box.left(), box.top() + 6, 5, box.height() - 12);
        EXPECT_GT(redFractionIn(img, leftBand), 0.30)
            << kB1 << typeName(GetParam()) << " " << when
            << ": the loaded field's red border is not what renders";
    }

    std::filesystem::path saved_;
};

TEST_P(FormFieldLoadedAppearanceModel, LoadedFieldKeepsItsOwnMkAndBs) {
    loadFieldAuthoredElsewhere(true);
    expectAuthoredLook("as loaded");
}

TEST_P(FormFieldLoadedAppearanceModel, LoadedFieldKeepsItsMkWhenItsValueChanges) {
    loadFieldAuthoredElsewhere(true);
    // setFieldValue regenerates the appearance stream (pdf_update_annot).
    static_cast<void>(manager_->setFieldValue(page_.get(), 0, "abc"));
    expectAuthoredLook("after a value change");
}

TEST_P(FormFieldLoadedAppearanceModel, LoadedFieldKeepsItsMkThroughDeleteAndUndo) {
    loadFieldAuthoredElsewhere(true);
    undo_->execute(std::make_unique<DeleteFormFieldCommand>(
        manager_.get(), page_.get(), 0));
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
        << "harness: delete did not remove the field";
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << "harness: undo of delete did not restore the field";
    expectAuthoredLook("after delete+undo");
}

TEST_P(FormFieldLoadedAppearanceModel, LoadedFieldWithoutMkIsNotGivenOne) {
    loadFieldAuthoredElsewhere(false);
    {
        const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
        ASSERT_FALSE(mk.hasMk) << "harness: the authored field should have no /MK";
        ASSERT_FALSE(mk.hasBs) << "harness: the authored field should have no /BS";
    }
    undo_->execute(std::make_unique<DeleteFormFieldCommand>(
        manager_.get(), page_.get(), 0));
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
        << "harness: undo of delete did not restore the field";
    const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
    EXPECT_FALSE(mk.hasMk)
        << kB1 << typeName(GetParam())
        << ": a loaded field without /MK was given one by delete+undo; the app "
           "must not restyle fields it did not create";
    EXPECT_FALSE(mk.hasBs)
        << kB1 << typeName(GetParam())
        << ": a loaded field without /BS was given one by delete+undo";
}

INSTANTIATE_TEST_SUITE_P(TextAndChoice, FormFieldLoadedAppearanceModel,
                         ::testing::Values(FormFieldType::Text,
                                           FormFieldType::ComboBox,
                                           FormFieldType::ListBox),
                         fieldTypeParamName);

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Pipeline form-textfield, phase 3: model tests for the GitHub #459 fix (typed
// text visible while editing a form text field). FormTool::onPaintOverlay now
// paints the field being edited -- opaque /MK /BG fill, editBuffer_ and the
// underlined IME preedit in the /DA font, size and colour, with MuPDF's
// padding and single-line / multiline / password / comb layouts -- and puts
// the caret at the QFontMetricsF advance of the text before it.
//
//   FormFieldEditPaintModel -- each layout and appearance input above, driven
//       through a real FormTool and read back from the painted pixels. The
//       field's /DA, /Ff, /MaxLen and /MK are authored directly in the widget
//       dictionary, as a field loaded from a file carries them.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker. Lambdas passed to MuPdfContext::execute capture and
// declare only trivially-destructible values (fz_try discipline).
// ════════════════════════════════════════════════════════════════════════════

#include "forms/FormTool.h"

#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>

namespace {

using pdfe::forms::FormTool;

constexpr const char* kB2 =
    "GitHub #459 (typed text invisible while editing a form text field): ";

// Ink statistics over `box`, skipping the columns [excludeX0, excludeX1] (the
// caret). A pixel is ink when its luminance is below `threshold`.
struct InkStats {
    long count = 0;
    int top = -1;
    int bottom = -1;
    int left = -1;
    int right = -1;
    double sumX = 0.0;
    long red = 0;
    long darkNotRed = 0;
    int height() const { return top < 0 ? 0 : bottom - top + 1; }
    double centroidX() const { return count ? sumX / count : -1.0; }
};

InkStats inkStats(const QImage& img, const QRect& box, int excludeX0 = -1,
                  int excludeX1 = -2, int threshold = 160) {
    InkStats s;
    const QRect r = box.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            if (x >= excludeX0 && x <= excludeX1) continue;
            const QRgb c = row[x];
            const bool isRed = qRed(c) > 150 && qGreen(c) < 100 && qBlue(c) < 100;
            if (isRed) ++s.red;
            if (luminance(c) < 110 && !isRed) ++s.darkNotRed;
            if (luminance(c) >= threshold) continue;
            ++s.count;
            s.sumX += x;
            if (s.top < 0) s.top = y;
            s.bottom = y;
            if (s.left < 0 || x < s.left) s.left = x;
            if (x > s.right) s.right = x;
        }
    }
    return s;
}

// Longest horizontal run of ink (luminance < 160) in any single row of `box`.
int longestInkRow(const QImage& img, const QRect& box) {
    const QRect r = box.intersected(img.rect());
    int best = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        int run = 0;
        for (int x = r.left(); x <= r.right(); ++x) {
            run = luminance(row[x]) < 160 ? run + 1 : 0;
            best = std::max(best, run);
        }
    }
    return best;
}

// The QFont the fix maps a "Helv" /DA to, at `px` pixels.
QFont helveticaPx(int px) {
    QFont font(QStringLiteral("Helvetica"));
    font.setStyleHint(QFont::Helvetica);
    font.setPixelSize(px);
    return font;
}

// Widget dictionary authoring (POD captures only).
void authorDa(MuPdfContext* ctx, const Page* page, const char* da) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, da](fz_context* c) {
        pdf_dict_put_text_string(c, obj, PDF_NAME(DA), da);
    });
}

void setFieldFlagBits(MuPdfContext* ctx, const Page* page, int bits, bool on) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, bits, on](fz_context* c) {
        const int ff = pdf_dict_get_int(c, obj, PDF_NAME(Ff));
        pdf_dict_put_int(c, obj, PDF_NAME(Ff), on ? (ff | bits) : (ff & ~bits));
    });
}

void setMaxLen(MuPdfContext* ctx, const Page* page, int maxLen) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, maxLen](fz_context* c) {
        pdf_dict_put_int(c, obj, PDF_NAME(MaxLen), maxLen);
    });
}

// Replace /MK with one whose /BG is the RGB colour given, or drop /MK.
void authorBackground(MuPdfContext* ctx, const Page* page, bool present,
                      float r, float g, float b) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, present, r, g, b](fz_context* c) {
        pdf_dict_del(c, obj, PDF_NAME(MK));
        if (!present) return;
        pdf_obj* mk = pdf_dict_put_dict(c, obj, PDF_NAME(MK), 1);
        pdf_obj* bg = pdf_dict_put_array(c, mk, PDF_NAME(BG), 3);
        pdf_array_push_real(c, bg, r);
        pdf_array_push_real(c, bg, g);
        pdf_array_push_real(c, bg, b);
    });
}

class FormFieldEditPaintModel : public BlankDocTest {
protected:
    void TearDown() override {
        tool_.reset();
        BlankDocTest::TearDown();
    }

    // Create a text field at `rect`, let `author` shape its dictionary, then
    // start editing it through a click, as the user does.
    template <typename Author>
    void beginEdit(const QRectF& rect, Author author) {
        rect_ = rect;
        ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                            rect, "edit_paint"),
                  0)
            << "harness: text field was not created";
        author();
        tool_ = std::make_unique<FormTool>(manager_.get(), nullptr, undo_.get());
        tool_->activate();
        tool_->setPage(page_.get());
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, rect.center());
        ASSERT_TRUE(tool_->isEditingField())
            << "harness: clicking the text field did not start an edit";
    }
    void beginEdit() {
        beginEdit(kWideRect, [] {});
    }

    void type(const QString& s) {
        for (const QChar ch : s) {
            QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
            tool_->onKeyPress(&ev);
        }
    }

    void key(int k) {
        QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
        tool_->onKeyPress(&ev);
    }

    QImage paint(float zoom, const QColor& canvasColor = QColor(Qt::white)) {
        QImage canvas(1200, 800, QImage::Format_ARGB32);
        canvas.fill(canvasColor);
        QPainter painter(&canvas);
        tool_->onPaintOverlay(painter, 0, zoom, QPointF(0, 0), 792.0f);
        painter.end();
        return canvas;
    }

    // The field interior on screen, inset past the 2px selection outline.
    QRect interior(float zoom) const {
        return pixelBox(rect_, zoom).adjusted(4, 4, -4, -4);
    }

    QRectF caret() const {
        QRectF r;
        EXPECT_TRUE(tool_->caretScreenRect(r))
            << kB2 << "harness: no caret was recorded for the active edit";
        return r;
    }

    // Text ink inside the field, the caret column excluded.
    InkStats textInk(const QImage& img, float zoom) {
        const int cx = static_cast<int>(std::floor(caret().left()));
        return inkStats(img, interior(zoom), cx - 3, cx + 3);
    }

    // Caret x after painting at `zoom`.
    double caretX(float zoom) {
        paint(zoom);
        return caret().left();
    }

    QRectF rect_;
    std::unique_ptr<FormTool> tool_;
};

// ─── Background ──────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, BackgroundIsAnOpaqueFillInTheMkBgColour) {
    beginEdit(kWideRect, [this] {
        authorBackground(ctx_.get(), page_.get(), true, 1.0f, 1.0f, 0.0f);
    });
    // Magenta canvas: anything that is not an opaque fill shows through.
    const QImage img = paint(2.0f, QColor(255, 0, 255));
    const QRect in = interior(2.0f);
    // Sample the right half of the empty field: no text, no caret there.
    for (const QPoint p : {QPoint(in.right() - 10, in.top() + 3),
                           QPoint(in.right() - 10, in.bottom() - 3),
                           QPoint(in.center().x() + 60, in.center().y())}) {
        const QColor c = img.pixelColor(p);
        EXPECT_NEAR(c.red(), 255, 3) << kB2 << "background red at " << p.x() << "," << p.y();
        EXPECT_NEAR(c.green(), 255, 3) << kB2 << "background green at " << p.x() << "," << p.y();
        EXPECT_NEAR(c.blue(), 0, 3)
            << kB2 << "the field interior is not the /MK /BG yellow (blue "
            << c.blue() << ") -- the fill is missing or not opaque";
    }
}

TEST_F(FormFieldEditPaintModel, BackgroundIsWhiteWithoutMkBg) {
    beginEdit(kWideRect, [this] {
        authorBackground(ctx_.get(), page_.get(), false, 0.0f, 0.0f, 0.0f);
    });
    const QImage img = paint(2.0f, QColor(255, 0, 255));
    const QColor c = img.pixelColor(interior(2.0f).right() - 10,
                                    interior(2.0f).center().y());
    EXPECT_EQ(c, QColor(Qt::white))
        << kB2 << "a field without /MK /BG must be filled white while edited, got "
        << c.name().toStdString();
}

TEST_F(FormFieldEditPaintModel, BackgroundOfANewFieldIsTheAppDefault) {
    beginEdit();
    const auto def = FormManager::defaultBorderAppearance(FormFieldType::Text);
    const QImage img = paint(2.0f, QColor(255, 0, 255));
    const QColor c = img.pixelColor(interior(2.0f).right() - 10,
                                    interior(2.0f).center().y());
    EXPECT_NEAR(c.red(), std::lround(def.backgroundColor[0] * 255), 2) << kB2;
    EXPECT_NEAR(c.green(), std::lround(def.backgroundColor[1] * 255), 2) << kB2;
    EXPECT_NEAR(c.blue(), std::lround(def.backgroundColor[2] * 255), 2) << kB2;
}

// ─── /DA colour and size ─────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, RedDaColourGivesRedInk) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 12 Tf 1 0 0 rg");
    });
    type(QStringLiteral("WWWW"));
    const InkStats s = textInk(paint(2.0f), 2.0f);
    EXPECT_GT(s.red, 150) << kB2 << "a red /DA painted only " << s.red
                          << " red pixels";
    EXPECT_LT(s.darkNotRed, s.red / 10)
        << kB2 << "a red /DA painted " << s.darkNotRed
        << " dark non-red pixels -- the ink is not the /DA colour";
}

TEST_F(FormFieldEditPaintModel, BlackDaColourGivesNoRedInk) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 12 Tf 0 g");
    });
    type(QStringLiteral("WWWW"));
    const InkStats s = textInk(paint(2.0f), 2.0f);
    EXPECT_GT(s.darkNotRed, 150) << kB2 << "a black /DA painted little dark ink";
    EXPECT_EQ(s.red, 0) << kB2 << "a black /DA painted red pixels";
}

TEST_F(FormFieldEditPaintModel, BiggerDaSizeGivesTallerInk) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 8 Tf 0 g");
    });
    type(QStringLiteral("WH"));
    const int small = textInk(paint(2.0f), 2.0f).height();
    authorDa(ctx_.get(), page_.get(), "/Helv 20 Tf 0 g");
    const int large = textInk(paint(2.0f), 2.0f).height();
    EXPECT_GT(small, 6) << kB2 << "an 8pt /DA painted no visible text";
    // Cap height scales linearly: 20/8 = 2.5x. Allow for rounding.
    EXPECT_GT(large, small * 2.1)
        << kB2 << "a 20pt /DA painted ink " << large << "px tall vs " << small
        << "px for 8pt -- the paint ignores the /DA size";
    EXPECT_LT(large, small * 2.9) << kB2 << "ink height out of proportion";
}

TEST_F(FormFieldEditPaintModel, AutoSizeZeroFitsTheFieldHeight) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 0 g");
    });
    type(QStringLiteral("H"));
    // Content height: (40 - 2 * 2pt padding) * zoom 2 = 72px. Helvetica's cap
    // height is 0.718 em, so a height-fitted "H" is ~52px tall; a 12pt
    // fallback would be ~17px.
    const InkStats one = textInk(paint(2.0f), 2.0f);
    EXPECT_GT(one.height(), 40)
        << kB2 << "auto-size (/DA size 0) painted an \"H\" " << one.height()
        << "px tall in a 72px content box -- it does not fit the field height";
    EXPECT_LT(one.height(), 72) << kB2 << "auto-size overflowed the field";
}

TEST_F(FormFieldEditPaintModel, AutoSizeZeroShrinksLongTextToTheFieldWidth) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 0 g");
    });
    type(QStringLiteral("HHHHHHHHHHHHHHHHHHHH"));
    const QImage img = paint(2.0f);
    const InkStats s = textInk(img, 2.0f);
    const QRect box = pixelBox(kWideRect, 2.0f);
    EXPECT_GT(s.count, 200) << kB2 << "auto-sized long text painted nothing";
    EXPECT_LE(s.right, box.right() - 3)
        << kB2 << "auto-sized text runs past the field's right padding";
    EXPECT_LT(s.height(), 40)
        << kB2 << "twenty characters were not shrunk to fit the width";
    // The first glyph starts at the left padding: the text was not scrolled.
    EXPECT_LE(s.left, box.left() + 4 + 6) << kB2 << "auto-sized text was scrolled";
}

// ─── Layouts ─────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, MultilineWrapsToASecondLine) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    type(QStringLiteral("WWWW"));
    const InkStats oneLine = textInk(paint(2.0f), 2.0f);
    const double caretTop1 = caret().top();
    type(QStringLiteral(" WWWW WWWW WWWW WWWW WWWW"));
    const InkStats wrapped = textInk(paint(2.0f), 2.0f);
    const double caretTop2 = caret().top();
    EXPECT_GT(wrapped.height(), oneLine.height() * 18 / 10)
        << kB2 << "multiline text " << wrapped.height() << "px tall vs one line "
        << oneLine.height() << "px -- it did not wrap to a second line";
    // Line height is 1.116 em: 1.116 * 12pt * 2 = 26.8px.
    EXPECT_NEAR(caretTop2 - caretTop1, 26.8, 1.5)
        << kB2 << "the caret did not move down exactly one line";
    EXPECT_LE(wrapped.right, pixelBox(tall, 2.0f).right() - 4)
        << kB2 << "wrapped text overruns the right padding";
}

TEST_F(FormFieldEditPaintModel, SingleLineDoesNotWrap) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [] {});
    type(QStringLiteral("WWWW WWWW WWWW WWWW WWWW WWWW"));
    const InkStats s = textInk(paint(2.0f), 2.0f);
    EXPECT_LT(s.height(), 30) << kB2 << "a single-line field painted "
                              << s.height() << "px of ink height";
}

TEST_F(FormFieldEditPaintModel, MultilineEnterStartsANewLine) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    type(QStringLiteral("W"));
    paint(2.0f);
    const QRectF c1 = caret();
    key(Qt::Key_Return);
    type(QStringLiteral("W"));
    paint(2.0f);
    const QRectF c2 = caret();
    EXPECT_NEAR(c2.top() - c1.top(), 26.8, 1.5) << kB2 << "Enter did not start a line";
    EXPECT_NEAR(c2.left(), c1.left(), 1.0) << kB2 << "the new line is not left-aligned";
}

TEST_F(FormFieldEditPaintModel, PasswordShowsBulletsNotTheText) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_PASSWORD, true);
    });
    type(QStringLiteral("WWWW"));
    const QImage masked = paint(2.0f);
    const QRect box = pixelBox(kWideRect, 2.0f);

    // The same buffer without the password flag: the real glyphs.
    setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_PASSWORD, false);
    const QImage plain = paint(2.0f);
    // A non-password field whose text IS four bullets.
    tool_->syncEditBuffer("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    const QImage bullets = paint(2.0f);

    EXPECT_GT(inkStats(masked, interior(2.0f)).count, 40)
        << kB2 << "a password field painted nothing while typing";
    EXPECT_EQ(masked.copy(box), bullets.copy(box))
        << kB2 << "a password field does not paint one bullet per character";
    EXPECT_NE(masked.copy(box), plain.copy(box))
        << kB2 << "a password field painted the typed text in clear";
    EXPECT_EQ(tool_->editBufferForTesting(),
              "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2")
        << "harness: syncEditBuffer did not take";
}

TEST_F(FormFieldEditPaintModel, CombCellsAreEvenlySpaced) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 4);
    });
    type(QStringLiteral("II"));
    const QImage two = paint(2.0f);
    const QRect box = pixelBox(kWideRect, 2.0f);
    const double cellW = box.width() / 4.0;   // 120px
    // The caret sits on the boundary after the second cell.
    EXPECT_NEAR(caret().left(), box.left() + 2 * cellW, 1.0)
        << kB2 << "the comb caret is not at the cell boundary";
    type(QStringLiteral("II"));
    const QImage four = paint(2.0f);
    for (int i = 0; i < 4; ++i) {
        const QRect cell(static_cast<int>(box.left() + i * cellW + 4), box.top() + 4,
                         static_cast<int>(cellW) - 8, box.height() - 8);
        const InkStats s = inkStats(four, cell);
        EXPECT_GT(s.count, 30) << kB2 << "comb cell " << i << " holds no character";
        EXPECT_NEAR(s.centroidX(), box.left() + (i + 0.5) * cellW, 3.0)
            << kB2 << "the character in comb cell " << i << " is not centred in it";
    }
    // And the third cell was empty before the third character was typed.
    const QRect third(static_cast<int>(box.left() + 2 * cellW + 4), box.top() + 4,
                      static_cast<int>(cellW) - 8, box.height() - 8);
    EXPECT_EQ(inkStats(two, third).count, 0)
        << kB2 << "comb cell 2 held ink before its character was typed";
}

// ─── Caret ───────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, CaretFollowsAMidStringCursorMove) {
    beginEdit();
    const double x0 = caretX(2.0f);
    type(QStringLiteral("WiWi"));
    const QFontMetricsF fm(helveticaPx(24));
    EXPECT_NEAR(caretX(2.0f) - x0, fm.horizontalAdvance(QStringLiteral("WiWi")), 1.0)
        << kB2 << "caret at end";
    key(Qt::Key_Left);
    key(Qt::Key_Left);
    EXPECT_NEAR(caretX(2.0f) - x0, fm.horizontalAdvance(QStringLiteral("Wi")), 1.0)
        << kB2 << "after two Left presses the caret is not after \"Wi\"";
    key(Qt::Key_Left);
    EXPECT_NEAR(caretX(2.0f) - x0, fm.horizontalAdvance(QStringLiteral("W")), 1.0)
        << kB2 << "after three Left presses the caret is not after \"W\"";
    key(Qt::Key_Home);
    EXPECT_NEAR(caretX(2.0f), x0, 0.5) << kB2 << "Home did not return the caret";
    // Typing at the cursor inserts there, and the caret advances past it.
    type(QStringLiteral("i"));
    EXPECT_EQ(tool_->editBufferForTesting(), "iWiWi") << "harness: insert at cursor";
    EXPECT_NEAR(caretX(2.0f) - x0, fm.horizontalAdvance(QStringLiteral("i")), 1.0)
        << kB2 << "the caret did not follow an insertion at the start";
}

TEST_F(FormFieldEditPaintModel, CaretStartsAtTheLeftPadding) {
    beginEdit();
    // Padding is 2 * the 1pt border = 2pt, i.e. 4px at zoom 2.
    EXPECT_NEAR(caretX(2.0f), pixelBox(kWideRect, 2.0f).left() + 4.0, 1.0)
        << kB2 << "the empty-field caret is not at MuPDF's 2pt text padding";
}

// ─── Preedit ─────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, PreeditIsUnderlinedAndTheBufferIsNot) {
    beginEdit();
    tool_->setPreeditText(QStringLiteral("WWWW"));
    const QImage composing = paint(2.0f);
    const double adv =
        QFontMetricsF(helveticaPx(24)).horizontalAdvance(QStringLiteral("WWWW"));
    // Accept the composition: same text, now in editBuffer_.
    tool_->commitPreedit(QStringLiteral("WWWW"), 0, 0);
    ASSERT_EQ(tool_->editBufferForTesting(), "WWWW") << "harness: commitPreedit";
    const QImage committed = paint(2.0f);

    const QRect in = interior(2.0f);
    const int underlined = longestInkRow(composing, in);
    const int plain = longestInkRow(committed, in);
    EXPECT_GT(underlined, adv * 0.9)
        << kB2 << "the preedit has no underline: longest ink row " << underlined
        << "px vs text advance " << adv << "px";
    EXPECT_LT(plain, adv * 0.5)
        << kB2 << "committed text is underlined like a composition (longest ink "
           "row " << plain << "px)";
    // Same glyphs either way: the composition is drawn, not just underlined.
    EXPECT_GT(textInk(committed, 2.0f).count, 100) << kB2;
    EXPECT_GT(textInk(composing, 2.0f).count, textInk(committed, 2.0f).count)
        << kB2 << "the preedit paint is not the text plus an underline";
}

TEST_F(FormFieldEditPaintModel, PreeditIsInsertedAtTheCaretNotAppended) {
    beginEdit();
    type(QStringLiteral("WW"));
    key(Qt::Key_Left);           // caret between the two Ws
    const double x0 = caretX(2.0f);
    tool_->setPreeditText(QStringLiteral("ii"));
    const QFontMetricsF fm(helveticaPx(24));
    // The caret sits after the composition, which sits at the cursor.
    EXPECT_NEAR(caretX(2.0f) - x0, fm.horizontalAdvance(QStringLiteral("ii")), 1.0)
        << kB2 << "the composition was not laid out at the cursor";
    EXPECT_EQ(tool_->editBufferForTesting(), "WW")
        << kB2 << "the composition leaked into editBuffer_";
}

// ─── Zoom ────────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintModel, ZoomScalesTheTextAndTheCaret) {
    beginEdit();
    const double a1 = caretX(1.0f);
    const double a3 = caretX(3.0f);
    type(QStringLiteral("WWWW"));
    const double d1 = caretX(1.0f) - a1;
    const InkStats h1 = textInk(paint(1.0f), 1.0f);
    const double d3 = caretX(3.0f) - a3;
    const InkStats h3 = textInk(paint(3.0f), 3.0f);
    const QString w4 = QStringLiteral("WWWW");
    EXPECT_NEAR(d1, QFontMetricsF(helveticaPx(12)).horizontalAdvance(w4), 1.0)
        << kB2 << "zoom 1: the text is not 12px (DA 12pt x zoom 1)";
    EXPECT_NEAR(d3, QFontMetricsF(helveticaPx(36)).horizontalAdvance(w4), 1.0)
        << kB2 << "zoom 3: the text is not 36px (DA 12pt x zoom 3)";
    EXPECT_GT(h3.height(), h1.height() * 25 / 10)
        << kB2 << "ink height " << h3.height() << "px at zoom 3 vs " << h1.height()
        << "px at zoom 1";
    EXPECT_NEAR(a3 - pixelBox(kWideRect, 3.0f).left(), 6.0, 1.0)
        << kB2 << "the padding does not scale with zoom";
}

// ─── The document is untouched while editing ─────────────────────────────────

TEST_F(FormFieldEditPaintModel, PaintingNeverWritesTheDocument) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 1 0 0 rg");
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_PASSWORD, true);
    });
    type(QStringLiteral("secret"));
    tool_->setPreeditText(QStringLiteral("xy"));
    paint(1.0f);
    paint(2.0f);
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "")
        << kB2 << "painting the edit wrote the field value";
    EXPECT_FALSE(undo_->canUndo()) << kB2 << "painting pushed an undo command";
}

// ─── FormFieldData carries the default appearance (read in readWidgetData) ───

TEST_F(FormFieldEditPaintModel, FieldDataCarriesTheDefaultAppearance) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                        kWideRect, "da_read"),
              0)
        << "harness: text field was not created";
    {
        const FormFieldData d = manager_->getFormField(page_.get(), 0);
        EXPECT_EQ(d.daFontName, "Helv") << kB2 << "a new field's /DA font";
        EXPECT_FLOAT_EQ(d.daFontSize, 12.0f) << kB2 << "a new field's /DA size";
        EXPECT_EQ(d.daColorComponents, 1) << kB2 << "a new field's /DA colour";
        EXPECT_FLOAT_EQ(d.daColor[0], 0.0f) << kB2;
    }
    authorDa(ctx_.get(), page_.get(), "/TiRo 18 Tf 1 0 0 rg");
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daFontName, "TiRo") << kB2;
    EXPECT_FLOAT_EQ(d.daFontSize, 18.0f) << kB2;
    EXPECT_EQ(d.daColorComponents, 3) << kB2;
    EXPECT_FLOAT_EQ(d.daColor[0], 1.0f) << kB2;
    EXPECT_FLOAT_EQ(d.daColor[1], 0.0f) << kB2;
    EXPECT_FLOAT_EQ(d.daColor[2], 0.0f) << kB2;
    // The background colour the paint uses is /MK /BG, already captured.
    EXPECT_EQ(d.borderAppearance.backgroundColorComponents, 3) << kB2;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Phase 3, /Q quadding: the live edit aligns as the committed appearance
// does (MuPDF 1.26.10 write_variable_text: tx = (w - tw) / 2 or w - tw inside
// the padded content box; per line for multiline; comb fields ignore /Q), so a
// centred or right-aligned field does not jump on commit.
// ════════════════════════════════════════════════════════════════════════════

namespace {

void setQuadding(MuPdfContext* ctx, const Page* page, int q) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, q](fz_context* c) {
        pdf_dict_put_int(c, obj, PDF_NAME(Q), q);
    });
}

class FormFieldEditPaintQuadding : public FormFieldEditPaintModel {
protected:
    // Live overlay ink vs the committed appearance rendered by MuPDF (as the
    // B1 probes render it), for "WWWW" in a field with /Q `q`.
    void expectLiveMatchesCommitted(int q) {
        beginEdit(kWideRect, [this, q] { setQuadding(ctx_.get(), page_.get(), q); });
        type(QStringLiteral("WWWW"));
        const InkStats live = textInk(paint(2.0f), 2.0f);

        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, QPointF(5.0, 5.0));   // outside: commits
        ASSERT_FALSE(tool_->isEditingField()) << "harness: the edit did not commit";
        ASSERT_EQ(manager_->getFormField(page_.get(), 0).value, "WWWW")
            << "harness: commit did not write the value";
        const QImage committed = renderOverWhite(doc_.get(), 0, 2.0f);
        ASSERT_FALSE(committed.isNull()) << "harness: page did not rasterize";
        // Inset past the 1pt (2px) /MK /BC border.
        const InkStats done =
            inkStats(committed, pixelBox(kWideRect, 2.0f).adjusted(5, 5, -5, -5));

        ASSERT_GT(done.count, 100) << "harness: the committed value rendered no ink";
        ASSERT_GT(live.count, 100) << kB2 << "/Q " << q << ": the live edit painted no ink";
        EXPECT_NEAR(live.left, done.left, 4)
            << kB2 << "/Q " << q << ": live text starts at x=" << live.left
            << " but the committed appearance starts at x=" << done.left
            << " -- the text jumps on commit";
        EXPECT_NEAR(live.right, done.right, 4)
            << kB2 << "/Q " << q << ": live text ends at x=" << live.right
            << " but the committed appearance ends at x=" << done.right;
    }
};

TEST_F(FormFieldEditPaintQuadding, CentredFieldPutsTheInkAtTheFieldCentre) {
    beginEdit(kWideRect, [this] { setQuadding(ctx_.get(), page_.get(), 1); });
    type(QStringLiteral("WWWW"));
    const InkStats s = textInk(paint(2.0f), 2.0f);
    const double centre = pixelBox(kWideRect, 2.0f).center().x();
    ASSERT_GT(s.count, 100) << kB2 << "a centred field painted no ink";
    EXPECT_NEAR(s.centroidX(), centre, 4.0)
        << kB2 << "/Q 1: the ink centroid is at x=" << s.centroidX()
        << ", the field centre at x=" << centre;
}

TEST_F(FormFieldEditPaintQuadding, RightAlignedFieldPutsTheInkAtTheRightEdge) {
    beginEdit(kWideRect, [this] { setQuadding(ctx_.get(), page_.get(), 2); });
    type(QStringLiteral("WWWW"));
    const InkStats s = textInk(paint(2.0f), 2.0f);
    const QRect box = pixelBox(kWideRect, 2.0f);
    const double contentRight = box.left() + box.width() - 4.0;   // 2pt padding
    ASSERT_GT(s.count, 100) << kB2 << "a right-aligned field painted no ink";
    EXPECT_GT(s.right, contentRight - 8)
        << kB2 << "/Q 2: the ink ends at x=" << s.right
        << ", short of the padded right edge at x=" << contentRight;
    EXPECT_LE(s.right, contentRight) << kB2 << "/Q 2: ink runs into the padding";
    EXPECT_GT(s.left, box.center().x())
        << kB2 << "/Q 2: four characters should sit in the right half";
}

TEST_F(FormFieldEditPaintQuadding, CaretFollowsTheAlignment) {
    const QRect box = pixelBox(kWideRect, 2.0f);
    const double contentLeft = box.left() + 4.0;
    const double contentW = box.width() - 8.0;
    const QFontMetricsF fm(helveticaPx(24));
    const double adv = fm.horizontalAdvance(QStringLiteral("WWWW"));

    beginEdit(kWideRect, [this] { setQuadding(ctx_.get(), page_.get(), 1); });
    EXPECT_NEAR(caretX(2.0f), contentLeft + contentW / 2.0, 1.0)
        << kB2 << "/Q 1: an empty centred field's caret is not at the centre";
    type(QStringLiteral("WWWW"));
    EXPECT_NEAR(caretX(2.0f), contentLeft + (contentW + adv) / 2.0, 1.0)
        << kB2 << "/Q 1: the caret is not after the centred text";
    key(Qt::Key_Home);
    EXPECT_NEAR(caretX(2.0f), contentLeft + (contentW - adv) / 2.0, 1.0)
        << kB2 << "/Q 1: Home does not put the caret before the centred text";
    key(Qt::Key_Right);
    tool_->setPreeditText(QStringLiteral("WW"));
    // The composition is laid out (and aligned) with the buffer: six Ws,
    // centred, caret after the first three.
    const double adv6 = fm.horizontalAdvance(QStringLiteral("WWWWWW"));
    const double adv3 = fm.horizontalAdvance(QStringLiteral("WWW"));
    EXPECT_NEAR(caretX(2.0f), contentLeft + (contentW - adv6) / 2.0 + adv3, 1.0)
        << kB2 << "/Q 1: the preedit is not aligned together with the buffer";

    setQuadding(ctx_.get(), page_.get(), 2);
    tool_->clearPreedit();
    key(Qt::Key_End);
    EXPECT_NEAR(caretX(2.0f), contentLeft + contentW, 1.0)
        << kB2 << "/Q 2: the caret after right-aligned text is not at the padded "
           "right edge";
}

TEST_F(FormFieldEditPaintQuadding, LiveInkMatchesTheCommittedAppearanceLeft) {
    expectLiveMatchesCommitted(0);
}

TEST_F(FormFieldEditPaintQuadding, LiveInkMatchesTheCommittedAppearanceCentred) {
    expectLiveMatchesCommitted(1);
}

TEST_F(FormFieldEditPaintQuadding, LiveInkMatchesTheCommittedAppearanceRight) {
    expectLiveMatchesCommitted(2);
}

TEST_F(FormFieldEditPaintQuadding, MultilineCentresEachLine) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
        setQuadding(ctx_.get(), page_.get(), 1);
    });
    type(QStringLiteral("W"));
    key(Qt::Key_Return);
    type(QStringLiteral("WWWWWW"));
    const QImage img = paint(2.0f);
    const QRect box = pixelBox(tall, 2.0f);
    const double centre = box.center().x();
    // Baselines at (2 + 1.116 * 12) pt and one 13.4pt line below, at zoom 2:
    // ~31px and ~58px below the top. Bands split between them.
    const QRect line1(box.left() + 4, box.top() + 4, box.width() - 8, 34);
    const QRect line2(box.left() + 4, box.top() + 40, box.width() - 8, 26);
    const int cx = static_cast<int>(std::floor(caret().left()));
    const InkStats a = inkStats(img, line1, cx - 3, cx + 3);
    const InkStats b = inkStats(img, line2, cx - 3, cx + 3);
    ASSERT_GT(a.count, 20) << kB2 << "the first line painted no ink";
    ASSERT_GT(b.count, 100) << kB2 << "the second line painted no ink";
    EXPECT_NEAR(a.centroidX(), centre, 4.0)
        << kB2 << "/Q 1 multiline: line 1 is not centred";
    EXPECT_NEAR(b.centroidX(), centre, 4.0)
        << kB2 << "/Q 1 multiline: line 2 is not centred";
    EXPECT_GT(b.right - b.left, (a.right - a.left) * 3)
        << "harness: the lines should differ in width";
}

TEST_F(FormFieldEditPaintQuadding, CombFieldIgnoresQuadding) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 4);
        setQuadding(ctx_.get(), page_.get(), 2);
    });
    type(QStringLiteral("II"));
    const QImage right = paint(2.0f);
    setQuadding(ctx_.get(), page_.get(), 0);
    const QImage left = paint(2.0f);
    const QRect box = pixelBox(kWideRect, 2.0f);
    EXPECT_GT(inkStats(left, box.adjusted(4, 4, -4, -4)).count, 30)
        << kB2 << "the comb field painted no ink";
    EXPECT_EQ(right.copy(box), left.copy(box))
        << kB2 << "/Q moved a comb field's characters; MuPDF's comb writer ignores it";
}

}  // namespace
// ════════════════════════════════════════════════════════════════════════════
// Pipeline form-textfield, phase 3 remediation: the security-audit follow-ups
// to the GitHub #459 live edit paint, and (carried over from the phase-2
// audit) the /MK and /BS clamps of GitHub #458.
//
//   FormFieldEditPaintHostile -- hostile values reach the live paint and the
//       widget readers/writers without a crash, a non-finite number, an
//       out-of-range float->int conversion or an unbounded loop:
//         * /DA Tf sizes 1e30, 1e50 (inf), inf, -inf, nan, negative and 0 --
//           all reachable, since MuPDF's /DA parser runs fz_strtof -- are
//           finite and in [0, 1000] once read, and paint as auto-size or at
//           the 1000 pt cap; a FormFieldData carrying them directly (the test
//           seam FormTool::paintFieldEditForTesting) paints the same way;
//         * size x zoom is clamped to [1, 10000] px before qRound; a NaN,
//           infinite, non-positive or absurd zoom, or a non-finite box,
//           paints nothing;
//         * /MaxLen 1e9 on a comb, a value longer than /MaxLen, and a
//           megabyte comb value: bounded glyph draws;
//         * invalid UTF-8 and lone surrogates in the buffer and the preedit;
//         * a 1 MB multiline value with a tiny font: one paint in < 1 s
//           (the phase-3 original took ~230 s on the development host), the
//           layout cached across repaints and rebuilt on every change, and
//           only the visible lines drawn; the same for a 1 MB single line
//           and 1 MB of zero-width codepoints;
//         * /MK /BC and /BG arrays of length 2 or 5, non-numeric, NaN or out
//           of range entries, and /BS /W of 1e30, negative or NaN, both read
//           from a file and written by restoreFormField.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker. Lambdas passed to MuPdfContext::execute capture and
// declare only trivially-destructible values (fz_try discipline).
// ════════════════════════════════════════════════════════════════════════════

#include <QElapsedTimer>

#include <limits>

namespace {

constexpr const char* kSA = "phase-3 security audit (hostile input): ";

/// The bound one paint must meet. Generous: the fixed paint takes well under
/// 0.3 s for every input below on the development host (a Release build).
/// The budget is build-config-aware: a Debug build runs the paint/layout code
/// several times slower, so the 1 s Release bound is scaled for Debug. A real
/// algorithmic blowup (e.g. a quadratic regression pushing a 1 MiB input to tens
/// of seconds) still fails in BOTH configurations; only ordinary Debug / loaded-
/// runner timing variance (observed up to ~2.7 s) is tolerated.
#ifdef NDEBUG
constexpr qint64 kOnePaintBudgetMs = 1000;
#else
constexpr qint64 kOnePaintBudgetMs = 5000;
#endif

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// 1 MiB of ASCII words.
std::string megabyteOfWords() {
    std::string big;
    big.reserve(std::size_t{1} << 20);
    const char* words = "lorem ipsum dolor sit amet consectetur ";
    while (big.size() < (std::size_t{1} << 20)) big += words;
    big.resize(std::size_t{1} << 20);
    return big;
}

// ~1 MiB of U+200B ZERO WIDTH SPACE (3 bytes each in UTF-8).
std::string megabyteOfZeroWidthSpaces() {
    std::string big;
    big.reserve(std::size_t{1} << 20);
    while (big.size() + 3 <= (std::size_t{1} << 20)) big += "\xE2\x80\x8B";
    return big;
}

// /BS << /W w /S /S >> on the first widget.
void authorBorderWidth(MuPdfContext* ctx, const Page* page, float w) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, w](fz_context* c) {
        pdf_obj* bs = pdf_dict_put_dict(c, obj, PDF_NAME(BS), 2);
        pdf_dict_put_real(c, bs, PDF_NAME(W), w);
        pdf_dict_put_name(c, bs, PDF_NAME(S), "S");
    });
}

// A hostile colour array: `n` entries, entry i a real v[i] (kind 0), the name
// /Junk (kind 1) or the string (0.5) (kind 2). POD, so it may be captured.
struct HostileArray {
    int n = 0;
    int kind[6] = {0, 0, 0, 0, 0, 0};
    float v[6] = {0, 0, 0, 0, 0, 0};
};

// Replace /MK with one holding `a` as /BC (bc) or /BG.
void authorMkArray(MuPdfContext* ctx, const Page* page, HostileArray a, bool bc) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, a, bc](fz_context* c) {
        pdf_dict_del(c, obj, PDF_NAME(MK));
        pdf_obj* mk = pdf_dict_put_dict(c, obj, PDF_NAME(MK), 2);
        pdf_obj* arr = pdf_dict_put_array(c, mk, bc ? PDF_NAME(BC) : PDF_NAME(BG), a.n);
        for (int i = 0; i < a.n && i < 6; ++i) {
            if (a.kind[i] == 1) pdf_array_push_name(c, arr, "Junk");
            else if (a.kind[i] == 2) pdf_array_push_text_string(c, arr, "0.5");
            else pdf_array_push_real(c, arr, a.v[i]);
        }
    });
}

HostileArray reals(std::initializer_list<float> vs) {
    HostileArray a;
    for (const float v : vs) {
        if (a.n >= 6) break;
        a.v[a.n++] = v;
    }
    return a;
}

QRectF screenBox(const QRectF& r, float zoom) {
    return QRectF(r.x() * zoom, r.y() * zoom, r.width() * zoom, r.height() * zoom);
}

bool finiteRect(const QRectF& r) {
    return std::isfinite(r.left()) && std::isfinite(r.top()) &&
           std::isfinite(r.right()) && std::isfinite(r.bottom());
}

// A text field as FormManager would hand it to the paint: Helv 12, black.
FormFieldData seamField(float daSize) {
    FormFieldData f;
    f.type = FormFieldType::Text;
    f.rect = kWideRect;
    f.daFontName = "Helv";
    f.daFontSize = daSize;
    f.daColorComponents = 1;
    return f;
}

struct SeamPaint {
    QImage img;
    FormTool::EditPaintStats stats;
};

// The live paint, called directly with `field` (FormTool's test seam).
SeamPaint seamPaint(const FormFieldData& field, float zoom, const QRectF& box,
                    const std::string& buffer = "WWWW", int cursorByte = -1) {
    SeamPaint out;
    out.img = QImage(1200, 800, QImage::Format_ARGB32);
    out.img.fill(Qt::white);
    QPainter painter(&out.img);
    out.stats = FormTool::paintFieldEditForTesting(
        painter, field, box, zoom, buffer,
        cursorByte < 0 ? static_cast<int>(buffer.size()) : cursorByte, "");
    painter.end();
    return out;
}

class FormFieldEditPaintHostile : public FormFieldEditPaintModel {
protected:
    const FormTool::EditPaintStats& stats() const {
        return tool_->lastEditPaintStatsForTesting();
    }

    // One paint at `zoom`, timed, in milliseconds.
    qint64 timedPaint(float zoom, QImage* out = nullptr) {
        QElapsedTimer t;
        t.start();
        QImage img = paint(zoom);
        const qint64 ms = t.elapsed();
        if (out) *out = std::move(img);
        return ms;
    }

    void expectCaretSane(const char* what) {
        QRectF r;
        ASSERT_TRUE(tool_->caretScreenRect(r)) << kSA << what << ": no caret recorded";
        EXPECT_TRUE(finiteRect(r)) << kSA << what << ": the caret is not finite";
        const QRect box = pixelBox(rect_, 2.0f);
        EXPECT_GE(r.top(), box.top()) << kSA << what << ": caret above the field";
        EXPECT_LE(r.bottom(), box.bottom() + 1) << kSA << what << ": caret below the field";
    }
};

// ─── /DA size: read clamp ────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, DaSizeIsFiniteAndCappedWhenRead) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kWideRect,
                                        "hostile_da"),
              0)
        << "harness: text field was not created";
    struct Case {
        const char* da;
        float expect;
    };
    const Case cases[] = {
        {"/Helv 18 Tf 0 g", 18.0f},        // control: a sane size is kept
        {"/Helv 1000 Tf 0 g", 1000.0f},    // the cap itself
        {"/Helv 1e30 Tf 0 g", 1000.0f},    // huge -> the cap
        {"/Helv 1000.5 Tf 0 g", 1000.0f},
        {"/Helv 1e50 Tf 0 g", 0.0f},       // overflows fz_strtof to inf -> auto
        {"/Helv inf Tf 0 g", 0.0f},
        {"/Helv -inf Tf 0 g", 0.0f},
        {"/Helv nan Tf 0 g", 0.0f},
        {"/Helv -5 Tf 0 g", 0.0f},         // negative -> auto
        {"/Helv 0 Tf 0 g", 0.0f},          // auto stays auto
    };
    for (const Case& c : cases) {
        authorDa(ctx_.get(), page_.get(), c.da);
        const FormFieldData d = manager_->getFormField(page_.get(), 0);
        EXPECT_TRUE(std::isfinite(d.daFontSize))
            << kSA << "/DA \"" << c.da << "\" read back a non-finite size";
        EXPECT_FLOAT_EQ(d.daFontSize, c.expect)
            << kSA << "/DA \"" << c.da << "\" read back size " << d.daFontSize;
    }
    // A NaN colour operand reads as 0, not NaN.
    authorDa(ctx_.get(), page_.get(), "/Helv 12 Tf nan inf 0.5 rg");
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daColorComponents, 3) << kSA << "harness: rg not parsed";
    EXPECT_FLOAT_EQ(d.daColor[0], 0.0f) << kSA << "a NaN /DA colour component";
    EXPECT_FLOAT_EQ(d.daColor[1], 0.0f) << kSA << "an infinite /DA colour component";
    EXPECT_FLOAT_EQ(d.daColor[2], 0.5f) << kSA << "a sane /DA colour component";
}

// ─── /DA size: the live paint ────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, HostileDaSizesPaintAsAutoSizeOrAtTheCap) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 0 g");
    });
    type(QStringLiteral("WWWW"));
    const QRect box = pixelBox(kWideRect, 2.0f);
    const QImage autoImg = paint(2.0f);
    const int autoPx = stats().fontPixelSize;
    ASSERT_GT(autoPx, 20) << "harness: auto-size should fill the 36pt content height";
    ASSERT_GT(textInk(autoImg, 2.0f).count, 100) << "harness: auto-size painted no ink";

    for (const char* da : {"/Helv nan Tf 0 g", "/Helv inf Tf 0 g", "/Helv -inf Tf 0 g",
                           "/Helv 1e50 Tf 0 g", "/Helv -5 Tf 0 g"}) {
        authorDa(ctx_.get(), page_.get(), da);
        const QImage img = paint(2.0f);
        EXPECT_EQ(stats().fontPixelSize, autoPx)
            << kSA << "/DA \"" << da << "\" painted at " << stats().fontPixelSize
            << "px, not as auto-size (" << autoPx << "px)";
        EXPECT_EQ(img.copy(box), autoImg.copy(box))
            << kSA << "/DA \"" << da << "\" does not paint exactly as auto-size";
        expectCaretSane(da);
    }

    // 1e30 is capped at 1000 pt: 2000 px at zoom 2, within [1, 10000].
    authorDa(ctx_.get(), page_.get(), "/Helv 1e30 Tf 0 g");
    paint(2.0f);
    EXPECT_EQ(stats().fontPixelSize, 2000)
        << kSA << "/DA 1e30 painted at " << stats().fontPixelSize << "px";
    expectCaretSane("/DA 1e30");
}

TEST_F(FormFieldEditPaintHostile, NonFiniteSizeInFieldDataPaintsAsAutoSize) {
    const QRectF box = screenBox(kWideRect, 2.0f);
    const SeamPaint base = seamPaint(seamField(0.0f), 2.0f, box);
    ASSERT_GT(base.stats.fontPixelSize, 20) << "harness: auto-size";
    ASSERT_GT(inkStats(base.img, pixelBox(kWideRect, 2.0f).adjusted(4, 4, -4, -4)).count, 100)
        << "harness: the seam painted no ink";
    for (const float size : {kNaN, kInf, -kInf, -1.0f}) {
        const SeamPaint p = seamPaint(seamField(size), 2.0f, box);
        EXPECT_EQ(p.stats.fontPixelSize, base.stats.fontPixelSize)
            << kSA << "a FormFieldData /DA size of " << size << " painted at "
            << p.stats.fontPixelSize << "px, not as auto-size";
        EXPECT_EQ(p.img, base.img)
            << kSA << "a FormFieldData /DA size of " << size
            << " does not paint exactly as auto-size";
        EXPECT_TRUE(finiteRect(p.stats.caret)) << kSA << "caret for size " << size;
    }
    EXPECT_EQ(seamPaint(seamField(1e30f), 2.0f, box).stats.fontPixelSize, 2000)
        << kSA << "a FormFieldData /DA size of 1e30 is not capped at 1000 pt";
}

TEST_F(FormFieldEditPaintHostile, PixelSizeIsClampedBeforeTheIntConversion) {
    struct Case {
        float size;
        float zoom;
        int expectPx;
    };
    const Case cases[] = {
        {1000.0f, 9.99f, 9990},     // in range: qRound(size x zoom) as before
        {1000.0f, 10.0f, 10000},    // the cap
        {1000.0f, 20.0f, 10000},    // 20000 -> the cap
        {1000.0f, 1000.0f, 10000},  // 1e6 -> the cap
        {0.01f, 1.0f, 1},           // below a pixel -> 1
        {12.0f, 2.0f, 24},          // control
    };
    for (const Case& c : cases) {
        const SeamPaint p = seamPaint(seamField(c.size), c.zoom, screenBox(kWideRect, c.zoom), "WW");
        EXPECT_EQ(p.stats.fontPixelSize, c.expectPx)
            << kSA << "/DA " << c.size << " pt at zoom " << c.zoom << " asked for a "
            << p.stats.fontPixelSize << "px font";
        EXPECT_TRUE(finiteRect(p.stats.caret))
            << kSA << "/DA " << c.size << " at zoom " << c.zoom << ": caret not finite";
    }
}

TEST_F(FormFieldEditPaintHostile, NonFiniteOrAbsurdZoomOrBoxPaintsNothing) {
    QImage white(1200, 800, QImage::Format_ARGB32);
    white.fill(Qt::white);
    const QRectF box = screenBox(kWideRect, 2.0f);
    for (const float zoom : {kNaN, kInf, -kInf, 0.0f, -2.0f, 1e30f, 1001.0f}) {
        const SeamPaint p = seamPaint(seamField(12.0f), zoom, box);
        EXPECT_TRUE(p.stats.caret.isNull()) << kSA << "zoom " << zoom << " recorded a caret";
        EXPECT_EQ(p.stats.fontPixelSize, 0) << kSA << "zoom " << zoom << " chose a font";
        EXPECT_EQ(p.img, white) << kSA << "zoom " << zoom << " painted something";
    }
    const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
    const qreal inf = std::numeric_limits<qreal>::infinity();
    for (const QRectF& bad : {QRectF(nan, 10, 100, 20), QRectF(10, 10, inf, 20),
                              QRectF(10, 10, 1e8, 20), QRectF(-1e9, 10, 100, 20)}) {
        const SeamPaint p = seamPaint(seamField(12.0f), 2.0f, bad);
        EXPECT_TRUE(p.stats.caret.isNull()) << kSA << "a hostile box recorded a caret";
        EXPECT_EQ(p.img, white) << kSA << "a hostile box painted something";
    }
    // Control: the same call with a sane zoom and box paints.
    EXPECT_FALSE(seamPaint(seamField(12.0f), 2.0f, box).stats.caret.isNull())
        << "harness: the seam does not paint at all";
}

// ─── /BS /W ──────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, BorderWidthIsClampedWhenRead) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kWideRect,
                                        "hostile_bs"),
              0)
        << "harness: text field was not created";
    struct Case {
        float w;
        float expect;
    };
    for (const Case c : {Case{3.0f, 3.0f}, Case{1e30f, 100.0f}, Case{100.5f, 100.0f},
                         Case{-7.0f, 0.0f}, Case{kNaN, 1.0f}, Case{kInf, 1.0f}}) {
        authorBorderWidth(ctx_.get(), page_.get(), c.w);
        const FormFieldData d = manager_->getFormField(page_.get(), 0);
        ASSERT_TRUE(d.borderAppearance.hasBorderStyle) << "harness: /BS not read";
        EXPECT_FLOAT_EQ(d.borderAppearance.borderWidth, c.expect)
            << kSA << "/BS /W " << c.w << " read back as " << d.borderAppearance.borderWidth;
    }
}

TEST_F(FormFieldEditPaintHostile, HugeBorderWidthInFieldDataKeepsThePaintFinite) {
    const QRectF box = screenBox(kWideRect, 2.0f);
    FormFieldData one = seamField(12.0f);
    one.borderAppearance.hasBorderStyle = true;
    one.borderAppearance.borderWidth = 1.0f;
    const SeamPaint base = seamPaint(one, 2.0f, box);

    FormFieldData huge = one;
    huge.borderAppearance.borderWidth = 1e30f;
    const SeamPaint h = seamPaint(huge, 2.0f, box);
    EXPECT_TRUE(finiteRect(h.stats.caret))
        << kSA << "/BS /W 1e30 put the caret at " << h.stats.caret.left();
    EXPECT_GE(h.stats.caret.left(), box.left()) << kSA << "/BS /W 1e30 caret";
    EXPECT_LE(h.stats.caret.left(), box.right()) << kSA << "/BS /W 1e30 caret";

    FormFieldData nan = one;
    nan.borderAppearance.borderWidth = kNaN;
    EXPECT_EQ(seamPaint(nan, 2.0f, box).img, base.img)
        << kSA << "/BS /W NaN does not paint as the default width of 1";
}

// ─── /MK colour arrays ───────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, MkColourArraysAreSanitisedWhenRead) {
    ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kWideRect,
                                        "hostile_mk"),
              0)
        << "harness: text field was not created";
    auto bg = [this](HostileArray a) {
        authorMkArray(ctx_.get(), page_.get(), a, false);
        return manager_->getFormField(page_.get(), 0).borderAppearance;
    };
    auto bc = [this](HostileArray a) {
        authorMkArray(ctx_.get(), page_.get(), a, true);
        return manager_->getFormField(page_.get(), 0).borderAppearance;
    };

    EXPECT_EQ(bg(reals({0.5f, 0.5f})).backgroundColorComponents, 0)
        << kSA << "a 2-entry /MK /BG is not a colour";
    EXPECT_EQ(bg(reals({0.1f, 0.2f, 0.3f, 0.4f, 0.5f})).backgroundColorComponents, 0)
        << kSA << "a 5-entry /MK /BG is not a colour";
    EXPECT_EQ(bc(reals({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f})).borderColorComponents, 0)
        << kSA << "a 6-entry /MK /BC is not a colour";

    const FormFieldBorderAppearance big = bg(reals({5.0f, -3.0f, 1e30f}));
    ASSERT_EQ(big.backgroundColorComponents, 3) << kSA << "an out-of-range RGB /BG";
    EXPECT_FLOAT_EQ(big.backgroundColor[0], 1.0f) << kSA << "/BG 5 not clamped to 1";
    EXPECT_FLOAT_EQ(big.backgroundColor[1], 0.0f) << kSA << "/BG -3 not clamped to 0";
    EXPECT_FLOAT_EQ(big.backgroundColor[2], 1.0f) << kSA << "/BG 1e30 not clamped to 1";

    HostileArray junk = reals({0.0f, 0.0f, 0.25f});
    junk.kind[0] = 1;   // /Junk
    junk.kind[1] = 2;   // (0.5)
    const FormFieldBorderAppearance j = bg(junk);
    ASSERT_EQ(j.backgroundColorComponents, 3) << kSA << "a 3-entry /BG with junk";
    EXPECT_FLOAT_EQ(j.backgroundColor[0], 0.0f) << kSA << "a name entry in /BG";
    EXPECT_FLOAT_EQ(j.backgroundColor[1], 0.0f) << kSA << "a string entry in /BG";
    EXPECT_FLOAT_EQ(j.backgroundColor[2], 0.25f) << kSA << "a sane entry in /BG";

    const FormFieldBorderAppearance n = bc(reals({kNaN, 0.5f, kInf}));
    ASSERT_EQ(n.borderColorComponents, 3) << kSA << "a NaN-bearing RGB /BC";
    EXPECT_FLOAT_EQ(n.borderColor[0], 0.0f) << kSA << "/BC NaN not read as 0";
    EXPECT_FLOAT_EQ(n.borderColor[1], 0.5f) << kSA << "/BC 0.5";
    EXPECT_FLOAT_EQ(n.borderColor[2], 0.0f) << kSA << "/BC inf not read as 0";
}

TEST_F(FormFieldEditPaintHostile, OutOfRangeMkBgPaintsTheClampedColour) {
    beginEdit(kWideRect, [this] {
        authorMkArray(ctx_.get(), page_.get(), reals({5.0f, -3.0f, 1e30f}), false);
    });
    // Clamped to (1, 0, 1): magenta. Sample the empty right half of the field.
    const QImage img = paint(2.0f, QColor(0, 255, 0));
    const QColor c = img.pixelColor(interior(2.0f).right() - 10, interior(2.0f).center().y());
    EXPECT_EQ(c, QColor(255, 0, 255))
        << kSA << "/MK /BG [5 -3 1e30] painted " << c.name().toStdString()
        << ", not the clamped (1 0 1)";
}

TEST_F(FormFieldEditPaintHostile, RestoreWritesOnlySanitisedMkAndBs) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.rect = kWideRect;
    data.fieldName = "restored_hostile";
    data.borderAppearanceCaptured = true;
    // A component count past the 4-element arrays: nothing may be written
    // (and nothing may be read past the array).
    data.borderAppearance.borderColorComponents = 7;
    data.borderAppearance.borderColor = {0.1f, 0.2f, 0.3f, 0.4f};
    data.borderAppearance.backgroundColorComponents = 3;
    data.borderAppearance.backgroundColor = {2.0f, kNaN, -1.0f, 0.0f};
    data.borderAppearance.hasBorderStyle = true;
    data.borderAppearance.borderWidth = 1e30f;
    data.borderAppearance.borderStyle = 'S';
    ASSERT_GE(manager_->restoreFormField(page_.get(), data), 0)
        << "harness: restoreFormField failed";

    const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
    EXPECT_EQ(mk.bcLen, 0) << kSA << "a 7-component /BC was written";
    ASSERT_EQ(mk.bgLen, 3) << kSA << "the RGB /BG was not written";
    EXPECT_FLOAT_EQ(mk.bg[0], 1.0f) << kSA << "/BG 2 written unclamped";
    EXPECT_FLOAT_EQ(mk.bg[1], 0.0f) << kSA << "/BG NaN written as a non-zero";
    EXPECT_FLOAT_EQ(mk.bg[2], 0.0f) << kSA << "/BG -1 written unclamped";
    ASSERT_TRUE(mk.hasBs) << kSA << "/BS was not written";
    EXPECT_FLOAT_EQ(mk.bsWidth, 100.0f) << kSA << "/BS /W 1e30 written unclamped";
}

// ─── Comb ────────────────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, CombDrawsNothingPastMaxLen) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 4);
    });
    const QRect box = pixelBox(kWideRect, 2.0f);
    tool_->syncEditBuffer("IIII");
    const QImage four = paint(2.0f);
    EXPECT_EQ(stats().runsDrawn, 4) << "harness: four comb cells";
    tool_->syncEditBuffer("IIIIIIII");
    const QImage eight = paint(2.0f);
    EXPECT_EQ(stats().runsDrawn, 4)
        << kSA << "a comb with /MaxLen 4 drew " << stats().runsDrawn
        << " glyphs for an 8-character value";
    EXPECT_EQ(eight.copy(box), four.copy(box))
        << kSA << "characters past /MaxLen changed the comb's pixels";
    // A composition past /MaxLen is not drawn either.
    tool_->syncEditBuffer("III");
    tool_->setPreeditText(QStringLiteral("xyz"));
    paint(2.0f);
    EXPECT_EQ(stats().runsDrawn, 4) << kSA << "a preedit past /MaxLen was drawn";
}

TEST_F(FormFieldEditPaintHostile, CombWithAMaxLenOfOneBillionIsBounded) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 1000000000);
    });
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).maxLen, 1000000000)
        << "harness: /MaxLen 1e9 not read";
    type(QStringLiteral("WWWW"));
    const QImage img = paint(2.0f);
    EXPECT_GT(inkStats(img, interior(2.0f)).count, 20) << kSA << "/MaxLen 1e9 painted nothing";
    EXPECT_LE(stats().runsDrawn, 4) << kSA << "/MaxLen 1e9";
    expectCaretSane("/MaxLen 1e9");

    tool_->syncEditBuffer(megabyteOfWords());
    const qint64 ms = timedPaint(2.0f);
    EXPECT_LT(ms, kOnePaintBudgetMs)
        << kSA << "a 1 MB comb value with /MaxLen 1e9 took " << ms << " ms to paint";
    // One glyph per pixel column at most: the box is 480 px wide.
    EXPECT_LE(stats().runsDrawn, pixelBox(kWideRect, 2.0f).width() + 1)
        << kSA << "/MaxLen 1e9 drew " << stats().runsDrawn << " glyphs";
    expectCaretSane("/MaxLen 1e9, 1 MB");
}

TEST_F(FormFieldEditPaintHostile, CombMegabyteValueWithASmallMaxLenPaintsQuickly) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 4);
    });
    tool_->syncEditBuffer(megabyteOfWords());
    const qint64 ms = timedPaint(2.0f);
    EXPECT_LT(ms, kOnePaintBudgetMs)
        << kSA << "a 1 MB value in a 4-cell comb took " << ms << " ms to paint";
    EXPECT_EQ(stats().runsDrawn, 4) << kSA << "a 4-cell comb drew " << stats().runsDrawn;
}

// ─── Invalid UTF-8 and lone surrogates ───────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, InvalidUtf8AndLoneSurrogatesPaintSafely) {
    // CESU-8 lone surrogate U+D800, two invalid bytes, a truncated 2-byte
    // sequence, a stray continuation byte.
    const std::string bad = "W\xED\xA0\x80W\xFF\xFE\xC3W\x80";
    beginEdit();
    tool_->syncEditBuffer(bad);
    EXPECT_GT(textInk(paint(2.0f), 2.0f).count, 100) << kSA << "invalid UTF-8 painted nothing";
    expectCaretSane("invalid UTF-8, caret at end");
    for (const int k : {Qt::Key_Left, Qt::Key_Left, Qt::Key_Left, Qt::Key_Home,
                        Qt::Key_Right, Qt::Key_End}) {
        key(k);
        paint(2.0f);
        expectCaretSane("invalid UTF-8, caret moved");
    }

    // Lone high and low surrogates in the composition.
    QString pre;
    pre += QChar(0xD800);
    pre += QLatin1Char('W');
    pre += QChar(0xDC00);
    tool_->setPreeditText(pre);
    EXPECT_GT(textInk(paint(2.0f), 2.0f).count, 100) << kSA << "lone-surrogate preedit";
    expectCaretSane("lone-surrogate preedit");
    EXPECT_EQ(tool_->editBufferForTesting(), bad) << kSA << "the preedit leaked into the buffer";

    // The same buffer in every layout.
    for (const int flag : {PDF_TX_FIELD_IS_MULTILINE, PDF_TX_FIELD_IS_PASSWORD,
                           PDF_TX_FIELD_IS_COMB}) {
        setFieldFlagBits(ctx_.get(), page_.get(), flag, true);
        if (flag == PDF_TX_FIELD_IS_COMB) setMaxLen(ctx_.get(), page_.get(), 6);
        paint(2.0f);
        expectCaretSane("invalid UTF-8 in a multiline / password / comb field");
        setFieldFlagBits(ctx_.get(), page_.get(), flag, false);
    }

    // The seam, with the cursor byte inside the invalid sequence.
    for (const int at : {1, 2, 3, 5, 7, 9, 100, -5}) {
        const SeamPaint p = seamPaint(seamField(12.0f), 2.0f, screenBox(kWideRect, 2.0f),
                                      bad, at);
        EXPECT_TRUE(finiteRect(p.stats.caret)) << kSA << "cursor byte " << at;
        EXPECT_FALSE(p.stats.caret.isNull()) << kSA << "cursor byte " << at;
    }
}

// ─── A megabyte value ────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, MegabyteMultilineValueWithATinyFontPaintsWithinASecond) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
        authorDa(ctx_.get(), page_.get(), "/Helv 0.5 Tf 0 g");
    });
    tool_->syncEditBuffer(megabyteOfWords());

    QImage img;
    const qint64 first = timedPaint(2.0f, &img);
    // ASSERT, not EXPECT: with the quadratic wrap this paint takes minutes,
    // and the rest of the test would only repeat it.
    ASSERT_LT(first, kOnePaintBudgetMs)
        << kSA << "the first paint of a 1 MB multiline value took " << first << " ms";
    const FormTool::EditPaintStats s1 = stats();
    EXPECT_FALSE(s1.layoutReused) << kSA << "the first paint reused a layout";
    EXPECT_GT(s1.linesTotal, 100) << "harness: a 1 MB value should wrap to many lines";
    EXPECT_LT(s1.linesDrawn, s1.linesTotal / 4)
        << kSA << "drew " << s1.linesDrawn << " of " << s1.linesTotal
        << " lines -- the lines outside the field are not culled";
    EXPECT_LT(s1.unitsDrawn, (1 << 20) / 4) << kSA << "drew " << s1.unitsDrawn << " units";
    EXPECT_GT(inkStats(img, interior(2.0f)).count, 100) << kSA << "painted no ink";
    expectCaretSane("1 MB multiline");

    const qint64 second = timedPaint(2.0f);
    EXPECT_LT(second, kOnePaintBudgetMs) << kSA << "a repaint took " << second << " ms";
    EXPECT_TRUE(stats().layoutReused) << kSA << "an unchanged repaint rebuilt the layout";

    type(QStringLiteral("x"));
    const qint64 third = timedPaint(2.0f);
    EXPECT_LT(third, kOnePaintBudgetMs) << kSA << "the paint after a keystroke took "
                                        << third << " ms";
    EXPECT_FALSE(stats().layoutReused) << kSA << "a keystroke did not invalidate the layout";
}

TEST_F(FormFieldEditPaintHostile, MegabyteSingleLineValuePaintsWithinASecond) {
    beginEdit(kWideRect, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0.5 Tf 0 g");
    });
    tool_->syncEditBuffer(megabyteOfWords());
    QImage img;
    const qint64 ms = timedPaint(2.0f, &img);
    EXPECT_LT(ms, kOnePaintBudgetMs) << kSA << "a 1 MB single line took " << ms << " ms";
    EXPECT_LT(stats().unitsDrawn, 65536)
        << kSA << "drew " << stats().unitsDrawn << " units of a 1 MB single line -- "
        << "the off-field text is not culled";
    EXPECT_GT(inkStats(img, interior(2.0f)).count, 100) << kSA << "painted no ink";
    expectCaretSane("1 MB single line");
    // Scrolled so the caret (at the end) sits at the padded right edge.
    QRectF caret;
    ASSERT_TRUE(tool_->caretScreenRect(caret));
    const QRect box = pixelBox(kWideRect, 2.0f);
    EXPECT_NEAR(caret.left(), box.left() + box.width() - 4.0, 1.0)
        << kSA << "the caret of a scrolled 1 MB line is not at the right edge";

    // Auto-size measures the whole value: also bounded.
    authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 0 g");
    const qint64 autoMs = timedPaint(2.0f);
    EXPECT_LT(autoMs, kOnePaintBudgetMs)
        << kSA << "a 1 MB auto-sized single line took " << autoMs << " ms";
    // Shrunk to MuPDF's 1 pt floor: 2 px at zoom 2.
    EXPECT_EQ(stats().fontPixelSize, 2) << kSA << "1 MB auto-sized to more than 1 pt";
}

TEST_F(FormFieldEditPaintHostile, MegabyteOfZeroWidthCodepointsPaintsWithinASecond) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    const std::string zw = megabyteOfZeroWidthSpaces();
    const long long units = static_cast<long long>(zw.size() / 3);
    tool_->syncEditBuffer(zw);
    const qint64 multi = timedPaint(2.0f);
    EXPECT_LT(multi, kOnePaintBudgetMs)
        << kSA << "1 MB of U+200B (multiline) took " << multi << " ms";
    // No width bounds such a line; the layout cuts lines at 65536 units and
    // draws at most 16384 of each visible one -- well under the whole value.
    EXPECT_LT(stats().unitsDrawn, units / 2) << kSA << "multiline U+200B units drawn";
    expectCaretSane("1 MB U+200B multiline");
    setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, false);
    const qint64 single = timedPaint(2.0f);
    EXPECT_LT(single, kOnePaintBudgetMs)
        << kSA << "1 MB of U+200B (single line) took " << single << " ms";
    EXPECT_LE(stats().unitsDrawn, 65536) << kSA << "single-line U+200B units drawn";
    expectCaretSane("1 MB U+200B single line");
}

// ─── The layout cache ────────────────────────────────────────────────────────

TEST_F(FormFieldEditPaintHostile, LayoutCacheIsRebuiltOnEveryChange) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    type(QStringLiteral("WWWW WWWW WWWW"));
    paint(2.0f);
    EXPECT_FALSE(stats().layoutReused) << "harness: first paint";
    paint(2.0f);
    EXPECT_TRUE(stats().layoutReused) << kSA << "an unchanged repaint rebuilt the layout";

    // A keystroke: the layout must follow the new text. Compare with a fresh
    // tool that has never laid out the old text.
    type(QStringLiteral(" WW WWW"));
    const QImage edited = paint(2.0f);
    EXPECT_FALSE(stats().layoutReused) << kSA << "a keystroke reused the old layout";
    FormTool fresh(manager_.get(), nullptr, undo_.get());
    fresh.activate();
    fresh.setPage(page_.get());
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    fresh.onMousePress(&ev, 0, tall.center());
    ASSERT_TRUE(fresh.isEditingField()) << "harness: the fresh tool did not start an edit";
    fresh.syncEditBuffer(tool_->editBufferForTesting());
    QImage freshImg(1200, 800, QImage::Format_ARGB32);
    freshImg.fill(Qt::white);
    {
        QPainter p(&freshImg);
        fresh.onPaintOverlay(p, 0, 2.0f, QPointF(0, 0), 792.0f);
    }
    const QRect box = pixelBox(tall, 2.0f);
    EXPECT_EQ(edited.copy(box), freshImg.copy(box))
        << kSA << "after a keystroke the painted layout differs from a fresh layout "
                  "of the same text -- a stale layout was reused";

    // A composition, a /DA change and a zoom change each rebuild it too.
    tool_->setPreeditText(QStringLiteral("ii"));
    paint(2.0f);
    EXPECT_FALSE(stats().layoutReused) << kSA << "a preedit reused the old layout";
    paint(2.0f);
    EXPECT_TRUE(stats().layoutReused) << "harness: unchanged repaint";
    authorDa(ctx_.get(), page_.get(), "/Helv 9 Tf 0 g");
    paint(2.0f);
    EXPECT_FALSE(stats().layoutReused) << kSA << "a /DA size change reused the old layout";
    paint(3.0f);
    EXPECT_FALSE(stats().layoutReused) << kSA << "a zoom change reused the old layout";
}

}  // namespace
// ════════════════════════════════════════════════════════════════════════════
// Pipeline form-textfield, phase 3, remediation round 2 (claim-check gate).
// Each suite below mechanises a claim the gate found resting on prose alone:
//
//   FormFieldEditPaintCanvasIme -- Qt::ImCursorRectangle, asked of a real
//       PdfCanvas whose current tool is an editing FormTool, is the caret
//       that paintEvent actually drew, in widget coordinates: equal to the
//       rect FormTool recorded, and within 1 px of the drawn caret line's
//       pixel column, after typing, after a cursor move and with a preedit.
//   FormFieldEditPaintDrawnCaret -- the same pixel check without the canvas,
//       for single line, /Q 1, multiline (second line) and comb, so a drift
//       of the drawn caret against the reported one is caught below 2 px
//       (the ink tests only exclude a +-3 px caret window).
//   FormFieldEditPaintParentQuadding -- a /Q set only on the parent field
//       dictionary. MuPDF 1.26.10's pdf_annot_quadding reads /Q from the
//       widget dictionary alone, so the committed appearance is left-aligned;
//       the live paint must be too, or the text jumps on commit.
//   FormFieldEditPaintFonts -- the /DA font mapping (TiRo serif, Cour
//       monospace, an unknown name to sans-serif) and the 12 pt auto-size of
//       a multiline field.
//   FormFieldEditPaintHostileBounds -- the two LOW items of the phase-3
//       security audit: a zoom below 1e-3 (a denormal one included) paints
//       nothing, and a multiline paint never draws more lines than the box
//       can show.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker. Lambdas passed to MuPdfContext::execute capture and
// declare only trivially-destructible values (fz_try discipline).
// ════════════════════════════════════════════════════════════════════════════

#include "rendering/TileCache.h"
#include "rendering/TileRenderer.h"
#include "ui/PdfCanvas.h"

#include <QApplication>
#include <QFontInfo>
#include <QPixmap>
#include <QVariant>

namespace {

constexpr const char* kCC = "phase-3 claim-check: ";

// The caret line as drawn: the pixel columns of `area` whose longest vertical
// run of dark pixels (luminance < 60: the caret is drawn aliased, in the /DA
// colour, black here) is at least `minRun` pixels. Glyphs without ascenders
// or descenders ("a", "c", "m", "n", "u") never reach that height, so only the
// caret qualifies.
struct DrawnCaret {
    int columns = 0;
    double meanColumn = -1.0;
    int top = -1;
    int bottom = -1;
};

DrawnCaret findDrawnCaret(const QImage& img, const QRect& area, int minRun) {
    DrawnCaret out;
    const QRect r = area.intersected(img.rect());
    double sum = 0.0;
    for (int x = r.left(); x <= r.right(); ++x) {
        int best = 0, bestTop = -1, run = 0, runTop = -1;
        for (int y = r.top(); y <= r.bottom(); ++y) {
            if (luminance(img.pixel(x, y)) < 60) {
                if (run == 0) runTop = y;
                ++run;
                if (run > best) {
                    best = run;
                    bestTop = runTop;
                }
            } else {
                run = 0;
            }
        }
        if (best < minRun) continue;
        ++out.columns;
        sum += x;
        if (out.top < 0 || bestTop < out.top) out.top = bestTop;
        out.bottom = std::max(out.bottom, bestTop + best - 1);
    }
    if (out.columns) out.meanColumn = sum / out.columns;
    return out;
}

// The drawn caret against the reported rect: one aliased column whose pixel
// centre is within 1 px of the rect's x, spanning the rect's height.
void expectDrawnCaretMatches(const QImage& img, const QRect& area, const QRectF& reported,
                             const std::string& when) {
    const int minRun = std::max(8, static_cast<int>(reported.height() * 0.8));
    const DrawnCaret d = findDrawnCaret(img, area, minRun);
    ASSERT_GE(d.columns, 1) << kCC << when << ": no caret line was drawn (no column "
                            << "with a " << minRun << "px dark run)";
    EXPECT_LE(d.columns, 2) << kCC << when << ": the caret is " << d.columns
                            << " columns wide";
    EXPECT_NEAR(d.meanColumn + 0.5, reported.left(), 1.0)
        << kCC << when << ": the caret is DRAWN at pixel column " << d.meanColumn
        << " but REPORTED at x=" << reported.left()
        << " -- the IME candidate window would not sit at the visible caret";
    EXPECT_NEAR(d.top, reported.top(), 1.5)
        << kCC << when << ": drawn caret top " << d.top << " vs reported "
        << reported.top();
    EXPECT_NEAR(d.bottom + 1, reported.bottom(), 1.5)
        << kCC << when << ": drawn caret bottom " << d.bottom << " vs reported "
        << reported.bottom();
}

// ─── ImCursorRectangle through a real PdfCanvas ──────────────────────────────

class FormFieldEditPaintCanvasIme : public BlankDocTest {
protected:
    void SetUp() override {
        BlankDocTest::SetUp();
        if (HasFatalFailure()) return;
        ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text, kWideRect,
                                            "ime_canvas"),
                  0)
            << "harness: text field was not created";
        cache_ = std::make_unique<pdfe::rendering::TileCache>(8 * 1024 * 1024);
        renderer_ = std::make_unique<pdfe::rendering::TileRenderer>(doc_.get());
        canvas_ = std::make_unique<pdfe::ui::PdfCanvas>();
        canvas_->resize(1400, 1000);
        canvas_->setDocument(doc_.get(), cache_.get(), renderer_.get());
        canvas_->setZoom(2.0f);
        ASSERT_FLOAT_EQ(canvas_->zoom(), 2.0f) << "harness: canvas zoom";
        auto tool = std::make_unique<FormTool>(manager_.get(), nullptr, undo_.get());
        tool_ = tool.get();
        canvas_->setCurrentTool(std::move(tool));

        // Click the field through the canvas, as the user does: the canvas
        // binds the tool to the clicked page and starts the edit.
        const QPointF centre = toWidget(kWideRect.center());
        QMouseEvent press(QEvent::MouseButtonPress, centre, canvas_->mapToGlobal(centre),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas_.get(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, centre,
                            canvas_->mapToGlobal(centre), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas_.get(), &release);
        ASSERT_TRUE(tool_->isEditingField())
            << "harness: clicking the field through the canvas did not start an edit";
        ASSERT_TRUE(canvas_->inputMethodEnabledForTesting())
            << "harness: the canvas does not advertise IME while a field is edited";
    }

    void TearDown() override {
        tool_ = nullptr;
        canvas_.reset();
        renderer_.reset();
        cache_.reset();
        BlankDocTest::TearDown();
    }

    // Page point (fitz space) to canvas widget coordinates.
    QPointF toWidget(const QPointF& pt) const {
        QPoint origin;
        int w = 0, h = 0;
        EXPECT_TRUE(canvas_->getPageScreenRect(0, origin, w, h)) << "harness: page rect";
        return QPointF(origin.x() + pt.x() * canvas_->zoom(),
                       origin.y() + pt.y() * canvas_->zoom());
    }

    QRect fieldInteriorOnCanvas() const {
        const QPointF tl = toWidget(kWideRect.topLeft());
        const QPointF br = toWidget(kWideRect.bottomRight());
        return QRect(QPoint(static_cast<int>(std::lround(tl.x())),
                            static_cast<int>(std::lround(tl.y()))),
                     QPoint(static_cast<int>(std::lround(br.x())) - 1,
                            static_cast<int>(std::lround(br.y())) - 1))
            .adjusted(4, 4, -4, -4);
    }

    void typeOnCanvas(const QString& s) {
        for (const QChar ch : s) {
            QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
            QApplication::sendEvent(canvas_.get(), &ev);
        }
    }

    void keyOnCanvas(int k) {
        QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
        QApplication::sendEvent(canvas_.get(), &ev);
    }

    // Paint the canvas (paintEvent -> FormTool::onPaintOverlay), then ask it
    // for Qt::ImCursorRectangle exactly as the platform input context does.
    // Checks the answer against the rect FormTool recorded and against the
    // caret pixels in the painted image; returns the answer.
    QRectF paintAndCheck(const std::string& when) {
        const QImage img =
            canvas_->grab().toImage().convertToFormat(QImage::Format_RGB32);
        EXPECT_FALSE(img.isNull()) << "harness: the canvas did not paint";
        const QVariant v = canvas_->inputMethodQuery(Qt::ImCursorRectangle);
        EXPECT_TRUE(v.isValid()) << kCC << when << ": no ImCursorRectangle";
        const QRectF reported = v.toRectF();
        QRectF recorded;
        EXPECT_TRUE(tool_->caretScreenRect(recorded))
            << kCC << when << ": FormTool recorded no caret for the painted edit";
        EXPECT_EQ(reported, recorded)
            << kCC << when << ": ImCursorRectangle is (" << reported.x() << ","
            << reported.y() << " " << reported.width() << "x" << reported.height()
            << ") but the caret FormTool drew is (" << recorded.x() << ","
            << recorded.y() << " " << recorded.width() << "x" << recorded.height() << ")";
        const QRect field = fieldInteriorOnCanvas();
        EXPECT_TRUE(QRectF(field).adjusted(-4, -4, 4, 4).contains(reported))
            << kCC << when << ": ImCursorRectangle lies outside the edited field";
        expectDrawnCaretMatches(img, field, reported, when);
        return reported;
    }

    std::unique_ptr<pdfe::rendering::TileCache> cache_;
    std::unique_ptr<pdfe::rendering::TileRenderer> renderer_;
    std::unique_ptr<pdfe::ui::PdfCanvas> canvas_;
    FormTool* tool_ = nullptr;
};

TEST_F(FormFieldEditPaintCanvasIme, CursorRectangleIsTheDrawnCaretAfterTyping) {
    const QRectF empty = paintAndCheck("empty field");
    typeOnCanvas(QStringLiteral("mana"));
    ASSERT_EQ(tool_->editBufferForTesting(), "mana")
        << "harness: keys sent to the canvas did not reach the field";
    const QRectF typed = paintAndCheck("after typing \"mana\"");
    const double adv =
        QFontMetricsF(helveticaPx(24)).horizontalAdvance(QStringLiteral("mana"));
    EXPECT_NEAR(typed.left() - empty.left(), adv, 1.0)
        << kCC << "the reported caret did not advance by the typed text's width";
}

TEST_F(FormFieldEditPaintCanvasIme, CursorRectangleFollowsACursorMove) {
    typeOnCanvas(QStringLiteral("manama"));
    const QRectF end = paintAndCheck("caret at the end");
    keyOnCanvas(Qt::Key_Left);
    keyOnCanvas(Qt::Key_Left);
    const QRectF mid = paintAndCheck("after two Left presses");
    const QFontMetricsF fm(helveticaPx(24));
    EXPECT_NEAR(end.left() - mid.left(), fm.horizontalAdvance(QStringLiteral("ma")), 1.0)
        << kCC << "the reported caret did not move back over \"ma\"";
    keyOnCanvas(Qt::Key_Home);
    const QRectF home = paintAndCheck("after Home");
    EXPECT_NEAR(mid.left() - home.left(), fm.horizontalAdvance(QStringLiteral("mana")), 1.0)
        << kCC << "the reported caret did not move to the start";
}

TEST_F(FormFieldEditPaintCanvasIme, CursorRectangleFollowsAPreedit) {
    typeOnCanvas(QStringLiteral("mana"));
    keyOnCanvas(Qt::Key_Left);
    const QRectF before = paintAndCheck("before the composition");
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(QStringLiteral("uu"), QString()))
        << "harness: the canvas did not accept the input-method event";
    ASSERT_EQ(tool_->preeditTextForTesting(), "uu") << "harness: preedit";
    const QRectF composing = paintAndCheck("with the preedit \"uu\"");
    EXPECT_NEAR(composing.left() - before.left(),
                QFontMetricsF(helveticaPx(24)).horizontalAdvance(QStringLiteral("uu")), 1.0)
        << kCC << "the reported caret is not after the composition";
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(QString(), QStringLiteral("uu")))
        << "harness: commit";
    ASSERT_EQ(tool_->editBufferForTesting(), "manuua") << "harness: commit position";
    const QRectF committed = paintAndCheck("after committing the composition");
    EXPECT_NEAR(committed.left(), composing.left(), 1.0)
        << kCC << "committing the composition moved the caret";
}

// ─── The drawn caret column matches the recorded rect, per layout ───────────

class FormFieldEditPaintDrawnCaret : public FormFieldEditPaintModel {
protected:
    void expectMatches(const std::string& when) {
        const QImage img = paint(2.0f);
        expectDrawnCaretMatches(img, interior(2.0f), caret(), when);
    }
};

TEST_F(FormFieldEditPaintDrawnCaret, SingleLine) {
    beginEdit();
    expectMatches("empty single line");
    type(QStringLiteral("mancu"));
    expectMatches("single line, caret at the end");
    key(Qt::Key_Left);
    key(Qt::Key_Left);
    expectMatches("single line, caret moved left");
    tool_->setPreeditText(QStringLiteral("mm"));
    expectMatches("single line with a preedit");
}

TEST_F(FormFieldEditPaintDrawnCaret, CentredSingleLine) {
    beginEdit(kWideRect, [this] { setQuadding(ctx_.get(), page_.get(), 1); });
    type(QStringLiteral("mancu"));
    expectMatches("/Q 1, caret at the end");
    key(Qt::Key_Home);
    expectMatches("/Q 1, caret at the start");
}

TEST_F(FormFieldEditPaintDrawnCaret, MultilineSecondLine) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    type(QStringLiteral("mancu"));
    key(Qt::Key_Return);
    type(QStringLiteral("nun"));
    expectMatches("multiline, caret on the second line");
}

TEST_F(FormFieldEditPaintDrawnCaret, Comb) {
    beginEdit(kWideRect, [this] {
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_COMB, true);
        setMaxLen(ctx_.get(), page_.get(), 6);
    });
    type(QStringLiteral("ac"));
    expectMatches("comb, caret after two cells");
}

// ─── /Q only on the parent field ─────────────────────────────────────────────

// Give the first widget a new parent field carrying /Q `q`, and remove /Q
// from the widget itself, so /Q is reachable only through /Parent.
void moveQuaddingToANewParent(MuPdfContext* ctx, const Page* page, int q) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, q](fz_context* c) {
        pdf_document* doc = pdf_get_bound_document(c, obj);
        pdf_obj* parent = pdf_add_new_dict(c, doc, 4);
        pdf_dict_put_name(c, parent, PDF_NAME(FT), "Tx");
        pdf_dict_put_text_string(c, parent, PDF_NAME(T), "parent_q");
        pdf_dict_put_int(c, parent, PDF_NAME(Q), q);
        pdf_obj* kids = pdf_dict_put_array(c, parent, PDF_NAME(Kids), 1);
        pdf_array_push(c, kids, obj);
        pdf_dict_put(c, obj, PDF_NAME(Parent), parent);
        pdf_dict_del(c, obj, PDF_NAME(Q));
        pdf_drop_obj(c, parent);
    });
}

// /Q as the widget dictionary has it, and as /Parent has it (-1: absent).
std::pair<int, int> readWidgetAndParentQ(MuPdfContext* ctx, const Page* page) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return {-2, -2};
    int own = -1;
    int inherited = -1;
    int* o = &own;
    int* p = &inherited;
    ctx->execute([obj, o, p](fz_context* c) {
        pdf_obj* q = pdf_dict_get(c, obj, PDF_NAME(Q));
        *o = q ? pdf_to_int(c, q) : -1;
        pdf_obj* pq = pdf_dict_get(c, pdf_dict_get(c, obj, PDF_NAME(Parent)), PDF_NAME(Q));
        *p = pq ? pdf_to_int(c, pq) : -1;
    });
    return {own, inherited};
}

class FormFieldEditPaintParentQuadding : public FormFieldEditPaintModel {};

TEST_F(FormFieldEditPaintParentQuadding, ParentOnlyQIsIgnoredLikeTheCommittedAppearance) {
    beginEdit(kWideRect, [this] { moveQuaddingToANewParent(ctx_.get(), page_.get(), 2); });
    const auto [own, inherited] = readWidgetAndParentQ(ctx_.get(), page_.get());
    ASSERT_EQ(own, -1) << "harness: the widget still carries its own /Q";
    ASSERT_EQ(inherited, 2) << "harness: the parent field does not carry /Q 2";

    type(QStringLiteral("WWWW"));
    const InkStats live = textInk(paint(2.0f), 2.0f);

    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMousePress(&ev, 0, QPointF(5.0, 5.0));   // outside: commits
    ASSERT_FALSE(tool_->isEditingField()) << "harness: the edit did not commit";
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).value, "WWWW")
        << "harness: commit did not write the value";
    const QImage committed = renderOverWhite(doc_.get(), 0, 2.0f);
    ASSERT_FALSE(committed.isNull()) << "harness: page did not rasterize";
    const QRect box = pixelBox(kWideRect, 2.0f);
    const InkStats done = inkStats(committed, box.adjusted(5, 5, -5, -5));

    ASSERT_GT(done.count, 100) << "harness: the committed value rendered no ink";
    ASSERT_GT(live.count, 100) << kCC << "parent-only /Q: the live edit painted no ink";
    // MuPDF 1.26.10 (pdf_annot_quadding reads the widget dictionary only):
    // the committed text is left-aligned, at the 2pt (4px) padding.
    EXPECT_LE(done.left, box.left() + 4 + 6)
        << "harness: MuPDF right-aligned the committed value from the parent /Q; "
           "its pdf_annot_quadding now walks /Parent and FormManager must follow";
    EXPECT_LE(live.left, box.left() + 4 + 6)
        << kCC << "parent-only /Q 2: the live edit is not left-aligned (starts at x="
        << live.left << ")";
    EXPECT_NEAR(live.left, done.left, 4)
        << kCC << "parent-only /Q: live text starts at x=" << live.left
        << " but the committed appearance starts at x=" << done.left
        << " -- the text jumps on commit";
    EXPECT_NEAR(live.right, done.right, 4)
        << kCC << "parent-only /Q: live text ends at x=" << live.right
        << " but the committed appearance ends at x=" << done.right;
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).quadding, 0)
        << kCC << "FormFieldData.quadding took the parent's /Q";
}

// ─── /DA font mapping and the multiline auto-size ───────────────────────────

QFont timesPx(int px) {
    QFont font(QStringLiteral("Times"));
    font.setStyleHint(QFont::Times);
    font.setPixelSize(px);
    return font;
}

QFont courierPx(int px) {
    QFont font(QStringLiteral("Courier"));
    font.setStyleHint(QFont::Courier);
    font.setFixedPitch(true);
    font.setPixelSize(px);
    return font;
}

QFont sansPx(int px) {
    QFont font(QStringLiteral("sans-serif"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(px);
    return font;
}

class FormFieldEditPaintFonts : public FormFieldEditPaintModel {
protected:
    // Caret advance over `s`, typed into the field being edited, at zoom 2.
    double advanceOf(const QString& s) {
        key(Qt::Key_End);
        while (!tool_->editBufferForTesting().empty()) key(Qt::Key_Backspace);
        const double x0 = caretX(2.0f);
        type(s);
        return caretX(2.0f) - x0;
    }
};

TEST_F(FormFieldEditPaintFonts, TiRoPaintsInASerifFace) {
    const QString s = QStringLiteral("aaaa");
    const double times = QFontMetricsF(timesPx(24)).horizontalAdvance(s);
    const double helv = QFontMetricsF(helveticaPx(24)).horizontalAdvance(s);
    ASSERT_GT(std::abs(times - helv), 4.0)
        << "harness: this host resolves Times and Helvetica to faces of the same "
           "metrics, so the mapping cannot be told apart";
    EXPECT_FALSE(QFontInfo(timesPx(24)).fixedPitch()) << "harness: Times is fixed pitch";
    beginEdit(kWideRect, [this] { authorDa(ctx_.get(), page_.get(), "/TiRo 12 Tf 0 g"); });
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "TiRo") << "harness: /DA";
    const double adv = advanceOf(s);
    EXPECT_NEAR(adv, times, 1.0)
        << kCC << "/TiRo: \"aaaa\" advanced " << adv << "px; the Times (serif) face "
        << "gives " << times << "px, Helvetica " << helv << "px";
}

TEST_F(FormFieldEditPaintFonts, CourPaintsInAMonospaceFace) {
    beginEdit(kWideRect, [this] { authorDa(ctx_.get(), page_.get(), "/Cour 12 Tf 0 g"); });
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Cour") << "harness: /DA";
    const double narrow = advanceOf(QStringLiteral("iiii"));
    const double wide = advanceOf(QStringLiteral("WWWW"));
    EXPECT_GT(narrow, 20.0) << kCC << "/Cour: \"iiii\" painted no advance";
    EXPECT_NEAR(narrow, wide, 1.0)
        << kCC << "/Cour: \"iiii\" advanced " << narrow << "px but \"WWWW\" " << wide
        << "px -- the face is not monospace";
    EXPECT_NEAR(wide, QFontMetricsF(courierPx(24)).horizontalAdvance(QStringLiteral("WWWW")),
                1.0)
        << kCC << "/Cour: the advance is not the Courier face's";
}

TEST_F(FormFieldEditPaintFonts, UnknownFontFallsBackToSans) {
    const QString s = QStringLiteral("aaaa");
    const double sans = QFontMetricsF(sansPx(24)).horizontalAdvance(s);
    const double times = QFontMetricsF(timesPx(24)).horizontalAdvance(s);
    ASSERT_GT(std::abs(sans - times), 4.0)
        << "harness: this host's sans-serif and Times faces have the same metrics";

    // From a file, MuPDF 1.26.10's pdf_parse_default_appearance already maps
    // any name other than Cour, Helv, TiRo, Symb and ZaDb to Helv, so the
    // paint's fallback is reached by /Symb and /ZaDb (no Qt equivalent) ...
    beginEdit(kWideRect, [this] { authorDa(ctx_.get(), page_.get(), "/Symb 12 Tf 0 g"); });
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Symb") << "harness: /DA";
    const double symb = advanceOf(s);
    EXPECT_NEAR(symb, sans, 1.0)
        << kCC << "/Symb: \"aaaa\" advanced " << symb << "px; the sans-serif "
        << "fallback gives " << sans << "px, Times " << times << "px";
    authorDa(ctx_.get(), page_.get(), "/ZaDb 12 Tf 0 g");
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "ZaDb") << "harness: /DA";
    const double zadb = advanceOf(s);
    EXPECT_NEAR(zadb, sans, 1.0) << kCC << "/ZaDb: not the sans-serif fallback";

    // ... and by a name no file can deliver, in a hand-built FormFieldData.
    FormFieldData f = seamField(12.0f);
    f.daFontName = "NoSuchFont";
    const QRectF box = screenBox(kWideRect, 2.0f);
    const double start = seamPaint(f, 2.0f, box, "aaaa", 0).stats.caret.left();
    const double end = seamPaint(f, 2.0f, box, "aaaa", 4).stats.caret.left();
    EXPECT_NEAR(end - start, sans, 1.0)
        << kCC << "an unknown font name: \"aaaa\" advanced " << (end - start)
        << "px; the sans-serif fallback gives " << sans << "px";

    // Control: an unknown name in a file's /DA reads back as Helv.
    authorDa(ctx_.get(), page_.get(), "/Foo 12 Tf 0 g");
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Helv")
        << "harness: MuPDF no longer maps an unknown /DA font to Helv";
}

TEST_F(FormFieldEditPaintFonts, AutoSizeMultilineIsTwelvePoint) {
    const QRectF tall(100.0, 100.0, 240.0, 80.0);
    beginEdit(tall, [this] {
        authorDa(ctx_.get(), page_.get(), "/Helv 0 Tf 0 g");
        setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, true);
    });
    const double adv = advanceOf(QStringLiteral("WWWW"));
    EXPECT_EQ(tool_->lastEditPaintStatsForTesting().fontPixelSize, 24)
        << kCC << "a multiline field with /DA size 0 is not painted at 12 pt (24px "
                  "at zoom 2)";
    EXPECT_NEAR(adv, QFontMetricsF(helveticaPx(24)).horizontalAdvance(QStringLiteral("WWWW")),
                1.0)
        << kCC << "multiline auto-size: the text is not 12 pt";
    // Control: the same field single-line fits the (much taller) content
    // height instead, so the 12 pt above is the multiline branch.
    setFieldFlagBits(ctx_.get(), page_.get(), PDF_TX_FIELD_IS_MULTILINE, false);
    paint(2.0f);
    EXPECT_GT(tool_->lastEditPaintStatsForTesting().fontPixelSize, 60)
        << "harness: single-line auto-size should fit the 76pt content height";
}

// ─── The two LOW items of the phase-3 security audit ────────────────────────

class FormFieldEditPaintHostileBounds : public FormFieldEditPaintHostile {};

TEST_F(FormFieldEditPaintHostileBounds, ZoomBelowTheLowerBoundPaintsNothing) {
    QImage white(1200, 800, QImage::Format_ARGB32);
    white.fill(Qt::white);
    const QRectF box = screenBox(kWideRect, 2.0f);
    const float denormal = std::numeric_limits<float>::denorm_min();
    for (const bool multiline : {false, true}) {
        FormFieldData f = seamField(12.0f);
        f.isMultiline = multiline;
        for (const float zoom : {9.99e-4f, 1e-4f, 1e-10f, 1e-38f, 1e-40f, denormal}) {
            const SeamPaint p = seamPaint(f, zoom, box, "WWWW WWWW WWWW");
            EXPECT_TRUE(p.stats.caret.isNull())
                << kSA << (multiline ? "multiline" : "single line") << ", zoom " << zoom
                << " recorded a caret";
            EXPECT_EQ(p.stats.fontPixelSize, 0)
                << kSA << "zoom " << zoom << " chose a font";
            EXPECT_EQ(p.stats.linesDrawn, 0) << kSA << "zoom " << zoom << " drew lines";
            EXPECT_EQ(p.img, white) << kSA << "zoom " << zoom << " painted something";
        }
        // The bound itself is accepted, and paints finitely.
        const SeamPaint ok = seamPaint(f, 1e-3f, box, "WWWW WWWW WWWW");
        EXPECT_FALSE(ok.stats.caret.isNull())
            << kSA << "zoom 1e-3 (the lower bound) painted nothing";
        EXPECT_TRUE(finiteRect(ok.stats.caret)) << kSA << "zoom 1e-3: caret not finite";
        EXPECT_LE(ok.stats.linesDrawn, 3) << kSA << "zoom 1e-3 drew too many lines";
    }
}

TEST_F(FormFieldEditPaintHostileBounds, MultilineNeverDrawsMoreLinesThanTheBoxCanShow) {
    FormFieldData f = seamField(1.0f);   // 1 pt: ~2.2px lines at zoom 2
    f.isMultiline = true;
    const QRectF box = screenBox(kWideRect, 2.0f);
    const std::string big = megabyteOfWords();
    const double reach = 2.0 * QFontMetricsF(helveticaPx(2)).height();
    const double lineH = 1.116 * 1.0 * 2.0;
    const double bound = std::ceil((box.height() + 2.0 * reach) / lineH) + 2.0;
    for (const int cursor : {0, static_cast<int>(big.size() / 2), static_cast<int>(big.size())}) {
        QElapsedTimer t;
        t.start();
        const SeamPaint p = seamPaint(f, 2.0f, box, big, cursor);
        const qint64 ms = t.elapsed();
        EXPECT_GT(p.stats.linesTotal, 1000) << "harness: the value did not wrap";
        EXPECT_GT(p.stats.linesDrawn, 0) << kSA << "cursor " << cursor << ": nothing drawn";
        EXPECT_LE(p.stats.linesDrawn, static_cast<long long>(bound))
            << kSA << "cursor " << cursor << ": drew " << p.stats.linesDrawn
            << " lines, more than the " << bound << " the field can show";
        EXPECT_TRUE(finiteRect(p.stats.caret)) << kSA << "cursor " << cursor;
        EXPECT_LT(ms, kOnePaintBudgetMs) << kSA << "cursor " << cursor << ": " << ms << " ms";
    }
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Pipeline form-textfield, phase 4: model tests for the GitHub #460 fix (a
// form text field's font, size and colour -- its /DA -- can be set through an
// undoable command and survive undo, redo, delete+undo and save).
//
//   FormFieldDefaultAppearanceModel -- FormManager::setFieldDefaultAppearance
//       writes the /DA string and reads it back; accepts exactly the five
//       base-14 keys, sizes 0 and 1..1000 and opaque in-range colours; rejects
//       everything else with the serialised document byte-identical;
//       SetFieldAppearanceCommand undo/redo restore the exact previous and
//       next /DA (and the /DS that MuPDF's setter deletes); the command never
//       merges and its execute() is idempotent; the /DA survives save+reopen;
//       delete+undo and undo/redo of create keep /DA, /MK and /Q, including
//       an inherited /DA, a /DA naming a non-base-14 /DR font, a hostile
//       /DA string and an overlong one (captured as its parsed values);
//       hand-built restore data gets the app default.
//   FormFieldDefaultAppearanceEditPaint -- a /DA change made while a text
//       field is being typed into is picked up by the live edit paint (size
//       and colour) without ending the edit or touching the buffer, through
//       the setter and through the command and its undo.
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker. Lambdas passed to MuPdfContext::execute capture and
// declare only trivially-destructible values (fz_try discipline).
// ════════════════════════════════════════════════════════════════════════════

#include <limits>

namespace {

using pdfe::forms::FormFieldOwnDefaultAppearance;
using pdfe::forms::SetFieldAppearanceCommand;

constexpr const char* kB3M =
    "GitHub #460 (font settings ignored for form text fields): ";

using pdfe::testsupport::documentBytes;

// Author the first widget's own /DA as raw bytes (embedded NULs allowed).
void authorOwnDaBytes(MuPdfContext* ctx, const Page* page, const char* data, size_t len) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, data, len](fz_context* c) {
        pdf_dict_put_string(c, obj, PDF_NAME(DA), data, len);
    });
}

void authorOwnDs(MuPdfContext* ctx, const Page* page, const char* ds) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, ds](fz_context* c) {
        pdf_dict_put_text_string(c, obj, PDF_NAME(DS), ds);
    });
}

void removeOwnDa(MuPdfContext* ctx, const Page* page) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj](fz_context* c) { pdf_dict_del(c, obj, PDF_NAME(DA)); });
}

// A /DA that is a NAME, not a string (invalid PDF).
void authorOwnDaAsName(MuPdfContext* ctx, const Page* page) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj](fz_context* c) {
        pdf_dict_put_name(c, obj, PDF_NAME(DA), "Helv");
    });
}

void setAcroFormDa(MuPdfContext* ctx, const Page* page, const char* da) {
    pdf_obj* w = firstWidgetObj(ctx, page);
    if (!w) return;
    ctx->execute([w, da](fz_context* c) {
        pdf_document* doc = pdf_get_bound_document(c, w);
        pdf_obj* acro = pdf_dict_getl(c, pdf_trailer(c, doc), PDF_NAME(Root),
                                      PDF_NAME(AcroForm), nullptr);
        if (acro) pdf_dict_put_text_string(c, acro, PDF_NAME(DA), da);
    });
}

// /DR << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Courier >> >> >>
// on the AcroForm (onWidget == false) or on the widget itself.
void addDrFontF1(MuPdfContext* ctx, const Page* page, bool onWidget) {
    pdf_obj* w = firstWidgetObj(ctx, page);
    if (!w) return;
    ctx->execute([w, onWidget](fz_context* c) {
        pdf_obj* host = w;
        if (!onWidget) {
            pdf_document* doc = pdf_get_bound_document(c, w);
            host = pdf_dict_getl(c, pdf_trailer(c, doc), PDF_NAME(Root),
                                 PDF_NAME(AcroForm), nullptr);
        }
        if (!host) return;
        pdf_obj* dr = pdf_dict_put_dict(c, host, PDF_NAME(DR), 1);
        pdf_obj* fonts = pdf_dict_put_dict(c, dr, PDF_NAME(Font), 1);
        pdf_obj* f1 = pdf_dict_puts_dict(c, fonts, "F1", 3);
        pdf_dict_put(c, f1, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(c, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_name(c, f1, PDF_NAME(BaseFont), "Courier");
    });
}

bool widgetHasOwnDr(MuPdfContext* ctx, const Page* page) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return false;
    return ctx->executeReturn<bool>([obj](fz_context* c) -> bool {
        return pdf_is_dict(c, pdf_dict_get(c, obj, PDF_NAME(DR))) != 0;
    });
}

// The widget dictionary's own /Q, or -1 when absent.
int readOwnQ(MuPdfContext* ctx, const Page* page) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return -2;
    return ctx->executeReturn<int>([obj](fz_context* c) -> int {
        pdf_obj* q = pdf_dict_get(c, obj, PDF_NAME(Q));
        return q ? pdf_to_int(c, q) : -1;
    });
}

class FormFieldDefaultAppearanceModel : public BlankDocTest {
protected:
    void createText(const char* name = "da_model") {
        ASSERT_EQ(manager_->createFormField(page_.get(), FormFieldType::Text,
                                            kWideRect, name),
                  0)
            << "harness: text field was not created";
    }

    FormFieldOwnDefaultAppearance own() {
        const auto s = manager_->getFieldOwnDefaultAppearance(page_.get(), 0);
        EXPECT_TRUE(s.has_value()) << "harness: widget 0 did not resolve";
        return s.value_or(FormFieldOwnDefaultAppearance{});
    }

    std::string bytes() { return documentBytes(ctx_.get(), page_.get()); }

    // Baseline bytes, with a control that serialising is itself deterministic.
    std::string stableBytes() {
        const std::string a = bytes();
        const std::string b = bytes();
        EXPECT_FALSE(a.empty()) << "harness: the document did not serialise";
        EXPECT_EQ(a, b) << "harness: serialising twice gave different bytes";
        return a;
    }

    bool set(const std::string& font, float size, const QColor& color) {
        return manager_->setFieldDefaultAppearance(page_.get(), 0, font, size, color);
    }

    // Save to a scratch file and reopen it as the document under test.
    void saveAndReopen() {
        saved_ = scratchDir() / (currentTestSlug() + "_saved.pdf");
        std::error_code ec;
        std::filesystem::remove(saved_, ec);
        undo_ = std::make_unique<UndoManager>();
        page_.reset();
        doc_->saveAs(saved_);
        doc_.reset();
        doc_ = Document::open(ctx_, saved_);
        ASSERT_NE(doc_, nullptr) << "harness: saved file did not reopen";
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr) << "harness: reopened page missing";
    }

    void deleteAndUndo() {
        undo_->execute(std::make_unique<DeleteFormFieldCommand>(
            manager_.get(), page_.get(), 0));
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u)
            << "harness: delete did not remove the field";
        undo_->undo();
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u)
            << "harness: undo of delete did not restore the field";
    }

    void TearDown() override {
        BlankDocTest::TearDown();
        std::error_code ec;
        if (!saved_.empty()) std::filesystem::remove(saved_, ec);
    }

    std::filesystem::path saved_;
};

// ─── The setter ──────────────────────────────────────────────────────────────

TEST_F(FormFieldDefaultAppearanceModel, SetterWritesTheDaStringAndReadsItBack) {
    createText();
    ASSERT_TRUE(set("Cour", 9.0f, QColor(0, 0, 255))) << kB3M << "Cour 9 blue was rejected";
    const FormFieldOwnDefaultAppearance s = own();
    EXPECT_TRUE(s.hasDa) << kB3M << "no own /DA after the setter";
    EXPECT_EQ(s.da, "/Cour 9 Tf 0 0 1 rg") << kB3M << "the /DA string written";
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daFontName, "Cour") << kB3M;
    EXPECT_FLOAT_EQ(d.daFontSize, 9.0f) << kB3M;
    EXPECT_EQ(d.daColorComponents, 3) << kB3M;
    EXPECT_FLOAT_EQ(d.daColor[0], 0.0f) << kB3M;
    EXPECT_FLOAT_EQ(d.daColor[1], 0.0f) << kB3M;
    EXPECT_FLOAT_EQ(d.daColor[2], 1.0f) << kB3M;
    EXPECT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    EXPECT_EQ(d.defaultAppearanceString, "/Cour 9 Tf 0 0 1 rg") << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, EveryBase14KeyIsAcceptedAndReadsBack) {
    createText();
    for (const char* key : {"Helv", "TiRo", "Cour", "Symb", "ZaDb"}) {
        EXPECT_TRUE(set(key, 10.0f, QColor(Qt::black))) << kB3M << key << " was rejected";
        EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, key)
            << kB3M << key << " did not read back";
        EXPECT_EQ(own().da, std::string("/") + key + " 10 Tf 0 0 0 rg") << kB3M << key;
    }
}

TEST_F(FormFieldDefaultAppearanceModel, ComboAndListFieldsAcceptTheSetter) {
    for (FormFieldType t : {FormFieldType::ComboBox, FormFieldType::ListBox}) {
        undo_->execute(std::make_unique<CreateFormFieldCommand>(
            manager_.get(), page_.get(), t, kWideRect, "choice"));
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u) << "harness";
        EXPECT_TRUE(set("TiRo", 14.0f, QColor(255, 0, 0))) << kB3M << typeName(t);
        EXPECT_EQ(own().da, "/TiRo 14 Tf 1 0 0 rg") << kB3M << typeName(t);
        undo_->undo();
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u) << "harness";
    }
}

TEST_F(FormFieldDefaultAppearanceModel, BoundarySizesAreAccepted) {
    createText();
    EXPECT_TRUE(set("Helv", 0.0f, QColor(Qt::black))) << kB3M << "size 0 (auto)";
    EXPECT_EQ(own().da, "/Helv 0 Tf 0 0 0 rg") << kB3M;
    EXPECT_TRUE(set("Helv", -0.0f, QColor(Qt::black))) << kB3M << "size -0 (auto)";
    EXPECT_EQ(own().da, "/Helv 0 Tf 0 0 0 rg") << kB3M << "-0 must be written as 0";
    EXPECT_TRUE(set("Helv", 1.0f, QColor(Qt::black))) << kB3M << "size 1";
    EXPECT_EQ(own().da, "/Helv 1 Tf 0 0 0 rg") << kB3M;
    EXPECT_TRUE(set("Helv", 1000.0f, QColor(Qt::black))) << kB3M << "size 1000";
    EXPECT_EQ(own().da, "/Helv 1000 Tf 0 0 0 rg") << kB3M;
    EXPECT_FLOAT_EQ(manager_->getFormField(page_.get(), 0).daFontSize, 1000.0f) << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, InvalidFontIsRejectedAndTheDocumentIsUnchanged) {
    createText();
    manager_->setFieldValue(page_.get(), 0, "abc");
    const std::string before = stableBytes();
    const std::string withNul("ZaDb\0x", 6);
    const std::string huge(10000, 'H');
    for (const std::string& font :
         {std::string(), std::string("helv"), std::string("HELV"), std::string("Helvetica"),
          std::string("F1"), std::string("Helv "), std::string(" Helv"), std::string("/Helv"),
          std::string("Helv 12 Tf"), std::string("Ti Ro"), std::string("Arial"),
          std::string("Helv)Tj("), withNul, huge}) {
        EXPECT_FALSE(set(font, 12.0f, QColor(255, 0, 0)))
            << kB3M << "font '" << font.substr(0, 20) << "' (length " << font.size()
            << ") was accepted";
        EXPECT_EQ(bytes(), before)
            << kB3M << "rejecting font '" << font.substr(0, 20) << "' modified the document";
    }
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 g") << kB3M << "the /DA changed";
}

TEST_F(FormFieldDefaultAppearanceModel, NonFiniteNegativeAndOversizedSizesAreRejected) {
    createText();
    const std::string before = stableBytes();
    for (float size : {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(), -1.0f, -12.0f,
                       std::numeric_limits<float>::denorm_min(), 0.5f, 0.999f,
                       1000.01f, 1001.0f, 1e30f, std::numeric_limits<float>::max()}) {
        EXPECT_FALSE(set("Helv", size, QColor(255, 0, 0)))
            << kB3M << "size " << size << " was accepted";
        EXPECT_EQ(bytes(), before) << kB3M << "rejecting size " << size
                                   << " modified the document";
    }
}

TEST_F(FormFieldDefaultAppearanceModel, OutOfRangeOrTranslucentColoursAreRejected) {
    createText();
    const std::string before = stableBytes();
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const QColor bad[] = {
        QColor(),                               // invalid
        QColor::fromRgbF(1.5f, 0.0f, 0.0f),     // extended RGB, > 1
        QColor::fromRgbF(-0.1f, 0.0f, 0.0f),    // extended RGB, < 0
        QColor::fromRgbF(0.0f, inf, 0.0f),      // extended RGB, inf
        QColor::fromRgbF(0.0f, 0.0f, -inf),     // extended RGB, -inf
        QColor::fromRgbF(nan, 2.0f, 0.0f),      // extended RGB, NaN
        QColor(255, 0, 0, 128),                 // translucent
        QColor::fromRgbF(0.5f, 0.5f, 0.5f, 0.5f),
        QColor(255, 0, 0, 0),                   // fully transparent
    };
    int i = 0;
    for (const QColor& c : bad) {
        EXPECT_FALSE(set("Helv", 12.0f, c)) << kB3M << "colour #" << i << " was accepted";
        EXPECT_EQ(bytes(), before) << kB3M << "rejecting colour #" << i
                                   << " modified the document";
        ++i;
    }
}

TEST_F(FormFieldDefaultAppearanceModel, NanThroughNonExtendedFromRgbFNeverReachesTheDa) {
    // Intent: a NaN colour component must never reach the field's /DA.
    //
    // The input cannot be built with QColor::fromRgbF(NaN, 0, 0): a NaN fails
    // both of fromRgbF's range comparisons, so it takes Qt's ORDINARY RGB path,
    // and in this Qt 6.10 debug build that path asserts !isnan and aborts the
    // process before any product code runs. We therefore deliver the NaN through
    // the EXTENDED-RGB representation, the only QColor spec that actually retains
    // a non-finite component -- and the exact spec FormManager::
    // defaultAppearanceRgb guards with std::isfinite. A component outside [0, 1]
    // is merely the vehicle that forces that spec; the NaN in the first (red)
    // component is what the setter's isfinite check rejects first.
    //
    // The setter must reject it and leave the document byte-identical, so no
    // non-finite number can ever be written to the /DA.
    createText();
    const std::string before = stableBytes();
    const QColor c =
        QColor::fromRgbF(std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.0f);
    ASSERT_EQ(c.spec(), QColor::ExtendedRgb)
        << "harness: expected the extended-RGB path that preserves the NaN";
    ASSERT_TRUE(std::isnan(c.redF()))
        << "harness: the NaN did not survive into the colour";
    EXPECT_FALSE(set("Helv", 12.0f, c))
        << kB3M << "a NaN colour component was accepted";
    EXPECT_EQ(bytes(), before)
        << kB3M << "rejecting a NaN colour modified the document";
    const std::string da = own().da;
    EXPECT_TRUE(da.find("nan") == std::string::npos &&
                da.find("inf") == std::string::npos &&
                da.find("NAN") == std::string::npos &&
                da.find("INF") == std::string::npos)
        << kB3M << "a non-finite number reached the /DA: '" << da << "'";
}

TEST_F(FormFieldDefaultAppearanceModel, OtherColourSpecsAreConvertedToRgb) {
    createText();
    EXPECT_TRUE(set("Helv", 12.0f, QColor::fromHsv(240, 255, 255))) << kB3M << "HSV blue";
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 0 1 rg") << kB3M;
    EXPECT_TRUE(set("Helv", 12.0f, QColor::fromCmykF(0.0f, 1.0f, 1.0f, 0.0f)))
        << kB3M << "CMYK red";
    EXPECT_EQ(own().da, "/Helv 12 Tf 1 0 0 rg") << kB3M;
    EXPECT_TRUE(set("Helv", 12.0f, QColor::fromRgbF(1.0f, 0.0f, 0.0f)))
        << kB3M << "an in-range RGB-float colour";
    EXPECT_EQ(own().da, "/Helv 12 Tf 1 0 0 rg") << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, NonVariableTextWidgetsAndBadIndicesAreRejected) {
    for (FormFieldType t : {FormFieldType::Checkbox, FormFieldType::RadioButton,
                            FormFieldType::PushButton, FormFieldType::Signature}) {
        undo_->execute(std::make_unique<CreateFormFieldCommand>(
            manager_.get(), page_.get(), t, rectFor(t), "button"));
        ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u) << "harness";
        const std::string before = stableBytes();
        EXPECT_FALSE(set("Helv", 12.0f, QColor(Qt::black))) << kB3M << typeName(t);
        EXPECT_EQ(bytes(), before) << kB3M << typeName(t) << ": document modified";
        undo_->undo();
    }
    createText();
    const std::string before = stableBytes();
    for (int idx : {-1, 1, 7, std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max()}) {
        EXPECT_FALSE(manager_->setFieldDefaultAppearance(page_.get(), idx, "Helv", 12.0f,
                                                         QColor(Qt::black)))
            << kB3M << "index " << idx;
        EXPECT_FALSE(manager_->getFieldOwnDefaultAppearance(page_.get(), idx).has_value())
            << kB3M << "index " << idx;
        EXPECT_FALSE(manager_->restoreFieldOwnDefaultAppearance(
            page_.get(), idx, FormFieldOwnDefaultAppearance{}))
            << kB3M << "index " << idx;
    }
    EXPECT_FALSE(manager_->setFieldDefaultAppearance(nullptr, 0, "Helv", 12.0f,
                                                     QColor(Qt::black)))
        << kB3M << "null page";
    EXPECT_EQ(bytes(), before) << kB3M << "a bad index modified the document";
}

// ─── The command ─────────────────────────────────────────────────────────────

TEST_F(FormFieldDefaultAppearanceModel, CommandUndoAndRedoRestoreTheExactDaStrings) {
    createText();
    // An unusual but valid /DA (odd spacing, ".5"), plus a /DS that MuPDF's
    // setter deletes: undo must put back both, byte for byte.
    const std::string original = "  /TiRo   11 Tf 0 0 .5 rg ";
    authorOwnDaBytes(ctx_.get(), page_.get(), original.data(), original.size());
    authorOwnDs(ctx_.get(), page_.get(), "font: 11pt Times");
    manager_->setFieldValue(page_.get(), 0, "abc");

    auto cmd = std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Cour", 14.0f, QColor(255, 0, 0));
    SetFieldAppearanceCommand* raw = cmd.get();
    undo_->execute(std::move(cmd));
    EXPECT_TRUE(raw->applied()) << kB3M;
    EXPECT_EQ(own().da, "/Cour 14 Tf 1 0 0 rg") << kB3M << "execute";
    EXPECT_FALSE(own().hasDs) << kB3M << "MuPDF's setter is expected to delete /DS";
    const std::string afterBytes = bytes();

    undo_->undo();
    FormFieldOwnDefaultAppearance s = own();
    EXPECT_TRUE(s.hasDa) << kB3M << "undo";
    EXPECT_EQ(s.da, original) << kB3M << "undo did not restore the exact /DA";
    EXPECT_TRUE(s.hasDs) << kB3M << "undo did not restore /DS";
    EXPECT_EQ(s.ds, "font: 11pt Times") << kB3M;
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "TiRo") << kB3M;
    EXPECT_FLOAT_EQ(manager_->getFormField(page_.get(), 0).daFontSize, 11.0f) << kB3M;

    undo_->redo();
    s = own();
    EXPECT_EQ(s.da, "/Cour 14 Tf 1 0 0 rg") << kB3M << "redo";
    EXPECT_FALSE(s.hasDs) << kB3M << "redo";
    EXPECT_EQ(bytes(), afterBytes) << kB3M << "redo is not byte-identical to execute";

    undo_->undo();
    EXPECT_EQ(own().da, original) << kB3M << "second undo";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "abc") << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, CommandUndoOfAnInheritedDaRemovesTheOwnDa) {
    createText();
    setAcroFormDa(ctx_.get(), page_.get(), "/Cour 9 Tf 0 0 1 rg");
    removeOwnDa(ctx_.get(), page_.get());
    ASSERT_FALSE(own().hasDa) << "harness: own /DA still present";
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Cour")
        << "harness: the AcroForm /DA is not in effect";

    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 18.0f, QColor(255, 0, 0)));
    EXPECT_EQ(own().da, "/TiRo 18 Tf 1 0 0 rg") << kB3M;
    undo_->undo();
    EXPECT_FALSE(own().hasDa) << kB3M << "undo left an own /DA on a field that had none";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Cour")
        << kB3M << "the inherited /DA is not in effect again";
}

TEST_F(FormFieldDefaultAppearanceModel, CommandUndoOfANonStringDaRemovesIt) {
    // Documented: a non-string /DA (not valid PDF) is captured as absent, so
    // undo removes the entry rather than reproducing the invalid object.
    createText();
    authorOwnDaAsName(ctx_.get(), page_.get());
    ASSERT_FALSE(own().hasDa) << "harness: a name /DA must read as absent";
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Helv", 10.0f, QColor(Qt::black)));
    EXPECT_EQ(own().da, "/Helv 10 Tf 0 0 0 rg") << kB3M;
    undo_->undo();
    EXPECT_FALSE(own().hasDa) << kB3M;
    EXPECT_FALSE(widgetHasOwnDr(ctx_.get(), page_.get())) << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, RejectedCommandIsANoOpIncludingUndoAndRedo) {
    createText();
    const std::string before = stableBytes();
    auto cmd = std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Arial", 12.0f, QColor(255, 0, 0));
    // Observe the rejection BEFORE handing ownership away. A rejected command
    // (font "Arial" fails validation) reports failed(), so UndoManager::execute
    // does a bare return without pushing it -- which means the moved-from
    // unique_ptr DELETES the command. Reading cmd->applied() AFTER the move
    // would be a use-after-free. execute() is idempotent, so running it here and
    // again inside the UndoManager is a no-op either way.
    cmd->execute();
    EXPECT_FALSE(cmd->applied()) << kB3M;
    undo_->execute(std::move(cmd));
    EXPECT_EQ(bytes(), before) << kB3M << "execute";
    undo_->undo();
    EXPECT_EQ(bytes(), before) << kB3M << "undo";
    undo_->redo();
    EXPECT_EQ(bytes(), before) << kB3M << "redo";
}

TEST_F(FormFieldDefaultAppearanceModel, CommandsNeverMergeSoEachUndoRevertsOneChange) {
    createText();
    auto first = std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 10.0f, QColor(255, 0, 0));
    auto second = std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 20.0f, QColor(255, 0, 0));
    EXPECT_EQ(first->mergeId(), 0) << kB3M;
    EXPECT_FALSE(first->mergeWith(second.get())) << kB3M;
    undo_->execute(std::move(first));
    undo_->execute(std::move(second));
    EXPECT_EQ(undo_->undoStackSize(), 2) << kB3M << "the two changes were merged";
    EXPECT_EQ(own().da, "/TiRo 20 Tf 1 0 0 rg") << kB3M;
    undo_->undo();
    EXPECT_EQ(own().da, "/TiRo 10 Tf 1 0 0 rg") << kB3M << "first undo";
    undo_->undo();
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 g") << kB3M << "second undo";
    EXPECT_FALSE(undo_->canUndo()) << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, ExecuteIsIdempotentAndKeepsTheFirstSnapshot) {
    createText();
    SetFieldAppearanceCommand cmd(manager_.get(), page_.get(), 0, "Cour", 16.0f,
                                  QColor(0, 0, 255));
    cmd.execute();
    cmd.execute();
    EXPECT_EQ(own().da, "/Cour 16 Tf 0 0 1 rg") << kB3M;
    cmd.undo();
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 g")
        << kB3M << "a second execute re-took the undo snapshot";
}

// ─── Save, delete+undo, undo/redo of create ──────────────────────────────────

TEST_F(FormFieldDefaultAppearanceModel, DaSurvivesSaveAndReopen) {
    createText();
    ASSERT_TRUE(set("TiRo", 18.0f, QColor(255, 0, 0))) << kB3M;
    manager_->setFieldValue(page_.get(), 0, "HHHH");
    saveAndReopen();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u) << "harness";
    EXPECT_EQ(own().da, "/TiRo 18 Tf 1 0 0 rg") << kB3M << "after save+reopen";
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daFontName, "TiRo") << kB3M;
    EXPECT_FLOAT_EQ(d.daFontSize, 18.0f) << kB3M;
    EXPECT_EQ(d.value, "HHHH") << kB3M;
    const QRect interior = pixelBox(kWideRect, kRenderScale).adjusted(9, 9, -9, -9);
    const InkStats ink = inkStats(renderOverWhite(doc_.get(), 0, kRenderScale), interior);
    ASSERT_GT(ink.count, 0) << kB3M << "no value ink after reopen";
    EXPECT_GT(static_cast<double>(ink.red) / ink.count, 0.8)
        << kB3M << "the reopened field's value is not drawn red";
}

TEST_F(FormFieldDefaultAppearanceModel, DeleteAndUndoKeepsDaMkAndQ) {
    createText();
    authorFirstWidgetLook(ctx_.get(), page_.get(), true);  // red /BC, yellow /BG, 2pt dashed
    ASSERT_TRUE(set("Cour", 14.0f, QColor(0, 0, 255))) << kB3M;
    setQuadding(ctx_.get(), page_.get(), 2);
    manager_->setFieldValue(page_.get(), 0, "abc");
    ASSERT_EQ(manager_->getFormField(page_.get(), 0).quadding, 2) << "harness: /Q";

    deleteAndUndo();

    EXPECT_EQ(own().da, "/Cour 14 Tf 0 0 1 rg") << kB3M << "delete+undo lost the /DA";
    EXPECT_EQ(readOwnQ(ctx_.get(), page_.get()), 2) << kB3M << "delete+undo lost /Q";
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.quadding, 2) << kB3M;
    EXPECT_EQ(d.value, "abc") << kB3M;
    const MkBsReadback mk = readFirstWidgetMkBs(ctx_.get(), page_.get());
    ASSERT_TRUE(mk.hasMk) << kB3M << "delete+undo lost /MK";
    ASSERT_EQ(mk.bcLen, 3) << kB3M;
    ASSERT_EQ(mk.bgLen, 3) << kB3M;
    EXPECT_FLOAT_EQ(mk.bc[0], 1.0f) << kB3M << "/MK /BC";
    EXPECT_FLOAT_EQ(mk.bc[1], 0.0f) << kB3M << "/MK /BC";
    EXPECT_FLOAT_EQ(mk.bg[2], 0.8f) << kB3M << "/MK /BG";
    EXPECT_TRUE(mk.hasBs) << kB3M;
    EXPECT_FLOAT_EQ(mk.bsWidth, 2.0f) << kB3M;
    EXPECT_EQ(mk.bsStyle, 'D') << kB3M;
    // The regenerated value is right-aligned: its ink sits in the right half.
    const QRect box = pixelBox(kWideRect, kRenderScale);
    const InkStats ink = inkStats(renderOverWhite(doc_.get(), 0, kRenderScale),
                                  box.adjusted(12, 12, -12, -12));
    ASSERT_GT(ink.count, 0) << kB3M << "no value ink after delete+undo";
    EXPECT_GT(ink.centroidX(), box.center().x())
        << kB3M << "the restored value is not right-aligned";
}

TEST_F(FormFieldDefaultAppearanceModel, UndoRedoOfCreateKeepsDaAndQ) {
    ASSERT_EQ(createViaCommand(FormFieldType::Text, kWideRect, "da_create"), 0) << "harness";
    ASSERT_TRUE(set("TiRo", 20.0f, QColor(0, 128, 0))) << kB3M;
    const std::string da = own().da;
    setQuadding(ctx_.get(), page_.get(), 1);
    undo_->undo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 0u) << "harness";
    undo_->redo();
    ASSERT_EQ(manager_->getFormFields(page_.get()).size(), 1u) << "harness";
    EXPECT_EQ(own().da, da) << kB3M << "undo/redo of create lost the /DA";
    EXPECT_EQ(readOwnQ(ctx_.get(), page_.get()), 1) << kB3M << "undo/redo lost /Q";
}

TEST_F(FormFieldDefaultAppearanceModel, QuaddingZeroIsNotWrittenOnRestore) {
    createText();
    deleteAndUndo();
    EXPECT_EQ(readOwnQ(ctx_.get(), page_.get()), -1) << kB3M << "a /Q 0 was added";
}

TEST_F(FormFieldDefaultAppearanceModel, HandBuiltRestoreDataGetsTheDefaultDaAndASaneQ) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.rect = kWideRect;
    data.fieldName = "hand_built";
    data.daFontName = "TiRo";   // parsed fields alone do not drive the restore
    data.daFontSize = 30.0f;
    data.quadding = 7;          // out of range: not written
    ASSERT_EQ(manager_->restoreFormField(page_.get(), data), 0) << "harness";
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 g") << kB3M;
    EXPECT_EQ(readOwnQ(ctx_.get(), page_.get()), -1) << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, InheritedDaIsRestoredAsTheWidgetsOwn) {
    createText();
    setAcroFormDa(ctx_.get(), page_.get(), "/Cour 9 Tf 0 0 1 rg");
    removeOwnDa(ctx_.get(), page_.get());
    manager_->setFieldValue(page_.get(), 0, "xyz");
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    ASSERT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    ASSERT_EQ(d.defaultAppearanceString, "/Cour 9 Tf 0 0 1 rg") << kB3M;
    deleteAndUndo();
    EXPECT_EQ(own().da, "/Cour 9 Tf 0 0 1 rg") << kB3M;
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "Cour") << kB3M;
}

TEST_F(FormFieldDefaultAppearanceModel, ExoticDrFontDaIsKeptThroughDeleteAndUndo) {
    // A loaded field whose /DA names /F1, a non-base-14 font resource in the
    // AcroForm /DR. MuPDF's parsed font maps it to Helv, but the /DA string
    // itself is carried byte for byte. A widget-level /DR is NOT carried
    // (restoreFormField builds a new widget dictionary); the AcroForm /DR is
    // untouched by delete+undo, so the name still resolves.
    createText();
    addDrFontF1(ctx_.get(), page_.get(), false);
    addDrFontF1(ctx_.get(), page_.get(), true);
    const std::string exotic = "/F1 9 Tf 0 0 1 rg";
    authorOwnDaBytes(ctx_.get(), page_.get(), exotic.data(), exotic.size());
    manager_->setFieldValue(page_.get(), 0, "WWWW");
    ASSERT_TRUE(widgetHasOwnDr(ctx_.get(), page_.get())) << "harness: widget /DR";
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daFontName, "Helv") << kB3M << "MuPDF maps an unknown name to Helv";
    EXPECT_EQ(d.defaultAppearanceString, exotic) << kB3M;

    EXPECT_NO_THROW(deleteAndUndo()) << kB3M;
    EXPECT_EQ(own().da, exotic) << kB3M << "the exotic /DA was not kept";
    EXPECT_FALSE(widgetHasOwnDr(ctx_.get(), page_.get()))
        << kB3M << "documented: a widget-level /DR is not carried by delete+undo";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "WWWW") << kB3M;
    const QRect interior = pixelBox(kWideRect, kRenderScale).adjusted(9, 9, -9, -9);
    const QImage img = renderOverWhite(doc_.get(), 0, kRenderScale);
    long blue = 0;
    const QRect r = interior.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.blue() > 150 && c.red() < 100 && c.green() < 100) ++blue;
        }
    EXPECT_GT(blue, 50) << kB3M << "the restored value is not drawn in the /DA blue";
}

TEST_F(FormFieldDefaultAppearanceModel, HostileDaStringIsCarriedByteForByteWithoutThrowing) {
    createText();
    const std::string hostile("/NoSuch 1e30 Tf nan inf -5 rg ) Tj (\0tail", 41);
    ASSERT_EQ(hostile.size(), 41u) << "harness";
    authorOwnDaBytes(ctx_.get(), page_.get(), hostile.data(), hostile.size());
    EXPECT_NO_THROW(manager_->setFieldValue(page_.get(), 0, "abc")) << kB3M;
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.defaultAppearanceString, hostile) << kB3M;
    EXPECT_TRUE(std::isfinite(d.daFontSize)) << kB3M;
    EXPECT_LE(d.daFontSize, 1000.0f) << kB3M;
    EXPECT_NO_THROW(deleteAndUndo()) << kB3M;
    EXPECT_EQ(own().da, hostile) << kB3M << "the /DA bytes changed through delete+undo";
    EXPECT_EQ(own().da.size(), 41u) << kB3M << "the embedded NUL truncated the /DA";
    // The command's undo also puts back the hostile bytes exactly.
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Helv", 12.0f, QColor(Qt::black)));
    EXPECT_EQ(own().da, "/Helv 12 Tf 0 0 0 rg") << kB3M;
    EXPECT_NO_THROW(undo_->undo()) << kB3M;
    EXPECT_EQ(own().da, hostile) << kB3M << "command undo";
}

TEST_F(FormFieldDefaultAppearanceModel, OverlongDaIsCapturedAsItsParsedValues) {
    // getFormField runs on every live-edit repaint: a crafted megabyte /DA is
    // not copied verbatim. Past kMaxCapturedDefaultAppearanceBytes the parsed
    // values (all MuPDF's 100-byte parser reads) are captured, re-printed.
    createText();
    std::string longDa = "/TiRo 18 Tf 1 0 0 rg";
    longDa.append(1 << 20, ' ');
    authorOwnDaBytes(ctx_.get(), page_.get(), longDa.data(), longDa.size());
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    EXPECT_LE(d.defaultAppearanceString.size(),
              pdfe::forms::kMaxCapturedDefaultAppearanceBytes)
        << kB3M << "the overlong /DA was copied into FormFieldData";
    EXPECT_EQ(d.defaultAppearanceString, "/TiRo 18 Tf 1 0 0 rg") << kB3M;
    deleteAndUndo();
    EXPECT_EQ(own().da, "/TiRo 18 Tf 1 0 0 rg") << kB3M << "after delete+undo";

    // Exactly at the cap it is still carried verbatim.
    std::string atCap = "/Cour 9 Tf 0 g";
    atCap.append(pdfe::forms::kMaxCapturedDefaultAppearanceBytes - atCap.size(), ' ');
    authorOwnDaBytes(ctx_.get(), page_.get(), atCap.data(), atCap.size());
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).defaultAppearanceString, atCap) << kB3M;
}

// ─── The live edit paint follows a /DA change made mid-edit ──────────────────

class FormFieldDefaultAppearanceEditPaint : public FormFieldEditPaintModel {
protected:
    // Commit by clicking outside the field, as the user does.
    void clickOutside() {
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->onMousePress(&ev, 0, QPointF(5.0, 5.0));
    }
};

TEST_F(FormFieldDefaultAppearanceEditPaint, SetterMidEditIsPaintedLiveAndKeepsTheBuffer) {
    beginEdit();
    type(QStringLiteral("HHHH"));
    const InkStats before = textInk(paint(2.0f), 2.0f);
    ASSERT_GT(before.count, 0) << "harness: no live ink before the change";
    EXPECT_EQ(before.red, 0) << kB3M << "the default /DA is black";

    ASSERT_TRUE(manager_->setFieldDefaultAppearance(page_.get(), 0, "Helv", 24.0f,
                                                    QColor(255, 0, 0)))
        << kB3M;
    EXPECT_TRUE(tool_->isEditingField()) << kB3M << "the change ended the edit";
    EXPECT_EQ(tool_->editBufferForTesting(), "HHHH") << kB3M << "the buffer changed";

    const InkStats after = textInk(paint(2.0f), 2.0f);
    ASSERT_GT(after.count, 0) << kB3M << "no live ink after the change";
    const double ratio = static_cast<double>(after.height()) / before.height();
    EXPECT_NEAR(ratio, 2.0, 0.35) << kB3M << "live ink height " << before.height()
                                  << " -> " << after.height() << " for 12 -> 24 pt";
    EXPECT_GT(static_cast<double>(after.red) / after.count, 0.8)
        << kB3M << "the live ink did not turn red";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "")
        << kB3M << "the value was written before commit";

    clickOutside();
    EXPECT_FALSE(tool_->isEditingField()) << "harness: the click did not commit";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).value, "HHHH") << kB3M;
    const auto own = manager_->getFieldOwnDefaultAppearance(page_.get(), 0);
    ASSERT_TRUE(own.has_value()) << "harness";
    EXPECT_EQ(own->da, "/Helv 24 Tf 1 0 0 rg") << kB3M << "commit changed the /DA";
}

TEST_F(FormFieldDefaultAppearanceEditPaint, CommandAndItsUndoMidEditRepaintLive) {
    beginEdit();
    type(QStringLiteral("HHHH"));
    const InkStats before = textInk(paint(2.0f), 2.0f);
    ASSERT_GT(before.count, 0) << "harness";

    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Helv", 24.0f, QColor(255, 0, 0)));
    const InkStats changed = textInk(paint(2.0f), 2.0f);
    EXPECT_GT(static_cast<double>(changed.red) / std::max<long>(changed.count, 1), 0.8)
        << kB3M << "the command's /DA is not painted live";
    EXPECT_GT(changed.height(), before.height() + before.height() / 2) << kB3M;

    undo_->undo();
    EXPECT_TRUE(tool_->isEditingField()) << kB3M << "undo ended the edit";
    EXPECT_EQ(tool_->editBufferForTesting(), "HHHH") << kB3M << "undo changed the buffer";
    const InkStats restored = textInk(paint(2.0f), 2.0f);
    EXPECT_EQ(restored.red, 0) << kB3M << "the undone /DA is still painted red";
    EXPECT_NEAR(restored.height(), before.height(), 1) << kB3M;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Pipeline form-textfield, phase 4, remediation (claim-check + security audit).
//
//   FormFieldDefaultAppearanceRemediation --
//     * the /RC that MuPDF's setter deletes is put back byte for byte by
//       SetFieldAppearanceCommand's undo, together with /DS, and removed
//       again by redo (claim-check finding 1);
//     * the /Parent step of the /DA capture lookup (own, then /Parent, then
//       AcroForm): a /DA held only by the parent field, with a DIFFERENT
//       AcroForm /DA, is the one captured and restored as the widget's own by
//       delete+undo, and a command's undo makes it inherited again
//       (claim-check finding 2);
//     * an over-cap /DA whose font name is long is re-printed whole, never
//       truncated, and so is one with the longest numbers (security audit).
//
// Every assertion runs unconditionally. No DISABLED_, GTEST_SKIP or
// expected-fail marker. Lambdas passed to MuPdfContext::execute capture and
// declare only trivially-destructible values (fz_try discipline).
// ════════════════════════════════════════════════════════════════════════════

namespace {

void authorOwnRc(MuPdfContext* ctx, const Page* page, const char* rc) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, rc](fz_context* c) {
        pdf_dict_put_text_string(c, obj, PDF_NAME(RC), rc);
    });
}

// Make the first widget a kid of a new parent field that holds `da`, and
// remove the widget's own /DA.
void moveDaToANewParent(MuPdfContext* ctx, const Page* page, const char* da) {
    pdf_obj* obj = firstWidgetObj(ctx, page);
    if (!obj) return;
    ctx->execute([obj, da](fz_context* c) {
        pdf_document* doc = pdf_get_bound_document(c, obj);
        pdf_obj* parent = pdf_add_new_dict(c, doc, 4);
        pdf_dict_put_name(c, parent, PDF_NAME(FT), "Tx");
        pdf_dict_put_text_string(c, parent, PDF_NAME(T), "parent_da");
        pdf_dict_put_text_string(c, parent, PDF_NAME(DA), da);
        pdf_obj* kids = pdf_dict_put_array(c, parent, PDF_NAME(Kids), 1);
        pdf_array_push(c, kids, obj);
        pdf_dict_put(c, obj, PDF_NAME(Parent), parent);
        pdf_dict_del(c, obj, PDF_NAME(DA));
        pdf_drop_obj(c, parent);
    });
}

constexpr const char* kParentDa = "/TiRo 15 Tf 0 0.5 0 rg";
constexpr const char* kOtherAcroFormDa = "/Cour 9 Tf 0 0 1 rg";

class FormFieldDefaultAppearanceRemediation : public FormFieldDefaultAppearanceModel {
protected:
    // A text field whose /DA lives only on its parent, with a different
    // AcroForm /DA, so a lookup that skips /Parent lands on Cour, not TiRo.
    void createParentOnlyDaField() {
        createText("kid");
        setAcroFormDa(ctx_.get(), page_.get(), kOtherAcroFormDa);
        moveDaToANewParent(ctx_.get(), page_.get(), kParentDa);
        ASSERT_FALSE(own().hasDa) << "harness: the widget still has its own /DA";
        ASSERT_EQ(manager_->getFormField(page_.get(), 0).daFontName, "TiRo")
            << "harness: MuPDF does not see the parent's /DA";
    }
};

TEST_F(FormFieldDefaultAppearanceRemediation, CommandUndoRestoresDsAndRcByteExactlyAndRedoRemovesThem) {
    createText();
    const std::string da = "/Helv 10 Tf 0 g";
    authorOwnDaBytes(ctx_.get(), page_.get(), da.data(), da.size());
    authorOwnDs(ctx_.get(), page_.get(), "font: 10pt Helvetica; color: #000000");
    authorOwnRc(ctx_.get(), page_.get(), "<body><p>rich</p></body>");
    {
        const FormFieldOwnDefaultAppearance s = own();
        ASSERT_TRUE(s.hasDa && s.hasDs && s.hasRc) << "harness: /DA, /DS, /RC not authored";
    }

    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "TiRo", 12.0f, QColor(255, 0, 0)));
    FormFieldOwnDefaultAppearance s = own();
    EXPECT_EQ(s.da, "/TiRo 12 Tf 1 0 0 rg") << kB3M << "execute";
    EXPECT_FALSE(s.hasDs) << kB3M << "MuPDF's setter is expected to delete /DS";
    EXPECT_FALSE(s.hasRc) << kB3M << "MuPDF's setter is expected to delete /RC";

    undo_->undo();
    s = own();
    EXPECT_TRUE(s.hasDa) << kB3M << "undo: /DA";
    EXPECT_EQ(s.da, da) << kB3M << "undo: /DA bytes";
    EXPECT_TRUE(s.hasDs) << kB3M << "undo did not restore /DS";
    EXPECT_EQ(s.ds, "font: 10pt Helvetica; color: #000000") << kB3M << "undo: /DS bytes";
    EXPECT_TRUE(s.hasRc) << kB3M << "undo did not restore /RC";
    EXPECT_EQ(s.rc, "<body><p>rich</p></body>") << kB3M << "undo: /RC bytes";

    undo_->redo();
    s = own();
    EXPECT_EQ(s.da, "/TiRo 12 Tf 1 0 0 rg") << kB3M << "redo: /DA";
    EXPECT_FALSE(s.hasDs) << kB3M << "redo left /DS";
    EXPECT_FALSE(s.hasRc) << kB3M << "redo left /RC";
}

TEST_F(FormFieldDefaultAppearanceRemediation, ParentOnlyDaIsCapturedAndRestoredAsTheWidgetsOwn) {
    createParentOnlyDaField();
    manager_->setFieldValue(page_.get(), 0, "abc");
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    EXPECT_EQ(d.defaultAppearanceString, kParentDa)
        << kB3M << "the capture skipped /Parent (the AcroForm /DA is " << kOtherAcroFormDa << ")";

    deleteAndUndo();
    const FormFieldOwnDefaultAppearance s = own();
    EXPECT_TRUE(s.hasDa) << kB3M << "delete+undo: no own /DA";
    EXPECT_EQ(s.da, kParentDa) << kB3M << "delete+undo did not restore the parent's /DA";
    const FormFieldData r = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(r.daFontName, "TiRo") << kB3M;
    EXPECT_FLOAT_EQ(r.daFontSize, 15.0f) << kB3M;
    EXPECT_EQ(r.value, "abc") << kB3M;
}

TEST_F(FormFieldDefaultAppearanceRemediation, CommandUndoOnAParentOnlyDaInheritsTheParentAgain) {
    createParentOnlyDaField();
    undo_->execute(std::make_unique<SetFieldAppearanceCommand>(
        manager_.get(), page_.get(), 0, "Cour", 20.0f, QColor(0, 0, 255)));
    EXPECT_EQ(own().da, "/Cour 20 Tf 0 0 1 rg") << kB3M << "execute";
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).defaultAppearanceString,
              "/Cour 20 Tf 0 0 1 rg")
        << kB3M << "the widget's own /DA must win over the parent's";

    undo_->undo();
    EXPECT_FALSE(own().hasDa) << kB3M << "undo left an own /DA";
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_EQ(d.daFontName, "TiRo") << kB3M << "the parent's /DA is not in effect again";
    EXPECT_EQ(d.defaultAppearanceString, kParentDa) << kB3M;

    undo_->redo();
    EXPECT_EQ(own().da, "/Cour 20 Tf 0 0 1 rg") << kB3M << "redo";
}

TEST_F(FormFieldDefaultAppearanceRemediation, OverCapDaWithALongFontNameIsReprintedWhole) {
    createText();
    std::string longDa = "/" + std::string(300, 'X') + " 18 Tf 1 0 0 rg";
    longDa.append(8192, ' ');
    authorOwnDaBytes(ctx_.get(), page_.get(), longDa.data(), longDa.size());
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    EXPECT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    // MuPDF's parser keeps 99 bytes of the /DA: the 300-character name alone
    // fills them, so the Tf operand is never reached and the parsed values
    // are its defaults, Helv 12 with no colour. The re-print is that, whole.
    EXPECT_EQ(d.defaultAppearanceString, "/Helv 12 Tf") << kB3M;
    EXPECT_LE(d.defaultAppearanceString.size(), 64u) << kB3M;

    // A long name that still lets the parser reach the operands.
    std::string named = "/" + std::string(40, 'Q') + " 18 Tf 1 0 0 rg";
    named.append(8192, ' ');
    authorOwnDaBytes(ctx_.get(), page_.get(), named.data(), named.size());
    EXPECT_EQ(manager_->getFormField(page_.get(), 0).defaultAppearanceString,
              "/Helv 18 Tf 1 0 0 rg")
        << kB3M << "an unknown long name is re-printed as Helv, untruncated";
    deleteAndUndo();
    EXPECT_EQ(own().da, "/Helv 18 Tf 1 0 0 rg") << kB3M << "after delete+undo";
}

TEST_F(FormFieldDefaultAppearanceRemediation, OverCapDaWithTheLongestNumbersIsReprintedWhole) {
    // The longest re-print: ZaDb, the 1000 pt cap and four CMYK components
    // at the smallest denormal. MuPDF's %g never uses exponent form, so each
    // prints as "." + 44 zeros + digits and the whole /DA is longer than the
    // 128-byte buffer this re-print first used (which truncated it).
    createText();
    std::string da = "/ZaDb 1000 Tf 1e-45 1e-45 1e-45 1e-45 k";
    da.append(8192, ' ');
    authorOwnDaBytes(ctx_.get(), page_.get(), da.data(), da.size());
    const FormFieldData d = manager_->getFormField(page_.get(), 0);
    ASSERT_TRUE(d.defaultAppearanceStringCaptured) << kB3M;
    const std::string& s = d.defaultAppearanceString;
    EXPECT_EQ(s.rfind("/ZaDb 1000 Tf ", 0), 0u) << kB3M << "'" << s << "'";
    ASSERT_GE(s.size(), 2u) << kB3M;
    EXPECT_EQ(s.substr(s.size() - 2), " k") << kB3M << "the re-print was truncated: '" << s << "'";
    EXPECT_EQ(std::count(s.begin(), s.end(), ' '), 7) << kB3M << "'" << s << "'";
    EXPECT_GT(s.size(), 128u) << kB3M << "this case no longer exceeds 128 bytes: '" << s << "'";
    EXPECT_LE(s.size(), 300u) << kB3M << "'" << s << "'";
    deleteAndUndo();
    EXPECT_EQ(own().da, s) << kB3M << "after delete+undo";
}

}  // namespace
