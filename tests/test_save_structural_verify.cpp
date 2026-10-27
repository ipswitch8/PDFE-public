// -*- coding: utf-8 -*-
//
// Round-3 findings R3-05 (incremental-save structural verify) and R3-10 (no-op
// idempotence oracle) — save-output structural verification.
//
// R3-10 MECHANISM (tested elsewhere too): QpdfWrapper::checkStructure() forces a
// full parse of the object graph + xref (the reusable "qpdf --check" equivalent).
//
// R3-05 — INCREMENTAL SAVE STRUCTURAL VERIFY
//   After an additive edit + incremental save (MuPDF do_incremental=1, appended
//   xref), the output must (a) pass a full structural check and (b) RETAIN the
//   prior revision — an incremental save appends a new xref section without
//   discarding the previous body, so the file carries >= 2 startxref markers and
//   is strictly larger than the original (a superset that preserves the signed
//   byte prefix). A silent full-rewrite masquerading as incremental would fail
//   both.
//
// R3-10 — NO-OP IDEMPOTENCE
//   Opening a fixture that carries UN-MODELED structure (an embedded-file name
//   tree, an AcroForm, a signature /ByteRange) and saving it with NO edits must
//   not silently drop that structure. This pins that a full-rewrite no-op save
//   round-trips un-modeled features rather than dropping what the editor does
//   not itself understand.
//
// EXPECTED OUTCOME: GREEN = existing save behavior verified/pinned. A RED here is
// a genuine drop/corruption to be reported as a defect — the assertions must not
// be weakened to pass.

#include <gtest/gtest.h>

#include "core/QpdfWrapper.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationType.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <QRectF>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::core::QpdfWrapper;

std::string joinProblems(const std::vector<std::string>& problems) {
    std::string out;
    for (const auto& p : problems) { out += "\n  - "; out += p; }
    return out;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// ── R3-05: incremental-save structural verify ────────────────────────────────
TEST(SaveStructuralVerify, IncrementalSaveRetainsPriorRevisionAndVerifies) {
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto temp = std::filesystem::temp_directory_path() / "pdfe_incr_verify.pdf";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    std::filesystem::copy_file(
        fixture, temp, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "staging failed: " << ec.message();

    const auto originalSize = std::filesystem::file_size(temp);
    const int originalStartxref = countOccurrences(slurp(temp), "startxref");

    auto ctx = std::make_shared<MuPdfContext>();
    {
        auto doc = Document::open(ctx, temp);
        ASSERT_NE(doc, nullptr);

        // Additive edit: add a sticky-note annotation. This is an incremental-
        // safe (purely additive) change — exactly the case incremental save
        // exists for.
        pdfe::annotations::AnnotationManager annotMgr(ctx);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::CreateAnnotationCommand cmd(
            &annotMgr, page.get(), pdfe::annotations::AnnotationType::Text,
            QRectF(72.0, 72.0, 120.0, 48.0));
        cmd.execute();
        page.reset();  // drop the Page holder before the append

        ASSERT_NO_THROW(doc->saveIncremental());
        doc.reset();
    }

    // (a) The incremental output passes a full structural check.
    auto chk = QpdfWrapper::checkFile(temp);
    EXPECT_TRUE(chk.ok)
        << "incremental-save output failed structural check:" << joinProblems(chk.problems);

    // (b) The prior revision is retained: an appended xref (>= 2 startxref) and
    //     a strictly larger file (the original byte prefix is preserved).
    const std::string bytes = slurp(temp);
    const int startxrefCount = countOccurrences(bytes, "startxref");
    EXPECT_GE(startxrefCount, 2)
        << "incremental save must append a new xref section (found "
        << startxrefCount << " startxref, original had " << originalStartxref << ")";
    EXPECT_GT(std::filesystem::file_size(temp), originalSize)
        << "incremental save must grow the file (superset of the original prefix)";

    std::filesystem::remove(temp, ec);
}

// ── R3-10: no-op idempotence over un-modeled structure ───────────────────────
enum class Feature { EmbeddedFiles, AcroForm, Signature };

struct IdempotenceCase {
    std::string fixture;
    Feature feature;
    const char* label;
};

const char* featureName(Feature f) {
    switch (f) {
        case Feature::EmbeddedFiles: return "EmbeddedFiles";
        case Feature::AcroForm:      return "AcroForm";
        case Feature::Signature:     return "Signature";
    }
    return "?";
}

// Does the open document still carry the given un-modeled feature?
bool featurePresent(QPDF& q, Feature feature) {
    QPDFObjectHandle root = q.getRoot();
    switch (feature) {
        case Feature::EmbeddedFiles: {
            if (!root.isDictionary()) return false;
            QPDFObjectHandle names = root.getKey("/Names");
            if (!names.isDictionary()) return false;
            QPDFObjectHandle ef = names.getKey("/EmbeddedFiles");
            if (!ef.isDictionary()) return false;
            QPDFObjectHandle arr = ef.getKey("/Names");
            if (arr.isArray() && arr.getArrayNItems() > 0) return true;
            QPDFObjectHandle kids = ef.getKey("/Kids");
            return kids.isArray() && kids.getArrayNItems() > 0;
        }
        case Feature::AcroForm: {
            if (!root.isDictionary()) return false;
            QPDFObjectHandle af = root.getKey("/AcroForm");
            if (!af.isDictionary()) return false;
            QPDFObjectHandle fields = af.getKey("/Fields");
            return fields.isArray() && fields.getArrayNItems() > 0;
        }
        case Feature::Signature: {
            // A signature /V dict carries a /ByteRange. Scan every object so we
            // find it regardless of whether it lives inside a compressed objstm.
            for (auto& obj : q.getAllObjects()) {
                if (obj.isDictionary() && obj.hasKey("/ByteRange")) return true;
            }
            return false;
        }
    }
    return false;
}

class NoOpIdempotence : public ::testing::TestWithParam<IdempotenceCase> {};

TEST_P(NoOpIdempotence, FullRewriteNoOpSavePreservesUnmodeledStructure) {
    const IdempotenceCase& c = GetParam();
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / c.fixture;
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << c.fixture << " not available";
    }

    // Sanity: the ORIGINAL fixture actually carries the feature under test —
    // otherwise the survival assertion would be vacuous.
    {
        QpdfWrapper w;
        w.open(fixture);
        ASSERT_TRUE(featurePresent(w.qpdf(), c.feature))
            << "fixture " << c.fixture << " lacks its " << featureName(c.feature)
            << " precondition";
    }

    auto outPath = std::filesystem::temp_directory_path() /
                   (std::string("pdfe_noop_") + c.label + ".pdf");
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    auto ctx = std::make_shared<MuPdfContext>();
    {
        auto doc = Document::open(ctx, fixture);
        ASSERT_NE(doc, nullptr);
        // Force the full-rewrite path with NO content edits (saveAs always
        // full-rewrites to a new path).
        doc->markRequiresFullRewrite();
        ASSERT_NO_THROW(doc->saveAs(outPath));
        doc.reset();
    }

    // The reopened output must pass a structural check ...
    auto chk = QpdfWrapper::checkFile(outPath);
    EXPECT_TRUE(chk.ok)
        << "no-op save output failed structural check:" << joinProblems(chk.problems);

    // ... and still carry the un-modeled feature.
    {
        QpdfWrapper w;
        ASSERT_NO_THROW(w.open(outPath));
        EXPECT_TRUE(featurePresent(w.qpdf(), c.feature))
            << "full-rewrite no-op save DROPPED the " << featureName(c.feature)
            << " structure from " << c.fixture;
    }

    std::filesystem::remove(outPath, ec);
}

INSTANTIATE_TEST_SUITE_P(
    UnmodeledFeatures, NoOpIdempotence,
    ::testing::Values(
        IdempotenceCase{"test_portfolio.pdf", Feature::EmbeddedFiles, "EmbeddedFiles"},
        IdempotenceCase{"test_form.pdf",      Feature::AcroForm,      "AcroForm"},
        IdempotenceCase{"test_signed.pdf",    Feature::Signature,     "Signature"}),
    [](const ::testing::TestParamInfo<IdempotenceCase>& info) {
        return std::string(info.param.label);
    });

// ── The property nine deleted comments used to claim ─────────────────────────
//
// Until 2026-08-31 there were NINE `pdf_drop_page_tree()` calls in
// Document.cpp, seven of them commented some variant of "flush cached page tree
// so pdf_save_document writes all in-memory modifications". Every one of those
// calls was a no-op — MuPDF 1.26.10 defines the function as
// `{ /* Historical entry point. Now does nothing. We drop 'just in time'. */ }`
// (pdf-page.c:152) and invalidates its own page map inside the object-mutation
// path (pdf-object.c:1818). Deleting one of them killed no test, because there
// was nothing there to kill.
//
// The calls are gone. The CLAIM they made is worth keeping, and was never
// actually tested: a save must serialise BOTH kinds of in-memory change —
// structural (the page tree itself) and content (an annotation on a page) — in
// one write. If a future MuPDF ever does require an explicit invalidation, this
// is the test that reddens, rather than nine comments that were already wrong.
//
// Full investigation: docs/audit/page-tree-flush-noop-2026-08-31.md.
TEST(SaveStructuralVerify, StructuralAndContentEditsBothReachTheSavedFile) {
    namespace fs = std::filesystem;
    const fs::path src = fs::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(fs::exists(src)) << src.string();

    const fs::path work =
        fs::temp_directory_path() / "pdfe_pagetree_flush_src.pdf";
    const fs::path out =
        fs::temp_directory_path() / "pdfe_pagetree_flush_out.pdf";
    std::error_code ec;
    fs::remove(work, ec);
    fs::remove(out, ec);
    fs::copy_file(src, work, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    constexpr const char* kToken = "PAGETREE_FLUSH_TOKEN";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    // Capture the text of the page about to be deleted, BEFORE any of the code
    // under test runs. This is the input side of the oracle: "these words were
    // on page 1, and after the save they must be nowhere in the document".
    std::string doomedText;
    {
        auto p1 = doc->page(1);
        ASSERT_NE(p1, nullptr);
        doomedText = p1->extractPlainText();
    }
    ASSERT_FALSE(doomedText.empty())
        << "precondition: page 1 must carry extractable text, otherwise its "
           "absence after the delete proves nothing";

    // STRUCTURAL change: the page tree itself.
    doc->deletePage(1);
    ASSERT_EQ(doc->pageCount(), 2);

    // CONTENT change on a surviving page, in the same session.
    pdfe::annotations::AnnotationManager annots(ctx);
    {
        auto p0 = doc->page(0);
        ASSERT_NE(p0, nullptr);
        annots.createAnnotation(p0.get(),
                                pdfe::annotations::AnnotationType::Square,
                                QRectF(90, 100, 150, 45));
        const auto created = annots.getAnnotations(p0.get());
        ASSERT_FALSE(created.empty());
        annots.setContents(p0.get(), created.back().annotIndex, kToken);
    }

    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out));
    doc.reset();

    // Both edits must be in the WRITTEN FILE, not merely in the session.
    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr);

    EXPECT_EQ(reopened->pageCount(), 2)
        << "the structural edit (page delete) did not reach the saved file";

    for (int i = 0; i < reopened->pageCount(); ++i) {
        auto p = reopened->page(i);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->extractPlainText().find(doomedText), std::string::npos)
            << "page " << i << " of the saved file still carries the text of "
               "the DELETED page — the page tree was written from a stale view";
    }

    bool found = false;
    {
        pdfe::annotations::AnnotationManager reAnnots(ctx);
        auto rp0 = reopened->page(0);
        ASSERT_NE(rp0, nullptr);
        for (const auto& a : reAnnots.getAnnotations(rp0.get())) {
            if (a.contents == kToken) { found = true; break; }
        }
    }
    EXPECT_TRUE(found)
        << "the content edit (annotation \"" << kToken << "\") did not reach "
           "the saved file — a save that serialises the structural change but "
           "not the content one is exactly the failure the removed flush "
           "comments claimed to prevent";

    // And the result is a structurally sound file, not just one with the right
    // page count: a stale page tree can leave dangling /Kids or a wrong /Count.
    const auto check = QpdfWrapper::checkFile(out);
    EXPECT_TRUE(check.ok)
        << "the saved file failed a full structural check:"
        << joinProblems(check.problems);

    reopened.reset();
    fs::remove(work, ec);
    fs::remove(out, ec);
}

}  // namespace
