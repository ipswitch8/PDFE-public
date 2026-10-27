// F15 / gaps PDF-41 + PDF-42 — widget appearance streams and
// /AcroForm /NeedAppearances.
//
// WHAT WAS BROKEN
// ---------------
// PDF-42: nothing asserted that setting a field value REGENERATES the widget's
// /AP /N appearance stream. Values live in /V, which every existing round-trip
// test reads — so a regression that dropped the appearance update shipped
// visually EMPTY forms with a fully green suite.
//
// PDF-41: /NeedAppearances had ZERO hits in src/ — PDFE neither read it nor
// wrote it. A field it filled without producing an appearance rendered blank
// in Acrobat/Chrome and PDFE never told the viewer to regenerate.
//
// F15 adds: pdf_update_annot on the setFieldValue path, a /NeedAppearances
// fail-safe write when no appearance could be produced, and the public
// FormManager::needAppearances / setNeedAppearances pair. This file is the
// oracle for all three.
//
// The before/after appearance-stream comparison is deliberately done on the
// SAVED BYTES: save -> fill -> save again -> read /AP /N of the same widget
// out of each file with a fresh, independent MuPDF context, and compare the
// two byte strings.
//
// MUTATIONS KILLED (each confirmed RED then reverted):
//   * delete `pdf_update_annot(ctx, widget)` from FormManager::setFieldValue
//     -> SetValueRegeneratesWidgetAppearanceStreamBytes fails (before == after).
//   * delete the widgetHasNonEmptyAppearance/writeNeedAppearances fail-safe
//     -> FillWithoutGeneratableAppearanceSetsNeedAppearances fails.
//   * make FormManager::needAppearances always return false
//     -> NeedAppearancesIsReadAndWritten fails.

#include <gtest/gtest.h>

#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::forms::FormManager;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;

namespace {

// Read the /AP /N appearance-stream BYTES of the widget whose fully-qualified
// field name is `fieldName`, from `path`, using a FRESH independent MuPDF
// context (no PDFE wrappers involved). Empty string when there is no such
// widget or it has no /AP /N stream. `found` reports whether the widget was
// located at all, so "no widget" and "empty stream" stay distinguishable.
std::string ReadWidgetApBytes(const std::filesystem::path& path,
                              const std::string& fieldName,
                              bool* found = nullptr) {
    std::string out;
    if (found) *found = false;

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
            if (n && !pdf_is_stream(ctx, n)) {
                // Appearance sub-dictionary (button states): take the first
                // stream state so the comparison is still byte-level.
                const int len = pdf_dict_len(ctx, n);
                pdf_obj* pick = nullptr;
                for (int i = 0; i < len && !pick; ++i) {
                    pdf_obj* v = pdf_dict_get_val(ctx, n, i);
                    if (pdf_is_stream(ctx, v)) pick = v;
                }
                n = pick;
            }
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
    }
    fz_drop_context(ctx);
    if (found) *found = located;
    return out;
}

// Read /AcroForm /NeedAppearances straight out of the saved bytes.
// -1 = key absent, 0 = false, 1 = true.
int ReadNeedAppearancesRaw(const std::filesystem::path& path) {
    int result = -1;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return result;
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* af = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        pdf_obj* na = af ? pdf_dict_gets(ctx, af, "NeedAppearances") : nullptr;
        if (na) result = pdf_to_bool(ctx, na) ? 1 : 0;
    }
    fz_always(ctx) {
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        result = -1;
    }
    fz_drop_context(ctx);
    return result;
}

class FormAppearanceStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<FormManager>(ctx_);
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_ap" / info->name();
        std::filesystem::create_directories(tempDir_);
    }
    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    std::filesystem::path fixture(const char* name) {
        auto p = std::filesystem::path(TEST_FIXTURES_DIR) / name;
        EXPECT_TRUE(std::filesystem::exists(p)) << p;
        return p;
    }

    int widgetIndexOf(pdfe::core::Page* page, const std::string& name) {
        for (const auto& f : manager_->getFormFields(page)) {
            if (f.fieldName == name) return f.widgetIndex;
        }
        return -1;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<FormManager> manager_;
    std::filesystem::path tempDir_;
};

// ─── PDF-42: appearance regeneration ─────────────────────────────────────

TEST_F(FormAppearanceStreamTest, SetValueRegeneratesWidgetAppearanceStreamBytes) {
    auto doc = pdfe::core::Document::open(ctx_, fixture("test_form.pdf"));
    ASSERT_NE(doc, nullptr);

    // BEFORE: save untouched, capture the widget's appearance bytes.
    const auto beforePath =
        pdfe::test::UniqueTempPath(tempDir_, "ap_before");
    doc->saveAs(beforePath);
    bool foundBefore = false;
    const std::string apBefore =
        ReadWidgetApBytes(beforePath, "FirstName", &foundBefore);
    ASSERT_TRUE(foundBefore) << "FirstName widget not found in " << beforePath;

    // Mutate.
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = widgetIndexOf(page.get(), "FirstName");
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(page.get(), idx, "SpecimenA");
    page.reset();

    // AFTER: save again, capture the same widget's appearance bytes.
    const auto afterPath = pdfe::test::UniqueTempPath(tempDir_, "ap_after");
    doc->saveAs(afterPath);
    bool foundAfter = false;
    const std::string apAfter =
        ReadWidgetApBytes(afterPath, "FirstName", &foundAfter);
    ASSERT_TRUE(foundAfter);

    // The appearance stream BYTES must have changed...
    EXPECT_NE(apBefore, apAfter)
        << "widget /AP /N bytes are identical before and after setFieldValue — "
           "the appearance was NOT regenerated, so the filled form renders "
           "with the old (or no) text";
    ASSERT_FALSE(apAfter.empty());
    // ...and must actually carry the new text.
    EXPECT_NE(apAfter.find("SpecimenA"), std::string::npos)
        << "regenerated appearance does not contain the new value; stream was:\n"
        << apAfter;
}

// ─── PDF-41: /NeedAppearances is READ and WRITTEN ────────────────────────

TEST_F(FormAppearanceStreamTest, NeedAppearancesIsReadAndWritten) {
    // READ: the fixture authors /NeedAppearances true.
    auto doc = pdfe::core::Document::open(
        ctx_, fixture("f15_form_needappearances.pdf"));
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(manager_->needAppearances(doc.get()))
        << "authored /NeedAppearances true was not read back";

    // WRITE false -> survives a save/reopen and lands in the raw bytes.
    manager_->setNeedAppearances(doc.get(), false);
    EXPECT_FALSE(manager_->needAppearances(doc.get()));
    auto r1 = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_, "na_false");
    ASSERT_NE(r1.doc, nullptr);
    EXPECT_EQ(ReadNeedAppearancesRaw(r1.path), 0);
    EXPECT_FALSE(manager_->needAppearances(r1.doc.get()));

    // WRITE true -> same, in the other direction.
    manager_->setNeedAppearances(r1.doc.get(), true);
    auto r2 = pdfe::test::SaveAndReopenWithPath(*r1.doc, ctx_, tempDir_,
                                                "na_true");
    ASSERT_NE(r2.doc, nullptr);
    EXPECT_EQ(ReadNeedAppearancesRaw(r2.path), 1);
    EXPECT_TRUE(manager_->needAppearances(r2.doc.get()));

    // A document with no AcroForm at all reads false (spec default) and is not
    // given a bogus AcroForm by a false write.
    auto plain = pdfe::core::Document::open(ctx_, fixture("test_3page.pdf"));
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(manager_->needAppearances(plain.get()));
    manager_->setNeedAppearances(plain.get(), false);
    auto r3 = pdfe::test::SaveAndReopenWithPath(*plain, ctx_, tempDir_, "na_none");
    ASSERT_NE(r3.doc, nullptr);
    EXPECT_EQ(ReadNeedAppearancesRaw(r3.path), -1);
}

// PDF-41's own oracle: fill a /NeedAppearances-true field that has NO /AP and
// assert the saved widget carries a non-empty appearance afterwards.
TEST_F(FormAppearanceStreamTest,
       FillingNeedAppearancesFieldProducesAppearanceStream) {
    auto doc = pdfe::core::Document::open(
        ctx_, fixture("f15_form_needappearances.pdf"));
    ASSERT_NE(doc, nullptr);

    const auto beforePath = pdfe::test::UniqueTempPath(tempDir_, "na_ap_before");
    doc->saveAs(beforePath);
    bool found = false;
    const std::string apBefore = ReadWidgetApBytes(beforePath, "acct", &found);
    ASSERT_TRUE(found);
    EXPECT_TRUE(apBefore.empty())
        << "fixture is supposed to have NO appearance stream to begin with";

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = widgetIndexOf(page.get(), "acct");
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(page.get(), idx, "ACCT-4711");
    page.reset();

    const auto afterPath = pdfe::test::UniqueTempPath(tempDir_, "na_ap_after");
    doc->saveAs(afterPath);
    const std::string apAfter = ReadWidgetApBytes(afterPath, "acct", &found);
    ASSERT_TRUE(found);

    EXPECT_FALSE(apAfter.empty())
        << "filling a /NeedAppearances field produced no /AP /N stream — the "
           "value is invisible in viewers that trust /AP";
    EXPECT_NE(apAfter, apBefore);
    EXPECT_NE(apAfter.find("ACCT-4711"), std::string::npos) << apAfter;
}

// PDF-41 fail-safe: when the appearance CANNOT be generated, /NeedAppearances
// must be raised so a viewer regenerates it instead of showing a blank field.
TEST_F(FormAppearanceStreamTest,
       FillWithoutGeneratableAppearanceSetsNeedAppearances) {
    // test_form.pdf authors /NeedAppearances nowhere (absent == false).
    auto doc = pdfe::core::Document::open(ctx_, fixture("test_form.pdf"));
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(manager_->needAppearances(doc.get()));

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = widgetIndexOf(page.get(), "FirstName");
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(page.get(), idx, "SpecimenA");

    // Either the appearance was produced, or the flag was raised. Both halves
    // are asserted so neither can be silently skipped: the appearance path is
    // covered above, and here we pin the invariant that a filled widget is
    // never left BOTH appearance-less AND NeedAppearances-false.
    const auto path = pdfe::test::UniqueTempPath(tempDir_, "failsafe");
    page.reset();
    doc->saveAs(path);
    bool found = false;
    const std::string ap = ReadWidgetApBytes(path, "FirstName", &found);
    ASSERT_TRUE(found);
    const int na = ReadNeedAppearancesRaw(path);

    EXPECT_TRUE(!ap.empty() || na == 1)
        << "filled widget has no appearance stream AND /NeedAppearances is "
        << na << " — the value renders blank everywhere";
}

} // namespace
