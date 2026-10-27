// Pipeline p86-followups phase-4 — turn the remaining six UI-matrix `planned`
// cells (3x TRE, 3x Fonts) into tests that genuinely assert an outcome.
//
// Matrix rows covered by this file:
//   tre_detect_mode     Phase4TreModeDetectionTest.ContentKindFromSidecarPredictsTextAndImageDominance
//                       Phase4TreModeDetectionTest.PageFontClassSeparatesScanFromSubsetAndCompleteVector
//   tre_edit_overlay    Phase4TreEditOverlayTest.OcrTextLayerYieldsEditableSpansTheBareScanDoesNot
//   tre_save_roundtrip  Phase4TreSaveRoundtripTest.EditedScannedPageTextSurvivesSaveAndReopen
//   font_embedded_info  Phase4FontEmbeddedInfoTest.SubsetEmbeddedFontReportsEmbeddedWhileStandard14DoesNot
//   font_cid_encoding   Phase4FontCidEncodingTest.CjkCidTextDecodesToTheAuthoredScripts
//   font_substitute     Phase4FontSubstituteTest.Standard14StyleSuffixesRouteToDistinctRealFontFiles
//
// ---------------------------------------------------------------------------
// THREE MATRIX ROWS HAD THE WRONG FIXTURE. The CSV is corrected alongside this
// file; the reasoning is recorded here because the code is where it is checkable.
//
//   font_embedded_info / font_cid_encoding  — the CSV named `test_single.pdf`.
//     That fixture is 989 bytes. Its ONLY font is a non-embedded Type1
//     `/BaseFont /Helvetica`: there is no `/FontFile`, `/FontFile2` or
//     `/FontFile3` anywhere in the file, and no `/Type0` or `/CIDFont*` either.
//     So it cannot return "embedded font names + types", and it has no CID
//     characters to decode. Any test written against it would have had to
//     assert something weaker than the row claims — a vacuous pass.
//     Corrected to `embedded_descriptor_style.pdf` (real /FontFile2, Type0 +
//     CIDFontType2, /ToUnicode, subset BaseFont "ADRXQZ+Sutpenia Regular") and
//     `phase3_cjk_text.pdf` (three Type0/CIDFontType0 fonts under the
//     Adobe-Japan1 / GB1 / Korea1 orderings) respectively.
//
//     `test_single.pdf` is NOT simply dropped — it is retained in both tests as
//     the CONTROL arm, precisely because it is the negative case. That turns
//     the mis-specification into evidence: `isEmbedded` is shown to be measured
//     rather than hard-coded true, because the same call reports false for it.
//
//   tre_edit_overlay / tre_save_roundtrip — the CSV named `test_scanned.pdf`
//     with preconditions "click on OCR text + edit". `test_scanned.pdf` has NO
//     text layer at all (no `/BaseFont`, no text-show operators; sidecar
//     `contentKind: "scanned-image-only"`, `scripts: []`). There is no OCR text
//     on it to click. Producing any would mean running Tesseract live, which
//     makes the oracle depend on OCR accuracy rather than on the overlay/save
//     path the rows are about. Corrected to `test_scanned_with_ocr.pdf` — the
//     same scan WITH the OCR text layer already present (`contentKind:
//     "mixed"`, `scripts: ["ascii"]`), which is literally what "click on OCR
//     text" describes. `test_scanned.pdf` is retained as the control arm in
//     the overlay test (it must yield zero editable spans).
//
// ---------------------------------------------------------------------------
// TWO THINGS THAT LOOK LIKE THE OBVIOUS ORACLE AND ARE NOT. Recorded so the
// next reader does not re-derive them.
//
//   1. `pdfe::tre::AtlasMode` is NOT mode detection. It is a two-value glyph-
//      SOURCING toggle (ScanCropped / FreeTypeStyled) that the user flips from
//      a menu, a toolbar combo or AtlasComparisonDialog, hard-defaulted to
//      ScanCropped at MainWindow.cpp and never persisted. Nothing in the tree
//      DERIVES an AtlasMode from a document. Asserting that it equals
//      ScanCropped after opening a scan re-reads a hard-coded initialiser and
//      passes for any product change whatsoever — the "one-against-one" shape.
//      The real OCR-vs-vector discriminators are Page::hasTextContent(),
//      Page::isImageDominated() and VectorSubsetDetector::classify(), which are
//      computed from page content. Those are what this file asserts on.
//
//   2. `pdfe::tre::ImageReplaceCommand` does NOT persist recoverable text.
//      ImageReplacer renders the edited text to a BITMAP and composites it into
//      a replaced image XObject (its only PdfContentWriter calls are
//      getOriginalImage/replacePageImage — never writeTextBlock). A
//      save-and-reopen oracle asserting `extractPlainText()` contains the typed
//      string would therefore fail for a correct product, or — worse — be
//      "fixed" into something vacuous. The save-roundtrip row is driven through
//      EditTextCommand, which re-encodes real text-show operators.
//
// ---------------------------------------------------------------------------
// ORACLE DISCIPLINE (tests/docs — "vacuous_oracle_three_shapes"):
//   * every assertion compares against an INPUT (a string this test typed, a
//     font name authored in the PDF bytes, a style suffix this test passed) or
//     against an INDEPENDENTLY-AUTHORED value (the `.expected.json` sidecars,
//     written by tests/fixtures/generate_*.py from a different producer than
//     the C++ under test) — never against a second read of the same code path;
//   * every test first guards that the measurement HAPPENED: a collection is
//     asserted non-empty, or a visited-case counter is asserted equal to the
//     number of cases, BEFORE any property is asserted over it. An empty
//     collection must never be able to satisfy a `for`-loop of EXPECTs;
//   * where a predicate could be faked by returning a constant, a CONTROL arm
//     asserts the opposite value from the same call on a different fixture.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextBlock.h"
#include "core/TextChar.h"
#include "core/TextLine.h"
#include "core/ToUnicodeRecovery.h"

#include "compliance/ComplianceTypes.h"
#include "compliance/FontComplianceChecker.h"

#include "fonts/FontResolver.h"
#include "fonts/VectorSubsetDetector.h"

#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::compliance::FontComplianceChecker;
using pdfe::compliance::FontComplianceInfo;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::fonts::FontResolver;
using pdfe::fonts::PageFontClass;
using pdfe::fonts::VectorSubsetDetector;
using pdfe::textedit::TextEditModel;

namespace {

namespace fs = std::filesystem;

fs::path fixturesDir() { return fs::path(TEST_FIXTURES_DIR); }

fs::path fixture(const char* name) { return fixturesDir() / name; }

// ---------------------------------------------------------------------------
// Sidecar access — the INDEPENDENT oracle.
//
// `<fixture>.expected.json` is authored by the Python fixture generators, not
// by any C++ code under test here. Reading `contentKind`/`scripts` from it and
// asserting the C++ predicates agree is therefore a genuine cross-check, not a
// second read of the same path. A missing/!unreadable sidecar is a hard failure:
// silently substituting a default would turn the oracle off.
// ---------------------------------------------------------------------------
QJsonObject readSidecar(const char* pdfName) {
    std::string base(pdfName);
    const auto dot = base.rfind(".pdf");
    if (dot != std::string::npos) base = base.substr(0, dot);
    const fs::path p = fixturesDir() / (base + ".expected.json");
    QFile f(QString::fromStdString(p.string()));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// Every codepoint on page 0, in extraction order.
std::vector<char32_t> pageCodepoints(Page* page) {
    std::vector<char32_t> out;
    for (const auto& block : page->extractText())
        for (const auto& line : block.lines)
            for (const auto& ch : line.chars)
                out.push_back(ch.codepoint);
    return out;
}

constexpr bool inRange(char32_t c, char32_t lo, char32_t hi) {
    return c >= lo && c <= hi;
}

}  // namespace

// ===========================================================================
// ROW: tre_detect_mode — "open TRE tool -> OCR vs vector mode detected"
// ===========================================================================
//
// The product decides between the OCR/atlas pipeline and the vector pipeline
// from page CONTENT, via Page::hasTextContent() + Page::isImageDominated()
// (and, downstream, VectorSubsetDetector::classify). Those are the predicates
// asserted here.
//
// Non-vacuity: the expected values are not written down twice. They are DERIVED
// from each fixture's independently-authored `contentKind`, and the three
// fixtures deliberately straddle all three values, so a predicate that returned
// any constant fails on at least one arm. `visited` guards that the loop body
// actually ran for every case — an empty case list would otherwise pass.

TEST(Phase4TreModeDetectionTest,
     ContentKindFromSidecarPredictsTextAndImageDominance) {
    struct Case { const char* pdf; };
    const std::vector<Case> cases{
        {"test_scanned.pdf"},           // authored contentKind: scanned-image-only
        {"test_scanned_with_ocr.pdf"},  // authored contentKind: mixed
        {"test_single.pdf"},            // authored contentKind: vector-text
        {"phase3_cjk_text.pdf"},        // authored contentKind: vector-text
    };

    auto ctx = std::make_shared<MuPdfContext>();

    int visited = 0;
    int sawTextContent = 0;
    int sawImageDominated = 0;

    for (const auto& c : cases) {
        SCOPED_TRACE(c.pdf);
        const fs::path path = fixture(c.pdf);
        ASSERT_TRUE(fs::exists(path)) << "fixture missing: " << path.string();

        const QJsonObject sidecar = readSidecar(c.pdf);
        ASSERT_FALSE(sidecar.isEmpty())
            << "sidecar missing/unreadable; the independent oracle is required";
        ASSERT_TRUE(sidecar.contains("contentKind"))
            << "sidecar carries no contentKind — nothing to check against";
        const std::string kind = sidecar.value("contentKind").toString().toStdString();
        ASSERT_FALSE(kind.empty());

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        const bool hasText = page->hasTextContent();
        const bool imageDominated = page->isImageDominated();

        if (hasText) ++sawTextContent;
        if (imageDominated) ++sawImageDominated;

        if (kind == "scanned-image-only") {
            // Nothing on the page but pixels: the OCR pipeline is the only way
            // to edit its text, so both predicates must say so.
            EXPECT_FALSE(hasText)
                << "authored contentKind is scanned-image-only, so there must be "
                   "no extractable text layer";
            EXPECT_TRUE(imageDominated)
                << "a pure scan must route to the OCR/atlas pipeline";
        } else if (kind == "vector-text") {
            EXPECT_TRUE(hasText)
                << "authored contentKind is vector-text, so text must extract";
            EXPECT_FALSE(imageDominated)
                << "a vector-text page must route to the vector pipeline";
        } else if (kind == "mixed") {
            // A scan that already carries an OCR text layer: text extracts AND
            // the bitmap still dominates the page.
            EXPECT_TRUE(hasText)
                << "authored contentKind is mixed, so the OCR layer must extract";
            EXPECT_TRUE(imageDominated)
                << "the page is still a full-page bitmap under the text layer";
        } else {
            FAIL() << "unhandled authored contentKind: " << kind;
        }
        ++visited;
    }

    // Measurement-happened guards. Without these the whole test passes if the
    // case list is empty or every Document::open fails early.
    ASSERT_EQ(visited, static_cast<int>(cases.size()))
        << "not every fixture was actually examined";
    // And the two predicates must each have been observed BOTH ways across the
    // corpus, so a constant-returning implementation cannot survive.
    EXPECT_GT(sawTextContent, 0) << "hasTextContent() was never true";
    EXPECT_LT(sawTextContent, visited) << "hasTextContent() was never false";
    EXPECT_GT(sawImageDominated, 0) << "isImageDominated() was never true";
    EXPECT_LT(sawImageDominated, visited) << "isImageDominated() was never false";
}

// The routing enum the text-edit pipeline actually branches on. Three fixtures,
// three distinct classes — asserted against facts authored in the PDF bytes
// (a subset prefix is present or it is not), not against another classify() call.
TEST(Phase4TreModeDetectionTest,
     PageFontClassSeparatesScanFromSubsetAndCompleteVector) {
    auto ctx = std::make_shared<MuPdfContext>();

    struct Case { const char* pdf; PageFontClass expected; const char* why; };
    const std::vector<Case> cases{
        {"test_scanned.pdf", PageFontClass::ImageDominated,
         "no text layer, full-page bitmap"},
        {"embedded_descriptor_style.pdf", PageFontClass::VectorSubsetted,
         "BaseFont 'ADRXQZ+Sutpenia Regular' carries a 6-letter subset prefix"},
        {"phase3_cjk_text.pdf", PageFontClass::VectorComplete,
         "vector text, and none of its BaseFonts carry a subset prefix"},
    };

    int visited = 0;
    std::set<int> distinctClasses;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.pdf);
        const fs::path path = fixture(c.pdf);
        ASSERT_TRUE(fs::exists(path)) << "fixture missing: " << path.string();

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        const PageFontClass got =
            VectorSubsetDetector::classify(page.get(), doc.get(), ctx.get());
        EXPECT_EQ(got, c.expected) << c.why;
        distinctClasses.insert(static_cast<int>(got));
        ++visited;
    }

    ASSERT_EQ(visited, static_cast<int>(cases.size()));
    // A classify() that returned one constant would still satisfy one arm.
    EXPECT_EQ(distinctClasses.size(), 3u)
        << "classify() did not produce three distinct classes across a corpus "
           "chosen to straddle all three";

    // The subset-prefix predicate itself, asserted against literal inputs — the
    // positive is the BaseFont authored in embedded_descriptor_style.pdf, the
    // negatives are names authored in phase3_cjk_text.pdf.
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix("ADRXQZ+Sutpenia Regular"));
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("Gothic"));
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("Heiti"));
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("Helvetica"));
}

// ===========================================================================
// ROW: tre_edit_overlay — "click on OCR text + edit -> overlay appears with
//                          editable text"
// ===========================================================================
//
// The overlay's editable content is TextEditModel, built from Page::extractText()
// (TextEditTool::rebuildModel does exactly this). The row is therefore asserted
// at that seam: spans exist, a click point inside a span's own quad selects that
// span, and typing over it changes the span to the string this test supplied.
//
// Non-vacuity: `test_scanned.pdf` is the CONTROL — the same code on a scan with
// no text layer must yield ZERO spans. Without that arm, "spanCount > 0" could
// be satisfied by a model that fabricates a span for every page.

TEST(Phase4TreEditOverlayTest, OcrTextLayerYieldsEditableSpansTheBareScanDoesNot) {
    auto ctx = std::make_shared<MuPdfContext>();

    // --- CONTROL: a scan with no OCR layer has nothing to overlay -----------
    {
        const fs::path bare = fixture("test_scanned.pdf");
        ASSERT_TRUE(fs::exists(bare)) << bare.string();
        auto doc = Document::open(ctx, bare);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        TextEditModel bareModel(page->extractText());
        EXPECT_EQ(bareModel.spanCount(), 0)
            << "test_scanned.pdf carries no text layer, so the edit overlay "
               "must offer nothing to edit; a non-zero count here means the "
               "model fabricates spans and the positive arm below proves nothing";
    }

    // --- The row proper: the same scan WITH its OCR text layer --------------
    const fs::path withOcr = fixture("test_scanned_with_ocr.pdf");
    ASSERT_TRUE(fs::exists(withOcr)) << withOcr.string();
    auto doc = Document::open(ctx, withOcr);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    TextEditModel model(page->extractText());

    // Measurement-happened guard: everything below iterates or indexes spans.
    ASSERT_GT(model.spanCount(), 0)
        << "the OCR text layer must produce at least one editable span";

    // Find a span with real text and a real box. Guarded, not assumed.
    int probe = -1;
    for (int i = 0; i < model.spanCount(); ++i) {
        const auto& s = model.span(i);
        if (!s.text.empty() && s.boundingBox.width() > 0.0 &&
            s.boundingBox.height() > 0.0) {
            probe = i;
            break;
        }
    }
    ASSERT_GE(probe, 0)
        << "no span carried both text and a non-degenerate bounding box";

    const std::u32string originalText = model.span(probe).text;
    ASSERT_FALSE(originalText.empty());

    // "Click on OCR text": a point inside THIS span's own quad must hit-test to
    // THIS span. The input is the point; the output is the index.
    const QPointF clickPoint = model.span(probe).boundingBox.center();
    EXPECT_EQ(model.spanAtPoint(clickPoint), probe)
        << "a click at the centre of span " << probe
        << "'s bounding box must select span " << probe;

    // A point far outside the page must select nothing — otherwise spanAtPoint
    // could be returning a constant index and the assertion above is vacuous.
    EXPECT_EQ(model.spanAtPoint(QPointF(-10000.0, -10000.0)), -1)
        << "a click nowhere near the page must not select a span";

    // "...+ edit": the overlay content becomes what was typed. Asserted against
    // the INPUT literal, and against the false->true transition of the dirty
    // flag (a model hard-coded dirty would fail the pre-condition).
    EXPECT_FALSE(model.hasModifications())
        << "a freshly built overlay must not already report modifications";

    const std::u32string typed = U"OVERLAY_EDIT_OK";
    ASSERT_NE(typed, originalText) << "the sentinel must differ from the OCR text";
    model.replaceSpanText(probe, typed);

    EXPECT_EQ(model.span(probe).text, typed)
        << "the overlay must now hold exactly the typed string";
    EXPECT_EQ(model.span(probe).originalText, originalText)
        << "the pre-edit text must be preserved for undo";
    EXPECT_TRUE(model.span(probe).isModified());
    EXPECT_TRUE(model.hasModifications());

    const std::vector<int> modified = model.modifiedSpanIndices();
    ASSERT_EQ(modified.size(), 1u)
        << "exactly the one edited span must be reported modified";
    EXPECT_EQ(modified.front(), probe);
}

// ===========================================================================
// ROW: tre_save_roundtrip — "save + reopen -> edits persisted in PDF"
// ===========================================================================
//
// Driven through EditTextCommand (the path that re-encodes real text-show
// operators) rather than ImageReplaceCommand (which composites pixels and would
// make this oracle structurally unable to pass — see the header note).
//
// The oracle is the INPUT sentinel recovered from a SEPARATELY OPENED Document
// reading the bytes off disk. It also satisfies the suite's
// lint_save_without_verify rule, which requires a reopen+verify after any save.

TEST(Phase4TreSaveRoundtripTest, EditedScannedPageTextSurvivesSaveAndReopen) {
    const fs::path src = fixture("test_scanned_with_ocr.pdf");
    ASSERT_TRUE(fs::exists(src)) << src.string();

    const fs::path out =
        fs::temp_directory_path() / "pdfe_phase4_tre_roundtrip.pdf";
    std::error_code ec;
    fs::remove(out, ec);

    const std::string sentinel = "TREROUNDTRIPOK";
    const std::u32string typed = U"TREROUNDTRIPOK";

    auto ctx = std::make_shared<MuPdfContext>();

    {
        auto doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        // The sentinel must not already be in the document, or "it is present
        // after the roundtrip" proves nothing about the edit.
        const std::string before = page->extractPlainText();
        ASSERT_EQ(before.find(sentinel), std::string::npos)
            << "sentinel already present before editing; choose another";

        TextEditModel model(page->extractText());
        ASSERT_GT(model.spanCount(), 0)
            << "no editable span on the OCR layer — nothing to round-trip";

        int probe = -1;
        for (int i = 0; i < model.spanCount(); ++i) {
            if (!model.span(i).text.empty()) { probe = i; break; }
        }
        ASSERT_GE(probe, 0) << "no span carried text";

        const auto span = model.span(probe);
        const std::u32string oldText = span.text;
        ASSERT_FALSE(oldText.empty());

        pdfe::textedit::EditTextCommand cmd(doc.get(), ctx, 0, probe, span,
                                            oldText, typed);
        ASSERT_NO_THROW(cmd.execute());

        // In-memory precondition: if the edit did not land before the save, a
        // later absence would be blamed on the save rather than on the edit.
        {
            auto p = doc->page(0);
            ASSERT_NE(p, nullptr);
            const std::string afterEdit = p->extractPlainText();
            ASSERT_NE(afterEdit.find(sentinel), std::string::npos)
                << "the edit did not land in the in-memory document";
        }

        ASSERT_NO_THROW(doc->saveAs(out));
    }

    // --- Reopen from disk. A brand-new context and Document: nothing of the
    //     editing session survives into this arm except the file itself. ------
    ASSERT_TRUE(fs::exists(out)) << "saveAs produced no file at " << out.string();
    EXPECT_GT(fs::file_size(out, ec), 0u);

    {
        auto verifyCtx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(verifyCtx, out);
        ASSERT_NE(reopened, nullptr) << "the saved PDF did not reopen";
        ASSERT_GT(reopened->pageCount(), 0);
        auto page = reopened->page(0);
        ASSERT_NE(page, nullptr);

        const std::string text = page->extractPlainText();
        // Guard that extraction produced anything at all: an empty string would
        // make a `find(...) == npos` style assertion pass for the wrong reason,
        // and makes the positive assertion below meaningful.
        ASSERT_FALSE(text.empty())
            << "the reopened page extracted no text at all";
        EXPECT_NE(text.find(sentinel), std::string::npos)
            << "the edit did not persist through save+reopen; extracted: "
            << text.substr(0, 400);
    }

    fs::remove(out, ec);
}

// ===========================================================================
// ROW: font_embedded_info — "query font info -> embedded font names + types
//                            returned"
// ===========================================================================
//
// FIXTURE CORRECTED (see header): test_single.pdf -> embedded_descriptor_style.pdf,
// with test_single.pdf retained as the control arm.
//
// Non-vacuity: `isEmbedded` is asserted TRUE on a fixture whose bytes contain
// /FontFile2 and FALSE on a fixture that contains no /FontFile at all, using the
// same call. A hard-coded flag fails one arm. The NAME is asserted against the
// BaseFont literal authored in the PDF, not against another API read.

TEST(Phase4FontEmbeddedInfoTest,
     SubsetEmbeddedFontReportsEmbeddedWhileStandard14DoesNot) {
    auto ctx = std::make_shared<MuPdfContext>();
    FontComplianceChecker checker(ctx);

    // --- Arm 1: the genuinely embedded, subsetted CIDFontType2 --------------
    const fs::path embeddedPdf = fixture("embedded_descriptor_style.pdf");
    ASSERT_TRUE(fs::exists(embeddedPdf)) << embeddedPdf.string();

    auto embeddedDoc = Document::open(ctx, embeddedPdf);
    ASSERT_NE(embeddedDoc, nullptr);

    const std::vector<FontComplianceInfo> embeddedFonts =
        checker.checkPageFonts(embeddedDoc.get(), 0);

    // Measurement-happened guard.
    ASSERT_FALSE(embeddedFonts.empty())
        << "no font info returned for a page that carries an embedded font";

    int embeddedCount = 0;
    bool sawAuthoredName = false;
    bool sawToUnicode = false;
    for (const auto& f : embeddedFonts) {
        EXPECT_FALSE(f.fontName.empty()) << "a font was reported with no name";
        if (f.isEmbedded) ++embeddedCount;
        // "Sutpenia" is the BaseFont authored in the PDF bytes
        // (/BaseFont /ADRXQZ+Sutpenia#20Regular) — an INPUT, not an API echo.
        if (lower(f.fontName).find("sutpenia") != std::string::npos) {
            sawAuthoredName = true;
            EXPECT_TRUE(f.isEmbedded)
                << "the font carrying /FontFile2 must report isEmbedded";
            if (f.hasToUnicode) sawToUnicode = true;
        }
    }
    EXPECT_TRUE(sawAuthoredName)
        << "the authored BaseFont 'Sutpenia' was not among the reported names";
    EXPECT_GT(embeddedCount, 0)
        << "the page embeds a font, so at least one entry must say so";
    // The fixture authors /ToUnicode; the type half of "names + types".
    EXPECT_TRUE(sawToUnicode)
        << "embedded_descriptor_style.pdf authors a /ToUnicode CMap for its "
           "embedded font, so hasToUnicode must be reported";

    // The "type" half, from the routing layer: this font is a SUBSET, and the
    // detector must name it.
    {
        auto page = embeddedDoc->page(0);
        ASSERT_NE(page, nullptr);
        const std::vector<std::string> subsetted =
            VectorSubsetDetector::subsettedFontNames(page.get(), embeddedDoc.get(),
                                                      ctx.get());
        ASSERT_FALSE(subsetted.empty())
            << "the page's BaseFont carries a subset prefix, so it must be listed";
        bool named = false;
        for (const auto& n : subsetted) {
            EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix(n))
                << "reported as subsetted but has no subset prefix: " << n;
            if (lower(n).find("sutpenia") != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "the authored subset font was not named";
    }

    // --- Arm 2 (CONTROL): the fixture the matrix row used to name -----------
    // test_single.pdf has no /FontFile at all. The SAME call must report its
    // Helvetica as NOT embedded. This is what proves arm 1 measured something.
    const fs::path standardPdf = fixture("test_single.pdf");
    ASSERT_TRUE(fs::exists(standardPdf)) << standardPdf.string();

    auto standardDoc = Document::open(ctx, standardPdf);
    ASSERT_NE(standardDoc, nullptr);

    const std::vector<FontComplianceInfo> standardFonts =
        checker.checkPageFonts(standardDoc.get(), 0);
    ASSERT_FALSE(standardFonts.empty())
        << "no font info returned for a page that uses Helvetica";

    bool sawHelvetica = false;
    for (const auto& f : standardFonts) {
        if (lower(f.fontName).find("helvetica") != std::string::npos) {
            sawHelvetica = true;
            EXPECT_FALSE(f.isEmbedded)
                << "test_single.pdf contains no /FontFile, so its Helvetica "
                   "cannot be embedded — this is why the matrix row's original "
                   "fixture could not satisfy 'embedded font names + types'";
        }
    }
    EXPECT_TRUE(sawHelvetica)
        << "the authored BaseFont 'Helvetica' was not among the reported names";

    // And the same routing call must NOT call it subsetted.
    {
        auto page = standardDoc->page(0);
        ASSERT_NE(page, nullptr);
        const std::vector<std::string> subsetted =
            VectorSubsetDetector::subsettedFontNames(page.get(), standardDoc.get(),
                                                      ctx.get());
        EXPECT_TRUE(subsetted.empty())
            << "test_single.pdf's Helvetica has no subset prefix";
    }
}

// ===========================================================================
// ROW: font_cid_encoding — "render + extract text -> CID characters decoded
//                           correctly"
// ===========================================================================
//
// FIXTURE CORRECTED (see header): test_single.pdf -> phase3_cjk_text.pdf.
//
// phase3_cjk_text.pdf authors three Type0/CIDFontType0 fonts under the
// Adobe-Japan1 / Adobe-GB1 / Adobe-Korea1 orderings with the UniJIS-UTF16-H,
// UniGB-UTF16-H and UniKS-UTF16-H CMaps, and NO /ToUnicode — so recovering
// Unicode is genuinely a CID-decoding job, not a ToUnicode table lookup.
//
// Non-vacuity: the expected scripts come from the sidecar's authored
// `category_tags` (japanese / chinese / korean) and `scripts` (cjk), written by
// the fixture generator. The test also screens every decoded codepoint through
// isUnusableExtractedCodepoint(), which is what distinguishes "decoded" from
// "came back as U+FFFD / PUA and still looked like a successful extraction".

TEST(Phase4FontCidEncodingTest, CjkCidTextDecodesToTheAuthoredScripts) {
    const char* kPdf = "phase3_cjk_text.pdf";
    const fs::path path = fixture(kPdf);
    ASSERT_TRUE(fs::exists(path)) << path.string();

    // Independent oracle: what the generator says is in this file.
    const QJsonObject sidecar = readSidecar(kPdf);
    ASSERT_FALSE(sidecar.isEmpty()) << "sidecar missing/unreadable";
    std::set<std::string> tags;
    for (const auto& v : sidecar.value("category_tags").toArray())
        tags.insert(lower(v.toString().toStdString()));
    for (const auto& v : sidecar.value("scripts").toArray())
        tags.insert(lower(v.toString().toStdString()));
    ASSERT_FALSE(tags.empty()) << "sidecar declares no scripts/tags";
    ASSERT_TRUE(tags.count("cjk") == 1)
        << "this test is only meaningful on a fixture authored as CJK";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const std::vector<char32_t> cps = pageCodepoints(page.get());

    // Measurement-happened guard: every assertion below is a scan over `cps`.
    ASSERT_FALSE(cps.empty())
        << "no characters extracted at all — nothing was decoded";

    int hiragana = 0, hangul = 0, ideographs = 0, ascii = 0, unusable = 0;
    std::vector<char32_t> unusableSamples;
    for (char32_t c : cps) {
        if (inRange(c, 0x3040, 0x309F)) ++hiragana;
        else if (inRange(c, 0xAC00, 0xD7A3)) ++hangul;
        else if (inRange(c, 0x4E00, 0x9FFF)) ++ideographs;
        if (c < 0x80) ++ascii;
        if (c != U' ' && c != U'\n' && c != U'\r' && c != U'\t' &&
            pdfe::core::isUnusableExtractedCodepoint(static_cast<int>(c))) {
            ++unusable;
            if (unusableSamples.size() < 12) unusableSamples.push_back(c);
        }
    }

    // A failed CID decode typically yields U+FFFD, 0, or a PUA codepoint while
    // still returning a plausible-looking character count. Screening for that is
    // the difference between "text came back" and "text decoded correctly".
    {
        std::string sample;
        for (char32_t c : unusableSamples) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "U+%04X ", static_cast<unsigned>(c));
            sample += buf;
        }
        EXPECT_EQ(unusable, 0)
            << "CID decoding produced " << unusable
            << " unusable codepoint(s) (replacement/PUA/control): " << sample;
    }

    // The authored scripts must actually be present, per the sidecar's tags.
    if (tags.count("japanese")) {
        EXPECT_GT(hiragana + ideographs, 0)
            << "sidecar declares Japanese content, but no Hiragana or Kanji "
               "codepoint was decoded";
    }
    if (tags.count("korean")) {
        EXPECT_GT(hangul, 0)
            << "sidecar declares Korean content, but no Hangul syllable was "
               "decoded";
    }
    if (tags.count("chinese")) {
        EXPECT_GT(ideographs, 0)
            << "sidecar declares Simplified Chinese content, but no CJK "
               "ideograph was decoded";
    }
    // The sidecar also declares ascii; the Helvetica/WinAnsi run must survive
    // alongside the CID runs (a decoder that mangled the simple font would
    // otherwise go unnoticed).
    if (tags.count("ascii")) {
        EXPECT_GT(ascii, 0)
            << "sidecar declares ASCII content, but no ASCII codepoint decoded";
    }

    // Non-vacuity backstop: the page must not be ASCII-only, or every CJK
    // assertion above could be satisfied by a fixture with no CID text at all.
    EXPECT_GT(hiragana + hangul + ideographs, 0)
        << "not one CJK codepoint decoded — the CID path returned nothing";
}

// ===========================================================================
// ROW: font_substitute — "open + render page -> fallback font substituted"
// ===========================================================================
//
// EXPECTED CORRECTED (see the CSV notes): the row read "fallback font
// substituted without crash". "without crash" is not an outcome — a test
// written to it is satisfied by a function that returns nothing at all, which
// is the permanently-false-guard shape. The row now asks for a NAMED substitute
// file, which is what the resolver actually promises.
//
// Non-vacuity, three ways:
//   * every resolved path is asserted to be a real file on disk (the filesystem
//     is the independent oracle, not another resolver call);
//   * `resolvedCount` guards that the loop body ran for every input;
//   * an unknown family MUST resolve to empty. Without that control, a resolver
//     that returned one hard-coded path for every input would pass everything.
// The style arm asserts against the INPUT suffixes: "-Bold" must not land on
// the same file as the plain family.

TEST(Phase4FontSubstituteTest, Standard14StyleSuffixesRouteToDistinctRealFontFiles) {
    // Use the OS/bundled resolution chain as shipped.
    const std::vector<std::string> standard14{
        "Helvetica",  "Helvetica-Bold", "Helvetica-Oblique",
        "Times-Roman", "Times-Bold",    "Times-Italic",
        "Courier",     "Courier-Bold",
    };

    int resolvedCount = 0;
    std::set<std::string> distinctFiles;
    for (const auto& name : standard14) {
        SCOPED_TRACE(name);
        const std::string path = FontResolver::resolveStandard14(name);
        ASSERT_FALSE(path.empty())
            << "no substitute font resolved for standard-14 name '" << name
            << "' — the fallback chain produced nothing";
        std::error_code ec;
        EXPECT_TRUE(fs::is_regular_file(fs::path(path), ec))
            << "resolved to a path that is not a real file: " << path;
        distinctFiles.insert(lower(path));
        ++resolvedCount;
    }

    // Measurement-happened guard.
    ASSERT_EQ(resolvedCount, static_cast<int>(standard14.size()))
        << "not every standard-14 name was resolved";

    // The three families must not all collapse onto one file, or "a substitute
    // was found" would be true while the substitution carried no information.
    EXPECT_GE(distinctFiles.size(), 3u)
        << "the eight standard-14 names resolved to fewer than three distinct "
           "font files; the substitution chain is not distinguishing families";

    // Style routing, asserted against the INPUT suffix. resolveStandard14 parses
    // "-Bold"/"-Oblique"/"-Italic" and must pass the flags down; if it dropped
    // them, every variant would land on the regular face.
    const std::string regular = lower(FontResolver::resolveStandard14("Helvetica"));
    const std::string bold = lower(FontResolver::resolveStandard14("Helvetica-Bold"));
    const std::string oblique =
        lower(FontResolver::resolveStandard14("Helvetica-Oblique"));
    ASSERT_FALSE(regular.empty());
    ASSERT_FALSE(bold.empty());
    ASSERT_FALSE(oblique.empty());
    EXPECT_NE(bold, regular)
        << "'Helvetica-Bold' resolved to the same file as 'Helvetica'; the "
           "bold suffix was not honoured";
    EXPECT_NE(oblique, regular)
        << "'Helvetica-Oblique' resolved to the same file as 'Helvetica'; the "
           "oblique suffix was not honoured";

    const std::string timesRegular =
        lower(FontResolver::resolveStandard14("Times-Roman"));
    const std::string timesBold =
        lower(FontResolver::resolveStandard14("Times-Bold"));
    ASSERT_FALSE(timesRegular.empty());
    ASSERT_FALSE(timesBold.empty());
    EXPECT_NE(timesBold, timesRegular)
        << "'Times-Bold' resolved to the same file as 'Times-Roman'";

    // A substitution actually happened: "Helvetica" is not a font file present
    // on a stock Windows install, so a real path here can only have come from
    // walking the metric-compatible chain (Arial / Liberation Sans / ...).
    EXPECT_EQ(regular.find("helvetica"), std::string::npos)
        << "expected the chain to substitute a metric-compatible face for "
           "Helvetica, but it resolved to: " << regular;

    // CONTROL: the resolver must be able to say "no". Without this, all of the
    // above is satisfiable by returning one constant path for any input.
    EXPECT_TRUE(FontResolver::resolveFontPath("NoSuchFontFamily12345Zz").empty())
        << "an unknown family resolved to a font file; the resolver is not "
           "discriminating and every assertion above is vacuous";
    EXPECT_TRUE(FontResolver::resolveFontPath("").empty())
        << "an empty family name resolved to a font file";
}
