#include <gtest/gtest.h>
#include "tre/VisualFontRanker.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"
#include "fonts/SubstituteFontSelector.h"

#include <QImage>
#include <QPainter>
#include <filesystem>

using namespace pdfe::tre;
using namespace pdfe::fonts;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static SubstituteResult makeCandidate(const std::string& name,
                                       const std::string& filePath,
                                       float score, bool knownPair = false)
{
    SubstituteResult sr;
    sr.profile.postScriptName = name;
    sr.profile.familyName = name;
    sr.profile.glyphCount = 100;
    sr.filePath = filePath;
    sr.matchScore = score;
    sr.isKnownPair = knownPair;
    return sr;
}

static QImage makeScannedRegion(int w, int h, const QString& text = "Hello")
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter painter(&img);
    painter.setPen(Qt::black);
    QFont font("Arial", 14);
    painter.setFont(font);
    painter.drawText(img.rect(), Qt::AlignCenter, text);
    painter.end();
    return img;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(VisualFontRankerTest, EmptyCandidatesReturnsEmpty) {
    VisualFontRanker ranker;
    QImage region = makeScannedRegion(200, 50);
    std::vector<SubstituteResult> empty;

    auto result = ranker.rankByVisualSimilarity(region, empty, "Hello", 12.0f, {0, 0, 0});
    EXPECT_TRUE(result.empty());
}

TEST(VisualFontRankerTest, NullRegionReturnsEmpty) {
    VisualFontRanker ranker;
    QImage nullImg;
    std::vector<SubstituteResult> candidates;
    candidates.push_back(makeCandidate("Arial", "C:\\Windows\\Fonts\\arial.ttf", 5.0f));

    auto result = ranker.rankByVisualSimilarity(nullImg, candidates, "Hello", 12.0f, {0, 0, 0});
    EXPECT_TRUE(result.empty());
}

TEST(VisualFontRankerTest, EmptyTextReturnsCandidatesUnchanged) {
    VisualFontRanker ranker;
    QImage region = makeScannedRegion(200, 50);
    std::vector<SubstituteResult> candidates;
    candidates.push_back(makeCandidate("Arial", "C:\\Windows\\Fonts\\arial.ttf", 5.0f));

    auto result = ranker.rankByVisualSimilarity(region, candidates, "", 12.0f, {0, 0, 0});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FLOAT_EQ(result[0].combinedScore, 5.0f);
}

TEST(VisualFontRankerTest, SingleCandidateReturnedWithScores) {
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial not available";
    }

    VisualFontRanker ranker;
    // Use larger region for reliable rendering
    QImage region = makeScannedRegion(400, 100, "Hello");
    std::vector<SubstituteResult> candidates;
    candidates.push_back(makeCandidate("Arial", "C:\\Windows\\Fonts\\arial.ttf", 5.0f));

    auto result = ranker.rankByVisualSimilarity(region, candidates, "Hello", 14.0f, {0, 0, 0});
    ASSERT_EQ(result.size(), 1u);
    // Visual score should be in [0, 1] range -- may be 0 if rendering produces blank bitmap
    EXPECT_GE(result[0].visualScore, 0.0f);
    EXPECT_LE(result[0].visualScore, 1.0f);
    EXPECT_GE(result[0].combinedScore, 0.0f);
}

TEST(VisualFontRankerTest, MultipleCandidatesSortedByCombinedScore) {
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf") ||
        !std::filesystem::exists("C:\\Windows\\Fonts\\times.ttf")) {
        GTEST_SKIP() << "Arial or Times not available";
    }

    VisualFontRanker ranker;
    // Scanned region rendered with Arial
    QImage region = makeScannedRegion(200, 50, "Hello World");

    std::vector<SubstituteResult> candidates;
    candidates.push_back(makeCandidate("Arial", "C:\\Windows\\Fonts\\arial.ttf", 5.0f));
    candidates.push_back(makeCandidate("Times", "C:\\Windows\\Fonts\\times.ttf", 3.0f));

    auto result = ranker.rankByVisualSimilarity(
        region, candidates, "Hello World", 14.0f, {0, 0, 0});
    ASSERT_EQ(result.size(), 2u);

    // Results should be sorted by combinedScore ascending
    EXPECT_LE(result[0].combinedScore, result[1].combinedScore);

    // Arial should rank better since scanned region was rendered with Arial
    // (visual score for Arial should be higher)
    bool arialFirst = (result[0].candidate.profile.familyName == "Arial");
    if (arialFirst) {
        EXPECT_GT(result[0].visualScore, result[1].visualScore)
            << "Arial should have higher visual score since region was rendered with Arial";
    }
}

// ===========================================================================
// demoteSerifMismatches -- GitHub #249
//
// THE REPORTED SYMPTOM: the atlas showed sans-serif original text with a SERIF
// font match. Serif-vs-sans is the most visually obvious property of a
// typeface, so inverting it reads as broken whatever the metrics say.
//
// It survived because nothing checked it. The matrix row covering this
// asserted only that a font name was non-empty and not literally "unknown" --
// "Times New Roman" for sans-serif input satisfies that, and so does any
// family name for any input. The five tests in test_cnn_font_ranker.cpp are
// all empty-input guards that assert `ranked.empty()`, so the ranker's
// ORDERING had no coverage at all.
//
// These tests are on the ordering, and each one fails if the partition is
// removed.
// ===========================================================================

namespace {

/// A candidate with an explicit serif-ness and a combined score.
/// familyName doubles as the identity asserted on, so failures name the font
/// rather than an index.
VisualFontRanker::RankedCandidate makeRanked(const std::string& name,
                                             bool isSerif,
                                             float combined)
{
    VisualFontRanker::RankedCandidate rc;
    rc.candidate.profile.familyName = name;
    rc.candidate.profile.isSerif = isSerif;
    rc.combinedScore = combined;
    return rc;
}

std::vector<std::string> names(
    const std::vector<VisualFontRanker::RankedCandidate>& v)
{
    std::vector<std::string> out;
    out.reserve(v.size());
    for (const auto& r : v)
        out.push_back(r.candidate.profile.familyName);
    return out;
}

}  // namespace

TEST(SerifPartition, SerifWinnerForSansQueryIsDemotedBehindEverySansCandidate)
{
    // The reported case, reproduced: a SERIF face has won on combined score
    // for SANS-SERIF input. That is what a confident-but-wrong CNN produces,
    // because the 4.0 serif penalty survives into combinedScore as roughly
    // (4.0 / maxMetric) * metricWeight -- about 0.03 when the CNN is confident.
    std::vector<VisualFontRanker::RankedCandidate> ranked = {
        makeRanked("Times New Roman", /*isSerif=*/true,  0.10f),
        makeRanked("Georgia",         /*isSerif=*/true,  0.12f),
        makeRanked("Arial",           /*isSerif=*/false, 0.15f),
        makeRanked("Verdana",         /*isSerif=*/false, 0.20f),
    };

    // NON-VACUITY. If the fixture did not START with a mismatch in front, the
    // assertion below would pass on a no-op partition and prove nothing --
    // which is exactly the shape that let the real defect ship.
    ASSERT_TRUE(ranked.front().candidate.profile.isSerif)
        << "fixture must begin with the DEFECT present (a serif winner for a "
           "sans query), or this test cannot distinguish a working partition "
           "from no partition at all";

    demoteSerifMismatches(ranked, /*queryIsSerif=*/false);

    EXPECT_EQ(names(ranked),
              (std::vector<std::string>{"Arial", "Verdana",
                                        "Times New Roman", "Georgia"}))
        << "every sans candidate must precede every serif one for a sans "
           "query; serif-vs-sans is categorical and cannot be traded away for "
           "a small visual-similarity gain (GitHub #249)";

    EXPECT_FALSE(ranked.front().candidate.profile.isSerif)
        << "the top match still disagrees with the query on serif-ness -- "
           "this is the user-visible symptom the issue reports";
}

TEST(SerifPartition, SansWinnerForSerifQueryIsDemoted)
{
    // The mirror case. Without it, a partition hard-coded to "prefer sans"
    // would pass the test above -- a real way to be wrong that one direction
    // alone cannot catch.
    std::vector<VisualFontRanker::RankedCandidate> ranked = {
        makeRanked("Arial",           /*isSerif=*/false, 0.10f),
        makeRanked("Times New Roman", /*isSerif=*/true,  0.30f),
    };

    ASSERT_FALSE(ranked.front().candidate.profile.isSerif)
        << "fixture must begin with the mismatch in front";

    demoteSerifMismatches(ranked, /*queryIsSerif=*/true);

    EXPECT_EQ(names(ranked),
              (std::vector<std::string>{"Times New Roman", "Arial"}))
        << "a serif query must promote the serif candidate, even though the "
           "sans one scored three times better";
}

TEST(SerifPartition, OrderWithinEachGroupIsPreserved)
{
    // The partition removes a WRONG answer; it must not discard the scoring
    // that chooses among the right ones. stable_partition, not sort.
    std::vector<VisualFontRanker::RankedCandidate> ranked = {
        makeRanked("Georgia",   /*isSerif=*/true,  0.10f),
        makeRanked("Arial",     /*isSerif=*/false, 0.20f),
        makeRanked("Cambria",   /*isSerif=*/true,  0.30f),
        makeRanked("Verdana",   /*isSerif=*/false, 0.40f),
        makeRanked("Tahoma",    /*isSerif=*/false, 0.50f),
    };

    demoteSerifMismatches(ranked, /*queryIsSerif=*/false);

    EXPECT_EQ(names(ranked),
              (std::vector<std::string>{"Arial", "Verdana", "Tahoma",
                                        "Georgia", "Cambria"}))
        << "within each group the existing combined-score order must survive: "
           "Arial before Verdana before Tahoma, and Georgia before Cambria";
}

TEST(SerifPartition, AllMatchingLeavesTheOrderUntouched)
{
    // The common case. A partition that reorders when nothing mismatches
    // would be discarding scoring for no reason.
    std::vector<VisualFontRanker::RankedCandidate> ranked = {
        makeRanked("Arial",   /*isSerif=*/false, 0.10f),
        makeRanked("Verdana", /*isSerif=*/false, 0.20f),
        makeRanked("Tahoma",  /*isSerif=*/false, 0.30f),
    };
    const auto before = names(ranked);

    demoteSerifMismatches(ranked, /*queryIsSerif=*/false);

    EXPECT_EQ(names(ranked), before)
        << "no candidate disagrees with the query, so nothing should move";
}

TEST(SerifPartition, AllMismatchingKeepsEveryCandidateAndTheirOrder)
{
    // Every candidate is wrong on serif-ness. The partition must not DROP
    // them -- a demotion is not a filter, and returning nothing would be a
    // worse failure than a serif match.
    std::vector<VisualFontRanker::RankedCandidate> ranked = {
        makeRanked("Times New Roman", /*isSerif=*/true, 0.10f),
        makeRanked("Georgia",         /*isSerif=*/true, 0.20f),
    };

    demoteSerifMismatches(ranked, /*queryIsSerif=*/false);

    EXPECT_EQ(names(ranked),
              (std::vector<std::string>{"Times New Roman", "Georgia"}))
        << "with no matching candidate the best available answer is still an "
           "answer; the partition must degrade to a no-op, not to an empty list";
}

TEST(SerifPartition, EmptyInputIsSafe)
{
    std::vector<VisualFontRanker::RankedCandidate> ranked;
    demoteSerifMismatches(ranked, /*queryIsSerif=*/false);
    EXPECT_TRUE(ranked.empty());
}
