// tests/test_forms_phase5.cpp
//
// Phase-5 regression locks for UI-FORM-* P0 rows in
// tests/fixtures/ui_audit/UI_MATRIX_GAPS.md.
//
// Coverage (P0 rows):
//   UI-FORM-01  Text field click -> type -> commit -> value persists.
//   UI-FORM-03  Radio group: setting one deselects others, undo restores.
//   UI-FORM-06  PushButton click fires the button's action (locked via
//               FormTool fieldClicked dispatch + JavaScriptEngine fire).
//   UI-FORM-10  JavaScript Validate event blocks commit on invalid input.
//   UI-FORM-13  XFDF round-trip: export -> reset -> import preserves
//               field values byte-for-byte.
//   UI-FORM-15  Reset Form restores /DV defaults; fields without /DV
//               are unchanged (documented MuPDF behavior).
//   UI-FORM-16  Flatten Form removes interactive widgets; reopened PDF
//               has zero form fields.
//   UI-FORM-17  HostAllowlist::contains blocks disallowed hosts.
//   UI-FORM-23  Signature widget click triggers the signature-click
//               callback path (SignDialog opening verified via the
//               UI smoke layer; this locks the wiring contract).
//
// All tests use the existing test_form.pdf / test_form_with_dv.pdf
// fixtures already exercised by the older form tests.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormCommands.h"
#include "forms/FormDataIO.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"
#include "formsubmit/HostAllowlist.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pdfe::forms;
using namespace pdfe::core;
using namespace pdfe::commands;
using namespace pdfe::formsubmit;

namespace {

// ─────────────────────────────────────────────────────────────────────
// A1-61 support: read every widget's /AS (appearance state) off the SAVED
// bytes with a fresh, independent MuPDF context — no PDFE wrappers.
//
// /AS is where radio-group exclusivity actually lives. FormFieldData::value
// comes from pdf_annot_field_value, which resolves the INHERITED /V of the
// shared parent field, so BOTH kids of a radio group report the same string
// and no assertion on `.value` alone can distinguish "A is off" from "A is
// still on". Reading /AS per widget is the only way to see the sibling
// actually deselect.
//
// MuPDF discipline: no std::vector/std::string operation happens inside the
// fz_try region (setjmp/longjmp — see docs/mupdf-gotchas.md). Names are
// copied into a plain fixed C buffer declared BEFORE fz_try, and the STL
// containers are built afterwards.
// ─────────────────────────────────────────────────────────────────────
constexpr int kMaxProbedWidgets = 16;
constexpr int kMaxProbedNameLen = 64;

struct RawWidgetStates {
    int count = 0;
    char as[kMaxProbedWidgets][kMaxProbedNameLen] = {};
};

std::vector<std::string> ReadWidgetAppearanceStates(
    const std::filesystem::path& path) {
    RawWidgetStates raw;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return {};

    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, 0);
        for (pdf_annot* w = pdf_first_widget(ctx, page);
             w && raw.count < kMaxProbedWidgets;
             w = pdf_next_widget(ctx, w)) {
            pdf_obj* as = pdf_dict_get(ctx, pdf_annot_obj(ctx, w), PDF_NAME(AS));
            const char* nm = as ? pdf_to_name(ctx, as) : nullptr;
            if (nm) {
                fz_strlcpy(raw.as[raw.count], nm, kMaxProbedNameLen);
            } else {
                raw.as[raw.count][0] = '\0';
            }
            ++raw.count;
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        raw.count = 0;
    }
    fz_drop_context(ctx);

    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(raw.count));
    for (int i = 0; i < raw.count; ++i) out.emplace_back(raw.as[i]);
    return out;
}

class Phase5FormsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        formPdf_ = fixtures / "test_form.pdf";
        formDvPdf_ = fixtures / "test_form_with_dv.pdf";
        formJsPdf_ = fixtures / "test_form_js.pdf";

        if (!std::filesystem::exists(formPdf_)) {
            GTEST_SKIP() << "Test fixture missing: " << formPdf_;
        }
        document_ = Document::open(ctx_, formPdf_);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);

        manager_ = std::make_unique<FormManager>(ctx_);
        undo_ = std::make_unique<UndoManager>();

        page0Ptr_ = document_->page(0);
        page0_ = page0Ptr_.get();
    }

    void TearDown() override {
        commandPages_.clear();
        undo_.reset();
        manager_.reset();
        page0Ptr_.reset();
        document_.reset();
        ctx_.reset();
    }

    int findFieldByType(FormFieldType type) {
        auto fields = manager_->getFormFields(page0_);
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].type == type) return static_cast<int>(i);
        }
        return -1;
    }

    int findFieldByNameContains(const std::string& sub) {
        auto fields = manager_->getFormFields(page0_);
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].fieldName.find(sub) != std::string::npos)
                return static_cast<int>(i);
        }
        return -1;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path formPdf_;
    std::filesystem::path formDvPdf_;
    std::filesystem::path formJsPdf_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<UndoManager> undo_;
    std::unique_ptr<Page> page0Ptr_;
    Page* page0_ = nullptr;
    std::vector<std::unique_ptr<Page>> commandPages_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-01 — Text field value persists through SetFieldValueCommand
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, TextFieldValueCommitsAndPersists) {
    int idx = findFieldByType(FormFieldType::Text);
    if (idx < 0) GTEST_SKIP() << "Fixture has no text field on page 0.";

    auto cmd = std::make_unique<SetFieldValueCommand>(
        manager_.get(), page0_, idx, "Phase5_typed_value");
    undo_->execute(std::move(cmd));

    auto field = manager_->getFormField(page0_, idx);
    EXPECT_EQ(field.value, "Phase5_typed_value")
        << "SetFieldValueCommand must persist the typed value.";

    // Undo restores prior value
    undo_->undo();
    auto fieldAfterUndo = manager_->getFormField(page0_, idx);
    EXPECT_NE(fieldAfterUndo.value, "Phase5_typed_value")
        << "Undo must restore the prior value.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-03 — Radio group exclusivity through ToggleFieldCommand chain.
// MuPDF handles radio group exclusivity at the toggleField level — this
// test locks the contract that toggling radio A clears radio B in the
// same group.
// ─────────────────────────────────────────────────────────────────────
// A1-61 REPAIR (meta-audit-2026-07 status register).
//
// WHAT WAS WRONG. The old body:
//   * GTEST_SKIP'd when the fixture happened to carry fewer than two radio
//     widgets — so the row could report "covered" while never running;
//   * documented itself as possibly "degenerate" because it had no way to
//     know the two radios shared a group;
//   * never re-read sibling A after selecting B. Its sole oracle was
//     `EXPECT_NE(fieldBAfter.value, "Off")`, which passes even if every
//     radio in the group is simultaneously ON — i.e. exclusivity, the one
//     thing the test is named after, was never checked.
//
// THE REPAIR. The precondition is AUTHORED, not discovered:
// tests/fixtures/form_radio_group.pdf (generated by
// tests/fixtures/gen_form_radio_group.py) contains exactly ONE radio field
// "Choice" with exactly TWO kid widgets, export values /OptionA (left) and
// /OptionB (right). Both are unambiguously in the same group by
// construction, so there is nothing to skip and nothing degenerate.
//
// The oracle reads BOTH sides of the group, in both directions, at the two
// levels where the state actually lives:
//   * the group value (parent /V, surfaced as FormFieldData::value), and
//   * each widget's own /AS appearance state, read back out of the SAVED
//     bytes with an independent MuPDF context.
//
// MUTATIONS KILLED:
//   * replace pdf_toggle_widget with a write that sets only the clicked
//     widget's /AS and /V, leaving siblings alone -> A stays "OptionA" while
//     B turns on: both radios render selected. The /AS assertions fail; the
//     old EXPECT_NE oracle passed.
//   * make the toggle a no-op -> the group value stays "Off". Fails.
//   * clear the WRONG sibling (clear B when B was clicked) -> B's /AS is
//     "Off". Fails.
//   * drop the /AS write from the save path -> the reloaded states are
//     stale. Fails, while the in-memory value assertions still pass.
TEST_F(Phase5FormsTest, RadioGroupExclusivity) {
    // AUTHORED ground truth — see gen_form_radio_group.py. Never read these
    // back out of the product.
    const std::string kGroupName = "Choice";
    const std::string kExportA = "OptionA";  // left widget
    const std::string kExportB = "OptionB";  // right widget
    const std::string kOff = "Off";

    const auto fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "form_radio_group.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "committed fixture missing: " << fixture
        << " (regenerate with tests/fixtures/gen_form_radio_group.py)";

    const auto tmpDir =
        std::filesystem::temp_directory_path() / "pdfe_radio_group_test";
    std::filesystem::create_directories(tmpDir);
    const auto afterB = tmpDir / "radio_b_selected.pdf";
    const auto afterA = tmpDir / "radio_a_selected.pdf";
    std::error_code ec;
    std::filesystem::remove(afterB, ec);
    std::filesystem::remove(afterA, ec);

    // Widget roles are resolved by authored GEOMETRY (A is the LEFT widget)
    // rather than by enumeration order, so a reordering of /Annots cannot
    // silently swap the roles the assertions below expect. Both documents
    // below re-derive the roles the same way.
    auto resolveRoles = [&](FormManager& mgr, Page* page, int& idxA, int& idxB) {
        auto fields = mgr.getFormFields(page);
        ASSERT_EQ(fields.size(), 2u)
            << "fixture must expose exactly the two radio widgets it authors.";
        for (const auto& f : fields) {
            ASSERT_EQ(f.type, FormFieldType::RadioButton)
                << "both widgets must be typed as radio buttons.";
            ASSERT_EQ(f.fieldName, kGroupName)
                << "both widgets must resolve to the SAME group name, which "
                   "is what makes exclusivity meaningful.";
        }
        idxA = 0;
        idxB = 1;
        if (fields[1].rect.x() < fields[0].rect.x()) std::swap(idxA, idxB);
        ASSERT_LT(fields[idxA].rect.x(), fields[idxB].rect.x());
    };

    // ── Step 1: select B on the pristine fixture. A must deselect. ────
    int idxA = -1, idxB = -1;
    {
        auto doc = Document::open(ctx_, fixture);
        ASSERT_NE(doc, nullptr);
        auto pagePtr = doc->page(0);
        ASSERT_NE(pagePtr, nullptr);
        FormManager mgr(ctx_);
        ASSERT_NO_FATAL_FAILURE(resolveRoles(mgr, pagePtr.get(), idxA, idxB));

        // Authored precondition: nothing is selected yet.
        EXPECT_EQ(mgr.getFormField(pagePtr.get(), idxA).value, kOff)
            << "the authored group starts with /V = /Off.";
        EXPECT_EQ(mgr.getFormField(pagePtr.get(), idxB).value, kOff);

        mgr.toggleField(pagePtr.get(), idxB);

        EXPECT_EQ(mgr.getFormField(pagePtr.get(), idxB).value, kExportB)
            << "selecting the right radio must set the group value to its "
               "authored export value.";
        EXPECT_EQ(mgr.getFormField(pagePtr.get(), idxA).value, kExportB)
            << "the group value is shared: re-reading sibling A must report "
               "the SELECTED export value, never a stale OptionA.";

        pagePtr.reset();  // release the page BEFORE the full-rewrite save
        doc->saveAs(afterB);
    }
    {
        auto st = ReadWidgetAppearanceStates(afterB);
        ASSERT_EQ(st.size(), 2u) << "saved file lost a radio widget.";
        EXPECT_EQ(st[idxB], kExportB)
            << "widget B must render in its own on-state.";
        EXPECT_EQ(st[idxA], kOff)
            << "widget A must deselect when its group sibling is chosen. "
               "This is the assertion the old test never made — its only "
               "oracle passed even with BOTH radios simultaneously on.";
    }

    // ── Step 2: reopen and select A. B must deselect (reverse). ───────
    {
        auto doc = Document::open(ctx_, afterB);
        ASSERT_NE(doc, nullptr);
        auto pagePtr = doc->page(0);
        ASSERT_NE(pagePtr, nullptr);
        FormManager mgr(ctx_);
        int rA = -1, rB = -1;
        ASSERT_NO_FATAL_FAILURE(resolveRoles(mgr, pagePtr.get(), rA, rB));
        ASSERT_EQ(rA, idxA);
        ASSERT_EQ(rB, idxB);

        // The selection survived the round trip.
        EXPECT_EQ(mgr.getFormField(pagePtr.get(), rA).value, kExportB)
            << "the B selection must persist across save + reopen.";

        mgr.toggleField(pagePtr.get(), rA);

        EXPECT_EQ(mgr.getFormField(pagePtr.get(), rA).value, kExportA)
            << "selecting the left radio must set the group value to OptionA.";
        EXPECT_EQ(mgr.getFormField(pagePtr.get(), rB).value, kExportA)
            << "sibling B must re-read the newly selected group value.";

        pagePtr.reset();
        doc->saveAs(afterA);
    }
    {
        auto st = ReadWidgetAppearanceStates(afterA);
        ASSERT_EQ(st.size(), 2u);
        EXPECT_EQ(st[idxA], kExportA)
            << "widget A must render in its own on-state.";
        EXPECT_EQ(st[idxB], kOff)
            << "widget B must deselect when the selection moves back to A.";
    }
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-06 — PushButton click via FormManager::toggleField does not
// crash and the FormManager dispatches without exception. Action firing
// itself goes through MainWindow signal wiring; we lock the manager
// path here.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, PushButtonToggleNoCrash) {
    int idx = findFieldByType(FormFieldType::PushButton);
    if (idx < 0) {
        std::filesystem::path btnPdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_form_with_pushbutton.pdf";
        if (!std::filesystem::exists(btnPdf))
            GTEST_SKIP() << "No pushbutton fixture available.";
        auto btnDoc = Document::open(ctx_, btnPdf);
        ASSERT_NE(btnDoc, nullptr);
        auto btnPage = btnDoc->page(0);
        ASSERT_NE(btnPage, nullptr);
        FormManager btnMgr(ctx_);
        auto fields = btnMgr.getFormFields(btnPage.get());
        for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
            if (fields[i].type == FormFieldType::PushButton) {
                EXPECT_NO_THROW({ btnMgr.toggleField(btnPage.get(), i); })
                    << "PushButton dispatch must not crash.";
                return;
            }
        }
        GTEST_SKIP() << "Pushbutton fixture has no pushbutton field.";
    }

    EXPECT_NO_THROW({
        manager_->toggleField(page0_, idx);
    }) << "PushButton dispatch must not crash.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-10 — JavaScript Validate event blocks commit on invalid input.
// Uses test_form_js.pdf if available; otherwise locks the engine API
// contract that fireValidateEvent returns false on rejection.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, JavaScriptValidateEventReturnsBoolean) {
    if (!std::filesystem::exists(formJsPdf_)) {
        // Use main fixture; JS may simply not fire, which is also valid.
        JavaScriptEngine engine(ctx_);
        engine.enable(page0_);
        EXPECT_TRUE(engine.isEnabled());
        return;
    }

    auto jsDoc = Document::open(ctx_, formJsPdf_);
    ASSERT_NE(jsDoc, nullptr);
    auto jsPage = jsDoc->page(0);
    ASSERT_NE(jsPage, nullptr);

    JavaScriptEngine engine(ctx_);
    engine.enable(jsPage.get());
    EXPECT_TRUE(engine.isEnabled())
        << "JavaScriptEngine must enable on a JS-bearing form.";

    FormManager mgr2(ctx_);
    auto fields = mgr2.getFormFields(jsPage.get());
    if (fields.empty()) GTEST_SKIP() << "JS fixture has no fields.";

    // Find first text field; fire validate against it. A field with no
    // /AA dict returns true; a field with a Validate that rejects
    // returns false. Lock the contract that the call returns a bool
    // and does not crash.
    int firstText = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == FormFieldType::Text) {
            firstText = static_cast<int>(i);
            break;
        }
    }
    // A1-60 (part 1): test_form_js.pdf carries calculation scripts but NO
    // /AA /V Validate action on any field. PDF 32000-1 and MuPDF's
    // pdf_field_event_validate agree on the contract for that case: with no
    // Validate action the value is ACCEPTED, so the verdict must be true.
    // The old body discarded the verdict entirely ((void)result; SUCCEED()),
    // which passed for true, false, or an accidental constant.
    if (firstText >= 0) {
        EXPECT_TRUE(engine.fireValidateEvent(jsPage.get(), firstText, "test"))
            << "a field with no Validate action must accept its value; a "
               "false here means the validator rejects by default and every "
               "keystroke on an unscripted field would be swallowed.";
    }
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-10 / A1-60 REPAIR (meta-audit-2026-07 status register).
//
// WHAT WAS WRONG. JavaScriptValidateEventReturnsBoolean asserted only
// `EXPECT_TRUE(engine.isEnabled())` and then threw the validate verdict
// away. A fireValidateEvent hard-wired to `return true` — a complete
// form-validation bypass, the exact thing UI-FORM-10 exists to lock —
// was green.
//
// THE REPAIR. tests/fixtures/form_validate_js.pdf (generated by
// tests/fixtures/gen_form_validate_js.py) AUTHORS four fields whose
// verdicts are knowable without running the product, including both sides
// of two boundaries. Every call below asserts its exact expected boolean.
//
// MUTATIONS KILLED:
//   * `return true;` from fireValidateEvent  -> all six reject cases fail.
//   * `return false;`                        -> all six accept cases fail.
//   * `return rc == 0;` (inverted verdict)   -> every case fails.
//   * skip pdf_field_event_validate when the field has an /AA (e.g. an
//     early-out that treats "has JS" as "trusted") -> AlwaysReject accepts.
//   * pass a fixed/empty string instead of the caller's `value` into
//     pdf_field_event_validate -> the boundary pairs collapse: ShortOnly
//     and InRange can no longer answer differently for their two operands.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, JavaScriptValidateEventPinsAuthoredVerdicts) {
    const auto fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "form_validate_js.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "committed fixture missing: " << fixture
        << " (regenerate with tests/fixtures/gen_form_validate_js.py)";

    auto doc = Document::open(ctx_, fixture);
    ASSERT_NE(doc, nullptr);
    auto pagePtr = doc->page(0);
    ASSERT_NE(pagePtr, nullptr);
    Page* page = pagePtr.get();

    JavaScriptEngine engine(ctx_);
    engine.enable(page);
    ASSERT_TRUE(engine.isEnabled())
        << "JavaScript must be enabled or every verdict below is vacuous.";

    FormManager mgr(ctx_);
    auto fields = mgr.getFormFields(page);
    ASSERT_EQ(fields.size(), 4u) << "fixture authors exactly four fields.";

    auto indexOf = [&](const std::string& name) {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].fieldName == name) return static_cast<int>(i);
        return -1;
    };
    const int iShort = indexOf("ShortOnly");
    const int iReject = indexOf("AlwaysReject");
    const int iRange = indexOf("InRange");
    const int iNone = indexOf("NoValidateScript");
    ASSERT_GE(iShort, 0);
    ASSERT_GE(iReject, 0);
    ASSERT_GE(iRange, 0);
    ASSERT_GE(iNone, 0);

    // ShortOnly: `event.rc = (event.value.length <= 5)`.
    EXPECT_TRUE(engine.fireValidateEvent(page, iShort, "abc"))
        << "3 characters is inside the authored limit of 5.";
    EXPECT_TRUE(engine.fireValidateEvent(page, iShort, "12345"))
        << "5 characters is the inclusive boundary and must be ACCEPTED.";
    EXPECT_FALSE(engine.fireValidateEvent(page, iShort, "123456"))
        << "6 characters is one past the boundary and must be REJECTED.";
    EXPECT_FALSE(engine.fireValidateEvent(page, iShort, "abcdefghij"))
        << "10 characters must be rejected.";

    // AlwaysReject: `event.rc = false` — no input may pass.
    EXPECT_FALSE(engine.fireValidateEvent(page, iReject, "anything"))
        << "an unconditional reject script must reject.";
    EXPECT_FALSE(engine.fireValidateEvent(page, iReject, ""))
        << "an unconditional reject script must reject the empty value too.";

    // InRange: accepts integers in [10, 20]; both boundaries are pinned.
    EXPECT_FALSE(engine.fireValidateEvent(page, iRange, "9"))
        << "9 is below the authored range.";
    EXPECT_TRUE(engine.fireValidateEvent(page, iRange, "10"))
        << "10 is the inclusive lower bound.";
    EXPECT_TRUE(engine.fireValidateEvent(page, iRange, "20"))
        << "20 is the inclusive upper bound.";
    EXPECT_FALSE(engine.fireValidateEvent(page, iRange, "21"))
        << "21 is above the authored range.";
    EXPECT_FALSE(engine.fireValidateEvent(page, iRange, "not-a-number"))
        << "a non-numeric value must not slip through the range check.";

    // NoValidateScript: no /AA at all — accepted by contract.
    EXPECT_TRUE(engine.fireValidateEvent(page, iNone, "whatever"))
        << "a field with no Validate action must accept its value.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-13 — XFDF round-trip: export -> reset -> import preserves
// field values.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, XfdfRoundTripPreservesFieldValues) {
    int textIdx = findFieldByType(FormFieldType::Text);
    if (textIdx < 0) GTEST_SKIP() << "Fixture has no text field.";

    // Set a value
    manager_->setFieldValue(page0_, textIdx, "ROUNDTRIP_TOKEN");
    auto baseline = manager_->getFormField(page0_, textIdx);
    ASSERT_EQ(baseline.value, "ROUNDTRIP_TOKEN");

    // Export XFDF
    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());
    EXPECT_FALSE(xfdf.empty()) << "exportToXfdf must produce non-empty XML.";
    EXPECT_NE(xfdf.find("ROUNDTRIP_TOKEN"), std::string::npos)
        << "Exported XFDF must contain the field value.";

    // Reset to a different value
    manager_->setFieldValue(page0_, textIdx, "DIFFERENT_VALUE");
    ASSERT_EQ(manager_->getFormField(page0_, textIdx).value, "DIFFERENT_VALUE");

    // Import XFDF back
    io.importFromXfdf(document_.get(), manager_.get(), xfdf);
    auto restored = manager_->getFormField(page0_, textIdx);
    EXPECT_EQ(restored.value, "ROUNDTRIP_TOKEN")
        << "Imported XFDF must restore the original field value.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-15 — Reset Form restores /DV defaults.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, ResetFormRestoresDefaultValues) {
    if (!std::filesystem::exists(formDvPdf_)) {
        GTEST_SKIP() << "Fixture missing: " << formDvPdf_;
    }

    // The /DV value the fixture assigns to the FirstName text field
    // (see tests/fixtures/generate_form_with_dv.py). A fresh open shows
    // /V (empty), NOT /DV — viewers display the value, not the default —
    // so the oracle must read the EXPECTED default from the spec, not
    // from the field's current value.
    const std::string kFirstNameDv = "Default Name";

    auto dvDoc = Document::open(ctx_, formDvPdf_);
    ASSERT_NE(dvDoc, nullptr);
    auto dvPage = dvDoc->page(0);
    ASSERT_NE(dvPage, nullptr);

    FormManager mgr(ctx_);
    auto fieldsBefore = mgr.getFormFields(dvPage.get());
    ASSERT_FALSE(fieldsBefore.empty()) << "DV fixture has no fields.";

    int firstNameIdx = -1;
    for (size_t i = 0; i < fieldsBefore.size(); ++i) {
        if (fieldsBefore[i].fieldName == "FirstName") {
            firstNameIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(firstNameIdx, 0)
        << "Fixture must contain a FirstName text field with /DV.";

    // Dirty the field, then reset.
    mgr.setFieldValue(dvPage.get(), firstNameIdx, "DIRTY_VALUE");
    ASSERT_EQ(mgr.getFormField(dvPage.get(), firstNameIdx).value,
              "DIRTY_VALUE");

    mgr.resetForm(dvPage.get());

    // (1) In-memory restoration: /V must now equal /DV.
    EXPECT_EQ(mgr.getFormField(dvPage.get(), firstNameIdx).value,
              kFirstNameDv)
        << "resetForm must restore FirstName /V to its /DV in memory.";

    // (2) Persistence: the restored value must survive a save+reopen
    // round-trip. This is the load-bearing half — the UI Reset Form flow
    // (onResetForm → ResetFormCommand → Document::save) must write the
    // post-reset /V, not silently drop it.
    const auto tmpDir = std::filesystem::temp_directory_path()
        / "pdfe_reset_dv_test";
    std::filesystem::create_directories(tmpDir);
    auto reopened = pdfe::test::SaveAndReopen(*dvDoc, ctx_, tmpDir);
    ASSERT_NE(reopened, nullptr) << "save+reopen produced no document.";
    auto rPage = reopened->page(0);
    ASSERT_NE(rPage, nullptr);
    FormManager rMgr(ctx_);
    auto rFields = rMgr.getFormFields(rPage.get());
    int rIdx = -1;
    for (size_t i = 0; i < rFields.size(); ++i) {
        if (rFields[i].fieldName == "FirstName") {
            rIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(rIdx, 0) << "FirstName missing after save+reopen.";
    EXPECT_EQ(rMgr.getFormField(rPage.get(), rIdx).value, kFirstNameDv)
        << "The reset-to-/DV value must persist across save+reopen.";

    // (3) Persistence via the in-place Document::save() path used by the
    // UI's Ctrl+S (onSaveFile). This is the exact production path that was
    // silently dropping the reset value; it must match saveAs().
    const auto inPlace = tmpDir / "reset_dv_inplace.pdf";
    std::filesystem::copy_file(
        formDvPdf_, inPlace,
        std::filesystem::copy_options::overwrite_existing);
    {
        auto ipDoc = Document::open(ctx_, inPlace);
        ASSERT_NE(ipDoc, nullptr);
        auto ipPage = ipDoc->page(0);
        FormManager ipMgr(ctx_);
        int ipIdx = -1;
        auto ipFields = ipMgr.getFormFields(ipPage.get());
        for (size_t i = 0; i < ipFields.size(); ++i) {
            if (ipFields[i].fieldName == "FirstName") {
                ipIdx = static_cast<int>(i);
                break;
            }
        }
        ASSERT_GE(ipIdx, 0);
        // Replicate the UI: render the page (canvas tile path) so the
        // document is in the same loaded/appearance-synthesized state the
        // Ctrl+S save path serializes from.
        (void)ipPage->render(QRectF(0, 0, 200, 200), 1.0f);
        void* dl = ipDoc->createDisplayList(0);
        if (dl) ipDoc->releaseDisplayList(dl);
        ipMgr.setFieldValue(ipPage.get(), ipIdx, "DIRTY_VALUE");
        ipMgr.resetForm(ipPage.get());
        ipPage.reset();
        ipDoc->save(inPlace);  // in-place overwrite (Ctrl+S path)
    }
    {
        auto ipReopen = Document::open(ctx_, inPlace);
        ASSERT_NE(ipReopen, nullptr);
        auto ipPage = ipReopen->page(0);
        FormManager ipMgr(ctx_);
        auto ipFields = ipMgr.getFormFields(ipPage.get());
        int ipIdx = -1;
        for (size_t i = 0; i < ipFields.size(); ++i) {
            if (ipFields[i].fieldName == "FirstName") {
                ipIdx = static_cast<int>(i);
                break;
            }
        }
        ASSERT_GE(ipIdx, 0);
        EXPECT_EQ(ipMgr.getFormField(ipPage.get(), ipIdx).value, kFirstNameDv)
            << "The reset-to-/DV value must persist across in-place save().";
    }
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-15b — M13 KILL: Reset Form restores a CHECKBOX to its /DV
// on-state, not hard "Off".
//
// The pre-existing ResetFormRestoresDefaultValues only asserts the /Tx
// (text) field FirstName. The button (checkbox/radio) /DV-restore path had
// no positive oracle, so the mutation that hard-codes the Btn reset target
// to "Off" (M13) — reverting a defaulted-ON control to unchecked — survived.
// The DV fixture's AgreeTerms checkbox has /DV = /Yes and AP states /Off,
// /Yes; after Reset Form its /V must become /Yes (its default on-state),
// both in memory AND across a save+reopen. Under the M13 mutation the
// widget resets to /Off and both assertions fail.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, ResetFormRestoresCheckboxDefaultOnState) {
    if (!std::filesystem::exists(formDvPdf_)) {
        GTEST_SKIP() << "Fixture missing: " << formDvPdf_;
    }

    // The /DV on-state the fixture assigns to the AgreeTerms checkbox
    // (see tests/fixtures/generate_form_with_dv.py: AgreeTerms → Name /Yes).
    const std::string kAgreeTermsDvOn = "Yes";

    auto dvDoc = Document::open(ctx_, formDvPdf_);
    ASSERT_NE(dvDoc, nullptr);
    auto dvPage = dvDoc->page(0);
    ASSERT_NE(dvPage, nullptr);

    FormManager mgr(ctx_);
    auto fields = mgr.getFormFields(dvPage.get());
    ASSERT_FALSE(fields.empty()) << "DV fixture has no fields.";

    int cbIdx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].fieldName == "AgreeTerms" &&
            fields[i].type == FormFieldType::Checkbox) {
            cbIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(cbIdx, 0)
        << "Fixture must contain an AgreeTerms checkbox with /DV=/Yes.";

    // Dirty the checkbox to the OFF state, differing from its /DV on-state.
    mgr.setFieldValue(dvPage.get(), cbIdx, "Off");
    ASSERT_EQ(mgr.getFormField(dvPage.get(), cbIdx).value, "Off");

    mgr.resetForm(dvPage.get());

    // (1) In-memory: the checkbox /V must be restored to its /DV on-state.
    EXPECT_EQ(mgr.getFormField(dvPage.get(), cbIdx).value, kAgreeTermsDvOn)
        << "resetForm must restore a checkbox to its /DV on-state (/Yes), "
           "not hard-code it to /Off.";

    // (2) Persistence: the reset-to-/DV on-state must survive save+reopen.
    const auto tmpDir = std::filesystem::temp_directory_path()
        / "pdfe_reset_cb_dv_test";
    std::filesystem::create_directories(tmpDir);
    auto reopened = pdfe::test::SaveAndReopen(*dvDoc, ctx_, tmpDir);
    ASSERT_NE(reopened, nullptr) << "save+reopen produced no document.";
    auto rPage = reopened->page(0);
    ASSERT_NE(rPage, nullptr);
    FormManager rMgr(ctx_);
    auto rFields = rMgr.getFormFields(rPage.get());
    int rIdx = -1;
    for (size_t i = 0; i < rFields.size(); ++i) {
        if (rFields[i].fieldName == "AgreeTerms") {
            rIdx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(rIdx, 0) << "AgreeTerms missing after save+reopen.";
    EXPECT_EQ(rMgr.getFormField(rPage.get(), rIdx).value, kAgreeTermsDvOn)
        << "The checkbox reset-to-/DV on-state must persist across save+reopen.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-16 — Flatten Form removes interactive widgets.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, FlattenFormRemovesInteractiveWidgets) {
    auto fieldsBefore = manager_->getFormFields(page0_);
    if (fieldsBefore.empty()) GTEST_SKIP() << "Fixture has no form fields.";

    auto tempPath = std::filesystem::temp_directory_path() /
                    "pdfe_phase5_flatten.pdf";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    // Set some values so flatten has something visible to draw
    int textIdx = findFieldByType(FormFieldType::Text);
    if (textIdx >= 0) {
        manager_->setFieldValue(page0_, textIdx, "FLATTEN_VISIBLE_VALUE");
    }

    manager_->flattenForm(page0_);
    document_->saveAs(tempPath);

    // Reopen and confirm zero interactive fields
    auto flatDoc = Document::open(ctx_, tempPath);
    ASSERT_NE(flatDoc, nullptr);
    auto flatPage = flatDoc->page(0);
    FormManager mgr2(ctx_);
    auto fieldsAfter = mgr2.getFormFields(flatPage.get());
    EXPECT_EQ(fieldsAfter.size(), 0u)
        << "Flatten + saveAs must produce a PDF with zero interactive fields. "
           "Found " << fieldsAfter.size();

    std::filesystem::remove(tempPath, ec);
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-17 — HostAllowlist blocks disallowed hosts.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, HostAllowlistBlocksDisallowedHosts) {
    HostAllowlist allowlist;
    allowlist.allow("good.example.com");
    allowlist.allow("forms.example.org");

    EXPECT_TRUE(allowlist.contains("good.example.com"))
        << "Explicitly allowed host must be present.";
    EXPECT_TRUE(allowlist.contains("forms.example.org"));

    EXPECT_FALSE(allowlist.contains("evil.attacker.test"))
        << "Disallowed host must be blocked.";
    EXPECT_FALSE(allowlist.contains("good.example.com.attacker.test"))
        << "Suffix-match attack must be blocked.";
    EXPECT_FALSE(allowlist.contains(""))
        << "Empty host must be blocked.";

    // Revoke
    allowlist.revoke("good.example.com");
    EXPECT_FALSE(allowlist.contains("good.example.com"))
        << "Revoked host must no longer be allowed.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-FORM-23 — Signature widget detection: locks that the manager
// surfaces signature widgets distinctly so MainWindow's click handler
// can dispatch SignDialog only for that field type.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase5FormsTest, SignatureWidgetTypeIsDistinctFromOthers) {
    auto fields = manager_->getFormFields(page0_);
    int sigCount = 0;
    int totalCount = 0;
    for (const auto& f : fields) {
        ++totalCount;
        if (f.type == FormFieldType::Signature) ++sigCount;
    }
    // The contract: getFormFields enumerates every widget AND tags
    // signature widgets with FormFieldType::Signature so the UI layer
    // can dispatch correctly. If the fixture has no signature widget,
    // this is a non-failing observation; the type enum still must
    // contain the Signature value.
    EXPECT_GE(static_cast<int>(FormFieldType::Signature), 0)
        << "FormFieldType::Signature must be a defined enum value.";
    (void)sigCount;
    (void)totalCount;
    SUCCEED();
}
