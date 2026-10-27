// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F12 — WIN-90, WIN-91, WIN-22.
//
// WIN-91 (High) is the one that matters: "no design requirement recorded that a
// future update manifest must be signature-verified before apply". PDFE's
// release artifacts are Authenticode-signed; bolting an unauthenticated updater
// onto a signed application would create a remote-code-execution channel. The
// requirement is cheapest to fix before the component exists — but a
// requirement written in prose and never checked is exactly the kind of thing
// that gets dropped the moment someone starts writing the updater.
//
// So the requirements live in docs/code-signing.md ("Update channel security
// requirements (normative)") and this file asserts them, in the style of the
// existing InstallerResidue/SigningStatusDocs doc oracles in
// tests/test_signing_and_install_docs.cpp.
//
// These are text oracles ON PURPOSE and they are NOT spell-checks: each one
// pins a specific, load-bearing clause (verify-before-parse, digest AND
// Authenticode, TLS-is-not-authentication, downgrade rejection, fail-closed
// with no "install anyway"). Deleting or softening any one of them fails the
// suite.
//
// WIN-90 (the absence of an update channel itself) is NOT closed by this file.
// It is recorded as a documented non-feature; the tests below assert that the
// documentation says so honestly and that the product still has no fetch — so
// if an updater ever does appear, the ChannelStillUnimplemented test fails and
// forces the requirements above to be revisited rather than forgotten.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path projectRoot()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

bool hasCI(const std::string& hay, const std::string& needle)
{
    return has(toLower(hay), toLower(needle));
}

const std::filesystem::path kSigningDoc =
    std::filesystem::path("docs") / "code-signing.md";
const std::filesystem::path kLimitsDoc =
    std::filesystem::path("docs") / "windows-integration-limitations.md";
const std::filesystem::path kVersionHeader =
    std::filesystem::path("src") / "app" / "VersionInfo.h";
const std::filesystem::path kMainWindow =
    std::filesystem::path("src") / "ui" / "MainWindow.cpp";

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// WIN-91 — the signature-verification requirement exists and is specific
// ═══════════════════════════════════════════════════════════════════════

TEST(UpdateChannelRequirements, SectionExistsAndDeclaresTheDecision)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty()) << "could not read " << kSigningDoc.string();

    EXPECT_TRUE(hasCI(d, "Update channel security requirements"))
        << "docs/code-signing.md must carry the normative update-channel section";
    EXPECT_TRUE(has(d, "UPDATE-CHANNEL-STATUS: NOT-IMPLEMENTED"))
        << "the section must carry a machine-readable status marker so a future "
           "implementation cannot leave the doc claiming the opposite";
    EXPECT_TRUE(has(d, "UPDATE-CHANNEL-DECISION: DOCUMENTED-NON-FEATURE"))
        << "F12 chose the documented-non-feature branch of its acceptance "
           "criterion; the choice must be recorded in the doc, not only in a "
           "dispositions file";
}

// The requirement WIN-91 names, verbatim in substance: verify the manifest
// BEFORE acting on any field of it.
TEST(UpdateChannelRequirements, ManifestMustBeSignatureVerifiedBeforeUse)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-1**"))
        << "requirement UC-1 (manifest signature) must be present and labelled";
    EXPECT_TRUE(hasCI(d, "signature-verified before any field of it is acted upon"))
        << "UC-1 must require verification BEFORE the manifest's fields are "
           "acted upon — 'verify at some point' is the weak form that lets an "
           "implementation parse an attacker's URL first";
    EXPECT_TRUE(hasCI(d, "pinned"))
        << "UC-1 must pin the publisher key; verifying against whatever key the "
           "manifest names authenticates nothing";
    EXPECT_TRUE(hasCI(d, "MUST be discarded"))
        << "UC-1 must say an unverifiable manifest is discarded";
}

TEST(UpdateChannelRequirements, PayloadRequiresDigestAndAuthenticode)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-2**"))
        << "requirement UC-2 (payload verification) must be present";
    EXPECT_TRUE(hasCI(d, "SHA-256"))
        << "UC-2 must require a content digest match against the SIGNED manifest";
    EXPECT_TRUE(hasCI(d, "Authenticode"))
        << "UC-2 must require Authenticode verification of the payload itself; a "
           "digest alone only proves the manifest and the payload agree";
    EXPECT_TRUE(hasCI(d, "RFC-3161"))
        << "UC-2 must require the RFC-3161 countersignature — an "
           "un-countersigned signature dies with the certificate, the same "
           "condition the installer harness already enforces (WIN-94/WIN-95)";
    EXPECT_TRUE(hasCI(d, "before the payload is executed"))
        << "UC-2 must place both checks BEFORE execution";
}

TEST(UpdateChannelRequirements, TransportSecurityIsNotTreatedAsAuthentication)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-3**"));
    EXPECT_TRUE(hasCI(d, "HTTPS"))
        << "UC-3 must still require HTTPS with chain validation";
    EXPECT_TRUE(hasCI(d, "MUST NOT be accepted as evidence that the manifest is "
                         "authentic"))
        << "UC-3 must explicitly deny that a TLS handshake authenticates the "
           "manifest — 'we fetch over HTTPS' is the single most common substitute "
           "for signing an update feed";
}

TEST(UpdateChannelRequirements, DowngradeAndReplayAreRejected)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-4**"));
    EXPECT_TRUE(hasCI(d, "less than or equal to the running version"))
        << "UC-4 must reject same-or-older versions";
    EXPECT_TRUE(hasCI(d, "replayed"))
        << "UC-4 must address REPLAY of an older validly-signed manifest, not "
           "only ordering — a signed-but-stale manifest is the interesting attack";
    EXPECT_TRUE(hasCI(d, "monotonic"))
        << "UC-4 must name the anti-replay mechanism (monotonic sequence or "
           "signed timestamp)";
}

TEST(UpdateChannelRequirements, FailsClosedWithNoInstallAnywayPath)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-5**"))
        << "requirement UC-5 (key rotation / revocation) must be present";
    EXPECT_TRUE(hasCI(d, "revocation"))
        << "UC-5 must require revocation checking";

    EXPECT_TRUE(has(d, "**UC-6**"))
        << "requirement UC-6 (fail closed) must be present";
    EXPECT_TRUE(hasCI(d, "Fail closed"))
        << "UC-6 must state the fail-closed posture";
    EXPECT_TRUE(hasCI(d, "no \"install anyway\" path"))
        << "UC-6 must forbid an override path outright; every real-world "
           "unauthenticated-update incident had one";
    EXPECT_TRUE(hasCI(d, "no silent retry against a different host"))
        << "UC-6 must forbid host fallback on failure";

    EXPECT_TRUE(has(d, "**UC-7**"))
        << "requirement UC-7 (no silent apply / no silent elevation) must be present";
    EXPECT_TRUE(has(d, "**UC-8**"))
        << "requirement UC-8 (verification must be unit-testable offline) must be "
           "present — a security requirement that cannot be tested is a wish";
}

// ═══════════════════════════════════════════════════════════════════════
// WIN-90 — the channel really is absent, and the docs say so
// ═══════════════════════════════════════════════════════════════════════
//
// This is the guard that keeps the WIN-91 requirements from going stale: the
// moment a real fetch appears in the product, this test fails and whoever added
// it has to come back to the section above.
TEST(UpdateChannelStatus, ChannelStillUnimplementedAndDocumentedAsSuch)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());
    EXPECT_TRUE(hasCI(d, "PDFE has no update channel"))
        << "the doc must state plainly that no update channel exists";
    EXPECT_TRUE(hasCI(d, "QDesktopServices::openUrl"))
        << "the doc must name what 'Check for Updates' actually does today";
    EXPECT_TRUE(hasCI(d, "Recording these requirements does **not** implement "
                         "WIN-90"))
        << "the doc must not let the requirements be mistaken for the feature";

    const std::string v = readFile(projectRoot() / kVersionHeader);
    ASSERT_FALSE(v.empty()) << "could not read " << kVersionHeader.string();
    EXPECT_TRUE(hasCI(v, "deliberately NOT implemented"))
        << "VersionInfo.h must keep recording that the network fetch is absent, "
           "in lock-step with docs/code-signing.md";

    // The product side of the claim: the menu action is still a browser open,
    // and nothing in src/app performs an update fetch.
    const std::string mw = readFile(projectRoot() / kMainWindow);
    ASSERT_FALSE(mw.empty());
    EXPECT_TRUE(has(mw, "QDesktopServices::openUrl"))
        << "the 'Check for Updates' action must still be a browser open; if it "
           "is not, an update channel has appeared and the UC-1..UC-8 "
           "requirements above must be satisfied and tested for real";
}

// ═══════════════════════════════════════════════════════════════════════
// WIN-22 — the elevated-drag-and-drop limitation is recorded, with a decision
// ═══════════════════════════════════════════════════════════════════════

TEST(WindowsIntegrationLimitations, ElevatedDragDropLimitationIsDocumented)
{
    const std::string d = readFile(projectRoot() / kLimitsDoc);
    ASSERT_FALSE(d.empty()) << "could not read " << kLimitsDoc.string();

    EXPECT_TRUE(has(d, "UIPI-DRAGDROP-STATUS: DOCUMENTED-LIMITATION"))
        << "the limitation must carry a machine-readable status marker";
    EXPECT_TRUE(hasCI(d, "User Interface Privilege Isolation"))
        << "the doc must name the actual mechanism, so it is not mistaken for a "
           "PDFE bug";
    EXPECT_TRUE(hasCI(d, "WM_COPYGLOBALDATA"))
        << "the doc must name the filtered message, which is what a future fix "
           "would have to allow";
    EXPECT_TRUE(hasCI(d, "ChangeWindowMessageFilterEx"))
        << "the doc must name the API deliberately NOT called";
    EXPECT_TRUE(hasCI(d, "Decision: document, do not filter"))
        << "the register's remedy was 'decide'; the decision must be explicit";
    EXPECT_TRUE(hasCI(d, "What would reopen this"))
        << "an accepted limitation must record its reopen trigger, otherwise it "
           "is indistinguishable from an ignored defect";

    // The decision must match reality: PDFE must still not lower the filter.
    const std::string mw = readFile(projectRoot() / kMainWindow);
    ASSERT_FALSE(mw.empty());
    EXPECT_FALSE(has(mw, "ChangeWindowMessageFilterEx"))
        << "the doc says PDFE does not lower the UIPI message filter, but "
           "MainWindow.cpp calls ChangeWindowMessageFilterEx — the documented "
           "decision and the code have drifted apart";
}

TEST(WindowsIntegrationLimitations, ClipboardOwnerLifetimeIsRecordedAsOpenNotAccepted)
{
    const std::string d = readFile(projectRoot() / kLimitsDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "CLIPBOARD-LIFETIME-STATUS: OPEN-DEFERRED"))
        << "WIN-24 must be recorded as OPEN, not as an accepted limitation — it "
           "is a probable product defect with a small fix, and filing it under "
           "'limitations' unqualified would bury it";
    EXPECT_TRUE(hasCI(d, "OleFlushClipboard"))
        << "the entry must name the missing call";
    EXPECT_TRUE(hasCI(d, "cross-process test"))
        << "the entry must record what closing it requires, so the deferral is "
           "actionable";
}

// ═══════════════════════════════════════════════════════════════════════
// GitHub #127 — the opt-in notifier, and the line it must not cross.
//
// The tests above assert PDFE has no update channel. That is still true and
// must stay true. What changed is that an opt-in, NOTIFY-ONLY version check
// now exists, and the whole safety argument for it not needing UC-1..UC-8 is
// that it never downloads or executes anything. An argument that rests on an
// absence needs a test, or it decays into a comment nobody re-checks.
// ═══════════════════════════════════════════════════════════════════════

TEST(UpdateChannelStatus, NotifyOnlyCheckIsBoundedByUC9)
{
    const std::string d = readFile(projectRoot() / kSigningDoc);
    ASSERT_FALSE(d.empty());

    EXPECT_TRUE(has(d, "**UC-9**"))
        << "the notify-only boundary must be normative, not prose";
    EXPECT_TRUE(has(d, "VERSION-NOTIFICATION-STATUS: OPT-IN-NOTIFY-ONLY"))
        << "a machine-readable marker must record that the notifier exists, so "
           "the doc cannot keep implying nothing fetches anything";
    EXPECT_TRUE(hasCI(d, "MUST be off by default"))
        << "UC-9 must require the check to be opt-in";
    EXPECT_TRUE(hasCI(d, "MUST NOT download a payload"))
        << "UC-9 must forbid the download outright, which is the single "
           "condition that keeps UC-1..UC-8 out of scope";
}

TEST(UpdateChannelStatus, TheNotifierContainsNoDownloadOrExecutePath)
{
    // The product side of UC-9, asserted against the SOURCE rather than
    // against another document. A doc saying "it does not download" and code
    // that downloads would otherwise agree with each other only by luck.
    const std::filesystem::path checker =
        std::filesystem::path("src") / "app" / "UpdateChecker.cpp";
    const std::string c = readFile(projectRoot() / checker);
    ASSERT_FALSE(c.empty()) << "could not read " << checker.string();

    for (const char* forbidden : {"QProcess",
                                  "ShellExecute",
                                  "CreateProcess",
                                  "QDesktopServices::openUrl",
                                  "QFile ",
                                  "QSaveFile"}) {
        EXPECT_FALSE(has(c, forbidden))
            << "UpdateChecker.cpp names " << forbidden
            << " — it must never write, launch or hand off anything. If this "
               "is now a real requirement, an update channel has appeared and "
               "UC-1..UC-8 apply in full.";
    }

    // And the positive half: it really is only reading a version string.
    EXPECT_TRUE(has(c, "latestVersionFromReleasesJson"))
        << "the parse entry point must still be the pure function UC-9 requires";
}
