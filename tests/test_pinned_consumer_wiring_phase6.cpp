// -*- coding: utf-8 -*-
// =============================================================================
// Linux port, phase 6 — the PinnedTempFile CONSUMERS actually seal and verify.
// =============================================================================
//
// tests/test_temp_file_safety_phase60.cpp proves seal() and verifyUnchanged()
// work as primitives. That is not the same as proving the product uses them:
// the defect class this project keeps logging is a hardened primitive beside a
// call site that never calls it. These cases drive the two real consumers —
// CombineFilesService::combine() and MainWindow::pasteImageFromClipboard() —
// and tamper with the staged file through each one's test seam, which fires in
// exactly the window a same-user attacker would use: after the seal, before
// the by-path re-read.
//
// What would fail if the wiring were deleted:
//   * remove verifyUnchanged() from combine()/paste -> the POSIX tamper cases
//     succeed instead of being refused (the substitutes are VALID files, so
//     nothing else refuses them);
//   * remove seal() from either -> the seal-failure cases succeed instead of
//     being refused, and on POSIX verifyUnchanged() refuses EVERY combine/paste
//     (unsealed files never verify), failing the untampered cases.
//
// The tamper cases are POSIX-only by #if: on Windows the held share mode makes
// the tamper itself impossible, which test_temp_file_safety_phase60 and
// test_clipboard_paste_substitution_phase60 already assert. The untampered and
// seal-failure cases run on every platform.
#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/TempFileSafety.h"
#include "ui/CombineFilesService.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QImage>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using pdfe::ui::CombineFilesService;
using pdfe::ui::MainWindow;

namespace {

fs::path fixture(const char* name) { return fs::path(TEST_FIXTURES_DIR) / name; }

fs::path outPath(const std::string& tag)
{
    std::error_code ec;
#if defined(_WIN32)
    const auto pid = std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
    const auto pid = std::to_string(::getpid());
#endif
    return fs::temp_directory_path(ec) / ("pdfe_p6_wiring_" + tag + "_" + pid + ".pdf");
}

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// Resets every seam this file sets, however the test exits.
struct SeamReset {
    ~SeamReset()
    {
        CombineFilesService::setSealedWrapperHookForTesting({});
        MainWindow::setPasteSealedHookForTesting({});
        pdfe::core::testseam::setPinnedSealFailureForTesting(0);
    }
};

int pageCount(const std::shared_ptr<pdfe::core::MuPdfContext>& ctx, const fs::path& p)
{
    auto d = pdfe::core::Document::open(ctx, p);
    return d ? d->pageCount() : -1;
}

#if !defined(_WIN32)
/// Same-user attacker #1: rename a VALID substitute over the sealed file.
void renameOver(const fs::path& victim, const std::string& bytes)
{
    const fs::path decoy = victim.parent_path() / (victim.filename().string() + ".decoy");
    {
        std::ofstream f(decoy, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_EQ(::rename(decoy.c_str(), victim.c_str()), 0) << "errno " << errno;
}

/// Same-user attacker #2: chmod back, rewrite in place, restore 0400.
void rewriteInPlace(const fs::path& victim, const std::string& bytes)
{
    ASSERT_EQ(::chmod(victim.c_str(), 0600), 0);
    {
        std::ofstream f(victim, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.good());
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_EQ(::chmod(victim.c_str(), 0400), 0);
}
#endif

bool putImageOnClipboard(const QImage& img)
{
    QClipboard* cb = QApplication::clipboard();
    if (!cb) return false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        cb->setImage(img);
        QApplication::processEvents();
        if (!cb->image().isNull()) return true;
    }
    return false;
}

QImage pasteImage()
{
    QImage img(24, 16, QImage::Format_RGB32);
    img.fill(qRgb(0x21, 0x8B, 0xE5));
    return img;
}

std::string attackerPng()
{
    QImage evil(24, 16, QImage::Format_RGB32);
    evil.fill(qRgb(0, 0, 0));
    QByteArray bytes;
    {
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        evil.save(&buf, "PNG");
    }
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

bool undoEnabled(MainWindow& w)
{
    auto* a = w.findChild<QAction*>("undoAction");
    return a != nullptr && a->isEnabled();
}

}  // namespace

// =============================================================================
// CombineFilesService
// =============================================================================

class CombineSealVerifyPhase6 : public ::testing::Test {
protected:
    std::shared_ptr<pdfe::core::MuPdfContext> ctx =
        std::make_shared<pdfe::core::MuPdfContext>();
};

TEST_F(CombineSealVerifyPhase6, UntouchedSealedWrapperCombines)
{
    SeamReset reset;
    const fs::path pdf = fixture("test_3page.pdf");
    const fs::path img = fixture("test_image_rgb.png");
    ASSERT_TRUE(fs::exists(pdf) && fs::exists(img));
    int fired = 0;
    bool sealedWhenFired = false;
    CombineFilesService::setSealedWrapperHookForTesting([&](const fs::path& p) {
        ++fired;
#if !defined(_WIN32)
        struct stat st {};
        sealedWhenFired = ::stat(p.c_str(), &st) == 0 && (st.st_mode & 07777) == 0400;
#else
        sealedWhenFired = fs::exists(p);
#endif
    });

    CombineFilesService svc(ctx);
    const fs::path out = outPath("untouched");
    const auto r = svc.combine({pdf, img}, out);
    ASSERT_TRUE(r.ok) << r.errorMessage;
    EXPECT_EQ(fired, 1) << "the seam must fire once per image wrapper";
    EXPECT_TRUE(sealedWhenFired)
        << "the wrapper was not sealed (0400 on POSIX) when the seam fired";
    EXPECT_EQ(r.outputPageCount, pageCount(ctx, pdf) + 1);
    std::error_code ec;
    fs::remove(out, ec);
}

TEST_F(CombineSealVerifyPhase6, SealFailureIsReportedAsAConversionFailure)
{
    SeamReset reset;
    const fs::path pdf = fixture("test_3page.pdf");
    const fs::path img = fixture("test_image_rgb.png");
    int fired = 0;
    CombineFilesService::setSealedWrapperHookForTesting([&](const fs::path&) { ++fired; });
    pdfe::core::testseam::setPinnedSealFailureForTesting(EIO);

    CombineFilesService svc(ctx);
    const fs::path out = outPath("sealfail");
    const auto r = svc.combine({pdf, img}, out);
    EXPECT_FALSE(r.ok) << "a wrapper that could not be sealed was combined anyway";
    EXPECT_NE(r.errorMessage.find("Failed to wrap image as PDF"), std::string::npos)
        << r.errorMessage;
    EXPECT_FALSE(r.userMessage.isEmpty());
    EXPECT_EQ(fired, 0) << "the post-seal seam fired although the seal failed";
    EXPECT_FALSE(fs::exists(out));
}

#if !defined(_WIN32)
TEST_F(CombineSealVerifyPhase6, WrapperRenamedOverAfterSealIsRefusedPosix)
{
    // Image FIRST, so the refusal exercised is the base-document check.
    SeamReset reset;
    const fs::path pdf = fixture("test_3page.pdf");
    const fs::path img = fixture("test_image_rgb.png");
    const std::string substitute = readAll(pdf);  // a VALID 3-page PDF
    ASSERT_FALSE(substitute.empty());
    int fired = 0;
    CombineFilesService::setSealedWrapperHookForTesting([&](const fs::path& p) {
        ++fired;
        renameOver(p, substitute);
    });

    CombineFilesService svc(ctx);
    const fs::path out = outPath("renamed");
    const auto r = svc.combine({img, pdf}, out);
    ASSERT_EQ(fired, 1);
    EXPECT_FALSE(r.ok)
        << "a wrapper renamed over after its seal was merged: the attacker's "
           "pages are in the output (" << r.outputPageCount << " pages)";
    EXPECT_NE(r.errorMessage.find("changed before it was read"), std::string::npos)
        << r.errorMessage;
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(CombineSealVerifyPhase6, WrapperRewrittenInPlaceAfterSealIsRefusedPosix)
{
    // Image SECOND, so the refusal exercised is the grafted-input check.
    SeamReset reset;
    const fs::path pdf = fixture("test_3page.pdf");
    const fs::path img = fixture("test_image_rgb.png");
    const std::string substitute = readAll(pdf);
    int fired = 0;
    CombineFilesService::setSealedWrapperHookForTesting([&](const fs::path& p) {
        ++fired;
        rewriteInPlace(p, substitute);
    });

    CombineFilesService svc(ctx);
    const fs::path out = outPath("inplace");
    const auto r = svc.combine({pdf, img}, out);
    ASSERT_EQ(fired, 1);
    EXPECT_FALSE(r.ok)
        << "a wrapper rewritten in place after its seal was merged ("
        << r.outputPageCount << " pages)";
    EXPECT_NE(r.errorMessage.find("changed before it was read"), std::string::npos)
        << r.errorMessage;
    EXPECT_FALSE(fs::exists(out));
}
#endif

// =============================================================================
// MainWindow::pasteImageFromClipboard
// =============================================================================

TEST(PasteSealVerifyPhase6, UntouchedSealedPasteSucceeds)
{
    SeamReset reset;
    MainWindow w;
    w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
    QApplication::processEvents();
    ASSERT_TRUE(putImageOnClipboard(pasteImage()));
    int fired = 0;
    bool sealedWhenFired = false;
    MainWindow::setPasteSealedHookForTesting([&](const fs::path& p) {
        ++fired;
#if !defined(_WIN32)
        struct stat st {};
        sealedWhenFired = ::stat(p.c_str(), &st) == 0 && (st.st_mode & 07777) == 0400;
#else
        sealedWhenFired = fs::exists(p);
#endif
    });
    const bool undoBefore = undoEnabled(w);
    EXPECT_TRUE(w.pasteImageFromClipboard()) << "an untampered paste was refused";
    QApplication::processEvents();
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(sealedWhenFired) << "the PNG was not sealed when the seam fired";
    EXPECT_FALSE(undoBefore) << "precondition: a freshly opened document has no undo";
    EXPECT_TRUE(undoEnabled(w)) << "the paste did not reach the undo stack";
}

TEST(PasteSealVerifyPhase6, SealFailureRefusesThePaste)
{
    SeamReset reset;
    MainWindow w;
    w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
    QApplication::processEvents();
    ASSERT_TRUE(putImageOnClipboard(pasteImage()));
    int fired = 0;
    MainWindow::setPasteSealedHookForTesting([&](const fs::path&) { ++fired; });
    pdfe::core::testseam::setPinnedSealFailureForTesting(EIO);
    EXPECT_FALSE(w.pasteImageFromClipboard())
        << "a PNG that could not be sealed was inserted anyway";
    QApplication::processEvents();
    EXPECT_EQ(fired, 0);
    EXPECT_FALSE(undoEnabled(w)) << "a refused paste still pushed an undo entry";
}

#if !defined(_WIN32)
TEST(PasteSealVerifyPhase6, PngRenamedOverAfterSealRefusesThePastePosix)
{
    SeamReset reset;
    MainWindow w;
    w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
    QApplication::processEvents();
    ASSERT_TRUE(putImageOnClipboard(pasteImage()));
    const std::string evil = attackerPng();
    ASSERT_FALSE(evil.empty());
    int fired = 0;
    MainWindow::setPasteSealedHookForTesting([&](const fs::path& p) {
        ++fired;
        renameOver(p, evil);
    });
    EXPECT_FALSE(w.pasteImageFromClipboard())
        << "a PNG renamed over after its seal was inserted into the document";
    QApplication::processEvents();
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(undoEnabled(w)) << "the substituted image reached the undo stack";
}

TEST(PasteSealVerifyPhase6, PngRewrittenInPlaceAfterSealRefusesThePastePosix)
{
    SeamReset reset;
    MainWindow w;
    w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
    QApplication::processEvents();
    ASSERT_TRUE(putImageOnClipboard(pasteImage()));
    const std::string evil = attackerPng();
    int fired = 0;
    MainWindow::setPasteSealedHookForTesting([&](const fs::path& p) {
        ++fired;
        rewriteInPlace(p, evil);
    });
    EXPECT_FALSE(w.pasteImageFromClipboard())
        << "a PNG rewritten in place after its seal was inserted";
    QApplication::processEvents();
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(undoEnabled(w));
}
#endif
