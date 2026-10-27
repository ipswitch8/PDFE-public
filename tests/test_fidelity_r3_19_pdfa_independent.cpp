// -*- coding: utf-8 -*-
//
// R3-19 — PDF/A validation is a self-referential oracle.
// ======================================================
//
// THE GAP (register finding R3-19)
// --------------------------------
// PDFE's PDF/A support is hand-rolled: `PdfAConverter` WRITES the PDF/A
// markers (XMP `pdfaid`, an sRGB `/OutputIntents` entry, embeds fonts, …)
// and `PdfAValidator` then READS those same markers back and pronounces the
// file "compliant". The validator and the converter share one code path and
// one mental model of what "PDF/A" means, so the validator can only ever
// confirm that the converter did what the converter intended — it cannot
// catch a class of defect that BOTH sides are blind to. A green
// `PdfAValidator::isCompliant()` is therefore NOT independent evidence of
// conformance; it is the converter grading its own homework.
//
// A genuine conformance oracle must come from code that does NOT descend from
// the PdfAValidator/PdfAConverter lineage. The gold standard is veraPDF (the
// PDF Association's reference PDF/A validator). This file builds an
// *independent reference* from two libraries that are each on a different code
// path from the validator, plus the veraPDF reference itself:
//
//   (1) QPDF structural check  — QpdfWrapper::checkFile() forces a full parse
//       of the object graph + cross-reference table via a DIFFERENT PDF
//       library (QPDF, Apache-2.0) than the one PdfAValidator uses (MuPDF).
//       It cannot be fooled by markers that merely LOOK right; it validates
//       that the object graph the markers live in is actually well-formed.
//
//   (2) Raw-MuPDF marker walk  — a hand-written descent of the PDF object tree
//       (Root -> /OutputIntents[0] -> /S, /DestOutputProfile ; Root ->
//       /Metadata XMP -> pdfaid:part / pdfaid:conformance) that DELIBERATELY
//       never constructs a PdfAValidator. It re-reads the exact markers from
//       first principles so a divergence between "what PdfAValidator asserts"
//       and "what is physically in the file" is observable.
//
//   (3) veraPDF reference      — the true reference validator, resolved by
//       tests/support/ConformanceChecker.{h,cpp}. meta-audit follow-up F14
//       (PDF-10) replaced the old `VERAPDF_JAR` opt-in — a variable set
//       nowhere, so the check had never run — with deterministic, variable-free
//       resolution (provisioned location first, then conventional installs; see
//       tools/conformance/README.md). The resolution is TOTAL, so Part C
//       asserts the real verdict when the checker is reachable and the recorded
//       reason plus named prerequisite when it is not. It never GTEST_SKIPs.
//       The QPDF + raw-MuPDF cross-check stays always-on regardless.
//
// HONESTY CONTRACT
// ----------------
// The convergence tests assert that the converter's OWN output survives the
// INDEPENDENT checks — not merely PdfAValidator's self-check. If the converter
// emits something PdfAValidator is happy with but an independent library
// rejects, the corresponding EXPECT_* is left to fail RED. That RED *is* the
// R3-19 gap surfacing; it must not be weakened to force green.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/QpdfWrapper.h"
#include "compliance/PdfAValidator.h"
#include "compliance/PdfAConverter.h"
#include "compliance/ComplianceTypes.h"
#include "support/ConformanceChecker.h"

// Independent raw-MuPDF descent uses its OWN fz_context (created from scratch,
// exactly like tests/support/PdfContentOracle.h) so it shares no state with
// the MuPdfContext instance the validator/converter run on.
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::compliance;

namespace {

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

// Externally-produced PDF/A-1b fixture. Crucially it was NOT round-tripped
// through PDFE's converter, so agreement between it and our independent
// reference validates the reference itself (not the converter).
std::filesystem::path externalPdfaPath() {
    // TEST_FIXTURES_DIR = <repo>/tests/fixtures ; test_PDFs is at repo root.
    return std::filesystem::path(TEST_FIXTURES_DIR)
        .parent_path()   // tests/
        .parent_path()   // repo root
        / "test_PDFs" / "crazyones-pdfa.pdf";
}

std::filesystem::path sourceDocPath() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
}

// ---------------------------------------------------------------------------
// Independent reference (2): raw-MuPDF PDF/A marker walk.
// A fresh fz_context, a hand descent of the object graph, ZERO PdfAValidator.
// ---------------------------------------------------------------------------

struct RawPdfaMarkers {
    bool opened = false;                    // file parsed as a PDF at all
    bool hasOutputIntent = false;           // Root -> /OutputIntents[0] present
    bool outputIntentSubtypeGtsPdfa = false;// its /S == GTS_PDFA1
    std::string outputIntentSubtype;        // observed /S value (for diagnostics)
    bool destOutputProfileIsStream = false; // /DestOutputProfile is a real stream
    bool hasMetadata = false;               // Root -> /Metadata present
    bool xmpHasPdfaidPart = false;          // XMP contains "pdfaid:part"
    bool xmpHasPdfaidConformance = false;   // XMP contains "pdfaid:conformance"
};

RawPdfaMarkers walkRawPdfaMarkers(const std::filesystem::path& path) {
    RawPdfaMarkers m;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return m;
    fz_register_document_handlers(ctx);

    fz_document* fdoc = nullptr;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, path.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        if (doc) {
            m.opened = true;

            pdf_obj* trailer = pdf_trailer(ctx, doc);
            pdf_obj* root    = pdf_dict_gets(ctx, trailer, "Root");

            // Root -> /OutputIntents[0]
            pdf_obj* intents = pdf_dict_gets(ctx, root, "OutputIntents");
            pdf_obj* intent0 = nullptr;
            if (pdf_is_array(ctx, intents) && pdf_array_len(ctx, intents) > 0) {
                intent0 = pdf_array_get(ctx, intents, 0);
            } else if (pdf_is_dict(ctx, intents)) {
                intent0 = intents; // tolerate a single-dict encoding
            }
            if (intent0) {
                m.hasOutputIntent = true;

                // /S must be the PDF/A output-intent subtype GTS_PDFA1 (this
                // name is identical across PDF/A parts 1/2/3).
                pdf_obj* s = pdf_dict_gets(ctx, intent0, "S");
                if (pdf_is_name(ctx, s)) {
                    const char* sn = pdf_to_name(ctx, s);
                    if (sn) {
                        m.outputIntentSubtype = sn;
                        m.outputIntentSubtypeGtsPdfa =
                            (m.outputIntentSubtype == "GTS_PDFA1");
                    }
                }

                // /DestOutputProfile must be an actual ICC profile STREAM, not
                // a dangling ref or a name — a validator that only checks for
                // the key's presence would miss a broken profile.
                pdf_obj* prof = pdf_dict_gets(ctx, intent0, "DestOutputProfile");
                if (prof && pdf_is_stream(ctx, prof)) {
                    fz_buffer* pb = pdf_load_stream(ctx, prof);
                    if (pb) {
                        m.destOutputProfileIsStream =
                            (fz_buffer_storage(ctx, pb, nullptr) > 0);
                        fz_drop_buffer(ctx, pb);
                    }
                }
            }

            // Root -> /Metadata XMP stream -> pdfaid:part / pdfaid:conformance.
            pdf_obj* meta = pdf_dict_gets(ctx, root, "Metadata");
            if (meta && pdf_is_stream(ctx, meta)) {
                m.hasMetadata = true;
                fz_buffer* mb = pdf_load_stream(ctx, meta);
                if (mb) {
                    unsigned char* d = nullptr;
                    size_t n = fz_buffer_storage(ctx, mb, &d);
                    if (d && n) {
                        std::string xmp(reinterpret_cast<const char*>(d), n);
                        m.xmpHasPdfaidPart =
                            xmp.find("pdfaid:part") != std::string::npos;
                        m.xmpHasPdfaidConformance =
                            xmp.find("pdfaid:conformance") != std::string::npos;
                    }
                    fz_drop_buffer(ctx, mb);
                }
            }
        }
    }
    fz_always(ctx) {
        if (fdoc) fz_drop_document(ctx, fdoc);
    }
    fz_catch(ctx) {
        // Leave m as-is; opened==false (or partial) signals the failure.
    }
    fz_drop_context(ctx);
    return m;
}

// ---------------------------------------------------------------------------
// veraPDF (independent reference, gold standard).
//
// meta-audit follow-up F14 (PDF-10). The local locator that used to live here
// consulted `std::getenv("VERAPDF_JAR")` and the test below `GTEST_SKIP`ped
// when it came back empty. That variable was set nowhere in the repository or
// in CI, so the gold-standard check had NEVER executed.
//
// Resolution now lives in tests/support/ConformanceChecker.{h,cpp}, is
// deterministic, consults no checker-specific variable, and is TOTAL — it
// returns either a usable checker or a concrete reason plus the named
// prerequisite. Part C below therefore asserts in both branches instead of
// skipping. See tools/conformance/README.md.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class FidelityR3_19_PdfaIndependent : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
    }
    void TearDown() override {
        context_.reset();
    }
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace

// ===========================================================================
// Part A — establish the independent reference against a KNOWN-GOOD external
// PDF/A that never touched PDFE's converter. If our reference cannot agree
// that an externally-produced PDF/A is PDF/A-shaped, the reference is broken
// and nothing downstream is trustworthy.
// ===========================================================================

// Targets the self-referential weakness directly: an entirely different PDF
// library (QPDF, not MuPDF) confirms the object graph / xref of a real PDF/A
// is structurally sound — evidence PdfAValidator never produces.
TEST_F(FidelityR3_19_PdfaIndependent, ExternalPdfa_QpdfStructureOk) {
    auto path = externalPdfaPath();
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "crazyones-pdfa.pdf not found at " << path;
    }

    pdfe::core::StructureCheck check = pdfe::core::QpdfWrapper::checkFile(path);
    EXPECT_TRUE(check.ok)
        << "Independent QPDF structural check FAILED on a known-good external "
           "PDF/A — first problem: "
        << (check.problems.empty() ? std::string("(none reported)")
                                   : check.problems.front());
}

// Targets the weakness that PdfAValidator reads markers written by its own
// lineage: here a from-scratch MuPDF object walk re-derives the same markers
// on an external file and confirms they are physically present + well-typed.
TEST_F(FidelityR3_19_PdfaIndependent, ExternalPdfa_RawMupdfMarkersPresent) {
    auto path = externalPdfaPath();
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "crazyones-pdfa.pdf not found at " << path;
    }

    RawPdfaMarkers m = walkRawPdfaMarkers(path);
    ASSERT_TRUE(m.opened) << "Raw MuPDF could not parse the external PDF/A";

    EXPECT_TRUE(m.hasOutputIntent)
        << "External PDF/A is missing Root -> /OutputIntents[0]";
    EXPECT_TRUE(m.outputIntentSubtypeGtsPdfa)
        << "OutputIntent /S expected GTS_PDFA1, observed '"
        << m.outputIntentSubtype << "'";
    EXPECT_TRUE(m.destOutputProfileIsStream)
        << "/DestOutputProfile is not a non-empty ICC stream";
    EXPECT_TRUE(m.hasMetadata)
        << "External PDF/A is missing Root -> /Metadata (XMP)";
    EXPECT_TRUE(m.xmpHasPdfaidPart)
        << "XMP does not contain pdfaid:part";
    EXPECT_TRUE(m.xmpHasPdfaidConformance)
        << "XMP does not contain pdfaid:conformance";
}

// ===========================================================================
// Part B — CONVERGENCE. Convert a plain doc to PDF/A with PDFE's OWN converter,
// then judge the OUTPUT with the INDEPENDENT reference (QPDF + raw MuPDF), NOT
// with PdfAValidator. This is where R3-19 bites: if the converter's output
// pleases PdfAValidator but fails an independent library, the EXPECT_* below
// go RED and that is the finding — do not weaken them.
// ===========================================================================

// Converter output must survive an independent QPDF object-graph/xref parse.
// PdfAValidator never performs this check, so a structural defect the
// converter introduces (dangling ref, broken xref after full-rewrite) would
// otherwise ship "validated".
TEST_F(FidelityR3_19_PdfaIndependent, ConverterOutput_QpdfStructureOk) {
    auto src = sourceDocPath();
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf fixture not found";
    }

    auto doc = pdfe::core::Document::open(context_, src);
    ASSERT_NE(doc, nullptr);

    auto outPath = std::filesystem::temp_directory_path()
        / "r3_19_converter_qpdf.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    PdfAConverter converter(context_);
    ComplianceResult conv =
        converter.convertAndSave(doc.get(), PdfALevel::PdfA_2b, outPath);
    doc.reset();

    ASSERT_TRUE(std::filesystem::exists(outPath))
        << "Converter did not write an output file"
        << (conv.errorMessage.empty() ? "" : (": " + conv.errorMessage));

    pdfe::core::StructureCheck check = pdfe::core::QpdfWrapper::checkFile(outPath);
    EXPECT_TRUE(check.ok)
        << "R3-19: converter output failed an INDEPENDENT QPDF structural "
           "check that PdfAValidator does not perform — first problem: "
        << (check.problems.empty() ? std::string("(none reported)")
                                   : check.problems.front());

    std::filesystem::remove(outPath, ec);
}

// Converter output must carry the PDF/A markers as verified by a from-scratch
// MuPDF walk (NOT PdfAValidator), AND — the decisive R3-19 assertion — when
// PdfAValidator pronounces the output compliant, the independent walk must
// agree. A pass on the left with a fail on the right is the self-referential
// gap made visible.
TEST_F(FidelityR3_19_PdfaIndependent, ConverterOutput_IndependentMarkersConverge) {
    auto src = sourceDocPath();
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf fixture not found";
    }

    auto doc = pdfe::core::Document::open(context_, src);
    ASSERT_NE(doc, nullptr);

    auto outPath = std::filesystem::temp_directory_path()
        / "r3_19_converter_markers.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    const PdfALevel level = PdfALevel::PdfA_2b;

    PdfAConverter converter(context_);
    ComplianceResult conv = converter.convertAndSave(doc.get(), level, outPath);
    doc.reset();

    ASSERT_TRUE(std::filesystem::exists(outPath))
        << "Converter did not write an output file"
        << (conv.errorMessage.empty() ? "" : (": " + conv.errorMessage));

    // The converter's OWN self-check (the self-referential oracle under test).
    auto reopened = pdfe::core::Document::open(context_, outPath);
    ASSERT_NE(reopened, nullptr);
    PdfAValidator validator(context_);
    bool selfSaysCompliant = validator.isCompliant(reopened.get(), level);
    reopened.reset();

    // The INDEPENDENT reference (different code path, from first principles).
    RawPdfaMarkers m = walkRawPdfaMarkers(outPath);
    ASSERT_TRUE(m.opened) << "Raw MuPDF could not parse the converter output";

    // These four are the physical markers PDF/A requires. If the converter
    // wrote them, they are present regardless of PdfAValidator's opinion.
    EXPECT_TRUE(m.hasOutputIntent)
        << "R3-19: converter output lacks /OutputIntents";
    EXPECT_TRUE(m.outputIntentSubtypeGtsPdfa)
        << "R3-19: converter OutputIntent /S is '" << m.outputIntentSubtype
        << "', expected GTS_PDFA1";
    EXPECT_TRUE(m.destOutputProfileIsStream)
        << "R3-19: converter output /DestOutputProfile is not a real ICC stream";
    EXPECT_TRUE(m.xmpHasPdfaidPart && m.xmpHasPdfaidConformance)
        << "R3-19: converter XMP lacks pdfaid:part/conformance "
           "(part=" << m.xmpHasPdfaidPart
        << ", conformance=" << m.xmpHasPdfaidConformance << ")";

    // THE convergence assertion: the self-referential validator must not claim
    // compliance that the independent marker walk cannot corroborate. If
    // selfSaysCompliant is true, every independent marker must also hold.
    if (selfSaysCompliant) {
        bool independentAgrees =
            m.hasOutputIntent && m.outputIntentSubtypeGtsPdfa &&
            m.destOutputProfileIsStream &&
            m.xmpHasPdfaidPart && m.xmpHasPdfaidConformance;
        EXPECT_TRUE(independentAgrees)
            << "R3-19 SURFACED: PdfAValidator.isCompliant()==true but the "
               "INDEPENDENT raw-MuPDF marker walk disagrees. The validator is "
               "confirming its own converter's intent, not real conformance.";
    }

    std::filesystem::remove(outPath, ec);
}

// ===========================================================================
// Part C — veraPDF gold-standard reference (F14 / PDF-10).
//
// No longer optional and no longer skip-guarded. The checker is resolved by
// tests/support/ConformanceChecker.cpp without any human-supplied variable, and
// the resolution is TOTAL, so this test asserts in both branches: the real
// verdict when the checker is reachable, and the recorded reason plus named
// prerequisite when it is not. The outcome is written to the phase-14
// result-or-reason record either way.
// ===========================================================================

// PDFE-F45-CI-ONLY: FidelityR3_19_PdfaIndependent.ExternalPdfa_VeraPdfReference
// deferred-backlog row F45, closed in follow-up phase-46. RED IN CI ONLY,
// green locally, genuine (failed every rerun), and pre-existing.
// CAUSE — NOT what the backlog row assumed. The row filed this as a veraPDF
// availability problem. It is not: the `else` branch below handles an absent
// checker as an honest PASS, so unavailability can never redden this test.
// The only hard failure path is the ASSERT_TRUE(exists) on the INPUT,
// test_PDFs/crazyones-pdfa.pdf, which `.gitignore` excluded wholesale as part
// of the "large binary collection". The two sibling tests in this file that
// use the same input GTEST_SKIP instead, which is exactly why they were not in
// the failing set — the failing set is precisely the hard-asserting subset.
// FIX: the 16 KB input is now re-included and committed. Assertions unchanged.
// See docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md.
TEST_F(FidelityR3_19_PdfaIndependent, ExternalPdfa_VeraPdfReference) {
    const auto& resolution = pdfe::test::resolveConformanceChecker();
    auto path = externalPdfaPath();
    ASSERT_TRUE(std::filesystem::exists(path))
        << "the known-good external PDF/A is missing: " << path;

    pdfe::test::CheckerVerdict v = pdfe::test::runConformanceCheck(path, "1b");
    v.file = "crazyones-pdfa.pdf (R3-19 gold-standard reference)";
    pdfe::test::recordConformanceResult(v);

    if (resolution.available) {
        ASSERT_TRUE(v.ran)
            << "the checker resolved but did not produce a verdict: "
            << v.reason;
        EXPECT_TRUE(v.valid)
            << "veraPDF (independent gold standard) rejected a known-good "
               "external PDF/A-1b:\n"
            << v.output;
    } else {
        // Honest unavailability — asserted, never skipped (F14 criterion 4).
        EXPECT_FALSE(v.ran);
        EXPECT_FALSE(resolution.reason.empty())
            << "an unavailable checker must state why";
        EXPECT_FALSE(resolution.prerequisite.empty())
            << "an unavailable checker must name the prerequisite; see "
               "tools/conformance/README.md";
    }
}
