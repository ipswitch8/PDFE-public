// -*- coding: utf-8 -*-
// Phase 5c: AttachmentManager tests — CRUD on /EmbeddedFiles, byte-exact
// round-trip through save/reload, filename sanitization (path traversal
// rejection on extract).
#include <gtest/gtest.h>
#include "attachments/AttachmentManager.h"
#include "attachments/AttachmentData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::attachments;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempDir(const char* suffix)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_attach_tests" / suffix;
    std::filesystem::create_directories(d);
    return d;
}

void writeFile(const std::filesystem::path& p, const std::string& content)
{
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// -------- sanitizeFilename --------

TEST(AttachmentSanitize, PlainBasenamePassesThrough)
{
    EXPECT_EQ(AttachmentManager::sanitizeFilename("report.pdf"), "report.pdf");
}

TEST(AttachmentSanitize, StripsDirectoryPrefix)
{
    EXPECT_EQ(AttachmentManager::sanitizeFilename("subdir/report.pdf"),
              "report.pdf");
    EXPECT_EQ(AttachmentManager::sanitizeFilename("C:/windows/bad.exe"),
              "bad.exe");
}

TEST(AttachmentSanitize, RejectsTraversalSequences)
{
    EXPECT_EQ(AttachmentManager::sanitizeFilename("../../etc/passwd"),
              "passwd");
    EXPECT_EQ(AttachmentManager::sanitizeFilename(".."),
              "attachment");
    EXPECT_EQ(AttachmentManager::sanitizeFilename("."),
              "attachment");
}

TEST(AttachmentSanitize, StripsBackslashPaths)
{
    EXPECT_EQ(AttachmentManager::sanitizeFilename(
        "C:\\\\Windows\\\\System32\\\\evil.dll"), "evil.dll");
}

TEST(AttachmentSanitize, StripsLeadingDots)
{
    // ".secret" → "secret" (no hidden files via attachments)
    EXPECT_EQ(AttachmentManager::sanitizeFilename(".secret"), "secret");
}

TEST(AttachmentSanitize, EmptyReturnsPlaceholder)
{
    EXPECT_EQ(AttachmentManager::sanitizeFilename(""), "attachment");
}

// -------- CRUD --------

class AttachmentCRUDTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = fixturesDir() / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        workDir = tempDir("crud");
        // Copy source to a working file so each test starts fresh.
        workPdf = workDir / "work.pdf";
        std::filesystem::copy_file(src, workPdf,
            std::filesystem::copy_options::overwrite_existing);

        // Create a small test attachment.
        attachSrc = workDir / "attach.txt";
        attachContent = "Hello, attachment!\nLine 2.\n";
        writeFile(attachSrc, attachContent);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path workDir;
    std::filesystem::path workPdf;
    std::filesystem::path attachSrc;
    std::string attachContent;
};

TEST_F(AttachmentCRUDTest, EmptyListOnFreshDocument)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    auto list = mgr.list(doc.get());
    EXPECT_TRUE(list.empty());
}

TEST_F(AttachmentCRUDTest, AddListDeleteSequence)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);

    AttachmentManager mgr(ctx);
    ASSERT_TRUE(mgr.add(doc.get(), attachSrc, "mynote.txt"));

    auto listAfterAdd = mgr.list(doc.get());
    ASSERT_EQ(listAfterAdd.size(), 1u);
    EXPECT_EQ(listAfterAdd[0].name, "mynote.txt");

    EXPECT_TRUE(mgr.remove(doc.get(), "mynote.txt"));
    auto listAfterRemove = mgr.list(doc.get());
    EXPECT_TRUE(listAfterRemove.empty());
}

TEST_F(AttachmentCRUDTest, AttachmentSurvivesSaveReload)
{
    {
        auto doc = Document::open(ctx, workPdf);
        ASSERT_NE(doc, nullptr);
        AttachmentManager mgr(ctx);
        ASSERT_TRUE(mgr.add(doc.get(), attachSrc, "persist.txt"));
        // Save back to the same work path (Document::saveAs).
        auto savedTo = workDir / "work_saved.pdf";
        doc->saveAs(savedTo);
        ASSERT_TRUE(std::filesystem::exists(savedTo));
        workPdf = savedTo;
    }
    // Re-open.
    auto doc2 = Document::open(ctx, workPdf);
    ASSERT_NE(doc2, nullptr);
    AttachmentManager mgr2(ctx);
    auto list = mgr2.list(doc2.get());
    ASSERT_EQ(list.size(), 1u) << "Attachment lost across save/reload";
    EXPECT_EQ(list[0].name, "persist.txt");
}

TEST_F(AttachmentCRUDTest, ExtractRoundTripIsByteExact)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    ASSERT_TRUE(mgr.add(doc.get(), attachSrc, "byteexact.txt"));

    auto savedTo = workDir / "byteexact_saved.pdf";
    doc->saveAs(savedTo);
    ASSERT_TRUE(std::filesystem::exists(savedTo));

    auto doc2 = Document::open(ctx, savedTo);
    ASSERT_NE(doc2, nullptr);
    AttachmentManager mgr2(ctx);

    auto outDir = workDir / "extract_out";
    std::filesystem::remove_all(outDir);
    auto extracted = mgr2.extract(doc2.get(), "byteexact.txt", outDir);
    ASSERT_FALSE(extracted.empty()) << "Extract failed";
    ASSERT_TRUE(std::filesystem::exists(extracted));

    EXPECT_EQ(readFile(extracted), attachContent)
        << "Extracted bytes differ from original";
}

TEST_F(AttachmentCRUDTest, ExtractSanitizesMaliciousFilename)
{
    // Add an attachment whose stored display name is a traversal path.
    // sanitizeFilename at add time rewrites the stored name, so the
    // attack surface here is minimal — but we also verify extract()
    // never writes outside outDir.
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    ASSERT_TRUE(mgr.add(doc.get(), attachSrc, "../../escape.txt"));

    // Stored name should be sanitized to "escape.txt".
    auto list = mgr.list(doc.get());
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "escape.txt")
        << "Sanitization must strip ../ from stored name at add time";

    auto outDir = workDir / "extract_safe";
    std::filesystem::remove_all(outDir);
    auto extracted = mgr.extract(doc.get(), "escape.txt", outDir);
    ASSERT_FALSE(extracted.empty());

    // The file must be INSIDE outDir (no escape).
    const auto canonOut = std::filesystem::weakly_canonical(outDir);
    const auto canonExtracted = std::filesystem::weakly_canonical(extracted);
    const auto canonOutStr = canonOut.string();
    EXPECT_EQ(canonExtracted.string().rfind(canonOutStr, 0), 0u)
        << "Extract wrote outside outDir: " << canonExtracted;
}

TEST_F(AttachmentCRUDTest, ListReportsSizeForEmbeddedFile)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    ASSERT_TRUE(mgr.add(doc.get(), attachSrc, "sized.txt"));

    auto list = mgr.list(doc.get());
    ASSERT_EQ(list.size(), 1u);
    // Size may be -1 (unknown) on MuPDF builds that don't expose the
    // stream's Params /Size — the test accepts either -1 or the exact
    // byte count.
    EXPECT_TRUE(list[0].size == -1 ||
                list[0].size == static_cast<int64_t>(attachContent.size()))
        << "Unexpected reported size: " << list[0].size;
}

TEST_F(AttachmentCRUDTest, AddRejectsMissingSourceFile)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    EXPECT_FALSE(mgr.add(doc.get(),
        workDir / "does_not_exist.bin", "missing.bin"));
    EXPECT_TRUE(mgr.list(doc.get()).empty());
}

TEST_F(AttachmentCRUDTest, RemoveUnknownNameReturnsFalse)
{
    auto doc = Document::open(ctx, workPdf);
    ASSERT_NE(doc, nullptr);
    AttachmentManager mgr(ctx);
    EXPECT_FALSE(mgr.remove(doc.get(), "nosuchkey"));
}
