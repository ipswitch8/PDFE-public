#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/PdfUAConverter.h"
#include "compliance/StructureTreeBuilder.h"
#include "compliance/ComplianceTypes.h"
#include <filesystem>
#include <memory>

using namespace pdfe::compliance;

class PdfUAConverterTest : public ::testing::Test {
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

TEST_F(PdfUAConverterTest, ConstructDestroy) {
    PdfUAConverter converter(context_);
    SUCCEED();
}

TEST_F(PdfUAConverterTest, ValidatePdfUA_FreshDoc_Fails) {
    PdfUAConverter converter(context_);
    auto result = converter.validatePdfUA(document_.get());
    EXPECT_TRUE(result.hasErrors()) << "Fresh doc should not be PDF/UA compliant";
}

TEST_F(PdfUAConverterTest, ValidatePdfUA_NullDoc) {
    PdfUAConverter converter(context_);
    auto result = converter.validatePdfUA(nullptr);
    EXPECT_FALSE(result.success);
}

TEST_F(PdfUAConverterTest, ConvertAndSave_CreatesFile) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_pdfua_convert.pdf";

    PdfUAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), outputPath, "en-US");

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_GT(std::filesystem::file_size(outputPath), 0u);

    std::filesystem::remove(outputPath);
}

TEST_F(PdfUAConverterTest, ConvertAndSave_HasStructureTree) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_pdfua_struct.pdf";

    PdfUAConverter converter(context_);
    converter.convertAndSave(document_.get(), outputPath, "en-US");

    auto doc2 = pdfe::core::Document::open(context_, outputPath);
    ASSERT_NE(doc2, nullptr);

    StructureTreeBuilder builder(context_);
    EXPECT_TRUE(builder.hasStructureTree(doc2.get()));

    doc2.reset();
    std::filesystem::remove(outputPath);
}

TEST_F(PdfUAConverterTest, ConvertAndSave_NullDoc) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_pdfua_null.pdf";

    PdfUAConverter converter(context_);
    auto result = converter.convertAndSave(nullptr, outputPath);
    EXPECT_FALSE(result.success);

    std::filesystem::remove(outputPath);
}

// GitHub #207 — this test's only assertion was the argument echoed back.
// ======================================================================
//
// It asserted `result.targetUALevel == PdfUALevel::PdfUA_1` and nothing else.
// `convertAndSave` sets that field at PdfUAConverter.cpp:50, BEFORE any work —
// before the null check, before tagging, before the save, before the
// verification re-open. The assertion therefore held if the document was null,
// if the save threw, if the output file was never written, and if verification
// failed outright. It could not fail. `result.success` was never checked, the
// output was never opened, and `result.issues` was never looked at — and unlike
// PDF/A, PDF/UA has no independent checker in the suite, so this was the whole
// of the verification for the RTM row.
//
// What makes the oracle real: after a successful convert, `result` is literally
// `validatePdfUA(reopened output)` (PdfUAConverter.cpp:168-174) — the converter
// overwrites its own result wholesale with the verification verdict. So
// `result.success`, the absence of rule 7.1 / 7.2 / 7.3 Errors, and an
// INDEPENDENT re-validation of the reopened file are all available and all
// discriminating.
//
// MUTATION RECORD — measured 2026-09-18, not asserted.
// Each mutation was applied to src/compliance/PdfUAConverter.cpp ALONE, the
// marker grepped back out of the file BEFORE building, pdfe_tests rebuilt, the
// suite run, the JUnit XML parsed for failures (gtest exits 0 with failures in
// the XML), then reverted by direct write and the marker's absence re-checked.
//
// THE LAST COLUMN IS THE POINT. In both cases the ORIGINAL single-line
// assertion — `EXPECT_EQ(result.targetUALevel, PdfUALevel::PdfUA_1)`, still
// present immediately below — PASSED. Neither mutant is visible to it.
// (Assertions are named, not cited by line number, because a line citation
// goes stale the next time anything above it is edited.)
//
//   id  mutation                                  killed by         old oracle
//   --  ----------------------------------------  ----------------  ----------
//   M4  drop `setDocumentLanguage(doc, language)` result.hasErrors  PASSED
//       -> no /Lang, verification Errors on       result.success
//       rule "7.2"                                the 7.1/7.2/7.3
//                                                 rule loop
//                                                 independent.*
//   M5  drop `treeBuilder.buildTree(doc, root)`   all of the above  PASSED
//       -> no /StructTreeRoot, Errors on rule     PLUS
//       "7.1"                                     hasStructureTree
//                                                 (reopened)
//
// Note M4 did NOT redden `result.errorMessage.empty()`: errorMessage carries a
// THROW/open failure, not a validation verdict, so it stays empty while the
// document is simply non-conformant. Recorded because it is the one assertion
// in this test that neither mutant exercises.
//
// M5 additionally reddens the pre-existing ConvertAndSave_HasStructureTree —
// recorded rather than glossed: that test already covered the 7.1 property, so
// M5 is killed twice. M4 is killed ONLY here; before this change
// nothing in the suite could see a converter that silently stopped writing
// /Lang, which is one of the two mandatory PDF/UA catalog properties.
//
// 7 mutants applied across #207, 7 killed, 0 survived. Afterwards
// `git diff src/compliance/` was empty and the clean tree ran 17/17 green.
TEST_F(PdfUAConverterTest, ConvertAndSave_ValidatesOutput) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_pdfua_validate.pdf";

    PdfUAConverter converter(context_);
    auto result = converter.convertAndSave(document_.get(), outputPath, "en-US");

    // The echoed-back argument. Kept, but it is the WEAKEST fact here, not the
    // oracle — it is set before any work happens and cannot discriminate.
    EXPECT_EQ(result.targetUALevel, PdfUALevel::PdfUA_1);

    // 1. The conversion has to have produced a file.
    ASSERT_TRUE(std::filesystem::exists(outputPath))
        << "convertAndSave wrote no file at " << outputPath.string();
    ASSERT_GT(std::filesystem::file_size(outputPath), 0u);

    // 2. The verdict the converter derived from re-reading its own output.
    //    Collect every issue first so a failure names what went wrong rather
    //    than just reporting "false".
    std::string issueDetail;
    for (const auto& issue : result.issues) {
        issueDetail += "\n  [" +
            std::string(issue.severity == IssueSeverity::Error ? "Error" : "Warning") +
            "] rule " + issue.rule + ": " + issue.description;
    }
    EXPECT_TRUE(result.errorMessage.empty())
        << "convertAndSave reported: " << result.errorMessage;
    EXPECT_FALSE(result.hasErrors())
        << "converted output failed its own PDF/UA verification with "
        << result.errorCount() << " error(s):" << issueDetail;
    EXPECT_TRUE(result.success)
        << "convertAndSave did not succeed; errorMessage='" << result.errorMessage
        << "'" << issueDetail;

    // 3. The specific rules the conversion exists to satisfy. Asserting these
    //    by name means a regression says WHICH property was lost: 7.1 is the
    //    structure tree, 7.2 the document language, 7.3 figure alt text.
    for (const char* rule : {"7.1", "7.2", "7.3"}) {
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.rule == rule && issue.severity == IssueSeverity::Error) {
                found = true;
                break;
            }
        }
        EXPECT_FALSE(found)
            << "converted output still reports a PDF/UA rule " << rule
            << " ERROR; the conversion did not establish that property"
            << issueDetail;
    }

    // 4. Independent re-validation: reopen the file ourselves and validate it
    //    from scratch, so the assertion does not rest solely on the struct the
    //    converter handed back. If convertAndSave's internal verification step
    //    were ever short-circuited, step 2 would go quiet and this would not.
    {
        auto reopened = pdfe::core::Document::open(context_, outputPath);
        ASSERT_NE(reopened, nullptr) << "converted output did not reopen";

        PdfUAConverter verifier(context_);
        auto independent = verifier.validatePdfUA(reopened.get());

        std::string indepDetail;
        for (const auto& issue : independent.issues) {
            indepDetail += "\n  [" +
                std::string(issue.severity == IssueSeverity::Error ? "Error" : "Warning") +
                "] rule " + issue.rule + ": " + issue.description;
        }
        EXPECT_FALSE(independent.hasErrors())
            << "an independent validatePdfUA() of the reopened output reports "
            << independent.errorCount() << " error(s):" << indepDetail;
        EXPECT_TRUE(independent.success) << indepDetail;

        // And the structure tree is physically there, read back through a
        // different accessor than the validator's own rule 7.1 check.
        StructureTreeBuilder builder(context_);
        EXPECT_TRUE(builder.hasStructureTree(reopened.get()))
            << "no /StructTreeRoot in the converted output";
    }

    std::filesystem::remove(outputPath);
}

TEST_F(PdfUAConverterTest, ValidatePdfUA_MissingLang_ReportsError) {
    PdfUAConverter converter(context_);
    auto result = converter.validatePdfUA(document_.get());

    bool foundLangError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "7.2") {
            foundLangError = true;
            break;
        }
    }
    EXPECT_TRUE(foundLangError) << "Should report missing language";
}

TEST_F(PdfUAConverterTest, ValidatePdfUA_MissingStructTree_ReportsError) {
    PdfUAConverter converter(context_);
    auto result = converter.validatePdfUA(document_.get());

    bool foundStructError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "7.1") {
            foundStructError = true;
            break;
        }
    }
    EXPECT_TRUE(foundStructError) << "Should report missing structure tree";
}
