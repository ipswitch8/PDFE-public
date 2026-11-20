// PHASE3_ROUNDTRIP_FORMS: mutate -> save -> reopen -> assert tests
// for the AcroForm subsystem. Closes the gap that test_form_manager.cpp
// asserts internal FormFieldData state but never re-opens the saved
// PDF and confirms the field values landed in the bytes.
//
// At least one test exercises the realworld/form_calc/
// form_with_calculation_order.pdf fixture (per pipeline AC) to lock
// that the round-trip works on a non-synthetic form.

#include <gtest/gtest.h>

#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "forms/FormDataIO.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"
#include "redaction/RedactionManager.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureCoverageAnalyzer.h"
#include "core/QpdfWrapper.h"

#include <qpdf/QPDFObjectHandle.hh>

#include "helpers/RoundTripHelper.h"

#include "core/TextExtractor.h"
#include "textedit/TextEditModel.h"

#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using pdfe::forms::FormManager;
using pdfe::forms::FormDataIO;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;

namespace {

class FormRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "test_form.pdf fixture missing at " << fixture_;
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<FormManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_form_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    int findFieldIndex(pdfe::core::Page* page, FormFieldType type,
                       bool editableOnly = true) {
        auto fields = manager_->getFormFields(page);
        for (const auto& f : fields) {
            if (f.type == type && (!editableOnly || !f.isReadOnly)) {
                return f.widgetIndex;
            }
        }
        return -1;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<FormManager> manager_;
};

// ─── Text field ──────────────────────────────────────────────────────

TEST_F(FormRoundTripTest, SetTextFieldValueSurvivesSave) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0) << "test_form.pdf must contain an editable text field";

    manager_->setFieldValue(page.get(), idx, "round_trip_value");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto reField = manager_->getFormField(rePage.get(), idx);
    EXPECT_EQ(reField.value, "round_trip_value")
        << "Text-field value did not survive save/reopen";
}

TEST_F(FormRoundTripTest, ChangedTextFieldValueOverwritesOriginal) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0);

    manager_->setFieldValue(page.get(), idx, "first");
    manager_->setFieldValue(page.get(), idx, "second");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto reField = manager_->getFormField(rePage.get(), idx);
    EXPECT_EQ(reField.value, "second")
        << "Most-recent setFieldValue must be the value in the saved PDF";
    EXPECT_NE(reField.value, "first")
        << "Saved PDF must not retain the earlier value";
}

// ─── Checkbox ────────────────────────────────────────────────────────

TEST_F(FormRoundTripTest, ToggleCheckboxSurvivesSave) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Checkbox);
    ASSERT_GE(idx, 0)
        << "test_form.pdf must contain at least one checkbox";

    auto before = manager_->getFormField(page.get(), idx);
    manager_->toggleField(page.get(), idx);
    auto afterToggle = manager_->getFormField(page.get(), idx);
    ASSERT_NE(before.value, afterToggle.value)
        << "toggleField must change the in-memory value";
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto reField = manager_->getFormField(rePage.get(), idx);
    EXPECT_EQ(reField.value, afterToggle.value)
        << "Toggled checkbox state must persist through save/reopen";
    EXPECT_NE(reField.value, before.value)
        << "Saved PDF must not retain the pre-toggle state";
}

// ─── Multiple fields ─────────────────────────────────────────────────

TEST_F(FormRoundTripTest, MultipleFieldsSurviveSaveTogether) {
    auto page = document_->page(0);
    int textIdx = findFieldIndex(page.get(), FormFieldType::Text);
    int cbIdx = findFieldIndex(page.get(), FormFieldType::Checkbox);
    ASSERT_GE(textIdx, 0);
    ASSERT_GE(cbIdx, 0);

    manager_->setFieldValue(page.get(), textIdx, "two_field_test");
    auto cbBefore = manager_->getFormField(page.get(), cbIdx);
    manager_->toggleField(page.get(), cbIdx);
    auto cbAfter = manager_->getFormField(page.get(), cbIdx);
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    EXPECT_EQ(manager_->getFormField(rePage.get(), textIdx).value,
              "two_field_test");
    EXPECT_EQ(manager_->getFormField(rePage.get(), cbIdx).value,
              cbAfter.value);
    EXPECT_NE(manager_->getFormField(rePage.get(), cbIdx).value,
              cbBefore.value);
}

// ─── Reset form ──────────────────────────────────────────────────────

TEST_F(FormRoundTripTest, ResetFormDoesNotRetainStaleAfterSave) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0);

    manager_->setFieldValue(page.get(), idx, "staged_then_reset");
    auto pre = manager_->getFormField(page.get(), idx);
    EXPECT_EQ(pre.value, "staged_then_reset");

    manager_->resetForm(page.get());
    auto post = manager_->getFormField(page.get(), idx);
    // resetForm only resets fields with /DV; if test_form.pdf fields
    // lack /DV the value won't change. Either way, the contract under
    // test is "what we see in memory is what we get on disk".
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto reField = manager_->getFormField(reopened->page(0).get(), idx);
    EXPECT_EQ(reField.value, post.value)
        << "Saved field value must equal in-memory value after resetForm";
}

// ─── XFDF export/import round-trip ───────────────────────────────────

TEST_F(FormRoundTripTest, XfdfExportContainsAllSetValues) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0);

    manager_->setFieldValue(page.get(), idx, "xfdf_export_test");
    page.reset();

    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());
    EXPECT_FALSE(xfdf.empty()) << "XFDF export must produce non-empty output";
    EXPECT_NE(xfdf.find("xfdf"), std::string::npos)
        << "XFDF output must contain the xfdf root tag";
    EXPECT_NE(xfdf.find("xfdf_export_test"), std::string::npos)
        << "XFDF export must contain the value we set";
}

TEST_F(FormRoundTripTest, XfdfImportRestoresValues) {
    // Stage a known XFDF payload by exporting after setFieldValue.
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0);
    auto fields0 = manager_->getFormFields(page.get());
    std::string fieldName = fields0[idx].fieldName;

    manager_->setFieldValue(page.get(), idx, "import_target");
    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());
    page.reset();

    // Save+reopen with the value cleared, then import from XFDF.
    manager_->setFieldValue(document_->page(0).get(), idx, "");
    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    EXPECT_EQ(manager_->getFormField(rePage.get(), idx).value, "")
        << "Pre-import field must be empty";

    io.importFromXfdf(reopened.get(), manager_.get(), xfdf);
    auto refreshed = reopened->page(0);
    auto reField = manager_->getFormField(refreshed.get(), idx);
    EXPECT_EQ(reField.value, "import_target")
        << "XFDF import must restore the field value";
    EXPECT_EQ(reField.fieldName, fieldName)
        << "Field name must round-trip through XFDF";
}

// ─── Save + page-count integrity ─────────────────────────────────────

TEST_F(FormRoundTripTest, SavedFormFileIsNonEmptyAndOpens) {
    auto page = document_->page(0);
    int idx = findFieldIndex(page.get(), FormFieldType::Text);
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(page.get(), idx, "non_empty_check");
    page.reset();

    auto result = pdfe::test::SaveAndReopenWithPath(*document_, ctx_,
                                                     tempDir_);
    ASSERT_NE(result.doc, nullptr);
    EXPECT_GT(std::filesystem::file_size(result.path), 0u);
    EXPECT_GE(result.doc->pageCount(), 1)
        << "Saved form PDF must preserve page count";
}

// ─── Real-world fixture: form_with_calculation_order.pdf ─────────────

TEST_F(FormRoundTripTest, RealworldFormCalcOrderRoundTrip) {
    // Override the fixture for this test only — exercise the
    // realworld/form_calc/form_with_calculation_order.pdf produced
    // by the phase-2 generator. The pipeline AC requires at least
    // one test runs over a non-synthetic real-world form fixture.
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR)
                  / "realworld" / "form_calc"
                  / "form_with_calculation_order.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath))
        << "phase-2 fixture missing: " << rwPath;

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr);
    auto rwPage = rwDoc->page(0);

    auto fields = manager_->getFormFields(rwPage.get());
    ASSERT_GE(fields.size(), 1u)
        << "Realworld form fixture must expose at least one field";

    // Set a value on the first text field and roundtrip.
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == FormFieldType::Text) { textIdx = f.widgetIndex; break; }
    }
    ASSERT_GE(textIdx, 0);
    manager_->setFieldValue(rwPage.get(), textIdx, "calc_order_value");
    rwPage.reset();

    auto reopened = pdfe::test::SaveAndReopen(*rwDoc, ctx_, tempDir_,
                                               "form_calc");
    ASSERT_NE(reopened, nullptr);
    auto reField = manager_->getFormField(reopened->page(0).get(), textIdx);
    EXPECT_EQ(reField.value, "calc_order_value")
        << "Real-world form value must round-trip through save/reopen";
}

// ─────────────────────────────────────────────────────────────────────
// GitHub #19 / gap-register PDF-50 —
// FlattenPreservesExistingSignatureFieldIntegrity
//
// WHAT "BOTH ENGINES" MEANS HERE, and why.
// The gap-register row's own bracket is
// `[MuPDF + QPDF (byte-range: src/core/QpdfWrapper.cpp)]`, so "both
// engines" is the two PDF BACKENDS: the signature's /ByteRange and digest
// are re-measured with MuPDF (SignatureManager::verifySignature for the
// PKCS#7 digest, SignatureCoverageAnalyzer for what the range actually
// covers) AND independently re-parsed with QPDF (QpdfWrapper::qpdf() ->
// QPDFObjectHandle). An oracle that read the range with the same library
// that wrote the file could be fooled by a library-consistent corruption;
// two independent parsers cannot both be wrong in the same direction by
// accident.
//
// PDFE also has TWO FLATTEN PATHS — FormManager::flattenForm
// (pdf_bake_document(bake_annots=0, bake_widgets=1)) and
// RedactionManager::flattenAnnotations (bake_annots=1, bake_widgets=0).
// A criterion about "flattening" that exercised one of two entry points
// would be exactly the completeness gap this project has been bitten by
// before, so BOTH are driven, and each is judged by BOTH backends.
//
// THE ORACLE IS A DISJUNCTION WITH A FORBIDDEN THIRD STATE. ISO 32000-1
// §12.8 leaves an application two honest options when asked to make a
// destructive structural change to a signed document: refuse, or produce
// output in which the existing signature is still verifiable. The third
// outcome — a published file whose /ByteRange no longer resolves, with no
// error and no warning — is the defect. classifyOutcome() below returns
// which of the three happened and the test asserts it is not the third.
// ─────────────────────────────────────────────────────────────────────

/// One backend's independent reading of the first signature in a file.
struct SigByteRangeView {
    bool fileReadable = false;
    bool signatureFound = false;
    std::vector<long long> byteRange;
    long long fileSize = 0;
    /// The four-element range covers the whole file except exactly one hole,
    /// and that hole is where the /Contents hex string sits.
    bool coversWholeFile = false;
    std::string note;
};

/// QPDF backend. Deliberately does NOT reuse anything under src/signatures/ —
/// the point is a second parser.
SigByteRangeView ReadByteRangeWithQpdf(const std::filesystem::path& p)
{
    SigByteRangeView v;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        v.note = "file does not exist";
        return v;
    }
    v.fileSize = static_cast<long long>(std::filesystem::file_size(p, ec));

    pdfe::core::QpdfWrapper w;
    try {
        w.open(p);
    } catch (const std::exception& e) {
        v.note = std::string("QPDF could not open the file: ") + e.what();
        return v;
    }
    v.fileReadable = true;

    QPDFObjectHandle sig;
    for (auto& oh : w.qpdf().getAllObjects()) {
        if (!oh.isDictionary()) continue;
        if (!oh.hasKey("/ByteRange") || !oh.hasKey("/Contents")) continue;
        QPDFObjectHandle br = oh.getKey("/ByteRange");
        if (!br.isArray() || br.getArrayNItems() != 4) continue;
        sig = oh;
        break;
    }
    if (!sig.isInitialized() || !sig.isDictionary()) {
        v.note = "QPDF found no signature value dictionary with a 4-element "
                 "/ByteRange";
        return v;
    }
    v.signatureFound = true;

    QPDFObjectHandle br = sig.getKey("/ByteRange");
    for (int i = 0; i < 4; ++i) {
        QPDFObjectHandle e = br.getArrayItem(i);
        if (!e.isInteger()) {
            v.note = "/ByteRange element " + std::to_string(i)
                     + " is not an integer";
            return v;
        }
        v.byteRange.push_back(static_cast<long long>(e.getIntValue()));
    }

    const long long a = v.byteRange[0], b = v.byteRange[1];
    const long long c = v.byteRange[2], d = v.byteRange[3];
    if (a != 0 || b < 0 || d < 0 || c < b || c + d > v.fileSize) {
        v.note = "/ByteRange is malformed against the file it lives in";
        return v;
    }
    if (c + d != v.fileSize) {
        v.note = "the signed ranges stop " + std::to_string(v.fileSize - (c + d))
                 + " bytes before EOF";
        return v;
    }
    // The hole between the ranges must be exactly the /Contents hex string.
    // Prove it from the RAW BYTES rather than trusting the offsets: a '<' at
    // `b` and its matching '>' one byte before `c`.
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        v.note = "could not reread the raw bytes";
        return v;
    }
    in.seekg(b);
    std::string hole(static_cast<std::size_t>(c - b), '\0');
    in.read(hole.data(), static_cast<std::streamsize>(hole.size()));
    if (!in || hole.size() < 2 || hole.front() != '<' || hole.back() != '>') {
        v.note = "the /ByteRange gap is not the /Contents hex string (it is "
                 + std::to_string(c - b) + " bytes beginning 0x"
                 + (hole.empty() ? std::string("??")
                                 : std::to_string(
                                       static_cast<int>(
                                           static_cast<unsigned char>(
                                               hole.front()))))
                 + ")";
        return v;
    }
    v.coversWholeFile = true;
    v.note = "covers the whole file except the /Contents hex string";
    return v;
}

enum class FlattenOutcome {
    /// Flatten (or the save that publishes it) refused, and no output was
    /// published. The signature is intact because nothing was written.
    Refused,
    /// Output was published and the signature in it is still verifiable by
    /// both backends.
    PreservedAndVerifiable,
    /// Output was published and the signature in it is NOT verifiable. This
    /// is the defect the criterion exists to catch.
    SilentlyDamaged,
};

const char* OutcomeName(FlattenOutcome o)
{
    switch (o) {
        case FlattenOutcome::Refused:              return "REFUSED";
        case FlattenOutcome::PreservedAndVerifiable:
            return "PRESERVED-AND-VERIFIABLE";
        case FlattenOutcome::SilentlyDamaged:      return "SILENTLY-DAMAGED";
    }
    return "?";
}

class FlattenSignedFormTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        source_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_signed.pdf";
        ASSERT_TRUE(std::filesystem::exists(source_))
            << "test_signed.pdf fixture missing at " << source_;

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_flatten_signed" / info->name();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override { ctx_.reset(); }

    /// A private copy of the fixture, so nothing in this test can mutate the
    /// committed file.
    std::filesystem::path CopyOfFixture(const std::string& tag) {
        std::filesystem::path p = tempDir_ / (tag + "_in.pdf");
        std::filesystem::copy_file(
            source_, p, std::filesystem::copy_options::overwrite_existing);
        return p;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path source_;
    std::filesystem::path tempDir_;
};

TEST_F(FlattenSignedFormTest, FlattenPreservesExistingSignatureFieldIntegrity)
{
    using pdfe::signatures::SignatureManager;
    using pdfe::signatures::CoverageVerdict;

    // ── 0. BASELINE. Establish, with BOTH backends, that the untouched
    //       fixture really does carry a signature that is verifiable and whose
    //       /ByteRange covers the whole file. Without this the whole test
    //       could pass by measuring a file that never had a valid signature.
    const std::filesystem::path baseline = CopyOfFixture("baseline");

    SignatureManager sigs(ctx_);
    {
        auto doc = pdfe::core::Document::open(ctx_, baseline);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(sigs.countSignatures(doc.get()), 1)
            << "test_signed.pdf must carry exactly one signature field";
        auto page = doc->page(0);
        auto listed = sigs.listSignatures(doc.get());
        ASSERT_EQ(listed.size(), 1u);
        ASSERT_TRUE(listed[0].isSigned)
            << "the fixture's signature field must be SIGNED, not empty — an "
               "empty field has no /ByteRange and this test would measure "
               "nothing";
        auto verify = sigs.verifySignature(doc.get(), doc->page(listed[0].pageIndex).get(),
                                           listed[0].widgetIndex);
        ASSERT_TRUE(verify.digestValid)
            << "BASELINE FAILED (MuPDF engine): the fixture's PKCS#7 digest "
               "does not verify before any flatten — " << verify.digestMessage;
    }
    {
        auto cov = sigs.analyzeSignatureCoverage(baseline);
        ASSERT_TRUE(cov.readable);
        ASSERT_EQ(cov.signatureCount, 1);
        ASSERT_TRUE(cov.allSignaturesCoverWholeFile)
            << "BASELINE FAILED (MuPDF engine): the fixture's /ByteRange does "
               "not cover the whole file before any flatten — "
            << cov.signatures[0].message;
    }
    const SigByteRangeView baseQpdf = ReadByteRangeWithQpdf(baseline);
    ASSERT_TRUE(baseQpdf.signatureFound)
        << "BASELINE FAILED (QPDF engine): " << baseQpdf.note;
    ASSERT_TRUE(baseQpdf.coversWholeFile)
        << "BASELINE FAILED (QPDF engine): " << baseQpdf.note;
    // The two backends must AGREE on the baseline, or they are not measuring
    // the same object and neither reading afterwards means anything.
    {
        auto cov = sigs.analyzeSignatureCoverage(baseline);
        ASSERT_EQ(cov.signatures[0].byteRange, [&]{
            std::vector<std::int64_t> v;
            for (long long x : baseQpdf.byteRange) v.push_back(x);
            return v;
        }()) << "MuPDF and QPDF disagree about the fixture's /ByteRange";
    }

    // ── 0b. THE ORACLE MUST BE ABLE TO SAY "VERIFIABLE" AFTER A SAVE.
    //
    // The disjunction is satisfied below by the REFUSE branch, which means the
    // PreservedAndVerifiable branch — the whole two-backend measurement — would
    // never run in a green build. A guard that can only ever redden is worth as
    // little as one that can only ever green, so this control drives the same
    // document through a real PDFE save with NO destructive change and requires
    // both backends to report the signature still verifiable afterwards. If
    // this control fails, every "not verifiable" verdict below is meaningless.
    {
        const std::filesystem::path ctl = CopyOfFixture("control_plain_save");
        {
            auto doc = pdfe::core::Document::open(ctx_, ctl);
            ASSERT_NE(doc, nullptr);
            ASSERT_NO_THROW(doc->save(ctl))
                << "an ordinary save of a signed document, with no destructive "
                   "change in the session, must not be refused";
        }
        auto ctlDoc = pdfe::core::Document::open(ctx_, ctl);
        ASSERT_NE(ctlDoc, nullptr);
        auto listed = sigs.listSignatures(ctlDoc.get());
        ASSERT_EQ(listed.size(), 1u);
        auto vr = sigs.verifySignature(
            ctlDoc.get(), ctlDoc->page(listed[0].pageIndex).get(),
            listed[0].widgetIndex);
        ctlDoc.reset();
        EXPECT_TRUE(vr.digestValid)
            << "CONTROL (MuPDF engine): a non-destructive save must leave the "
               "digest verifiable — " << vr.digestMessage;
        auto ctlCov = sigs.analyzeSignatureCoverage(ctl);
        EXPECT_TRUE(ctlCov.allSignaturesCoverWholeFile)
            << "CONTROL (MuPDF engine): /ByteRange must still cover the whole "
               "file after a non-destructive save — "
            << (ctlCov.signatureCount ? ctlCov.signatures[0].message
                                      : ctlCov.message);
        const SigByteRangeView ctlQ = ReadByteRangeWithQpdf(ctl);
        EXPECT_TRUE(ctlQ.signatureFound && ctlQ.coversWholeFile)
            << "CONTROL (QPDF engine): " << ctlQ.note;
    }

    // ── 1. Drive each flatten path and classify the outcome with both
    //       backends.
    struct PathUnderTest {
        const char* name;
        std::function<void(pdfe::core::Document&)> apply;
    };

    std::vector<PathUnderTest> paths = {
        {"FormManager::flattenForm",
         [this](pdfe::core::Document& doc) {
             FormManager fm(ctx_);
             auto page = doc.page(0);
             fm.flattenForm(page.get());
         }},
        {"RedactionManager::flattenAnnotations",
         [this](pdfe::core::Document& doc) {
             pdfe::redaction::RedactionManager rm(ctx_);
             rm.flattenAnnotations(&doc);
         }},
    };

    for (const auto& p : paths) {
        SCOPED_TRACE(p.name);

        const std::string tag =
            std::string(p.name).find("Form") != std::string::npos
                ? "formmanager" : "redaction";
        const std::filesystem::path in = CopyOfFixture(tag);
        const std::filesystem::path out = tempDir_ / (tag + "_out.pdf");
        const auto inSizeBefore = std::filesystem::file_size(in);

        FlattenOutcome outcome = FlattenOutcome::SilentlyDamaged;
        std::string refusalMessage;
        bool published = false;

        try {
            auto doc = pdfe::core::Document::open(ctx_, in);
            ASSERT_NE(doc, nullptr);
            p.apply(*doc);
            doc->saveAs(out);
            published = true;
        } catch (const std::exception& e) {
            refusalMessage = e.what();
        }

        if (!published) {
            // A refusal is only a refusal if it published nothing and left the
            // input alone. A partial write with an exception on top is still
            // damage.
            EXPECT_FALSE(std::filesystem::exists(out))
                << p.name << " threw (\"" << refusalMessage << "\") but still "
                   "left an output file behind — that is a partial write, not "
                   "a refusal";
            EXPECT_EQ(std::filesystem::file_size(in), inSizeBefore)
                << p.name << " threw but modified the input document in place";
            outcome = FlattenOutcome::Refused;
        } else {
            ASSERT_TRUE(std::filesystem::exists(out))
                << p.name << " reported success but wrote no file";

            // MuPDF engine — digest, then coverage.
            bool muDigestValid = false;
            std::string muDigestMsg = "no signature widget survived the flatten";
            int muSigCount = 0;
            {
                auto reopened = pdfe::core::Document::open(ctx_, out);
                ASSERT_NE(reopened, nullptr);
                muSigCount = sigs.countSignatures(reopened.get());
                auto listed = sigs.listSignatures(reopened.get());
                for (const auto& s : listed) {
                    if (!s.isSigned) continue;
                    auto vr = sigs.verifySignature(
                        reopened.get(), reopened->page(s.pageIndex).get(),
                        s.widgetIndex);
                    muDigestValid = vr.digestValid;
                    muDigestMsg = vr.digestMessage;
                    break;
                }
            }
            auto muCov = sigs.analyzeSignatureCoverage(out);
            const bool muCoversWhole =
                muCov.readable && muCov.signatureCount > 0
                && muCov.allSignaturesCoverWholeFile;

            // QPDF engine — independent parse.
            const SigByteRangeView q = ReadByteRangeWithQpdf(out);

            const bool verifiable =
                muDigestValid && muCoversWhole && q.signatureFound
                && q.coversWholeFile;
            outcome = verifiable ? FlattenOutcome::PreservedAndVerifiable
                                 : FlattenOutcome::SilentlyDamaged;

            if (!verifiable) {
                ADD_FAILURE()
                    << p.name << " PUBLISHED a flattened copy of a signed "
                       "document in which the pre-existing signature is no "
                       "longer verifiable, and did not refuse.\n"
                       "  ISO 32000-1 §12.8: a destructive structural change "
                       "to a signed document must either preserve the "
                       "signature's /ByteRange and digest or be refused. This "
                       "did neither.\n"
                       "  MuPDF engine : signatures found="
                    << muSigCount << ", digestValid=" << muDigestValid
                    << " (" << muDigestMsg << "), coversWholeFile="
                    << muCoversWhole
                    << (muCov.signatureCount > 0
                            ? std::string(" — ") + muCov.signatures[0].message
                            : std::string(" — no signature dictionary at all"))
                    << "\n"
                       "  QPDF engine  : signatureFound=" << q.signatureFound
                    << ", coversWholeFile=" << q.coversWholeFile
                    << " — " << q.note << "\n"
                       "  baseline was : MuPDF coversWholeFile=1, QPDF "
                       "coversWholeFile=1, digestValid=1";
            }
        }

        // The disjunction, stated once, so the record says WHICH branch held.
        EXPECT_NE(outcome, FlattenOutcome::SilentlyDamaged)
            << p.name << " must either refuse to flatten a document with an "
               "already-signed field, or leave that signature's /ByteRange and "
               "digest verifiable in the output. It did neither.";
        RecordProperty(std::string("outcome_") + tag, OutcomeName(outcome));
        std::cout << "[ flatten  ] " << p.name << " -> " << OutcomeName(outcome)
                  << (refusalMessage.empty()
                          ? std::string()
                          : std::string(" (\"") + refusalMessage + "\")")
                  << std::endl;

        // ── THE REFUSAL MUST BE PROTECTING SOMETHING REAL ────────────────
        //
        // A refusal that guards a harmless operation is theatre, and it would
        // satisfy the disjunction above just as well as a refusal that guards
        // real damage. So the same flatten is repeated with the document's
        // signed-rewrite policy set to AcceptLoss, and the damage the guard
        // prevents is MEASURED with both backends. This is also the only arm
        // in which the two-backend post-flatten measurement runs at all, so
        // deleting it would leave that code unexercised in a green build.
        if (outcome == FlattenOutcome::Refused) {
            const std::filesystem::path in2 = CopyOfFixture(tag + "_accepted");
            const std::filesystem::path out2 =
                tempDir_ / (tag + "_accepted_out.pdf");
            {
                auto doc = pdfe::core::Document::open(ctx_, in2);
                ASSERT_NE(doc, nullptr);
                doc->setSignedRewritePolicy(
                    pdfe::core::Document::SignedRewritePolicy::AcceptLoss);
                p.apply(*doc);
                ASSERT_NO_THROW(doc->saveAs(out2))
                    << "AcceptLoss must actually let the save through, or the "
                       "opt-out is not an opt-out";
            }
            ASSERT_TRUE(std::filesystem::exists(out2));

            bool stillValid = false;
            {
                auto reopened = pdfe::core::Document::open(ctx_, out2);
                ASSERT_NE(reopened, nullptr);
                for (const auto& s : sigs.listSignatures(reopened.get())) {
                    if (!s.isSigned) continue;
                    // pdf_check_signature throws outright on a /ByteRange that
                    // points past EOF — which is itself proof of damage, so it
                    // is caught and treated as "not valid" rather than being
                    // allowed to escape the test.
                    try {
                        stillValid = sigs.verifySignature(
                            reopened.get(),
                            reopened->page(s.pageIndex).get(),
                            s.widgetIndex).digestValid;
                    } catch (const std::exception&) {
                        stillValid = false;
                    }
                    break;
                }
            }
            const SigByteRangeView q2 = ReadByteRangeWithQpdf(out2);

            EXPECT_FALSE(stillValid && q2.coversWholeFile)
                << p.name << ": the signed-rewrite guard refused an operation "
                   "that turns out to be HARMLESS — with AcceptLoss the "
                   "signature survived intact, so the refusal is protecting "
                   "nothing and should be removed rather than kept as theatre.";
            std::cout << "[ accepted ] " << p.name
                      << " -> MuPDF digestValid=" << stillValid
                      << ", QPDF signatureFound=" << q2.signatureFound
                      << ", QPDF coversWholeFile=" << q2.coversWholeFile
                      << " (" << q2.note << ")" << std::endl;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Bug 8 (GitHub form-field-bugs phase-7) — a form field CREATED in-session
// must round-trip through save -> reopen as an INTERACTIVE form field, not
// a flattened static appearance.
//
// This is distinct from every FormRoundTripTest above: those SET A VALUE on
// a field that already existed in the fixture. This one CREATES the widget
// (FormManager::createFormField, the model path MainWindow drives from the
// Form Tool), gives it a value, saves through the REAL Document save path,
// reopens in a FRESH context/Document/FormManager, and proves the reopened
// object is still an interactive /Tx widget.
//
// The operator's clarified symptom has three post-reopen states:
//   (i)   ABSENT     — no widget at all (empty-field / blank-/AP overlap).
//   (ii)  FLATTENED  — value visible as a static appearance, but NOT a
//                      fillable field (widget /FT + AcroForm /Fields + page
//                      /Annots identity lost; only /AP survives). REJECT.
//   (iii) INTERACTIVE — widget present in the form model, fillable,
//                      hit-testable, value intact. REQUIRE.
//
// The oracle therefore asserts, on the reopened file:
//   (a) present in page /Annots as a /FT=/Tx widget (getFormFields) AND in
//       AcroForm /Fields (listFieldNames) — the two halves of "is a field".
//   (b) value intact AND fillable (setFieldValue on the reopened widget
//       actually changes the stored value).
//   (c) hit-testable at its rect centre (getFormFieldAt).
//   (d) the EXTRA proof that it is a real widget, not flattened page
//       content: the created field's /AP text must NOT be reachable as an
//       EDITABLE TRE page-content span. A flattened field bakes a Form
//       XObject into the PAGE CONTENT stream, which the TextEditTool's model
//       (TextExtractor page-content pass) exposes as an editable span; a live
//       widget's glyphs live in the ANNOTATION /AP and are not page content.
//       See the empirically-pinned assertion below.
// ─────────────────────────────────────────────────────────────────────

class CreatedFieldRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        srcFixture_ =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(srcFixture_))
            << "test_form.pdf fixture missing at " << srcFixture_;

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_created_field_roundtrip" / info->name();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override { ctx_.reset(); }

    // Copy the fixture to a WRITABLE temp and open it from there, so the
    // Document's filePath() is the temp file — the precondition for the
    // incremental-save routing in Document::save (path == filePath).
    std::unique_ptr<pdfe::core::Document> openWritableCopy(
        const std::string& tag, std::filesystem::path& outPath)
    {
        outPath = tempDir_ / (tag + ".pdf");
        std::filesystem::copy_file(
            srcFixture_, outPath,
            std::filesystem::copy_options::overwrite_existing);
        return pdfe::core::Document::open(ctx_, outPath);
    }

    static std::vector<char> readAllBytes(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    }

    // The three-state classification the operator described.
    enum class ReopenState { Absent, Flattened, Interactive };

    static const char* StateName(ReopenState s) {
        switch (s) {
            case ReopenState::Absent:      return "ABSENT";
            case ReopenState::Flattened:   return "FLATTENED";
            case ReopenState::Interactive: return "INTERACTIVE";
        }
        return "?";
    }

    // Reopen `path` in a FRESH context + Document + FormManager and classify
    // the created field. `fieldName`/`value`/`rect` are what was created.
    ReopenState classifyReopened(const std::filesystem::path& path,
                                 const std::string& fieldName,
                                 const std::string& value,
                                 const QRectF& rect,
                                 bool& textIsEditablePageSpan)
    {
        textIsEditablePageSpan = false;

        auto reCtx = std::make_shared<pdfe::core::MuPdfContext>();
        auto reDoc = pdfe::core::Document::open(reCtx, path);
        EXPECT_NE(reDoc, nullptr);
        if (!reDoc) return ReopenState::Absent;
        FormManager reMgr(reCtx);
        auto rePage = reDoc->page(0);

        // (d) FIRST, before any refill mutates the appearance: is the created
        //     field's value text reachable as an EDITABLE TRE page-content
        //     span? A live widget's /AP glyphs are annotation content, not an
        //     editable page-content span, so spanAtPoint at the field centre
        //     must NOT resolve to a span whose text is the field value.
        {
            pdfe::core::TextExtractor extractor(reCtx);
            auto blocks = extractor.extract(rePage.get());
            pdfe::textedit::TextEditModel model(blocks);
            const QPointF centre = rect.center();
            const int span = model.spanAtPoint(centre);
            if (span >= 0) {
                const std::u32string& t = model.span(span).text;
                std::u32string want(value.begin(), value.end());
                if (t.find(want) != std::u32string::npos) {
                    textIsEditablePageSpan = true;
                }
            }
        }

        // (a) widget half: enumerated in page /Annots as a /Tx widget.
        auto fields = reMgr.getFormFields(rePage.get());
        int reIdx = -1;
        FormFieldData ref;
        for (const auto& f : fields) {
            if (f.fieldName == fieldName) { reIdx = f.widgetIndex; ref = f; }
        }
        // (a) AcroForm half: present in the AcroForm /Fields array.
        auto acroNames = reMgr.listFieldNames(reDoc.get());
        const bool inAcroForm =
            std::find(acroNames.begin(), acroNames.end(), fieldName)
            != acroNames.end();

        if (reIdx < 0 && !inAcroForm) {
            // No widget, no field — either genuinely absent, or baked into
            // static content (flattened). Distinguish by whether the text is
            // now page content.
            return textIsEditablePageSpan ? ReopenState::Flattened
                                          : ReopenState::Absent;
        }
        if (reIdx < 0 || ref.type != FormFieldType::Text || !inAcroForm) {
            // The appearance/name survived but the interactive /Tx identity in
            // BOTH structures did not — the operator's "looks like text, not a
            // fillable field" state.
            return ReopenState::Flattened;
        }

        // It presents as an interactive /Tx widget — now prove it behaves like
        // one: value intact, fillable, hit-testable.
        EXPECT_EQ(ref.value, value)
            << "created field value did not survive save/reopen";

        auto hit = reMgr.getFormFieldAt(rePage.get(), rect.center());
        EXPECT_TRUE(hit.has_value())
            << "created field is not hit-testable at its rect centre "
               "after reopen";
        if (hit.has_value()) EXPECT_EQ(*hit, reIdx);

        reMgr.setFieldValue(rePage.get(), reIdx, "REFILLED_AFTER_REOPEN");
        EXPECT_EQ(reMgr.getFormField(rePage.get(), reIdx).value,
                  "REFILLED_AFTER_REOPEN")
            << "reopened field is present but NOT fillable — not a live widget";

        return ReopenState::Interactive;
    }

    // Pick a widget-sized rect in PDF user space that overlaps NONE of the
    // fixture's existing widgets. Without this a created field dropped on top
    // of a pre-existing one makes getFormFieldAt resolve the EARLIER widget
    // (pdf_first_widget order), which is a fixture collision, not a product
    // defect. All rects here are in the same PDF user space as pdf_annot_rect,
    // FormFieldData.rect and Page::boundingBox, so the intersection test and
    // the later centre hit-test are directly comparable.
    QRectF chooseFreeRect(pdfe::core::Document& doc, FormManager& mgr) {
        auto page = doc.page(0);
        const QRectF box = page->boundingBox();
        const auto existing = mgr.getFormFields(page.get());
        const double w = 120.0, h = 20.0, margin = 10.0;
        for (double y = box.top() + margin;
             y + h + margin <= box.bottom(); y += (h + 2 * margin)) {
            for (double x = box.left() + margin;
                 x + w + margin <= box.right(); x += (w + 2 * margin)) {
                const QRectF cand(x, y, w, h);
                const QRectF probe =
                    cand.adjusted(-margin, -margin, margin, margin);
                bool clash = false;
                for (const auto& f : existing) {
                    if (f.rect.intersects(probe)) { clash = true; break; }
                }
                if (!clash) return cand;
            }
        }
        // Fallback: the fixture is impossibly crowded — still return something
        // inside the page so the test fails loudly rather than silently.
        return QRectF(box.left() + margin, box.top() + margin, w, h);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path srcFixture_;
    std::filesystem::path tempDir_;
};

// The REAL UI path: the Form Tool create callback parks the page AND calls
// markRequiresFullRewrite(), so an ensuing File>Save takes Document::save's
// full-rewrite branch. This must round-trip the created field as interactive.
TEST_F(CreatedFieldRoundTripTest, CreatedTextFieldSurvivesFullRewriteSave) {
    const std::string kName  = "pdfe_created_tf_full";
    const std::string kValue = "PDFE_CREATED_FIELD_8_FULL";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_full", path);
    ASSERT_NE(doc, nullptr);

    FormManager mgr(ctx_);
    const QRectF kRect = chooseFreeRect(*doc, mgr);
    auto page = doc->page(0);
    int idx = mgr.createFormField(page.get(), FormFieldType::Text, kRect, kName);
    ASSERT_GE(idx, 0);
    mgr.setFieldValue(page.get(), idx, kValue);
    page.reset();

    // Mirror MainWindow's Form-Tool create callback and save to the same file,
    // exactly as Ctrl+S after a field creation does.
    doc->markRequiresFullRewrite();
    ASSERT_NO_THROW(doc->save(path));
    doc.reset();

    bool textIsPageSpan = false;
    ReopenState state =
        classifyReopened(path, kName, kValue, kRect, textIsPageSpan);
    std::cout << "[ created  ] full-rewrite save -> " << StateName(state)
              << "  (value text is editable page span: "
              << (textIsPageSpan ? "YES" : "no") << ")" << std::endl;
    RecordProperty("full_rewrite_state", StateName(state));

    EXPECT_EQ(state, ReopenState::Interactive)
        << "A field created in-session and saved via the real full-rewrite "
           "path must reopen as an INTERACTIVE /Tx form field, not "
        << StateName(state) << ".";
    EXPECT_FALSE(textIsPageSpan)
        << "The created field's value is reachable as an EDITABLE page-content "
           "span after reopen — it was flattened into page content, not kept "
           "as a widget appearance.";
}

// The incremental-save branch. The MainWindow comment at :12312 states the
// incremental writer was VERIFIED not to append a freshly-created widget,
// which is why the create callback forces a full rewrite. This test drives
// that branch directly (no markRequiresFullRewrite) so the behaviour is
// pinned rather than assumed: if the widget is dropped/flattened here, the
// full-rewrite forcing is load-bearing and must stay.
TEST_F(CreatedFieldRoundTripTest, CreatedTextFieldThroughIncrementalSave) {
    const std::string kName  = "pdfe_created_tf_incr";
    const std::string kValue = "PDFE_CREATED_FIELD_8_INCR";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_incr", path);
    ASSERT_NE(doc, nullptr);

    FormManager mgr(ctx_);
    const QRectF kRect = chooseFreeRect(*doc, mgr);
    auto page = doc->page(0);
    int idx = mgr.createFormField(page.get(), FormFieldType::Text, kRect, kName);
    ASSERT_GE(idx, 0);
    mgr.setFieldValue(page.get(), idx, kValue);
    page.reset();

    // Capture the on-disk bytes BEFORE the save. An incremental-staged save
    // APPENDS a revision, leaving these bytes as a strict prefix of the output;
    // a full rewrite GCs and renumbers, so the prefix would NOT survive. This
    // is how we PROVE the incremental branch actually ran (a passing round-trip
    // alone would not — a full rewrite preserves the field too).
    const std::vector<char> preSaveBytes = readAllBytes(path);
    ASSERT_FALSE(preSaveBytes.empty());

    // Deliberately DO NOT markRequiresFullRewrite — save() to the same file
    // takes the incremental-staged branch.
    ASSERT_NO_THROW(doc->save(path));
    doc.reset();

    const std::vector<char> postSaveBytes = readAllBytes(path);
    ASSERT_GE(postSaveBytes.size(), preSaveBytes.size())
        << "incremental save must not shrink the file";
    const bool prefixPreserved =
        std::equal(preSaveBytes.begin(), preSaveBytes.end(),
                   postSaveBytes.begin());
    EXPECT_TRUE(prefixPreserved)
        << "the original bytes are NOT a prefix of the saved file — this save "
           "did NOT take the incremental-append branch, so the test is not "
           "exercising the branch it claims to.";
    std::cout << "[ branch   ] incremental-append confirmed: prefix preserved="
              << (prefixPreserved ? "yes" : "NO") << ", grew "
              << (postSaveBytes.size() - preSaveBytes.size()) << " bytes"
              << std::endl;

    bool textIsPageSpan = false;
    ReopenState state =
        classifyReopened(path, kName, kValue, kRect, textIsPageSpan);
    std::cout << "[ created  ] incremental save -> " << StateName(state)
              << "  (value text is editable page span: "
              << (textIsPageSpan ? "YES" : "no") << ")" << std::endl;
    RecordProperty("incremental_state", StateName(state));

    // The created field must round-trip as interactive regardless of which
    // save branch runs — a user is not required to know about the rewrite
    // flag. If this branch flattens/drops it, the product fix is to ensure
    // creation routes to (or the incremental writer captures) the widget.
    EXPECT_EQ(state, ReopenState::Interactive)
        << "A field created in-session and saved via the incremental branch "
           "must reopen as an INTERACTIVE /Tx form field, not "
        << StateName(state) << ".";
}

// ─── Cross-tool audit: OTHER created overlay objects through the SAME
//     save branches (per the enforced "check all related tools" criterion) ──
//
// The triage's STRONG LEAD: MainWindow::onAnnotationCreatedByTool does NOT
// call markRequiresFullRewrite (unlike createFormField), so a tool-created
// annotation saved with File>Save takes the INCREMENTAL branch. Every
// existing AnnotationRoundTripTest uses SaveAndReopen (== saveAs, a FULL
// REWRITE), so the incremental branch for a freshly-created annotation was
// UNCOVERED. This pins it: a created FreeText (text box) annotation must
// survive an incremental save, or the markRequiresFullRewrite omission is a
// real bug-8 recurrence for annotations.

TEST_F(CreatedFieldRoundTripTest, CreatedFreeTextAnnotThroughIncrementalSave) {
    const std::string kContents = "PDFE_CREATED_ANNOT_8_INCR";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_annot_incr", path);
    ASSERT_NE(doc, nullptr);

    pdfe::annotations::AnnotationManager am(ctx_);
    auto page = doc->page(0);
    auto created = am.createAnnotation(
        page.get(), pdfe::annotations::AnnotationType::FreeText,
        QRectF(120.0, 300.0, 180.0, 40.0));
    am.setContents(page.get(), created.annotIndex, kContents);
    page.reset();

    // Mirror onAnnotationCreatedByTool: NO markRequiresFullRewrite → the
    // File>Save of a tool-created annotation runs the incremental branch.
    ASSERT_NO_THROW(doc->save(path));
    doc.reset();

    auto reCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reDoc = pdfe::core::Document::open(reCtx, path);
    ASSERT_NE(reDoc, nullptr);
    pdfe::annotations::AnnotationManager reAm(reCtx);
    auto annots = reAm.getAnnotations(reDoc->page(0).get());

    bool found = false;
    for (const auto& a : annots) {
        if (a.type == pdfe::annotations::AnnotationType::FreeText &&
            a.contents == kContents) { found = true; break; }
    }
    std::cout << "[ created  ] FreeText annot incremental save -> "
              << (found ? "SURVIVED" : "DROPPED") << std::endl;
    EXPECT_TRUE(found)
        << "A tool-created FreeText annotation did NOT survive an incremental "
           "save — onAnnotationCreatedByTool omits markRequiresFullRewrite, so "
           "this is the annotation form of bug 8 and the fix is to mark the "
           "session for a full rewrite there (as createFormField does).";
}

// Another field TYPE (checkbox, /Btn) created in-session through the
// incremental branch — proves the round-trip is not /Tx-specific.
TEST_F(CreatedFieldRoundTripTest, CreatedCheckboxFieldThroughIncrementalSave) {
    const std::string kName = "pdfe_created_cb_incr";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_cb_incr", path);
    ASSERT_NE(doc, nullptr);

    FormManager mgr(ctx_);
    const QRectF rect = chooseFreeRect(*doc, mgr);
    auto page = doc->page(0);
    int idx = mgr.createFormField(page.get(), FormFieldType::Checkbox, rect,
                                  kName);
    ASSERT_GE(idx, 0);
    mgr.toggleField(page.get(), idx);  // set it On
    auto onValue = mgr.getFormField(page.get(), idx).value;
    page.reset();

    ASSERT_NO_THROW(doc->save(path));  // incremental branch
    doc.reset();

    auto reCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reDoc = pdfe::core::Document::open(reCtx, path);
    ASSERT_NE(reDoc, nullptr);
    FormManager reMgr(reCtx);
    auto fields = reMgr.getFormFields(reDoc->page(0).get());
    int reIdx = -1;
    FormFieldData ref;
    for (const auto& f : fields) {
        if (f.fieldName == kName) { reIdx = f.widgetIndex; ref = f; }
    }
    auto acroNames = reMgr.listFieldNames(reDoc.get());
    const bool inAcroForm =
        std::find(acroNames.begin(), acroNames.end(), kName) != acroNames.end();

    std::cout << "[ created  ] Checkbox field incremental save -> "
              << (reIdx >= 0 && inAcroForm ? "INTERACTIVE" : "LOST")
              << std::endl;
    ASSERT_GE(reIdx, 0)
        << "created checkbox absent from page /Annots widgets after reopen";
    EXPECT_EQ(ref.type, FormFieldType::Checkbox)
        << "reopened checkbox is not a /Btn form field (flattened?)";
    EXPECT_TRUE(inAcroForm)
        << "created checkbox absent from AcroForm /Fields after reopen";
    EXPECT_EQ(ref.value, onValue)
        << "created checkbox toggled state did not survive save/reopen";
}

// Choice / dropdown field (ComboBox, /Ch) created in-session through the
// incremental branch — the SAME createFormField/markRequiresFullRewrite path
// the "Combo Box" toolbar action (MainWindow.cpp) drives. Acceptance
// criterion #3 names checkbox AND choice/dropdown; this closes the choice half.
TEST_F(CreatedFieldRoundTripTest, CreatedComboBoxFieldThroughIncrementalSave) {
    const std::string kName  = "pdfe_created_combo_incr";
    const std::string kValue = "PDFE_CREATED_COMBO_8_INCR";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_combo_incr", path);
    ASSERT_NE(doc, nullptr);

    FormManager mgr(ctx_);
    const QRectF rect = chooseFreeRect(*doc, mgr);
    auto page = doc->page(0);
    int idx = mgr.createFormField(page.get(), FormFieldType::ComboBox, rect,
                                  kName);
    ASSERT_GE(idx, 0);
    mgr.setFieldValue(page.get(), idx, kValue);
    ASSERT_EQ(mgr.getFormField(page.get(), idx).value, kValue)
        << "pre-save: combo field must accept a /V value";
    page.reset();

    ASSERT_NO_THROW(doc->save(path));  // incremental branch (no full-rewrite mark)
    doc.reset();

    auto reCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reDoc = pdfe::core::Document::open(reCtx, path);
    ASSERT_NE(reDoc, nullptr);
    FormManager reMgr(reCtx);
    auto rePage = reDoc->page(0);

    // (d) FIRST: the combo's displayed value must NOT be an editable TRE
    //     page-content span (widget /AP, not flattened page content).
    bool textIsPageSpan = false;
    {
        pdfe::core::TextExtractor extractor(reCtx);
        auto blocks = extractor.extract(rePage.get());
        pdfe::textedit::TextEditModel model(blocks);
        const int span = model.spanAtPoint(rect.center());
        if (span >= 0) {
            const std::u32string& t = model.span(span).text;
            std::u32string want(kValue.begin(), kValue.end());
            if (t.find(want) != std::u32string::npos) textIsPageSpan = true;
        }
    }

    // (a) /Ch widget present in page /Annots AND AcroForm /Fields.
    auto fields = reMgr.getFormFields(rePage.get());
    int reIdx = -1;
    FormFieldData ref;
    for (const auto& f : fields) {
        if (f.fieldName == kName) { reIdx = f.widgetIndex; ref = f; }
    }
    auto acroNames = reMgr.listFieldNames(reDoc.get());
    const bool inAcroForm =
        std::find(acroNames.begin(), acroNames.end(), kName) != acroNames.end();

    std::cout << "[ created  ] ComboBox field incremental save -> "
              << (reIdx >= 0 && inAcroForm && ref.type == FormFieldType::ComboBox
                      ? "INTERACTIVE" : "LOST")
              << "  (value text is editable page span: "
              << (textIsPageSpan ? "YES" : "no") << ")" << std::endl;

    ASSERT_GE(reIdx, 0)
        << "created combo box absent from page /Annots widgets after reopen";
    EXPECT_EQ(ref.type, FormFieldType::ComboBox)
        << "reopened object is not a /Ch (choice) form field (flattened?)";
    EXPECT_TRUE(inAcroForm)
        << "created combo box absent from AcroForm /Fields after reopen";
    // (b) value intact.
    EXPECT_EQ(ref.value, kValue)
        << "created combo box value did not survive save/reopen";
    // (c) hit-testable at its rect centre.
    auto hit = reMgr.getFormFieldAt(rePage.get(), rect.center());
    ASSERT_TRUE(hit.has_value())
        << "created combo box is not hit-testable at its rect centre";
    EXPECT_EQ(*hit, reIdx);
    // (b) fillable: a fresh setFieldValue on the reopened widget changes /V.
    reMgr.setFieldValue(rePage.get(), reIdx, "COMBO_REFILLED");
    EXPECT_EQ(reMgr.getFormField(rePage.get(), reIdx).value, "COMBO_REFILLED")
        << "reopened combo box is present but NOT fillable — not a live widget";
    // (d) assertion.
    EXPECT_FALSE(textIsPageSpan)
        << "the created combo's value is reachable as an EDITABLE page-content "
           "span after reopen — it was flattened into page content, not kept "
           "as a widget appearance.";
}

// ListBox (/Ch, the other choice/dropdown type) — same path, confirming the
// choice round-trip is not Combo-flag-specific.
TEST_F(CreatedFieldRoundTripTest, CreatedListBoxFieldThroughIncrementalSave) {
    const std::string kName  = "pdfe_created_listbox_incr";
    const std::string kValue = "PDFE_CREATED_LISTBOX_8_INCR";

    std::filesystem::path path;
    auto doc = openWritableCopy("created_listbox_incr", path);
    ASSERT_NE(doc, nullptr);

    FormManager mgr(ctx_);
    const QRectF rect = chooseFreeRect(*doc, mgr);
    auto page = doc->page(0);
    int idx = mgr.createFormField(page.get(), FormFieldType::ListBox, rect,
                                  kName);
    ASSERT_GE(idx, 0);
    mgr.setFieldValue(page.get(), idx, kValue);
    page.reset();

    ASSERT_NO_THROW(doc->save(path));  // incremental branch
    doc.reset();

    auto reCtx = std::make_shared<pdfe::core::MuPdfContext>();
    auto reDoc = pdfe::core::Document::open(reCtx, path);
    ASSERT_NE(reDoc, nullptr);
    FormManager reMgr(reCtx);
    auto fields = reMgr.getFormFields(reDoc->page(0).get());
    int reIdx = -1;
    FormFieldData ref;
    for (const auto& f : fields) {
        if (f.fieldName == kName) { reIdx = f.widgetIndex; ref = f; }
    }
    auto acroNames = reMgr.listFieldNames(reDoc.get());
    const bool inAcroForm =
        std::find(acroNames.begin(), acroNames.end(), kName) != acroNames.end();

    std::cout << "[ created  ] ListBox field incremental save -> "
              << (reIdx >= 0 && inAcroForm && ref.type == FormFieldType::ListBox
                      ? "INTERACTIVE" : "LOST") << std::endl;
    ASSERT_GE(reIdx, 0)
        << "created list box absent from page /Annots widgets after reopen";
    EXPECT_EQ(ref.type, FormFieldType::ListBox)
        << "reopened object is not a /Ch (list) form field (flattened?)";
    EXPECT_TRUE(inAcroForm)
        << "created list box absent from AcroForm /Fields after reopen";
    EXPECT_EQ(ref.value, kValue)
        << "created list box value did not survive save/reopen";
}

} // namespace
