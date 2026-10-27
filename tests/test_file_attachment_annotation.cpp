// -*- coding: utf-8 -*-
// Pipeline E phase-5 / Bucket 3b: /FileAttachment annotation type.
//
// Verifies the AnnotationManager API for creating, retrieving the
// payload of, and round-tripping through save+reopen, /FileAttachment
// annotations — distinct from the existing document-level
// /EmbeddedFiles attachments.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/FileAttachmentTool.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

// Write `bytes` to `dir/filename` and return the full path.
std::filesystem::path writeFixtureFile(QTemporaryDir& dir,
                                       const std::string& filename,
                                       const std::string& bytes)
{
    std::filesystem::path full = std::filesystem::path(
        dir.path().toStdString()) / filename;
    std::ofstream out(full, std::ios::binary);
    out.write(bytes.data(),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return full;
}

} // namespace

class FileAttachmentAnnotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        page = doc->page(0);
        ASSERT_NE(page, nullptr);
        mgr = std::make_unique<AnnotationManager>(ctx);
        ASSERT_TRUE(workDir.isValid());
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
    QTemporaryDir workDir;
};

// (1) Create-annotation test
TEST_F(FileAttachmentAnnotationTest, CreateAnnotationWritesFileAttachmentSubtype)
{
    const std::string payload =
        "Hello from a Pipeline E phase-5 attachment fixture.";
    auto attachPath = writeFixtureFile(workDir, "note.txt", payload);

    QRectF rect(72, 72, 24, 24);
    AnnotationData data = mgr->createFileAttachmentAnnotation(
        page.get(), rect, attachPath, "PaperClip");

    EXPECT_EQ(data.type, AnnotationType::FileAttachment);
    EXPECT_EQ(data.iconName, "PaperClip");
    EXPECT_GE(data.annotIndex, 0);

    // The annotation should now be reachable via the standard
    // enumeration API as a /FileAttachment.
    auto annots = mgr->getAnnotations(page.get());
    bool sawAttachment = false;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::FileAttachment) {
            sawAttachment = true;
            EXPECT_EQ(a.iconName, "PaperClip");
            break;
        }
    }
    EXPECT_TRUE(sawAttachment);
}

// (2) Place + retrieve payload test
TEST_F(FileAttachmentAnnotationTest, RetrieveEmbeddedPayloadByteExact)
{
    // Use binary bytes (including a NUL) to verify byte-exact round-trip
    // through fz_buffer / pdf_load_embedded_file_contents.
    std::string payload;
    payload.reserve(256);
    for (int i = 0; i < 256; ++i) {
        payload.push_back(static_cast<char>(i));
    }
    auto attachPath = writeFixtureFile(workDir, "binary.bin", payload);

    AnnotationData data = mgr->createFileAttachmentAnnotation(
        page.get(), QRectF(100, 100, 24, 24), attachPath, "PushPin");
    ASSERT_GE(data.annotIndex, 0);

    auto bytes = mgr->readFileAttachmentBytes(page.get(), data.annotIndex);
    ASSERT_EQ(bytes.size(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        ASSERT_EQ(bytes[i], static_cast<unsigned char>(payload[i]))
            << "Byte mismatch at offset " << i;
    }

    // extractFileAttachmentPayload must produce a file with the same bytes.
    std::filesystem::path outDir = std::filesystem::path(
        workDir.path().toStdString()) / "extract";
    auto written = mgr->extractFileAttachmentPayload(
        page.get(), data.annotIndex, outDir);
    ASSERT_FALSE(written.empty());
    ASSERT_TRUE(std::filesystem::exists(written));
    std::ifstream in(written, std::ios::binary);
    std::string read((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(read, payload);
}

// (3) Save + reopen round-trip test
TEST_F(FileAttachmentAnnotationTest, SaveAndReopenPreservesAttachment)
{
    const std::string payload = "round-trip-payload-2026-05-02";
    auto attachPath = writeFixtureFile(workDir, "round_trip.txt", payload);

    AnnotationData data = mgr->createFileAttachmentAnnotation(
        page.get(), QRectF(150, 150, 24, 24), attachPath, "Graph",
        std::string("round_trip.txt"));
    ASSERT_GE(data.annotIndex, 0);

    std::string outPdf = (std::filesystem::path(workDir.path().toStdString())
                          / "with_attachment.pdf").string();
    ASSERT_NO_THROW(doc->saveAs(outPdf));

    page.reset();
    mgr.reset();
    doc.reset();

    auto reopened = Document::open(ctx, outPdf);
    ASSERT_NE(reopened, nullptr);
    auto reopenedPage = reopened->page(0);
    ASSERT_NE(reopenedPage, nullptr);
    auto reopenedMgr = std::make_unique<AnnotationManager>(ctx);

    auto annots = reopenedMgr->getAnnotations(reopenedPage.get());
    int attachIdx = -1;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::FileAttachment) {
            attachIdx = a.annotIndex;
            EXPECT_EQ(a.iconName, "Graph");
            break;
        }
    }
    ASSERT_GE(attachIdx, 0)
        << "/FileAttachment annotation lost during save+reopen";

    auto bytes = reopenedMgr->readFileAttachmentBytes(
        reopenedPage.get(), attachIdx);
    ASSERT_EQ(bytes.size(), payload.size());
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), payload);
}

// Defensive: invalid icon names fall back to PushPin (Acrobat's default).
TEST_F(FileAttachmentAnnotationTest, UnknownIconNameFallsBackToPushPin)
{
    auto attachPath = writeFixtureFile(workDir, "x.bin", "x");
    AnnotationData data = mgr->createFileAttachmentAnnotation(
        page.get(), QRectF(72, 72, 24, 24), attachPath,
        std::string("BogusIcon"));
    EXPECT_EQ(data.iconName, "PushPin");
}

// Missing source file should throw rather than silently creating an
// empty attachment.
TEST_F(FileAttachmentAnnotationTest, MissingSourceFileThrows)
{
    EXPECT_THROW(
        mgr->createFileAttachmentAnnotation(
            page.get(), QRectF(72, 72, 24, 24),
            std::filesystem::path("/this/file/does/not/exist.bin"),
            "PushPin"),
        std::invalid_argument);
}

// Tool-level smoke test: FileAttachmentTool reports the FileAttachment
// type and stores its source file / icon name without errors.
TEST(FileAttachmentToolSetup, ToolStoresConfiguration)
{
    auto ctx = std::make_shared<MuPdfContext>();
    AnnotationManager mgr(ctx);
    FileAttachmentTool tool(&mgr);
    EXPECT_EQ(tool.toolType(), AnnotationType::FileAttachment);
    tool.setSourceFile(std::filesystem::path("/tmp/fixture.bin"));
    tool.setIconName("Tag");
    EXPECT_EQ(tool.sourceFile(),
              std::filesystem::path("/tmp/fixture.bin"));
    EXPECT_EQ(tool.iconName(), std::string("Tag"));
}
