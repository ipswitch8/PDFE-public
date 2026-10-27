// PHASE1_HARDENING: skip=9 expect_ge=0 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/PageCommands.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

// AUTHORED ground truth for tests/fixtures/test_3page.pdf. These literals are
// copied verbatim from the fixture generator
// (tests/generate_fixtures.py::generate_3page_pdf) — NOT from what PDFE
// currently emits. One distinctive, single-Tj line per authored page cell.
const char* const kP1Title = "Page 1 - Title Page";
const char* const kP1Body  = "This is a test PDF for PDFE unit tests.";
const char* const kP2Title = "Page 2 - Body Content";
const char* const kP2Body  = "Lorem ipsum";
const char* const kP3Title = "Page 3 - Final Page";
const char* const kP3Item1 = "Item 1: First list item";
const char* const kP3Rect  = "Rectangle with text";

bool containsText(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

class SaveTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            undoManager = std::make_unique<pdfe::commands::UndoManager>();
        }

        outputPath = std::filesystem::temp_directory_path() / "pdfe_test_save.pdf";
    }

    void TearDown() override {
        undoManager.reset();
        document.reset();
        context.reset();
        if (std::filesystem::exists(outputPath)) {
            std::filesystem::remove(outputPath);
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
    std::filesystem::path outputPath;
};

TEST_F(SaveTest, SaveAs_CreatesFile) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    ASSERT_FALSE(std::filesystem::exists(outputPath));

    document->saveAs(outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_GT(std::filesystem::file_size(outputPath), 0);
}

// Pipeline b5e3a9f2 phase-4 / GAP-51 product-side proof: Document::saveAs()
// must replace an existing file's contents when invoked on an extant
// path. The UI integration test (test_critical_flows_p0::flow_save_as_overwrite)
// reported "file not overwritten" and was wrongly attributed to a product
// bug; this headless test proves Document::saveAs() handles overwrite
// correctly. If this test PASSES, the integration test's failure is in
// its dialog-driving code (Windows overwrite-confirmation modal not
// dismissed correctly), not in the product save path.
TEST_F(SaveTest, SaveAs_OverwritesExistingFile) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // Pre-populate the target path with an 11-byte placeholder.
    {
        std::ofstream ofs(outputPath, std::ios::binary | std::ios::trunc);
        ofs << "placeholder";
    }
    ASSERT_TRUE(std::filesystem::exists(outputPath));
    ASSERT_EQ(std::filesystem::file_size(outputPath), 11u);

    document->saveAs(outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    const auto saved_size = std::filesystem::file_size(outputPath);
    EXPECT_GT(saved_size, 11u)
        << "saveAs to existing path must overwrite (still placeholder size)";

    // Verify the overwritten file is a valid PDF (not just bigger junk).
    auto roundtripped = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(roundtripped, nullptr);
    EXPECT_EQ(roundtripped->pageCount(), document->pageCount());
}

// Pipeline f7c2a4e8 phase-2 (A5 / GAP-51 follow-up): path-traversal /
// embedded-null / ADS safety guards on Document::saveAs(). The tests
// verify the call is rejected via PdfeException BEFORE any file is
// created. Each test asserts both throw-behaviour and that no stray
// file landed in the temp directory.

TEST_F(SaveTest, SaveAs_RejectsEmbeddedNullPath) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // Construct a path containing an embedded NUL. std::filesystem::path
    // happily stores it; the CRT fopen() would truncate, silently
    // writing to "good_part.pdf" instead of the caller's intent.
    std::wstring evil = (std::filesystem::temp_directory_path() /
                        L"good_part.pdf").wstring();
    evil.push_back(L'\0');
    evil += L"malicious.exe";
    std::filesystem::path evilPath(evil);

    EXPECT_THROW(document->saveAs(evilPath), pdfe::core::PdfeException);

    // Neither the intended-looking nor the malicious-looking variant
    // should have been created.
    auto good = std::filesystem::temp_directory_path() / "good_part.pdf";
    if (std::filesystem::exists(good)) {
        std::filesystem::remove(good);
        FAIL() << "saveAs created a file at the pre-NUL portion of the path";
    }
}

TEST_F(SaveTest, SaveAs_RejectsAdsSelectorInFilename) {
#ifndef _WIN32
    GTEST_SKIP() << "Alternate Data Streams are NTFS-only; the ':' filename "
                    "rejection only runs on Windows (see Document.cpp:#ifdef _WIN32)";
#else
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // Windows alternate data stream selector: file.pdf:hidden
    auto evilPath = std::filesystem::temp_directory_path() /
                    "test_save_ads.pdf:hidden";

    EXPECT_THROW(document->saveAs(evilPath), pdfe::core::PdfeException);
#endif
}

TEST_F(SaveTest, SaveAs_RejectsEmptyPath) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    EXPECT_THROW(document->saveAs(std::filesystem::path{}),
                 pdfe::core::PdfeException);
}

TEST_F(SaveTest, SaveAs_AllowsParentTraversal) {
    // Parent-traversal in a path is NOT a saveAs() concern — it's a
    // dialog-layer concern. Document::saveAs() must not block legitimate
    // saves to a parent or sibling directory. This test pins that
    // behaviour so a future over-zealous guard doesn't regress it.
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    auto tmp = std::filesystem::temp_directory_path();
    auto deepTmp = tmp / "pdfe_save_traversal_test";
    std::filesystem::create_directories(deepTmp);
    auto traversalPath = deepTmp / ".." /
                         "pdfe_save_traversal_result.pdf";

    EXPECT_NO_THROW(document->saveAs(traversalPath));

    auto resolved = tmp / "pdfe_save_traversal_result.pdf";
    EXPECT_TRUE(std::filesystem::exists(resolved));
    if (std::filesystem::exists(resolved)) {
        std::filesystem::remove(resolved);
    }
    std::filesystem::remove(deepTmp);
}

TEST_F(SaveTest, SaveAs_ValidPdf) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    document->saveAs(outputPath);

    // Open the saved file and verify it's a valid PDF with correct page count
    auto savedDoc = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(savedDoc, nullptr);
    EXPECT_EQ(savedDoc->pageCount(), document->pageCount());
}

// A1-41/A1-46 oracle repair: page count + page size alone are a proxy oracle —
// a save path that wrote three correctly-sized BLANK pages passed. This now
// asserts the actual page CONTENT round-trips, anchored to the fixture
// generator's authored literals, and that it lands on the CORRECT page (page
// identity, not just "the text is somewhere in the file").
TEST_F(SaveTest, SaveAs_PreservesContent) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    int originalPageCount = document->pageCount();
    ASSERT_EQ(originalPageCount, 3) << "fixture must be the authored 3-page PDF";

    document->saveAs(outputPath);

    auto savedDoc = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(savedDoc, nullptr);
    EXPECT_EQ(savedDoc->pageCount(), originalPageCount);

    // Verify page sizes are preserved
    for (int i = 0; i < originalPageCount; ++i) {
        auto origInfo = document->pageInfo(i);
        auto savedInfo = savedDoc->pageInfo(i);
        EXPECT_NEAR(origInfo.widthPt, savedInfo.widthPt, 0.1f);
        EXPECT_NEAR(origInfo.heightPt, savedInfo.heightPt, 0.1f);
    }

    // --- Content oracle: each saved page carries ITS OWN authored text ---
    ASSERT_EQ(savedDoc->pageCount(), 3);
    auto p0 = savedDoc->page(0);
    auto p1 = savedDoc->page(1);
    auto p2 = savedDoc->page(2);
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    const std::string t0 = p0->extractPlainText();
    const std::string t1 = p1->extractPlainText();
    const std::string t2 = p2->extractPlainText();

    // (1) No page may come back blank — kills a save that emits structurally
    //     valid but empty /Contents streams.
    EXPECT_FALSE(t0.empty()) << "saved page 0 has no text at all";
    EXPECT_FALSE(t1.empty()) << "saved page 1 has no text at all";
    EXPECT_FALSE(t2.empty()) << "saved page 2 has no text at all";

    // (2) Authored text present on the page it was authored on.
    EXPECT_TRUE(containsText(t0, kP1Title)) << "page 0 text: " << t0;
    EXPECT_TRUE(containsText(t0, kP1Body))  << "page 0 text: " << t0;
    EXPECT_TRUE(containsText(t1, kP2Title)) << "page 1 text: " << t1;
    EXPECT_TRUE(containsText(t1, kP2Body))  << "page 1 text: " << t1;
    EXPECT_TRUE(containsText(t2, kP3Title)) << "page 2 text: " << t2;
    EXPECT_TRUE(containsText(t2, kP3Item1)) << "page 2 text: " << t2;
    EXPECT_TRUE(containsText(t2, kP3Rect))  << "page 2 text: " << t2;

    // (3) Page ORDER: a save that permutes or duplicates pages must go RED,
    //     so each page must NOT carry another page's authored heading.
    EXPECT_FALSE(containsText(t0, kP2Title));
    EXPECT_FALSE(containsText(t0, kP3Title));
    EXPECT_FALSE(containsText(t1, kP1Title));
    EXPECT_FALSE(containsText(t1, kP3Title));
    EXPECT_FALSE(containsText(t2, kP1Title));
    EXPECT_FALSE(containsText(t2, kP2Title));

    // (4) Byte-for-byte agreement with the pre-save in-memory document, so a
    //     partial-content save (some spans dropped) also goes RED.
    for (int i = 0; i < originalPageCount; ++i) {
        auto origPage = document->page(i);
        auto savedPage = savedDoc->page(i);
        ASSERT_NE(origPage, nullptr);
        ASSERT_NE(savedPage, nullptr);
        EXPECT_EQ(origPage->extractPlainText(), savedPage->extractPlainText())
            << "saved page " << i << " text differs from the source page";
    }
}

TEST_F(SaveTest, Save_AfterRotation) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // Rotate first page
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(), 0, 90);
    undoManager->execute(std::move(cmd));

    int expectedRotation = document->currentRotation(0);

    document->saveAs(outputPath);

    auto savedDoc = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(savedDoc, nullptr);
    EXPECT_EQ(savedDoc->currentRotation(0), expectedRotation);
}

// A1-46 oracle repair: pageCount() alone cannot tell "deleted page 1" from
// "deleted page 0" or "deleted page 2" — all three leave 2 pages. This asserts
// WHICH page went away and that the two survivors keep their own authored
// content, in order.
TEST_F(SaveTest, Save_AfterDelete) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    ASSERT_EQ(document->pageCount(), 3)
        << "fixture must be the authored 3-page PDF";

    // Delete second page (authored "Page 2 - Body Content")
    auto cmd = std::make_unique<pdfe::commands::DeletePageCommand>(
        document.get(), 1);
    undoManager->execute(std::move(cmd));

    int expectedCount = document->pageCount();
    EXPECT_EQ(expectedCount, 2);

    document->saveAs(outputPath);

    auto savedDoc = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(savedDoc, nullptr);
    ASSERT_EQ(savedDoc->pageCount(), expectedCount);
    ASSERT_EQ(savedDoc->pageCount(), 2);

    auto s0 = savedDoc->page(0);
    auto s1 = savedDoc->page(1);
    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    const std::string t0 = s0->extractPlainText();
    const std::string t1 = s1->extractPlainText();

    // (1) Survivors are not blank — kills a delete/save that keeps the page
    //     objects but drops their /Contents.
    EXPECT_FALSE(t0.empty()) << "surviving page 0 has no text";
    EXPECT_FALSE(t1.empty()) << "surviving page 1 has no text";

    // (2) The survivors are the authored pages 1 and 3, IN ORDER.
    EXPECT_TRUE(containsText(t0, kP1Title)) << "page 0 text: " << t0;
    EXPECT_TRUE(containsText(t0, kP1Body))  << "page 0 text: " << t0;
    EXPECT_TRUE(containsText(t1, kP3Title)) << "page 1 text: " << t1;
    EXPECT_TRUE(containsText(t1, kP3Item1)) << "page 1 text: " << t1;
    EXPECT_TRUE(containsText(t1, kP3Rect))  << "page 1 text: " << t1;

    // (3) The DELETED page's authored content is gone from the whole document —
    //     deleting the wrong index leaves "Page 2 - Body Content" behind and
    //     removes one of the assertions above instead.
    const std::string whole = t0 + "\n" + t1;
    EXPECT_FALSE(containsText(whole, kP2Title))
        << "authored page 2 content survived a delete of page index 1";
    EXPECT_FALSE(containsText(whole, kP2Body))
        << "authored page 2 body survived a delete of page index 1";

    // (4) Order sanity: page 3's heading must not land on index 0.
    EXPECT_FALSE(containsText(t0, kP3Title));
    EXPECT_FALSE(containsText(t1, kP1Title));
}

TEST_F(SaveTest, Save_AfterInsertBlank) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    int initialCount = document->pageCount();

    // Insert blank page
    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document.get(), 1, 612.0f, 792.0f);
    undoManager->execute(std::move(cmd));

    document->saveAs(outputPath);

    auto savedDoc = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(savedDoc, nullptr);
    EXPECT_EQ(savedDoc->pageCount(), initialCount + 1);
}

TEST_F(SaveTest, Save_CleanStateAfterSave) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // Make a change
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        document.get(), 0, 90);
    undoManager->execute(std::move(cmd));

    EXPECT_FALSE(undoManager->isClean());

    // Save and mark clean
    document->saveAs(outputPath);
    undoManager->setClean();

    EXPECT_TRUE(undoManager->isClean());

    // Pipeline a7c2e541 phase-16: verify the saveAs actually produced
    // a non-empty re-openable PDF. Without this check, a regression
    // that left the dirty-flag handling intact but broke the disk write
    // would still pass.
    ASSERT_TRUE(std::filesystem::exists(outputPath));
    ASSERT_GT(std::filesystem::file_size(outputPath), 0u);
    auto reopened = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(reopened, nullptr);
}

TEST_F(SaveTest, IsPdf_ReturnsTrueForPdf) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    EXPECT_TRUE(document->isPdf());
}

TEST_F(SaveTest, Save_InPlace) {
    ASSERT_NE(document, nullptr) << "test_3page.pdf not loaded";

    // First saveAs to create a copy we can modify in-place
    document->saveAs(outputPath);

    // Open the copy
    auto docCopy = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(docCopy, nullptr);
    int originalCount = docCopy->pageCount();

    // Rotate a page
    auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
        docCopy.get(), 0, 90);
    undoManager->execute(std::move(cmd));

    int expectedRotation = docCopy->currentRotation(0);

    // In-place save (uses temp file + atomic rename)
    docCopy->save(outputPath);

    // Re-open and verify
    auto reopened = pdfe::core::Document::open(context, outputPath);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), originalCount);
    EXPECT_EQ(reopened->currentRotation(0), expectedRotation);
}
