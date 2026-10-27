#include <gtest/gtest.h>
#include "ui/MetadataDialog.h"
#include "metadata/MetadataManager.h"
#include <QApplication>
#include <QLineEdit>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::metadata;

namespace {
    void ensureQApplication() {
        if (!QApplication::instance()) {
            static int argc = 0;
            static char* argv[] = { nullptr };
            static QApplication app(argc, argv);
        }
    }
}

class MetadataDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
    }

    void TearDown() override {
        // Cleanup if needed
    }

    // Helper function to create sample metadata
    DocumentMetadata createSampleMetadata() {
        DocumentMetadata metadata;
        metadata.title = "Test Document Title";
        metadata.author = "Test Author";
        metadata.subject = "Test Subject";
        metadata.keywords = "test, metadata, keywords";
        metadata.creator = "Test Creator Application";
        metadata.producer = "Test Producer";
        metadata.creationDate = "2024-01-15T10:30:00Z";
        metadata.modDate = "2024-01-20T15:45:00Z";
        return metadata;
    }
};

TEST_F(MetadataDialogTest, Construct) {
    // Arrange & Act
    std::unique_ptr<MetadataDialog> dialog;

    // Assert - should not throw
    ASSERT_NO_THROW({
        dialog = std::make_unique<MetadataDialog>();
    });

    // Verify objectName
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->objectName().toStdString(), "metadataDialog");
}

TEST_F(MetadataDialogTest, HasAllWidgets) {
    // Arrange
    MetadataDialog dialog;

    // Act - Find all required widgets
    QLineEdit* titleEdit = dialog.findChild<QLineEdit*>("metadataTitleEdit");
    QLineEdit* authorEdit = dialog.findChild<QLineEdit*>("metadataAuthorEdit");
    QLineEdit* subjectEdit = dialog.findChild<QLineEdit*>("metadataSubjectEdit");
    QLineEdit* keywordsEdit = dialog.findChild<QLineEdit*>("metadataKeywordsEdit");
    QLineEdit* creatorEdit = dialog.findChild<QLineEdit*>("metadataCreatorEdit");

    QLabel* producerLabel = dialog.findChild<QLabel*>("metadataProducerLabel");
    QLabel* creationDateLabel = dialog.findChild<QLabel*>("metadataCreationDateLabel");
    QLabel* modDateLabel = dialog.findChild<QLabel*>("metadataModDateLabel");

    QDialogButtonBox* buttonBox = dialog.findChild<QDialogButtonBox*>("metadataButtonBox");

    // Assert - all widgets should exist
    EXPECT_NE(titleEdit, nullptr) << "metadataTitleEdit not found";
    EXPECT_NE(authorEdit, nullptr) << "metadataAuthorEdit not found";
    EXPECT_NE(subjectEdit, nullptr) << "metadataSubjectEdit not found";
    EXPECT_NE(keywordsEdit, nullptr) << "metadataKeywordsEdit not found";
    EXPECT_NE(creatorEdit, nullptr) << "metadataCreatorEdit not found";

    EXPECT_NE(producerLabel, nullptr) << "metadataProducerLabel not found";
    EXPECT_NE(creationDateLabel, nullptr) << "metadataCreationDateLabel not found";
    EXPECT_NE(modDateLabel, nullptr) << "metadataModDateLabel not found";

    EXPECT_NE(buttonBox, nullptr) << "metadataButtonBox not found";
}

TEST_F(MetadataDialogTest, SetMetadata_UpdatesEditFields) {
    // Arrange
    MetadataDialog dialog;
    DocumentMetadata metadata = createSampleMetadata();

    // Act
    dialog.setMetadata(metadata);

    // Assert - verify all editable fields
    QLineEdit* titleEdit = dialog.findChild<QLineEdit*>("metadataTitleEdit");
    QLineEdit* authorEdit = dialog.findChild<QLineEdit*>("metadataAuthorEdit");
    QLineEdit* subjectEdit = dialog.findChild<QLineEdit*>("metadataSubjectEdit");
    QLineEdit* keywordsEdit = dialog.findChild<QLineEdit*>("metadataKeywordsEdit");
    QLineEdit* creatorEdit = dialog.findChild<QLineEdit*>("metadataCreatorEdit");

    ASSERT_NE(titleEdit, nullptr);
    ASSERT_NE(authorEdit, nullptr);
    ASSERT_NE(subjectEdit, nullptr);
    ASSERT_NE(keywordsEdit, nullptr);
    ASSERT_NE(creatorEdit, nullptr);

    EXPECT_EQ(titleEdit->text().toStdString(), metadata.title);
    EXPECT_EQ(authorEdit->text().toStdString(), metadata.author);
    EXPECT_EQ(subjectEdit->text().toStdString(), metadata.subject);
    EXPECT_EQ(keywordsEdit->text().toStdString(), metadata.keywords);
    EXPECT_EQ(creatorEdit->text().toStdString(), metadata.creator);
}

TEST_F(MetadataDialogTest, SetMetadata_UpdatesReadOnlyLabels) {
    // Arrange
    MetadataDialog dialog;
    DocumentMetadata metadata = createSampleMetadata();

    // Act
    dialog.setMetadata(metadata);

    // Assert - verify all read-only label fields
    QLabel* producerLabel = dialog.findChild<QLabel*>("metadataProducerLabel");
    QLabel* creationDateLabel = dialog.findChild<QLabel*>("metadataCreationDateLabel");
    QLabel* modDateLabel = dialog.findChild<QLabel*>("metadataModDateLabel");

    ASSERT_NE(producerLabel, nullptr);
    ASSERT_NE(creationDateLabel, nullptr);
    ASSERT_NE(modDateLabel, nullptr);

    EXPECT_EQ(producerLabel->text().toStdString(), metadata.producer);
    EXPECT_EQ(creationDateLabel->text().toStdString(), metadata.creationDate);
    EXPECT_EQ(modDateLabel->text().toStdString(), metadata.modDate);
}

TEST_F(MetadataDialogTest, Metadata_Roundtrip) {
    // Arrange
    MetadataDialog dialog;
    DocumentMetadata originalMetadata = createSampleMetadata();

    // Act
    dialog.setMetadata(originalMetadata);
    DocumentMetadata retrievedMetadata = dialog.metadata();

    // Assert - verify all fields match
    EXPECT_EQ(retrievedMetadata.title, originalMetadata.title);
    EXPECT_EQ(retrievedMetadata.author, originalMetadata.author);
    EXPECT_EQ(retrievedMetadata.subject, originalMetadata.subject);
    EXPECT_EQ(retrievedMetadata.keywords, originalMetadata.keywords);
    EXPECT_EQ(retrievedMetadata.creator, originalMetadata.creator);
    EXPECT_EQ(retrievedMetadata.producer, originalMetadata.producer);
    EXPECT_EQ(retrievedMetadata.creationDate, originalMetadata.creationDate);
    EXPECT_EQ(retrievedMetadata.modDate, originalMetadata.modDate);
}

TEST_F(MetadataDialogTest, InitialState_EmptyFields) {
    // Arrange & Act
    MetadataDialog dialog;

    // Assert - verify all editable fields are empty
    QLineEdit* titleEdit = dialog.findChild<QLineEdit*>("metadataTitleEdit");
    QLineEdit* authorEdit = dialog.findChild<QLineEdit*>("metadataAuthorEdit");
    QLineEdit* subjectEdit = dialog.findChild<QLineEdit*>("metadataSubjectEdit");
    QLineEdit* keywordsEdit = dialog.findChild<QLineEdit*>("metadataKeywordsEdit");
    QLineEdit* creatorEdit = dialog.findChild<QLineEdit*>("metadataCreatorEdit");

    ASSERT_NE(titleEdit, nullptr);
    ASSERT_NE(authorEdit, nullptr);
    ASSERT_NE(subjectEdit, nullptr);
    ASSERT_NE(keywordsEdit, nullptr);
    ASSERT_NE(creatorEdit, nullptr);

    EXPECT_TRUE(titleEdit->text().isEmpty()) << "Title field should be empty initially";
    EXPECT_TRUE(authorEdit->text().isEmpty()) << "Author field should be empty initially";
    EXPECT_TRUE(subjectEdit->text().isEmpty()) << "Subject field should be empty initially";
    EXPECT_TRUE(keywordsEdit->text().isEmpty()) << "Keywords field should be empty initially";
    EXPECT_TRUE(creatorEdit->text().isEmpty()) << "Creator field should be empty initially";

    // Verify read-only labels are also empty
    QLabel* producerLabel = dialog.findChild<QLabel*>("metadataProducerLabel");
    QLabel* creationDateLabel = dialog.findChild<QLabel*>("metadataCreationDateLabel");
    QLabel* modDateLabel = dialog.findChild<QLabel*>("metadataModDateLabel");

    ASSERT_NE(producerLabel, nullptr);
    ASSERT_NE(creationDateLabel, nullptr);
    ASSERT_NE(modDateLabel, nullptr);

    EXPECT_TRUE(producerLabel->text().isEmpty()) << "Producer label should be empty initially";
    EXPECT_TRUE(creationDateLabel->text().isEmpty()) << "Creation date label should be empty initially";
    EXPECT_TRUE(modDateLabel->text().isEmpty()) << "Modification date label should be empty initially";
}

TEST_F(MetadataDialogTest, SetMetadata_EditField_ReadBack) {
    // Arrange
    MetadataDialog dialog;
    DocumentMetadata originalMetadata = createSampleMetadata();
    const std::string newTitle = "Modified Title by User";

    // Act
    dialog.setMetadata(originalMetadata);

    // Simulate user editing the title field
    QLineEdit* titleEdit = dialog.findChild<QLineEdit*>("metadataTitleEdit");
    ASSERT_NE(titleEdit, nullptr);
    titleEdit->setText(QString::fromStdString(newTitle));

    // Read back the metadata
    DocumentMetadata modifiedMetadata = dialog.metadata();

    // Assert - verify title was changed
    EXPECT_EQ(modifiedMetadata.title, newTitle);

    // Verify other fields remain unchanged
    EXPECT_EQ(modifiedMetadata.author, originalMetadata.author);
    EXPECT_EQ(modifiedMetadata.subject, originalMetadata.subject);
    EXPECT_EQ(modifiedMetadata.keywords, originalMetadata.keywords);
    EXPECT_EQ(modifiedMetadata.creator, originalMetadata.creator);

    // Read-only fields should retain original values
    EXPECT_EQ(modifiedMetadata.producer, originalMetadata.producer);
    EXPECT_EQ(modifiedMetadata.creationDate, originalMetadata.creationDate);
    EXPECT_EQ(modifiedMetadata.modDate, originalMetadata.modDate);
}
