#include <gtest/gtest.h>
#include "metadata/MetadataCommands.h"
#include "metadata/MetadataManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include <filesystem>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::metadata;

class MetadataCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<MetadataManager>(context_);
        testPdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    }

    // Helper to create a temp copy for mutation tests
    std::filesystem::path makeTempCopy(const std::string& suffix) {
        auto tempPath = std::filesystem::temp_directory_path() / ("pdfe_test_metacmd_" + suffix + ".pdf");
        auto srcDoc = Document::open(context_, testPdfPath_);
        srcDoc->saveAs(tempPath);
        return tempPath;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<MetadataManager> manager_;
    std::string testPdfPath_;
};

// Test 1: Construct - no throw
TEST_F(MetadataCommandsTest, Construct_NoThrow) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto doc = Document::open(context_, testPdfPath_);
    DocumentMetadata newMeta;
    newMeta.title = "Test Title";

    EXPECT_NO_THROW({
        EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    });
}

// Test 2: Execute_WritesNewMetadata
TEST_F(MetadataCommandsTest, Execute_WritesNewMetadata) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto tempPath = makeTempCopy("exec_write");
    auto doc = Document::open(context_, tempPath);

    DocumentMetadata newMeta;
    newMeta.title = "New Test Title";
    newMeta.author = "New Test Author";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    cmd.execute();

    auto readBack = manager_->readMetadata(doc.get());
    EXPECT_EQ(readBack.title, "New Test Title");
    EXPECT_EQ(readBack.author, "New Test Author");

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 3: Undo_RestoresOldMetadata
TEST_F(MetadataCommandsTest, Undo_RestoresOldMetadata) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto tempPath = makeTempCopy("undo_restore");
    auto doc = Document::open(context_, tempPath);

    auto originalMeta = manager_->readMetadata(doc.get());

    DocumentMetadata newMeta;
    newMeta.title = "Modified Title";
    newMeta.author = "Modified Author";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    cmd.execute();
    cmd.undo();

    auto restored = manager_->readMetadata(doc.get());
    EXPECT_EQ(restored.title, originalMeta.title);
    EXPECT_EQ(restored.author, originalMeta.author);

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 4: Redo_ReappliesNewMetadata
TEST_F(MetadataCommandsTest, Redo_ReappliesNewMetadata) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto tempPath = makeTempCopy("redo");
    auto doc = Document::open(context_, tempPath);

    DocumentMetadata newMeta;
    newMeta.title = "Redo Title";
    newMeta.author = "Redo Author";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    cmd.execute();
    cmd.undo();
    cmd.redo();

    auto readBack = manager_->readMetadata(doc.get());
    EXPECT_EQ(readBack.title, "Redo Title");
    EXPECT_EQ(readBack.author, "Redo Author");

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 5: Description_ReturnsString
TEST_F(MetadataCommandsTest, Description_ReturnsString) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto doc = Document::open(context_, testPdfPath_);
    DocumentMetadata newMeta;
    newMeta.title = "Test";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    EXPECT_FALSE(cmd.description().empty());
}

// Test 6: Execute_MultipleFields
TEST_F(MetadataCommandsTest, Execute_MultipleFields) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto tempPath = makeTempCopy("multi_fields");
    auto doc = Document::open(context_, tempPath);

    DocumentMetadata newMeta;
    newMeta.title = "Complete Title";
    newMeta.author = "Complete Author";
    newMeta.subject = "Complete Subject";
    newMeta.keywords = "Complete Keywords";
    newMeta.creator = "Complete Creator";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    cmd.execute();

    auto readBack = manager_->readMetadata(doc.get());
    EXPECT_EQ(readBack.title, "Complete Title");
    EXPECT_EQ(readBack.author, "Complete Author");
    EXPECT_EQ(readBack.subject, "Complete Subject");
    EXPECT_EQ(readBack.keywords, "Complete Keywords");
    EXPECT_EQ(readBack.creator, "Complete Creator");

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 7: Undo_AllFields
TEST_F(MetadataCommandsTest, Undo_AllFields) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto tempPath = makeTempCopy("undo_all");
    auto doc = Document::open(context_, tempPath);

    auto originalMeta = manager_->readMetadata(doc.get());

    DocumentMetadata newMeta;
    newMeta.title = "All Fields Title";
    newMeta.author = "All Fields Author";
    newMeta.subject = "All Fields Subject";
    newMeta.keywords = "All Fields Keywords";
    newMeta.creator = "All Fields Creator";

    EditMetadataCommand cmd(manager_.get(), doc.get(), newMeta);
    cmd.execute();
    cmd.undo();

    auto restored = manager_->readMetadata(doc.get());
    EXPECT_EQ(restored.title, originalMeta.title);
    EXPECT_EQ(restored.author, originalMeta.author);
    EXPECT_EQ(restored.subject, originalMeta.subject);
    EXPECT_EQ(restored.keywords, originalMeta.keywords);
    EXPECT_EQ(restored.creator, originalMeta.creator);

    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 8: NullManager_NoThrow
TEST_F(MetadataCommandsTest, NullManager_NoThrow) {
    if (!std::filesystem::exists(testPdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdfPath_;
    }

    auto doc = Document::open(context_, testPdfPath_);
    DocumentMetadata newMeta;
    newMeta.title = "Null Manager Test";

    EditMetadataCommand cmd(nullptr, doc.get(), newMeta);
    EXPECT_NO_THROW({ cmd.execute(); });
}
