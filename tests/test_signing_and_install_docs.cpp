// test_signing_and_install_docs.cpp
//
// Meta-audit follow-up F7 (gaps WIN-01..WIN-14, WIN-92..WIN-97).
//
// Two families of oracle, both text-based on purpose:
//
//   1. SigningStatusDocs  -- docs/security/unsigned-binary-risk-acceptance.md and
//      src/app/VersionInfo.h each carry a machine-readable SIGNING-STATUS marker.
//      The tests assert the two markers AGREE, that they name the commit that
//      changed the status, that no stale "unsigned" claim survives, and -- the
//      part that makes this more than a spell-check -- that the claimed status
//      MATCHES REALITY by scanning .github/workflows for a signing invocation.
//      Both directions fail: claiming manual-only while CI signs is a defect, and
//      so is claiming automated while nothing in CI signs. This is what stops the
//      two files drifting apart again the way they did between 92e1328 and 7903f37.
//
//   2. InstallerResidueHarness / InstallerSignatureHarness / InstallerScopeDocs --
//      structural guards on scripts/vm-install-residue-harness.ps1, its
//      expected-residue baseline, and the installer-scope dispositions. The
//      harness itself can only run on a disposable VM; these tests keep its
//      contract (five stages, CPack installer not an extracted payload, the three
//      upgrade assertions, Valid + RFC-3161) from being quietly weakened between
//      VM runs, and they assert the committed baseline-consistency result.
//
// These tests deliberately do NOT install, uninstall, launch anything, or touch
// the registry.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool containsCI(const std::string& hay, const std::string& needle) {
    return contains(toLower(hay), toLower(needle));
}

// Pull "<marker>: VALUE" out of a file, tolerating the HTML-comment form used in
// Markdown and the // form used in C++. VALUE is the rest of the token run.
std::string markerValue(const std::string& text, const std::string& marker) {
    std::regex re(marker + R"(\s*:\s*([A-Za-z0-9_.\-]+))");
    std::smatch m;
    if (std::regex_search(text, m, re)) return m[1].str();
    return {};
}

const std::filesystem::path kRiskDoc =
    std::filesystem::path("docs") / "security" / "unsigned-binary-risk-acceptance.md";
const std::filesystem::path kVersionHeader =
    std::filesystem::path("src") / "app" / "VersionInfo.h";
const std::filesystem::path kHarness =
    std::filesystem::path("scripts") / "vm-install-residue-harness.ps1";
const std::filesystem::path kBaseline =
    std::filesystem::path("scripts") / "installer-expected-residue-baseline.json";
const std::filesystem::path kChecker =
    std::filesystem::path("scripts") / "check-installer-residue-baseline.ps1";
const std::filesystem::path kConsistency =
    std::filesystem::path("docs") / "audit" / "meta-audit-2026-07" / "followup" /
    "phase-7" / "baseline-consistency.json";
const std::filesystem::path kIntentDoc =
    std::filesystem::path("docs") / "installer-residue-and-hkcu-intent.md";

// Does any workflow actually SIGN something? (Verification-only steps are not
// signing and are deliberately excluded -- see the risk-acceptance doc §1.)
std::vector<std::string> workflowsInvokingASigningTool() {
    std::vector<std::string> hits;
    const std::filesystem::path dir = projectRoot() / ".github" / "workflows";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return hits;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = toLower(e.path().extension().string());
        if (ext != ".yml" && ext != ".yaml") continue;
        const std::string body = readFile(e.path());
        for (const char* tok : {"signtool", "Sign-Artifacts", "Build-Release-Signed"}) {
            if (containsCI(body, tok)) {
                hits.push_back(e.path().filename().string() + " -> " + tok);
                break;
            }
        }
    }
    return hits;
}

QJsonObject readJsonObject(const std::filesystem::path& p) {
    const std::string raw = readFile(p);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw), &err);
    if (err.error != QJsonParseError::NoError) return {};
    return doc.object();
}

} // namespace

// ===========================================================================
// 1. Signing-status reconciliation (WIN-92, merges WIN-98 / SEC-92 / A5-55)
// ===========================================================================

// The two files that describe the signing posture must carry the SAME status
// token. Between 92e1328 (signing landed) and this change they disagreed with
// reality for six weeks and with each other by construction, because nothing
// compared them.
TEST(SigningStatusDocs, RiskAcceptanceAndVersionInfoAgree) {
    const std::string doc = readFile(projectRoot() / kRiskDoc);
    const std::string hdr = readFile(projectRoot() / kVersionHeader);
    ASSERT_FALSE(doc.empty()) << "cannot read " << kRiskDoc.string();
    ASSERT_FALSE(hdr.empty()) << "cannot read " << kVersionHeader.string();

    const std::string docStatus = markerValue(doc, "SIGNING-STATUS");
    const std::string hdrStatus = markerValue(hdr, "SIGNING-STATUS");

    ASSERT_FALSE(docStatus.empty())
        << kRiskDoc.string() << " must carry a machine-readable "
           "'SIGNING-STATUS: <token>' marker so the posture cannot be asserted "
           "only in prose";
    ASSERT_FALSE(hdrStatus.empty())
        << kVersionHeader.string() << " must carry the same "
           "'SIGNING-STATUS: <token>' marker";
    EXPECT_EQ(docStatus, hdrStatus)
        << "Signing status has drifted: " << kRiskDoc.string() << " says '"
        << docStatus << "' but " << kVersionHeader.string() << " says '"
        << hdrStatus << "'. Both describe the same fact and must be updated together.";
}

// The claimed status must match what CI actually does. This is the assertion
// that keeps the doc honest as the repository changes: wire signing into a
// workflow and the token must become SIGNED-AUTOMATED; remove it and it must go
// back. Neither direction can pass silently.
TEST(SigningStatusDocs, StatusMatchesWorkflowReality) {
    const std::string doc = readFile(projectRoot() / kRiskDoc);
    ASSERT_FALSE(doc.empty());
    const std::string status = markerValue(doc, "SIGNING-STATUS");
    ASSERT_FALSE(status.empty());

    const std::vector<std::string> signingWorkflows = workflowsInvokingASigningTool();

    if (signingWorkflows.empty()) {
        EXPECT_EQ(status, "SIGNED-MANUAL-ONLY")
            << "No workflow under .github/workflows invokes signtool, "
               "Sign-Artifacts.ps1 or Build-Release-Signed.ps1, so signing is a "
               "manual release step. The documented status must say so "
               "(SIGNED-MANUAL-ONLY), not claim an automated gate that does not exist.";
        EXPECT_TRUE(containsCI(doc, "manual"))
            << "the risk-acceptance doc must state in prose that signing is a "
               "manual release step, matching the SIGNED-MANUAL-ONLY token";
    } else {
        std::string joined;
        for (const auto& h : signingWorkflows) joined += h + "; ";
        EXPECT_EQ(status, "SIGNED-AUTOMATED")
            << "A workflow now signs release artifacts (" << joined
            << ") but the documented status is still '" << status
            << "'. Update docs/security/unsigned-binary-risk-acceptance.md and "
               "src/app/VersionInfo.h together.";
    }
}

// WIN-92's acceptance criterion: the risk-acceptance file must record the
// signing status AND the commit that changed it.
TEST(SigningStatusDocs, RecordsTheCommitThatChangedTheStatus) {
    const std::string doc = readFile(projectRoot() / kRiskDoc);
    const std::string hdr = readFile(projectRoot() / kVersionHeader);
    ASSERT_FALSE(doc.empty());
    ASSERT_FALSE(hdr.empty());

    const std::string docCommit = markerValue(doc, "SIGNING-STATUS-COMMIT");
    const std::string hdrCommit = markerValue(hdr, "SIGNING-STATUS-COMMIT");

    EXPECT_EQ(docCommit, "92e1328")
        << "the risk-acceptance doc must name the commit that changed the signing "
           "status (92e1328, 'release(sign): code-sign v0.1.0 as Itwerx Corp')";
    EXPECT_EQ(hdrCommit, docCommit)
        << "VersionInfo.h and the risk-acceptance doc must cite the same commit";

    // ... and the substance of that commit, not just its hash.
    EXPECT_TRUE(containsCI(doc, "Azure Trusted Signing"))
        << "the doc must name the signing mechanism actually used";
    EXPECT_TRUE(containsCI(doc, "RFC-3161"))
        << "the doc must record that signatures are RFC-3161 countersigned";
    EXPECT_TRUE(containsCI(doc, "Itwerx Corp"))
        << "the doc must record the publisher identity the artifacts are signed as";
}

// No stale "we are unsigned" claim may survive in either file.
TEST(SigningStatusDocs, NoStaleUnsignedClaimSurvives) {
    struct Target { std::filesystem::path path; };
    const std::vector<Target> targets{{kRiskDoc}, {kVersionHeader}};
    const std::vector<std::string> stalePhrases{
        "is currently unsigned",
        "not code-signed",
        "no Authenticode certificate has been provisioned",
    };
    for (const auto& t : targets) {
        const std::string body = toLower(readFile(projectRoot() / t.path));
        ASSERT_FALSE(body.empty()) << "cannot read " << t.path.string();
        for (const auto& phrase : stalePhrases) {
            EXPECT_FALSE(contains(body, toLower(phrase)))
                << t.path.string() << " still contains the stale claim \"" << phrase
                << "\". Release artifacts have been Authenticode-signed since 92e1328.";
        }
    }
}

// ===========================================================================
// 2. The disposable-VM harness contract (WIN-01/02/10/11/12/13/14)
// ===========================================================================

TEST(InstallerResidueHarness, ExistsAndHasAllFiveStages) {
    const std::string h = readFile(projectRoot() / kHarness);
    ASSERT_FALSE(h.empty()) << "missing harness: " << kHarness.string();

    // Stage 1: filesystem + registry snapshot of all three documented roots.
    EXPECT_TRUE(containsCI(h, "New-MachineSnapshot")) << "no snapshot stage";
    EXPECT_TRUE(containsCI(h, "Get-RegistrySnapshot")) << "no registry snapshot";
    EXPECT_TRUE(containsCI(h, "Get-FilesystemSnapshot")) << "no filesystem snapshot";
    EXPECT_TRUE(containsCI(h, "HKLM:\\Software"))
        << "the snapshot must cover HKLM\\Software";
    EXPECT_TRUE(containsCI(h, "HKCR:\\")) << "the snapshot must cover HKCR";
    EXPECT_TRUE(containsCI(h, "HKCU:\\Software\\PDFE"))
        << "the snapshot must cover HKCU\\Software\\PDFE (the preserved user-data hive)";

    // Stage 2/3/4.
    EXPECT_TRUE(containsCI(h, "Invoke-SilentInstall")) << "no silent-install stage";
    EXPECT_TRUE(containsCI(h, "Invoke-LaunchStage"))   << "no app-launch stage";
    EXPECT_TRUE(containsCI(h, "Invoke-Uninstall"))     << "no uninstall stage";

    // Stage 5 + a machine-readable report at a path named in the script.
    EXPECT_TRUE(containsCI(h, "Invoke-DiffStage")) << "no diff stage";
    EXPECT_TRUE(containsCI(h, "ConvertTo-Json"))
        << "the residue report must be machine-readable JSON";
    EXPECT_TRUE(containsCI(h, "build/vm-harness/residue-report.json"))
        << "the harness must name the residue-report output path";
}

// The whole point of the harness: it EXECUTES the CPack-produced installer.
// scripts/verify-installer-deps.ps1 extracts the payload with 7-Zip instead
// ("no admin / no real install") and that is precisely the limitation being
// fixed, so an extraction-based install step here would be a regression to it.
TEST(InstallerResidueHarness, RunsTheCPackInstallerNotAnExtractedPayload) {
    const std::string h = readFile(projectRoot() / kHarness);
    ASSERT_FALSE(h.empty());

    // GitHub #182: the packaged artifact is now built from the DEDICATED
    // packaging tree (build/vs2022-package, configured PDFE_TESTING=OFF), not
    // from build/vs2022 which the test suite configures with the seams ON.
    // This assertion pins the CPack output directory of the PACKAGING tree
    // specifically -- pointing the harness back at build/vs2022 would mean
    // residue-testing a binary that still carries the test-pipe control
    // channel, which is not the artifact users install.
    EXPECT_TRUE(contains(h, "build/vs2022-package/installer"))
        << "the harness must reference the PACKAGING tree's CPack output "
           "directory (CPACK_PACKAGE_DIRECTORY = ${CMAKE_BINARY_DIR}/installer "
           "for the windows-vs2022-package preset)";
    EXPECT_FALSE(contains(h, "build/vs2022/installer"))
        << "the harness still points at the TEST tree's installer directory; "
           "that binary is built with PDFE_TESTING=ON and is not what ships";
    EXPECT_TRUE(contains(h, "PDFE-*-win64.exe"))
        << "the harness must reference the CPack installer name";
    EXPECT_TRUE(contains(h, "Start-Process -FilePath $InstallerPath"))
        << "the harness must EXECUTE the installer";
    EXPECT_TRUE(contains(h, "'/S'"))
        << "the install must be silent (/S)";
    EXPECT_TRUE(contains(h, "/D=$Prefix"))
        << "the install must target an explicit prefix (/D=)";

    // No 7-Zip extraction may be used as the install step.
    EXPECT_FALSE(containsCI(h, "7z.exe"))
        << "the harness must not extract the payload with 7-Zip; it must run the "
           "installer";
    EXPECT_FALSE(containsCI(h, "-SevenZip"))
        << "the harness must not take a 7-Zip path; that is the extraction-based "
           "approach it replaces";
}

// WIN-03 / WIN-04 / WIN-93: the upgrade case and its three assertions.
TEST(InstallerResidueHarness, UpgradeCaseAssertsAssociationSentinelAndDocument) {
    const std::string h = readFile(projectRoot() / kHarness);
    ASSERT_FALSE(h.empty());

    EXPECT_TRUE(containsCI(h, "Invoke-UpgradeStage")) << "no upgrade stage";
    EXPECT_TRUE(containsCI(h, "UpgradeInstaller"))
        << "the upgrade stage must be able to install a DIFFERENT (v_n+1) installer";
    EXPECT_TRUE(containsCI(h, "upgrade-install"))
        << "the upgrade stage must perform a second install over the first";

    // (a) file association still resolves
    EXPECT_TRUE(contains(h, "assoc_still_resolves"))
        << "upgrade must assert the file association still resolves";
    EXPECT_TRUE(containsCI(h, "OpenWithProgids"))
        << "the association assertion must actually query the .pdf ProgID list";
    // (b) the sentinel setting survives
    EXPECT_TRUE(contains(h, "sentinel_survived"))
        << "upgrade must assert the sentinel setting survives";
    // (c) the document survives
    EXPECT_TRUE(contains(h, "document_survived"))
        << "upgrade must assert the document survives";
    EXPECT_TRUE(containsCI(h, "Get-FileHash"))
        << "the document assertion must compare content, not merely existence";
}

// WIN-94 / WIN-95: Valid is not enough -- an un-countersigned signature dies with
// the certificate, so both conditions must be checked and both must fail the run.
TEST(InstallerSignatureHarness, RequiresValidStatusAndRfc3161Countersignature) {
    const std::string h = readFile(projectRoot() / kHarness);
    ASSERT_FALSE(h.empty());

    EXPECT_TRUE(contains(h, "Get-AuthenticodeSignature"))
        << "no Authenticode check in the harness";
    EXPECT_TRUE(contains(h, "$sig.Status -eq 'Valid'"))
        << "the harness must require Status -eq 'Valid'";
    EXPECT_TRUE(contains(h, "$sig.TimeStamperCertificate"))
        << "the harness must require an RFC-3161 countersignature "
           "(TimeStamperCertificate must be non-null)";
    EXPECT_TRUE(contains(h, "rfc3161_present"))
        << "the signature report must record the countersignature result";

    // The check must cover the shipped binaries AND the installer.
    EXPECT_TRUE(contains(h, "bin\\pdfe.exe"))
        << "the signature stage must cover the shipped pdfe.exe";
    EXPECT_TRUE(contains(h, "bin\\pdfe_preflight.exe"))
        << "the signature stage must cover the shipped pdfe_preflight.exe";
    EXPECT_TRUE(contains(h, "Invoke-SignatureStage @($Installer)"))
        << "the signature stage must cover the installer itself";
}

// The harness must fail CLOSED: it installs and uninstalls real software, so it
// must refuse to run on a machine that has not been declared disposable rather
// than mutating a developer or build host.
TEST(InstallerResidueHarness, RefusesToRunOnAnUndeclaredMachine) {
    const std::string h = readFile(projectRoot() / kHarness);
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(contains(h, "ConfirmDisposableMachine"))
        << "the harness must require an explicit disposable-machine confirmation";
    EXPECT_TRUE(containsCI(h, "Test-Elevated"))
        << "the harness must check for elevation; an unelevated install writes no "
           "HKLM keys and would make the residue diff meaningless";
    EXPECT_TRUE(containsCI(h, "REFUSED"))
        << "the refusal must be recorded in the report, never a silent pass";
}

// ===========================================================================
// 3. The expected-residue baseline and its consistency result
// ===========================================================================

TEST(InstallerResidueBaseline, ListsOnlyPreservedHkcuAsSurviving) {
    const QJsonObject b = readJsonObject(projectRoot() / kBaseline);
    ASSERT_FALSE(b.isEmpty()) << "missing/invalid baseline: " << kBaseline.string();

    EXPECT_EQ(b.value("hkcu_intent").toString().toStdString(), "PRESERVED")
        << "the baseline must record the documented PRESERVE intent";

    const QJsonArray surviving = b.value("expected_surviving").toObject().value("registry").toArray();
    ASSERT_FALSE(surviving.isEmpty())
        << "the baseline must name the HKCU user-data keys that are INTENDED to "
           "survive; an empty list would make surviving user data look like residue";
    for (const QJsonValue& v : surviving) {
        const std::string key = v.toString().toStdString();
        EXPECT_EQ(key.rfind("HKCU\\Software\\PDFE", 0), 0u)
            << "expected-surviving entry '" << key
            << "' is outside the documented HKCU user-data scope";
    }

    // The forbidden directive is the one that would destroy user data.
    const QJsonArray forbidden = b.value("uninstall_directives").toObject().value("forbidden").toArray();
    bool forbidsHkcuDelete = false;
    for (const QJsonValue& v : forbidden) {
        if (contains(v.toString().toStdString(), "DeleteRegKey HKCU")) forbidsHkcuDelete = true;
    }
    EXPECT_TRUE(forbidsHkcuDelete)
        << "the baseline must forbid DeleteRegKey HKCU \"Software\\PDFE\", which the "
           "intent note says the uninstaller must never issue";

    // Every uninstalled key really must be asserted absent.
    const QJsonArray absent = b.value("must_be_absent_after_uninstall").toObject().value("registry").toArray();
    EXPECT_GE(absent.size(), 8)
        << "the baseline must list every registry target the uninstaller removes "
           "plus the CPack-generated ARP key";
    bool hasArp = false;
    for (const QJsonValue& v : absent) {
        if (containsCI(v.toString().toStdString(), "CurrentVersion\\Uninstall\\PDFE")) hasArp = true;
    }
    EXPECT_TRUE(hasArp)
        << "WIN-11: the CPack-generated ARP/Uninstall key must be asserted absent "
           "after uninstall";
}

// The committed consistency result must show the baseline and the intent note in
// exact agreement. The checker itself is source-controlled and re-runnable.
TEST(InstallerResidueBaseline, ConsistencyResultIsCommittedAndClean) {
    ASSERT_TRUE(std::filesystem::exists(projectRoot() / kChecker))
        << "missing consistency checker: " << kChecker.string();

    const QJsonObject c = readJsonObject(projectRoot() / kConsistency);
    ASSERT_FALSE(c.isEmpty())
        << "missing/invalid consistency result: " << kConsistency.string();
    EXPECT_EQ(c.value("verdict").toString().toStdString(), "CONSISTENT");
    EXPECT_EQ(c.value("difference_count").toInt(-1), 0)
        << "the committed baseline-consistency result must show zero differences";
    EXPECT_GE(c.value("checks").toArray().size(), 8)
        << "the consistency result must cover every baseline section, not just one";
    for (const QJsonValue& v : c.value("checks").toArray()) {
        EXPECT_EQ(v.toObject().value("result").toString().toStdString(), "MATCH")
            << "check '" << v.toObject().value("name").toString().toStdString()
            << "' is not a match";
    }
}

// ===========================================================================
// 4. Installer-scope dispositions (WIN-05, WIN-08)
// ===========================================================================

// WIN-08: admin-only / per-machine-only is a real user-facing constraint and had
// no stated disposition anywhere.
TEST(InstallerScopeDocs, PerMachineAdminOnlyIsDocumented) {
    const std::string doc = readFile(projectRoot() / kIntentDoc);
    const std::string readme = readFile(projectRoot() / "README.md");
    ASSERT_FALSE(doc.empty());
    ASSERT_FALSE(readme.empty());

    EXPECT_TRUE(containsCI(doc, "per-machine"))
        << "the intent note must state the install scope";
    EXPECT_TRUE(containsCI(doc, "administrator"))
        << "the intent note must state that administrator rights are required";
    EXPECT_TRUE(containsCI(doc, "NO per-user"))
        << "the intent note must state the deliberate absence of a per-user "
           "(HKCU-scoped) install mode";
    EXPECT_TRUE(containsCI(readme, "Administrator rights are required to install"))
        << "README.md must tell a downloader that installation needs admin rights "
           "before they download";
}

// WIN-05: repair mode is architecturally absent (NSIS, not MSI) and that was
// nowhere recorded as a scope decision.
TEST(InstallerScopeDocs, RepairModeHasAStatedDisposition) {
    const std::string doc = readFile(projectRoot() / kIntentDoc);
    ASSERT_FALSE(doc.empty());
    const std::string lc = toLower(doc);
    EXPECT_TRUE(contains(lc, "repair mode"))
        << "the intent note must dispose of installer 'repair' explicitly";
    EXPECT_TRUE(contains(lc, "nsis installer, not an msi"))
        << "the disposition must state WHY repair is unavailable (NSIS has no "
           "installer database, so there is no Repair verb)";
    EXPECT_TRUE(contains(lc, "uninstall followed by reinstall"))
        << "the disposition must name the supported recovery path instead";
}

// ═══════════════════════════════════════════════════════════════════════
// GitHub #74 (WIN-06) — the installer must REQUEST ELEVATION explicitly.
//
// The installer writes to Program Files, writes HKLM file associations, and
// installs an all-users Start Menu shortcut. Every one of those needs
// administrator rights.
//
// Without an explicit request the behaviour falls to Windows' INSTALLER
// DETECTION heuristic, which guesses from the filename ("setup", "install")
// and the manifest. When it guesses wrong the installer runs unelevated and
// the privileged writes fail PART WAY THROUGH — files copied, registry
// entries missing, no uninstall entry. **A partial install that reports
// success is worse than a refusal**, because the user is left with a
// launchable application whose associations silently do not exist.
//
// `RequestExecutionLevel admin` makes it deterministic: a standard user gets
// a UAC prompt, and declining aborts before anything is written — the
// "prompts or refuses cleanly" behaviour the issue asks for, and a property
// of the generated script rather than of its filename.
//
// This is the text oracle the issue asks for. It is deliberately a source
// assertion rather than an installer run: producing an installer requires a
// full Release build plus NSIS, which is not available in the unit suite, and
// a test that silently skipped when NSIS was absent would assert nothing on
// exactly the machines that do not package.
// ═══════════════════════════════════════════════════════════════════════

TEST(InstallerScopeDocs, InstallerRequestsElevationExplicitly)
{
    // MEASURED, and the reason this test asserts what it does.
    //
    // #74 proposes adding `RequestExecutionLevel admin`. Running
    // `cpack -C Debug -G NSIS` and reading the generated script shows CPack's
    // NSIS template ALREADY emits it at top level (project.nsi:41, with no
    // Section or Function above it). The installer therefore already prompts a
    // standard user and aborts cleanly if they decline.
    //
    // Adding it again BREAKS THE BUILD: the obvious hook,
    // CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS, emits inside the core install
    // Section, and the directive is only valid at top level --
    // "Error: command RequestExecutionLevel not valid in Section".
    //
    // So a test asserting our CMakeLists contains the string would have PASSED
    // while the installer no longer built at all. That is the trap this test
    // exists on the other side of: it asserts we do NOT re-add it through a
    // section-scoped hook, which is the failure mode that is actually
    // reachable from this repository.
    const std::string cml = readFile(projectRoot() / "CMakeLists.txt");
    ASSERT_FALSE(cml.empty()) << "could not read CMakeLists.txt";

    // Match an ASSIGNMENT, not a mention. The explanatory comment in
    // CMakeLists.txt names this hook in prose, and a bare substring search
    // found THAT and reported a violation against the comment explaining why
    // there is no violation. Precisely the "a comment mentioning the guard is
    // not the guard" case that tools/gate/lint_unc_gate.py self-tests for --
    // made here, in the same phase, having just written that lint.
    const std::size_t hook = cml.find("set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS");
    if (hook != std::string::npos) {
        const std::size_t close = cml.find(')', hook);
        ASSERT_NE(close, std::string::npos)
            << "malformed CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS assignment";
        const std::string body = cml.substr(hook, close - hook);
        EXPECT_EQ(body.find("RequestExecutionLevel"), std::string::npos)
            << "RequestExecutionLevel is being emitted through "
               "CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS, which lands INSIDE the "
               "install Section. NSIS rejects it there and the installer will "
               "not build at all: \"command RequestExecutionLevel not valid "
               "in Section\". CPack's template already emits it at top level; "
               "it must not be added again here.";
    }

    // The elevation requirement itself is recorded so the decision is not
    // merely implicit in a third-party template we happen to inherit.
    EXPECT_NE(cml.find("elevation is ALREADY REQUESTED"), std::string::npos)
        << "the reasoning for relying on CPack's own RequestExecutionLevel is "
           "not recorded in CMakeLists.txt, so a future reader has no way to "
           "know it was checked rather than overlooked";
}

// Side finding from the same read: the installer advertised a repository that
// does not exist. Same class as the three placeholder links found in
// MainWindow during GitHub #127 — a URL nobody clicks during development.
TEST(InstallerScopeDocs, InstallerLinksNameTheRealRepository)
{
    const std::string cml = readFile(projectRoot() / "CMakeLists.txt");
    ASSERT_FALSE(cml.empty());

    EXPECT_EQ(cml.find("github.com/pdfe/pdfe"), std::string::npos)
        << "CPACK_NSIS_HELP_LINK / URL_INFO_ABOUT still point at the "
           "placeholder repository github.com/pdfe/pdfe, which does not "
           "exist; the installer's Help and About links would 404";

    EXPECT_NE(cml.find("github.com/ipswitch8/PDFE"), std::string::npos)
        << "the installer no longer names any repository at all";
}
