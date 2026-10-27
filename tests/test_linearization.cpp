// -*- coding: utf-8 -*-
// Linearization (web-optimized output) coverage (pipeline e5b9c231 Phase 4,
// rtm-coverage.md "Linearization — UNCOVERED"). The feature is advertised but
// nothing asserted that a saved file is ACTUALLY linearized. QPDF's own
// isLinearized() reads the /Linearized parameter dict + first-page hint tables,
// so it is an authoritative truth oracle (not a byte-grep proxy).
#include <gtest/gtest.h>
#include "core/QpdfWrapper.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"

#include <qpdf/QPDF.hh>

#include <QRectF>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {
std::filesystem::path fixtures() { return std::filesystem::path(TEST_FIXTURES_DIR); }

bool isLinearized(const std::filesystem::path& p) {
    QPDF q;
    q.processFile(p.string().c_str());
    return q.isLinearized();
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void spew(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// PDF-101's obstacle, and how it is solved.
//
// QpdfWrapper::checkStructure() only ever reports problems a CORRECT writer
// does not produce, so no mutation of the writer can make the "linearized
// output passes the check" assertion fail — mutating the linearize flag just
// yields a non-linearized file, which the existing
// NonLinearizedSaveIsNotLinearized already catches. A test nothing can redden
// is decoration. (That is why the row was declined once; see
// docs/audit/meta-audit-backlog-batch-mutation-evidence-2026-08-31.md.)
//
// The way out is to give the checker something real to catch: damage the
// linearization data itself, in place, so every other byte offset in the file
// stays valid and the document still parses. Only the linearization claim
// becomes a lie — exactly the defect class the check exists for.
//
// WHICH FIELD, AND WHY NOT THE OBVIOUS ONE. The first attempt corrupted `/L`,
// the total file length. That does not work, and the failure is informative:
// QPDF's isLinearized() itself validates `/L` against the actual file size as
// part of deciding whether the file is linearized at all. Measured:
// corrupting `/L` gives isLinearized(damaged) == false, so checkStructure()
// correctly SKIPS a linearization check that no longer applies, and the file
// passes. That is right behaviour, not a gap — the corruption was simply too
// coarse to reach the code under test.
//
// `/O` — the object number of the first page's page object — is not part of
// that cheap recognition test, so the file still reports as linearized, and
// checkLinearization() is what compares it against the real first page.
//
// Returns false if the expected shape was not found, so the caller FAILS rather
// than silently testing an uncorrupted file.
bool corruptLinearizedFirstPageObjectInPlace(std::string& bytes) {
    const size_t lin = bytes.find("/Linearized");
    if (lin == std::string::npos) return false;

    // Search only the leading parameter dictionary, not the whole file — "/O "
    // is far too short a needle to hunt for document-wide.
    const size_t limit = std::min(bytes.size(), lin + 512);
    const size_t o = bytes.find("/O ", lin);
    if (o == std::string::npos || o >= limit) return false;

    const size_t start = o + 3;
    size_t d = start;
    while (d < limit && bytes[d] >= '0' && bytes[d] <= '9') ++d;
    if (d == start) return false;

    // Same digit count, different value: bump the leading digit, wrapping 9->1
    // so the number never gains a leading zero or lands back on the truth.
    char& first = bytes[start];
    first = (first == '9') ? '1' : static_cast<char>(first + 1);
    return true;
}

// Per-page (text, annotation count) for content-equality comparisons.
struct PageContent {
    std::string text;
    size_t annotCount;
};

std::vector<PageContent> contentOf(const std::filesystem::path& p,
                                   const std::shared_ptr<MuPdfContext>& ctx) {
    std::vector<PageContent> out;
    auto doc = Document::open(ctx, p);
    if (!doc) return out;
    pdfe::annotations::AnnotationManager annots(ctx);
    for (int i = 0; i < doc->pageCount(); ++i) {
        auto page = doc->page(i);
        if (!page) { out.push_back(PageContent{}); continue; }
        out.push_back(PageContent{page->extractPlainText(),
                                  annots.getAnnotations(page.get()).size()});
    }
    return out;
}
} // namespace

// Saving with linearize=true produces a genuinely linearized file.
TEST(Linearization, LinearizedSaveProducesLinearizedFile) {
    auto src = fixtures() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto out = std::filesystem::temp_directory_path() / "pdfe_linearized.pdf";
    std::error_code ec; std::filesystem::remove(out, ec);
    {
        QpdfWrapper w;
        w.open(src);
        w.save(out, /*linearize=*/true);
    }
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_TRUE(isLinearized(out))
        << "save(linearize=true) did NOT produce a linearized file";
    std::filesystem::remove(out, ec);
}

// Control: linearize=false must NOT be linearized -- proves the flag actually
// drives the behavior (a save that always/never linearizes would make the
// positive test above vacuous).
TEST(Linearization, NonLinearizedSaveIsNotLinearized) {
    auto src = fixtures() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src));

    auto out = std::filesystem::temp_directory_path() / "pdfe_nonlinear.pdf";
    std::error_code ec; std::filesystem::remove(out, ec);
    {
        QpdfWrapper w;
        w.open(src);
        w.save(out, /*linearize=*/false);
    }
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_FALSE(isLinearized(out))
        << "save(linearize=false) unexpectedly produced a linearized file";
    std::filesystem::remove(out, ec);
}

// ── PDF-101 (meta-audit gap register, issue #5) ──────────────────────────────
//
// The register asks for "LinearizedOutputPassesQpdfCheck calling
// QpdfWrapper::verify() + text/annotation equality vs the source". There is no
// verify(); the real API is checkStructure()/checkFile(). Both halves below.
//
// The source is not the bare fixture: an annotation is added and saved first,
// so the annotation-equality half is not vacuously 0 == 0.
TEST(Linearization, LinearizedOutputPassesQpdfCheck) {
    namespace fs = std::filesystem;
    const fs::path fixture = fixtures() / "test_3page.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << fixture.string();

    const fs::path src = fs::temp_directory_path() / "pdfe_lin_src.pdf";
    const fs::path out = fs::temp_directory_path() / "pdfe_lin_check.pdf";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(out, ec);

    auto ctx = std::make_shared<MuPdfContext>();
    {
        auto doc = Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr);
        pdfe::annotations::AnnotationManager annots(ctx);
        auto page = doc->page(1);
        ASSERT_NE(page, nullptr);
        annots.createAnnotation(page.get(),
                                pdfe::annotations::AnnotationType::Square,
                                QRectF(80, 120, 160, 50));
        page.reset();
        ASSERT_NO_THROW(doc->saveAs(src));
    }
    ASSERT_TRUE(fs::exists(src));

    const auto before = contentOf(src, ctx);
    ASSERT_EQ(before.size(), 3u);
    size_t totalAnnots = 0;
    for (const auto& p : before) totalAnnots += p.annotCount;
    ASSERT_GT(totalAnnots, 0u)
        << "precondition: the source must carry at least one annotation, or the "
           "annotation-equality half of this test asserts nothing";

    {
        QpdfWrapper w;
        w.open(src);
        w.save(out, /*linearize=*/true);
    }
    ASSERT_TRUE(fs::exists(out));
    ASSERT_TRUE(isLinearized(out)) << "precondition: the output must be linearized";

    // (1) The structural check — including QPDF's own checkLinearization(),
    //     which checkStructure() runs only for a linearized file.
    const auto check = QpdfWrapper::checkFile(out);
    std::string problems;
    for (const auto& p : check.problems) { problems += "\n  - "; problems += p; }
    EXPECT_TRUE(check.ok)
        << "the linearized output failed a full structural check:" << problems;

    // (2) Content equality vs the source: linearization reorders and renumbers
    //     objects, so "the file still checks out" is not the same claim as
    //     "the file still says the same thing".
    const auto after = contentOf(out, ctx);
    ASSERT_EQ(after.size(), before.size())
        << "linearization changed the page count";
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].text, before[i].text)
            << "page " << i << " text differs after linearization";
        EXPECT_EQ(after[i].annotCount, before[i].annotCount)
            << "page " << i << " annotation count differs after linearization";
    }

    fs::remove(src, ec);
    fs::remove(out, ec);
}

// The control that makes the check above non-vacuous: a file whose
// linearization data is DELIBERATELY wrong must be REJECTED. Without this, a
// checkStructure() that had quietly stopped looking at linearization would keep
// the test above green forever.
TEST(Linearization, CorruptedLinearizationIsRejectedByTheStructuralCheck) {
    namespace fs = std::filesystem;
    const fs::path fixture = fixtures() / "test_3page.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << fixture.string();

    const fs::path good = fs::temp_directory_path() / "pdfe_lin_good.pdf";
    const fs::path bad  = fs::temp_directory_path() / "pdfe_lin_damaged.pdf";
    std::error_code ec;
    fs::remove(good, ec);
    fs::remove(bad, ec);

    {
        QpdfWrapper w;
        w.open(fixture);
        w.save(good, /*linearize=*/true);
    }
    ASSERT_TRUE(fs::exists(good));
    ASSERT_TRUE(QpdfWrapper::checkFile(good).ok)
        << "precondition: the undamaged linearized file must pass, otherwise a "
           "failure below would not be attributable to the damage";

    std::string bytes = slurp(good);
    ASSERT_FALSE(bytes.empty());
    const size_t sizeBefore = bytes.size();
    ASSERT_TRUE(corruptLinearizedFirstPageObjectInPlace(bytes))
        << "could not find the linearization parameter dictionary's first-page "
           "object entry — the corrupter did not apply, so this test would otherwise "
           "be checking an UNDAMAGED file and passing for the wrong reason";
    ASSERT_EQ(bytes.size(), sizeBefore)
        << "the corruption must be byte-length preserving, or every subsequent "
           "offset in the file is invalidated and the damage is no longer "
           "specific to the linearization data";
    spew(bad, bytes);

    // Reported in the failure message because it distinguishes the two ways
    // this test can fail: the damage stopped the file being recognised as
    // linearized at all (so checkStructure() correctly skipped a check that no
    // longer applies), versus the file still claims linearization and the check
    // simply did not look.
    const bool stillClaimsLinearized = isLinearized(bad);

    const auto check = QpdfWrapper::checkFile(bad);
    std::string problems;
    for (const auto& p : check.problems) { problems += "\n  - "; problems += p; }
    EXPECT_FALSE(check.ok)
        << "a file whose linearization first-page object is deliberately wrong "
           "passed the "
           "structural check — the check is not actually inspecting the "
           "linearization data, so LinearizedOutputPassesQpdfCheck proves "
           "nothing about it. isLinearized(damaged) = "
        << (stillClaimsLinearized ? "true" : "false");
    EXPECT_FALSE(check.problems.empty())
        << "the check reported not-ok but listed no problem";

    fs::remove(good, ec);
    fs::remove(bad, ec);
}
