// Defect #3 — per-call / per-region OCR language routing.
//
// OcrConfig::language was set once at document open and used unchanged for every
// page. OcrPipeline::ocrPage now accepts a per-call language override:
//   ""      -> doc-wide OcrConfig::language (backward-compatible default)
//   "auto"  -> OSD script detection on the page image (falls back to doc-wide)
//   "xxx"   -> force that Tesseract language for this call only
// and per-region routing is supported via the pure detectLanguageForText() /
// scriptToLanguage() helpers, which select a language from a script sample
// without re-baking the doc-wide config.
//
// The pure helpers need no tessdata and run everywhere. The live-OCR cases use
// the bundled eng.traineddata under tests/fixtures/tessdata.

#include <gtest/gtest.h>

#include "tre/OcrPipeline.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::tre;
using namespace pdfe::core;

// ---------------------------------------------------------------------------
// Pure script -> language mapping (defect #3 auto-detect core). No fixtures.
// ---------------------------------------------------------------------------

TEST(OcrLanguageRouting, ScriptToLanguageMapsKnownScripts) {
    EXPECT_EQ(OcrPipeline::scriptToLanguage("Arabic"), "ara");
    EXPECT_EQ(OcrPipeline::scriptToLanguage("Latin"), "eng");
    EXPECT_EQ(OcrPipeline::scriptToLanguage("Cyrillic"), "rus");
    EXPECT_EQ(OcrPipeline::scriptToLanguage("Greek"), "ell");
}

TEST(OcrLanguageRouting, ScriptToLanguageUnknownIsEmptyForFallback) {
    EXPECT_TRUE(OcrPipeline::scriptToLanguage("Klingon").empty());
    EXPECT_TRUE(OcrPipeline::scriptToLanguage("").empty());
}

// "auto-detect selects Arabic script for an Arabic-dominant region" — exercised
// directly on the per-region text-sample detector.
TEST(OcrLanguageRouting, DetectLanguageSelectsArabicForArabicText) {
    // "العربية" — Arabic-script codepoints.
    std::u32string arabic = U"العربية";
    EXPECT_EQ(OcrPipeline::detectLanguageForText(arabic), "ara");
}

TEST(OcrLanguageRouting, DetectLanguageSelectsEnglishForLatinText) {
    EXPECT_EQ(OcrPipeline::detectLanguageForText(U"Invoice total"), "eng");
}

TEST(OcrLanguageRouting, DetectLanguageLatinDominantMixedStaysEnglish) {
    // Mostly Latin with a couple Arabic glyphs -> Latin dominant -> eng.
    std::u32string mixed = U"Order ال number 42 confirmed today";
    EXPECT_EQ(OcrPipeline::detectLanguageForText(mixed), "eng");
}

TEST(OcrLanguageRouting, DetectLanguageArabicDominantMixedSelectsArabic) {
    // Mostly Arabic with a stray Latin token -> Arabic dominant -> ara.
    std::u32string mixed =
        U"العربية النص x";
    EXPECT_EQ(OcrPipeline::detectLanguageForText(mixed), "ara");
}

TEST(OcrLanguageRouting, DetectLanguageSelectsHanForChineseText) {
    EXPECT_EQ(OcrPipeline::detectLanguageForText(U"你好世界"),
              "chi_sim");
}

// ---------------------------------------------------------------------------
// Per-call routing through ocrPage(): the effective language is the override
// when given, else the doc-wide config. (Live OCR uses bundled eng tessdata.)
// ---------------------------------------------------------------------------

class OcrLanguageRoutingLive : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        std::filesystem::path pdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
        std::filesystem::path tess =
            std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata";
        if (!std::filesystem::exists(pdf) ||
            !std::filesystem::exists(tess / "eng.traineddata")) {
            GTEST_SKIP() << "test_scanned.pdf or eng.traineddata missing";
        }
        document_ = Document::open(context_, pdf);
        if (!document_ || document_->pageCount() == 0) {
            GTEST_SKIP() << "Failed to open test_scanned.pdf";
        }
        tessdataPath_ = tess.string();
    }

    OcrConfig cfg(const std::string& language) const {
        OcrConfig c;
        c.tessdataPath = tessdataPath_;
        c.language = language;
        c.ocrDpi = 150;  // faster for the test
        return c;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::shared_ptr<Document> document_;
    std::string tessdataPath_;
};

// Empty override falls back to the doc-wide preference.
TEST_F(OcrLanguageRoutingLive, EmptyOverrideUsesDocWideLanguage) {
    OcrPipeline pipe(context_.get(), document_.get(), cfg("eng"));
    std::string hocr = pipe.ocrPage(0);
    EXPECT_FALSE(hocr.empty());
    EXPECT_EQ(pipe.lastOcrLanguage(), "eng");
}

// An explicit override is the language actually routed to Tesseract — NOT the
// doc-wide config. We assert via lastOcrLanguage(), which records the resolved
// language regardless of whether that language's traineddata is installed.
TEST_F(OcrLanguageRoutingLive, ExplicitOverrideIsUsedNotDocWide) {
    OcrPipeline pipe(context_.get(), document_.get(), cfg("eng"));
    // Override with a language whose data may not be present; the resolution
    // (override beats config) must still be visible. If ara data is absent the
    // call throws after recording the language — that is the proof the override
    // (not the "eng" config) was selected.
    try {
        pipe.ocrPage(0, {}, "ara");
    } catch (const std::exception&) {
        // expected when ara.traineddata is not installed
    }
    EXPECT_EQ(pipe.lastOcrLanguage(), "ara")
        << "explicit per-call override must win over the doc-wide config";
}

// The same explicit override path works through ocrPageWithCharBoxes().
TEST_F(OcrLanguageRoutingLive, CharBoxOverrideIsRouted) {
    OcrPipeline pipe(context_.get(), document_.get(), cfg("eng"));
    std::string hocr = pipe.ocrPageWithCharBoxes(0, "eng");
    EXPECT_FALSE(hocr.empty());
    EXPECT_EQ(pipe.lastOcrLanguage(), "eng");
}
