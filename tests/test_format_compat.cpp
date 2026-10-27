// -*- coding: utf-8 -*-
//
// C-04 (coverage-expansion register) — FORMAT-COMPAT / MIGRATION.
//
// WHY THIS CLASS EXISTS
// ---------------------
// register.json C-04: no test opens files written in OLDER / exotic-but-valid
// on-disk PDF shapes and asserts they load + edit + save-roundtrip. A
// save-format change (or a reader regression) could silently break opening
// documents produced by a prior PDFE release or another producer — a real
// regression for existing users. This suite commits one 2-page document rendered
// into four structurally distinct on-disk shapes and proves each still:
//   (1) opens with the correct page count and known text,
//   (2) accepts an edit,
//   (3) survives save -> reopen with the edit PERSISTED and the original content
//       INTACT.
//
// THE CORPUS (tests/fixtures/gen_format_compat_corpus.py) — all carry the shared
// tokens COMPATMARK / ONE / TWO so the roundtrip oracle is common:
//   compat_objstm_xref.pdf  object streams (/ObjStm) + xref streams (PDF 1.5+).
//   compat_incremental.pdf  chained incremental updates (multiple /Prev).
//   compat_linearized.pdf   linearized ("fast web view").
//   compat_rebuilt_xref.pdf classic xref DAMAGED (zeroed) -> reader must rebuild.
//
// STRUCTURAL DISTINCTNESS is asserted separately (FormatCompatShape.*) by reading
// the committed bytes, so the roundtrip oracle cannot pass trivially on four
// identical files — each fixture genuinely represents its claimed layout.
//
// This is a correctness suite: SKIP=FAIL. The fixtures are pinned in the fixture
// floor (tests/CMakeLists.txt), so a missing file hard-fails configure.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationType.h"

#include <QRectF>

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::CreateAnnotationCommand;
using pdfe::annotations::AnnotationType;

constexpr const char* kEditToken = "COMPAT_EDIT_TOKEN_C04";

fs::path fixture(const std::string& name) {
    return fs::path(TEST_FIXTURES_DIR) / name;
}

std::string readAllBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

std::string pageText(Document* doc, int idx) {
    auto page = doc->page(idx);
    if (!page) return {};
    return page->extractPlainText();
}

// The shared open -> verify -> edit -> save -> reopen -> verify oracle.
void runOpenEditRoundtrip(const std::string& fixtureName) {
    const fs::path in = fixture(fixtureName);
    ASSERT_TRUE(fs::exists(in)) << "fixture missing: " << in;

    auto ctx = std::make_shared<MuPdfContext>();

    // (1) OPEN with correct page count + known text.
    auto doc = Document::open(ctx, in);
    ASSERT_NE(doc, nullptr) << "failed to open " << fixtureName;
    ASSERT_EQ(doc->pageCount(), 2) << fixtureName << " lost/gained pages on open";

    const std::string p0 = pageText(doc.get(), 0);
    const std::string p1 = pageText(doc.get(), 1);
    EXPECT_NE(p0.find("COMPATMARK"), std::string::npos)
        << fixtureName << " page 0 missing COMPATMARK (text: " << p0.substr(0, 80)
        << ")";
    EXPECT_NE(p0.find("ONE"), std::string::npos) << fixtureName << " page 0 missing ONE";
    EXPECT_NE(p1.find("TWO"), std::string::npos) << fixtureName << " page 1 missing TWO";

    // (2) EDIT: add a Text annotation carrying a unique token on page 0.
    UndoManager undo;
    AnnotationManager annots(ctx);
    auto page0 = doc->page(0);
    ASSERT_NE(page0, nullptr);
    undo.execute(std::make_unique<CreateAnnotationCommand>(
        &annots, page0.get(), AnnotationType::Text, QRectF(200, 200, 120, 26)));
    const auto created = annots.getAnnotations(page0.get());
    ASSERT_FALSE(created.empty()) << "annotation creation produced nothing";
    annots.setContents(page0.get(), created.back().annotIndex, kEditToken);

    // (3) SAVE -> REOPEN.
    const fs::path out = fs::temp_directory_path() /
                         ("pdfe_compat_" + fixtureName);
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out));
    page0.reset();
    doc.reset();

    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr) << "failed to reopen saved " << fixtureName;
    EXPECT_EQ(reopened->pageCount(), 2)
        << "reopened " << fixtureName << " lost/gained pages";

    // Original content INTACT after the roundtrip.
    const std::string rp0 = pageText(reopened.get(), 0);
    const std::string rp1 = pageText(reopened.get(), 1);
    EXPECT_NE(rp0.find("COMPATMARK"), std::string::npos)
        << "roundtrip dropped original page-0 text in " << fixtureName;
    EXPECT_NE(rp1.find("TWO"), std::string::npos)
        << "roundtrip dropped original page-1 text in " << fixtureName;

    // Edit PERSISTED: the token annotation survives the save-format rewrite.
    AnnotationManager reAnnots(ctx);
    auto rePage0 = reopened->page(0);
    ASSERT_NE(rePage0, nullptr);
    const auto reCreated = reAnnots.getAnnotations(rePage0.get());
    bool found = false;
    for (const auto& a : reCreated)
        if (a.contents == kEditToken) { found = true; break; }
    EXPECT_TRUE(found)
        << "the edit (annotation \"" << kEditToken << "\") did NOT survive "
           "save->reopen for " << fixtureName << " — a save-format change broke "
           "the roundtrip for this on-disk shape";

    rePage0.reset();
    reopened.reset();
    fs::remove(out, ec);
}

}  // namespace

// ── Roundtrip oracle, one per on-disk shape ──────────────────────────────────

TEST(FormatCompat, ObjStmXrefLoadsEditsRoundtrips) {
    runOpenEditRoundtrip("compat_objstm_xref.pdf");
}
TEST(FormatCompat, IncrementalChainLoadsEditsRoundtrips) {
    runOpenEditRoundtrip("compat_incremental.pdf");
}
TEST(FormatCompat, LinearizedLoadsEditsRoundtrips) {
    runOpenEditRoundtrip("compat_linearized.pdf");
}
TEST(FormatCompat, RebuiltXrefLoadsEditsRoundtrips) {
    runOpenEditRoundtrip("compat_rebuilt_xref.pdf");
}

// ── PDF-86 / GitHub issue #33 — the rebuilt xref must match its INTACT TWIN ──
//
// COVERAGE GAP, not a product gap: the reconstruction already recovers the
// document correctly. What was missing is an oracle able to say so.
//
// RebuiltXrefLoadsEditsRoundtrips above proves the damaged file opens, edits
// and round-trips. That is a weaker claim than it reads as. A reconstruction
// that recovered the WRONG objects — a stale revision, a subset of the pages,
// the right text on the wrong page — round-trips its own mistake perfectly
// well, because every one of that test's oracles is evaluated against the
// repaired document itself. Nothing in it is anchored outside the repair.
//
// EQUIVALENCE is the property worth asserting, and it needs a reference the
// thing under test did not produce. `compat_intact_xref.pdf` is that
// reference: gen_format_compat_corpus.py writes it from the SAME
// `doc.save(...)` call that produces compat_rebuilt_xref.pdf, one statement
// before the byte transform that damages the cross-reference. Same generator,
// same content, differing only by the damage.
//
// It is deliberately NOT built by re-saving the damaged file. That would
// compare MuPDF's repair against itself and pass however wrong the repair is.
//
// THREE NON-VACUITY GUARDS, because this comparison has three ways to be
// green while measuring nothing:
//   (a) THE FILES MUST DIFFER ON DISK. If the twin were a copy of the damaged
//       file, every assertion below is a tautology. Asserted on the bytes.
//   (b) THE DAMAGE MUST BE REAL. The damaged file must carry the zeroed xref
//       offsets, i.e. it genuinely forces the reconstruction path; the twin
//       must not. Without this the test compares two healthy files.
//   (c) THE EXTRACTED TEXT MUST BE NON-EMPTY. Two documents that both extract
//       "" compare equal — the zero-vs-zero vacuity shape. Asserted before the
//       equality, not after.
TEST(FormatCompat, RebuiltXrefMatchesIntactTwin) {
    const fs::path damagedPath = fixture("compat_rebuilt_xref.pdf");
    const fs::path intactPath = fixture("compat_intact_xref.pdf");
    ASSERT_TRUE(fs::exists(damagedPath)) << "fixture missing: " << damagedPath;
    ASSERT_TRUE(fs::exists(intactPath)) << "fixture missing: " << intactPath;

    const std::string damagedBytes = readAllBytes(damagedPath);
    const std::string intactBytes = readAllBytes(intactPath);
    ASSERT_FALSE(damagedBytes.empty());
    ASSERT_FALSE(intactBytes.empty());

    // (a) The twin is not the same file.
    ASSERT_NE(damagedBytes, intactBytes)
        << "compat_intact_xref.pdf is byte-identical to compat_rebuilt_xref.pdf "
           "— this test would be comparing the reconstruction with itself";

    // (b) The damage is present in one and absent from the other. A classic
    //     xref has exactly ONE "0000000000 " (the head free entry), so >1 means
    //     in-use offsets were zeroed and the reader must reconstruct.
    EXPECT_GT(countOccurrences(damagedBytes, "0000000000 "), 1)
        << "the damaged fixture no longer carries zeroed xref offsets, so it "
           "does not force the reconstruction path and this comparison proves "
           "nothing about repair";
    EXPECT_EQ(countOccurrences(intactBytes, "0000000000 "), 1)
        << "the intact twin carries zeroed xref offsets — it is damaged too";

    auto ctx = std::make_shared<MuPdfContext>();

    auto intact = Document::open(ctx, intactPath);
    ASSERT_NE(intact, nullptr) << "the intact twin does not open";
    auto damaged = Document::open(ctx, damagedPath);
    ASSERT_NE(damaged, nullptr) << "the damaged document does not open";

    // Page count first: a reconstruction that dropped a page is the most
    // likely way to be wrong, and it must not be able to hide behind a
    // per-page loop that only walks the pages that survived.
    ASSERT_EQ(intact->pageCount(), 2) << "the reference itself is wrong";
    EXPECT_EQ(damaged->pageCount(), intact->pageCount())
        << "xref reconstruction changed the page count";

    // (c) Non-vacuity: the reference must actually have text before an
    //     equality on text means anything.
    for (int i = 0; i < intact->pageCount(); ++i) {
        const std::string ref = pageText(intact.get(), i);
        ASSERT_FALSE(ref.empty())
            << "reference page " << i << " extracted no text — an equality "
               "against it would be the zero-vs-zero vacuity shape";
        EXPECT_NE(ref.find("COMPATMARK"), std::string::npos)
            << "reference page " << i << " lost the corpus marker";

        const std::string got = pageText(damaged.get(), i);
        EXPECT_EQ(got, ref)
            << "page " << i << " differs after xref reconstruction.\n"
            << "  reconstructed: " << got << "\n"
            << "  intact twin:   " << ref;
    }

    // Page GEOMETRY as well as text. Text equality alone cannot see a repair
    // that recovered the right content streams under the wrong /MediaBox, and
    // the page boxes live in the page dictionaries the rebuild has to relocate.
    for (int i = 0; i < intact->pageCount(); ++i) {
        auto refPage = intact->page(i);
        auto gotPage = damaged->page(i);
        ASSERT_NE(refPage, nullptr);
        ASSERT_NE(gotPage, nullptr);
        const QRectF refBounds = refPage->boundingBox();
        ASSERT_GT(refBounds.width(), 0.0);
        ASSERT_GT(refBounds.height(), 0.0);
        EXPECT_DOUBLE_EQ(gotPage->boundingBox().width(), refBounds.width())
            << "page " << i << " width differs after reconstruction";
        EXPECT_DOUBLE_EQ(gotPage->boundingBox().height(), refBounds.height())
            << "page " << i << " height differs after reconstruction";
    }
}

// ── Structural distinctness: prove each fixture really is its claimed shape, so
//    the roundtrip oracle above cannot pass trivially on four identical files. ──

TEST(FormatCompatShape, ObjStmXrefUsesObjectStreams) {
    const std::string bytes = readAllBytes(fixture("compat_objstm_xref.pdf"));
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.find("/ObjStm"), std::string::npos)
        << "compat_objstm_xref.pdf carries no object stream";
}

TEST(FormatCompatShape, IncrementalHasChainedXrefSections) {
    const std::string bytes = readAllBytes(fixture("compat_incremental.pdf"));
    ASSERT_FALSE(bytes.empty());
    // A chained incremental file has multiple xref sections (>1 startxref) and
    // at least one /Prev pointer linking them.
    EXPECT_GE(countOccurrences(bytes, "startxref"), 2)
        << "compat_incremental.pdf is not a chained incremental file";
    EXPECT_NE(bytes.find("/Prev"), std::string::npos)
        << "compat_incremental.pdf has no /Prev xref chain";
}

TEST(FormatCompatShape, LinearizedHasLinearizedDict) {
    const std::string bytes = readAllBytes(fixture("compat_linearized.pdf"));
    ASSERT_FALSE(bytes.empty());
    // The /Linearized parameter dictionary must be in the file's first bytes.
    const std::string head = bytes.substr(0, 2048);
    EXPECT_NE(head.find("/Linearized"), std::string::npos)
        << "compat_linearized.pdf is not linearized (no leading /Linearized dict)";
}

TEST(FormatCompatShape, RebuiltXrefHasDamagedCrossReference) {
    const std::string bytes = readAllBytes(fixture("compat_rebuilt_xref.pdf"));
    ASSERT_FALSE(bytes.empty());
    // The generator zeroes every in-use xref offset ("0000000000 NNNNN n"), so a
    // damaged fixture carries at least a couple of them — a normal file has none
    // (its offsets point at real bytes). This proves the fixture forces the
    // reader down the xref-reconstruction path.
    EXPECT_GE(countOccurrences(bytes, "0000000000 "), 2)
        << "compat_rebuilt_xref.pdf xref is not damaged — it would not exercise "
           "the reconstruction path";
}

// ── PDF-02 (meta-audit gap register, issue #1): the SAVED OUTPUT keeps the
//    compact on-disk shape, it is not only the fixture that has it. ──────────
//
// ObjStmXrefUsesObjectStreams above asserts the FIXTURE carries /ObjStm. That
// says nothing about what PDFE writes. A save that silently expands every
// object stream back into free-standing objects is a real regression for a
// user: the file grows (the whole point of /ObjStm is that object headers and
// the xref entries for them disappear into one compressed stream), and a
// PDF 1.5+ document quietly loses the structure its producer chose.
//
// This asserts the property on the BYTES PDFE PRODUCED, from a document that
// came in with object streams. Both halves matter: `/ObjStm` proves the
// objects were bundled, and `/Type /XRef`-bearing output proves the xref is a
// stream — an object stream is unreachable from a classic xref table, so a
// file cannot honestly claim one without the other.
TEST(FormatCompatShape, SavedOutputKeepsObjStm) {
    const fs::path in = fixture("compat_objstm_xref.pdf");
    ASSERT_TRUE(fs::exists(in)) << "fixture missing: " << in;

    const std::string sourceBytes = readAllBytes(in);
    ASSERT_NE(sourceBytes.find("/ObjStm"), std::string::npos)
        << "precondition: the source fixture must itself use object streams, "
           "otherwise this test asserts nothing about retention";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, in);
    ASSERT_NE(doc, nullptr);

    const fs::path out =
        fs::temp_directory_path() / "pdfe_compat_objstm_retention.pdf";
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out));
    doc.reset();

    const std::string savedBytes = readAllBytes(out);
    ASSERT_FALSE(savedBytes.empty());
    EXPECT_NE(savedBytes.find("/ObjStm"), std::string::npos)
        << "saving a document that arrived with object streams expanded them "
           "back into free-standing objects — the saved file no longer uses "
           "the compact PDF 1.5+ shape its producer chose";
    EXPECT_NE(savedBytes.find("/XRef"), std::string::npos)
        << "the saved file has no cross-reference stream, so any object stream "
           "in it would be unreachable";

    fs::remove(out, ec);
}

// ── Issue #124 — the OTHER direction of the same contract. ───────────────────
//
// SavedOutputKeepsObjStm above proves object streams are PRESERVED. On its own
// that is half a guard: `effectiveObjectStreamMode()` could be mutated to
// `return 1;` and it would stay green while every classic-xref PDF 1.4
// document silently became a 1.5+ file that older readers cannot open. That is
// the same species of unrequested format change applyVersionFloorForSave()
// (F14 / PDF-01) exists to prevent in the other direction.
//
// So: PRESERVE, never IMPOSE. A source that never used object streams must not
// acquire them on save. The precondition is ASSERTED on the fixture rather than
// assumed — a control test that runs against a source which already had the
// property either way proves nothing.
TEST(FormatCompatShape, ClassicXrefSourceDoesNotGainObjStm) {
    const fs::path in = fixture("test_3page.pdf");
    ASSERT_TRUE(fs::exists(in)) << "fixture missing: " << in;

    const std::string sourceBytes = readAllBytes(in);
    ASSERT_FALSE(sourceBytes.empty());
    ASSERT_EQ(sourceBytes.find("/ObjStm"), std::string::npos)
        << "precondition: test_3page.pdf must NOT already use object streams, "
           "otherwise this test cannot distinguish 'preserved' from 'imposed'";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, in);
    ASSERT_NE(doc, nullptr);

    // The seam itself, not just its downstream effect: a classic-xref source
    // must report false, which is what makes the write-side assertion below
    // attributable to this decision rather than to some other part of the save.
    EXPECT_FALSE(doc->sourceUsesObjectStreams())
        << "a classic-xref source was detected as using object streams";

    const fs::path out =
        fs::temp_directory_path() / "pdfe_compat_classic_xref_out.pdf";
    std::error_code ec;
    fs::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(fs::exists(out));
    doc.reset();

    const std::string savedBytes = readAllBytes(out);
    ASSERT_FALSE(savedBytes.empty());
    EXPECT_EQ(savedBytes.find("/ObjStm"), std::string::npos)
        << "saving a classic-xref document IMPOSED object streams on it — the "
           "file was silently upgraded to a PDF 1.5+ shape its producer never "
           "chose, and readers that predate object streams can no longer open "
           "it";

    // Still a readable document afterwards: "no object streams" is not worth
    // much if the output stopped opening.
    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr) << "the classic-xref output does not reopen";
    EXPECT_EQ(reopened->pageCount(), 3);
    reopened.reset();

    fs::remove(out, ec);
}
