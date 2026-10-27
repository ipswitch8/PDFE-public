// -*- coding: utf-8 -*-
// Phase 7c: ExcelExporter tests.
// Pure helpers (number parsing, column letter) plus end-to-end XLSX
// generation on a fixture. The XLSX is validated structurally (valid
// zip, required parts present, well-formed XML-like markers).
#include <gtest/gtest.h>
#include "exports/ExcelExporter.h"
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
    auto d = std::filesystem::temp_directory_path() / "pdfe_xlsx_tests";
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

// -------- Static helpers --------

TEST(ExcelExportNumber, AcceptsIntegersAndDecimals)
{
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("42"));
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("-17"));
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("3.14"));
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("1e6"));
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("-0.5"));
    EXPECT_TRUE(ExcelExporter::looksLikeNumber("42  "));  // trailing ws OK
}

TEST(ExcelExportNumber, RejectsNonNumericStrings)
{
    EXPECT_FALSE(ExcelExporter::looksLikeNumber(""));
    EXPECT_FALSE(ExcelExporter::looksLikeNumber("abc"));
    EXPECT_FALSE(ExcelExporter::looksLikeNumber("12abc"));
    EXPECT_FALSE(ExcelExporter::looksLikeNumber("12.34.56"));
    EXPECT_FALSE(ExcelExporter::looksLikeNumber("$12"));
}

TEST(ExcelExportColumnLetter, MapsAZAndWrapsToDoubleLetters)
{
    EXPECT_EQ(ExcelExporter::columnLetter(0),  "A");
    EXPECT_EQ(ExcelExporter::columnLetter(1),  "B");
    EXPECT_EQ(ExcelExporter::columnLetter(25), "Z");
    EXPECT_EQ(ExcelExporter::columnLetter(26), "AA");
    EXPECT_EQ(ExcelExporter::columnLetter(27), "AB");
    EXPECT_EQ(ExcelExporter::columnLetter(51), "AZ");
    EXPECT_EQ(ExcelExporter::columnLetter(52), "BA");
    EXPECT_EQ(ExcelExporter::columnLetter(701), "ZZ");
    EXPECT_EQ(ExcelExporter::columnLetter(702), "AAA");
}

// -------- End-to-end --------

class ExcelExporterTest : public ::testing::Test {
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

TEST_F(ExcelExporterTest, OutputIsValidZipWithRequiredParts)
{
    auto out = tempOut("basic.xlsx");
    std::filesystem::remove(out);

    ExcelExporter ex(doc.get());
    auto r = ex.exportTo(out, ExcelExportOptions{});
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_EQ(r.sheetsEmitted, 3);

    auto bytes = readAll(out);
    EXPECT_EQ(bytes.substr(0, 2), std::string("PK"));
    // Must contain the four structural parts.
    EXPECT_NE(bytes.find("[Content_Types].xml"), std::string::npos);
    EXPECT_NE(bytes.find("xl/workbook.xml"), std::string::npos);
    EXPECT_NE(bytes.find("xl/worksheets/sheet1.xml"), std::string::npos);
    EXPECT_NE(bytes.find("<workbook "), std::string::npos);
    EXPECT_NE(bytes.find("<sheetData>"), std::string::npos);
}

TEST_F(ExcelExporterTest, SheetCountMatchesPageRange)
{
    auto out = tempOut("range.xlsx");
    std::filesystem::remove(out);

    ExcelExporter ex(doc.get());
    ExcelExportOptions opts;
    opts.explicitPages = {0, 2};
    auto r = ex.exportTo(out, opts);
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.sheetsEmitted, 2);

    auto bytes = readAll(out);
    EXPECT_NE(bytes.find("sheet1.xml"), std::string::npos);
    EXPECT_NE(bytes.find("sheet2.xml"), std::string::npos);
    EXPECT_EQ(bytes.find("sheet3.xml"), std::string::npos);
}

TEST_F(ExcelExporterTest, EmptyPageRangeProducesZeroSheets)
{
    auto out = tempOut("empty.xlsx");
    std::filesystem::remove(out);

    ExcelExporter ex(doc.get());
    ExcelExportOptions opts;
    opts.explicitPages = {999};  // OOB → filtered to empty
    auto r = ex.exportTo(out, opts);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.sheetsEmitted, 0);

    auto bytes = readAll(out);
    EXPECT_EQ(bytes.substr(0, 2), std::string("PK"))
        << "zero-sheet workbook is still a valid zip";
    // No sheet1.xml in a zero-sheet workbook.
    EXPECT_EQ(bytes.find("worksheets/sheet1.xml"), std::string::npos);
}

TEST(ExcelExporterRobustness, NullDocumentReturnsError)
{
    ExcelExporter ex(nullptr);
    auto r = ex.exportTo(std::filesystem::temp_directory_path() / "nil.xlsx",
                         ExcelExportOptions{});
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.errorMessage.empty());
}
