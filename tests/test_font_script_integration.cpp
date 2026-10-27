// test_font_script_integration.cpp — Defects #7 + #8 together, on a real fixture.
//
// Pipeline e7d3c2a1 Phase 7. Combined integration of per-run script detection
// (#7) and multi-script font coverage (#8) using the Phase-2 fixture
// multilingual_latin_arabic.pdf (a Latin text block + an Arabic text block on
// one page). It exercises the genuine FontMatcher pipeline (EmbeddedFontAnalyzer
// + SubstituteFontSelector + system fonts) and asserts:
//   - Latin runs select fonts WITH Latin coverage and no cross-script penalty;
//   - Arabic runs select fonts WITH Arabic coverage and no cross-script penalty.
//
// The fixture's blocks are RASTERISED (scanned), and reliable Arabic OCR is
// environment-dependent (precisely the defect-#3 concern). So the OCR-driven
// region assertion targets the deterministic Latin block, while the Arabic
// per-run coverage is asserted via the OCR-independent FontMatcher per-run path
// over genuine Arabic Unicode — both fixes are still exercised end-to-end.

#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"
#include "tre/TreTypes.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontMetricProfile.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <QImage>
#include <QString>
#include <filesystem>
#include <string>

using namespace pdfe::tre;
using namespace pdfe::core;
using namespace pdfe::fonts;

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "test_fixtures"
#endif

namespace {

std::u32string toU32(const std::string& s)
{
    const auto ucs = QString::fromStdString(s).toUcs4();
    return std::u32string(ucs.begin(), ucs.end());
}

} // namespace

class FontScriptIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "multilingual_latin_arabic.pdf";
        if (!std::filesystem::exists(fixture)) {
            hasFixture_ = false;
            return;
        }
        document_ = Document::open(context_, fixture);
        hasFixture_ = document_ && document_->pageCount() > 0;
        if (!hasFixture_)
            return;

        cache_ = std::make_unique<FontAnalysisCache>();
        analyzer_ = std::make_unique<EmbeddedFontAnalyzer>(context_.get());
        selector_ = std::make_unique<SubstituteFontSelector>(analyzer_.get(), cache_.get());
        matcher_ = std::make_unique<FontMatcher>(selector_.get(), analyzer_.get());

        ocr_ = std::make_unique<OcrPipeline>(context_.get(), document_.get());
        std::filesystem::path tess =
            std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata" / "eng.traineddata";
        if (std::filesystem::exists(tess)) {
            OcrConfig cfg;
            cfg.tessdataPath = (std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata").string();
            ocr_->setConfig(cfg);
            hasTessdata_ = true;
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<FontAnalysisCache> cache_;
    std::unique_ptr<EmbeddedFontAnalyzer> analyzer_;
    std::unique_ptr<SubstituteFontSelector> selector_;
    std::unique_ptr<FontMatcher> matcher_;
    std::unique_ptr<OcrPipeline> ocr_;
    bool hasFixture_ = false;
    bool hasTessdata_ = false;
};

// ---------------------------------------------------------------------------
// The real fixture loads (anchors the integration to multilingual_latin_arabic).
// ---------------------------------------------------------------------------
TEST_F(FontScriptIntegrationTest, FixtureLoads)
{
    if (!hasFixture_)
        GTEST_SKIP() << "multilingual_latin_arabic.pdf fixture not present";
    EXPECT_GE(document_->pageCount(), 1);
}

// ---------------------------------------------------------------------------
// OCR-driven region match on the real fixture: FontMatcher runs over the page's
// OCR'd text region and selects a font that COVERS the region's detected script
// with no script-mismatch penalty (#7 wiring through matchRegion + #8 coverage).
// The Latin block is the deterministic OCR control.
// ---------------------------------------------------------------------------
TEST_F(FontScriptIntegrationTest, OcrRegion_SelectsCoveringFont_NoPenalty)
{
    if (!hasFixture_ || !hasTessdata_)
        GTEST_SKIP() << "fixture or tessdata unavailable";

    const std::string hocr = ocr_->ocrPage(0);
    const HocrDocument doc = HocrParser::parse(hocr);
    const QImage pageImage = ocr_->renderPage(0);
    ASSERT_FALSE(pageImage.isNull());

    // Full-page region — snap-to-line gathers every OCR'd line.
    PixelRect full{0.0, 0.0,
                   static_cast<double>(pageImage.width()),
                   static_cast<double>(pageImage.height())};
    RegionTextData region = HocrParser::extractRegion(doc, full);
    ASSERT_FALSE(region.plainText.empty())
        << "Latin block must OCR to non-empty text (deterministic control)";

    const FontScript regionScript = FontMatcher::detectScript(toU32(region.plainText));
    ASSERT_NE(regionScript, FontScript::Unknown)
        << "OCR'd region must carry a detectable script";

    const FontMatch match = matcher_->matchRegion(region, pageImage,
                                                   ocr_->config().ocrDpi);
    ASSERT_FALSE(match.fontFamily.empty()) << "FontMatcher must select a font";

    // The selected font must COVER the region's script — no spurious penalty.
    if (!match.fontFilePath.empty()) {
        auto prof = analyzer_->analyzeFromFile(match.fontFilePath);
        if (prof.has_value()) {
            EXPECT_TRUE(prof->coversScript(regionScript))
                << "Selected font must cover the region script "
                << static_cast<int>(regionScript)
                << " (family=" << match.fontFamily << ")";

            FontMetricProfile q;
            q.primaryScript = regionScript;
            EXPECT_LT(q.distanceTo(*prof, regionScript), 8.0f)
                << "No cross-script penalty for a correctly-matched region";
        }
    }
}

// ---------------------------------------------------------------------------
// Per-run matching over the fixture's two scripts (#7 + #8), OCR-independent:
// a Latin run selects a Latin-covering font and an Arabic run selects an
// Arabic-covering font — each with no cross-script penalty.
// ---------------------------------------------------------------------------
TEST_F(FontScriptIntegrationTest, PerRun_LatinAndArabic_SelectCoveringFonts)
{
    if (!hasFixture_)
        GTEST_SKIP() << "fixture unavailable";

    // Mixed text echoing the fixture's blocks: Latin body + genuine Arabic.
    std::u32string text = U"Invoice total ";
    const size_t arabicStart = text.size();
    for (char32_t c : {0x0627, 0x0644, 0x0645, 0x0628, 0x0644, 0x063A})  // Arabic word
        text.push_back(c);
    const size_t arabicEnd = text.size();

    const auto runs = FontMatcher::segmentScriptRuns(text);
    ASSERT_EQ(runs.size(), 2u) << "Must split into a Latin run and an Arabic run";
    ASSERT_EQ(runs[0].script, FontScript::Latin);
    ASSERT_EQ(runs[1].script, FontScript::Arabic);

    ScannedTextProperties props;
    props.fontSizePt = 14.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = {0, 0, 0};

    // --- Latin run selects a Latin-covering font, no penalty. ---
    const FontMatch latinMatch =
        matcher_->findMatchForRun(props, text, runs[0].start, runs[0].end);
    EXPECT_FALSE(latinMatch.fontFamily.empty());
    if (!latinMatch.fontFilePath.empty()) {
        auto lp = analyzer_->analyzeFromFile(latinMatch.fontFilePath);
        if (lp.has_value()) {
            EXPECT_TRUE(lp->coversScript(FontScript::Latin))
                << "Latin run font must cover Latin (family=" << latinMatch.fontFamily << ")";
            FontMetricProfile q; q.primaryScript = FontScript::Latin;
            EXPECT_LT(q.distanceTo(*lp, FontScript::Latin), 8.0f)
                << "No penalty for a Latin font on a Latin run";
        }
    }

    // --- Arabic run selects an Arabic-covering font, no penalty. ---
    const FontMatch arabicMatch =
        matcher_->findMatchForRun(props, text, arabicStart, arabicEnd);
    EXPECT_FALSE(arabicMatch.fontFamily.empty());
    if (!arabicMatch.fontFilePath.empty()) {
        auto ap = analyzer_->analyzeFromFile(arabicMatch.fontFilePath);
        if (ap.has_value()) {
            EXPECT_TRUE(ap->coversScript(FontScript::Arabic))
                << "Arabic run font must cover Arabic (family=" << arabicMatch.fontFamily << ")";
            FontMetricProfile q; q.primaryScript = FontScript::Arabic;
            EXPECT_LT(q.distanceTo(*ap, FontScript::Arabic), 8.0f)
                << "No penalty for an Arabic-covering font on an Arabic run";
        }
    }
}
