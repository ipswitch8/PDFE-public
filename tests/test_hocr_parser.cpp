#include <gtest/gtest.h>
#include "tre/HocrParser.h"
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <cmath>

using namespace pdfe::tre;

namespace {

/// Helper function to load a fixture file from the fixtures directory
static std::string loadFixture(const std::string& filename) {
    std::filesystem::path fixturePath = std::filesystem::path(TEST_FIXTURES_DIR) / filename;
    std::ifstream f(fixturePath);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open fixture file: " + fixturePath.string());
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Helper to compare floating point values with tolerance
static bool floatNear(float a, float b, float tolerance = 0.01f) {
    return std::abs(a - b) < tolerance;
}

} // anonymous namespace


// ---------------------------------------------------------------------------
// Test Fixture for HocrParser Tests
// ---------------------------------------------------------------------------

class HocrParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Load the sample HOCR fixture
        sampleHocr = loadFixture("sample_hocr.xml");
    }

    std::string sampleHocr;
};


// ---------------------------------------------------------------------------
// Test 1: Parse valid HOCR - Verify page dimensions and scan resolution
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_PageDimensions) {
    auto doc = HocrParser::parse(sampleHocr);

    EXPECT_EQ(doc.pageWidth, 2550);
    EXPECT_EQ(doc.pageHeight, 3300);
    EXPECT_EQ(doc.scanResX, 300);
    EXPECT_EQ(doc.scanResY, 300);
}


// ---------------------------------------------------------------------------
// Test 2: Block count - Verify correct number of blocks parsed
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_BlockCount) {
    auto doc = HocrParser::parse(sampleHocr);

    ASSERT_EQ(doc.blocks.size(), 2);
}


// ---------------------------------------------------------------------------
// Test 3: Paragraph/line structure - Block 1 has 1 paragraph with 2 lines
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_ParagraphLineStructure) {
    auto doc = HocrParser::parse(sampleHocr);

    ASSERT_EQ(doc.blocks.size(), 2);

    const auto& block1 = doc.blocks[0];
    ASSERT_EQ(block1.paragraphs.size(), 1);

    const auto& par1 = block1.paragraphs[0];
    ASSERT_EQ(par1.lines.size(), 2);
}


// ---------------------------------------------------------------------------
// Test 4: Word extraction - First line has words "The", "quick", "brown", "fox", "jumps"
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_WordExtraction) {
    auto doc = HocrParser::parse(sampleHocr);

    ASSERT_EQ(doc.blocks.size(), 2);
    const auto& block1 = doc.blocks[0];
    ASSERT_EQ(block1.paragraphs.size(), 1);
    const auto& par1 = block1.paragraphs[0];
    ASSERT_EQ(par1.lines.size(), 2);

    const auto& line1 = par1.lines[0];
    ASSERT_EQ(line1.words.size(), 5);

    EXPECT_EQ(line1.words[0].text, "The");
    EXPECT_EQ(line1.words[1].text, "quick");
    EXPECT_EQ(line1.words[2].text, "brown");
    EXPECT_EQ(line1.words[3].text, "fox");
    EXPECT_EQ(line1.words[4].text, "jumps");
}


// ---------------------------------------------------------------------------
// Test 5: Word bbox parsing - word "The" has bbox {150, 208, 280, 236}
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_WordBbox) {
    auto doc = HocrParser::parse(sampleHocr);

    const auto& line1 = doc.blocks[0].paragraphs[0].lines[0];
    const auto& wordThe = line1.words[0];

    EXPECT_EQ(wordThe.text, "The");
    EXPECT_DOUBLE_EQ(wordThe.bbox.x0, 150.0);
    EXPECT_DOUBLE_EQ(wordThe.bbox.y0, 208.0);
    EXPECT_DOUBLE_EQ(wordThe.bbox.x1, 280.0);
    EXPECT_DOUBLE_EQ(wordThe.bbox.y1, 236.0);
}


// ---------------------------------------------------------------------------
// Test 6: Confidence parsing - word "The" has confidence 95, word "t3xt" has confidence 45
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_WordConfidence) {
    auto doc = HocrParser::parse(sampleHocr);

    // Word "The" from line 1
    const auto& wordThe = doc.blocks[0].paragraphs[0].lines[0].words[0];
    EXPECT_EQ(wordThe.text, "The");
    EXPECT_EQ(wordThe.confidence, 95);

    // Word "t3xt" from second block, first paragraph, first line, third word
    ASSERT_EQ(doc.blocks.size(), 2);
    const auto& block2 = doc.blocks[1];
    ASSERT_EQ(block2.paragraphs.size(), 1);
    const auto& par2 = block2.paragraphs[0];
    ASSERT_EQ(par2.lines.size(), 1);
    const auto& line3 = par2.lines[0];
    ASSERT_EQ(line3.words.size(), 3);

    const auto& wordT3xt = line3.words[2];
    EXPECT_EQ(wordT3xt.text, "t3xt");
    EXPECT_EQ(wordT3xt.confidence, 45);
}


// ---------------------------------------------------------------------------
// Test 7: Font size parsing - Words have fontSizePx=14.0 or 13.0
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_FontSize) {
    auto doc = HocrParser::parse(sampleHocr);

    // First line words should have font size 14.0
    const auto& line1 = doc.blocks[0].paragraphs[0].lines[0];
    for (const auto& word : line1.words) {
        EXPECT_FLOAT_EQ(word.fontSizePx, 14.0f);
    }

    // Second block words should have font size 13.0
    const auto& line3 = doc.blocks[1].paragraphs[0].lines[0];
    for (const auto& word : line3.words) {
        EXPECT_FLOAT_EQ(word.fontSizePx, 13.0f);
    }
}


// ---------------------------------------------------------------------------
// Test 8: Baseline parsing - Line 1 has baselineSlope≈0.002, baselineOffset≈-7.0
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_BaselineParsing) {
    auto doc = HocrParser::parse(sampleHocr);

    const auto& line1 = doc.blocks[0].paragraphs[0].lines[0];

    EXPECT_TRUE(floatNear(line1.baselineSlope, 0.002f));
    EXPECT_TRUE(floatNear(line1.baselineOffset, -7.0f));
}


// ---------------------------------------------------------------------------
// Test 9: Line metrics - xSizePx, descendersPx, ascendersPx parsed
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_LineMetrics) {
    auto doc = HocrParser::parse(sampleHocr);

    const auto& line1 = doc.blocks[0].paragraphs[0].lines[0];

    EXPECT_TRUE(floatNear(line1.xSizePx, 28.5f));
    EXPECT_TRUE(floatNear(line1.descendersPx, 5.5f));
    EXPECT_TRUE(floatNear(line1.ascendersPx, 7.0f));
}


// ---------------------------------------------------------------------------
// Test 10: Region extraction - Selection covering line 1 only → returns 1 line with 5 words
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_SingleLine) {
    auto doc = HocrParser::parse(sampleHocr);

    // Create a selection rectangle that covers only line 1
    // Line 1 bbox is: 150 200 2400 240
    PixelRect selection{150, 200, 2400, 240};

    auto region = HocrParser::extractRegion(doc, selection);

    ASSERT_EQ(region.lines.size(), 1);
    ASSERT_EQ(region.lines[0].words.size(), 5);

    // Verify the words
    EXPECT_EQ(region.lines[0].words[0].text, "The");
    EXPECT_EQ(region.lines[0].words[1].text, "quick");
    EXPECT_EQ(region.lines[0].words[2].text, "brown");
    EXPECT_EQ(region.lines[0].words[3].text, "fox");
    EXPECT_EQ(region.lines[0].words[4].text, "jumps");
}


// ---------------------------------------------------------------------------
// Test 11: Region extraction snap - Selection partially overlapping a line still includes full line
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_SnapToLine) {
    auto doc = HocrParser::parse(sampleHocr);

    // Create a selection rectangle that only partially overlaps line 1
    // Line 1 bbox is: 150 200 2400 240
    // Use a selection that only covers the top half
    PixelRect selection{150, 200, 2400, 220};

    auto region = HocrParser::extractRegion(doc, selection);

    // Despite partial overlap, the entire line should be included
    ASSERT_EQ(region.lines.size(), 1);
    ASSERT_EQ(region.lines[0].words.size(), 5);
}


// ---------------------------------------------------------------------------
// Test 12: Region plain text - Extracted text is "The quick brown fox jumps" for line 1
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_PlainText) {
    auto doc = HocrParser::parse(sampleHocr);

    // Create a selection rectangle that covers only line 1
    PixelRect selection{150, 200, 2400, 240};

    auto region = HocrParser::extractRegion(doc, selection);

    EXPECT_EQ(region.plainText, "The quick brown fox jumps");
}


// ---------------------------------------------------------------------------
// Test 13: Multiple lines extraction - Selection covering both lines of first paragraph
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_MultipleLines) {
    auto doc = HocrParser::parse(sampleHocr);

    // Create a selection rectangle that covers both lines of first paragraph
    // Line 1: bbox 150 200 2400 240
    // Line 2: bbox 150 260 2400 300
    PixelRect selection{150, 200, 2400, 300};

    auto region = HocrParser::extractRegion(doc, selection);

    ASSERT_EQ(region.lines.size(), 2);

    // First line should have 5 words
    ASSERT_EQ(region.lines[0].words.size(), 5);

    // Second line should have 4 words
    ASSERT_EQ(region.lines[1].words.size(), 4);
    EXPECT_EQ(region.lines[1].words[0].text, "over");
    EXPECT_EQ(region.lines[1].words[1].text, "the");
    EXPECT_EQ(region.lines[1].words[2].text, "lazy");
    EXPECT_EQ(region.lines[1].words[3].text, "dog.");

    // Plain text should have both lines separated by newline
    EXPECT_EQ(region.plainText, "The quick brown fox jumps\nover the lazy dog.");
}


// ---------------------------------------------------------------------------
// Test 14: Empty XML - parse("") returns empty HocrDocument (0 blocks, pageWidth=0)
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseEmptyXml) {
    auto doc = HocrParser::parse("");

    EXPECT_EQ(doc.blocks.size(), 0);
    EXPECT_EQ(doc.pageWidth, 0);
    EXPECT_EQ(doc.pageHeight, 0);
    EXPECT_EQ(doc.scanResX, 300);  // Default value
    EXPECT_EQ(doc.scanResY, 300);  // Default value
}


// ---------------------------------------------------------------------------
// Test 15: Malformed XML - parse("<not>valid") returns empty HocrDocument or best-effort parse
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseMalformedXml) {
    auto doc = HocrParser::parse("<not>valid");

    // Should return empty document without crashing
    EXPECT_EQ(doc.blocks.size(), 0);
    EXPECT_EQ(doc.pageWidth, 0);
    EXPECT_EQ(doc.pageHeight, 0);
}


// ---------------------------------------------------------------------------
// Test 16: UTF-8 preservation - Test with custom HOCR containing accented characters
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseUtf8Characters) {
    std::string utf8Hocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN"
    "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml">
<body>
  <div class='ocr_page' id='page_1' title='bbox 0 0 1000 1000; scan_res 300 300'>
    <div class='ocr_carea' id='block_1' title="bbox 100 100 900 200">
      <p class='ocr_par' id='par_1' lang='fra'>
        <span class='ocr_line' id='line_1' title="bbox 100 100 900 150">
          <span class='ocrx_word' id='word_1' title='bbox 100 110 200 140; x_wconf 95'>café</span>
          <span class='ocrx_word' id='word_2' title='bbox 220 110 320 140; x_wconf 92'>naïve</span>
          <span class='ocrx_word' id='word_3' title='bbox 340 110 440 140; x_wconf 98'>résumé</span>
        </span>
      </p>
    </div>
  </div>
</body>
</html>)";

    auto doc = HocrParser::parse(utf8Hocr);

    ASSERT_EQ(doc.blocks.size(), 1);
    ASSERT_EQ(doc.blocks[0].paragraphs.size(), 1);
    ASSERT_EQ(doc.blocks[0].paragraphs[0].lines.size(), 1);

    const auto& line = doc.blocks[0].paragraphs[0].lines[0];
    ASSERT_EQ(line.words.size(), 3);

    // Verify UTF-8 characters are preserved
    EXPECT_EQ(line.words[0].text, "café");
    EXPECT_EQ(line.words[1].text, "naïve");
    EXPECT_EQ(line.words[2].text, "résumé");

    // Verify language attribute
    EXPECT_EQ(doc.blocks[0].paragraphs[0].lang, "fra");
}


// ---------------------------------------------------------------------------
// Test 17: Block bounding boxes - Verify block bbox parsing
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseValidHocr_BlockBbox) {
    auto doc = HocrParser::parse(sampleHocr);

    ASSERT_EQ(doc.blocks.size(), 2);

    // First block: bbox 150 200 2400 600
    const auto& block1 = doc.blocks[0];
    EXPECT_DOUBLE_EQ(block1.bbox.x0, 150.0);
    EXPECT_DOUBLE_EQ(block1.bbox.y0, 200.0);
    EXPECT_DOUBLE_EQ(block1.bbox.x1, 2400.0);
    EXPECT_DOUBLE_EQ(block1.bbox.y1, 600.0);

    // Second block: bbox 150 650 2400 900
    const auto& block2 = doc.blocks[1];
    EXPECT_DOUBLE_EQ(block2.bbox.x0, 150.0);
    EXPECT_DOUBLE_EQ(block2.bbox.y0, 650.0);
    EXPECT_DOUBLE_EQ(block2.bbox.x1, 2400.0);
    EXPECT_DOUBLE_EQ(block2.bbox.y1, 900.0);
}


// ---------------------------------------------------------------------------
// Test 18: Extract region with no overlap - Returns empty result
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_NoOverlap) {
    auto doc = HocrParser::parse(sampleHocr);

    // Create a selection rectangle that doesn't overlap any text
    PixelRect selection{3000, 3000, 3500, 3500};

    auto region = HocrParser::extractRegion(doc, selection);

    EXPECT_EQ(region.lines.size(), 0);
    EXPECT_TRUE(region.plainText.empty());
}


// ---------------------------------------------------------------------------
// Test 19: Extract region pixel bbox - Verify union of included lines
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ExtractRegion_PixelBbox) {
    auto doc = HocrParser::parse(sampleHocr);

    // Select both lines from first paragraph
    // Line 1: bbox 150 200 2400 240
    // Line 2: bbox 150 260 2400 300
    PixelRect selection{150, 200, 2400, 300};

    auto region = HocrParser::extractRegion(doc, selection);

    // The pixel bbox should be the union of both line bboxes
    EXPECT_DOUBLE_EQ(region.pixelBbox.x0, 150.0);
    EXPECT_DOUBLE_EQ(region.pixelBbox.y0, 200.0);
    EXPECT_DOUBLE_EQ(region.pixelBbox.x1, 2400.0);
    EXPECT_DOUBLE_EQ(region.pixelBbox.y1, 300.0);
}


// ===========================================================================
// Character-level HOCR parsing tests (ocrx_cinfo)
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 20: Parse ocrx_cinfo elements - chars populated with correct codepoints
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseCharBoxes_CodepointsAndBboxes) {
    std::string charHocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<html><body>
  <div class='ocr_page' title='bbox 0 0 1000 500; scan_res 300 300'>
    <div class='ocr_carea' title="bbox 10 10 200 50">
      <p class='ocr_par'>
        <span class='ocr_line' title="bbox 10 10 200 50; baseline 0.0 0; x_size 20">
          <span class='ocrx_word' title='bbox 10 10 80 40; x_wconf 95'>
            <span class='ocrx_cinfo' title='bbox 10 10 25 40; x_conf 96'>H</span>
            <span class='ocrx_cinfo' title='bbox 26 10 40 40; x_conf 94'>i</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);

    ASSERT_EQ(doc.blocks.size(), 1);
    const auto& line = doc.blocks[0].paragraphs[0].lines[0];
    ASSERT_EQ(line.words.size(), 1);

    const auto& word = line.words[0];
    ASSERT_EQ(word.chars.size(), 2);

    EXPECT_EQ(word.chars[0].codepoint, U'H');
    EXPECT_DOUBLE_EQ(word.chars[0].bbox.x0, 10.0);
    EXPECT_DOUBLE_EQ(word.chars[0].bbox.y0, 10.0);
    EXPECT_DOUBLE_EQ(word.chars[0].bbox.x1, 25.0);
    EXPECT_DOUBLE_EQ(word.chars[0].bbox.y1, 40.0);

    EXPECT_EQ(word.chars[1].codepoint, U'i');
    EXPECT_DOUBLE_EQ(word.chars[1].bbox.x0, 26.0);
    EXPECT_DOUBLE_EQ(word.chars[1].bbox.x1, 40.0);
}


// ---------------------------------------------------------------------------
// Test 21: No ocrx_cinfo elements - chars vector empty (backward compatible)
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseNoCharBoxes_CharsEmpty) {
    // Use the standard sample HOCR (no ocrx_cinfo elements)
    auto doc = HocrParser::parse(sampleHocr);

    ASSERT_FALSE(doc.blocks.empty());
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];
    EXPECT_EQ(word.text, "The");
    EXPECT_TRUE(word.chars.empty());
}


// ---------------------------------------------------------------------------
// Test 22: Character confidence parsed from x_conf
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseCharBoxes_ConfidenceParsed) {
    std::string charHocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<html><body>
  <div class='ocr_page' title='bbox 0 0 500 200; scan_res 300 300'>
    <div class='ocr_carea' title="bbox 10 10 100 50">
      <p class='ocr_par'>
        <span class='ocr_line' title="bbox 10 10 100 50">
          <span class='ocrx_word' title='bbox 10 10 60 40; x_wconf 90'>
            <span class='ocrx_cinfo' title='bbox 10 10 30 40; x_conf 98'>A</span>
            <span class='ocrx_cinfo' title='bbox 31 10 55 40; x_conf 72'>B</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 2);
    EXPECT_EQ(word.chars[0].confidence, 98);
    EXPECT_EQ(word.chars[1].confidence, 72);
}


// ---------------------------------------------------------------------------
// Test 23: Empty char text handled gracefully
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseCharBoxes_EmptyCharTextSkipped) {
    std::string charHocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<html><body>
  <div class='ocr_page' title='bbox 0 0 500 200; scan_res 300 300'>
    <div class='ocr_carea' title="bbox 10 10 100 50">
      <p class='ocr_par'>
        <span class='ocr_line' title="bbox 10 10 100 50">
          <span class='ocrx_word' title='bbox 10 10 60 40; x_wconf 90'>
            <span class='ocrx_cinfo' title='bbox 10 10 30 40; x_conf 90'>X</span>
            <span class='ocrx_cinfo' title='bbox 31 10 50 40; x_conf 80'></span>
            <span class='ocrx_cinfo' title='bbox 51 10 60 40; x_conf 85'>Y</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    // Empty char text should be skipped (codepoint == 0)
    EXPECT_EQ(word.chars.size(), 2);
    EXPECT_EQ(word.chars[0].codepoint, U'X');
    EXPECT_EQ(word.chars[1].codepoint, U'Y');
}


// ---------------------------------------------------------------------------
// Test 24: OcrConfig enableCharBoxes defaults to false
// ---------------------------------------------------------------------------

TEST(OcrConfigTest, EnableCharBoxesDefaultsFalse) {
    OcrConfig config;
    EXPECT_FALSE(config.enableCharBoxes);
}


// ---------------------------------------------------------------------------
// Test 25: Word text reconstructed from chars when child_value is empty
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseCharBoxes_WordTextReconstructedFromChars) {
    // When hocr_char_boxes=1, Tesseract may put text only in ocrx_cinfo spans
    // and leave the word span's direct text empty
    std::string charHocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<html><body>
  <div class='ocr_page' title='bbox 0 0 500 200; scan_res 300 300'>
    <div class='ocr_carea' title="bbox 10 10 100 50">
      <p class='ocr_par'>
        <span class='ocr_line' title="bbox 10 10 100 50">
          <span class='ocrx_word' title='bbox 10 10 80 40; x_wconf 95'>
            <span class='ocrx_cinfo' title='bbox 10 10 25 40; x_conf 96'>C</span>
            <span class='ocrx_cinfo' title='bbox 26 10 40 40; x_conf 94'>a</span>
            <span class='ocrx_cinfo' title='bbox 41 10 55 40; x_conf 97'>t</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 3);
    // Word text should be available (either from direct text or reconstructed from chars)
    EXPECT_FALSE(word.text.empty());
    // The concatenated chars should spell "Cat"
    EXPECT_EQ(word.chars[0].codepoint, U'C');
    EXPECT_EQ(word.chars[1].codepoint, U'a');
    EXPECT_EQ(word.chars[2].codepoint, U't');
}


// ---------------------------------------------------------------------------
// Test 26: UTF-8 chars in ocrx_cinfo elements (accented characters)
// ---------------------------------------------------------------------------

TEST_F(HocrParserTest, ParseCharBoxes_Utf8Characters) {
    std::string charHocr = R"(<?xml version="1.0" encoding="UTF-8"?>
<html><body>
  <div class='ocr_page' title='bbox 0 0 500 200; scan_res 300 300'>
    <div class='ocr_carea' title="bbox 10 10 100 50">
      <p class='ocr_par'>
        <span class='ocr_line' title="bbox 10 10 100 50">
          <span class='ocrx_word' title='bbox 10 10 80 40; x_wconf 90'>
            <span class='ocrx_cinfo' title='bbox 10 10 25 40; x_conf 88'>c</span>
            <span class='ocrx_cinfo' title='bbox 26 10 40 40; x_conf 85'>a</span>
            <span class='ocrx_cinfo' title='bbox 41 10 55 40; x_conf 90'>f</span>
            <span class='ocrx_cinfo' title='bbox 56 10 75 40; x_conf 82'>é</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 4);
    EXPECT_EQ(word.chars[0].codepoint, U'c');
    EXPECT_EQ(word.chars[1].codepoint, U'a');
    EXPECT_EQ(word.chars[2].codepoint, U'f');
    EXPECT_EQ(word.chars[3].codepoint, U'é');  // U+00E9
}


// ---------------------------------------------------------------------------
// Tests for word-level x_bboxes distribution to individual chars
// ---------------------------------------------------------------------------

TEST(HocrParserXBboxes, WordLevelXBboxesDistributedToChars) {
    // Tesseract 4.x/5.x may put ALL char bboxes in the word's title as a
    // flat list: "x_bboxes x0 y0 x1 y1 x0 y0 x1 y1 ..." rather than in
    // individual ocrx_cinfo span titles.
    std::string charHocr = R"(<?xml version='1.0' encoding='UTF-8'?>
<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN'
    'http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd'>
<html xmlns='http://www.w3.org/1999/xhtml'>
<body>
  <div class='ocr_page' title='bbox 0 0 2550 3300; scan_res 300 300'>
    <div class='ocr_carea' title='bbox 0 0 2550 3300'>
      <p class='ocr_par'>
        <span class='ocr_line' title='bbox 316 1107 578 1140; baseline 0 0; x_size 28.5'>
          <span class='ocrx_word' title='bbox 316 1107 404 1140; x_wconf 96; x_bboxes 316 1107 340 1140 341 1115 360 1140 361 1115 380 1140 381 1115 404 1140'>
            <span class='ocrx_cinfo' title='x_conf 96'>D</span>
            <span class='ocrx_cinfo' title='x_conf 95'>e</span>
            <span class='ocrx_cinfo' title='x_conf 94'>a</span>
            <span class='ocrx_cinfo' title='x_conf 93'>r</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    ASSERT_EQ(doc.blocks.size(), 1);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 4);

    // Verify char bboxes were distributed from word-level x_bboxes
    EXPECT_EQ(word.chars[0].codepoint, U'D');
    EXPECT_NEAR(word.chars[0].bbox.x0, 316, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.y0, 1107, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.x1, 340, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.y1, 1140, 0.1);

    EXPECT_EQ(word.chars[1].codepoint, U'e');
    EXPECT_NEAR(word.chars[1].bbox.x0, 341, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.y0, 1115, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.x1, 360, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.y1, 1140, 0.1);

    EXPECT_EQ(word.chars[2].codepoint, U'a');
    EXPECT_NEAR(word.chars[2].bbox.x0, 361, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.y0, 1115, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.x1, 380, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.y1, 1140, 0.1);

    EXPECT_EQ(word.chars[3].codepoint, U'r');
    EXPECT_NEAR(word.chars[3].bbox.x0, 381, 0.1);
    EXPECT_NEAR(word.chars[3].bbox.y0, 1115, 0.1);
    EXPECT_NEAR(word.chars[3].bbox.x1, 404, 0.1);
    EXPECT_NEAR(word.chars[3].bbox.y1, 1140, 0.1);
}

TEST(HocrParserXBboxes, PerCharBboxPreferredOverWordLevel) {
    // When individual ocrx_cinfo spans have their own bboxes, those should
    // take priority over word-level x_bboxes.
    std::string charHocr = R"(<?xml version='1.0' encoding='UTF-8'?>
<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN'
    'http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd'>
<html xmlns='http://www.w3.org/1999/xhtml'>
<body>
  <div class='ocr_page' title='bbox 0 0 1000 1000'>
    <div class='ocr_carea' title='bbox 0 0 1000 1000'>
      <p class='ocr_par'>
        <span class='ocr_line' title='bbox 10 10 100 40'>
          <span class='ocrx_word' title='bbox 10 10 50 40; x_bboxes 10 10 30 40 31 10 50 40'>
            <span class='ocrx_cinfo' title='x_bboxes 12 12 28 38; x_conf 99'>H</span>
            <span class='ocrx_cinfo' title='x_bboxes 33 12 48 38; x_conf 98'>i</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 2);

    // Per-char x_bboxes should be used (12,12,28,38) not word-level (10,10,30,40)
    EXPECT_EQ(word.chars[0].codepoint, U'H');
    EXPECT_NEAR(word.chars[0].bbox.x0, 12, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.y0, 12, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.x1, 28, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.y1, 38, 0.1);

    EXPECT_EQ(word.chars[1].codepoint, U'i');
    EXPECT_NEAR(word.chars[1].bbox.x0, 33, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.y0, 12, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.x1, 48, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.y1, 38, 0.1);
}

TEST(HocrParserXBboxes, EmptyCharTitlesUseWordXBboxes) {
    // When ocrx_cinfo spans have no title at all (empty/missing), the
    // word-level x_bboxes fallback should fill in the bboxes.
    std::string charHocr = R"(<?xml version='1.0' encoding='UTF-8'?>
<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN'
    'http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd'>
<html xmlns='http://www.w3.org/1999/xhtml'>
<body>
  <div class='ocr_page' title='bbox 0 0 1000 1000'>
    <div class='ocr_carea' title='bbox 0 0 1000 1000'>
      <p class='ocr_par'>
        <span class='ocr_line' title='bbox 100 200 300 240'>
          <span class='ocrx_word' title='bbox 100 200 200 240; x_bboxes 100 200 130 240 131 200 160 240 161 200 200 240'>
            <span class='ocrx_cinfo'>C</span>
            <span class='ocrx_cinfo'>a</span>
            <span class='ocrx_cinfo'>t</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 3);

    // No per-char title → word-level x_bboxes fallback
    EXPECT_EQ(word.chars[0].codepoint, U'C');
    EXPECT_NEAR(word.chars[0].bbox.x0, 100, 0.1);
    EXPECT_NEAR(word.chars[0].bbox.x1, 130, 0.1);

    EXPECT_EQ(word.chars[1].codepoint, U'a');
    EXPECT_NEAR(word.chars[1].bbox.x0, 131, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.x1, 160, 0.1);

    EXPECT_EQ(word.chars[2].codepoint, U't');
    EXPECT_NEAR(word.chars[2].bbox.x0, 161, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.x1, 200, 0.1);
}

TEST(HocrParserXBboxes, MoreCharsThanBboxesSafe) {
    // When there are more chars than x_bboxes entries, extra chars should
    // still have zero bboxes (no crash or out-of-bounds access).
    std::string charHocr = R"(<?xml version='1.0' encoding='UTF-8'?>
<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN'
    'http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd'>
<html xmlns='http://www.w3.org/1999/xhtml'>
<body>
  <div class='ocr_page' title='bbox 0 0 1000 1000'>
    <div class='ocr_carea' title='bbox 0 0 1000 1000'>
      <p class='ocr_par'>
        <span class='ocr_line' title='bbox 10 10 100 40'>
          <span class='ocrx_word' title='bbox 10 10 60 40; x_bboxes 10 10 30 40 31 10 60 40'>
            <span class='ocrx_cinfo'>A</span>
            <span class='ocrx_cinfo'>B</span>
            <span class='ocrx_cinfo'>C</span>
          </span>
        </span>
      </p>
    </div>
  </div>
</body></html>)";

    auto doc = HocrParser::parse(charHocr);
    const auto& word = doc.blocks[0].paragraphs[0].lines[0].words[0];

    ASSERT_EQ(word.chars.size(), 3);

    // A and B get bboxes from x_bboxes
    EXPECT_NEAR(word.chars[0].bbox.x0, 10, 0.1);
    EXPECT_NEAR(word.chars[1].bbox.x0, 31, 0.1);

    // C has no x_bboxes entry — bbox stays at zero
    EXPECT_NEAR(word.chars[2].bbox.x0, 0, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.y0, 0, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.x1, 0, 0.1);
    EXPECT_NEAR(word.chars[2].bbox.y1, 0, 0.1);
}
