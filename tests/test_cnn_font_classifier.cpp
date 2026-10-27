#include <gtest/gtest.h>
#include "tre/CNNFontClassifier.h"
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QCoreApplication>
#include <QFile>
#include <cmath>
#include <vector>

using namespace pdfe::tre;

// ============================================================================
// Helper: resolve model path relative to test executable
// ============================================================================
static std::string testModelPath()
{
    QString appDir = QCoreApplication::applicationDirPath();
    // Post-build deployed: <exe_dir>/models/font_embedder.onnx
    QString path = appDir + "/models/font_embedder.onnx";
    if (QFile::exists(path)) return path.toStdString();
    // MSVC multi-config dev build
    path = appDir + "/../../../../models/font_embedder.onnx";
    if (QFile::exists(path)) return path.toStdString();
    return {};
}

static bool modelAvailable()
{
    return !testModelPath().empty();
}

// Helper: render text in a given font to a QImage
static QImage renderTextImage(const QString& text, const QString& fontFamily, int ptSize = 14)
{
    QImage img(224, 224, QImage::Format_Grayscale8);
    img.fill(255);
    QPainter p(&img);
    QFont font(fontFamily, ptSize);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(img.rect(), Qt::AlignCenter, text);
    p.end();
    return img;
}

// ============================================================================
// Always-run tests (no model needed)
// ============================================================================

TEST(CNNFontClassifierTest, NotAvailableWhenNoModel)
{
    CNNFontClassifier classifier("/nonexistent/path/model.onnx");
    EXPECT_FALSE(classifier.isAvailable());
}

TEST(CNNFontClassifierTest, ExtractEmbeddingReturnsEmptyWhenUnavailable)
{
    CNNFontClassifier classifier("/nonexistent/path/model.onnx");
    QImage img(224, 224, QImage::Format_Grayscale8);
    img.fill(128);
    auto embedding = classifier.extractEmbedding(img);
    EXPECT_TRUE(embedding.empty());
}

TEST(CNNFontClassifierTest, CosineSimilarityIdentical)
{
    std::vector<float> v = {1.0f, 0.0f, 0.0f, 0.0f};
    float sim = CNNFontClassifier::cosineSimilarity(v, v);
    EXPECT_NEAR(sim, 1.0f, 1e-5f);
}

TEST(CNNFontClassifierTest, CosineSimilarityOrthogonal)
{
    std::vector<float> a = {1.0f, 0.0f, 0.0f};
    std::vector<float> b = {0.0f, 1.0f, 0.0f};
    float sim = CNNFontClassifier::cosineSimilarity(a, b);
    EXPECT_NEAR(sim, 0.0f, 1e-5f);
}

TEST(CNNFontClassifierTest, CosineSimilarityEmpty)
{
    std::vector<float> empty;
    std::vector<float> v = {1.0f, 2.0f};
    EXPECT_FLOAT_EQ(CNNFontClassifier::cosineSimilarity(empty, v), 0.0f);
    EXPECT_FLOAT_EQ(CNNFontClassifier::cosineSimilarity(v, empty), 0.0f);
    EXPECT_FLOAT_EQ(CNNFontClassifier::cosineSimilarity(empty, empty), 0.0f);
}

TEST(CNNFontClassifierTest, CosineSimilaritySizeMismatch)
{
    std::vector<float> a = {1.0f, 0.0f};
    std::vector<float> b = {1.0f, 0.0f, 0.0f};
    float sim = CNNFontClassifier::cosineSimilarity(a, b);
    EXPECT_FLOAT_EQ(sim, 0.0f);
}

TEST(CNNFontClassifierTest, EmbeddingSimilarityFallback)
{
    CNNFontClassifier classifier("/nonexistent/path/model.onnx");
    QImage imgA(100, 50, QImage::Format_Grayscale8);
    QImage imgB(100, 50, QImage::Format_Grayscale8);
    imgA.fill(0);
    imgB.fill(255);
    float sim = classifier.embeddingSimilarity(imgA, imgB);
    EXPECT_FLOAT_EQ(sim, 0.0f);
}

TEST(CNNFontClassifierTest, EmptyModelPath)
{
    CNNFontClassifier classifier("");
    EXPECT_FALSE(classifier.isAvailable());
    EXPECT_TRUE(classifier.extractEmbedding(QImage()).empty());
}

TEST(CNNFontClassifierTest, NullImage)
{
    CNNFontClassifier classifier("/nonexistent/path/model.onnx");
    auto embedding = classifier.extractEmbedding(QImage());
    EXPECT_TRUE(embedding.empty());
}

TEST(CNNFontClassifierTest, MoveConstructor)
{
    CNNFontClassifier a("/nonexistent/path/model.onnx");
    CNNFontClassifier b(std::move(a));
    EXPECT_FALSE(b.isAvailable());
}

// ============================================================================
// Conditional tests (skip when model file absent)
// ============================================================================

TEST(CNNFontClassifierTest, LoadModel)
{
    if (!modelAvailable()) GTEST_SKIP() << "CNN model file not found";
    CNNFontClassifier classifier(testModelPath());
    EXPECT_TRUE(classifier.isAvailable());
}

TEST(CNNFontClassifierTest, EmbeddingDimensions)
{
    if (!modelAvailable()) GTEST_SKIP() << "CNN model file not found";
    CNNFontClassifier classifier(testModelPath());
    QImage img = renderTextImage("Hello World", "Arial", 14);
    auto embedding = classifier.extractEmbedding(img);
    EXPECT_FALSE(embedding.empty());
    EXPECT_EQ(embedding.size(), 512u);
}

TEST(CNNFontClassifierTest, SameFontHighSimilarity)
{
    if (!modelAvailable()) GTEST_SKIP() << "CNN model file not found";
    CNNFontClassifier classifier(testModelPath());
    QImage a = renderTextImage("Hello World", "Arial", 14);
    QImage b = renderTextImage("Hello World", "Arial", 14);
    float sim = classifier.embeddingSimilarity(a, b);
    EXPECT_GT(sim, 0.90f) << "Same font+text should have very high similarity";
}

TEST(CNNFontClassifierTest, DifferentFontLowerSimilarity)
{
    if (!modelAvailable()) GTEST_SKIP() << "CNN model file not found";
    CNNFontClassifier classifier(testModelPath());
    QImage a = renderTextImage("Hello World", "Arial", 14);
    QImage b = renderTextImage("Hello World", "Times New Roman", 14);
    float sim = classifier.embeddingSimilarity(a, b);
    // Different fonts should have lower similarity than identical fonts
    // (but may still be somewhat similar for same text)
    EXPECT_LT(sim, 0.99f) << "Different fonts should not match as perfectly as same font";
}
