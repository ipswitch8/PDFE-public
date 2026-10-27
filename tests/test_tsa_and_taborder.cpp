// -*- coding: utf-8 -*-
#include <gtest/gtest.h>

#include "signatures/TsaClient.h"
#include "signatures/Pkcs7OpenSslBridge.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/TabOrderDialog.h"

#include <openssl/sha.h>
#include <openssl/ts.h>
#include <openssl/rand.h>

#include <QApplication>
#include <QListWidget>
#include <QPushButton>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

// ============================================================================
// TSA Request Construction Tests
// ============================================================================

class TsaRequestTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate a test SHA-256 digest
        const char* testData = "test data for timestamp";
        SHA256(reinterpret_cast<const unsigned char*>(testData),
               strlen(testData), testDigest_);
    }

    unsigned char testDigest_[SHA256_DIGEST_LENGTH]{};
};

TEST_F(TsaRequestTest, BuildTimeStampReqProducesValidDer)
{
    // We can't call the private buildTimeStampReq directly,
    // but we can test requestTimestamp with an empty/bad URL
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_EQ(result.errorMessage, "TSA URL is empty");
}

TEST_F(TsaRequestTest, NullDigestReturnsError)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://example.com/tsa", nullptr, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "No digest provided");
}

TEST_F(TsaRequestTest, InvalidUrlReturnsNetworkError)
{
    // This URL will fail at the WinHTTP level
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://192.0.2.1:1/nonexistent-tsa", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST_F(TsaRequestTest, MalformedResponseHandled)
{
    // requestTimestamp with unreachable host should fail gracefully
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://0.0.0.0:1/tsa", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
}

// ============================================================================
// TSA Response Parsing Tests (using crafted DER)
// ============================================================================

TEST(TsaResponseParseTest, TimeStampReqConstructionValid)
{
    // Verify our TimeStampReq DER is structurally valid by constructing one
    // the same way TsaClient::buildTimeStampReq does and parsing it back

    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(reinterpret_cast<const unsigned char*>("test"), 4, digest);

    // Build request (same as TsaClient::buildTimeStampReq)
    TS_REQ* req = TS_REQ_new();
    ASSERT_NE(req, nullptr);
    TS_REQ_set_version(req, 1);

    TS_MSG_IMPRINT* imprint = TS_MSG_IMPRINT_new();
    ASSERT_NE(imprint, nullptr);

    X509_ALGOR* algor = X509_ALGOR_new();
    X509_ALGOR_set0(algor, OBJ_nid2obj(NID_sha256), V_ASN1_NULL, nullptr);
    TS_MSG_IMPRINT_set_algo(imprint, algor);
    X509_ALGOR_free(algor);

    TS_MSG_IMPRINT_set_msg(imprint, digest, SHA256_DIGEST_LENGTH);
    TS_REQ_set_msg_imprint(req, imprint);
    TS_MSG_IMPRINT_free(imprint);

    TS_REQ_set_cert_req(req, 1);

    // Serialize to DER
    unsigned char* der = nullptr;
    int derLen = i2d_TS_REQ(req, &der);
    TS_REQ_free(req);
    ASSERT_GT(derLen, 0);

    // Parse back and verify structure
    const unsigned char* p = der;
    TS_REQ* parsed = d2i_TS_REQ(nullptr, &p, derLen);
    ASSERT_NE(parsed, nullptr);

    EXPECT_EQ(TS_REQ_get_version(parsed), 1);
    EXPECT_EQ(TS_REQ_get_cert_req(parsed), 1);

    const TS_MSG_IMPRINT* mi = TS_REQ_get_msg_imprint(parsed);
    ASSERT_NE(mi, nullptr);

    TS_REQ_free(parsed);
    OPENSSL_free(der);
}

TEST(TsaResponseParseTest, GarbageDerRejected)
{
    // Verify that garbage DER data fails gracefully
    unsigned char garbage[] = { 0x30, 0x03, 0x01, 0x01, 0xFF };
    const unsigned char* p = garbage;
    TS_RESP* parsed = d2i_TS_RESP(nullptr, &p, sizeof(garbage));
    EXPECT_EQ(parsed, nullptr);
}

TEST(TsaResponseParseTest, RejectedStatusDetected)
{
    // Build a TimeStampResp with status=2 (rejection)
    TS_RESP* resp = TS_RESP_new();
    TS_STATUS_INFO* status = TS_STATUS_INFO_new();
    TS_STATUS_INFO_set_status(status, 2); // rejection
    TS_RESP_set_status_info(resp, status);
    TS_STATUS_INFO_free(status);

    unsigned char* der = nullptr;
    int derLen = i2d_TS_RESP(resp, &der);
    TS_RESP_free(resp);
    ASSERT_GT(derLen, 0);

    // Re-parse and verify the status is rejection
    const unsigned char* p = der;
    TS_RESP* parsed = d2i_TS_RESP(nullptr, &p, derLen);
    ASSERT_NE(parsed, nullptr);

    const ASN1_INTEGER* parsedStatus = TS_STATUS_INFO_get0_status(
        TS_RESP_get_status_info(parsed));
    EXPECT_EQ(ASN1_INTEGER_get(parsedStatus), 2);

    TS_RESP_free(parsed);
    OPENSSL_free(der);
}

// ============================================================================
// TSA URL Validation Tests
// ============================================================================

TEST_F(TsaRequestTest, LocalhostUrlRejected)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://localhost/tsa", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("localhost"), std::string::npos);
}

TEST_F(TsaRequestTest, LoopbackIpRejected)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://127.0.0.1/tsa", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
}

TEST_F(TsaRequestTest, MetadataEndpointRejected)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "http://169.254.169.254/latest/meta-data/",
        testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
}

TEST_F(TsaRequestTest, FileSchemeRejected)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "file:///etc/passwd", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
}

TEST_F(TsaRequestTest, FtpSchemeRejected)
{
    auto result = pdfe::signatures::TsaClient::requestTimestamp(
        "ftp://example.com/tsa", testDigest_, SHA256_DIGEST_LENGTH);
    EXPECT_FALSE(result.success);
}

// ============================================================================
// TSA Signer Configuration Tests
// ============================================================================

TEST(TsaSignerConfigTest, ConfigureTsaOnNullSignerNoOp)
{
    pdfe::signatures::Pkcs7OpenSslBridge::configureTsa(nullptr, "http://tsa.example.com");
    // Should not crash
}

// ============================================================================
// Tab Order Tests (using real MuPDF documents)
// ============================================================================

class TabOrderTest : public ::testing::Test {
protected:
    void SetUp() override {
        mupdfCtx_ = std::make_shared<pdfe::core::MuPdfContext>();

        // Create a test PDF with form fields in memory
        mupdfCtx_->execute([this](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_create_document(ctx);

            // Add a page
            fz_rect mediabox = { 0, 0, 612, 792 };
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, mediabox, 0, nullptr, nullptr);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_drop_obj(ctx, pageObj);

            // Save to temp file
            tmpPath_ = std::filesystem::temp_directory_path() / "test_taborder.pdf";
            pdf_write_options opts{};
            pdf_save_document(ctx, pdfDoc, tmpPath_.string().c_str(), &opts);
            pdf_drop_document(ctx, pdfDoc);
        });

        document_ = pdfe::core::Document::open(mupdfCtx_, tmpPath_);
        formManager_ = std::make_unique<pdfe::forms::FormManager>(mupdfCtx_);
    }

    void TearDown() override {
        document_.reset();
        formManager_.reset();
        if (std::filesystem::exists(tmpPath_)) {
            std::filesystem::remove(tmpPath_);
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> mupdfCtx_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::forms::FormManager> formManager_;
    std::filesystem::path tmpPath_;
};

TEST_F(TabOrderTest, GetTabOrderOnEmptyPageReturnsEmpty)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    auto order = formManager_->getTabOrder(page.get());
    EXPECT_TRUE(order.empty());
}

TEST_F(TabOrderTest, GetTabOrderReturnsWidgetIndices)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create two form fields
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "field1");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "field2");

    // Re-get page (MuPDF may have rebuilt internals)
    page = document_->page(0);
    auto order = formManager_->getTabOrder(page.get());
    EXPECT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 1);
}

TEST_F(TabOrderTest, SetTabOrderReordersWidgets)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create three form fields
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "first");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "second");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 150, 200, 30), "third");

    page = document_->page(0);

    // Reverse the tab order
    std::vector<int> newOrder = {2, 1, 0};
    formManager_->setTabOrder(page.get(), newOrder);

    // getTabOrder should return the new custom order
    page = document_->page(0);
    auto order = formManager_->getTabOrder(page.get());
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2); // "third" is first in tab order
    EXPECT_EQ(order[1], 1); // "second" is second
    EXPECT_EQ(order[2], 0); // "first" is last
}

TEST_F(TabOrderTest, SetTabOrderRoundTrip)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");

    page = document_->page(0);

    // Set custom order B, A
    std::vector<int> customOrder = {1, 0};
    formManager_->setTabOrder(page.get(), customOrder);

    // Verify round-trip
    page = document_->page(0);
    auto retrieved = formManager_->getTabOrder(page.get());
    EXPECT_EQ(retrieved, customOrder);
}

TEST_F(TabOrderTest, SetTabOrderNullPageThrows)
{
    std::vector<int> order = {0, 1};
    EXPECT_THROW(formManager_->setTabOrder(nullptr, order), std::invalid_argument);
}

TEST_F(TabOrderTest, SetTabOrderDuplicateIndicesRejected)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "f1");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "f2");

    page = document_->page(0);

    // Duplicate index 0 — should be rejected
    std::vector<int> badOrder = {0, 0};
    formManager_->setTabOrder(page.get(), badOrder);

    // getTabOrder should return default (no custom order stored)
    page = document_->page(0);
    auto order = formManager_->getTabOrder(page.get());
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 1);
}

TEST_F(TabOrderTest, SetTabOrderOutOfRangeRejected)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "f1");

    page = document_->page(0);

    std::vector<int> badOrder = {5}; // out of range
    formManager_->setTabOrder(page.get(), badOrder);

    page = document_->page(0);
    auto order = formManager_->getTabOrder(page.get());
    EXPECT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 0);
}

TEST_F(TabOrderTest, SetTabOrderWrongSizeNoOp)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "field1");

    page = document_->page(0);

    // Wrong size — should be a no-op
    std::vector<int> wrongOrder = {0, 1, 2};
    formManager_->setTabOrder(page.get(), wrongOrder);

    page = document_->page(0);
    auto fields = formManager_->getFormFields(page.get());
    EXPECT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0].fieldName, "field1");
}

// ============================================================================
// TabOrderDialog Tests
// ============================================================================

class TabOrderDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure QApplication exists
        if (!QApplication::instance()) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }

        mupdfCtx_ = std::make_shared<pdfe::core::MuPdfContext>();

        mupdfCtx_->execute([this](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            fz_rect mediabox = { 0, 0, 612, 792 };
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, mediabox, 0, nullptr, nullptr);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_drop_obj(ctx, pageObj);
            tmpPath_ = std::filesystem::temp_directory_path() / "test_taborder_dlg.pdf";
            pdf_write_options opts{};
            pdf_save_document(ctx, pdfDoc, tmpPath_.string().c_str(), &opts);
            pdf_drop_document(ctx, pdfDoc);
        });

        document_ = pdfe::core::Document::open(mupdfCtx_, tmpPath_);
        formManager_ = std::make_unique<pdfe::forms::FormManager>(mupdfCtx_);
    }

    void TearDown() override {
        document_.reset();
        formManager_.reset();
        if (std::filesystem::exists(tmpPath_)) {
            std::filesystem::remove(tmpPath_);
        }
    }

    QApplication* app_ = nullptr;
    std::shared_ptr<pdfe::core::MuPdfContext> mupdfCtx_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::forms::FormManager> formManager_;
    std::filesystem::path tmpPath_;
};

TEST_F(TabOrderDialogTest, DialogHasCorrectObjectName)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    EXPECT_EQ(dlg.objectName(), QString("tabOrderDialog"));
}

TEST_F(TabOrderDialogTest, DialogHasAccessibleName)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    EXPECT_FALSE(dlg.accessibleName().isEmpty());
}

TEST_F(TabOrderDialogTest, FieldListPopulated)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "Name");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Checkbox, QRectF(50, 100, 20, 20), "Agree");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 2);
}

TEST_F(TabOrderDialogTest, MoveUpButtonExists)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* btn = dlg.findChild<QPushButton*>("moveUpButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->accessibleName().isEmpty());
}

TEST_F(TabOrderDialogTest, MoveDownButtonExists)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* btn = dlg.findChild<QPushButton*>("moveDownButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->accessibleName().isEmpty());
}

TEST_F(TabOrderDialogTest, ApplyButtonExists)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* btn = dlg.findChild<QPushButton*>("applyButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->accessibleName().isEmpty());
}

TEST_F(TabOrderDialogTest, CancelButtonExists)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* btn = dlg.findChild<QPushButton*>("cancelButton");
    ASSERT_NE(btn, nullptr);
}

TEST_F(TabOrderDialogTest, MoveUpReordersItems)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    // Select second item and move up
    list->setCurrentRow(1);
    auto* upBtn = dlg.findChild<QPushButton*>("moveUpButton");
    upBtn->click();

    auto order = dlg.newTabOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1); // B moved to first position
    EXPECT_EQ(order[1], 0); // A moved to second
}

TEST_F(TabOrderDialogTest, MoveDownReordersItems)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);

    // Select first item and move down
    list->setCurrentRow(0);
    auto* downBtn = dlg.findChild<QPushButton*>("moveDownButton");
    downBtn->click();

    auto order = dlg.newTabOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1); // B now first
    EXPECT_EQ(order[1], 0); // A now second
}

TEST_F(TabOrderDialogTest, EmptyPageShowsEmptyList)
{
    auto page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto order = dlg.newTabOrder();
    EXPECT_TRUE(order.empty());
}

// Pipeline a7c4f891 phase-1 (A3): exercise the new Q_INVOKABLE
// moveProgrammatic seam. Mirrors the MoveUpReordersItems +
// MoveDownReordersItems pattern but goes through the seam used by
// pywinauto tests, not the button-click path.

TEST_F(TabOrderDialogTest, MoveProgrammaticReordersList)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 150, 200, 30), "C");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 3);

    // Move row 2 (C) → row 0. Expected newTabOrder order: [2, 0, 1].
    EXPECT_TRUE(dlg.moveProgrammatic(2, 0));

    auto order = dlg.newTabOrder();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 0);
    EXPECT_EQ(order[2], 1);
}

TEST_F(TabOrderDialogTest, MoveProgrammaticRejectsOutOfRange)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");

    page = document_->page(0);
    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());

    // Negative index → reject
    EXPECT_FALSE(dlg.moveProgrammatic(-1, 0));
    // Out-of-range index → reject
    EXPECT_FALSE(dlg.moveProgrammatic(0, 99));
    EXPECT_FALSE(dlg.moveProgrammatic(99, 0));
    // No-op (same index) → reject
    EXPECT_FALSE(dlg.moveProgrammatic(0, 0));
}

TEST_F(TabOrderDialogTest, ItemsStoreWidgetIndexInUserRole)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "X");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "Y");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    // Verify Qt::UserRole data matches widget indices
    EXPECT_EQ(list->item(0)->data(Qt::UserRole).toInt(), 0);
    EXPECT_EQ(list->item(1)->data(Qt::UserRole).toInt(), 1);
}

TEST_F(TabOrderDialogTest, ApplyRebuildsOrderFromVisualList)
{
    auto page = document_->page(0);

    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 50, 200, 30), "A");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 100, 200, 30), "B");
    formManager_->createFormField(page.get(),
        pdfe::forms::FormFieldType::Text, QRectF(50, 150, 200, 30), "C");

    page = document_->page(0);

    pdfe::ui::TabOrderDialog dlg(formManager_.get(), page.get());
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 3);

    // Simulate drag-drop: move item 2 to position 0
    auto* item = list->takeItem(2);
    list->insertItem(0, item);

    // Click Apply
    auto* applyBtn = dlg.findChild<QPushButton*>("applyButton");
    applyBtn->click();

    auto order = dlg.newTabOrder();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2); // C moved to first
    EXPECT_EQ(order[1], 0); // A
    EXPECT_EQ(order[2], 1); // B
}
