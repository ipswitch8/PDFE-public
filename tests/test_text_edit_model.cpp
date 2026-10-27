#include <gtest/gtest.h>
#include "textedit/TextEditModel.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"
#include <vector>

using namespace pdfe::textedit;
using namespace pdfe::core;

// Helper function to build a TextChar with realistic PDF coordinates
static TextChar makeChar(char32_t cp, float x, float y, const std::string& font = "Helvetica", float size = 12.0f) {
    TextChar ch;
    ch.codepoint = cp;
    ch.origin = QPointF(x, y);
    ch.quad = QRectF(x, y, 7.0, 12.0);  // typical char width ~7pt, height ~12pt
    ch.fontName = font;
    ch.fontSize = size;
    return ch;
}

// Helper function to calculate line bounding box from chars
static QRectF calcLineBBox(const std::vector<TextChar>& chars) {
    if (chars.empty()) return QRectF();
    QRectF bbox = chars[0].quad;
    for (size_t i = 1; i < chars.size(); ++i) {
        bbox = bbox.united(chars[i].quad);
    }
    return bbox;
}

// Helper function to calculate block bounding box from lines
static QRectF calcBlockBBox(const std::vector<TextLine>& lines) {
    if (lines.empty()) return QRectF();
    QRectF bbox = lines[0].boundingBox;
    for (size_t i = 1; i < lines.size(); ++i) {
        bbox = bbox.united(lines[i].boundingBox);
    }
    return bbox;
}

class TextEditModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup if needed
    }

    void TearDown() override {
        // Common cleanup if needed
    }
};

// Test 1: ConstructFromEmptyBlocks - empty vector produces 0 spans
TEST_F(TextEditModelTest, ConstructFromEmptyBlocks) {
    // Arrange
    std::vector<TextBlock> blocks;

    // Act
    TextEditModel model(blocks);

    // Assert
    EXPECT_EQ(0, model.spanCount());
}

// Test 2: ConstructFromSingleBlock - single block with one line, all same font -> 1 span
TEST_F(TextEditModelTest, ConstructFromSingleBlock) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    // Create "Hello" with all chars in Helvetica
    line.chars.push_back(makeChar(U'H', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'o', 38.0f, 100.0f, "Helvetica", 12.0f));

    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    // Act
    TextEditModel model(blocks);

    // Assert
    EXPECT_EQ(1, model.spanCount());
    const TextSpan& span = model.span(0);
    EXPECT_EQ(0, span.blockIndex);
    EXPECT_EQ(0, span.lineIndex);
    EXPECT_EQ(0, span.charStartIndex);
    EXPECT_EQ(5, span.charEndIndex);
    EXPECT_EQ(U"Hello", span.text);
    EXPECT_EQ(U"Hello", span.originalText);
    EXPECT_EQ("Helvetica", span.fontName);
    EXPECT_FLOAT_EQ(12.0f, span.fontSize);
    EXPECT_FALSE(span.isModified());
}

// Test 3: ConstructFromMultipleFonts - one line with two fonts -> 2 spans
TEST_F(TextEditModelTest, ConstructFromMultipleFonts) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    // "Hello" in Helvetica
    line.chars.push_back(makeChar(U'H', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'o', 38.0f, 100.0f, "Helvetica", 12.0f));

    // "World" in TimesRoman
    line.chars.push_back(makeChar(U'W', 45.0f, 100.0f, "TimesRoman", 12.0f));
    line.chars.push_back(makeChar(U'o', 52.0f, 100.0f, "TimesRoman", 12.0f));
    line.chars.push_back(makeChar(U'r', 59.0f, 100.0f, "TimesRoman", 12.0f));
    line.chars.push_back(makeChar(U'l', 66.0f, 100.0f, "TimesRoman", 12.0f));
    line.chars.push_back(makeChar(U'd', 73.0f, 100.0f, "TimesRoman", 12.0f));

    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    // Act
    TextEditModel model(blocks);

    // Assert
    EXPECT_EQ(2, model.spanCount());

    const TextSpan& span1 = model.span(0);
    EXPECT_EQ(U"Hello", span1.text);
    EXPECT_EQ("Helvetica", span1.fontName);
    EXPECT_EQ(0, span1.charStartIndex);
    EXPECT_EQ(5, span1.charEndIndex);

    const TextSpan& span2 = model.span(1);
    EXPECT_EQ(U"World", span2.text);
    EXPECT_EQ("TimesRoman", span2.fontName);
    EXPECT_EQ(5, span2.charStartIndex);
    EXPECT_EQ(10, span2.charEndIndex);
}

// Test 4: ConstructFromMultipleBlocks - 2 blocks, each with 1 line -> 2 spans
TEST_F(TextEditModelTest, ConstructFromMultipleBlocks) {
    // Arrange
    std::vector<TextBlock> blocks(2);

    // Block 0: "First"
    TextLine line1;
    line1.chars.push_back(makeChar(U'F', 10.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'i', 17.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'r', 24.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U's', 31.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U't', 38.0f, 100.0f, "Helvetica", 12.0f));
    line1.boundingBox = calcLineBBox(line1.chars);
    line1.writingMode = 0;
    blocks[0].lines.push_back(line1);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    // Block 1: "Second"
    TextLine line2;
    line2.chars.push_back(makeChar(U'S', 10.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'e', 17.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'c', 24.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'o', 31.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'n', 38.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'd', 45.0f, 150.0f, "Helvetica", 12.0f));
    line2.boundingBox = calcLineBBox(line2.chars);
    line2.writingMode = 0;
    blocks[1].lines.push_back(line2);
    blocks[1].boundingBox = calcBlockBBox(blocks[1].lines);
    blocks[1].type = TextBlockType::Paragraph;

    // Act
    TextEditModel model(blocks);

    // Assert
    EXPECT_EQ(2, model.spanCount());

    const TextSpan& span1 = model.span(0);
    EXPECT_EQ(0, span1.blockIndex);
    EXPECT_EQ(0, span1.lineIndex);
    EXPECT_EQ(U"First", span1.text);

    const TextSpan& span2 = model.span(1);
    EXPECT_EQ(1, span2.blockIndex);
    EXPECT_EQ(0, span2.lineIndex);
    EXPECT_EQ(U"Second", span2.text);
}

// Test 5: SpanAtPointInside - point inside span bbox returns correct index
TEST_F(TextEditModelTest, SpanAtPointInside) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    // "Test" at y=100
    line.chars.push_back(makeChar(U'T', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U's', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U't', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point inside the span bounding box (middle of "Test")
    int spanIndex = model.spanAtPoint(QPointF(20.0, 105.0));
    EXPECT_EQ(0, spanIndex);
}

// Test 6: SpanAtPointOutside - point far outside any span returns -1
TEST_F(TextEditModelTest, SpanAtPointOutside) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    line.chars.push_back(makeChar(U'T', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U's', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U't', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point far away from any span
    int spanIndex = model.spanAtPoint(QPointF(500.0, 500.0));
    EXPECT_EQ(-1, spanIndex);
}

// Test 7: SpanAtPointBetweenLines - point between two spans returns -1
TEST_F(TextEditModelTest, SpanAtPointBetweenLines) {
    // Arrange
    std::vector<TextBlock> blocks(1);

    // First line at y=100
    TextLine line1;
    line1.chars.push_back(makeChar(U'F', 10.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'i', 17.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'r', 24.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U's', 31.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U't', 38.0f, 100.0f, "Helvetica", 12.0f));
    line1.boundingBox = calcLineBBox(line1.chars);
    line1.writingMode = 0;

    // Second line at y=150 (50pt gap)
    TextLine line2;
    line2.chars.push_back(makeChar(U'S', 10.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'e', 17.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'c', 24.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'o', 31.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'n', 38.0f, 150.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'd', 45.0f, 150.0f, "Helvetica", 12.0f));
    line2.boundingBox = calcLineBBox(line2.chars);
    line2.writingMode = 0;

    blocks[0].lines.push_back(line1);
    blocks[0].lines.push_back(line2);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point between the two lines (y=125, between 112 and 150)
    int spanIndex = model.spanAtPoint(QPointF(20.0, 125.0));
    EXPECT_EQ(-1, spanIndex);
}

// Test 8: CharIndexAtPointLeftEdge - point at left of first char returns 0
TEST_F(TextEditModelTest, CharIndexAtPointLeftEdge) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    line.chars.push_back(makeChar(U'A', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'B', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'C', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point at the very left edge of first char
    int charIndex = model.charIndexAtPoint(0, QPointF(10.0, 105.0));
    EXPECT_EQ(0, charIndex);
}

// Test 9: CharIndexAtPointMiddle - point at middle of span returns correct index
TEST_F(TextEditModelTest, CharIndexAtPointMiddle) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    // "ABCDE" - each char 7pt wide starting at x=10
    line.chars.push_back(makeChar(U'A', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'B', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'C', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'D', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'E', 38.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point in middle of 'C' (third char, index 2) - x=27 is middle of char at x=24
    int charIndex = model.charIndexAtPoint(0, QPointF(27.0, 105.0));
    EXPECT_EQ(2, charIndex);
}

// Test 10: CharIndexAtPointRightEdge - point past last char returns text length
TEST_F(TextEditModelTest, CharIndexAtPointRightEdge) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    line.chars.push_back(makeChar(U'A', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'B', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'C', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act & Assert
    // Point past the last char (C ends at x=31, point at x=50)
    int charIndex = model.charIndexAtPoint(0, QPointF(50.0, 105.0));
    EXPECT_EQ(3, charIndex);  // Length of "ABC"
}

// Test 11: ReplaceSpanText - replaces text, marks modified
TEST_F(TextEditModelTest, ReplaceSpanText) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    line.chars.push_back(makeChar(U'H', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'o', 38.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act
    model.replaceSpanText(0, U"Goodbye");

    // Assert
    const TextSpan& span = model.span(0);
    EXPECT_EQ(U"Goodbye", span.text);
    EXPECT_EQ(U"Hello", span.originalText);
    EXPECT_TRUE(span.isModified());
    EXPECT_TRUE(model.hasModifications());
}

// Test 12: ReplaceCharRange - insert/delete/replace char range operations
TEST_F(TextEditModelTest, ReplaceCharRange) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    // "Hello"
    line.chars.push_back(makeChar(U'H', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'l', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'o', 38.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act - Replace "ell" (indices 1-4) with "i" -> "Hio"
    model.replaceCharRange(0, 1, 4, U"i");

    // Assert
    const TextSpan& span = model.span(0);
    EXPECT_EQ(U"Hio", span.text);
    EXPECT_EQ(U"Hello", span.originalText);
    EXPECT_TRUE(span.isModified());
}

// Test 13: HasModifications - false initially, true after edit
TEST_F(TextEditModelTest, HasModifications) {
    // Arrange
    std::vector<TextBlock> blocks(1);
    TextLine line;

    line.chars.push_back(makeChar(U'T', 10.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U'e', 17.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U's', 24.0f, 100.0f, "Helvetica", 12.0f));
    line.chars.push_back(makeChar(U't', 31.0f, 100.0f, "Helvetica", 12.0f));
    line.boundingBox = calcLineBBox(line.chars);
    line.writingMode = 0;

    blocks[0].lines.push_back(line);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Assert - Initially no modifications
    EXPECT_FALSE(model.hasModifications());

    // Act - Make a modification
    model.replaceSpanText(0, U"Modified");

    // Assert - Now has modifications
    EXPECT_TRUE(model.hasModifications());
}

// Test 14: ModifiedSpanIndices - returns only modified span indices
TEST_F(TextEditModelTest, ModifiedSpanIndices) {
    // Arrange
    std::vector<TextBlock> blocks(1);

    // Line 1: "First"
    TextLine line1;
    line1.chars.push_back(makeChar(U'F', 10.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'i', 17.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U'r', 24.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U's', 31.0f, 100.0f, "Helvetica", 12.0f));
    line1.chars.push_back(makeChar(U't', 38.0f, 100.0f, "Helvetica", 12.0f));
    line1.boundingBox = calcLineBBox(line1.chars);
    line1.writingMode = 0;

    // Line 2: "Second"
    TextLine line2;
    line2.chars.push_back(makeChar(U'S', 10.0f, 120.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'e', 17.0f, 120.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'c', 24.0f, 120.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'o', 31.0f, 120.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'n', 38.0f, 120.0f, "Helvetica", 12.0f));
    line2.chars.push_back(makeChar(U'd', 45.0f, 120.0f, "Helvetica", 12.0f));
    line2.boundingBox = calcLineBBox(line2.chars);
    line2.writingMode = 0;

    // Line 3: "Third"
    TextLine line3;
    line3.chars.push_back(makeChar(U'T', 10.0f, 140.0f, "Helvetica", 12.0f));
    line3.chars.push_back(makeChar(U'h', 17.0f, 140.0f, "Helvetica", 12.0f));
    line3.chars.push_back(makeChar(U'i', 24.0f, 140.0f, "Helvetica", 12.0f));
    line3.chars.push_back(makeChar(U'r', 31.0f, 140.0f, "Helvetica", 12.0f));
    line3.chars.push_back(makeChar(U'd', 38.0f, 140.0f, "Helvetica", 12.0f));
    line3.boundingBox = calcLineBBox(line3.chars);
    line3.writingMode = 0;

    blocks[0].lines.push_back(line1);
    blocks[0].lines.push_back(line2);
    blocks[0].lines.push_back(line3);
    blocks[0].boundingBox = calcBlockBBox(blocks[0].lines);
    blocks[0].type = TextBlockType::Paragraph;

    TextEditModel model(blocks);

    // Act - Modify only spans 0 and 2
    model.replaceSpanText(0, U"FIRST");
    model.replaceSpanText(2, U"THIRD");

    // Assert
    std::vector<int> modifiedIndices = model.modifiedSpanIndices();
    EXPECT_EQ(2, modifiedIndices.size());
    EXPECT_EQ(0, modifiedIndices[0]);
    EXPECT_EQ(2, modifiedIndices[1]);

    // Verify span 1 is not modified
    EXPECT_FALSE(model.span(1).isModified());
}
