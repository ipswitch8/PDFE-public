// test_tessdata_font_detection.cpp - Tests for tessdata deployment and
// font detection pipeline on image-dominated pages.
//
// Verifies:
// 1. eng.traineddata is deployed next to the test exe (post-build step)
// 2. OcrPipeline can initialize Tesseract using exe-relative tessdata
// 3. FontMatcher returns a non-empty family for scanned page OCR data
// 4. Full pipeline: OCR → FontMatcher → font family on test_scanned.pdf

#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"
#include "tre/FontMatcher.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"
#include "tre/CoordMapper.h"
#include "textedit/TextEditModel.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"

#include <QCoreApplication>
#include <QDir>
#include <filesystem>
#include <memory>

using namespace pdfe::core;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

// =============================================================================
// Fixture
// =============================================================================

class TessdataFontDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
    }

    std::shared_ptr<MuPdfContext> context_;
};

// =============================================================================
// 1. Tessdata deployment verification
// =============================================================================

TEST_F(TessdataFontDetectionTest, TessdataDeployed_NextToExe)
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata/eng.traineddata";
    EXPECT_TRUE(QFileInfo::exists(tessPath))
        << "eng.traineddata should be deployed next to exe at: "
        << tessPath.toStdString();
}

TEST_F(TessdataFontDetectionTest, TessdataDeployed_InFixtures)
{
    auto path = std::filesystem::path(fixturesDir()) / "tessdata" / "eng.traineddata";
    EXPECT_TRUE(std::filesystem::exists(path))
        << "eng.traineddata should exist in test fixtures at: "
        << path.string();
}

// =============================================================================
// 2. OcrPipeline initialization with exe-relative tessdata
// =============================================================================

TEST_F(TessdataFontDetectionTest, OcrPipeline_InitializesWithExeTessdata)
{
    auto scannedPath = std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
    if (!std::filesystem::exists(scannedPath)) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    auto doc = Document::open(context_, scannedPath);
    ASSERT_NE(doc, nullptr);

    // Use the exe-relative tessdata (deployed by post-build step)
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        // Fallback to fixture tessdata
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);

    // ocrPage should succeed without throwing
    std::string hocr;
    ASSERT_NO_THROW(hocr = pipeline.ocrPage(0))
        << "OCR should succeed with tessdata at: " << config.tessdataPath;
    EXPECT_FALSE(hocr.empty()) << "OCR should return non-empty HOCR";
}

// =============================================================================
// 3. FontMatcher returns non-empty family from OCR data
// =============================================================================

TEST_F(TessdataFontDetectionTest, FontMatcher_DetectsFont_FromOcrData)
{
    auto scannedPath = std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
    if (!std::filesystem::exists(scannedPath)) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    auto doc = Document::open(context_, scannedPath);
    ASSERT_NE(doc, nullptr);

    // Initialize OCR
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);
    std::string hocr = pipeline.ocrPage(0);
    ASSERT_FALSE(hocr.empty());

    // Parse HOCR
    auto hocrDoc = pdfe::tre::HocrParser::parse(hocr);
    ASSERT_FALSE(hocrDoc.blocks.empty()) << "HOCR should contain text blocks";

    // Build RegionTextData
    pdfe::tre::RegionTextData regionData;
    std::string plainText;
    for (const auto& block : hocrDoc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                regionData.lines.push_back(line);
                for (const auto& word : line.words) {
                    if (!plainText.empty()) plainText += ' ';
                    plainText += word.text;
                }
            }
        }
    }
    regionData.plainText = plainText;
    EXPECT_FALSE(plainText.empty()) << "OCR should detect some text";

    // Initialize FontMatcher
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    // Render page image for visual analysis
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QImage pageImage = page->render(page->boundingBox());
    ASSERT_FALSE(pageImage.isNull());

    // Detect font
    auto fontMatch = matcher.matchRegion(regionData, pageImage, 300);
    EXPECT_FALSE(fontMatch.fontFamily.empty())
        << "FontMatcher should return a non-empty font family";
    EXPECT_GT(fontMatch.confidence, 0.0f)
        << "FontMatcher should return non-zero confidence";
    EXPECT_GT(fontMatch.sizePt, 0.0f)
        << "FontMatcher should detect a non-zero font size";
}

// =============================================================================
// 4. Full pipeline on image-dominated page
// =============================================================================

TEST_F(TessdataFontDetectionTest, FullPipeline_ImageDominatedPage)
{
    auto scannedPath = std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
    if (!std::filesystem::exists(scannedPath)) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    auto doc = Document::open(context_, scannedPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Verify it's image-dominated
    bool imageDom = page->isImageDominated();
    // test_scanned.pdf should be image-dominated (it's a scanned page)
    // If not, the test is still valid — just tests the pipeline
    if (!imageDom) {
        // Still run the pipeline to verify it works
    }

    // Initialize full pipeline
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);

    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto fontCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), fontCache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    // Run full detection pipeline (same as MainWindow code path)
    bool fontOverrideSet = false;
    std::string detectedFamily;

    std::string hocrXml = pipeline.ocrPage(0);
    ASSERT_FALSE(hocrXml.empty());

    auto hocrDoc = pdfe::tre::HocrParser::parse(hocrXml);
    ASSERT_FALSE(hocrDoc.blocks.empty());

    pdfe::tre::RegionTextData regionData;
    std::string plainText;
    for (const auto& block : hocrDoc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                regionData.lines.push_back(line);
                for (const auto& word : line.words) {
                    if (!plainText.empty()) plainText += ' ';
                    plainText += word.text;
                }
            }
        }
    }
    regionData.plainText = plainText;

    QImage pageImage = page->render(page->boundingBox());
    ASSERT_FALSE(pageImage.isNull());

    auto fontMatch = matcher.matchRegion(regionData, pageImage, 300);
    if (!fontMatch.fontFamily.empty()) {
        detectedFamily = fontMatch.fontFamily;
        fontOverrideSet = true;
    }

    EXPECT_TRUE(fontOverrideSet)
        << "Full pipeline should detect a font family for scanned page";
    EXPECT_FALSE(detectedFamily.empty())
        << "Detected family should be non-empty";

    // The detected font should correspond to a real file on disk
    // (SubstituteFontSelector returns fonts with filePath set)
    EXPECT_FALSE(fontMatch.fontFilePath.empty())
        << "Detected font '" << detectedFamily
        << "' should have a valid file path";
}

// =============================================================================
// 5. OcrPipeline fails gracefully without tessdata
// =============================================================================

TEST_F(TessdataFontDetectionTest, OcrPipeline_FailsGracefully_NoTessdata)
{
    auto scannedPath = std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
    if (!std::filesystem::exists(scannedPath)) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    auto doc = Document::open(context_, scannedPath);
    ASSERT_NE(doc, nullptr);

    // Point to non-existent tessdata directory
    pdfe::tre::OcrConfig config;
    config.tessdataPath = "C:/nonexistent/tessdata";

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);

    // ocrPage should throw (not crash) when tessdata is missing
    EXPECT_THROW(pipeline.ocrPage(0), std::runtime_error)
        << "OCR with missing tessdata should throw runtime_error";
}

// =============================================================================
// 6. FontMatcher detectProperties doesn't crash on empty data
// =============================================================================

TEST_F(TessdataFontDetectionTest, FontMatcher_DetectProperties_EmptyRegion)
{
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    pdfe::tre::RegionTextData emptyRegion;
    QImage emptyImage;

    auto props = matcher.detectProperties(emptyRegion, emptyImage, 300);

    // Should return defaults without crashing
    EXPECT_FLOAT_EQ(props.fontSizePt, 12.0f) << "Default font size should be 12pt";
    EXPECT_FLOAT_EQ(props.lineSpacingRatio, 1.2f) << "Default line spacing should be 1.2";
}

// =============================================================================
// 7. Tessdata path resolution robustness
// =============================================================================

TEST_F(TessdataFontDetectionTest, TessdataPath_AtLeastOneResolvable)
{
    // Verify that at least one path resolution works in the test environment
    QString appDir = QCoreApplication::applicationDirPath();

    bool found = false;
    QStringList candidates = {
        appDir + "/tessdata",
        appDir + "/../tessdata",
        appDir + "/../../../../tests/fixtures/tessdata",
        appDir + "/../../tests/fixtures/tessdata",
    };

    for (const auto& path : candidates) {
        if (QDir(path).exists("eng.traineddata")) {
            found = true;
            break;
        }
    }

    // Also check fixture path directly
    auto fixturePath = std::filesystem::path(fixturesDir()) / "tessdata" / "eng.traineddata";
    if (std::filesystem::exists(fixturePath)) {
        found = true;
    }

    EXPECT_TRUE(found) << "At least one tessdata path must resolve in test environment. "
                       << "appDir=" << appDir.toStdString();
}

// ---------------------------------------------------------------------------
// GitHub #261 -- the scanned document these three cases run against.
//
// They used to hardcode an absolute path inside another developer's home
// directory (a real bank deposit letter). That path exists on no machine in
// the project, so all three GTEST_SKIP()'d on every run, everywhere, for as
// long as the path had been wrong -- three of the suite's 22 live skips.
//
// The original input genuinely cannot be committed: it is third-party personal
// financial data. But nothing these tests ASSERT needs it. They check that the
// page is image-dominated, that OCR finds text, that a font is detected, that
// the script is Latin, and that an English document is not matched to an Arabic
// font. A committed scanned fixture exercises every one of those.
//
// So the resolution order is: an explicit override first, for a developer who
// does have a real-world document and wants these run against it, then the
// committed fixture. There is no third branch -- falling through to a skip is
// what made these invisible for months.
std::filesystem::path scannedDocumentUnderTest()
{
    if (const char* env = std::getenv("PDFE_SCANNED_PDF")) {
        if (env[0] != '\0') {
            std::filesystem::path p(env);
            if (std::filesystem::exists(p)) return p;
        }
    }
    return std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
}

// GitHub #261. The ghost-text case needs a document that carries an INVISIBLE
// text layer over the scan -- it compares an OCR line's bbox against the PDF
// span underneath it, and a pure scan has no spans to compare with. The
// fixture above deliberately has none (it is a clean scan), so this case gets
// the OCR-layer fixture instead. Same override hook, different default.
std::filesystem::path ghostTextDocumentUnderTest()
{
    if (const char* env = std::getenv("PDFE_GHOSTTEXT_PDF")) {
        if (env[0] != '\0') {
            std::filesystem::path p(env);
            if (std::filesystem::exists(p)) return p;
        }
    }
    return std::filesystem::path(fixturesDir()) / "test_scanned_with_ocr.pdf";
}

// =============================================================================
// 8. Scanned PDF: font detection returns a Latin font, not an Arabic one
// =============================================================================

TEST_F(TessdataFontDetectionTest, RealWorldPdf_FontDetection_SansSerif)
{
    // Use a real scanned PDF known to trigger the serif-selection bug
    const std::filesystem::path pdfPath = scannedDocumentUnderTest();
    ASSERT_TRUE(std::filesystem::exists(pdfPath))
        << "no scanned document resolved at " << pdfPath.string();

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(page->isImageDominated())
        << "This PDF should be detected as image-dominated";

    // Initialize OCR
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);
    std::string hocrXml = pipeline.ocrPage(0);
    ASSERT_FALSE(hocrXml.empty()) << "OCR should find text on this page";

    auto hocrDoc = pdfe::tre::HocrParser::parse(hocrXml);
    ASSERT_FALSE(hocrDoc.blocks.empty());

    pdfe::tre::RegionTextData regionData;
    std::string plainText;
    for (const auto& block : hocrDoc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                regionData.lines.push_back(line);
                for (const auto& word : line.words) {
                    if (!plainText.empty()) plainText += ' ';
                    plainText += word.text;
                }
            }
        }
    }
    regionData.plainText = plainText;

    // Initialize FontMatcher
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto fontCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), fontCache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    QImage pageImage = page->render(page->boundingBox());
    ASSERT_FALSE(pageImage.isNull());

    auto fontMatch = matcher.matchRegion(regionData, pageImage, 300);

    // Report what was detected (visible in test output even if test passes)
    std::cout << "  Detected font family: " << fontMatch.fontFamily << std::endl;
    std::cout << "  Detected font style:  " << fontMatch.fontStyle << std::endl;
    std::cout << "  Detected font file:   " << fontMatch.fontFilePath << std::endl;
    std::cout << "  Confidence:           " << fontMatch.confidence << std::endl;
    std::cout << "  Size (pt):            " << fontMatch.sizePt << std::endl;

    EXPECT_FALSE(fontMatch.fontFamily.empty())
        << "Should detect a font for this PDF";

    // Verify it's a Latin font (English document)
    auto u32text = pdfe::tre::utf8ToU32(plainText);
    auto detectedScript = pdfe::tre::FontMatcher::detectScript(u32text);
    EXPECT_EQ(detectedScript, pdfe::fonts::FontScript::Latin)
        << "English document should detect Latin script";

    // Scheherazade must be classified as Arabic (not Latin)
    auto scheherazadeProfile = analyzer->analyzeFromFile("C:\\Windows\\Fonts\\scheherazade-bold.ttf");
    if (scheherazadeProfile) {
        EXPECT_EQ(scheherazadeProfile->primaryScript, pdfe::fonts::FontScript::Arabic)
            << "Scheherazade should be classified as Arabic, not Latin";
    }

    // The detected font should NOT be Scheherazade (Arabic font for English text)
    EXPECT_TRUE(fontMatch.fontFamily.find("Scheherazade") == std::string::npos)
        << "English document should not match Scheherazade, got: "
        << fontMatch.fontFamily;
}

// =============================================================================
// 9. Diagnostic: font size vs bitmap dimensions in save pipeline
// =============================================================================

TEST_F(TessdataFontDetectionTest, RealWorldPdf_FontSize_FitsInSpanBbox)
{
    const std::filesystem::path pdfPath = scannedDocumentUnderTest();
    ASSERT_TRUE(std::filesystem::exists(pdfPath))
        << "no scanned document resolved at " << pdfPath.string();

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Initialize OCR
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);
    std::string hocrXml = pipeline.ocrPage(0);
    ASSERT_FALSE(hocrXml.empty());

    auto hocrDoc = pdfe::tre::HocrParser::parse(hocrXml);

    // Find the "Dear Member" line
    // GitHub #261. This used to search for a line containing "Dear" or
    // "Member" -- words from one bank letter -- and skip when it found none.
    // What the test then ASSERTS is that the detected font size fits the line's
    // own bbox, which has nothing to do with the words. Selecting the widest
    // line with at least two words is deterministic, works on any document, and
    // favours a line with real content over a stray OCR fragment.
    pdfe::tre::HocrLine targetLine;
    bool found = false;
    for (const auto& block : hocrDoc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                if (line.words.size() < 2) continue;
                if (!found || line.bbox.width() > targetLine.bbox.width()) {
                    targetLine = line;
                    found = true;
                }
            }
        }
    }
    ASSERT_TRUE(found)
        << "OCR produced no line with two or more words on "
        << pdfPath.string() << ", so there is nothing to measure. That is a "
           "failure of the OCR path, not a reason to skip.";

    // Build region data from just this line
    pdfe::tre::RegionTextData regionData;
    regionData.lines.push_back(targetLine);
    std::string plainText;
    for (const auto& w : targetLine.words) {
        if (!plainText.empty()) plainText += ' ';
        plainText += w.text;
    }
    regionData.plainText = plainText;
    regionData.pixelBbox = targetLine.bbox;

    std::cout << "\n  === Font Size Diagnostic ===" << std::endl;
    std::cout << "  Target line text: '" << plainText << "'" << std::endl;
    std::cout << "  Line bbox (px): (" << targetLine.bbox.x0 << ", " << targetLine.bbox.y0
              << ", " << targetLine.bbox.x1 << ", " << targetLine.bbox.y1 << ")" << std::endl;
    std::cout << "  Line size (px): " << targetLine.bbox.width()
              << " x " << targetLine.bbox.height() << std::endl;
    std::cout << "  Line xSizePx: " << targetLine.xSizePx << std::endl;
    std::cout << "  Line ascendersPx: " << targetLine.ascendersPx << std::endl;
    std::cout << "  Line descendersPx: " << targetLine.descendersPx << std::endl;

    // Detect font match
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto fontCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), fontCache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    QImage pageImage = page->render(page->boundingBox());
    auto fontMatch = matcher.matchRegion(regionData, pageImage, 300);

    std::cout << "  Detected sizePt: " << fontMatch.sizePt << std::endl;
    std::cout << "  Font: " << fontMatch.fontFamily << " (" << fontMatch.fontFilePath << ")" << std::endl;

    // Simulate what ImageReplacer does
    int dpi = 300;
    int pixelWidth = std::max(1, static_cast<int>(targetLine.bbox.width()));
    int pixelHeight = std::max(1, static_cast<int>(targetLine.bbox.height()));

    float maxFontPt = static_cast<float>(pixelHeight) * 72.0f
                      / static_cast<float>(dpi) / 1.2f;
    std::cout << "  pixelWidth: " << pixelWidth << std::endl;
    std::cout << "  pixelHeight: " << pixelHeight << std::endl;
    std::cout << "  maxFontPt (/ 1.2): " << maxFontPt << std::endl;

    if (fontMatch.sizePt > maxFontPt) {
        std::cout << "  *** CAPPING to " << maxFontPt << " from " << fontMatch.sizePt << std::endl;
        fontMatch.sizePt = maxFontPt;
    }

    // Now render with BitmapCompositor to check actual dimensions
    if (!fontMatch.fontFilePath.empty() && std::filesystem::exists(fontMatch.fontFilePath)) {
        pdfe::tre::BitmapCompositor compositor;
        std::u32string text = pdfe::tre::utf8ToU32(plainText);

        QImage rendered = compositor.renderText(text, fontMatch, pixelWidth, pixelHeight, dpi);
        std::cout << "  Rendered bitmap: " << rendered.width() << " x " << rendered.height() << std::endl;

        // Count pixels outside the target area
        int nonTransparent = 0;
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x)
                if (qAlpha(rendered.pixel(x, y)) > 0) nonTransparent++;
        std::cout << "  Non-transparent pixels: " << nonTransparent << std::endl;

        EXPECT_GT(nonTransparent, 0) << "Should render some text";
    }
}

// =============================================================================
// 10. Diagnostic: ghost text - PDF span bbox vs HOCR line bbox overlap
// =============================================================================

TEST_F(TessdataFontDetectionTest, RealWorldPdf_GhostText_BboxOverlap)
{
    const std::filesystem::path pdfPath = ghostTextDocumentUnderTest();
    ASSERT_TRUE(std::filesystem::exists(pdfPath))
        << "no scanned document resolved at " << pdfPath.string();

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(page->isImageDominated());

    // Initialize OCR
    pdfe::tre::OcrConfig config;
    QString appDir = QCoreApplication::applicationDirPath();
    QString tessPath = appDir + "/tessdata";
    if (QDir(tessPath).exists("eng.traineddata")) {
        config.tessdataPath = tessPath.toStdString();
    } else {
        config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    }

    pdfe::tre::OcrPipeline pipeline(context_.get(), doc.get(), config);

    // Get CoordMapper transform to convert PDF bbox to pixel coordinates
    pdfe::tre::CoordMapper coordMapper(context_.get(), doc.get());
    const auto& transform = coordMapper.getTransform(0);

    // OCR the page to get HOCR data
    std::string hocrXml = pipeline.ocrPage(0);
    ASSERT_FALSE(hocrXml.empty());
    auto hocrDoc = pdfe::tre::HocrParser::parse(hocrXml);

    // GitHub #261. Was "find the Dear Member: line", i.e. keyed to one bank
    // letter; now the widest line with two or more words. The overlap property
    // asserted below holds for any line.
    pdfe::tre::HocrLine dearMemberLine;
    bool foundLine = false;
    for (const auto& blk : hocrDoc.blocks) {
        for (const auto& par : blk.paragraphs) {
            for (const auto& ln : par.lines) {
                if (ln.words.size() < 2) continue;
                if (!foundLine || ln.bbox.width() > dearMemberLine.bbox.width()) {
                    dearMemberLine = ln;
                    foundLine = true;
                }
            }
        }
    }
    ASSERT_TRUE(foundLine)
        << "OCR produced no line with two or more words on "
        << pdfPath.string() << ", so there is no span to overlap. That is a "
           "failure of the OCR path, not a reason to skip.";
    {
        std::string lineText;
        for (const auto& w : dearMemberLine.words) {
            if (!lineText.empty()) lineText += ' ';
            lineText += w.text;
        }
        std::cout << "\n  === Ghost Text Bbox Diagnostic ===" << std::endl;
        std::cout << "  HOCR line text: '" << lineText << "'" << std::endl;
        std::cout << "  HOCR line bbox (px): ("
                  << dearMemberLine.bbox.x0 << ", " << dearMemberLine.bbox.y0 << ", "
                  << dearMemberLine.bbox.x1 << ", " << dearMemberLine.bbox.y1 << ")"
                  << " size=" << dearMemberLine.bbox.width()
                  << "x" << dearMemberLine.bbox.height() << std::endl;
    }

    // Now get the PDF text spans using TextEditModel to find the span bbox
    // (the invisible Tr=3 text position)
    auto textBlocks = page->extractText();
    pdfe::textedit::TextEditModel model(textBlocks);
    int spanCount = model.spanCount();

    std::cout << "  TextEditModel spans: " << spanCount << std::endl;

    // GitHub #261. Widest span, not the one containing "Dear". The overlap
    // property asserted below is about geometry and holds for any span; keying
    // it to one document's wording is what kept this test skipped.
    int targetSpanIdx = -1;
    for (int i = 0; i < spanCount; ++i) {
        const auto& span = model.span(i);
        if (span.text.empty()) continue;
        if (targetSpanIdx < 0 ||
            span.boundingBox.width() > model.span(targetSpanIdx).boundingBox.width()) {
            targetSpanIdx = i;
        }
    }
    if (targetSpanIdx >= 0) {
        const auto& span = model.span(targetSpanIdx);
        std::string utf8;
        for (char32_t c : span.text) {
            if (c < 128) utf8 += static_cast<char>(c);
        }
        std::cout << "  Target span[" << targetSpanIdx << "]: '" << utf8 << "'" << std::endl;
        std::cout << "  Span bbox (PDF fitz): ("
                 << span.boundingBox.left() << ", " << span.boundingBox.top() << ", "
                 << span.boundingBox.right() << ", " << span.boundingBox.bottom() << ")"
                 << std::endl;
        std::cout << "  Font: " << span.fontName << " size=" << span.fontSize << std::endl;
    }

    if (targetSpanIdx < 0) {
        // Print all spans for debugging
        std::cout << "\n  All spans:" << std::endl;
        for (int i = 0; i < std::min(20, spanCount); ++i) {
            const auto& span = model.span(i);
            std::string utf8;
            for (char32_t c : span.text) {
                if (c < 128) utf8 += static_cast<char>(c);
            }
            std::cout << "  Span[" << i << "]: '" << utf8.substr(0, 40) << "'"
                     << " bbox=(" << span.boundingBox.left()
                     << "," << span.boundingBox.top()
                     << "," << span.boundingBox.right()
                     << "," << span.boundingBox.bottom() << ")" << std::endl;
        }
        FAIL() << "TextEditModel exposed no usable text span on "
               << pdfPath.string() << ". This fixture carries an invisible OCR "
                  "layer, so zero spans means the span extraction is broken -- "
                  "which is a failure to report, not a reason to skip.";
    }

    // Convert span bbox to pixel coordinates (same as ImageReplacer)
    const auto& span = model.span(targetSpanIdx);
    pdfe::tre::PdfRect selectionPdf;
    selectionPdf.x0 = span.boundingBox.left();
    selectionPdf.y0 = span.boundingBox.top();
    selectionPdf.x1 = span.boundingBox.right();
    selectionPdf.y1 = span.boundingBox.bottom();
    pdfe::tre::PixelRect pixelSelection = coordMapper.pdfRectToPixel(selectionPdf, transform);

    std::cout << "  pixelSelection (from span bbox): ("
             << pixelSelection.x0 << ", " << pixelSelection.y0 << ", "
             << pixelSelection.x1 << ", " << pixelSelection.y1 << ")"
             << " size=" << pixelSelection.width() << "x" << pixelSelection.height()
             << std::endl;

    // Check overlap with HOCR lines
    std::cout << "\n  Checking overlap between pixelSelection and ALL HOCR lines:" << std::endl;
    int overlapCount = 0;
    pdfe::tre::PixelRect closestLineBbox;
    double minYDist = 1e9;
    double bestOverlapArea = 0.0;
    pdfe::tre::HocrLine pairedLine;
    bool havePairedLine = false;
    for (const auto& blk : hocrDoc.blocks) {
        for (const auto& par : blk.paragraphs) {
            for (const auto& ln : par.lines) {
                bool overlaps = ln.bbox.intersects(pixelSelection);
                if (overlaps) ++overlapCount;
                double yCenter1 = (pixelSelection.y0 + pixelSelection.y1) / 2.0;
                double yCenter2 = (ln.bbox.y0 + ln.bbox.y1) / 2.0;
                double yDist = std::abs(yCenter1 - yCenter2);
                std::string lineText;
                for (const auto& w : ln.words) {
                    if (!lineText.empty()) lineText += ' ';
                    lineText += w.text;
                }
                if (lineText.find("Dear") != std::string::npos || overlaps) {
                    std::cout << "  Line: '" << lineText.substr(0, 50) << "'"
                             << " bbox=(" << ln.bbox.x0 << "," << ln.bbox.y0
                             << "," << ln.bbox.x1 << "," << ln.bbox.y1 << ")"
                             << " overlaps=" << overlaps
                             << " yDist=" << yDist << std::endl;
                }
                if (yDist < minYDist) {
                    minYDist = yDist;
                    closestLineBbox = ln.bbox;
                }
                // GitHub #261. The OCR line this span actually sits on, by
                // intersection area. The coverage assertion below compares the
                // span's render bbox against a line, and that only means
                // anything if the two are the SAME line of text. Selecting the
                // span and the line independently -- as the first cut of this
                // fix did, each by width -- compares a span against whatever
                // unrelated line happened to be widest, and fails for a reason
                // that says nothing about ghost text.
                if (overlaps) {
                    const double ix0 = std::max(ln.bbox.x0, pixelSelection.x0);
                    const double iy0 = std::max(ln.bbox.y0, pixelSelection.y0);
                    const double ix1 = std::min(ln.bbox.x1, pixelSelection.x1);
                    const double iy1 = std::min(ln.bbox.y1, pixelSelection.y1);
                    const double area = std::max(0.0, ix1 - ix0) * std::max(0.0, iy1 - iy0);
                    if (area > bestOverlapArea) {
                        bestOverlapArea = area;
                        pairedLine = ln;
                        havePairedLine = true;
                    }
                }
            }
        }
    }

    std::cout << "\n  Summary:" << std::endl;
    std::cout << "  Overlapping HOCR lines: " << overlapCount << std::endl;
    std::cout << "  Closest HOCR line yDist: " << minYDist << std::endl;
    std::cout << "  Closest bbox: (" << closestLineBbox.x0 << "," << closestLineBbox.y0
             << "," << closestLineBbox.x1 << "," << closestLineBbox.y1 << ")" << std::endl;

    // Simulate extractRegion
    pdfe::tre::RegionTextData regionData =
        pdfe::tre::HocrParser::extractRegion(hocrDoc, pixelSelection);

    std::cout << "  extractRegion lines: " << regionData.lines.size() << std::endl;
    std::cout << "  extractRegion pixelBbox: ("
             << regionData.pixelBbox.x0 << "," << regionData.pixelBbox.y0 << ","
             << regionData.pixelBbox.x1 << "," << regionData.pixelBbox.y1 << ")"
             << " size=" << regionData.pixelBbox.width() << "x"
             << regionData.pixelBbox.height() << std::endl;

    // Compute renderBbox (same logic as ImageReplacer::replace for snapToLines=false)
    pdfe::tre::PixelRect renderBbox = pixelSelection;
    if (regionData.pixelBbox.width() > 0 && regionData.pixelBbox.height() > 0) {
        renderBbox.x0 = std::min(renderBbox.x0, regionData.pixelBbox.x0);
        renderBbox.y0 = std::min(renderBbox.y0, regionData.pixelBbox.y0);
        renderBbox.x1 = std::max(renderBbox.x1, regionData.pixelBbox.x1);
        renderBbox.y1 = std::max(renderBbox.y1, regionData.pixelBbox.y1);
    }
    std::cout << "  renderBbox (union): ("
             << renderBbox.x0 << "," << renderBbox.y0 << ","
             << renderBbox.x1 << "," << renderBbox.y1 << ")"
             << " size=" << renderBbox.width() << "x" << renderBbox.height()
             << std::endl;

    // GitHub #261. Compare against the line the selected span actually sits on,
    // not a line picked independently. `pairedLine` is the OCR line with the
    // largest intersection with this span's pixel selection.
    ASSERT_TRUE(havePairedLine)
        << "the selected text span overlaps no OCR line at all on "
        << pdfPath.string() << ". The span and the scan are describing "
           "different places, which IS the ghost-text failure this test is "
           "named for -- reported rather than skipped.";
    const pdfe::tre::HocrLine& comparedLine = pairedLine;

    // Check: does renderBbox cover the OCR line the span sits on?
    bool coversHocr = renderBbox.y0 <= comparedLine.bbox.y0 &&
                      renderBbox.y1 >= comparedLine.bbox.y1 &&
                      renderBbox.x0 <= comparedLine.bbox.x0 &&
                      renderBbox.x1 >= comparedLine.bbox.x1;
    std::cout << "  renderBbox covers HOCR line: " << coversHocr << std::endl;

    if (!coversHocr) {
        std::cout << "\n  *** GHOST TEXT ROOT CAUSE: renderBbox does NOT cover HOCR line!" << std::endl;
        std::cout << "  *** Gap: top=" << (renderBbox.y0 - comparedLine.bbox.y0)
                 << " bottom=" << (comparedLine.bbox.y1 - renderBbox.y1)
                 << " left=" << (renderBbox.x0 - comparedLine.bbox.x0)
                 << " right=" << (comparedLine.bbox.x1 - renderBbox.x1) << std::endl;
    }

    EXPECT_TRUE(coversHocr) << "renderBbox must cover the HOCR text line to prevent ghost text";
}
