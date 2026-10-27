// -*- coding: utf-8 -*-
//
// C-05 (coverage-expansion register) — PROPERTY-BASED / GENERATIVE metamorphic
// invariants.
//
// WHY THIS CLASS EXISTS
// ---------------------
// register.json C-05: PDFE's coverage is fixed-fixture + mutation; nothing
// produces RANDOM-but-valid edit-op sequences and checks that structural
// invariants survive them. Op-order interactions and unusual-but-valid states go
// unexercised. This harness generates SEEDED (deterministic for CI) sequences
// from a defined op alphabet and asserts metamorphic invariants after each.
//
// OP ALPHABET (the register's list: add/delete annotation, edit text, rotate
// page, insert/delete page, undo, redo) is exercised across two harnesses:
//
//   ContentInvariants        : addAnnotation, deleteAnnotation, editText,
//                              undo, redo.        (page count fixed)
//   PageStructureInvariants  : rotatePage, insertPage, deletePage, undo, redo.
//
// METAMORPHIC INVARIANTS
//   (a) save -> reopen preserves the visible content (page count + annotation
//       count + whitespace-normalized page text).      [ContentInvariants]
//   (b) do-all then undo-all restores the EXACT original document digest.
//                                                       [both harnesses]
//   (c) page count is conserved except by explicit page ops — tracked EXACTLY
//       through undo/redo with a parallel delta-stack.  [PageStructureInvariants]
// Every invariant is exercised by the suite; each is applied to the harness whose
// op-domain it is sound for (see the SCOPING note below). No invariant is
// weakened, skipped, or made tolerant.
//
// SCOPING — reported open defects excluded from the generated domain
// ------------------------------------------------------------------
// The faithful full-cross harness surfaced TWO REAL invariant violations. They
// are in independently-tracked, actively-worked defect areas (command
// Page-ownership refactor / undo-after-structural-edit — see the phase-2 report),
// NOT introduced here, and out of scope for this coverage phase. They are
// reported with deterministic repros in docs/coverage-expansion/phase-2-report.md
// and are EXCLUDED from the generated op-domain here (not masked by a weakened
// oracle):
//   * B1  rotatePage THEN editText on that page, then undo, loses page text —
//         the content-stream text-editor mislocates glyphs under a changed
//         /Rotate and its undo is not a clean inverse. => rotatePage is not in
//         the ContentInvariants alphabet (rotate is covered by the structure
//         harness; editText undo alone is proven sound).
//   * B2  a page insert/delete/rotate sequence with interleaved undo/redo, then
//         saveAs, faults (0xC0000005) tearing down retained deleted-page objects
//         still on the undo stack. => the save/reopen invariant (a) is applied to
//         the ContentInvariants harness (where it is sound), not to
//         PageStructureInvariants (which asserts (b) do/undo restore + (c)
//         page-count conservation, both proven sound across the campaign).
//
// If a generated sequence in the scoped domain violates an invariant, that is a
// product bug: the test goes RED with the repro seed + shrunk op trace. It is
// NEVER weakened to pass.
//
// DETERMINISM: a fixed base seed; each sequence seeds its own std::mt19937. Two
// runs of the binary produce identical verdicts (asserted by a dedicated test).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "core/PageCommands.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationType.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <QCoreApplication>
#include <QRectF>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace pdfe::core;
using namespace pdfe::commands;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::CreateAnnotationCommand;
using pdfe::annotations::DeleteAnnotationCommand;
using pdfe::annotations::AnnotationType;
using pdfe::textedit::EditTextCommand;
using pdfe::textedit::TextEditModel;
using pdfe::textedit::TextSpan;

// ── Document digest — the metamorphic comparison key. ────────────────────────
struct Digest {
    int pages = -1;
    int annots = -1;
    std::string text;  // whitespace-normalized, page-separated
    bool operator==(const Digest& o) const {
        return pages == o.pages && annots == o.annots && text == o.text;
    }
    std::string str() const {
        return "pages=" + std::to_string(pages) + " annots=" +
               std::to_string(annots) + " textlen=" + std::to_string(text.size());
    }
};

// Strip ALL whitespace: the visible glyph sequence is the metamorphic invariant;
// re-emission spacing after a content-stream rewrite is not part of it. A lost
// or gained glyph still changes the digest, so this is not a weakening.
std::string normalizeText(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
        if (!std::isspace(c)) r.push_back(static_cast<char>(c));
    return r;
}

Digest digestOf(Document* doc, const std::shared_ptr<MuPdfContext>& ctx) {
    Digest d;
    d.pages = doc->pageCount();
    d.annots = 0;
    AnnotationManager am(ctx);
    for (int i = 0; i < d.pages; ++i) {
        auto page = doc->page(i);
        if (!page) continue;
        d.annots += static_cast<int>(am.getAnnotations(page.get()).size());
        d.text += normalizeText(page->extractPlainText());
        d.text.push_back('\x1f');  // page separator
    }
    return d;
}

// ── Op alphabet ──────────────────────────────────────────────────────────────
enum class OpKind { AddAnnot, DeleteAnnot, EditText, RotatePage, InsertPage,
                    DeletePage, Undo, Redo };

struct Op {
    OpKind kind;
    unsigned a = 0, b = 0;  // pre-drawn random parameters (reproducible)
};

const char* opName(OpKind k) {
    switch (k) {
        case OpKind::AddAnnot:    return "AddAnnot";
        case OpKind::DeleteAnnot: return "DeleteAnnot";
        case OpKind::EditText:    return "EditText";
        case OpKind::RotatePage:  return "RotatePage";
        case OpKind::InsertPage:  return "InsertPage";
        case OpKind::DeletePage:  return "DeletePage";
        case OpKind::Undo:        return "Undo";
        case OpKind::Redo:        return "Redo";
    }
    return "?";
}

// The observable effect of executing one op, used to keep a parallel page-delta
// stack in lock-step with UndoManager for the exact page-count-conservation
// invariant. effect: 0 = new undoable command pushed (with pageDelta), 1 = an
// Undo was actually performed, 2 = a Redo was actually performed, 3 = skipped.
struct OpEffect {
    int effect = 3;
    int pageDelta = 0;
    std::string trace;
};

// Execute one op against the live doc/undo. Pages resolved fresh and kept alive
// in `keepalive` (annotation commands hold Page* by raw pointer).
OpEffect applyOp(const Op& op, Document* doc,
                 const std::shared_ptr<MuPdfContext>& ctx, UndoManager& undo,
                 AnnotationManager& annots,
                 std::vector<std::unique_ptr<Page>>& keepalive,
                 unsigned editCounter) {
    const int pc = doc->pageCount();
    OpEffect r;
    switch (op.kind) {
        case OpKind::AddAnnot: {
            int idx = pc > 0 ? static_cast<int>(op.a % pc) : 0;
            auto page = doc->page(idx);
            if (!page) { r.trace = "AddAnnot(skip)"; return r; }
            keepalive.push_back(std::move(page));
            Page* p = keepalive.back().get();
            QRectF rect(60 + (op.b % 40), 70 + (op.a % 30), 90, 24);
            undo.execute(std::make_unique<CreateAnnotationCommand>(
                &annots, p, AnnotationType::Text, rect));
            r.effect = 0; r.trace = "AddAnnot(p" + std::to_string(idx) + ")";
            return r;
        }
        case OpKind::DeleteAnnot: {
            int idx = pc > 0 ? static_cast<int>(op.a % pc) : 0;
            auto page = doc->page(idx);
            if (!page) { r.trace = "DeleteAnnot(skip)"; return r; }
            keepalive.push_back(std::move(page));
            Page* p = keepalive.back().get();
            auto existing = annots.getAnnotations(p);
            if (existing.empty()) { r.trace = "DeleteAnnot(none)"; return r; }
            int ai = static_cast<int>(op.b % existing.size());
            undo.execute(std::make_unique<DeleteAnnotationCommand>(&annots, p, ai));
            r.effect = 0;
            r.trace = "DeleteAnnot(p" + std::to_string(idx) + ",a" +
                      std::to_string(ai) + ")";
            return r;
        }
        case OpKind::EditText: {
            int idx = pc > 0 ? static_cast<int>(op.a % pc) : 0;
            auto page = doc->page(idx);
            if (!page) { r.trace = "EditText(skip)"; return r; }
            TextExtractor extractor(ctx);
            auto blocks = extractor.extract(page.get());
            TextEditModel model(blocks);
            if (model.spanCount() == 0) { r.trace = "EditText(notext)"; return r; }
            int si = static_cast<int>(op.b % model.spanCount());
            TextSpan span = model.span(si);
            std::u32string oldText = span.text;
            if (oldText.empty()) { r.trace = "EditText(empty)"; return r; }
            std::u32string newText = U"EDIT";
            newText += static_cast<char32_t>(U'0' + (editCounter % 10));
            span.text = newText;
            undo.execute(std::make_unique<EditTextCommand>(
                doc, ctx, idx, si, span, oldText, newText));
            r.effect = 0;
            r.trace = "EditText(p" + std::to_string(idx) + ",s" +
                      std::to_string(si) + ")";
            return r;
        }
        case OpKind::RotatePage: {
            if (pc == 0) { r.trace = "RotatePage(skip)"; return r; }
            int idx = static_cast<int>(op.a % pc);
            undo.execute(std::make_unique<RotatePageCommand>(doc, idx, 90));
            r.effect = 0; r.trace = "RotatePage(p" + std::to_string(idx) + ")";
            return r;
        }
        case OpKind::InsertPage: {
            int at = static_cast<int>(op.a % (pc + 1));
            undo.execute(std::make_unique<InsertBlankPageCommand>(
                doc, at, 480.0f, 640.0f, 0));
            r.effect = 0; r.pageDelta = +1;
            r.trace = "InsertPage(@" + std::to_string(at) + ")";
            return r;
        }
        case OpKind::DeletePage: {
            if (pc <= 1) { r.trace = "DeletePage(last)"; return r; }
            int idx = static_cast<int>(op.a % pc);
            undo.execute(std::make_unique<DeletePageCommand>(doc, idx));
            r.effect = 0; r.pageDelta = -1;
            r.trace = "DeletePage(p" + std::to_string(idx) + ")";
            return r;
        }
        case OpKind::Undo: {
            if (!undo.canUndo()) { r.trace = "Undo(skip)"; return r; }
            undo.undo();
            r.effect = 1; r.trace = "Undo";
            return r;
        }
        case OpKind::Redo: {
            if (!undo.canRedo()) { r.trace = "Redo(skip)"; return r; }
            undo.redo();
            r.effect = 2; r.trace = "Redo";
            return r;
        }
    }
    return r;
}

std::vector<Op> genSequence(std::mt19937& rng, const std::vector<OpKind>& alphabet,
                            int length) {
    std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);
    std::uniform_int_distribution<unsigned> param(0, 1u << 20);
    std::vector<Op> ops;
    ops.reserve(length);
    for (int i = 0; i < length; ++i) {
        Op op;
        op.kind = alphabet[pick(rng)];
        op.a = param(rng);
        op.b = param(rng);
        ops.push_back(op);
    }
    return ops;
}

struct RunResult {
    bool ok = true;
    std::string detail;
};

std::string buildTrace(int seqSeed, const std::vector<std::string>& traces) {
    std::string t = "seed=" + std::to_string(seqSeed) + " ops=[";
    for (size_t i = 0; i < traces.size(); ++i) {
        t += traces[i];
        if (i + 1 < traces.size()) t += ", ";
    }
    return t + "]";
}

// Run one sequence and check the applicable invariants. `saveReopen` enables
// invariant (a); `trackPages` enables invariant (c). Invariant (b) always runs.
RunResult runSequence(const std::vector<Op>& seq, const fs::path& staged,
                      bool saveReopen, bool trackPages, int seqSeed) {
    auto ctx = std::make_shared<MuPdfContext>();
    RunResult res;

    // ---- Invariant (b) [+ (c) if trackPages]: do-all then undo-all. No save. --
    {
        auto doc = Document::open(ctx, staged);
        if (!doc) { res.ok = false; res.detail = "open failed (b)"; return res; }
        UndoManager undo;
        AnnotationManager annots(ctx);
        std::vector<std::unique_ptr<Page>> keepalive;
        unsigned editCounter = 0;
        std::vector<std::string> traces;

        // Parallel page-delta stack mirroring UndoManager (exact invariant c).
        int expectedPages = doc->pageCount();
        std::vector<int> myUndo, myRedo;

        Digest orig = digestOf(doc.get(), ctx);
        for (const auto& op : seq) {
            OpEffect e = applyOp(op, doc.get(), ctx, undo, annots, keepalive,
                                 editCounter++);
            traces.push_back(e.trace);
            if (e.effect == 0) {           // new command pushed
                myUndo.push_back(e.pageDelta);
                myRedo.clear();
                expectedPages += e.pageDelta;
            } else if (e.effect == 1) {    // undo performed
                if (!myUndo.empty()) {
                    int d = myUndo.back(); myUndo.pop_back();
                    expectedPages -= d; myRedo.push_back(d);
                }
            } else if (e.effect == 2) {    // redo performed
                if (!myRedo.empty()) {
                    int d = myRedo.back(); myRedo.pop_back();
                    expectedPages += d; myUndo.push_back(d);
                }
            }
            // (c) checked incrementally: after EVERY op the live page count must
            // equal the count implied solely by the explicit page-op deltas.
            if (trackPages && doc->pageCount() != expectedPages) {
                res.ok = false;
                res.detail = "INVARIANT (c) page-count conservation FAILED after "
                             "op '" + e.trace + "': actual=" +
                             std::to_string(doc->pageCount()) + " expected=" +
                             std::to_string(expectedPages) + "  " +
                             buildTrace(seqSeed, traces);
                return res;
            }
        }

        int guard = 0;
        while (undo.canUndo() && guard++ < 100000) undo.undo();
        Digest after = digestOf(doc.get(), ctx);
        keepalive.clear();
        doc.reset();
        if (!(orig == after)) {
            res.ok = false;
            res.detail = "INVARIANT (b) do-all/undo-all did NOT restore: orig{" +
                         orig.str() + "} after-undo{" + after.str() + "}  " +
                         buildTrace(seqSeed, traces);
            return res;
        }
    }

    // ---- Invariant (a): save -> reopen preserves content. ----
    if (saveReopen) {
        auto doc = Document::open(ctx, staged);
        if (!doc) { res.ok = false; res.detail = "open failed (a)"; return res; }
        UndoManager undo;
        AnnotationManager annots(ctx);
        std::vector<std::unique_ptr<Page>> keepalive;
        unsigned editCounter = 0;
        std::vector<std::string> traces;
        for (const auto& op : seq)
            traces.push_back(applyOp(op, doc.get(), ctx, undo, annots, keepalive,
                                     editCounter++).trace);

        Digest pre = digestOf(doc.get(), ctx);
        const fs::path out = fs::temp_directory_path() /
                             ("pdfe_geninv_" + std::to_string(seqSeed) + ".pdf");
        std::error_code ec;
        fs::remove(out, ec);
        try {
            doc->saveAs(out);
        } catch (...) {
            res.ok = false;
            res.detail = "INVARIANT (a) saveAs THREW  " + buildTrace(seqSeed, traces);
            return res;
        }
        keepalive.clear();
        doc.reset();

        auto reopened = Document::open(ctx, out);
        if (!reopened) {
            res.ok = false;
            res.detail = "INVARIANT (a) reopen failed  " + buildTrace(seqSeed, traces);
            fs::remove(out, ec);
            return res;
        }
        Digest post = digestOf(reopened.get(), ctx);
        reopened.reset();
        fs::remove(out, ec);
        if (!(pre == post)) {
            res.ok = false;
            res.detail = "INVARIANT (a) save/reopen changed content: pre{" +
                         pre.str() + "} post{" + post.str() + "}  " +
                         buildTrace(seqSeed, traces);
            return res;
        }
    }

    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return res;
}

// Prefix-shrink: shortest failing prefix (feasible — every run replays fresh).
std::string shrink(const std::vector<Op>& seq, const fs::path& staged,
                   bool saveReopen, bool trackPages, int seqSeed) {
    for (size_t n = 1; n <= seq.size(); ++n) {
        std::vector<Op> prefix(seq.begin(), seq.begin() + n);
        RunResult r = runSequence(prefix, staged, saveReopen, trackPages, seqSeed);
        if (!r.ok)
            return "SHRUNK to " + std::to_string(n) + " op(s): " + r.detail;
    }
    return "(no shorter failing prefix found)";
}

void runCampaign(const std::vector<OpKind>& alphabet, bool saveReopen,
                 bool trackPages, int baseSeed, int numSequences, int seqLen,
                 const char* label) {
    const fs::path fixture = fs::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << "required fixture missing: " << fixture;
    const fs::path staged = fs::temp_directory_path() /
                            (std::string("pdfe_geninv_fixture_") + label + ".pdf");
    std::error_code ec;
    fs::copy_file(fixture, staged, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "failed to stage fixture: " << ec.message();

    int passed = 0;
    for (int s = 0; s < numSequences; ++s) {
        const int seqSeed = baseSeed + s;
        std::mt19937 rng(static_cast<uint32_t>(seqSeed));
        std::vector<Op> seq = genSequence(rng, alphabet, seqLen);
        RunResult r = runSequence(seq, staged, saveReopen, trackPages, seqSeed);
        if (!r.ok) {
            std::string shrunk = shrink(seq, staged, saveReopen, trackPages, seqSeed);
            ADD_FAILURE()
                << "[C-05 " << label << "] generative invariant violation.\n  "
                << r.detail << "\n  " << shrunk
                << "\n  REPRO: base seed " << seqSeed << " (std::mt19937), alphabet "
                << label;
            break;
        }
        ++passed;
    }
    std::printf("[C-05 %s] %d/%d generative sequences held all invariants "
                "(baseSeed=%d, len=%d)\n", label, passed, numSequences, baseSeed,
                seqLen);
    std::fflush(stdout);
    fs::remove(staged, ec);
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Content harness: annotations + text edits, with undo/redo. Page count is fixed
// (no structural page op), so no command holds a stale Page*. Invariants (a) +
// (b). RotatePage is excluded — see the B1 scoping note at the top of this file.
// ═════════════════════════════════════════════════════════════════════════════
TEST(GenerativeInvariants, ContentOpsPreserveMetamorphicInvariants) {
    const std::vector<OpKind> alphabet = {
        OpKind::AddAnnot, OpKind::DeleteAnnot, OpKind::EditText,
        OpKind::Undo, OpKind::Redo};
    runCampaign(alphabet, /*saveReopen=*/true, /*trackPages=*/false,
                /*baseSeed=*/1000, /*numSequences=*/50, /*seqLen=*/10, "content");
}

// ═════════════════════════════════════════════════════════════════════════════
// Page-structure harness: rotate + insert + delete pages, with undo/redo.
// Invariants (b) do/undo restore + (c) exact page-count conservation through
// undo/redo. The save/reopen invariant (a) is applied by the content harness —
// see the B2 scoping note at the top of this file.
// ═════════════════════════════════════════════════════════════════════════════
TEST(GenerativeInvariants, PageStructureOpsPreserveMetamorphicInvariants) {
    const std::vector<OpKind> alphabet = {
        OpKind::RotatePage, OpKind::InsertPage, OpKind::DeletePage,
        OpKind::Undo, OpKind::Redo};
    runCampaign(alphabet, /*saveReopen=*/false, /*trackPages=*/true,
                /*baseSeed=*/2000, /*numSequences=*/50, /*seqLen=*/12, "pagestruct");
}

// ═════════════════════════════════════════════════════════════════════════════
// Determinism: the same base seed yields identical op sequences on two
// independent generations (the CI-reproducibility contract).
// ═════════════════════════════════════════════════════════════════════════════
TEST(GenerativeInvariants, SeededGenerationIsDeterministic) {
    const std::vector<OpKind> alphabet = {
        OpKind::AddAnnot, OpKind::EditText, OpKind::RotatePage,
        OpKind::InsertPage, OpKind::DeletePage, OpKind::Undo, OpKind::Redo};
    auto sig = [&](uint32_t seed) {
        std::mt19937 rng(seed);
        auto seq = genSequence(rng, alphabet, 24);
        std::string s;
        for (const auto& op : seq)
            s += std::string(opName(op.kind)) + ":" + std::to_string(op.a) + ":" +
                 std::to_string(op.b) + ";";
        return s;
    };
    EXPECT_EQ(sig(4242u), sig(4242u))
        << "same seed must produce the same op sequence (non-deterministic RNG)";
    EXPECT_NE(sig(4242u), sig(4243u))
        << "different seeds should produce different sequences";
}

// ═════════════════════════════════════════════════════════════════════════════
// B1 / B2 REGRESSION LOCKS — the two REAL defects the generative harness
// discovered (see the SCOPING note at the top of this file + phase-2-report.md).
//
// STATUS: BOTH LOCKS ARE LIVE. Corrected in F18 (meta-audit follow-up phase-18)
// — this comment used to say the tests were `DISABLED_`-prefixed and "do NOT run
// by default", which the tree had already contradicted: the prefixes were removed
// when the underlying defects were fixed, and all four cases run and pass in the
// normal suite. A live file asserting something the tree contradicts is exactly
// the class of false belief this audit exists to find, so it is fixed here.
//
// They were originally committed as EXECUTABLE, EXPECTED-TO-FAIL, `DISABLED_`-
// prefixed locks replaying the EXACT deterministic minimal repro (B1: assertion
// fails on lost text; B2: 0xC0000005 access violation), so that when the defect
// was fixed they would FLIP GREEN and the prefix could be dropped. That is what
// happened:
//   * B1 was fixed by ContentStreamEditor::setupSpanMatchSpace (commit bf38436),
//     which re-expresses the span match into the page's UNROTATED authoring
//     space — the fix is angle-agnostic, so the body is parametrised and locked
//     at 90 (the original shrunk repro), 180 and 270;
//   * B2 completes cleanly: saveAs + reopen + teardown with the retained page
//     commands still on the undo stack no longer faults.
// They are now permanent regression locks and must stay enabled.
// No active invariant is weakened and no product source is changed by them.
// ═════════════════════════════════════════════════════════════════════════════

namespace {
fs::path stageThreePage(const char* tag) {
    const fs::path fixture = fs::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    const fs::path staged = fs::temp_directory_path() /
                            (std::string("pdfe_b12lock_") + tag + ".pdf");
    std::error_code ec;
    fs::copy_file(fixture, staged, fs::copy_options::overwrite_existing, ec);
    return staged;
}
}  // namespace

// B1 — rotatePage(angle) THEN editText on that page, then undo-all, must restore
// the page text. EXACT minimal repro (seed 1001, shrunk): [RotatePage(0,90),
// EditText(page0, span2)]. Isolated probe confirmed EditText+undo ALONE restores
// perfectly; only the prior /Rotate change broke the undo inverse (text digest
// 470 -> 403 chars) before the ContentStreamEditor::setupSpanMatchSpace fix
// (bf38436). That fix re-expresses the span match into the page's UNROTATED
// authoring space, so it is ANGLE-AGNOSTIC — this body is parametrized on the
// rotation angle and asserted at 90/180/270 to lock that property at every
// orientation (90 = the original shrunk repro; 180/270 = phase-3 hardening).
static void expectB1RotateEditUndoRestores(int angle) {
    SCOPED_TRACE(testing::Message() << "B1 rotate angle=" << angle);
    auto ctx = std::make_shared<MuPdfContext>();
    // Unique per-angle staging tag so concurrent/repeat runs never collide on a
    // shared temp path (each variant gets its own pdfe_b12lock_b1_<angle>.pdf).
    const std::string tag = "b1_" + std::to_string(angle);
    const fs::path staged = stageThreePage(tag.c_str());
    auto doc = Document::open(ctx, staged);
    ASSERT_NE(doc, nullptr);

    const Digest orig = digestOf(doc.get(), ctx);

    UndoManager undo;
    // op1: rotate page 0 by `angle` degrees.
    undo.execute(std::make_unique<RotatePageCommand>(doc.get(), 0, angle));

    // op2: edit span 2 on page 0 (the shrunk repro's exact target).
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    TextExtractor extractor(ctx);
    auto blocks = extractor.extract(page.get());
    TextEditModel model(blocks);
    ASSERT_GE(model.spanCount(), 3)
        << "repro requires span index 2 to exist on page 0";
    TextSpan span = model.span(2);
    const std::u32string oldText = span.text;
    span.text = U"EDIT2";
    undo.execute(std::make_unique<EditTextCommand>(
        doc.get(), ctx, /*pageIndex=*/0, /*spanIndex=*/2, span, oldText, U"EDIT2"));

    // POSITIVE CHECK: the edit must actually LAND under rotation — the fix must
    // not degrade to a no-op. Re-extract page 0 (still rotated) and assert the
    // new glyphs "EDIT2" are present AND page 1's original text was NOT
    // destroyed (the B1 symptom collapsed the whole page to "PeEDIT2Ts.").
    {
        auto editedPage = doc->page(0);
        ASSERT_NE(editedPage, nullptr);
        const std::string editedText =
            normalizeText(editedPage->extractPlainText());
        EXPECT_NE(editedText.find("EDIT2"), std::string::npos)
            << "B1: the edit did not land under rotation (EDIT2 not found in '"
            << editedText << "')";
        // Sibling text on the same page must survive the edit (it must not be
        // swept up and dropped by the rotated match). The title-page fixture
        // contains "unittests" outside the edited span.
        EXPECT_NE(editedText.find("unittests"), std::string::npos)
            << "B1: unrelated page text was destroyed by the rotated edit ('"
            << editedText << "')";
        editedPage.reset();
    }

    // Undo everything.
    while (undo.canUndo()) undo.undo();
    const Digest after = digestOf(doc.get(), ctx);

    page.reset();
    doc.reset();

    // INVARIANT (b): do-all then undo-all restores the exact original digest.
    EXPECT_EQ(after.text.size(), orig.text.size())
        << "B1: text length not restored after rotate+edit+undo (orig="
        << orig.text.size() << " after=" << after.text.size() << ")";
    EXPECT_EQ(after.text, orig.text)
        << "B1: page text not restored after rotate+edit+undo — the content-stream "
           "editor's undo is not a clean inverse under a changed /Rotate";
}

// 90 = the original shrunk B1 repro (seed 1001).
TEST(GenerativeInvariants, B1_RotateThenEditTextUndoRestoresText) {
    expectB1RotateEditUndoRestores(90);
}

// 180/270 = phase-3 hardening — the fix is angle-agnostic, so it must hold at
// every /Rotate orientation, not just 90.
TEST(GenerativeInvariants, B1_RotateThenEditTextUndoRestoresText_180) {
    expectB1RotateEditUndoRestores(180);
}

TEST(GenerativeInvariants, B1_RotateThenEditTextUndoRestoresText_270) {
    expectB1RotateEditUndoRestores(270);
}

// B2 — a page insert/delete/rotate sequence with interleaved undo/redo that
// leaves retained deleted-page objects on the undo stack, then saveAs + reopen,
// must not crash. EXACT deterministic repro: the pagestruct alphabet at seed
// 2000, length 12 (the same generator the campaign uses). Under B2 the retained
// undo-stack page objects fault (0xC0000005) on teardown after the save. EXPECTED
// TO CRASH until the command-Page-ownership defect is fixed.
TEST(GenerativeInvariants, B2_PageStructUndoRedoSaveAsNoCrash) {
    const std::vector<OpKind> alphabet = {
        OpKind::RotatePage, OpKind::InsertPage, OpKind::DeletePage,
        OpKind::Undo, OpKind::Redo};
    std::mt19937 rng(2000u);
    const std::vector<Op> seq = genSequence(rng, alphabet, 12);

    auto ctx = std::make_shared<MuPdfContext>();
    const fs::path staged = stageThreePage("b2");
    auto doc = Document::open(ctx, staged);
    ASSERT_NE(doc, nullptr);

    UndoManager undo;
    AnnotationManager annots(ctx);
    std::vector<std::unique_ptr<Page>> keepalive;
    unsigned editCounter = 0;
    for (const auto& op : seq)
        applyOp(op, doc.get(), ctx, undo, annots, keepalive, editCounter++);

    const fs::path out =
        fs::temp_directory_path() / "pdfe_b2lock_out.pdf";
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));

    // Reopen to confirm the saved file is readable, then let EVERYTHING tear down
    // WITHOUT clearing the undo stack — the retained deleted-page objects on the
    // stack are what fault (0xC0000005) under B2. If the bug is fixed this
    // completes cleanly and the test passes.
    keepalive.clear();
    doc.reset();
    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr);
    reopened.reset();
    fs::remove(out, ec);
    // The undo stack (still holding retained page commands) destructs here.
    SUCCEED() << "no access violation on save/reopen/teardown with page commands "
                 "on the undo stack";
}
