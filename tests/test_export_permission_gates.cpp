// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F2 — gap PDF-21.
//
// `PlainTextExporter::write` has consulted `Document::copyAllowed()` since
// R2-10 and refuses (returns false, writes nothing) on a copy-denied document.
// `WordExporter`, `ExcelExporter` and `ImageExporter` did not: a copy-denied
// PDF could be exported to DOCX, RTF, XLSX, PNG or JPEG with one click and the
// content walked straight out. Three of the four exit doors were unlocked.
//
// One test per exporter asserting refusal, each paired with a positive control
// on a permission-granted document so an implementation that simply refused
// everything cannot pass. PlainTextExporter is NOT touched here — it already
// has its coverage in tests/test_permission_enforcement.cpp; it appears below
// only as the reference contract the other three are asserted to match.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "exports/ExcelExporter.h"
#include "exports/ImageExporter.h"
#include "exports/WordExporter.h"

#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::encryption;
using namespace pdfe::exports;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

/// Copy-denied variant of a fixture: empty user password (so it opens with no
/// prompt and at USER authority, which is where the restriction bites) and an
/// owner password with only the copy/extract permission cleared.
std::filesystem::path makeCopyDenied(const std::filesystem::path& dir,
                                     const std::string& name,
                                     const std::string& sourceFixture) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / sourceFixture);
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "";
    settings.ownerPassword = "ownerpw";
    settings.permissions = PermissionFlags::all();
    settings.permissions.copy = false;

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

/// Same fixture, same encryption, copy PERMITTED — the positive control.
std::filesystem::path makeCopyAllowedEncrypted(const std::filesystem::path& dir,
                                               const std::string& name,
                                               const std::string& sourceFixture) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / sourceFixture);
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "";
    settings.ownerPassword = "ownerpw";
    settings.permissions = PermissionFlags::all();

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

int countFilesIn(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) return 0;
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace

// ===========================================================================
// PDF-21 — WordExporter
// ===========================================================================

TEST(ExportPermissionGates, WordExporterRefusesCopyDenied) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyDenied(dir, "word_denied.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed()) << "fixture must actually deny copy";

    // DOCX
    {
        auto out = dir / "leak.docx";
        WordExporter exporter(doc.get());
        WordExportOptions opts;
        opts.format = WordFormat::Docx;
        auto r = exporter.exportTo(out, opts);
        EXPECT_FALSE(r.success)
            << "DOCX export of a copy-denied document must be refused";
        EXPECT_FALSE(r.errorMessage.empty())
            << "the refusal must say why";
        EXPECT_FALSE(std::filesystem::exists(out))
            << "copy-denied content must not be written to disk";
    }

    // RTF — the same exporter's other output format must be gated too.
    {
        auto out = dir / "leak.rtf";
        WordExporter exporter(doc.get());
        WordExportOptions opts;
        opts.format = WordFormat::Rtf;
        auto r = exporter.exportTo(out, opts);
        EXPECT_FALSE(r.success)
            << "RTF export of a copy-denied document must be refused";
        EXPECT_FALSE(std::filesystem::exists(out));
    }
}

TEST(ExportPermissionGates, WordExporterAllowsWhenPermitted) {
    // Over-refusal control: the SAME encryption, copy granted, must export.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyAllowedEncrypted(dir, "word_ok.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed());

    auto out = dir / "ok.docx";
    WordExporter exporter(doc.get());
    WordExportOptions opts;
    opts.format = WordFormat::Docx;
    auto r = exporter.exportTo(out, opts);
    EXPECT_TRUE(r.success) << "copy-permitted export must succeed: "
                           << r.errorMessage;
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ===========================================================================
// PDF-21 — ExcelExporter
// ===========================================================================

TEST(ExportPermissionGates, ExcelExporterRefusesCopyDenied) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyDenied(dir, "excel_denied.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed());

    auto out = dir / "leak.xlsx";
    ExcelExporter exporter(doc.get());
    ExcelExportOptions opts;
    auto r = exporter.exportTo(out, opts);
    EXPECT_FALSE(r.success)
        << "XLSX export of a copy-denied document must be refused";
    EXPECT_FALSE(r.errorMessage.empty());
    EXPECT_FALSE(std::filesystem::exists(out))
        << "copy-denied tabular content must not be written to disk";
    EXPECT_EQ(r.sheetsEmitted, 0)
        << "no sheet may be built from a copy-denied document";
}

TEST(ExportPermissionGates, ExcelExporterAllowsWhenPermitted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyAllowedEncrypted(dir, "excel_ok.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed());

    auto out = dir / "ok.xlsx";
    ExcelExporter exporter(doc.get());
    ExcelExportOptions opts;
    auto r = exporter.exportTo(out, opts);
    EXPECT_TRUE(r.success) << "copy-permitted export must succeed: "
                           << r.errorMessage;
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ===========================================================================
// PDF-21 — ImageExporter
// ===========================================================================

TEST(ExportPermissionGates, ImageExporterRefusesCopyDenied) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyDenied(dir, "image_denied.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed());

    const auto outDir = dir / "img_denied_out";
    ImageExporter exporter(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;
    auto r = exporter.exportToFolder(outDir, opts);

    EXPECT_EQ(r.successCount, 0)
        << "no page of a copy-denied document may be rasterised out";
    EXPECT_GT(r.errorCount, 0) << "the refusal must be reported";
    ASSERT_FALSE(r.fileResults.empty());
    EXPECT_FALSE(r.fileResults.front().errorMessage.empty())
        << "the refusal must say why";
    EXPECT_EQ(countFilesIn(outDir), 0)
        << "copy-denied page images must not be written to disk";
}

TEST(ExportPermissionGates, ImageExporterAllowsWhenPermitted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyAllowedEncrypted(dir, "image_ok.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed());

    const auto outDir = dir / "img_ok_out";
    ImageExporter exporter(doc.get());
    ImageExportOptions opts;
    opts.format = ImageFormat::Png;
    opts.dpi = 72;
    opts.explicitPages = {0};
    auto r = exporter.exportToFolder(outDir, opts);

    EXPECT_EQ(r.errorCount, 0);
    EXPECT_EQ(r.successCount, 1) << "copy-permitted export must produce a page";
    EXPECT_GT(countFilesIn(outDir), 0);
}

// ===========================================================================
// The four exporters must share ONE contract, not three-plus-one.
// ===========================================================================

TEST(ExportPermissionGates, AllThreeExportersMatchPlainTextRefusalContract) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto enc = makeCopyDenied(dir, "contract.pdf", "test_3page.pdf");

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed());

    // Reference behaviour (PlainTextExporter, unchanged by this phase):
    // refuse, write nothing.
    const bool wordRefused = !WordExporter(doc.get())
        .exportTo(dir / "c.docx", WordExportOptions{}).success;
    const bool excelRefused = !ExcelExporter(doc.get())
        .exportTo(dir / "c.xlsx", ExcelExportOptions{}).success;
    ImageExportOptions iopts;
    iopts.dpi = 72;
    const bool imageRefused =
        ImageExporter(doc.get()).exportToFolder(dir / "c_img", iopts).successCount == 0;

    EXPECT_TRUE(wordRefused);
    EXPECT_TRUE(excelRefused);
    EXPECT_TRUE(imageRefused);
    EXPECT_FALSE(std::filesystem::exists(dir / "c.docx"));
    EXPECT_FALSE(std::filesystem::exists(dir / "c.xlsx"));
    EXPECT_EQ(countFilesIn(dir / "c_img"), 0);
}
