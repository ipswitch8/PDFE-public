#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/PdfAConverter.h"
#include "compliance/PdfAValidator.h"
#include "compliance/XmpMetadataWriter.h"
#include "compliance/IccProfileManager.h"
#include "compliance/FontComplianceChecker.h"
#include "compliance/ComplianceTypes.h"
#include "compliance/ProhibitedFeatureScanner.h"
#include <windows.h>
#include <filesystem>
#include <set>
#include <memory>
#include <cstring>

using namespace pdfe::compliance;

class PdfAConverterTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(PdfAConverterTest, ConstructDestroy) {
    PdfAConverter converter(context_);
    SUCCEED();
}

TEST_F(PdfAConverterTest, AnalyzeForConversion_PdfA2b) {
    PdfAConverter converter(context_);
    auto result = converter.analyzeForConversion(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.targetLevel, PdfALevel::PdfA_2b);
}

TEST_F(PdfAConverterTest, AnalyzeForConversion_None_Fails) {
    PdfAConverter converter(context_);
    auto result = converter.analyzeForConversion(document_.get(), PdfALevel::None);
    EXPECT_FALSE(result.success);
}

TEST_F(PdfAConverterTest, AnalyzeForConversion_NullDoc_Fails) {
    PdfAConverter converter(context_);
    auto result = converter.analyzeForConversion(nullptr, PdfALevel::PdfA_2b);
    EXPECT_FALSE(result.success);
}

TEST_F(PdfAConverterTest, ConvertAndSave_PdfA2b) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_2b.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), PdfALevel::PdfA_2b, outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_GT(std::filesystem::file_size(outputPath), 0u);

    // Verify the output has XMP and OutputIntent
    auto doc2 = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(doc2, nullptr);

    XmpMetadataWriter xmpWriter(context_);
    PdfALevel detected = xmpWriter.detectPdfALevel(doc2.get());
    EXPECT_EQ(detected, PdfALevel::PdfA_2b);

    IccProfileManager iccMgr(context_);
    EXPECT_TRUE(iccMgr.hasOutputIntent(doc2.get()));

    doc2.reset();
    std::filesystem::remove(outputPath);
}

TEST_F(PdfAConverterTest, ConvertAndSave_PdfA1b) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_1b.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), PdfALevel::PdfA_1b, outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));

    auto doc2 = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(doc2, nullptr);

    XmpMetadataWriter xmpWriter(context_);
    PdfALevel detected = xmpWriter.detectPdfALevel(doc2.get());
    EXPECT_EQ(detected, PdfALevel::PdfA_1b);

    doc2.reset();
    std::filesystem::remove(outputPath);
}

TEST_F(PdfAConverterTest, ConvertAndSave_PdfA3b) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_3b.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), PdfALevel::PdfA_3b, outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));

    auto doc2 = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(doc2, nullptr);

    XmpMetadataWriter xmpWriter(context_);
    PdfALevel detected = xmpWriter.detectPdfALevel(doc2.get());
    EXPECT_EQ(detected, PdfALevel::PdfA_3b);

    doc2.reset();
    std::filesystem::remove(outputPath);
}

TEST_F(PdfAConverterTest, ConvertAndSave_OutputNotEncrypted) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_noenc.pdf";

    PdfAConverter converter(context_);
    converter.convertAndSave(document_.get(), PdfALevel::PdfA_2b, outputPath);

    auto doc2 = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(doc2, nullptr);

    // Verify not encrypted (a PDF/A requirement)
    // Just verify the document opens without password
    EXPECT_GT(doc2->pageCount(), 0);

    doc2.reset();
    std::filesystem::remove(outputPath);
}

TEST_F(PdfAConverterTest, ConvertAndSave_ResultHasTargetLevel) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_level.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), PdfALevel::PdfA_2b, outputPath);

    EXPECT_EQ(result.targetLevel, PdfALevel::PdfA_2b);

    std::filesystem::remove(outputPath);
}

// NAME-BLIND — WIN-87 phase-59. This asserted
// !exists("<out>.pdfe_pdfa_tmp"), and that name no longer exists: the converter
// stages behind an exclusive handle at an unpredictable name. A check for a
// name nothing produces can never fail for any bug, so it reads as coverage
// while testing nothing. Phases 57 and 58 hit this three times between them
// when ".redact_tmp"/".redact_out"/".part" were removed; the answer is the same
// each time — assert on the whole DIRECTORY, so residue of any name is caught,
// including one nobody has thought of.
TEST_F(PdfAConverterTest, ConvertAndSave_LeavesNoResidueOfAnyName) {
    auto dir = std::filesystem::temp_directory_path()
             / ("pdfe_pdfa_residue_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto outputPath = dir / "test_convert_clean.pdf";

    PdfAConverter converter(context_);
    converter.convertAndSave(document_.get(), PdfALevel::PdfA_2b, outputPath);

    std::set<std::string> left;
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path() == outputPath) continue;
        left.insert(it->path().filename().string());
    }
    std::string names;
    for (const auto& n : left) { names += n; names += ' '; }
    EXPECT_TRUE(left.empty())
        << "the PDF/A conversion left residue behind: " << names;

    std::filesystem::remove_all(dir, ec);
}

TEST_F(PdfAConverterTest, ConvertAndSave_None_Fails) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_none.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), PdfALevel::None, outputPath);

    EXPECT_FALSE(result.success);
    std::filesystem::remove(outputPath);
}

TEST_F(PdfAConverterTest, ConvertAndSave_NullDoc_Fails) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_convert_null.pdf";

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(nullptr, PdfALevel::PdfA_2b, outputPath);

    EXPECT_FALSE(result.success);
    std::filesystem::remove(outputPath);
}

// pipeline b3f19d72 phase-2: verify standard-14 fonts (referenced in
// the fixture by /BaseFont with no FontDescriptor) are embedded into
// the converted output. The validator's rule 6.3.3 check is "is the
// /FontDescriptor present AND does it have /FontFile, /FontFile2,
// or /FontFile3?" — so we use FontComplianceChecker (which performs
// exactly that test internally via pdf_dict_get on each font
// resource) and assert no standard-14 name appears unembedded.
TEST_F(PdfAConverterTest, StandardFourteenFontsAreEmbedded) {
    auto outputPath = std::filesystem::temp_directory_path()
                      / "test_convert_std14_embed.pdf";

    PdfAConverter converter(context_);
    converter.convertAndSave(document_.get(), PdfALevel::PdfA_1b, outputPath);
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Re-open the converted document so we inspect the on-disk
    // structure (not the in-memory state of the source doc).
    auto convDoc = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(convDoc, nullptr);

    FontComplianceChecker fontChecker(context_);
    auto fonts = fontChecker.checkFonts(convDoc.get(), /*requireUnicode*/false);

    std::string unembeddedReport;
    int unembeddedStd14Count = 0;
    int totalUnembedded = 0;
    for (const auto& f : fonts) {
        if (!f.isEmbedded) {
            ++totalUnembedded;
            if (PdfAConverter::isStandardFourteen(f.fontName)) {
                ++unembeddedStd14Count;
                unembeddedReport += "  /BaseFont=" + f.fontName
                                  + " (page " + std::to_string(f.pageIndex)
                                  + ")\n";
            }
        }
    }

    // The fixture (test_single.pdf) references /BaseFont /Helvetica
    // with /Subtype /Type1 and no FontDescriptor — confirmed by
    // inspection of tests/fixtures/test_single.pdf. After
    // convertAndSave, no standard-14 font may remain unembedded.
    // Note: the converter replaces the entire font resource via
    // pdf_add_simple_font, so /BaseFont in the output may now read
    // "ArialMT" (Arial's PostScript name) rather than "Helvetica"
    // — that's the correct outcome and PdfAConverter::isStandardFourteen
    // will not flag it (ArialMT is not in the standard-14 set), so
    // it won't count toward unembeddedStd14Count.
    EXPECT_EQ(unembeddedStd14Count, 0)
        << "After convertAndSave, the following standard-14 fonts are "
           "still missing /FontDescriptor + /FontFile{,2,3} (rule 6.3.3 "
           "violation):\n" << unembeddedReport
        << "Total unembedded fonts seen: " << totalUnembedded;

    // Sanity check: the converter SHOULD have embedded the
    // Helvetica reference; with everything embedded, FontComplianceChecker
    // should see at least one font (the substituted one) and that
    // font should be embedded.
    bool anyEmbedded = false;
    for (const auto& f : fonts) {
        if (f.isEmbedded) { anyEmbedded = true; break; }
    }
    EXPECT_TRUE(anyEmbedded)
        << "Converted output has no embedded fonts at all — the "
           "Helvetica substitution didn't take effect. Total fonts in "
           "output: " << fonts.size();

    convDoc.reset();
    std::filesystem::remove(outputPath);
}

// pipeline b3f19d72 phase-2: pure-function sanity check on the
// public helper used by the substitution path. Catches regressions
// where the standard-14 set diverges from the ISO 32000-1 list or
// where the subset-prefix stripping breaks.
TEST(PdfAConverterStandardFourteenTest, NameRecognition) {
    using pdfe::compliance::PdfAConverter;
    // Every standard-14 name must be recognized.
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Helvetica"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Helvetica-Bold"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Helvetica-Oblique"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Helvetica-BoldOblique"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Times-Roman"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Times-Bold"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Times-Italic"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Times-BoldItalic"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Courier"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Courier-Bold"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Courier-Oblique"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Courier-BoldOblique"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("Symbol"));
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("ZapfDingbats"));

    // Subset-prefix stripping
    EXPECT_TRUE(PdfAConverter::isStandardFourteen("ABCDEF+Helvetica"));

    // Negative cases
    EXPECT_FALSE(PdfAConverter::isStandardFourteen("ArialMT"));
    EXPECT_FALSE(PdfAConverter::isStandardFourteen("DejaVuSans"));
    EXPECT_FALSE(PdfAConverter::isStandardFourteen(""));
}

// WIN-87 phase-59 REGRESSION PIN — an ENCRYPTED source must still convert.
//
// This case exists because the phase-59 migration BROKE this and nothing
// caught it: the old converter set `opts.do_encrypt = PDF_ENCRYPT_NONE` on its
// MuPDF bake, and `pdf_write_options{}` zero-initialises that field to
// PDF_ENCRYPT_KEEP. So the migrated bake re-encrypted an encrypted source, the
// rewrite callback got a stream QPDF cannot read without the password, and a
// conversion that used to succeed threw "invalid password". The whole
// compliance suite stayed green because every case in it converts an
// UNENCRYPTED fixture.
//
// PDF/A forbids encryption outright (rule 6.1.3), and analyzeForConversion
// already promises the user "Encryption will be removed during conversion" —
// so a converter that FAILS on encrypted input is failing at the one input its
// own warning says it handles.
TEST_F(PdfAConverterTest, ConvertAndSave_EncryptedSourceStillConvertsAndOutputIsNotEncrypted) {
    const auto src = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "test_encrypted_aes256.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_encrypted_aes256.pdf not found";
    }
    // A USER-PASSWORD fixture, deliberately, and the first attempt at this case
    // used the owner-only one and MEASURED NOT DISCRIMINATING: owner-only
    // encryption opens with an EMPTY user password, so QPDF could read the
    // re-encrypted bake anyway and the case passed with the fix mutated out. A
    // regression pin that cannot go red for the regression it names is
    // decoration. With a real user password, a bake written PDF_ENCRYPT_KEEP is
    // unreadable to a QPDF handed no password, which is the failure.
    auto encrypted = pdfe::core::Document::open(context_, src, "test123");
    ASSERT_NE(encrypted, nullptr);
    ProhibitedFeatureScanner scanner(context_);
    ASSERT_TRUE(scanner.hasEncryption(encrypted.get()))
        << "this fixture is not encrypted, so this case is not exercising the "
           "regression it claims to";

    const auto outputPath = std::filesystem::temp_directory_path()
                          / ("pdfe_pdfa_encsrc_"
                             + std::to_string(::GetCurrentProcessId()) + ".pdf");
    std::error_code ec;
    std::filesystem::remove(outputPath, ec);

    PdfAConverter converter(context_);
    auto result = converter.convertAndSave(encrypted.get(), PdfALevel::PdfA_2b,
                                           outputPath);

    // The conversion may report conformance ISSUES — that is the validator's
    // job and not what this pins. What it must not do is fail to produce an
    // output, and the error message is asserted on so a regression says why.
    EXPECT_TRUE(result.errorMessage.find("password") == std::string::npos)
        << "the conversion failed on a password: " << result.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(outputPath))
        << "no PDF/A output was produced from an encrypted source: "
        << result.errorMessage;

    auto converted = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(converted, nullptr);
    EXPECT_FALSE(scanner.hasEncryption(converted.get()))
        << "the PDF/A output is still encrypted, which rule 6.1.3 forbids";
    EXPECT_GT(converted->pageCount(), 0);

    converted.reset();
    encrypted.reset();
    std::filesystem::remove(outputPath, ec);
}
