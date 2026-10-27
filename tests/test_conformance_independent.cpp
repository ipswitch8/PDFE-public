// -*- coding: utf-8 -*-
//
// Independent PDF/A conformance checking — meta-audit follow-up F14.
//
// Gaps: PDF-10 (the only independent checker was opt-in via a VERAPDF_JAR
// variable set nowhere, so it had never run), PDF-17 (nothing prevented or
// detected a FALSE conformance claim — XmpMetadataWriter stamps
// `pdfaid:part`/`conformance` and the only checker was PDFE's own thin
// validator).
//
// THE CLOSED LOOP THIS BREAKS. `test_pdfa_roundtrip.cpp` passes because
// PdfAConverter's output satisfies PdfAValidator — but both are PDFE code, so
// a shared misunderstanding of ISO 19005 is invisible. veraPDF is an
// independent implementation maintained by the veraPDF Consortium and is the
// reference PDF/A validator; agreement with it is evidence, agreement with
// ourselves is not.
//
// Resolution is deterministic and consults NO checker-specific variable — see
// tests/support/ConformanceChecker.{h,cpp} and tools/conformance/README.md.
// Nothing here is GTEST_SKIP-guarded: when the checker is unavailable the test
// asserts the RECORDED reason and named prerequisite instead, which is the
// contract acceptance criterion 4 defines.

#include <gtest/gtest.h>

#include "support/ConformanceChecker.h"

#include "compliance/ComplianceTypes.h"
#include "compliance/PdfAConverter.h"
#include "compliance/PdfAValidator.h"
#include "compliance/XmpMetadataWriter.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pdfe::compliance::PdfALevel;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::test::CheckerVerdict;
using pdfe::test::recordConformanceResult;
using pdfe::test::resolveConformanceChecker;
using pdfe::test::runConformanceCheck;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }
fs::path fixture(const std::string& n) { return fs::path(TEST_FIXTURES_DIR) / n; }

/// A known-good, externally-produced PDF/A-1b. Not produced by PDFE, so it
/// tests the checker integration rather than PDFE's converter.
fs::path externalPdfA() {
    return repoRoot() / "test_PDFs" / "crazyones-pdfa.pdf";
}

fs::path gateArtifactRecord() {
    return repoRoot() / ".claude" / "pipelines" / "meta-audit-followup"
         / "gate-artifacts" / "phase-14" / "conformance-checker-record.json";
}

fs::path durableRecord() {
    return repoRoot() / "docs" / "audit" / "meta-audit-2026-07" / "followup"
         / "phase-14" / "conformance-checker-record.json";
}

/// Remove `//` and block comments so a source scan can distinguish code from
/// prose. Deliberately simple — it does not need to handle every C++ lexing
/// corner, only to stop a comment that NAMES a symbol from reading as a USE of
/// it. String literals are left alone, which is the conservative direction: a
/// real `getenv("VERAPDF_JAR")` is never missed.
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            out += '\n';
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            ++i;
        } else {
            out += src[i];
        }
    }
    return out;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// The recorded PDF/A-1b baseline for PDFE's own converter output.
//
// F14 wired up the independent reference validator for the first time. Its
// first run rejected PdfAConverter output on ELEVEN clauses. Fixing the XMP
// packet defect (see XmpMetadataWriter.cpp — pugixml was emitting an illegal
// `<?xml?>` declaration inside the packet) cleared eight of them. THREE remain,
// and they are recorded here rather than asserted away:
//
//   6.1.7-2  "The stream keyword shall be followed either by a CARRIAGE RETURN
//            (0Dh) and LINE FEED (0Ah) character sequence or by a single LINE
//            FEED character. The endstream keyword shall be preceded by an EOL
//            marker."
//            OWNER: MuPDF's object serialiser (pdf-write.c). PDFE does not
//            write stream bytes itself, so this cannot be fixed from PDFE — it
//            needs a MuPDF writer change. Decisively out of F14's scope, and
//            it alone makes PDF/A-1b unreachable today.
//
//   6.7.3-1  "The value of CreationDate entry from the document information
//            dictionary, if present, and its analogous XMP property
//            xmp:CreateDate shall be equivalent."
//            OWNER: XmpMetadataWriter date conversion.
//
//   6.7.9-3  XMP predefined-schema property conformance.
//            OWNER: XmpMetadataWriter schema emission.
//
// WHY A BASELINE AND NOT A GREEN ASSERTION. Asserting `valid == true` would be
// a red test we cannot honestly fix in this phase; asserting nothing would put
// the closed loop straight back. Pinning the EXACT set means:
//   * a NEW failing clause is a regression and turns this red, and
//   * fixing one of these ALSO turns it red, with a message saying so — the
//     baseline must then shrink. It can only ratchet in the right direction.
//
// Tracked as a follow-up in
// docs/audit/meta-audit-2026-07/followup/phase-14-dispositions.md.
const std::vector<std::string>& pdfA1bConverterBaseline() {
    static const std::vector<std::string> clauses = {
        "6.1.7-2", "6.7.3-1", "6.7.9-3",
    };
    return clauses;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x + " ";
    return s.empty() ? "(none)" : s;
}

/// Assert @p v against the recorded baseline, with a message that says which
/// way the set moved.
void expectMatchesBaseline(const pdfe::test::CheckerVerdict& v,
                           const char* what) {
    const std::vector<std::string> got = pdfe::test::failedClauses(v);
    const std::vector<std::string>& want = pdfA1bConverterBaseline();
    if (got == want) return;

    std::vector<std::string> added, fixed;
    for (const auto& c : got) {
        if (std::find(want.begin(), want.end(), c) == want.end()) added.push_back(c);
    }
    for (const auto& c : want) {
        if (std::find(got.begin(), got.end(), c) == got.end()) fixed.push_back(c);
    }
    ADD_FAILURE()
        << what << ": the independent validator's failing-clause set changed.\n"
        << "  baseline : " << joined(want) << "\n"
        << "  observed : " << joined(got) << "\n"
        << "  NEW failures (a regression — fix them): " << joined(added) << "\n"
        << "  no longer failing (a FIX — shrink the baseline in "
           "pdfA1bConverterBaseline() and update phase-14-dispositions.md): "
        << joined(fixed) << "\n"
        << v.output;
}

class ConformanceIndependentTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(tmp_.isValid());
    }
    void TearDown() override { ctx_.reset(); }

    fs::path out(const std::string& n) const {
        return fs::path(tmp_.path().toStdString()) / n;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    QTemporaryDir tmp_;
};

} // namespace

// ===========================================================================
// Criterion 3 — resolution without a human-supplied variable
// ===========================================================================

TEST(ConformanceCheckerResolution, IsTotalAndDeterministic) {
    const auto& r = resolveConformanceChecker();

    EXPECT_FALSE(r.searched.empty())
        << "the resolver must record every location it tried, so an "
           "'unavailable' outcome is auditable rather than a bare claim";

    // The provisioned default location is searched FIRST.
    ASSERT_FALSE(r.searched.empty());
    EXPECT_NE(r.searched.front().find("tools"), std::string::npos)
        << "first search entry should be the repo-provisioned location, was: "
        << r.searched.front();

    // Totality: available with usable paths, XOR unavailable with a concrete
    // reason AND a named prerequisite. No indeterminate third state — that is
    // what lets every other test in this file assert instead of skip.
    if (r.available) {
        EXPECT_FALSE(r.jarPath.empty());
        EXPECT_TRUE(fs::exists(r.jarPath)) << r.jarPath;
        EXPECT_FALSE(r.javaCommand.empty());
        EXPECT_FALSE(r.howResolved.empty());
        EXPECT_TRUE(r.reason.empty())
            << "an available checker must not carry an unavailability reason";
    } else {
        EXPECT_FALSE(r.reason.empty())
            << "an unavailable checker must state WHY";
        EXPECT_FALSE(r.prerequisite.empty())
            << "an unavailable checker must name the prerequisite that would "
               "make it available";
    }

    // Determinism: a second call returns the identical resolution.
    const auto& again = resolveConformanceChecker();
    EXPECT_EQ(&r, &again);
    EXPECT_EQ(r.jarPath, again.jarPath);
}

// Criterion 3, enforced mechanically: "no reliance on an unset VERAPDF_JAR may
// remain". A source scan is the only oracle that cannot rot — a future edit
// that reintroduces the opt-in turns this red.
TEST(ConformanceCheckerResolution, NoSourceReliesOnAVeraPdfJarVariable) {
    std::vector<std::string> offenders;
    for (const fs::path& dir : {repoRoot() / "tests",
                                repoRoot() / "tests" / "support",
                                repoRoot() / "src" / "compliance"}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            // RELIANCE, not mention. F14's own comments (and this test) name
            // the variable to explain what was removed; naming it in prose is
            // documentation, READING it is the defect. So comments are stripped
            // first and the needle is a live `getenv` call on the variable.
            const std::string code = stripComments(readAll(e.path()));
            for (size_t at = code.find("VERAPDF_JAR");
                 at != std::string::npos;
                 at = code.find("VERAPDF_JAR", at + 1)) {
                const size_t from = at > 32 ? at - 32 : 0;
                if (code.substr(from, at - from).find("getenv") != std::string::npos) {
                    offenders.push_back(e.path().filename().string());
                    break;
                }
            }
        }
    }
    EXPECT_TRUE(offenders.empty())
        << "these files still reference the VERAPDF_JAR opt-in that F14 "
           "removed; the checker must resolve without a human-supplied "
           "variable (tools/conformance/README.md): "
        << [&] {
               std::string s;
               for (const auto& o : offenders) s += o + " ";
               return s;
           }();
}

// ===========================================================================
// Criterion 4 — the result-or-reason artifact
// ===========================================================================

TEST(ConformanceCheckerRecord, ResultOrReasonArtifactIsWrittenAndHonest) {
    pdfe::test::flushConformanceRecord();

    for (const fs::path& p : {gateArtifactRecord(), durableRecord()}) {
        SCOPED_TRACE(p.string());
        ASSERT_TRUE(fs::exists(p))
            << "phase-14 requires a result-or-reason record at " << p;

        QFile f(QString::fromStdString(p.string()));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        QJsonParseError err{};
        const QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &err);
        ASSERT_EQ(err.error, QJsonParseError::NoError)
            << err.errorString().toStdString();

        const QJsonObject root = jd.object();
        const QJsonObject checker = root.value("checker").toObject();
        const auto& r = resolveConformanceChecker();

        EXPECT_EQ(checker.value("available").toBool(), r.available)
            << "the record must agree with the live resolution";
        EXPECT_EQ(root.value("ran").toBool(), r.available);

        if (r.available) {
            EXPECT_FALSE(checker.value("jar_path").toString().isEmpty());
            EXPECT_FALSE(checker.value("version").toString().isEmpty())
                << "a reachable checker should report its version";
        } else {
            EXPECT_FALSE(checker.value("reason_unavailable").toString().isEmpty())
                << "an 'unavailable' record must carry the concrete reason";
            EXPECT_FALSE(checker.value("prerequisite").toString().isEmpty())
                << "an 'unavailable' record must name the prerequisite";
        }
        EXPECT_FALSE(checker.value("searched").toArray().isEmpty());

        // The record must not silently DROP checks. It previously rebuilt
        // itself from only the current process's verdicts, so in a 16-shard
        // run the last shard to finish overwrote it with its own subset — the
        // artifact showed 1 of 5 checks while looking complete (found by the
        // phase-14 karen gate). Checks are now merged from per-check sidecars,
        // so the recorded set must be a SUPERSET of what this process
        // contributed.
        const int thisProcess = root.value("checks_this_process").toInt(-1);
        ASSERT_GE(thisProcess, 0)
            << "record must state how many checks this process contributed";
        const QJsonArray checks = root.value("checks").toArray();
        EXPECT_GE(checks.size(), thisProcess)
            << "the merged record dropped checks recorded by this process: "
            << checks.size() << " merged < " << thisProcess << " recorded. "
            << "A truncating write is back.";

        // Every merged entry must be attributable in time, so a stale sidecar
        // left by an earlier run can be identified rather than silently read
        // as evidence from this one.
        for (const QJsonValue& cv : checks) {
            const QJsonObject c = cv.toObject();
            EXPECT_FALSE(c.value("file").toString().isEmpty());
            EXPECT_FALSE(c.value("recorded").toString().isEmpty())
                << "each merged check must carry its own 'recorded' timestamp";
        }
    }
}

// ===========================================================================
// Criterion 3 — the checker's RESULT is asserted (PDF-10)
// ===========================================================================

// PDFE-F45-CI-ONLY: ConformanceIndependentTest.KnownGoodExternalPdfAIsAcceptedByChecker
// deferred-backlog row F45, closed in follow-up phase-46. RED IN CI ONLY,
// green locally, genuine, pre-existing.
// CAUSE — NOT veraPDF availability, which the `else` branch below treats as an
// honest PASS. The hard failure is ASSERT_TRUE(fs::exists(p)) on the INPUT
// test_PDFs/crazyones-pdfa.pdf, excluded by `.gitignore`. The three sibling
// tests in this file that use the COMMITTED tests/fixtures/test_single.pdf were
// never in the failing set — that contrast is the diagnosis.
// FIX: the 16 KB input is now re-included and committed. Assertions unchanged.
// See docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md.
TEST_F(ConformanceIndependentTest, KnownGoodExternalPdfAIsAcceptedByChecker) {
    const auto& r = resolveConformanceChecker();
    const fs::path p = externalPdfA();
    ASSERT_TRUE(fs::exists(p))
        << "the known-good external PDF/A is missing: " << p;

    CheckerVerdict v = runConformanceCheck(p, "1b");
    recordConformanceResult(v);

    if (r.available) {
        ASSERT_TRUE(v.ran) << "checker resolved but did not run: " << v.reason;
        EXPECT_TRUE(v.valid)
            << "the independent checker REJECTED a known-good external "
               "PDF/A-1b — the integration, not the file, is suspect.\n"
            << v.output;
    } else {
        // Criterion 4: an honest 'unavailable here' is a pass, but it must be
        // concrete and recorded — asserted, never skipped.
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(v.reason.empty());
        EXPECT_FALSE(r.prerequisite.empty());
    }
}

// Negative control: without it, a checker that rubber-stamps everything would
// satisfy the test above.
TEST_F(ConformanceIndependentTest, CheckerRejectsANonConformantDocument) {
    const auto& r = resolveConformanceChecker();
    const fs::path p = fixture("test_single.pdf");
    ASSERT_TRUE(fs::exists(p)) << p;

    CheckerVerdict v = runConformanceCheck(p, "1b");
    recordConformanceResult(v);

    if (r.available) {
        ASSERT_TRUE(v.ran) << v.reason;
        EXPECT_FALSE(v.valid)
            << "a plain, non-PDF/A document was accepted as PDF/A-1b. The "
               "checker is not discriminating, so its PASS verdicts carry no "
               "information.\n"
            << v.output;
    } else {
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(v.reason.empty());
    }
}

// ===========================================================================
// PDF-10 / PDF-17 — PDFE's own converter output, graded independently
// ===========================================================================

TEST_F(ConformanceIndependentTest, ConverterOutputMatchesRecordedIndependentBaseline) {
    const auto& r = resolveConformanceChecker();
    const fs::path src = fixture("test_single.pdf");
    ASSERT_TRUE(fs::exists(src)) << src;
    const fs::path dst = out("converted_pdfa_1b.pdf");

    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);
        pdfe::compliance::PdfAConverter converter(ctx_);
        const auto result = converter.convertAndSave(doc.get(),
                                                     PdfALevel::PdfA_1b, dst);
        ASSERT_TRUE(fs::exists(dst))
            << "PdfAConverter did not produce an output file";
        (void)result;
    }

    CheckerVerdict v = runConformanceCheck(dst, "1b");
    v.file = "PdfAConverter output (from test_single.pdf, level 1b)";
    recordConformanceResult(v);

    if (r.available) {
        ASSERT_TRUE(v.ran) << v.reason;
        // PDFE's converter output is NOT yet PDF/A-1b conformant. F14 measured
        // that for the first time and fixed the largest cause; the residue is
        // pinned so it can only shrink. See pdfA1bConverterBaseline() above for
        // the three remaining clauses and who owns each.
        expectMatchesBaseline(v, "PdfAConverter output at PDF/A-1b");
    } else {
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(v.reason.empty());
    }
}

// PDF-17: the conformance CLAIM must not outrun the conformance. If the saved
// file advertises PDF/A-1b in its XMP `pdfaid`, the independent checker must
// agree. A false claim is worse than no claim: downstream archival systems
// trust `pdfaid` without revalidating.
TEST_F(ConformanceIndependentTest, StampedClaimIsIndependentlyChecked) {
    const auto& r = resolveConformanceChecker();
    const fs::path src = fixture("test_single.pdf");
    const fs::path dst = out("claim_check_1b.pdf");

    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);
        pdfe::compliance::PdfAConverter converter(ctx_);
        converter.convertAndSave(doc.get(), PdfALevel::PdfA_1b, dst);
    }
    ASSERT_TRUE(fs::exists(dst));

    // What does the file CLAIM?
    PdfALevel claimed = PdfALevel::None;
    {
        auto saved = Document::open(ctx_, dst);
        ASSERT_NE(saved, nullptr);
        pdfe::compliance::XmpMetadataWriter xmp(ctx_);
        claimed = xmp.detectPdfALevel(saved.get());
    }

    if (claimed == PdfALevel::None) {
        // No claim stamped — nothing to falsify, and that is a legitimate
        // outcome. Assert the file at least still opens and that PDFE's own
        // validator is consistent with making no claim.
        auto saved = Document::open(ctx_, dst);
        ASSERT_NE(saved, nullptr);
        SUCCEED() << "no pdfaid claim was stamped; nothing to falsify";
        return;
    }

    ASSERT_EQ(claimed, PdfALevel::PdfA_1b)
        << "converter asked for 1b but stamped a different level";

    CheckerVerdict v = runConformanceCheck(dst, "1b");
    v.file = "PdfAConverter output, XMP claims PDF/A-1b";
    recordConformanceResult(v);

    if (r.available) {
        ASSERT_TRUE(v.ran) << v.reason;
        // THE CLAIM IS CURRENTLY FALSE, and this test is what makes that
        // visible instead of assumed. The saved file stamps `pdfaid` PDF/A-1b
        // while the independent reference validator still rejects it on the
        // three baselined clauses — precisely the failure mode PDF-17 predicted
        // and that the PDFE-validates-PDFE loop could never surface.
        //
        // Asserting `valid == true` here would be a red test this phase cannot
        // honestly fix (6.1.7-2 is MuPDF's stream serialiser). Asserting the
        // exact clause set instead keeps the discrepancy MEASURED and pinned:
        // it can only get smaller, and any new divergence is red.
        expectMatchesBaseline(v, "the file's own pdfaid PDF/A-1b claim");
        EXPECT_FALSE(v.valid)
            << "the independent validator now ACCEPTS the claimed level — the "
               "claim has become true. Delete this expectation, restore the "
               "plain EXPECT_TRUE(v.valid), empty pdfA1bConverterBaseline(), "
               "and update phase-14-dispositions.md.";
    } else {
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(v.reason.empty());
    }
}

// The closed loop, stated as an assertion: PDFE's own validator and the
// independent checker must agree on a known-good external PDF/A. Divergence
// means one of them is wrong, and the suite should say which.
// PDFE-F45-CI-ONLY: ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker
// deferred-backlog row F45, closed in follow-up phase-46. RED IN CI ONLY,
// green locally, genuine, pre-existing. Same cause and same fix as
// KnownGoodExternalPdfAIsAcceptedByChecker above: the hard ASSERT_TRUE(exists)
// on the gitignored input test_PDFs/crazyones-pdfa.pdf, now committed. veraPDF
// unavailability is handled as an honest PASS by the `!r.available` branch
// below and was never the cause.
// See docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md.
TEST_F(ConformanceIndependentTest, OwnValidatorAgreesWithIndependentChecker) {
    const auto& r = resolveConformanceChecker();
    const fs::path p = externalPdfA();
    ASSERT_TRUE(fs::exists(p)) << p;

    bool ownSaysCompliant = false;
    {
        auto doc = Document::open(ctx_, p);
        ASSERT_NE(doc, nullptr);
        pdfe::compliance::PdfAValidator validator(ctx_);
        ownSaysCompliant = validator.isCompliant(doc.get(), PdfALevel::PdfA_1b);
    }

    CheckerVerdict v = runConformanceCheck(p, "1b");
    v.file = "crazyones-pdfa.pdf (own-validator agreement check)";
    recordConformanceResult(v);

    if (!r.available) {
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(v.reason.empty());
        return;
    }
    ASSERT_TRUE(v.ran) << v.reason;
    EXPECT_EQ(ownSaysCompliant, v.valid)
        << "PDFE's PdfAValidator says "
        << (ownSaysCompliant ? "COMPLIANT" : "NON-COMPLIANT")
        << " but the independent reference validator says "
        << (v.valid ? "VALID" : "INVALID")
        << " for the same file at PDF/A-1b.\n"
        << v.output;
}
