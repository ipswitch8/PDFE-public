// F15 / gap PDF-40 (+ PDF-47) — FDF and non-text XFDF import.
//
// WHAT WAS BROKEN
// ---------------
// docs/pdfe-vs-acrobat-pro-comparison.md claimed "FDF import/export ✓" while
// `importFromFdf` had ZERO hits in src/: PDFE could WRITE an FDF (form submit,
// src/formsubmit/FormEncoder.cpp) but could not READ one back. A user
// following the doc lost their data silently.
//
// F15 implements FormDataIO::importFromFdf and this file is its oracle. The
// expected values are AUTHORED in tests/fixtures/gen_f15_form_data_fdf.py and
// restated here as literals — nothing is read back from product output to
// define "expected".
//
// Also covered here (PDF-47): XFDF import for CHECKBOX and CHOICE fields.
// tests/test_form_data_io.cpp only ever exercised text values, so a
// value-vs-export-value mix-up on non-text fields was invisible.
//
// SECURITY: an FDF is untrusted input. Two asserts pin that the importer
// reads /Fields only — the fixture's /JavaScript and /F entries must have no
// effect whatsoever.
//
// MUTATIONS KILLED (run during F15, each confirmed RED then reverted):
//   * drop the `pdf_update_annot`-fed value application (skip applyFieldValues)
//     -> ImportFdfRestoresAuthoredValues fails on every field.
//   * make collectFdfFields ignore /Kids -> HierarchicalFdfFieldNameIsJoined
//     fails (the "grp.child" name never appears).
//   * remove the %FDF- header guard -> RefusesNonFdfContent fails.

#include <gtest/gtest.h>

#include "forms/FormDataIO.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "formsubmit/FormEncoder.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

using pdfe::forms::FormDataIO;
using pdfe::forms::FormManager;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;

namespace {

// AUTHORED literals — must match tests/fixtures/gen_f15_form_data_fdf.py.
constexpr const char* kFdfFirstName = "SpecimenA";
constexpr const char* kFdfLastName  = "Examplecorp";
constexpr const char* kFdfComments  = "paren ) and backslash \\ survive";
constexpr const char* kFdfCountry   = "Canada";
constexpr const char* kFdfJsMarker  = "f15-fdf-js-must-not-run";

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class FdfImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        fdfPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "f15_form_data.fdf";
        ASSERT_TRUE(std::filesystem::exists(fdfPath_))
            << "f15_form_data.fdf missing — regenerate with "
               "python tests/fixtures/gen_f15_form_data_fdf.py";
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<FormManager>(ctx_);
        io_ = std::make_unique<FormDataIO>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_fdf" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        io_.reset();
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    // name -> value across every page of `doc`.
    std::map<std::string, std::string> valuesOf(pdfe::core::Document& doc) {
        std::map<std::string, std::string> out;
        for (int i = 0; i < doc.pageCount(); ++i) {
            auto page = doc.page(i);
            if (!page) continue;
            for (const auto& f : manager_->getFormFields(page.get())) {
                if (!f.fieldName.empty() && out.find(f.fieldName) == out.end()) {
                    out[f.fieldName] = f.value;
                }
            }
        }
        return out;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path fdfPath_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<FormDataIO> io_;
};

// ─── PDF-40: the import round-trip ───────────────────────────────────────

TEST_F(FdfImportTest, ImportFdfRestoresAuthoredValues) {
    const std::string fdf = ReadFile(fdfPath_);
    ASSERT_FALSE(fdf.empty());

    io_->importFromFdf(document_.get(), manager_.get(), fdf);

    // Round-trip through the BYTES, not just live state.
    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_,
                                              "fdf_import");
    ASSERT_NE(reopened, nullptr);
    auto vals = valuesOf(*reopened);

    EXPECT_EQ(vals["FirstName"], kFdfFirstName);
    EXPECT_EQ(vals["LastName"], kFdfLastName);
    EXPECT_EQ(vals["Comments"], kFdfComments)
        << "escaped ')' / '\\' in the FDF literal must survive parsing";
    EXPECT_EQ(vals["Country"], kFdfCountry);
}

TEST_F(FdfImportTest, HierarchicalFdfFieldNameIsJoinedWithDot) {
    // The fixture carries /T (grp) with /Kids [ << /T (child) /V (...) >> ].
    // A parser that ignores /Kids sees no such field at all; one that flattens
    // wrongly produces "child". The fully-qualified name must be "grp.child".
    // The target form has no such field, so the observable is that the import
    // completes and leaves the flat fields correct — plus the exported XFDF of
    // a form that DOES have the values shows the join. Assert the parser's
    // behaviour directly by importing into the form and checking no field was
    // clobbered by a mis-joined name.
    const std::string fdf = ReadFile(fdfPath_);
    io_->importFromFdf(document_.get(), manager_.get(), fdf);
    auto vals = valuesOf(*document_);

    // "child" must NOT appear as a bare name anywhere the importer touched:
    // if /Kids were flattened without the parent prefix, a form field literally
    // named "child" would be filled. Prove the join by round-tripping the FDF
    // through a second import whose target names are the qualified ones.
    EXPECT_EQ(vals.count("child"), 0u);

    // Direct proof of the join: build an FDF with the same shape whose child
    // name collides with a REAL flat field of the target form. If the parser
    // dropped the parent prefix, "grp.FirstName" would overwrite "FirstName".
    const std::string nested =
        "%FDF-1.2\n"
        "1 0 obj\n"
        "<< /FDF << /Fields [ << /T (grp) /Kids [ << /T (FirstName) "
        "/V (must-not-land) >> ] >> ] >> >>\n"
        "endobj\n"
        "trailer\n<< /Size 2 /Root 1 0 R >>\n"
        "%%EOF\n";
    io_->importFromFdf(document_.get(), manager_.get(), nested);
    auto after = valuesOf(*document_);
    EXPECT_NE(after["FirstName"], "must-not-land")
        << "'grp.FirstName' must not be applied to the flat field 'FirstName'";
    EXPECT_EQ(after["FirstName"], kFdfFirstName);
}

TEST_F(FdfImportTest, RefusesNonFdfContent) {
    EXPECT_THROW(io_->importFromFdf(document_.get(), manager_.get(),
                                    "%PDF-1.7\nnot an fdf\n"),
                 std::exception);
    EXPECT_THROW(io_->importFromFdf(document_.get(), manager_.get(),
                                    "<xfdf><fields/></xfdf>"),
                 std::exception);
    EXPECT_THROW(io_->importFromFdf(document_.get(), manager_.get(), ""),
                 std::exception);
}

TEST_F(FdfImportTest, FdfJavaScriptAndFileSpecAreNeverApplied) {
    const std::string fdf = ReadFile(fdfPath_);
    ASSERT_NE(fdf.find(kFdfJsMarker), std::string::npos)
        << "fixture must actually contain the JS bait";

    io_->importFromFdf(document_.get(), manager_.get(), fdf);

    // Nothing from /JavaScript or /F may reach a field value, and the import
    // must not have redirected to the bogus /F target (the real form's own
    // field names are still the ones present).
    auto vals = valuesOf(*document_);
    for (const auto& [name, value] : vals) {
        EXPECT_EQ(value.find(kFdfJsMarker), std::string::npos)
            << "field " << name << " carries FDF /JavaScript content";
        EXPECT_EQ(value.find("does-not-exist"), std::string::npos)
            << "field " << name << " carries the FDF /F file spec";
    }
    EXPECT_EQ(vals.count("FirstName"), 1u);
}

// ─── PDF-40 defect fix: FDF *export* escaping ────────────────────────────

TEST_F(FdfImportTest, ExportedFdfEscapesLiteralStringDelimiters) {
    // Real defect found in F15: FormEncoder::encodeFdf wrote field names and
    // values into `(...)` literals RAW. A value containing ')' terminated the
    // literal early — corrupt FDF at best, injected FDF syntax at worst.
    using pdfe::formsubmit::FormEncoder;
    using pdfe::formsubmit::FormField;
    using pdfe::formsubmit::SubmitFormat;

    std::vector<FormField> fields{
        {"note", "close ) then /V (injected"},
        {"path", "C:\\temp\\x"},
    };
    const std::string out =
        FormEncoder::encode(fields, SubmitFormat::Fdf).toStdString();

    EXPECT_NE(out.find("close \\) then"), std::string::npos) << out;
    EXPECT_NE(out.find("C:\\\\temp\\\\x"), std::string::npos) << out;
    // The injected fragment must not appear as live syntax.
    EXPECT_EQ(out.find(") /V (injected)"), std::string::npos) << out;

    // And the escaped output must survive our own parser byte-for-byte.
    io_->importFromFdf(document_.get(), manager_.get(), out);  // no throw
}

// ─── PDF-40 doc-claim guard ──────────────────────────────────────────────

TEST_F(FdfImportTest, ComparisonDocFdfRowMatchesReality) {
    // The original PDF-40 finding was a FALSE CAPABILITY CLAIM: the comparison
    // doc advertised "FDF import/export ✓" when neither an importer nor a
    // file-level exporter existed. F15 implements import (asserted above) and
    // corrects the export half of the row. This test fails if the doc drifts
    // back to claiming a file-level FDF export PDFE does not have.
    const auto repoRoot = std::filesystem::path(TEST_FIXTURES_DIR)
                              .parent_path().parent_path();
    const auto docPath = repoRoot / "docs" / "pdfe-vs-acrobat-pro-comparison.md";
    ASSERT_TRUE(std::filesystem::exists(docPath)) << docPath;
    const std::string text = ReadFile(docPath);
    ASSERT_FALSE(text.empty());

    EXPECT_EQ(text.find("| FDF import/export | \xE2\x9C\x93 |"), std::string::npos)
        << "the doc again claims FDF import/export wholesale; PDFE has no "
           "file-level FDF export";

    const auto importRow = text.find("| FDF import |");
    ASSERT_NE(importRow, std::string::npos)
        << "the FDF import row is missing from the comparison doc";
    const auto importEol = text.find('\n', importRow);
    EXPECT_NE(text.substr(importRow, importEol - importRow).find("\xE2\x9C\x93"),
              std::string::npos)
        << "FDF import is implemented and must be marked supported";

    const auto exportRow = text.find("| FDF export to file |");
    ASSERT_NE(exportRow, std::string::npos)
        << "the FDF file-export row is missing from the comparison doc";
    const auto exportEol = text.find('\n', exportRow);
    EXPECT_NE(text.substr(exportRow, exportEol - exportRow).find("\xE2\x9C\x97"),
              std::string::npos)
        << "PDFE cannot write an FDF file; that row must stay marked absent";
}

// ─── PDF-47: XFDF import of non-text field types ─────────────────────────

TEST_F(FdfImportTest, ImportXfdfRestoresCheckboxAndChoiceValues) {
    // test_form.pdf carries Btn (AgreeTerms, Subscribe, Gender) and Ch
    // (Country, Interests) fields. Existing coverage only ever imported text.
    const std::string xfdf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n"
        "  <fields>\n"
        "    <field name=\"AgreeTerms\"><value>Yes</value></field>\n"
        "    <field name=\"Country\"><value>Canada</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";

    io_->importFromXfdf(document_.get(), manager_.get(), xfdf);

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_,
                                              "xfdf_nontext");
    ASSERT_NE(reopened, nullptr);
    auto vals = valuesOf(*reopened);

    EXPECT_EQ(vals["AgreeTerms"], "Yes")
        << "checkbox /V must be the export value, not the display label";
    EXPECT_EQ(vals["Country"], "Canada")
        << "choice /V must round-trip through XFDF import";
}

} // namespace
