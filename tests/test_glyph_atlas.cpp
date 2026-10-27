#include <gtest/gtest.h>
#include "tre/GlyphAtlas.h"
#include "tre/TreTypes.h"
#include <QImage>

using namespace pdfe::tre;

class GlyphAtlasTest : public ::testing::Test {
protected:
    // Create a simple test image with black text on white background
    QImage makeTestImage(int w, int h) {
        QImage img(w, h, QImage::Format_ARGB32);
        img.fill(QColor(255, 255, 255));  // white background
        // Draw some black pixels in specific regions for "characters"
        for (int y = 10; y < 30; ++y) {
            for (int x = 10; x < 25; ++x) {
                img.setPixel(x, y, qRgb(0, 0, 0));  // 'A' region
            }
            for (int x = 30; x < 45; ++x) {
                img.setPixel(x, y, qRgb(0, 0, 0));  // 'B' region
            }
            for (int x = 50; x < 65; ++x) {
                img.setPixel(x, y, qRgb(0, 0, 0));  // 'C' region
            }
        }
        return img;
    }

    // Create HOCR lines with char-level data
    std::vector<HocrLine> makeTestLines() {
        HocrChar charA;
        charA.codepoint = U'A';
        charA.bbox = {10, 10, 25, 30};
        charA.confidence = 95;

        HocrChar charB;
        charB.codepoint = U'B';
        charB.bbox = {30, 10, 45, 30};
        charB.confidence = 90;

        HocrChar charC;
        charC.codepoint = U'C';
        charC.bbox = {50, 10, 65, 30};
        charC.confidence = 85;

        HocrWord word;
        word.text = "ABC";
        word.bbox = {10, 10, 65, 30};
        word.confidence = 90;
        word.chars = {charA, charB, charC};

        HocrLine line;
        line.bbox = {10, 10, 65, 30};
        line.words = {word};

        return {line};
    }
};

TEST_F(GlyphAtlasTest, BuildFromCharData) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();
    RgbColor textColor{0, 0, 0};
    RgbColor bgColor{255, 255, 255};

    GlyphAtlas atlas;
    atlas.build(img, lines, textColor, bgColor);

    EXPECT_EQ(atlas.size(), 3);
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_TRUE(atlas.contains(U'C'));
}

TEST_F(GlyphAtlasTest, LookupExistingCharacter) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    QImage glyphA = atlas.glyph(U'A');
    EXPECT_FALSE(glyphA.isNull());
    EXPECT_EQ(glyphA.width(), 15);   // 25 - 10
    EXPECT_EQ(glyphA.height(), 20);  // 30 - 10
}

TEST_F(GlyphAtlasTest, LookupMissingCharacter) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    QImage glyphZ = atlas.glyph(U'Z');
    EXPECT_TRUE(glyphZ.isNull());
    EXPECT_FALSE(atlas.contains(U'Z'));
}

TEST_F(GlyphAtlasTest, MultiInstanceHigherConfidenceWins) {
    QImage img = makeTestImage(100, 50);

    // Two instances of 'A' with different confidence
    HocrChar charA1;
    charA1.codepoint = U'A';
    charA1.bbox = {10, 10, 25, 30};
    charA1.confidence = 70;

    HocrChar charA2;
    charA2.codepoint = U'A';
    charA2.bbox = {30, 10, 45, 30};  // different region
    charA2.confidence = 95;

    HocrWord word1;
    word1.text = "A";
    word1.bbox = {10, 10, 25, 30};
    word1.chars = {charA1};

    HocrWord word2;
    word2.text = "A";
    word2.bbox = {30, 10, 45, 30};
    word2.chars = {charA2};

    HocrLine line;
    line.bbox = {10, 10, 45, 30};
    line.words = {word1, word2};

    GlyphAtlas atlas;
    atlas.build(img, {line}, {0, 0, 0}, {255, 255, 255});

    EXPECT_EQ(atlas.size(), 1);  // Only one 'A'
    QImage glyphA = atlas.glyph(U'A');
    EXPECT_FALSE(glyphA.isNull());
    // Higher confidence version has bbox {30,10,45,30} → width=15
    EXPECT_EQ(glyphA.width(), 15);
}

TEST_F(GlyphAtlasTest, CoverageFullMatch) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    // All chars available
    EXPECT_FLOAT_EQ(atlas.coverage(U"ABC"), 1.0f);
}

TEST_F(GlyphAtlasTest, CoveragePartialMatch) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    // 3 of 4 unique chars available (A, B, C but not D)
    float cov = atlas.coverage(U"ABCD");
    EXPECT_NEAR(cov, 0.75f, 0.01f);
}

TEST_F(GlyphAtlasTest, CoverageOnlyWhitespace) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    EXPECT_FLOAT_EQ(atlas.coverage(U"   "), 1.0f);
}

TEST_F(GlyphAtlasTest, AlphaIsolation) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    QImage glyphA = atlas.glyph(U'A');
    ASSERT_FALSE(glyphA.isNull());

    // Text pixels (black on white) should have high alpha
    // The black region in our test image is at relative coords (0,0)-(15,20)
    QRgb textPixel = glyphA.pixel(5, 5);
    EXPECT_GT(qAlpha(textPixel), 200);  // Text should be mostly opaque
}

TEST_F(GlyphAtlasTest, EmptyPageImage) {
    QImage nullImg;
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(nullImg, lines, {0, 0, 0}, {255, 255, 255});

    EXPECT_EQ(atlas.size(), 0);
}

TEST_F(GlyphAtlasTest, EmptyLines) {
    QImage img = makeTestImage(100, 50);
    std::vector<HocrLine> emptyLines;

    GlyphAtlas atlas;
    atlas.build(img, emptyLines, {0, 0, 0}, {255, 255, 255});

    EXPECT_EQ(atlas.size(), 0);
}

TEST_F(GlyphAtlasTest, EmptyCoverageText) {
    QImage img = makeTestImage(100, 50);
    auto lines = makeTestLines();

    GlyphAtlas atlas;
    atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});

    EXPECT_FLOAT_EQ(atlas.coverage(U""), 1.0f);
}

TEST_F(GlyphAtlasTest, OutOfBoundsBboxClamped) {
    // Character bbox extends beyond image bounds -- should be clamped, not crash.
    // The clamped region (90,40)-(100,50) is 10x10 pixels of pure background.
    // After isolateAlpha + cropToContent, the all-transparent glyph is correctly
    // rejected (no invisible glyph). This is the expected behavior -- we don't
    // want glyphs that contain only background pixels.
    QImage img = makeTestImage(100, 50);

    HocrChar ch;
    ch.codepoint = U'X';
    ch.bbox = {90, 40, 120, 70};  // extends past 100x50 image
    ch.confidence = 80;

    HocrWord word;
    word.text = "X";
    word.bbox = ch.bbox;
    word.chars = {ch};

    HocrLine line;
    line.bbox = ch.bbox;
    line.words = {word};

    GlyphAtlas atlas;
    // Should not crash despite out-of-bounds bbox
    atlas.build(img, {line}, {0, 0, 0}, {255, 255, 255});

    // Background-only crop is correctly rejected after cropToContent
    EXPECT_FALSE(atlas.contains(U'X'))
        << "Background-only glyph (from OOB bbox) should be rejected";
}

TEST_F(GlyphAtlasTest, ZeroSizeBboxSkipped) {
    QImage img = makeTestImage(100, 50);

    HocrChar ch;
    ch.codepoint = U'Y';
    ch.bbox = {10, 10, 10, 10};  // zero width and height
    ch.confidence = 80;

    HocrWord word;
    word.text = "Y";
    word.bbox = ch.bbox;
    word.chars = {ch};

    HocrLine line;
    line.bbox = ch.bbox;
    line.words = {word};

    GlyphAtlas atlas;
    atlas.build(img, {line}, {0, 0, 0}, {255, 255, 255});

    EXPECT_FALSE(atlas.contains(U'Y'));
    EXPECT_EQ(atlas.size(), 0);
}

TEST_F(GlyphAtlasTest, IdenticalConfidenceKeepsFirst) {
    QImage img = makeTestImage(100, 50);

    HocrChar charA1;
    charA1.codepoint = U'A';
    charA1.bbox = {10, 10, 25, 30};  // width=15
    charA1.confidence = 90;

    HocrChar charA2;
    charA2.codepoint = U'A';
    charA2.bbox = {30, 10, 50, 30};  // width=20, different region
    charA2.confidence = 90;  // same confidence

    HocrWord word;
    word.text = "AA";
    word.bbox = {10, 10, 50, 30};
    word.chars = {charA1, charA2};

    HocrLine line;
    line.bbox = word.bbox;
    line.words = {word};

    GlyphAtlas atlas;
    atlas.build(img, {line}, {0, 0, 0}, {255, 255, 255});

    EXPECT_EQ(atlas.size(), 1);
    QImage glyph = atlas.glyph(U'A');
    EXPECT_FALSE(glyph.isNull());
    // First instance kept (confidence >= means second is rejected)
    EXPECT_EQ(glyph.width(), 15);
}

TEST_F(GlyphAtlasTest, CodepointZeroSkipped) {
    QImage img = makeTestImage(100, 50);

    HocrChar ch;
    ch.codepoint = 0;  // null codepoint
    ch.bbox = {10, 10, 25, 30};
    ch.confidence = 90;

    HocrWord word;
    word.text = "";
    word.bbox = ch.bbox;
    word.chars = {ch};

    HocrLine line;
    line.bbox = ch.bbox;
    line.words = {word};

    GlyphAtlas atlas;
    atlas.build(img, {line}, {0, 0, 0}, {255, 255, 255});

    EXPECT_EQ(atlas.size(), 0);
}
