#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/XmpMetadataWriter.h"
#include "compliance/ComplianceTypes.h"
#include <filesystem>
#include <memory>
#include <string>
#include <algorithm>

using namespace pdfe::compliance;

class XmpMetadataWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(XmpMetadataWriterTest, ConstructDestroy) {
    XmpMetadataWriter writer(context_);
    SUCCEED();
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfA1b) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_1b,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("1"), std::string::npos);
    EXPECT_NE(packetStr.find("B"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfA2b) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_2b,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("2"), std::string::npos);
    EXPECT_NE(packetStr.find("B"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfA2u) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_2u,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("2"), std::string::npos);
    EXPECT_NE(packetStr.find("U"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfA2a) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_2a,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("2"), std::string::npos);
    EXPECT_NE(packetStr.find("A"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfA3b) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_3b,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("3"), std::string::npos);
    EXPECT_NE(packetStr.find("B"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_WithMetadata) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::PdfA_2b,
        PdfUALevel::None,
        "Test Document Title",
        "Test Author Name",
        "Test Creator App",
        "Test Producer",
        "2024-01-01T12:00:00Z",
        "2024-01-02T12:00:00Z"
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("Test Document Title"), std::string::npos);
    EXPECT_NE(packetStr.find("Test Author Name"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_PdfUA1) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::None,
        PdfUALevel::PdfUA_1,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_NE(packetStr.find("pdfuaid:part"), std::string::npos);
    EXPECT_NE(packetStr.find("1"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, BuildXmpPacket_None) {
    XmpMetadataWriter writer(context_);
    auto packet = writer.buildXmpPacket(
        PdfALevel::None,
        PdfUALevel::None,
        "", "", "", "", "", ""
    );

    ASSERT_FALSE(packet.empty());
    std::string packetStr(packet.begin(), packet.end());

    EXPECT_EQ(packetStr.find("pdfaid:part"), std::string::npos);
}

TEST_F(XmpMetadataWriterTest, WritePdfA_ThenRead) {
    XmpMetadataWriter writer(context_);

    writer.writePdfAIdentification(document_.get(), PdfALevel::PdfA_1b);

    auto tempPath = std::filesystem::temp_directory_path() / "test_xmp_roundtrip_write.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    std::string xmpPacket = writer.readXmpPacket(doc2.get());
    EXPECT_FALSE(xmpPacket.empty());
    EXPECT_NE(xmpPacket.find("pdfaid"), std::string::npos);

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(XmpMetadataWriterTest, WritePdfA_ThenDetect) {
    XmpMetadataWriter writer(context_);

    writer.writePdfAIdentification(document_.get(), PdfALevel::PdfA_2b);

    auto tempPath = std::filesystem::temp_directory_path() / "test_xmp_detect.pdf";
    document_->saveAs(tempPath);

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    PdfALevel detected = writer.detectPdfALevel(doc2.get());
    EXPECT_EQ(detected, PdfALevel::PdfA_2b);

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(XmpMetadataWriterTest, DetectPdfALevel_NoPdfA) {
    XmpMetadataWriter writer(context_);

    PdfALevel detected = writer.detectPdfALevel(document_.get());
    EXPECT_EQ(detected, PdfALevel::None);
}
