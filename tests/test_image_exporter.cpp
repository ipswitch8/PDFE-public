// -*- coding: utf-8 -*-
// Phase 7a: ImageExporter tests.
// Covers filename-pattern substitution (tokens, zero-padding), full
// document export, page-range filtering, cancellation, atomic writes
// (no leftover .part files), PNG+JPEG format selection.
#include <gtest/gtest.h>
#include "exports/ImageExporter.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QImage>
#include <filesystem>
#include <set>
#include <memory>
#include <string>

using namespace pdfe::exports;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path freshOutDir(const char* suffix)
{
    auto d = std::filesystem::temp_directory_path()
           / "pdfe_image_export_tests" / suffix;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d);
    return d;
}

int countFilesWithExtension(const std::filesystem::path& dir, const std::string& ext)
{
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ext) ++n;
    }
    return n;
}

/// Every filename in `dir` that is not one of `expected` — WIN-87 site #6,
/// phase-58.
///
/// The two callers used to assert `countFilesWithExtension(dir, ".part") == 0`.
/// That name no longer exists: the export stages into a StagingFile with a
/// CSPRNG name, so a check for ".part" now reads as coverage while testing
/// nothing — it can never fail, for any bug. Phase-57 hit the identical problem
/// three times when ".redact_tmp"/".redact_out" were removed, and the answer is
/// the same: assert on the WHOLE listing rather than on a name, so the oracle
/// catches residue of any name including one nobody has thought of.
std::set<std::string> unexpectedResidue(const std::filesystem::path& dir,
                                        const std::set<std::string>& expectedExts)
{
    std::set<std::string> left;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        if (expectedExts.count(it->path().extension().string())) continue;
        left.insert(it->path().filename().string());
    }
    return left;
}

std::string joined(const std::set<std::string>& s)
{
    std::string out;
    for (const auto& n : s) { out += n; out += ' '; }
    return out;
}

} // namespace

// -------- Pattern substitution --------

TEST(ImageExporterPattern, PageTokenIsOneBasedAndZeroPadded)
{
    // 10 pages → 2-digit pad.
    EXPECT_EQ(ImageExporter::applyPattern("page_{page}",  0, 10, "doc", "png"),
              "page_01");
    EXPECT_EQ(ImageExporter::applyPattern("page_{page}",  9, 10, "doc", "png"),
              "page_10");
    // 100 pages → 3-digit pad.
    EXPECT_EQ(ImageExporter::applyPattern("page_{page}",  4, 100, "doc", "png"),
              "page_005");
}

TEST(ImageExporterPattern, PageZIsZeroBased)
{
    EXPECT_EQ(ImageExporter::applyPattern("p_{pageZ}", 0, 10, "doc", "png"),
              "p_00");
    EXPECT_EQ(ImageExporter::applyPattern("p_{pageZ}", 9, 10, "doc", "png"),
              "p_09");
}

TEST(ImageExporterPattern, BasenameAndExtTokensSubstitute)
{
    EXPECT_EQ(
        ImageExporter::applyPattern("{basename}_{page}.{ext}", 0, 5, "report", "jpg"),
        "report_1.jpg");
}

TEST(ImageExporterPattern, UnknownTokenLeftAsLiteral)
{
    // Literal braces without a known token pass through.
    auto s = ImageExporter::applyPattern("literal_{foo}_end", 0, 1, "a", "png");
    EXPECT_EQ(s, "literal_{foo}_end");
}

TEST(ImageExporterPattern, PatternWithNoTokensIsStatic)
{
    EXPECT_EQ(ImageExporter::applyPattern("just_text", 3, 10, "doc", "png"),
              "just_text");
}

// -------- End-to-end export --------

class ImageExporterEndToEnd : public ::testing::Test {
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

TEST_F(ImageExporterEndToEnd, PngExportProducesOneFilePerPage)
{
    auto outDir = freshOutDir("png_all");
    ImageExporter ex(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;  // fast
    auto result = ex.exportToFolder(outDir, opts);

    EXPECT_EQ(result.totalRequested, 3);
    EXPECT_EQ(result.successCount, 3);
    EXPECT_EQ(result.errorCount, 0);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(countFilesWithExtension(outDir, ".png"), 3);
    // NAME-BLIND: no residue of ANY name, not just the one this export used to
    // produce. ".part" is gone (WIN-87 site #6), and an assertion about a name
    // nothing produces cannot fail for any bug.
    {
        const auto left = unexpectedResidue(outDir, {".png"});
        EXPECT_TRUE(left.empty()) << "export residue survived: " << joined(left);
    }
}

TEST_F(ImageExporterEndToEnd, JpegExportHonoursFormatAndQuality)
{
    auto outDir = freshOutDir("jpeg_all");
    ImageExporter ex(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Jpeg;
    opts.jpegQuality = 75;
    opts.dpi = 72;
    auto result = ex.exportToFolder(outDir, opts);

    EXPECT_EQ(result.successCount, 3);
    EXPECT_EQ(countFilesWithExtension(outDir, ".jpg"), 3);
    EXPECT_EQ(countFilesWithExtension(outDir, ".png"), 0);

    // Sanity: the saved JPEGs open as readable images.
    for (const auto& fr : result.fileResults) {
        QImage img(QString::fromStdString(fr.outputPath.string()));
        EXPECT_FALSE(img.isNull()) << "failed to reopen " << fr.outputPath;
    }
}

TEST_F(ImageExporterEndToEnd, ExplicitPageRangeFiltersOutput)
{
    auto outDir = freshOutDir("page_range");
    ImageExporter ex(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;
    opts.explicitPages = {0, 2};  // skip page 1 (0-based)
    auto result = ex.exportToFolder(outDir, opts);

    EXPECT_EQ(result.totalRequested, 2);
    EXPECT_EQ(result.successCount, 2);
    EXPECT_EQ(countFilesWithExtension(outDir, ".png"), 2);
}

TEST_F(ImageExporterEndToEnd, CancellationStopsMidRun)
{
    auto outDir = freshOutDir("cancel");
    ImageExporter ex(doc.get());

    // Cancel BEFORE any work starts — first iteration's check should
    // observe cancelled_=true and stop immediately.
    ex.cancel();

    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;
    auto result = ex.exportToFolder(outDir, opts);

    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.successCount, 0);
    // No files written.
    EXPECT_EQ(countFilesWithExtension(outDir, ".png"), 0);
    // Name-blind, same reason as above: a cancelled export must leave the
    // directory exactly as it found it, whatever the staging artifact is called.
    {
        const auto left = unexpectedResidue(outDir, {});
        EXPECT_TRUE(left.empty())
            << "cancelled export left residue behind: " << joined(left);
    }
}

TEST_F(ImageExporterEndToEnd, FilenamePatternAppliedPerPage)
{
    auto outDir = freshOutDir("pattern");
    ImageExporter ex(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;
    opts.filenamePattern = "custom_{page}_of_{basename}";
    auto result = ex.exportToFolder(outDir, opts);

    ASSERT_EQ(result.successCount, 3);
    // Confirm at least one file named per the pattern exists.
    bool foundCustom = false;
    for (const auto& entry : std::filesystem::directory_iterator(outDir)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("custom_1_of_test_3page", 0) == 0) {
            foundCustom = true;
            break;
        }
    }
    EXPECT_TRUE(foundCustom) << "expected custom_1_of_test_3page.png";
}

TEST_F(ImageExporterEndToEnd, ProgressCallbackCalledPerFile)
{
    auto outDir = freshOutDir("progress");
    ImageExporter ex(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;

    int lastDone = -1;
    int lastTotal = -1;
    int calls = 0;
    ex.exportToFolder(outDir, opts, [&](int done, int total) {
        lastDone = done;
        lastTotal = total;
        ++calls;
    });

    EXPECT_EQ(calls, 3);
    EXPECT_EQ(lastDone, 3);
    EXPECT_EQ(lastTotal, 3);
}
