// -*- coding: utf-8 -*-
//
// PDF-48 (meta-audit gap register) / GitHub issue #18 —
// {RequiredFieldBlocksSubmit, PasswordFieldMasksDisplay}.
//
// THE ROW SPLITS. One half was already implemented and needed an oracle; the
// other was a product gap and needed a fix. Recording which is which matters
// more than the tests themselves, because the criterion is phrased as though
// both filters already pass, and neither name existed anywhere in tests/.
//
//   PasswordFieldMasksDisplay   COVERAGE GAP. FormFieldPropertiesPanel has set
//                               QLineEdit::Password from FormFieldData::
//                               isPassword since it was written, and
//                               FormManager parses that flag from
//                               PDF_TX_FIELD_IS_PASSWORD. Nothing asserted it.
//
//   RequiredFieldBlocksSubmit   PRODUCT GAP. FormManager parsed
//                               PDF_FIELD_IS_REQUIRED into
//                               FormFieldData::isRequired and the ONLY consumer
//                               was FormFieldPropertiesPanel appending the word
//                               "Required" to a label. src/formsubmit/ had zero
//                               references to it, so an empty required field
//                               submitted exactly like a filled one. Fixed by
//                               FormSubmitter::missingRequiredFields() +
//                               submitForm() + the FormFieldBridge conversion
//                               that carries the parsed flag across.

#include <gtest/gtest.h>

#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "formsubmit/FormEncoder.h"
#include "formsubmit/FormFieldBridge.h"
#include "formsubmit/FormSubmitter.h"
#include "formsubmit/HostAllowlist.h"
#include "ui/FormFieldPropertiesPanel.h"

#include <QLineEdit>
#include <QStackedWidget>
#include <QUrl>

#include <string>
#include <vector>

using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::formsubmit::FormField;
using pdfe::formsubmit::FormSubmitter;
using pdfe::formsubmit::HostAllowlist;
using pdfe::formsubmit::SubmitDecision;
using pdfe::formsubmit::SubmitFormat;
using pdfe::ui::FormFieldPropertiesPanel;

namespace {

FormFieldData textField(const std::string& name, const std::string& value)
{
    FormFieldData d;
    d.type = FormFieldType::Text;
    d.fieldName = name;
    d.value = value;
    return d;
}

}  // namespace

// ===========================================================================
// RequiredFieldBlocksSubmit — the product gap.
// ===========================================================================

// THE DEFECT ASSERTION. An empty Required field must stop the submit.
//
// THE CONTROL is in the same test and is the whole reason it is worth
// anything: the identical form, identical URL, identical everything except the
// Required field now has a value, must be ALLOWED. Without it a
// FormSubmitter that refused every submit — for any reason, including a
// mistake in the URL rules — would satisfy the refusal half perfectly.
TEST(FormFieldFlags, RequiredFieldBlocksSubmit)
{
    HostAllowlist allow;
    allow.allow("forms.example.com");
    const QUrl url("https://forms.example.com/collect");

    std::vector<FormField> incomplete = {
        FormField{"name", "Ada", false},
        FormField{"email", "", true},
    };
    std::vector<FormField> complete = {
        FormField{"name", "Ada", false},
        FormField{"email", "ada@example.com", true},
    };

    // Precondition, asserted rather than assumed: the two forms must differ
    // ONLY in the required field's value, otherwise the comparison below is
    // not attributable to the required-field rule.
    ASSERT_EQ(incomplete.size(), complete.size());
    ASSERT_EQ(incomplete[1].name, complete[1].name);
    ASSERT_TRUE(incomplete[1].required);
    ASSERT_TRUE(complete[1].required);

    // (1) The seam itself: WHICH field is missing, not merely that one is.
    const auto missing = FormSubmitter::missingRequiredFields(incomplete);
    ASSERT_EQ(missing.size(), 1u)
        << "expected exactly the empty required field to be reported";
    EXPECT_EQ(missing[0], "email");

    // CONTROL for the seam.
    EXPECT_TRUE(FormSubmitter::missingRequiredFields(complete).empty())
        << "a filled required field was reported as missing";

    // (2) The decision the UI asks for.
    EXPECT_EQ(FormSubmitter::shouldSubmit(url, allow, incomplete),
              SubmitDecision::RejectMissingRequiredField);
    EXPECT_EQ(FormSubmitter::shouldSubmit(url, allow, complete),
              SubmitDecision::Allow)
        << "the completed form was refused -- the refusal above is not "
           "attributable to the required-field rule";

    // (3) The rule as enforced by the submit path itself, which is what a
    //     caller that never asked shouldSubmit() is subject to. No network
    //     call can happen: the refusal returns before any request is made.
    FormSubmitter submitter;
    const auto refused =
        submitter.submitForm(url, incomplete, SubmitFormat::UrlEncoded, 1);
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(refused.httpStatus, 0)
        << "a refused submit must not have reached the network";
    EXPECT_TRUE(refused.errorMessage.contains("required"))
        << "the refusal did not name the reason; message was: "
        << refused.errorMessage.toStdString();
    EXPECT_TRUE(refused.errorMessage.contains("email"))
        << "the refusal did not name the offending field; message was: "
        << refused.errorMessage.toStdString();
}

// WHITESPACE IS NOT AN ANSWER. `value.empty()` is the obvious implementation
// and a single space defeats it, which makes the flag decoration again. This
// is a separate test because it is a separate claim: the one above passes with
// an emptiness-only check.
TEST(FormFieldFlags, RequiredFieldRejectsWhitespaceOnlyValue)
{
    const std::vector<FormField> spaces = {FormField{"email", "   ", true}};
    const std::vector<FormField> tabs = {FormField{"email", "\t\n", true}};
    const std::vector<FormField> real = {FormField{"email", " a ", true}};

    EXPECT_EQ(FormSubmitter::missingRequiredFields(spaces).size(), 1u)
        << "a required field containing only spaces was accepted as filled";
    EXPECT_EQ(FormSubmitter::missingRequiredFields(tabs).size(), 1u);

    // CONTROL. A value with real content among the whitespace is an answer;
    // an over-eager trim that rejected it would be a new defect.
    EXPECT_TRUE(FormSubmitter::missingRequiredFields(real).empty())
        << "a required field with actual content was rejected as blank";
}

// A NON-REQUIRED EMPTY FIELD MUST NOT BLOCK. Without this, "block whenever any
// field is empty" passes every assertion above while breaking every optional
// field in every form.
TEST(FormFieldFlags, OptionalEmptyFieldDoesNotBlockSubmit)
{
    const std::vector<FormField> fields = {
        FormField{"nickname", "", false},
        FormField{"email", "ada@example.com", true},
    };
    EXPECT_TRUE(FormSubmitter::missingRequiredFields(fields).empty())
        << "an EMPTY OPTIONAL field blocked the submit -- the rule is keyed on "
           "emptiness rather than on the Required flag";
}

// ORDERING. A blocked destination must stay a blocked destination whether or
// not the form is complete. If completeness were checked first, a hostile URL
// would be reported as a validation nag until the form was filled in, and the
// reason given for a security refusal would depend on unrelated state.
TEST(FormFieldFlags, BlockedDestinationOutranksMissingRequiredField)
{
    HostAllowlist allow;
    const QUrl metadata("http://169.254.169.254/latest/meta-data/");

    const std::vector<FormField> incomplete = {FormField{"email", "", true}};
    const std::vector<FormField> complete = {
        FormField{"email", "ada@example.com", true}};

    EXPECT_EQ(FormSubmitter::shouldSubmit(metadata, allow, incomplete),
              SubmitDecision::RejectBlockedDestination)
        << "an SSRF target was reported as an incomplete form";
    // CONTROL: the same URL with a complete form gives the SAME answer, i.e.
    // the reason does not depend on form state.
    EXPECT_EQ(FormSubmitter::shouldSubmit(metadata, allow, complete),
              SubmitDecision::RejectBlockedDestination);
}

// THE BRIDGE. The enforcement above is worth nothing if the flag never leaves
// the document model. FormManager parses PDF_FIELD_IS_REQUIRED into
// FormFieldData::isRequired; this asserts the conversion carries it.
TEST(FormFieldFlags, RequiredFlagSurvivesTheConversionToSubmitFields)
{
    std::vector<FormFieldData> docFields;
    FormFieldData req = textField("email", "");
    req.isRequired = true;
    docFields.push_back(req);
    docFields.push_back(textField("nickname", ""));

    const auto wire = pdfe::formsubmit::toSubmitFields(docFields);
    ASSERT_EQ(wire.size(), 2u);
    EXPECT_EQ(wire[0].name, "email");
    EXPECT_TRUE(wire[0].required)
        << "FormFieldData::isRequired did not reach the submit path -- the "
           "enforcement is a rule no data arrives at";
    // CONTROL: the flag is carried, not fabricated for every field.
    EXPECT_FALSE(wire[1].required)
        << "a field that is not required was marked required by the conversion";

    // End to end: the converted fields are what the enforcement then refuses.
    const auto missing = FormSubmitter::missingRequiredFields(wire);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "email");
}

// ===========================================================================
// PasswordFieldMasksDisplay — the coverage gap.
// ===========================================================================

class FormFieldFlagsPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel_ = std::make_unique<FormFieldPropertiesPanel>();
    }
    void TearDown() override { panel_.reset(); }
    std::unique_ptr<FormFieldPropertiesPanel> panel_;
};

// A password field must be displayed masked, and — the part that is easy to
// get wrong — the VALUE must be unchanged. Masking is a display property; a
// "fix" that scrubbed the stored value would satisfy a naive echo-mode
// assertion while losing the user's data on the next save.
//
// THE CONTROL is the same panel, same field, isPassword false: it must show
// Normal. Without it, a panel hardcoded to Password everywhere passes.
TEST_F(FormFieldFlagsPanelTest, PasswordFieldMasksDisplay)
{
    auto* edit = panel_->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(edit, nullptr) << "the single-line text editor is not in the panel";

    FormFieldData secret = textField("pin", "hunter2");
    secret.isPassword = true;
    panel_->setFormField(secret);

    auto* stack = panel_->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    ASSERT_EQ(stack->currentIndex(), 0)
        << "the panel is not showing the single-line editor, so its echo mode "
           "is not what the user sees";

    EXPECT_EQ(edit->echoMode(), QLineEdit::Password)
        << "a field flagged as a password is displayed in clear text";

    // The value itself is untouched — masking is display-only.
    EXPECT_EQ(edit->text(), QString("hunter2"))
        << "masking altered the stored value rather than only its display";

    // CONTROL: an ordinary text field in the SAME panel must be Normal, which
    // also proves the mode is re-evaluated per field rather than latched on.
    panel_->setFormField(textField("nickname", "ada"));
    EXPECT_EQ(edit->echoMode(), QLineEdit::Normal)
        << "the panel stayed in password mode after being shown an ordinary "
           "text field -- a masked field leaks into the next selection, or the "
           "panel is hardcoded and the assertion above proves nothing";
    EXPECT_EQ(edit->text(), QString("ada"));
}

// The masking must also survive the reverse order (ordinary field first, then
// password), because "set to Password once and never reset" and "set to Normal
// once and never set" are two different single-direction bugs and one test can
// only see one of them.
TEST_F(FormFieldFlagsPanelTest, PasswordMaskingIsAppliedInEitherOrder)
{
    auto* edit = panel_->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(edit, nullptr);

    panel_->setFormField(textField("nickname", "ada"));
    ASSERT_EQ(edit->echoMode(), QLineEdit::Normal);

    FormFieldData secret = textField("pin", "hunter2");
    secret.isPassword = true;
    panel_->setFormField(secret);
    EXPECT_EQ(edit->echoMode(), QLineEdit::Password)
        << "the panel never left Normal echo mode after an ordinary field";
}

// ===========================================================================
// CombFieldRespectsMaxLenAndCellLayout — PDF-44 / GitHub issue #16.
// ===========================================================================
//
// THE ROW SPLITS, same as PDF-48 above.
//
//   Cell layout        COVERAGE GAP. MuPDF's appearance generator has always
//                      honoured bit 25 + /MaxLen (pdf_write_tx_widget_
//                      appearance -> write_comb_string), and PDFE reaches it
//                      through FormManager::setFieldValue -> pdf_update_annot.
//                      NOTHING asserted the geometry. Every prior forms oracle
//                      checked that the characters were PRESENT, which an
//                      ordinary left-aligned run satisfies just as well — so
//                      a regression that dropped the comb flag before
//                      appearance generation was invisible.
//
//   /MaxLen overflow   PRODUCT GAP. FormFieldData had no maxLen at all and
//                      setFieldValue wrote /V unconditionally. For a comb
//                      field /MaxLen is the CELL COUNT, so an over-long value
//                      kept stepping one cell width per character and drew
//                      itself outside the widget and outside the clip, while
//                      /V claimed the whole string. Fixed by
//                      truncateToCodepoints on the comb path in
//                      FormManager::setFieldValue (comb only — truncating a
//                      plain field's programmatic fill would destroy data
//                      rather than protect a layout).
//
// WHY THE GEOMETRY IS ASSERTED AND NOT THE TEXT. The fixture's two fields are
// identical but for bit 25, so "the six characters are there" is true of BOTH.
// The only thing that separates them is WHERE each character sits, and that is
// what this test measures: character i must be centred at (i + 0.5) * cellW.
//
// MUTATIONS KILLED (each confirmed RED, then reverted — see
// docs/audit/p84-remaining-open/phase-6/notes-forms.md):
//   * strip PDF_TX_FIELD_IS_COMB from /Ff before pdf_update_annot in
//     setFieldValue (comb collapses to ordinary left-aligned text)
//     -> the per-cell centre assertions AND the "comb AP must use a TJ array"
//        assertion fail.
//   * delete the truncateToCodepoints call -> the /MaxLen overflow assertion
//     fails (ten characters stored in a six-cell field).

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>

namespace {

using pdfe::forms::FormManager;

// The fixture's authored geometry (tests/fixtures/gen_form_comb_maxlen.py).
// These are INPUTS to the expectation, not values read back from the product.
constexpr int    kCombMaxLen   = 6;
constexpr double kCombFieldW   = 120.0;   // -> 20.0 per cell
constexpr double kCombBorderW  = 1.0;
constexpr double kCombFontSize = 10.0;

// Helvetica advance width of `c`, in 1/1000 text-space units — the same font
// metric MuPDF's appearance writer uses, obtained independently of it.
double HelveticaAdvanceThousandths(char c)
{
    double advance = 0.0;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return advance;
    fz_font* font = nullptr;
    fz_try(ctx) {
        font = fz_new_base14_font(ctx, "Helvetica");
        const int g = fz_encode_character(
            ctx, font, static_cast<int>(static_cast<unsigned char>(c)));
        advance = fz_advance_glyph(ctx, font, g, 0) * 1000.0;
    }
    fz_always(ctx) {
        fz_drop_font(ctx, font);
    }
    fz_catch(ctx) {
        advance = 0.0;
    }
    fz_drop_context(ctx);
    return advance;
}

// Read the widget appearance-stream bytes of the widget named `fieldName`
// from `path`, using a FRESH independent MuPDF context.
std::string ReadCombApBytes(const std::filesystem::path& path,
                            const std::string& fieldName,
                            bool* found)
{
    std::string out;
    *found = false;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;

    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    fz_buffer* buf = nullptr;
    bool located = false;

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, 0);
        for (pdf_annot* w = pdf_first_widget(ctx, page); w;
             w = pdf_next_widget(ctx, w)) {
            const char* label = pdf_annot_field_label(ctx, w);
            if (!label || fieldName != label) continue;
            located = true;
            pdf_obj* obj = pdf_annot_obj(ctx, w);
            pdf_obj* ap = pdf_dict_get(ctx, obj, PDF_NAME(AP));
            pdf_obj* n = ap ? pdf_dict_get(ctx, ap, PDF_NAME(N)) : nullptr;
            if (n && pdf_is_stream(ctx, n)) {
                buf = pdf_load_stream(ctx, n);
                unsigned char* data = nullptr;
                const size_t len = fz_buffer_storage(ctx, buf, &data);
                out.assign(reinterpret_cast<const char*>(data), len);
            }
            break;
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        out.clear();
        located = false;
    }
    fz_drop_context(ctx);
    *found = located;
    return out;
}

// Every "x y m x y l s" stroke in the appearance stream, reported by its
// starting x. For a comb widget these are the cell dividers; the border
// rectangle uses "re s" and so is not matched.
std::vector<double> ParseVerticalRuleXs(const std::string& ap)
{
    std::vector<double> xs;
    std::istringstream in(ap);
    std::string line;
    while (std::getline(in, line)) {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int consumed = -1;
        // %n as in ParseTdOrigin: sscanf counts ASSIGNMENTS, not literals, so
        // without it a line that merely starts with four numbers would match.
        if (std::sscanf(line.c_str(), "%lf %lf m %lf %lf l s%n",
                        &x0, &y0, &x1, &y1, &consumed) >= 4 && consumed > 0) {
            xs.push_back(x0);
        }
    }
    return xs;
}

struct TjGlyph {
    double offset = 0.0;   // the TJ number immediately before the glyph
    char   ch     = '\0';
};

// Parse the single "[ ... ] TJ" array a comb appearance emits, in order.
// Returns false when there is no TJ array at all.
bool ParseTjArray(const std::string& ap, std::vector<TjGlyph>& out)
{
    out.clear();
    const size_t close = ap.find("] TJ");
    if (close == std::string::npos) return false;
    const size_t open = ap.rfind('[', close);
    if (open == std::string::npos) return false;

    double pending = 0.0;
    size_t i = open + 1;
    while (i < close) {
        const char c = ap[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '(') {
            ++i;
            if (i < close && ap[i] == '\\') ++i;
            if (i >= close) return false;
            const char glyph = ap[i];
            ++i;
            if (i >= close || ap[i] != ')') return false;
            ++i;
            out.push_back(TjGlyph{pending, glyph});
            pending = 0.0;
            continue;
        }
        size_t j = i;
        while (j < close) {
            const char d = ap[j];
            if (std::isdigit(static_cast<unsigned char>(d)) || d == '-' ||
                d == '+' || d == '.' || d == 'e' || d == 'E') {
                ++j;
            } else {
                break;
            }
        }
        if (j == i) return false;
        pending = std::atof(ap.substr(i, j - i).c_str());
        i = j;
    }
    return true;
}

// The "x y Td" text-space origin of the appearance's single text run.
bool ParseTdOrigin(const std::string& ap, double* x, double* y)
{
    std::istringstream in(ap);
    std::string line;
    while (std::getline(in, line)) {
        double a = 0, b = 0;
        int consumed = -1;
        // %n is load-bearing: sscanf reports the number of ASSIGNMENTS, so
        // "0 0 0 RG" matches "%lf %lf Td" with a return of 2 even though the
        // literal never matched. Requiring %n to have been reached is what
        // makes the "Td" literal actually part of the test.
        if (std::sscanf(line.c_str(), "%lf %lf Td%n", &a, &b, &consumed) >= 2 &&
            consumed > 0) {
            *x = a;
            *y = b;
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(FormFieldFlags, CombFieldRespectsMaxLenAndCellLayout)
{
    const auto fixturePath =
        std::filesystem::path(TEST_FIXTURES_DIR) / "form_comb_maxlen.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixturePath))
        << "committed fixture missing: " << fixturePath
        << " (regenerate with tests/fixtures/gen_form_comb_maxlen.py)";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, fixturePath);
    ASSERT_NE(doc, nullptr);
    auto pagePtr = doc->page(0);
    ASSERT_NE(pagePtr, nullptr);

    FormManager mgr(ctx);
    auto fields = mgr.getFormFields(pagePtr.get());
    ASSERT_EQ(fields.size(), 2u) << "fixture authors exactly two fields.";

    int iComb = -1, iPlain = -1;
    for (const auto& f : fields) {
        if (f.fieldName == "CombSix")  iComb = f.widgetIndex;
        if (f.fieldName == "PlainSix") iPlain = f.widgetIndex;
    }
    ASSERT_GE(iComb, 0);
    ASSERT_GE(iPlain, 0);

    // ── The flag and the cap are parsed at all. ──────────────────────────
    const auto combBefore  = mgr.getFormField(pagePtr.get(), iComb);
    const auto plainBefore = mgr.getFormField(pagePtr.get(), iPlain);

    EXPECT_TRUE(combBefore.isComb)
        << "bit 25 of /Ff (1 << 24) was not parsed as the comb flag";
    EXPECT_NE(combBefore.fieldFlags & (1 << 24), 0)
        << "the raw flag word lost bit 25";
    EXPECT_EQ(combBefore.maxLen, kCombMaxLen)
        << "/MaxLen was not read; without it there is no cell count";

    EXPECT_FALSE(plainBefore.isComb)
        << "the CONTROL field must not be comb, or the comb assertions below "
           "would pass for a build that combs every text field";
    EXPECT_EQ(plainBefore.maxLen, kCombMaxLen)
        << "/MaxLen must be read for a plain field too -- the control differs "
           "from the comb field in bit 25 and nothing else";

    // ── /MaxLen is the CELL COUNT: an over-long value cannot be stored. ──
    // Ten characters into a six-cell field. The characters have differing
    // Helvetica advances (A/B 667, C/D 722, E 667, F 611), so the centring
    // assertions below cannot be satisfied by a fixed-pitch accident.
    mgr.setFieldValue(pagePtr.get(), iComb, "ABCDEFGHIJ");
    EXPECT_EQ(mgr.getFormField(pagePtr.get(), iComb).value,
              std::string("ABCDEF"))
        << "a comb field accepted more characters than it has cells; the "
           "overflow is drawn outside the widget and /V disagrees with every "
           "conforming reader";

    // The control keeps the whole value: the cap is a comb-layout invariant,
    // not a general truncation of programmatic fills.
    mgr.setFieldValue(pagePtr.get(), iPlain, "ABCDEFGHIJ");
    EXPECT_EQ(mgr.getFormField(pagePtr.get(), iPlain).value,
              std::string("ABCDEFGHIJ"))
        << "a NON-comb field's /MaxLen silently destroyed user data";

    // ── Cell layout, measured off the generated appearance stream. ───────
    auto tempDir = std::filesystem::temp_directory_path() / "pdfe_comb_maxlen";
    std::filesystem::create_directories(tempDir);
    const auto saved = pdfe::test::UniqueTempPath(tempDir, "comb");
    doc->saveAs(saved);

    bool foundComb = false;
    const std::string combAp = ReadCombApBytes(saved, "CombSix", &foundComb);
    ASSERT_TRUE(foundComb) << "CombSix widget not found in " << saved;
    ASSERT_FALSE(combAp.empty())
        << "no appearance stream was generated for the comb field, so there "
           "is no layout to measure";

    const double cellW = kCombFieldW / kCombMaxLen;               // 20.0 pt
    const double cellT = (kCombFieldW * 1000.0 / kCombFontSize)
                         / kCombMaxLen;                           // 2000 units

    // (a) MaxLen - 1 cell dividers, dividing the bordered interior into
    //     MaxLen equal cells. MuPDF insets them by the border width, so the
    //     i-th divider sits at b + i * (w - 2b) / MaxLen -- within 0.67pt of
    //     the ideal i * cellW at this border width.
    //     EXPECT rather than ASSERT on the count, deliberately: a mutant that
    //     collapses the comb must be allowed to reach the per-character
    //     centring assertions below rather than dying here, or those never
    //     get exercised and the geometry is unproven.
    const std::vector<double> dividers = ParseVerticalRuleXs(combAp);
    EXPECT_EQ(dividers.size(), static_cast<size_t>(kCombMaxLen - 1))
        << "a comb field must draw MaxLen-1 cell dividers; got "
        << dividers.size();
    const double interiorCell =
        (kCombFieldW - 2 * kCombBorderW) / kCombMaxLen;
    if (dividers.size() == static_cast<size_t>(kCombMaxLen - 1)) {
        for (int i = 1; i < kCombMaxLen; ++i) {
            const double expected = kCombBorderW + interiorCell * i;
            EXPECT_NEAR(dividers[static_cast<size_t>(i - 1)], expected, 0.01)
                << "cell divider " << i << " is not on the boundary between "
                   "cell " << (i - 1) << " and cell " << i;
            EXPECT_NEAR(dividers[static_cast<size_t>(i - 1)], cellW * i, 1.0)
                << "cell divider " << i << " does not line up with the glyph "
                   "cell boundary at " << (cellW * i);
        }
    }

    // (b) One character per cell, each CENTRED in its own cell. This is the
    //     property an ordinary left-aligned rendering cannot have.
    std::vector<TjGlyph> glyphs;
    ASSERT_TRUE(ParseTjArray(combAp, glyphs))
        << "the comb appearance has no [ ... ] TJ array, so the characters "
           "were not positioned per cell at all:\n"
        << combAp;
    ASSERT_EQ(glyphs.size(), static_cast<size_t>(kCombMaxLen))
        << "expected one positioned glyph per cell";

    double td_x = -1.0, td_y = -1.0;
    ASSERT_TRUE(ParseTdOrigin(combAp, &td_x, &td_y));
    EXPECT_NEAR(td_x, 0.0, 0.001)
        << "the comb text run must start at the field's left edge; MuPDF uses "
           "zero padding for comb so the first cell starts at x = 0";

    const char* kExpectedGlyphs = "ABCDEF";
    double pen = 0.0;   // thousandths of text space, from the run origin
    for (int i = 0; i < kCombMaxLen; ++i) {
        const TjGlyph& g = glyphs[static_cast<size_t>(i)];
        EXPECT_EQ(g.ch, kExpectedGlyphs[i])
            << "cell " << i << " holds the wrong character";

        // A TJ number n displaces the pen by -n/1000 of a text-space unit.
        pen += -g.offset;
        const double advance = HelveticaAdvanceThousandths(g.ch);
        ASSERT_GT(advance, 0.0) << "no Helvetica metric for '" << g.ch << "'";
        const double centre = pen + advance / 2.0;
        pen += advance;

        EXPECT_NEAR(centre, (i + 0.5) * cellT, 1.0)
            << "character '" << g.ch << "' is not centred in comb cell " << i
            << ": its centre is at " << (centre * kCombFontSize / 1000.0)
            << "pt but cell " << i << " is centred at "
            << ((i + 0.5) * cellW) << "pt";
    }

    // ── CONTROL: the same value in the same geometry WITHOUT bit 25. ─────
    bool foundPlain = false;
    const std::string plainAp = ReadCombApBytes(saved, "PlainSix", &foundPlain);
    ASSERT_TRUE(foundPlain) << "PlainSix widget not found in " << saved;
    ASSERT_FALSE(plainAp.empty());

    EXPECT_EQ(plainAp.find("] TJ"), std::string::npos)
        << "the NON-comb control was laid out with per-character TJ offsets; "
           "the comb assertions above are then satisfied by every text field "
           "and prove nothing about bit 25:\n"
        << plainAp;
    EXPECT_NE(plainAp.find(" Tj"), std::string::npos)
        << "the non-comb control must draw its value as one ordinary run";
    EXPECT_TRUE(ParseVerticalRuleXs(plainAp).empty())
        << "the non-comb control drew comb cell dividers";

    double plain_x = -1.0, plain_y = -1.0;
    ASSERT_TRUE(ParseTdOrigin(plainAp, &plain_x, &plain_y));
    EXPECT_NEAR(plain_x, 2 * kCombBorderW, 0.001)
        << "the non-comb control must start at the left border inset (left-"
           "aligned), not at a cell centre. Appearance stream:\n"
        << plainAp;
}
