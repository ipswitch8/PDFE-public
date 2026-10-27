// -*- coding: utf-8 -*-
// GitHub #240 — can the LOW-INTEGRITY preflight child open a UNC path at all?
//
// WHY THIS MEASUREMENT COMES FIRST
//
// #240 asks to allow user-initiated opens of reachable network locations. The
// approved design is an off-thread reachability probe in front of the existing
// refusal. But MainWindow::openFile() calls isDocumentOpenSafe() immediately
// after that refusal, and that runs the open in a disposable child process
// created with a LOW-INTEGRITY token (src/core/PreflightOpen.cpp). A
// low-integrity process generally cannot authenticate to an SMB share.
//
// If that is true here, the whole feature is dead on arrival: the probe would
// say "reachable", and the preflight would then refuse the document anyway —
// with the hostile-document wording, which is exactly the misleading message
// GitHub #91 introduced the UI-level refusal to avoid. Every line built on top
// of the probe would be wasted, so this is measured BEFORE any of it is written.
//
// WHAT THIS TEST IS
//
// Not a guard on intended behaviour — an INSTRUMENT. It reports, via explicit
// assertions, whether the preflight reaches a UNC path. It is written so that
// whichever way the answer falls, the answer is recorded in the suite rather
// than in a note someone has to trust.
//
// It uses this machine's built-in administrative share (\\localhost\C$), which
// is a genuine SMB loopback — the redirector, the share and the authentication
// are all real. It SKIPS rather than fails when that share is not reachable
// (no Server service, not a member of Administrators, or a host where admin
// shares are disabled), because an environment without a share cannot answer
// the question either way, and a test that failed for that reason would be
// reporting the environment as a defect.
#include <gtest/gtest.h>

#include "core/PathSecurity.h"
#include "core/PreflightOpen.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// A minimal but genuinely openable PDF, written to `path`.
bool writeTinyPdf(const std::filesystem::path& path)
{
    static const char* kPdf =
        "%PDF-1.4\n"
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\n"
        "trailer<</Root 1 0 R>>\n";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(kPdf, static_cast<std::streamsize>(std::strlen(kPdf)));
    return out.good();
}

/// The UNC spelling of a local path via this machine's C$ admin share:
///   C:\Users\x\f.pdf  ->  \\localhost\C$\Users\x\f.pdf
QString uncViaAdminShare(const QString& localPath)
{
    const QString native = QDir::toNativeSeparators(localPath);
    if (native.size() < 3 || native[1] != QLatin1Char(':')) return {};
    return QStringLiteral("\\\\localhost\\") + native[0] + QStringLiteral("$") +
           native.mid(2);
}

class PreflightUncReachabilityTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        local_ = std::filesystem::temp_directory_path() /
                 "pdfe_p94_preflight_unc.pdf";
        ASSERT_TRUE(writeTinyPdf(local_)) << "could not stage the fixture";

        unc_ = uncViaAdminShare(QString::fromStdWString(local_.wstring()));
        if (unc_.isEmpty()) {
            GTEST_SKIP() << "temp dir is not on a drive-letter path; no admin "
                            "share spelling available";
        }
        // Reachability of the share, established WITHOUT the preflight so a
        // failure here cannot be mistaken for the thing under measurement.
        if (!QFileInfo(unc_).isFile()) {
            GTEST_SKIP() << "admin share not reachable at " << unc_.toStdString()
                         << " (Server service off, not an Administrators member, "
                            "or admin shares disabled) — this environment cannot "
                            "answer the question either way";
        }
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(local_, ec);
    }

    std::filesystem::path local_;
    QString unc_;
};

}  // namespace

// Control. If the preflight cannot clear the file LOCALLY, nothing about the
// UNC result would mean anything.
TEST_F(PreflightUncReachabilityTest, ThePreflightClearsTheSameFileLocally)
{
    EXPECT_TRUE(pdfe::core::isDocumentOpenSafe(local_))
        << "the preflight refused a well-formed local PDF, so this fixture "
           "cannot be used to measure the UNC case";
}

// The path is genuinely classified as remote — otherwise the UNC spelling is
// not exercising the code #240 is about.
TEST_F(PreflightUncReachabilityTest, TheUncSpellingIsClassifiedAsRemote)
{
    EXPECT_TRUE(pdfe::core::path_security::is_unc_or_remote(unc_.toStdWString()))
        << "the admin-share spelling was not seen as remote, so it does not "
           "exercise the gate under discussion";
}

// THE MEASUREMENT, pinned as a CANARY.
//
// Measured 2026-09-28 on this host: the preflight clears the fixture locally and
// does NOT clear the same file through its UNC spelling. The low-integrity token
// the child is created with cannot authenticate to the share.
//
// This asserts that CURRENT reality rather than the behaviour #240 wants,
// deliberately — a test that failed for the desired-but-absent behaviour would
// be permanently red and would poison every suite run and every gate.
//
// It is therefore a canary, and it is EXPECTED TO REDDEN when someone changes
// the preflight's isolation. If it does, that is the signal that GitHub #240's
// blocker has moved, not a regression: re-read this comment, flip the assertion,
// and get on with the probe. Until then it records, in the suite rather than in
// a note, why a reachability probe alone cannot deliver #240 — the probe would
// report "reachable" and the preflight would refuse the document anyway, with
// the hostile-document wording that #91 added the UI-level refusal to avoid.
TEST_F(PreflightUncReachabilityTest, TheLowIntegrityChildCannotReachAUncPathYet)
{
    const bool localOk = pdfe::core::isDocumentOpenSafe(local_);
    ASSERT_TRUE(localOk) << "control failed; the UNC result is uninterpretable";

    const bool uncOk = pdfe::core::isDocumentOpenSafe(
        std::filesystem::path(unc_.toStdWString()));

    EXPECT_FALSE(uncOk)
        << "The preflight NOW clears a UNC path (" << unc_.toStdString()
        << "). That is a CHANGE, not a failure: when this measurement was taken "
           "the low-integrity child could not authenticate to SMB, which is the "
           "blocker recorded against GitHub #240. If the preflight's isolation "
           "has deliberately changed, flip this assertion and the #240 "
           "reachability-probe design becomes viable. If it changed by accident, "
           "the crash-isolation control has been weakened and that is the bug.";
}
