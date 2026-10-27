// -*- coding: utf-8 -*-
// Pipeline E phase-11 / Bucket 3i: combine many files wizard.
//
// Tests the CombineFilesService merge engine and the
// CombineFilesWizard UI plumbing. The tests target the AC's three
// minimum cases:
//   (1) combine 3 PDFs, verify output page count,
//   (2) combine 1 PDF + 1 image, verify page count,
//   (3) cancel at step 1 produces no output file.
// Plus a few extra coverage tests for edge cases.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "ui/CombineFilesService.h"
#include "ui/CombineFilesWizard.h"

#include <QApplication>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::ui;

namespace {

QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_combine_files_wizard";
        static char* argv[] = { const_cast<char*>(argv0), nullptr };
        return new QApplication(argc, argv);
    }
    return nullptr;
}

std::filesystem::path fixturePath(const char* name) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

std::filesystem::path tempOut(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
             / (std::string("pdfe_phase11_") + tag + "_"
                + std::to_string(std::time(nullptr)) + ".pdf");
    return p;
}

int countPages(std::shared_ptr<MuPdfContext> ctx,
               const std::filesystem::path& path) {
    auto d = Document::open(ctx, path);
    if (!d) return -1;
    return d->pageCount();
}

} // namespace

class CombineFilesWizardTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

// (1) Combine 3 PDFs — output page count == sum.
TEST_F(CombineFilesWizardTest, ThreePdfsPageCountSum) {
    auto a = fixturePath("test_3page.pdf");
    auto b = fixturePath("test_single.pdf");
    auto c = fixturePath("test_3page.pdf");
    if (!std::filesystem::exists(a) || !std::filesystem::exists(b)) {
        GTEST_SKIP() << "fixtures missing";
    }
    int pa = countPages(ctx, a), pb = countPages(ctx, b),
        pc = countPages(ctx, c);
    ASSERT_GT(pa, 0); ASSERT_GT(pb, 0); ASSERT_GT(pc, 0);

    CombineFilesService svc(ctx);
    auto out = tempOut("3pdf");
    auto r = svc.combine({a, b, c}, out);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.outputPageCount, pa + pb + pc);
    EXPECT_TRUE(std::filesystem::exists(out));
    EXPECT_EQ(countPages(ctx, out), pa + pb + pc);

    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// (2) Combine 1 PDF + 1 image — output page count == pdf_pages + 1.
TEST_F(CombineFilesWizardTest, PdfPlusImagePageCount) {
    auto pdfIn = fixturePath("test_3page.pdf");
    auto imgIn = fixturePath("test_image_rgb.png");
    if (!std::filesystem::exists(pdfIn) || !std::filesystem::exists(imgIn)) {
        GTEST_SKIP() << "fixtures missing";
    }
    int pdfPages = countPages(ctx, pdfIn);
    ASSERT_GT(pdfPages, 0);

    CombineFilesService svc(ctx);
    auto out = tempOut("pdf_img");
    auto r = svc.combine({pdfIn, imgIn}, out);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.outputPageCount, pdfPages + 1);
    EXPECT_EQ(countPages(ctx, out), pdfPages + 1);

    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// (3) Cancel at step 1: wizard rejected, no output written.
TEST_F(CombineFilesWizardTest, CancelAtStep1ProducesNoOutput) {
    auto a = fixturePath("test_3page.pdf");
    auto b = fixturePath("test_single.pdf");
    if (!std::filesystem::exists(a) || !std::filesystem::exists(b)) {
        GTEST_SKIP() << "fixtures missing";
    }
    auto out = tempOut("cancel");
    {
        CombineFilesWizard wiz(ctx);
        wiz.setInputFiles({a, b});
        wiz.setOutputPath(out);
        EXPECT_EQ(wiz.currentStepIndex(), 0);
        wiz.reject();  // cancel at step 1
    }
    EXPECT_FALSE(std::filesystem::exists(out));
}

// fewer than 2 inputs → rejected with errorMessage.
TEST_F(CombineFilesWizardTest, FewerThanTwoInputsRejected) {
    auto pdfIn = fixturePath("test_3page.pdf");
    if (!std::filesystem::exists(pdfIn)) GTEST_SKIP();
    CombineFilesService svc(ctx);
    auto out = tempOut("rej");
    auto r = svc.combine({pdfIn}, out);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errorMessage.empty());
    EXPECT_FALSE(std::filesystem::exists(out));
}

// Unsupported extension is rejected.
TEST_F(CombineFilesWizardTest, UnsupportedInputRejected) {
    auto pdfIn = fixturePath("test_3page.pdf");
    if (!std::filesystem::exists(pdfIn)) GTEST_SKIP();
    auto bogus = std::filesystem::temp_directory_path()
                 / "phase11_bogus.xyz";
    {
        std::ofstream f(bogus);
        f << "junk";
    }
    CombineFilesService svc(ctx);
    auto out = tempOut("unsupported");
    auto r = svc.combine({pdfIn, bogus}, out);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.errorMessage.find("Unsupported"), std::string::npos);
    std::error_code ec;
    std::filesystem::remove(bogus, ec);
}

// Wizard exposes named controls for automation.
TEST_F(CombineFilesWizardTest, WizardExposesNamedControls) {
    CombineFilesWizard wiz(ctx);
    EXPECT_EQ(wiz.objectName().toStdString(), "combineFilesWizard");
    EXPECT_NE(wiz.findChild<QObject*>("fileList"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("addFilesButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("removeFileButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("moveUpButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("moveDownButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("outputPathEdit"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("browseOutputButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("combineProgressBar"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("combineStatusLabel"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("backButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("nextButton"), nullptr);
    EXPECT_NE(wiz.findChild<QObject*>("cancelButton"), nullptr);
}

// Perf measurement (informational, not asserted in the unit test).
// Workload matches AC #8 exactly: 10 × 10-page PDFs.
TEST_F(CombineFilesWizardTest, TenTenPageCombineMeasurement) {
    auto src = fixturePath("test_10page.pdf");
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_10page.pdf not available";
    }
    std::vector<std::filesystem::path> inputs(10, src);
    CombineFilesService svc(ctx);
    auto out = tempOut("10x10");
    auto t0 = std::chrono::steady_clock::now();
    auto r = svc.combine(inputs, out);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(r.outputPageCount, 100);
    GTEST_LOG_(INFO) << "Combine 10 × 10-page elapsed: " << elapsed
                     << " ms (Debug build; <3s budget for AC is "
                     << "Release and is asserted by perf-benchmarks gate)";
    std::error_code ec;
    std::filesystem::remove(out, ec);
}
