// -*- coding: utf-8 -*-
//
// GitHub #370 — the signature field a sign flow CREATES must land at the
// geometry the user chose, not the hard-coded QRectF(50,50,200,60) that used
// to be the only possibility. SignDialog gained page/X/Y/Width/Height
// placement controls; this drives the equivalent seam path
// (SignFlowTestInputs::hasFieldGeometry/fieldRect/fieldPageIndex, consumed by
// MainWindow::promptSigningOptions exactly where dialog.signingOptions()
// geometry is) through the REAL MainWindow sign flow, end to end through the
// actual signed save.
//
// Oracle: the widget rect of the signed field, read back from the WRITTEN
// output file via a fresh SignatureManager::listSignatures — the same
// pdf_annot_rect fitz-space values CreateSignatureFieldCommand wrote.
// Pre-fix this test FAILS: no geometry plumbing existed, so the created
// field sat at 50,50,200,60 regardless of any requested placement.
//
// Modeled on test_ui_sign_cancel_rollback.cpp (fixtures, seam discipline,
// dummy test_signer.pfx whose passphrase "test123" is public test data).

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <memory>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class SignFieldGeometryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixturesDir() / "test_single.pdf";
        pfx_ = fixturesDir() / "test_signer.pfx";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf fixture not available";
        }
        if (!std::filesystem::exists(pfx_)) {
            GTEST_SKIP() << "test_signer.pfx fixture not available";
        }

        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "sign_geometry_work.pdf";
        std::filesystem::copy_file(src, workPdf_);

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
        mw_->openFile(QString::fromStdString(workPdf_.string()));
        QApplication::processEvents();
    }

    void TearDown() override
    {
        if (mw_) {
            mw_->setSignFlowTestInputsForTesting(std::nullopt);
        }
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::filesystem::path pfx_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// Inject a NON-default placement, complete the sign, and assert the field in
// the written document carries the injected rect.
TEST_F(SignFieldGeometryTest, CreatedFieldUsesInjectedGeometry)
{
    ASSERT_EQ(0, mw_->signatureFieldCountForTesting())
        << "fixture should start with no signature fields";

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "sign_geometry_signed.pdf";

    const QRectF wanted(120.0, 300.0, 180.0, 45.0);
    // PREMISE: the injected rect is NOT the hard-coded default, on any
    // component — otherwise a surviving hard-code could satisfy the oracle.
    const QRectF hardCoded(50.0, 50.0, 200.0, 60.0);
    ASSERT_GT(std::abs(wanted.x() - hardCoded.x()), 1.0);
    ASSERT_GT(std::abs(wanted.y() - hardCoded.y()), 1.0);
    ASSERT_GT(std::abs(wanted.width() - hardCoded.width()), 1.0);
    ASSERT_GT(std::abs(wanted.height() - hardCoded.height()), 1.0);

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.hasFieldGeometry = true;        // GitHub #370 seam
    inputs.fieldRect = wanted;
    inputs.fieldPageIndex = 0;
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    ASSERT_EQ(1, mw_->signatureFieldCountForTesting())
        << "the completed sign should leave exactly one signature field";
    ASSERT_TRUE(std::filesystem::exists(outPdf))
        << "sign flow did not write the signed output";

    // Independent oracle: a fresh manager over the written file.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto mgr = std::make_unique<pdfe::signatures::SignatureManager>(ctx);
    auto doc = pdfe::core::Document::open(ctx, outPdf);
    ASSERT_NE(doc, nullptr);
    const auto sigs = mgr->listSignatures(doc.get());
    ASSERT_EQ(1u, sigs.size());
    EXPECT_TRUE(sigs[0].isSigned);
    EXPECT_EQ(0, sigs[0].pageIndex);

    const double eps = 1.5;  // widget-rect round-trip tolerance in points
    EXPECT_NEAR(sigs[0].rect.x(), wanted.x(), eps)
        << "GitHub #370: the created field must sit at the requested X";
    EXPECT_NEAR(sigs[0].rect.y(), wanted.y(), eps)
        << "GitHub #370: the created field must sit at the requested Y";
    EXPECT_NEAR(sigs[0].rect.width(), wanted.width(), eps)
        << "GitHub #370: the created field must have the requested width";
    EXPECT_NEAR(sigs[0].rect.height(), wanted.height(), eps)
        << "GitHub #370: the created field must have the requested height";
}

// No geometry injected (hasFieldGeometry=false): the flow behaves exactly as
// before #370 — the default 50,50,200,60 placement — so the plumbing cannot
// have broken the legacy path.
TEST_F(SignFieldGeometryTest, DefaultGeometryUnchangedWhenNotInjected)
{
    ASSERT_EQ(0, mw_->signatureFieldCountForTesting());

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "sign_geometry_default.pdf";

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    // hasFieldGeometry left false deliberately.
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    ASSERT_TRUE(std::filesystem::exists(outPdf));
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto mgr = std::make_unique<pdfe::signatures::SignatureManager>(ctx);
    auto doc = pdfe::core::Document::open(ctx, outPdf);
    ASSERT_NE(doc, nullptr);
    const auto sigs = mgr->listSignatures(doc.get());
    ASSERT_EQ(1u, sigs.size());

    const double eps = 1.5;
    EXPECT_NEAR(sigs[0].rect.x(), 50.0, eps);
    EXPECT_NEAR(sigs[0].rect.y(), 50.0, eps);
    EXPECT_NEAR(sigs[0].rect.width(), 200.0, eps);
    EXPECT_NEAR(sigs[0].rect.height(), 60.0, eps);
}
