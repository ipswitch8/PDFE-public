// -*- coding: utf-8 -*-
// ---------------------------------------------------------------------------
// GitHub #176 -- the Form XObject depth limit, pinned against a MEASURED
// native-stack failure depth instead of an argued one.
//
// WHAT WAS WRONG WITH THE OLD JUSTIFICATION
//
// GitHub #159 shipped `FormXObjectDepthGuard` with a limit of 256. The limit
// was chosen by reasoning: MuPDF's own catchable guard was known to cope from
// depth 64 through 1024, depth 4096 was known to kill the process, and 256 sits
// below that range "with margin". Nobody measured where the native stack
// actually runs out, so the margin was an estimate of an unmeasured quantity.
//
// tests/scripts/bisect_xobject_native_depth.py measures it, by raising the
// guard out of the way through a PDFE_TESTING-only seam and bisecting in a
// child process. Its output is committed at the path below.
//
// THE MEASUREMENT IS A BAND, NOT A NUMBER -- and this test is written around
// that. Depths near the boundary kill on some runs and survive on others
// (thread stack base is randomised, per-level consumption is not constant), so
// the margin here is computed against the CONSERVATIVE edge: the deepest depth
// that survived every trial. Asserting against the midpoint of a stochastic
// band would be asserting a number the measurement does not support.
//
// WHAT THIS TEST IS FOR. It is not a re-measurement -- re-running the bisect
// takes minutes and spawns dozens of child processes, which does not belong in
// the unit suite. It is the guard that stops the limit and the measurement from
// drifting apart silently: raise `kMaxFormXObjectDepth` toward the measured
// failure band, or replace the measurement with a shallower one, and this
// reddens.
// ---------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "core/FormXObjectDepthGuard.h"
#include "core/MuPdfContext.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kMeasurementPath =
    "docs/audit/p83-outstanding/phase-6/native-stack-depth.json";

// The margin the limit must carry against the deepest ALWAYS-surviving depth.
//
// 4x rather than "any margin at all": the measurement is taken on one platform,
// one MuPDF build and one document shape. A limit that merely squeaked under
// the measured edge would be pinned to this machine. 4x leaves room for a
// platform with a quarter of this stack, or a document shape that spends four
// times the stack per level -- which is the scenario GitHub #176 names
// explicitly as the reason the argued margin was not good enough.
constexpr int kRequiredMarginFactor = 4;

// Minimal field read. A JSON dependency for one integer would be the larger
// risk; this is deliberately narrow and fails loudly rather than defaulting.
bool readIntField(const std::string& blob, const std::string& key, long& out)
{
    const std::string needle = "\"" + key + "\"";
    const auto k = blob.find(needle);
    if (k == std::string::npos) return false;
    const auto colon = blob.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    std::size_t i = colon + 1;
    while (i < blob.size() && (blob[i] == ' ' || blob[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < blob.size() && (std::isdigit(static_cast<unsigned char>(blob[i])) ||
                               blob[i] == '-')) ++i;
    if (i == start) return false;
    out = std::strtol(blob.substr(start, i - start).c_str(), nullptr, 10);
    return true;
}

// The same document shape the bisect and the regression sweep use. Kept local
// and small on purpose: a shared fixture generator would couple this test to
// another file's anonymous namespace for twenty lines of string building.
std::string nestedXObjectPdf(int depth)
{
    std::string out = "%PDF-1.7\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /XObject << /X0 10 0 R >> >> /Contents 4 0 R >>\nendobj\n";
    const std::string pageStream = "q /X0 Do Q\n";
    out += "4 0 obj\n<< /Length " + std::to_string(pageStream.size()) +
           " >>\nstream\n" + pageStream + "endstream\nendobj\n";
    for (int i = 0; i < depth; ++i) {
        const int objNum = 10 + i;
        const bool innermost = (i == depth - 1);
        const std::string body = innermost
            ? std::string("0 0 100 100 re f\n")
            : std::string("q /X") + std::to_string(i + 1) + " Do Q\n";
        std::string dict = "<< /Type /XObject /Subtype /Form "
                           "/BBox [0 0 612 792] /Length " +
                           std::to_string(body.size());
        if (!innermost) {
            dict += " /Resources << /XObject << /X" + std::to_string(i + 1) +
                    " " + std::to_string(objNum + 1) + " 0 R >> >>";
        }
        dict += " >>";
        out += std::to_string(objNum) + " 0 obj\n" + dict + "\nstream\n" +
               body + "endstream\nendobj\n";
    }
    out += "trailer\n<< /Size " + std::to_string(10 + depth) +
           " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

// The margin assertions read maxFormXObjectDepth(), which is OVERRIDABLE in a
// PDFE_TESTING build. If the override were set -- by a leak from another test
// in the same process, or by an environment that happened to carry it -- those
// assertions would silently compare the measurement against a number that is
// not the shipped limit, and would pass or fail for a reason unrelated to what
// they claim to check. Clearing it first makes the tests independent of
// execution order rather than merely usually-correct.
void clearDepthOverride()
{
#if defined(PDFE_TESTING) && defined(_WIN32)
    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "");
#endif
}

std::string readMeasurement()
{
    const fs::path p = fs::path(PDFE_REPO_ROOT) / kMeasurementPath;
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// The measurement artifact must EXIST and be readable. Without this, every
// assertion below would pass vacuously on a tree where the file was deleted or
// renamed -- an empty-collection oracle, which this project has been bitten by
// before.
TEST(FormXObjectDepthMargin, TheMeasurementArtifactIsPresentAndParses)
{
    clearDepthOverride();
    const std::string blob = readMeasurement();
    ASSERT_FALSE(blob.empty())
        << "missing or unreadable: " << kMeasurementPath
        << "\nRegenerate with tests/scripts/bisect_xobject_native_depth.py";

    long safeEdge = 0;
    ASSERT_TRUE(readIntField(blob, "deepest_always_surviving_depth", safeEdge))
        << "artifact present but has no deepest_always_surviving_depth field";

    // Non-vacuity: a measurement of 0, or one at or below the shipped limit,
    // would make the margin assertion below trivially satisfiable or
    // nonsensical. It would also mean the bisect never actually got past the
    // guard -- exactly the failure the script's control run exists to catch.
    EXPECT_GT(safeEdge, pdfe::core::maxFormXObjectDepth())
        << "the measured surviving depth (" << safeEdge << ") is not deeper "
           "than the shipped limit. Either the bisect ran with the guard still "
           "in force, or the artifact is stale.";
}

// The shipped limit must carry real, measured margin.
TEST(FormXObjectDepthMargin, TheShippedLimitCarriesMeasuredMarginNotArguedMargin)
{
    clearDepthOverride();
    const std::string blob = readMeasurement();
    ASSERT_FALSE(blob.empty()) << "missing: " << kMeasurementPath;

    long safeEdge = 0;
    ASSERT_TRUE(readIntField(blob, "deepest_always_surviving_depth", safeEdge));

    const long limit = pdfe::core::maxFormXObjectDepth();
    ASSERT_GT(limit, 0);

    EXPECT_LE(limit * kRequiredMarginFactor, safeEdge)
        << "The Form XObject depth limit (" << limit << ") no longer carries a "
        << kRequiredMarginFactor << "x margin against the measured "
           "always-surviving depth (" << safeEdge << ").\n"
           "Either the limit was raised, or the measurement was re-taken on a "
           "configuration with a shallower stack. Both need a decision, not a "
           "bump of this constant.";
}

// The measurement must record that the boundary is STOCHASTIC. This is the
// property that makes the whole artifact trustworthy: a file reporting a single
// sharp threshold would be the false-precision answer the first bisect
// produced, and this test is what stops that answer from coming back.
TEST(FormXObjectDepthMargin, TheMeasurementRecordsABandRatherThanAThreshold)
{
    clearDepthOverride();
    const std::string blob = readMeasurement();
    ASSERT_FALSE(blob.empty()) << "missing: " << kMeasurementPath;

    EXPECT_NE(blob.find("stochastic_band_observed"), std::string::npos)
        << "the artifact does not record a stochastic band. A bisect that "
           "reports one sharp number over a non-monotonic predicate has "
           "reported a random point in the band as a threshold.";

    long trials = 0;
    ASSERT_TRUE(readIntField(blob, "trials_per_depth", trials))
        << "the artifact does not say how many trials each depth got";
    EXPECT_GE(trials, 2)
        << "a single trial per depth cannot distinguish 'survives' from "
           "'survived this once'";

    long safeEdge = 0, notSafe = 0;
    ASSERT_TRUE(readIntField(blob, "deepest_always_surviving_depth", safeEdge));
    ASSERT_TRUE(readIntField(blob, "shallowest_not_always_surviving_depth",
                             notSafe));
    EXPECT_LT(safeEdge, notSafe)
        << "the safe edge must be strictly below the not-always-safe edge";
}

// The bisect seam must actually reach the deciding site. A seam that the guard
// does not consult is decoration, and the bisect that depends on it would then
// be measuring the shipped constant while believing it had raised it -- which
// is how a bisect converges on its own input and reports it as a measurement.
TEST(FormXObjectDepthMargin, TheTestOnlyOverrideReachesTheLimitAccessor)
{
#ifndef PDFE_TESTING
    GTEST_SKIP() << "override seam is compiled out without -DPDFE_TESTING";
#else
    const int shipped = pdfe::core::maxFormXObjectDepth();

    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "1234");
    const int overridden = pdfe::core::maxFormXObjectDepth();
    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "");

    EXPECT_EQ(overridden, 1234)
        << "the override did not reach maxFormXObjectDepth()";
    EXPECT_EQ(pdfe::core::maxFormXObjectDepth(), shipped)
        << "clearing the override did not restore the shipped limit";

    // A rejected value must leave the shipped constant in force rather than
    // producing a degenerate limit. A limit of 0 would refuse every document.
    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "0");
    EXPECT_EQ(pdfe::core::maxFormXObjectDepth(), shipped);
    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "not-a-number");
    EXPECT_EQ(pdfe::core::maxFormXObjectDepth(), shipped);
    ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "");
#endif
}

// THE ONE THAT ACTUALLY GUARDS THE BISECT.
//
// The test above proves the override reaches the ACCESSOR. That is not the
// property the bisect depends on -- it depends on the override reaching the
// site that DECIDES, `formXObjectDepthExceeded`. Those were different things
// until this phase: the deciding site read the constant directly, so an
// override could move the accessor's answer while the refusal behaviour stayed
// pinned at 256. A bisect run against that build would have been answered by
// the guard at every probe and would have converged on 256 -- reporting its own
// input back as a measurement, with no symptom to notice.
//
// So this asserts REFUSAL BEHAVIOUR at a depth well inside the safe region
// (300 is two orders of magnitude below the measured band, so nothing here can
// exhaust a stack): the same document must be refused under a low override and
// accepted under a high one.
TEST(FormXObjectDepthMargin, TheOverrideChangesRefusalBehaviourNotJustTheAccessor)
{
#ifndef PDFE_TESTING
    GTEST_SKIP() << "override seam is compiled out without -DPDFE_TESTING";
#else
    const auto dir = fs::temp_directory_path() / "pdfe_xobj_margin";
    fs::create_directories(dir);
    const auto path = dir / "nested_300.pdf";
    {
        std::ofstream o(path, std::ios::binary);
        const std::string bytes = nestedXObjectPdf(300);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    auto measureAt = [&](const char* override) -> bool {
        ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", override);
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        bool exceeded = false;
        ctx->execute([&](fz_context* c) {
            pdf_document* doc = pdf_open_document(c, path.string().c_str());
            if (!doc) return;
            exceeded = pdfe::core::formXObjectDepthExceeded(c, doc, 0);
            pdf_drop_document(c, doc);
        });
        ::_putenv_s("PDFE_XOBJ_DEPTH_LIMIT_OVERRIDE", "");
        return exceeded;
    };

    // Limit 100, document nests 300 deep -> must be refused.
    EXPECT_TRUE(measureAt("100"))
        << "a 300-deep document was NOT refused with the limit overridden to "
           "100. The override is not reaching formXObjectDepthExceeded, which "
           "means the bisect script cannot raise the guard out of its own way "
           "and any depth it reports is the guard's limit, not the stack's.";

    // Limit 1000, same document -> must be accepted. Without this control the
    // assertion above would also pass on a guard that refuses everything.
    EXPECT_FALSE(measureAt("1000"))
        << "a 300-deep document was refused with the limit overridden to 1000. "
           "The guard is refusing regardless of the limit.";
#endif
}
