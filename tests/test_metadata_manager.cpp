#include <gtest/gtest.h>
#include "metadata/MetadataManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include <filesystem>
#include <memory>
#include <fstream>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::metadata;

class MetadataManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        std::string testPdfPath = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
        doc_ = Document::open(context_, testPdfPath);
        manager_ = std::make_unique<MetadataManager>(context_);
    }

    void TearDown() override {
        doc_.reset();
        manager_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::shared_ptr<Document> doc_;
    std::unique_ptr<MetadataManager> manager_;
};

// Test 1: Construct - no throw
TEST_F(MetadataManagerTest, Construct) {
    EXPECT_NO_THROW({
        auto context = std::make_shared<MuPdfContext>();
        MetadataManager manager(context);
    });
}

// Test 2: ReadMetadata_Default - read metadata from test_single.pdf, fields are strings (may be empty)
TEST_F(MetadataManagerTest, ReadMetadata_Default) {
    ASSERT_NE(doc_, nullptr);

    DocumentMetadata metadata = manager_->readMetadata(doc_.get());

    // All fields should be strings (may be empty, but should not throw)
    EXPECT_NO_THROW({
        std::string title = metadata.title;
        std::string author = metadata.author;
        std::string subject = metadata.subject;
        std::string keywords = metadata.keywords;
        std::string creator = metadata.creator;
        std::string producer = metadata.producer;
        std::string creationDate = metadata.creationDate;
        std::string modDate = metadata.modDate;
    });
}

// Test 3: ReadField_Title - readField with "info:Title" returns string
TEST_F(MetadataManagerTest, ReadField_Title) {
    ASSERT_NE(doc_, nullptr);

    std::string title = manager_->readField(doc_.get(), "info:Title");

    // Should return a string (may be empty)
    EXPECT_NO_THROW({
        size_t len = title.length();
        (void)len; // Suppress unused variable warning
    });
}

// Test 4: ReadField_InvalidKey - readField with "info:Nonexistent" returns empty string
TEST_F(MetadataManagerTest, ReadField_InvalidKey) {
    ASSERT_NE(doc_, nullptr);

    std::string value = manager_->readField(doc_.get(), "info:Nonexistent");

    EXPECT_EQ(value, "");
}

// Test 5: WriteField_Title - write "info:Title" = "Test Title", read back, verify
TEST_F(MetadataManagerTest, WriteField_Title) {
    ASSERT_NE(doc_, nullptr);

    const std::string testTitle = "Test Title";
    manager_->writeField(doc_.get(), "info:Title", testTitle);

    std::string readBack = manager_->readField(doc_.get(), "info:Title");
    EXPECT_EQ(readBack, testTitle);
}

// Test 6: WriteField_Author - write "info:Author" = "Test Author", read back, verify
TEST_F(MetadataManagerTest, WriteField_Author) {
    ASSERT_NE(doc_, nullptr);

    const std::string testAuthor = "Test Author";
    manager_->writeField(doc_.get(), "info:Author", testAuthor);

    std::string readBack = manager_->readField(doc_.get(), "info:Author");
    EXPECT_EQ(readBack, testAuthor);
}

// Test 7: WriteMetadata_Roundtrip - write full DocumentMetadata, read back, verify all fields match
TEST_F(MetadataManagerTest, WriteMetadata_Roundtrip) {
    ASSERT_NE(doc_, nullptr);

    DocumentMetadata testMetadata;
    testMetadata.title = "Test Document Title";
    testMetadata.author = "John Doe";
    testMetadata.subject = "Unit Testing";
    testMetadata.keywords = "test, pdf, metadata";
    testMetadata.creator = "PDFE Test Suite";
    testMetadata.producer = "PDFE Library";
    testMetadata.creationDate = "D:20240101120000";
    testMetadata.modDate = "D:20240201120000";

    manager_->writeMetadata(doc_.get(), testMetadata);

    DocumentMetadata readBack = manager_->readMetadata(doc_.get());

    EXPECT_EQ(readBack.title, testMetadata.title);
    EXPECT_EQ(readBack.author, testMetadata.author);
    EXPECT_EQ(readBack.subject, testMetadata.subject);
    EXPECT_EQ(readBack.keywords, testMetadata.keywords);
    EXPECT_EQ(readBack.creator, testMetadata.creator);
    EXPECT_EQ(readBack.producer, testMetadata.producer);
    EXPECT_EQ(readBack.creationDate, testMetadata.creationDate);
    EXPECT_EQ(readBack.modDate, testMetadata.modDate);

    // R-11 strengthening: the in-memory read-back above passes even for a
    // "stages metadata in the manager, never serializes to the /Info dict"
    // bug. A test named *_Roundtrip must actually round-trip through disk.
    // Save, reopen from a fresh Document, and assert every field persisted.
    const auto tmpPath =
        (std::filesystem::temp_directory_path()
         / "pdfe_meta_roundtrip_persist.pdf").string();
    std::error_code rtEc;
    std::filesystem::remove(tmpPath, rtEc);
    doc_->saveAs(tmpPath);

    auto reopened = Document::open(context_, tmpPath);
    ASSERT_NE(reopened, nullptr) << "save+reopen produced no document.";
    DocumentMetadata persisted = manager_->readMetadata(reopened.get());

    EXPECT_EQ(persisted.title, testMetadata.title)
        << "title must survive save+reopen, not just live in memory.";
    EXPECT_EQ(persisted.author, testMetadata.author)
        << "author must survive save+reopen.";
    EXPECT_EQ(persisted.subject, testMetadata.subject)
        << "subject must survive save+reopen.";
    EXPECT_EQ(persisted.keywords, testMetadata.keywords)
        << "keywords must survive save+reopen.";
    EXPECT_EQ(persisted.creator, testMetadata.creator)
        << "creator must survive save+reopen.";

    reopened.reset();
    std::filesystem::remove(tmpPath, rtEc);
}

// Test 8: WriteMetadata_SpecialChars - write metadata with special chars (UTF-8), read back
TEST_F(MetadataManagerTest, WriteMetadata_SpecialChars) {
    ASSERT_NE(doc_, nullptr);

    DocumentMetadata testMetadata;
    testMetadata.title = "Тест Unicode: 日本語 & Émojis 🎉";
    testMetadata.author = "José García-López";
    testMetadata.subject = "Spëcial Chärs & Symbols: €£¥";
    testMetadata.keywords = "测试, тест, テスト";

    manager_->writeMetadata(doc_.get(), testMetadata);

    DocumentMetadata readBack = manager_->readMetadata(doc_.get());

    EXPECT_EQ(readBack.title, testMetadata.title);
    EXPECT_EQ(readBack.author, testMetadata.author);
    EXPECT_EQ(readBack.subject, testMetadata.subject);
    EXPECT_EQ(readBack.keywords, testMetadata.keywords);
}

// Test 9: WriteField_EmptyValue - write empty string clears the field
TEST_F(MetadataManagerTest, WriteField_EmptyValue) {
    ASSERT_NE(doc_, nullptr);

    // First set a value
    manager_->writeField(doc_.get(), "info:Title", "Initial Title");
    EXPECT_EQ(manager_->readField(doc_.get(), "info:Title"), "Initial Title");

    // Then clear it with empty string
    manager_->writeField(doc_.get(), "info:Title", "");

    std::string readBack = manager_->readField(doc_.get(), "info:Title");
    EXPECT_EQ(readBack, "");
}

// Test 10: ClearMetadata - after write, clear, verify all fields empty
TEST_F(MetadataManagerTest, ClearMetadata) {
    ASSERT_NE(doc_, nullptr);

    // Write some metadata
    DocumentMetadata testMetadata;
    testMetadata.title = "Test Title";
    testMetadata.author = "Test Author";
    testMetadata.subject = "Test Subject";
    testMetadata.keywords = "test keywords";

    manager_->writeMetadata(doc_.get(), testMetadata);

    // Clear all metadata
    manager_->clearMetadata(doc_.get());

    // Verify all fields are empty
    DocumentMetadata readBack = manager_->readMetadata(doc_.get());

    EXPECT_EQ(readBack.title, "");
    EXPECT_EQ(readBack.author, "");
    EXPECT_EQ(readBack.subject, "");
    EXPECT_EQ(readBack.keywords, "");
}

// Test 11: ReadMetadata_NullDoc - returns empty DocumentMetadata
TEST_F(MetadataManagerTest, ReadMetadata_NullDoc) {
    DocumentMetadata metadata = manager_->readMetadata(nullptr);

    EXPECT_EQ(metadata.title, "");
    EXPECT_EQ(metadata.author, "");
    EXPECT_EQ(metadata.subject, "");
    EXPECT_EQ(metadata.keywords, "");
    EXPECT_EQ(metadata.creator, "");
    EXPECT_EQ(metadata.producer, "");
    EXPECT_EQ(metadata.creationDate, "");
    EXPECT_EQ(metadata.modDate, "");
}

// Test 12: WriteMetadata_NullDoc - does not throw
TEST_F(MetadataManagerTest, WriteMetadata_NullDoc) {
    DocumentMetadata testMetadata;
    testMetadata.title = "Test Title";

    EXPECT_NO_THROW({
        manager_->writeMetadata(nullptr, testMetadata);
    });
}

// Test 13: ClearMetadata_NullDoc - does not throw
TEST_F(MetadataManagerTest, ClearMetadata_NullDoc) {
    EXPECT_NO_THROW({
        manager_->clearMetadata(nullptr);
    });
}

// Test 14: WriteField_ReadBack_Persistence - write title, save, reopen, verify title persists
TEST_F(MetadataManagerTest, WriteField_ReadBack_Persistence) {
    ASSERT_NE(doc_, nullptr);

    // Create a temporary file path
    auto tempPath = (std::filesystem::temp_directory_path() / "pdfe_test_meta_persist.pdf").string();

    // Write metadata and save
    const std::string testTitle = "Persistent Test Title";
    manager_->writeField(doc_.get(), "info:Title", testTitle);
    doc_->saveAs(tempPath);

    // Close the document
    doc_.reset();

    // Reopen the saved document
    std::shared_ptr<Document> reopenedDoc = Document::open(context_, tempPath);
    ASSERT_NE(reopenedDoc, nullptr);

    // Verify the title persists
    std::string readBack = manager_->readField(reopenedDoc.get(), "info:Title");
    EXPECT_EQ(readBack, testTitle);

    // Clean up
    reopenedDoc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 15: ReadField_Producer - read "info:Producer" field (usually set by PDF library)
TEST_F(MetadataManagerTest, ReadField_Producer) {
    ASSERT_NE(doc_, nullptr);

    std::string producer = manager_->readField(doc_.get(), "info:Producer");

    // Producer may or may not be set, but should return a string without throwing
    EXPECT_NO_THROW({
        size_t len = producer.length();
        (void)len; // Suppress unused variable warning
    });
}
