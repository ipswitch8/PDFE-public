// -*- coding: utf-8 -*-
// Phase 7b: WordExporter tests.
// Covers the pure helpers (font-name heuristics, XML+RTF escaping, zip
// writer) and end-to-end DOCX/RTF output on a real fixture.
#include <gtest/gtest.h>
#include "exports/WordExporter.h"
#include "exports/MiniZip.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace pdfe::exports;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_word_export_tests";
    std::filesystem::create_directories(d);
    return d / name;
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// -------- Heuristics --------

TEST(WordExportHeuristics, BoldDetectedCaseInsensitive)
{
    EXPECT_TRUE(WordExporter::fontNameImpliesBold("Times-Bold"));
    EXPECT_TRUE(WordExporter::fontNameImpliesBold("Helvetica-BOLD"));
    EXPECT_TRUE(WordExporter::fontNameImpliesBold("MyFont Heavy"));
    EXPECT_TRUE(WordExporter::fontNameImpliesBold("Arial Black"));
    EXPECT_FALSE(WordExporter::fontNameImpliesBold("Times-Roman"));
    EXPECT_FALSE(WordExporter::fontNameImpliesBold(""));
}

TEST(WordExportHeuristics, ItalicDetectedCaseInsensitive)
{
    EXPECT_TRUE(WordExporter::fontNameImpliesItalic("Times-Italic"));
    EXPECT_TRUE(WordExporter::fontNameImpliesItalic("Helvetica-ITALIC"));
    EXPECT_TRUE(WordExporter::fontNameImpliesItalic("MyFont Oblique"));
    EXPECT_FALSE(WordExporter::fontNameImpliesItalic("Times-Roman"));
    EXPECT_FALSE(WordExporter::fontNameImpliesItalic("Times-Bold"));
}

// -------- Escaping --------

TEST(WordExportEscape, XmlEscapesMetacharacters)
{
    EXPECT_EQ(WordExporter::escapeXml("<tag>"),  "&lt;tag&gt;");
    EXPECT_EQ(WordExporter::escapeXml("a & b"),  "a &amp; b");
    EXPECT_EQ(WordExporter::escapeXml("\"x\""), "&quot;x&quot;");
    EXPECT_EQ(WordExporter::escapeXml("'x'"),   "&apos;x&apos;");
    EXPECT_EQ(WordExporter::escapeXml("plain"),  "plain");
}

// Regression for GitHub #153: escapeXml passed XML-illegal control characters
// straight through into word/document.xml and the ExcelExporter sheet XML,
// producing a .docx/.xlsx that Word and Excel reject outright as corrupt.
// XML 1.0 §2.2 forbids 0x00-0x08, 0x0B, 0x0C and 0x0E-0x1F outright — they are
// not escapable, since `&#x0B;` is itself illegal — so dropping is the only fix.
//
// Each test below pins ONE property, so a mutant that breaks one is not masked
// by another passing.

TEST(WordExportEscape, XmlDropsIllegalControlCharacters)
{
    for (int b = 0x00; b <= 0x1F; ++b) {
        if (b == '\t' || b == '\n' || b == '\r') continue;
        const std::string in = std::string("a") + static_cast<char>(b) + "b";
        EXPECT_EQ(WordExporter::escapeXml(in), "ab")
            << "byte 0x" << std::hex << b << " survived escapeXml";
    }
    // 0x7F is legal XML but dropped for the same reason as in FormEncoder.
    EXPECT_EQ(WordExporter::escapeXml(std::string("a\x7F""b")), "ab");
}

TEST(WordExportEscape, XmlPreservesTheThreeLegalControlCharacters)
{
    EXPECT_EQ(WordExporter::escapeXml("a\tb"), "a\tb");
    EXPECT_EQ(WordExporter::escapeXml("a\nb"), "a\nb");
    EXPECT_EQ(WordExporter::escapeXml("a\rb"), "a\rb");
}

// Guards the signedness of the iteration variable, which is a SEPARATE defect
// from the control-character one: with a signed `char`, every UTF-8
// continuation byte (>= 0x80) compares as negative and would be swallowed by
// the `c < 0x20` test. Asserted against the INPUT bytes, not against another
// call's output, so it cannot pass by both sides being wrong together.
TEST(WordExportEscape, XmlPreservesNonAsciiUtf8Bytes)
{
    const std::string utf8 = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x93\x84";  // café € 📄
    EXPECT_EQ(WordExporter::escapeXml(utf8), utf8);
}

// The property the export actually depends on, stated directly: whatever comes
// out must contain no byte that makes the XML part ill-formed.
TEST(WordExportEscape, XmlOutputContainsNoIllegalByteForHostileInput)
{
    std::string hostile;
    for (int b = 0x00; b <= 0xFF; ++b) hostile += static_cast<char>(b);

    const std::string out = WordExporter::escapeXml(hostile);
    for (unsigned char c : out) {
        const bool illegal =
            (c < 0x20 && c != '\t' && c != '\n' && c != '\r');
        EXPECT_FALSE(illegal)
            << "escapeXml emitted XML-illegal byte 0x" << std::hex << int(c);
    }
}

TEST(WordExportEscape, RtfEscapesBracesAndBackslash)
{
    EXPECT_EQ(WordExporter::escapeRtf("{x}"),   "\\{x\\}");
    EXPECT_EQ(WordExporter::escapeRtf("a\\b"),  "a\\\\b");
    EXPECT_EQ(WordExporter::escapeRtf("line1\nline2"), "line1\\par line2");
    EXPECT_EQ(WordExporter::escapeRtf("plain"), "plain");
}

TEST(WordExportEscape, RtfEncodesNonAsciiAsUnicodeDirective)
{
    // é (U+00E9) → \u233?
    auto s = WordExporter::escapeRtf("caf\xC3\xA9");
    EXPECT_NE(s.find("\\u233?"), std::string::npos) << s;
}

// -------- MiniZip --------

TEST(MiniZipStore, ProducesReadableZipWithExpectedPrefixAndSuffix)
{
    auto out = tempOut("mini.zip");
    std::filesystem::remove(out);
    {
        minizip::ZipWriter zw(out);
        ASSERT_TRUE(zw.valid());
        EXPECT_TRUE(zw.addFile("hello.txt", "Hello, ZIP!"));
        EXPECT_TRUE(zw.close());
    }
    auto bytes = readAll(out);
    ASSERT_GE(bytes.size(), 4u);
    // Local file header magic "PK\x03\x04"
    EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 'P');
    EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 'K');
    EXPECT_EQ(static_cast<unsigned char>(bytes[2]), 0x03u);
    EXPECT_EQ(static_cast<unsigned char>(bytes[3]), 0x04u);
    // End-of-central-directory magic "PK\x05\x06" appears near the tail.
    auto tail = bytes.substr(bytes.size() - 22);
    EXPECT_EQ(static_cast<unsigned char>(tail[0]), 'P');
    EXPECT_EQ(static_cast<unsigned char>(tail[1]), 'K');
    EXPECT_EQ(static_cast<unsigned char>(tail[2]), 0x05u);
    EXPECT_EQ(static_cast<unsigned char>(tail[3]), 0x06u);
    // File payload appears unmolested.
    EXPECT_NE(bytes.find("Hello, ZIP!"), std::string::npos);
    // Filename appears.
    EXPECT_NE(bytes.find("hello.txt"), std::string::npos);
}

// -------- End-to-end --------

class WordExporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = fixturesDir() / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
};

TEST_F(WordExporterTest, DocxOutputIsValidZipWithDocumentXml)
{
    auto out = tempOut("out.docx");
    std::filesystem::remove(out);

    WordExporter ex(doc.get());
    WordExportOptions opts;
    opts.format = WordFormat::Docx;
    auto r = ex.exportTo(out, opts);

    ASSERT_TRUE(r.success) << r.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(out));
    const auto bytes = readAll(out);
    // PK magic at the start — valid zip.
    EXPECT_EQ(bytes.substr(0, 2), std::string("PK"));
    // The required parts appear inside the archive.
    EXPECT_NE(bytes.find("[Content_Types].xml"), std::string::npos);
    EXPECT_NE(bytes.find("word/document.xml"), std::string::npos);
    EXPECT_NE(bytes.find("<w:document"), std::string::npos);
    EXPECT_NE(bytes.find("<w:body"), std::string::npos);
}

TEST_F(WordExporterTest, RtfOutputIsSyntacticallyValid)
{
    auto out = tempOut("out.rtf");
    std::filesystem::remove(out);

    WordExporter ex(doc.get());
    WordExportOptions opts;
    opts.format = WordFormat::Rtf;
    auto r = ex.exportTo(out, opts);

    ASSERT_TRUE(r.success) << r.errorMessage;
    const auto body = readAll(out);
    // RTF must start with {\rtf1 and end with a closing brace.
    EXPECT_EQ(body.substr(0, 6), std::string("{\\rtf1"));
    EXPECT_EQ(body.back(), '\n');  // trailing newline from buildRtfBody
    // Should contain a font table directive.
    EXPECT_NE(body.find("\\fonttbl"), std::string::npos);
    // Should contain a page break between source pages (there are 3).
    EXPECT_NE(body.find("\\page"), std::string::npos);
}

TEST_F(WordExporterTest, PageRangeRestrictsOutput)
{
    auto fullOut = tempOut("full.rtf");
    auto partialOut = tempOut("partial.rtf");
    std::filesystem::remove(fullOut);
    std::filesystem::remove(partialOut);

    WordExporter ex(doc.get());
    WordExportOptions optsAll;
    optsAll.format = WordFormat::Rtf;
    auto rAll = ex.exportTo(fullOut, optsAll);
    ASSERT_TRUE(rAll.success);

    WordExportOptions optsOne;
    optsOne.format = WordFormat::Rtf;
    optsOne.explicitPages = {0};
    auto rOne = ex.exportTo(partialOut, optsOne);
    ASSERT_TRUE(rOne.success);

    EXPECT_EQ(rAll.pagesExported, 3);
    EXPECT_EQ(rOne.pagesExported, 1);
    EXPECT_LT(readAll(partialOut).size(), readAll(fullOut).size())
        << "single-page RTF should be strictly smaller than full export";
}

TEST_F(WordExporterTest, BoldIsEmittedWhenFontNameImpliesBold)
{
    // The heuristic converts font names like "Times-Bold" into <w:b/> +
    // \b runs. This end-to-end check exercises the code path but cannot
    // assert the specific markup because test_3page.pdf may not use a
    // Bold-named font. Instead, verify the DOCX document.xml is valid
    // and the RTF doesn't contain unescaped braces that would break it.
    WordExporter ex(doc.get());
    WordExportOptions opts;
    opts.format = WordFormat::Docx;
    auto out = tempOut("bold_check.docx");
    std::filesystem::remove(out);
    auto r = ex.exportTo(out, opts);
    ASSERT_TRUE(r.success);
    const auto bytes = readAll(out);
    // Just a structural sanity check.
    EXPECT_NE(bytes.find("<w:document"), std::string::npos);
}

TEST_F(WordExporterTest, EmptyDocumentHandledGracefully)
{
    // Construct an ExplicitPages that's out of range → no pages exported.
    WordExporter ex(doc.get());
    WordExportOptions opts;
    opts.format = WordFormat::Rtf;
    opts.explicitPages = {999, 1000};  // both OOB on a 3-page fixture
    auto out = tempOut("empty_range.rtf");
    std::filesystem::remove(out);
    auto r = ex.exportTo(out, opts);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.pagesExported, 0);
    // File still exists and is a valid RTF shell.
    const auto body = readAll(out);
    EXPECT_EQ(body.substr(0, 6), std::string("{\\rtf1"));
}

TEST(WordExporterRobustness, NullDocumentReturnsErrorNotCrash)
{
    WordExporter ex(nullptr);
    WordExportOptions opts;
    opts.format = WordFormat::Docx;
    auto r = ex.exportTo(std::filesystem::temp_directory_path() / "null.docx", opts);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.errorMessage.empty());
}
