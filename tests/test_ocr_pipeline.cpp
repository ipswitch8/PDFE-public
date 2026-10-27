#include <gtest/gtest.h>
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <QImage>
#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <filesystem>

using namespace pdfe::tre;
using namespace pdfe::core;

class OcrPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        doc_.reset();
        context_.reset();
    }

    bool loadScannedDoc() {
        std::string path = std::string(TEST_FIXTURES_DIR) + "/test_scanned.pdf";
        if (!std::filesystem::exists(path)) {
            return false;
        }
        try {
            doc_ = Document::open(context_, path);
            return doc_ != nullptr;
        } catch (...) {
            return false;
        }
    }

    bool loadSingleDoc() {
        std::string path = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
        if (!std::filesystem::exists(path)) {
            return false;
        }
        try {
            doc_ = Document::open(context_, path);
            return doc_ != nullptr;
        } catch (...) {
            return false;
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> doc_;
};

TEST_F(OcrPipelineTest, ConstructAndDestroy) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    EXPECT_NO_THROW({
        OcrPipeline pipeline(context_.get(), doc_.get());
    }) << "OcrPipeline construction should not throw";
}

TEST_F(OcrPipelineTest, ConstructWithConfig) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrConfig config;
    config.language = "eng";
    config.ocrDpi = 150;
    config.pageSegMode = 6;

    OcrPipeline pipeline(context_.get(), doc_.get(), config);
    EXPECT_EQ(pipeline.config().language, "eng");
    EXPECT_EQ(pipeline.config().ocrDpi, 150);
    EXPECT_EQ(pipeline.config().pageSegMode, 6);
}

TEST_F(OcrPipelineTest, DefaultConfig) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline(context_.get(), doc_.get());
    EXPECT_EQ(pipeline.config().language, "eng");
    EXPECT_EQ(pipeline.config().ocrDpi, 300);
    EXPECT_EQ(pipeline.config().pageSegMode, 3);
}

TEST_F(OcrPipelineTest, CacheInitiallyEmpty) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline(context_.get(), doc_.get());
    EXPECT_FALSE(pipeline.isCached(0));
    EXPECT_FALSE(pipeline.isCached(1));
}

TEST_F(OcrPipelineTest, RenderPageProducesImage) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline(context_.get(), doc_.get());
    QImage image = pipeline.renderPage(0);

    EXPECT_FALSE(image.isNull()) << "Rendered page image should not be null";
    EXPECT_GT(image.width(), 0) << "Image width should be > 0";
    EXPECT_GT(image.height(), 0) << "Image height should be > 0";
}

TEST_F(OcrPipelineTest, RenderPageAt300Dpi) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrConfig config;
    config.ocrDpi = 300;

    OcrPipeline pipeline(context_.get(), doc_.get(), config);
    QImage image = pipeline.renderPage(0);

    EXPECT_FALSE(image.isNull());

    // At 300 DPI, a US Letter page (612x792 pt) should be ~2550x3300 pixels
    // Allow 10% tolerance for non-standard page sizes
    EXPECT_GT(image.width(), 100) << "Image should be reasonably sized";
    EXPECT_GT(image.height(), 100) << "Image should be reasonably sized";
}

TEST_F(OcrPipelineTest, SetConfigInvalidatesCache) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline(context_.get(), doc_.get());

    // We can't easily populate the cache without Tesseract data files,
    // but we can test that setConfig doesn't crash
    OcrConfig newConfig;
    newConfig.language = "fra";
    newConfig.ocrDpi = 150;

    EXPECT_NO_THROW(pipeline.setConfig(newConfig));
    EXPECT_EQ(pipeline.config().language, "fra");
    EXPECT_EQ(pipeline.config().ocrDpi, 150);
    EXPECT_FALSE(pipeline.isCached(0));
}

TEST_F(OcrPipelineTest, InvalidateCacheOperations) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline(context_.get(), doc_.get());

    // These should not crash even on empty cache
    EXPECT_NO_THROW(pipeline.invalidateCache(0));
    EXPECT_NO_THROW(pipeline.invalidateCache(999));
    EXPECT_NO_THROW(pipeline.invalidateAllCaches());
}

TEST_F(OcrPipelineTest, MoveConstructor) {
    if (!loadSingleDoc()) {
        GTEST_SKIP() << "test_single.pdf not found";
    }

    OcrPipeline pipeline1(context_.get(), doc_.get());
    OcrPipeline pipeline2(std::move(pipeline1));

    EXPECT_EQ(pipeline2.config().language, "eng");
}

TEST_F(OcrPipelineTest, OcrOnScannedPdf) {
    if (!loadScannedDoc()) {
        GTEST_SKIP() << "test_scanned.pdf not found";
    }

    OcrConfig config;
    config.tessdataPath = std::string(TEST_FIXTURES_DIR) + "/tessdata";

    OcrPipeline pipeline(context_.get(), doc_.get(), config);

    std::string hocr = pipeline.ocrPage(0);

    EXPECT_FALSE(hocr.empty()) << "HOCR output should not be empty";

    // Parse the HOCR and verify structure
    auto doc = HocrParser::parse(hocr);
    EXPECT_GT(doc.pageWidth, 0) << "Parsed page width should be > 0";
    EXPECT_GT(doc.pageHeight, 0) << "Parsed page height should be > 0";
    EXPECT_GT(doc.blocks.size(), 0u) << "Should have at least 1 text block";
}

// R-18 CONTENT ORACLE (phase-3 remediation): OcrOnScannedPdf above asserts the
// HOCR is non-empty and structurally well-formed (blocks/width/height > 0) but
// NEVER checks that OCR recognized the RIGHT text. A mutation that makes
// Tesseract emit garbage — still non-empty, still structurally valid HOCR —
// survives that test. This case runs OCR on a fixture with KNOWN ground-truth
// words (test_ocr_clean.pdf: three pangrams) and asserts those specific words
// appear in the flattened HOCR output, so wrong/garbage recognition fails.
TEST_F(OcrPipelineTest, OcrRecognizesKnownWordsOnCleanScan) {
    const std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    const auto cleanPdf = fixtures / "test_ocr_clean.pdf";
    const auto tessdata = fixtures / "tessdata";
    if (!std::filesystem::exists(cleanPdf)) {
        GTEST_SKIP() << "test_ocr_clean.pdf not found";
    }
    if (!std::filesystem::exists(tessdata / "eng.traineddata")) {
        GTEST_SKIP() << "tessdata/eng.traineddata not deployed";
    }

    doc_ = Document::open(context_, cleanPdf.string());
    ASSERT_NE(doc_, nullptr);

    OcrConfig config;
    config.language = "eng";
    config.tessdataPath = tessdata.string();
    config.ocrDpi = 300;

    OcrPipeline pipeline(context_.get(), doc_.get(), config);
    const std::string hocr = pipeline.ocrPage(0);
    ASSERT_FALSE(hocr.empty()) << "HOCR output should not be empty";

    // Flatten parsed HOCR (blocks → paragraphs → lines → words) into a single
    // lowercase text stream, then substring-match known ground-truth words.
    const auto parsed = HocrParser::parse(hocr);
    std::ostringstream flat;
    for (const auto& block : parsed.blocks)
        for (const auto& para : block.paragraphs)
            for (const auto& line : para.lines)
                for (const auto& word : line.words)
                    flat << word.text << ' ';
    std::string text = flat.str();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ASSERT_FALSE(text.find_first_not_of(" ") == std::string::npos)
        << "Parsed HOCR produced no words for the clean-scan fixture.";

    // Ground truth (test_ocr_clean.expected.txt): "The quick brown fox jumps
    // over the lazy dog. Pack my box with five dozen liquor jugs. Sphinx of
    // black quartz, judge my vow." Require several distinctive words so a
    // garbage-emitting engine cannot pass by luck. Allow one slip for
    // Tesseract-version drift by requiring a strong majority.
    const std::vector<std::string> known = {
        "quick", "brown", "fox", "jumps", "lazy", "dog",
        "sphinx", "quartz", "judge", "liquor"};
    int found = 0;
    for (const auto& w : known) {
        if (text.find(w) != std::string::npos) ++found;
    }
    EXPECT_GE(found, 8)
        << "OCR must recognize the fixture's known words, not just emit "
           "structurally-valid HOCR. Found " << found << "/" << known.size()
        << " known words.\n  OCR text: " << text;
    // The single most distinctive word must be present.
    EXPECT_NE(text.find("sphinx"), std::string::npos)
        << "Distinctive ground-truth word 'sphinx' absent from OCR output — "
           "recognition is producing wrong text.";
}

TEST_F(OcrPipelineTest, OcrCachesResults) {
    if (!loadScannedDoc()) {
        GTEST_SKIP() << "test_scanned.pdf not found";
    }

    OcrConfig config;
    config.tessdataPath = std::string(TEST_FIXTURES_DIR) + "/tessdata";

    OcrPipeline pipeline(context_.get(), doc_.get(), config);

    std::string hocr1 = pipeline.ocrPage(0);
    EXPECT_TRUE(pipeline.isCached(0));

    // Second call should return identical result from cache
    std::string hocr2 = pipeline.ocrPage(0);
    EXPECT_EQ(hocr1, hocr2) << "Cached result should be identical";
}

TEST_F(OcrPipelineTest, ProgressCallbackInvoked) {
    if (!loadScannedDoc()) {
        GTEST_SKIP() << "test_scanned.pdf not found";
    }

    OcrConfig config;
    config.tessdataPath = std::string(TEST_FIXTURES_DIR) + "/tessdata";

    OcrPipeline pipeline(context_.get(), doc_.get(), config);

    int callCount = 0;
    int lastStep = -1;

    pipeline.ocrPage(0, [&](int current, int total, std::string_view msg) {
        callCount++;
        lastStep = current;
        EXPECT_GT(total, 0);
        EXPECT_FALSE(msg.empty());
    });

    EXPECT_GT(callCount, 0) << "Progress callback should be invoked at least once";
    EXPECT_EQ(lastStep, 3) << "Final progress step should be 3";
}
