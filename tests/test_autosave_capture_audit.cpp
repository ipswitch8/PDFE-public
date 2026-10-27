// -*- coding: utf-8 -*-
//
// Autosave-coverage pipeline — Phase 1 DISCOVERY PROBE.
//
// WHAT THIS IS
// ------------
// A MEASUREMENT INSTRUMENT, not a correctness suite. For every edit family it
// records four booleans and emits a machine-readable gap register. It answers
// one question per family:
//
//     "When the user makes THIS kind of edit and the app then crashes, does the
//      autosave / crash-recovery feature capture that edit?"
//
// WHY IT MATTERS (the defect class under audit)
// ---------------------------------------------
// AutosaveManager::autosaveNow() writes a recovery copy ONLY when
// undoManager_->isClean() == false (see src/core/AutosaveManager.cpp:101).
// Capture itself is Document::writeRecoveryCopy() — a full pdf_save_document
// snapshot of the live in-memory pdfDoc. Therefore ANY edit path that mutates
// the document WITHOUT pushing an UndoManager command leaves isClean() == true,
// so autosave NEVER fires, so the edit is silently lost on crash.
//
// This probe drives each family the way PRODUCTION's UI drives it — through a
// real UndoManager for families that have a command class, via the direct
// manager API for families whose only/real production path is a bare mutator
// (redaction burn-in, page labels, direct layer ops, signature signing). That
// undo-vs-direct distinction is the whole point of the measurement.
//
// EXPECTED RESULT (do NOT "fix" these here — Phase 1 is honest measurement)
// -------------------------------------------------------------------------
// It is expected and correct for the register to show FALSE capture/restore
// cells for redaction-apply, page-labels, direct-layer, and signature-signing.
// Those are the confirmed gaps. Correctness assertions + fixes come in later
// phases. Every GoogleTest body here PASSES as long as the measurement ran; a
// family that fails to trigger/capture is DATA in the register, NOT a red test.
//
// OUTPUT
// ------
//   (a) a readable table printed to stdout (via the global environment teardown)
//   (b) JSON at
//       .claude/pipelines/autosave-coverage/gate-artifacts/phase-1/gap-register.json
//   (c) RecordProperty(...) per-family so each row is visible in the JUnit XML.

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "core/TextExtractor.h"

// Text edit
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

// Annotations
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"

// Redaction
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"

// Image
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "image/ImageCommands.h"

// Page ops
#include "core/PageCommands.h"

// Forms
#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"

// Metadata
#include "metadata/MetadataManager.h"
#include "metadata/MetadataCommands.h"

// Bookmarks
#include "core/BookmarkManager.h"
#include "core/BookmarkNode.h"

// Page labels
#include "core/PageLabels.h"

// Layers
#include "layers/LayerManager.h"

// Signatures
#include "signatures/SignatureManager.h"
#include "signatures/SignatureCommands.h"
#include "signatures/SignatureData.h"

// TRE
#include "tre/TreCommands.h"
#include "tre/TreTypes.h"
#include "tre/PdfContentWriter.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/QualityAssessor.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/PdfFontEmbedder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::commands::UndoManager;
using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;

// ─────────────────────────────────────────────────────────────────────────────
// The gap register: one row per edit family.
// ─────────────────────────────────────────────────────────────────────────────
struct ResultRow {
    std::string family;
    std::string seed;
    bool applied_via_undo = false;   ///< did we drive it through UndoManager?
    bool trigger_undo_dirty = false; ///< !undo->isClean() after the apply
    bool trigger_autosave_fired = false; ///< mgr.autosaveNow() return value
    bool capture_present = false;    ///< recovery file exists AND oracle finds edit
    bool restore_present = false;    ///< reopen recovery copy AND oracle finds edit
    bool probed = true;              ///< false → could not drive this family
    std::string note;                ///< blocker/reason when not fully probed
};

// Global registry, filled as each TEST runs; emitted by the environment.
std::vector<ResultRow>& registry() {
    static std::vector<ResultRow> rows;
    return rows;
}
std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// R-09 REMEDIATION (phase-3): this file began life (phase-1) as an assertion-
// FREE measurement probe — every family recorded capture_present but the body
// only ever SUCCEED()ed, so a regression that stopped autosave from capturing
// an edit family would sail through green. The autosave-coverage pipeline has
// since FIXED capture for every edit family that CAN be captured. This probe is
// now a REAL ORACLE: each probed family MUST assert capture_present == YES,
// except an explicit, documented allowlist of families that bypass autosave BY
// DESIGN (whose by-design absence is itself LOCKED with EXPECT_FALSE so it can't
// silently flip). Environmental blockers (missing fixture / tessdata / signer
// bridge) are honest SKIPs, never silent passes.
// ─────────────────────────────────────────────────────────────────────────────

// Allowlist: families whose edit persists through their OWN path and therefore
// legitimately is NOT captured by the undo/autosave model. Each entry is a
// documented design decision, not a gap:
//   SignatureSign — SignatureManager::signField() performs its own incremental
//   save to the source file (SignatureManager.h:48, not undoable by design), so
//   isClean() stays true and autosave never fires. The signature is on disk via
//   its own path; autosave capture is neither expected nor required.
bool captureByDesignAbsent(const std::string& family) {
    return family == "SignatureSign";
}

// Record a row into the register + JUnit RecordProperty, THEN assert the
// capture verdict. Call from inside a TEST body (the streamed EXPECT/SKIP is
// attributed to the running test).
void finishRow(const ResultRow& r) {
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        registry().push_back(r);
    }
    ::testing::Test::RecordProperty("family", r.family);
    ::testing::Test::RecordProperty("seed", r.seed);
    ::testing::Test::RecordProperty("applied_via_undo", r.applied_via_undo ? 1 : 0);
    ::testing::Test::RecordProperty("trigger_undo_dirty", r.trigger_undo_dirty ? 1 : 0);
    ::testing::Test::RecordProperty("trigger_autosave_fired", r.trigger_autosave_fired ? 1 : 0);
    ::testing::Test::RecordProperty("capture_present", r.capture_present ? 1 : 0);
    ::testing::Test::RecordProperty("restore_present", r.restore_present ? 1 : 0);
    ::testing::Test::RecordProperty("probed", r.probed ? 1 : 0);
    if (!r.note.empty()) {
        ::testing::Test::RecordProperty("note", r.note);
    }

    // Environmental blocker (missing seed/tessdata/pfx/bridge, no spans, etc.):
    // the family could not be measured. That is a genuine SKIP, not a capture
    // verdict — and NOT a silent pass. The blocker is documented in r.note.
    if (!r.probed) {
        GTEST_SKIP() << "family '" << r.family
                     << "' not probed (environmental blocker): " << r.note;
    }

    // By-design no-capture allowlist: LOCK the documented behavior so it cannot
    // silently flip in either direction. If this family ever starts capturing,
    // that is a change worth re-examining (update the allowlist deliberately).
    if (captureByDesignAbsent(r.family)) {
        EXPECT_FALSE(r.capture_present)
            << "family '" << r.family << "' is on the documented by-design "
               "no-capture allowlist (persists via its own incremental save, "
               "bypassing the undo/autosave model) yet autosave captured it. "
               "If this is now an undoable/autosave path, remove it from the "
               "allowlist so the positive oracle applies.";
        return;
    }

    // The real oracle: every probed, capturable edit family MUST be captured by
    // autosave. A NO here means the edit is silently lost on crash — a real
    // crash-recovery gap.
    EXPECT_TRUE(r.capture_present)
        << "AUTOSAVE CAPTURE GAP for edit family '" << r.family << "': the edit "
           "was applied (undo_dirty=" << r.trigger_undo_dirty
        << ", autosave_fired=" << r.trigger_autosave_fired
        << ") but the recovery copy did NOT carry it (capture_present=NO), so it "
           "would be silently lost on crash."
        << (r.note.empty() ? "" : (" note=" + r.note));
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-family isolated environment: temp seed copy + isolated recovery dir +
// live Document + UndoManager + AutosaveManager. Mirrors the fixture in
// tests/test_autosave_recovery.cpp.
// ─────────────────────────────────────────────────────────────────────────────
struct Probe {
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path tempPdf;
    std::filesystem::path recoveryDir;
    std::unique_ptr<Document> doc;
    std::unique_ptr<UndoManager> undo;
    std::unique_ptr<AutosaveManager> mgr;
    bool ok = false;
    std::string skipReason;

    Probe(const std::string& family, const std::string& seed) {
        ctx = std::make_shared<MuPdfContext>();
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / seed;
        std::error_code ec;
        if (!std::filesystem::exists(fixture, ec)) {
            skipReason = "seed fixture not available: " + seed;
            return;
        }
        const auto tmp = std::filesystem::temp_directory_path();
        tempPdf = tmp / ("pdfe_autosave_audit_" + family + ".pdf");
        recoveryDir = tmp / ("pdfe_autosave_audit_rec_" + family);
        std::filesystem::remove_all(recoveryDir, ec);
        std::filesystem::create_directories(recoveryDir, ec);
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            skipReason = "temp copy failed: " + ec.message();
            return;
        }
        doc = Document::open(ctx, tempPdf);
        if (!doc) {
            skipReason = "Document::open failed on temp copy";
            return;
        }
        undo = std::make_unique<UndoManager>();
        mgr = std::make_unique<AutosaveManager>(doc.get(), undo.get(),
                                                tempPdf, recoveryDir);
        ok = true;
    }

    ~Probe() {
        mgr.reset();
        undo.reset();
        doc.reset();
        ctx.reset();
        std::error_code ec;
        std::filesystem::remove_all(recoveryDir, ec);
        std::filesystem::remove(tempPdf, ec);
    }

    // Fill the trigger + capture fields of `r` from the current live state and
    // the given content oracle (a predicate on a recovery-copy path).
    template <typename Oracle>
    void measure(ResultRow& r, Oracle oracle) {
        r.trigger_undo_dirty = !undo->isClean();
        r.trigger_autosave_fired = mgr->autosaveNow();
        const auto recPath = mgr->recoveryFilePath();
        std::error_code ec;
        const bool fileWritten = std::filesystem::exists(recPath, ec);
        // capture = the autosave wrote a recovery copy AND the family's edit is
        // present inside it.
        r.capture_present = fileWritten && oracle(recPath);
        // restore = independently reopen the recovery copy and the oracle still
        // finds the edit (a fresh process's recovery path). Only meaningful when
        // a copy was actually written.
        r.restore_present = fileWritten && oracle(recPath);

        // Release any command now, while the caller's manager/page locals are
        // still alive. The UndoManager lives inside this Probe (destroyed last),
        // so a command left on the stack would otherwise be destroyed AFTER the
        // manager/page it references — a teardown UAF. Clearing here keeps the
        // command's lifetime strictly inside the manager's.
        undo->clear();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Content oracles. Each opens the given path FRESH (own context) — mirroring a
// recovering process — and reports whether the family's edit is present.
// ─────────────────────────────────────────────────────────────────────────────

bool oracleText(const std::filesystem::path& path, const std::u32string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::core::TextExtractor extractor(ctx);
    auto blocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel model(blocks);
    for (int i = 0; i < model.spanCount(); ++i) {
        if (model.span(i).text == wanted) return true;
    }
    return false;
}

bool oracleAnnotationOfType(const std::filesystem::path& path,
                            pdfe::annotations::AnnotationType wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::annotations::AnnotationManager mgr(ctx);
    for (const auto& a : mgr.getAnnotations(page.get())) {
        if (a.type == wanted) return true;
    }
    return false;
}

bool oracleRedactionMarkPresent(const std::filesystem::path& path) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::redaction::RedactionManager mgr(ctx);
    return !mgr.getRedactionMarks(page.get()).empty();
}

bool oracleRedactionApplied(const std::filesystem::path& path,
                            const std::vector<pdfe::redaction::RedactionData>& regions) {
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::redaction::RedactionVerifier verifier(ctx);
    auto res = verifier.verify(path, regions);
    return res.success;
}

bool oracleImageCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::image::ImageManager mgr(ctx);
    return mgr.imageCount(page.get(), doc.get()) >= minCount;
}

bool oracleRotation(const std::filesystem::path& path, int wantedDegrees) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    // Normalise both to [0,360).
    auto norm = [](int d) { d %= 360; if (d < 0) d += 360; return d; };
    return norm(page->rotation()) == norm(wantedDegrees);
}

bool oracleFormValue(const std::filesystem::path& path, int widgetIndex,
                     const std::string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc || doc->pageCount() == 0) return false;
    auto page = doc->page(0);
    if (!page) return false;
    pdfe::forms::FormManager mgr(ctx);
    return mgr.getFormField(page.get(), widgetIndex).value == wanted;
}

bool oracleMetaTitle(const std::filesystem::path& path, const std::string& wanted) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::metadata::MetadataManager mgr(ctx);
    return mgr.readMetadata(doc.get()).title == wanted;
}

bool oracleBookmarkCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::core::BookmarkManager mgr(ctx);
    mgr.load(doc.get());
    return mgr.totalBookmarkCount() >= minCount;
}

bool oraclePageLabelPresent(const std::filesystem::path& path) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    return !pdfe::core::PageLabelManager::read(doc.get()).empty();
}

bool oracleLayerNamed(const std::filesystem::path& path, const std::string& name) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::layers::LayerManager mgr(ctx);
    for (const auto& l : mgr.listLayers(doc.get())) {
        if (l.name == name) return true;
    }
    return false;
}

bool oracleSignatureCountAtLeast(const std::filesystem::path& path, int minCount) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::signatures::SignatureManager mgr(ctx);
    return mgr.countSignatures(doc.get()) >= minCount;
}

bool oracleSignedSignaturePresent(const std::filesystem::path& path) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    if (!doc) return false;
    pdfe::signatures::SignatureManager mgr(ctx);
    for (const auto& s : mgr.listSignatures(doc.get())) {
        if (s.isSigned) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Text edit — production path is EditTextCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, TextEdit) {
    ResultRow r;
    r.family = "TextEdit";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;

    Probe p("TextEdit", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    // Read span 0's current text, then edit it to text + ".EDIT".
    pdfe::textedit::TextSpan span;
    std::u32string original;
    {
        auto page = p.doc->page(0);
        pdfe::core::TextExtractor extractor(p.ctx);
        auto blocks = extractor.extract(page.get());
        pdfe::textedit::TextEditModel m(blocks);
        if (m.spanCount() == 0) {
            r.probed = false; r.note = "no text spans in seed"; finishRow(r); return;
        }
        span = m.span(0);
        original = span.text;
    }
    const std::u32string edited = original + U".EDIT.AUTOSAVE";
    p.undo->execute(std::make_unique<pdfe::textedit::EditTextCommand>(
        p.doc.get(), p.ctx, /*pageIndex=*/0, /*spanIndex=*/0, span, original, edited));

    p.measure(r, [&](const std::filesystem::path& rp) { return oracleText(rp, edited); });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Annotations — production path is CreateAnnotationCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, Annotations) {
    ResultRow r;
    r.family = "Annotations";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;

    Probe p("Annotations", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::annotations::AnnotationManager amgr(p.ctx);
    auto page = p.doc->page(0);
    p.undo->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        &amgr, page.get(), pdfe::annotations::AnnotationType::Square,
        QRectF(100, 100, 200, 80)));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleAnnotationOfType(rp, pdfe::annotations::AnnotationType::Square);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Redaction MARK — production path is CreateRedactionCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, RedactionMark) {
    ResultRow r;
    r.family = "RedactionMark";
    r.seed = "test_redaction.pdf";
    r.applied_via_undo = true;

    Probe p("RedactionMark", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::redaction::RedactionManager rmgr(p.ctx);
    auto page = p.doc->page(0);
    p.undo->execute(std::make_unique<pdfe::redaction::CreateRedactionCommand>(
        &rmgr, page.get(), QRectF(100, 100, 100, 50)));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleRedactionMarkPresent(rp);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Redaction APPLY / burn-in — SUSPECTED GAP.
// Production path is RedactionManager::applyAllRedactions() DIRECT (no command).
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, RedactionApply) {
    ResultRow r;
    r.family = "RedactionApply";
    r.seed = "test_redaction.pdf";
    r.applied_via_undo = false;  // there is no apply command; direct is the real path

    Probe p("RedactionApply", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::redaction::RedactionManager rmgr(p.ctx);
    std::vector<pdfe::redaction::RedactionData> regions;
    {
        auto page = p.doc->page(0);
        // Mark over a text area, then burn it in DIRECTLY (no undo command).
        auto created = rmgr.createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        regions.push_back(created);
        auto applyResult = rmgr.applyAllRedactions(p.doc.get());
        r.note = applyResult.success
                     ? ("applied marks=" + std::to_string(applyResult.marksApplied))
                     : ("applyAllRedactions failed: " + applyResult.errorMessage);
    }

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleRedactionApplied(rp, regions);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Image — production path is InsertImageCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, Image) {
    ResultRow r;
    r.family = "Image";
    r.seed = "test_pdf_with_images.pdf";
    r.applied_via_undo = true;

    Probe p("Image", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    const std::filesystem::path png =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
    if (!std::filesystem::exists(png)) {
        r.probed = false; r.note = "test_image_rgb.png not available"; finishRow(r); return;
    }

    pdfe::image::ImageManager imgr(p.ctx);
    auto page = p.doc->page(0);
    const int baseline = imgr.imageCount(page.get(), p.doc.get());

    pdfe::image::InsertImageOptions opts;
    opts.filePath = png.string();
    opts.targetRect = QRectF(60.0, 60.0, 120.0, 90.0);
    p.undo->execute(std::make_unique<pdfe::image::InsertImageCommand>(
        &imgr, page.get(), p.doc.get(), opts));

    r.note = "baseline images=" + std::to_string(baseline);
    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleImageCountAtLeast(rp, baseline + 1);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Page ops — production path is RotatePageCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, PageOps) {
    ResultRow r;
    r.family = "PageOps";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;

    Probe p("PageOps", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    int baseRotation = 0;
    {
        auto page = p.doc->page(0);
        baseRotation = page->rotation();
    }
    p.undo->execute(std::make_unique<pdfe::commands::RotatePageCommand>(
        p.doc.get(), /*pageIndex=*/0, /*angleDelta=*/90));

    const int expected = baseRotation + 90;
    r.note = "base rotation=" + std::to_string(baseRotation);
    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleRotation(rp, expected);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Form fill — production path is SetFieldValueCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, FormFill) {
    ResultRow r;
    r.family = "FormFill";
    r.seed = "test_form.pdf";
    r.applied_via_undo = true;

    Probe p("FormFill", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::forms::FormManager fmgr(p.ctx);
    auto page = p.doc->page(0);

    int widgetIdx = -1;
    for (const auto& f : fmgr.getFormFields(page.get())) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            widgetIdx = f.widgetIndex;
            break;
        }
    }
    if (widgetIdx < 0) {
        r.probed = false; r.note = "no editable text field in seed"; finishRow(r); return;
    }

    const std::string newValue = "AUTOSAVE_PROBE_VALUE";
    p.undo->execute(std::make_unique<pdfe::forms::SetFieldValueCommand>(
        &fmgr, page.get(), widgetIdx, newValue));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleFormValue(rp, widgetIdx, newValue);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Metadata — production path is EditMetadataCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, Metadata) {
    ResultRow r;
    r.family = "Metadata";
    r.seed = "test_single.pdf";
    r.applied_via_undo = true;

    Probe p("Metadata", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::metadata::MetadataManager mmgr(p.ctx);
    auto meta = mmgr.readMetadata(p.doc.get());
    const std::string probeTitle = "AUTOSAVE_PROBE_TITLE";
    meta.title = probeTitle;
    p.undo->execute(std::make_unique<pdfe::metadata::EditMetadataCommand>(
        &mmgr, p.doc.get(), meta));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleMetaTitle(rp, probeTitle);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Bookmarks — production path is CreateBookmarkCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, Bookmarks) {
    ResultRow r;
    r.family = "Bookmarks";
    r.seed = "test_with_bookmarks.pdf";
    r.applied_via_undo = true;

    Probe p("Bookmarks", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::core::BookmarkManager bmgr(p.ctx);
    bmgr.load(p.doc.get());
    const int before = bmgr.totalBookmarkCount();

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = 0;
    p.undo->execute(std::make_unique<pdfe::core::CreateBookmarkCommand>(
        &bmgr, pdfe::core::BookmarkPath{}, "AutosaveProbeBookmark", dest));

    // The bookmark tree lives in the manager and is committed to the PDF only
    // on save. Production wires AutosaveManager::setPreSnapshotFlush so the tree
    // is committed to the live document just before each recovery snapshot;
    // mirror that here so the probe measures the real production path.
    p.mgr->setPreSnapshotFlush([&]() { bmgr.commit(p.doc.get()); });

    r.note = "bookmarks before=" + std::to_string(before);
    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleBookmarkCountAtLeast(rp, before + 1);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Page labels — FIXED (Phase-2).
// Production path is now EditPageLabelsCommand via UndoManager (PageLabelsDialog
// routes accept() through the undo stack). The probe measures that real path.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, PageLabels) {
    ResultRow r;
    r.family = "PageLabels";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;  // now routed through EditPageLabelsCommand

    Probe p("PageLabels", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    std::vector<pdfe::core::PageLabelRange> ranges = {
        {0, pdfe::core::PageLabelStyle::RomanUpper, "", 1},
        {1, pdfe::core::PageLabelStyle::Decimal, "Ch-", 5},
    };
    p.undo->execute(std::make_unique<pdfe::core::EditPageLabelsCommand>(
        p.doc.get(), ranges));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oraclePageLabelPresent(rp);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Layers (undoable) — CreateLayerCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, LayersUndoable) {
    ResultRow r;
    r.family = "LayersUndoable";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;

    Probe p("LayersUndoable", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::layers::LayerManager lmgr(p.ctx);
    const std::string layerName = "AutosaveProbeLayerUndoable";
    p.undo->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
        &lmgr, p.doc.get(), layerName));

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleLayerNamed(rp, layerName);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Layers (direct) — FIXED (Phase-2).
// The Phase-1 gap was an artifact of driving LayerManager::createLayer()
// DIRECTLY. Production's LayerPanel already routes every layer mutation through
// the undoable commands (CreateLayer/SetLayerVisibility/Rename/DeleteLayer) —
// its undoManager_ is wired in MainWindow — so the probe now measures that real
// path (verified via grep: no direct layer mutators outside the null-undo
// fallback exist in production).
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, LayersDirect) {
    ResultRow r;
    r.family = "LayersDirect";
    r.seed = "test_3page.pdf";
    r.applied_via_undo = true;  // production routes through CreateLayerCommand

    Probe p("LayersDirect", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::layers::LayerManager lmgr(p.ctx);
    const std::string layerName = "AutosaveProbeLayerDirect";
    p.undo->execute(std::make_unique<pdfe::layers::CreateLayerCommand>(
        &lmgr, p.doc.get(), layerName));
    r.note = "created via CreateLayerCommand";

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleLayerNamed(rp, layerName);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Signature field create — CreateSignatureFieldCommand via UndoManager.
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, SignatureFieldCreate) {
    ResultRow r;
    r.family = "SignatureFieldCreate";
    r.seed = "test_single.pdf";
    r.applied_via_undo = true;

    Probe p("SignatureFieldCreate", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    pdfe::signatures::SignatureManager smgr(p.ctx);
    auto page = p.doc->page(0);
    const int before = smgr.countSignatures(p.doc.get());
    p.undo->execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
        &smgr, page.get(), "AutosaveProbeSigField", QRectF(100, 500, 200, 50)));

    r.note = "signatures before=" + std::to_string(before);
    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleSignatureCountAtLeast(rp, before + 1);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: Signature signing — SUSPECTED GAP.
// SignatureManager::signField() is NOT undoable by design (SignatureManager.h:48).
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, SignatureSign) {
    ResultRow r;
    r.family = "SignatureSign";
    r.seed = "test_single.pdf";
    r.applied_via_undo = false;

    Probe p("SignatureSign", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    const std::filesystem::path pfx =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
    if (!std::filesystem::exists(pfx)) {
        r.probed = false; r.note = "test_signer.pfx not available"; finishRow(r); return;
    }

    pdfe::signatures::SignatureManager smgr(p.ctx);
    if (!smgr.isPkcs7BridgeAvailable()) {
        r.probed = false; r.note = "PKCS7 bridge unavailable"; finishRow(r); return;
    }

    // Setup: create a signature field DIRECTLY (the field to sign). This mirrors
    // the field existing in the document; the operation under audit is the
    // SIGN, which is NOT undoable by design (SignatureManager.h:48).
    auto page = p.doc->page(0);
    const int widgetIdx = 0;
    smgr.createSignatureField(page.get(), "AutosaveProbeSignMe",
                              QRectF(100, 500, 200, 50));

    pdfe::signatures::SigningOptions opts;
    opts.pfxPath = pfx.string();
    opts.pfxPassword = "test123";  // fixture password (see test_signature_manager.cpp)
    opts.reason = "Autosave probe";
    opts.location = "Phase 1";

    try {
        // Signing goes DIRECT — no command is pushed onto the undo stack. Note:
        // signField() performs its OWN incremental save to the source file, so
        // the signature persists to disk THROUGH ITS OWN PATH, entirely bypassing
        // the undo/autosave model — which is exactly why autosave never fires.
        smgr.signField(p.doc.get(), page.get(), widgetIdx, opts);
        r.note = "signField completed (incremental-save side effect)";
    } catch (const std::exception& e) {
        // A genuine blocker (bad password / bridge error). Record honestly.
        r.probed = false;
        r.note = std::string("signField threw: ") + e.what();
        finishRow(r);
        return;
    }

    p.measure(r, [&](const std::filesystem::path& rp) {
        return oracleSignedSignaturePresent(rp);
    });
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// FAMILY: TRE scanned-text reconstruction — ImageReplaceCommand via UndoManager.
// Heavy: needs the scanned fixture + tessdata + full font stack. If either is
// unavailable, records "not-probed" with the concrete blocker (never faked).
// ─────────────────────────────────────────────────────────────────────────────
TEST(AutosaveCaptureAudit, TreScannedText) {
    ResultRow r;
    r.family = "TreScannedText";
    r.seed = "test_scanned.pdf";
    r.applied_via_undo = true;

    const std::filesystem::path tessdata =
        std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata";
    if (!std::filesystem::exists(tessdata / "eng.traineddata")) {
        r.probed = false; r.note = "tessdata/eng.traineddata not available";
        finishRow(r); return;
    }

    Probe p("TreScannedText", r.seed);
    if (!p.ok) { r.probed = false; r.note = p.skipReason; finishRow(r); return; }

    // Build the full TRE reconstruction stack (mirrors test_tre_undo_after_save
    // Oracle 2). Any construction/execute failure is a genuine blocker.
    try {
        auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(p.ctx.get());
        auto fontCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            analyzer.get(), fontCache.get());
        auto embedder = std::make_unique<pdfe::fonts::PdfFontEmbedder>(p.ctx.get());

        pdfe::tre::OcrConfig ocrConfig;
        ocrConfig.tessdataPath = tessdata.string();
        auto ocrPipeline = std::make_unique<pdfe::tre::OcrPipeline>(
            p.ctx.get(), p.doc.get(), ocrConfig);
        auto coordMapper = std::make_unique<pdfe::tre::CoordMapper>(
            p.ctx.get(), p.doc.get());
        auto bgDetector = std::make_unique<pdfe::tre::BgColorDetector>();
        auto fontMatcher = std::make_unique<pdfe::tre::FontMatcher>(
            selector.get(), analyzer.get());
        auto imageMasker = std::make_unique<pdfe::tre::ImageMasker>();
        auto bitmapCompositor = std::make_unique<pdfe::tre::BitmapCompositor>();
        auto contentWriter = std::make_unique<pdfe::tre::PdfContentWriter>(
            p.ctx.get(), embedder.get());
        auto imageReplacer = std::make_unique<pdfe::tre::ImageReplacer>(
            ocrPipeline.get(), coordMapper.get(), bgDetector.get(),
            fontMatcher.get(), imageMasker.get(), bitmapCompositor.get(),
            contentWriter.get());

        // Capture original image bytes for the change oracle.
        std::vector<uint8_t> originalData;
        {
            auto page = p.doc->page(0);
            originalData = contentWriter->getImageXObjectData(page.get(), p.doc.get());
        }
        if (originalData.empty()) {
            r.probed = false; r.note = "no image XObject data on page 0";
            finishRow(r); return;
        }

        pdfe::tre::PdfRect selection{50, 50, 500, 700};
        p.undo->execute(std::make_unique<pdfe::tre::ImageReplaceCommand>(
            p.doc.get(), p.ctx, imageReplacer.get(), contentWriter.get(),
            0, selection, "AUTOSAVE PROBE", pdfe::tre::RegionId{1}));

        // Oracle: reopen recovery copy → page 0 still carries a (changed,
        // non-empty) image XObject. Uses a fresh writer bound to a fresh ctx.
        auto oracle = [](const std::filesystem::path& rp,
                         const std::vector<uint8_t>& orig) -> bool {
            auto octx = std::make_shared<MuPdfContext>();
            auto odoc = Document::open(octx, rp);
            if (!odoc || odoc->pageCount() == 0) return false;
            auto oembed = std::make_unique<pdfe::fonts::PdfFontEmbedder>(octx.get());
            pdfe::tre::PdfContentWriter owriter(octx.get(), oembed.get());
            auto page = odoc->page(0);
            auto data = owriter.getImageXObjectData(page.get(), odoc.get());
            return !data.empty() && data != orig;
        };

        r.trigger_undo_dirty = !p.undo->isClean();
        r.trigger_autosave_fired = p.mgr->autosaveNow();
        const auto recPath = p.mgr->recoveryFilePath();
        std::error_code ec;
        const bool fileWritten = std::filesystem::exists(recPath, ec);
        r.capture_present = fileWritten && oracle(recPath, originalData);
        r.restore_present = fileWritten && oracle(recPath, originalData);
        r.note = "reconstruction executed";

        // Destroy the command (and its retained refs) NOW, while the TRE stack
        // it points at is still alive. p.undo lives in the Probe and is
        // destroyed after these locals, so leaving the command on the stack
        // would be a teardown UAF.
        p.undo->clear();
    } catch (const std::exception& e) {
        r.probed = false;
        r.note = std::string("TRE probe threw: ") + e.what();
    }
    finishRow(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// Register emitter: prints the table + writes the JSON gap register once, after
// every family TEST has run.
// ─────────────────────────────────────────────────────────────────────────────
class GapRegisterEmitter : public ::testing::Environment {
public:
    void TearDown() override {
        std::lock_guard<std::mutex> lk(registryMutex());
        const auto& rows = registry();

        // (a) Readable table to stdout.
        std::printf("\n================ AUTOSAVE CAPTURE GAP REGISTER "
                    "(phase-1 discovery) ================\n");
        std::printf("%-22s %-6s %-8s %-9s %-8s %-8s %-7s\n",
                    "family", "undoAP", "dirty", "autosave", "capture",
                    "restore", "probed");
        std::printf("--------------------------------------------------------"
                    "----------------------------\n");
        for (const auto& r : rows) {
            std::printf("%-22s %-6s %-8s %-9s %-8s %-8s %-7s%s\n",
                        r.family.c_str(),
                        r.applied_via_undo ? "undo" : "direct",
                        r.trigger_undo_dirty ? "yes" : "NO",
                        r.trigger_autosave_fired ? "yes" : "NO",
                        r.capture_present ? "yes" : "NO",
                        r.restore_present ? "yes" : "NO",
                        r.probed ? "yes" : "NO",
                        r.note.empty() ? "" : ("  # " + r.note).c_str());
        }
        std::printf("========================================================"
                    "============================\n");
        std::printf("GAP = probed family whose edit is NOT captured by autosave "
                    "(capture_present == NO).\n");
        for (const auto& r : rows) {
            if (r.probed && !r.capture_present) {
                std::printf("  GAP: %s\n", r.family.c_str());
            }
        }
        std::printf("\n");

        // (b) JSON gap register.
        // Guard: if NO audit rows were collected (e.g. this process ran a
        // filtered subset that excluded AutosaveCaptureAudit.*), do NOT write
        // the JSON — an empty register would clobber the real one produced by a
        // full audit run. The stdout table above already reflects the empty run.
        if (rows.empty()) {
            std::printf("[AutosaveCaptureAudit] no audit rows this run "
                        "(filtered subset) — leaving gap-register.json intact\n");
            return;
        }
        QJsonArray arr;
        int gapCount = 0;
        for (const auto& r : rows) {
            QJsonObject o;
            o["family"] = QString::fromStdString(r.family);
            o["seed"] = QString::fromStdString(r.seed);
            o["applied_via_undo"] = r.applied_via_undo;
            o["trigger_undo_dirty"] = r.trigger_undo_dirty;
            o["trigger_autosave_fired"] = r.trigger_autosave_fired;
            o["capture_present"] = r.capture_present;
            o["restore_present"] = r.restore_present;
            o["probed"] = r.probed;
            o["is_gap"] = (r.probed && !r.capture_present);
            if (r.probed && !r.capture_present) ++gapCount;
            if (!r.note.empty()) o["note"] = QString::fromStdString(r.note);
            arr.append(o);
        }
        QJsonObject root;
        root["schema"] = 1;
        root["phase"] = "phase-1";
        root["description"] =
            "Autosave capture coverage per edit family (discovery probe). "
            "is_gap == true means the family's edit is silently lost on crash "
            "because autosave never fired or the recovery copy did not carry it.";
        root["family_count"] = static_cast<int>(rows.size());
        root["gap_count"] = gapCount;
        root["families"] = arr;

        // GitHub #261's class. This was an absolute path under another
        // developer's home directory, so create_directories() made
        // C:/Users/dev/... on whatever machine ran the suite and the register
        // landed where nothing reads it -- while the printf below reported
        // success, truthfully, which is why it went unnoticed. Anchored to the
        // repo the test was built from instead.
        const std::filesystem::path outDir =
            std::filesystem::path(PDFE_REPO_ROOT) / ".claude" / "pipelines" /
            "autosave-coverage" / "gate-artifacts" / "phase-1";
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        const std::filesystem::path outPath = outDir / "gap-register.json";

        const QByteArray bytes =
            QJsonDocument(root).toJson(QJsonDocument::Indented);
        std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(bytes.constData(), bytes.size());
            ofs.close();
            std::printf("[AutosaveCaptureAudit] gap register written: %s\n",
                        outPath.string().c_str());
        } else {
            std::printf("[AutosaveCaptureAudit] WARNING: could not write %s\n",
                        outPath.string().c_str());
        }
    }
};

// Registered before RUN_ALL_TESTS via static init; TearDown() runs after all
// tests, so the register reflects every family that executed.
const bool kEmitterRegistered = [] {
    ::testing::AddGlobalTestEnvironment(new GapRegisterEmitter());
    return true;
}();

}  // namespace
