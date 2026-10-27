#include <gtest/gtest.h>
#include "tre/CNNFontRanker.h"
#include "tre/CNNFontClassifier.h"
#include "tre/VisualFontRanker.h"
#include "tre/TreTypes.h"
#include "fonts/SubstituteFontSelector.h"
#include <QImage>

using namespace pdfe::tre;

// Helper: create a synthetic SubstituteResult for testing
static pdfe::fonts::SubstituteResult makeCandidate(
    const std::string& familyName,
    const std::string& filePath,
    float matchScore)
{
    pdfe::fonts::SubstituteResult result;
    result.profile.familyName = familyName;
    result.filePath = filePath;
    result.matchScore = matchScore;
    result.isKnownPair = false;
    return result;
}

static QImage makeTestRegion(int w = 200, int h = 50)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    return img;
}

// ============================================================================
// Tests -- CNNFontRanker returns empty when model unavailable
// (matchRegion handles fallback to NCC or metric-only)
// ============================================================================

TEST(CNNFontRankerTest, NullClassifierReturnsEmpty)
{
    CNNFontRanker ranker(nullptr);
    auto candidates = {
        makeCandidate("Arial", "arial.ttf", 10.0f),
        makeCandidate("Times", "times.ttf", 20.0f)
    };
    QImage region = makeTestRegion();
    auto ranked = ranker.rankByVisualSimilarity(
        region, candidates, "Hello", 12.0f, RgbColor{0, 0, 0});
    EXPECT_TRUE(ranked.empty());
}

TEST(CNNFontRankerTest, UnavailableModelReturnsEmpty)
{
    CNNFontClassifier classifier("/nonexistent/model.onnx");
    CNNFontRanker ranker(&classifier);
    auto candidates = {
        makeCandidate("Arial", "arial.ttf", 10.0f),
        makeCandidate("Times", "times.ttf", 20.0f)
    };
    QImage region = makeTestRegion();
    auto ranked = ranker.rankByVisualSimilarity(
        region, candidates, "Hello", 12.0f, RgbColor{0, 0, 0});
    EXPECT_TRUE(ranked.empty());
}

TEST(CNNFontRankerTest, EmptyCandidatesReturnsEmpty)
{
    CNNFontClassifier classifier("/nonexistent/model.onnx");
    CNNFontRanker ranker(&classifier);
    std::vector<pdfe::fonts::SubstituteResult> empty;
    QImage region = makeTestRegion();
    auto ranked = ranker.rankByVisualSimilarity(
        region, empty, "Hello", 12.0f, RgbColor{0, 0, 0});
    EXPECT_TRUE(ranked.empty());
}

TEST(CNNFontRankerTest, NullRegionReturnsEmpty)
{
    CNNFontClassifier classifier("/nonexistent/model.onnx");
    CNNFontRanker ranker(&classifier);
    auto candidates = {makeCandidate("Arial", "arial.ttf", 10.0f)};
    auto ranked = ranker.rankByVisualSimilarity(
        QImage(), candidates, "Hello", 12.0f, RgbColor{0, 0, 0});
    EXPECT_TRUE(ranked.empty());
}

TEST(CNNFontRankerTest, EmptyTextReturnsEmpty)
{
    CNNFontClassifier classifier("/nonexistent/model.onnx");
    CNNFontRanker ranker(&classifier);
    auto candidates = {
        makeCandidate("Arial", "arial.ttf", 10.0f),
        makeCandidate("Times", "times.ttf", 5.0f)
    };
    QImage region = makeTestRegion();
    auto ranked = ranker.rankByVisualSimilarity(
        region, candidates, "", 12.0f, RgbColor{0, 0, 0});
    EXPECT_TRUE(ranked.empty());
}
