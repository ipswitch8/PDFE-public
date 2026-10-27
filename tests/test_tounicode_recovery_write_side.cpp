// -*- coding: utf-8 -*-
//
// GitHub #188 — the WRITE side of the /ToUnicode recovery added for #23.
//
// THE DECISION THESE TESTS ENFORCE
// --------------------------------
// #188 asked for a deliberate choice between keeping the in-memory mutation
// (`pdf_dict_put_drop(ctx, fontDict, PDF_NAME(ToUnicode), streamRef)` in
// src/core/ToUnicodeRecovery.cpp) and making the recovery extraction-only.
// The choice, with its reasoning and its re-evaluation triggers, is recorded in
// docs/tounicode-recovery-write-side-decision.md: **keep it, and test it.**
//
// The short version of why: an extraction-only side table would have to be
// consulted at all eleven fz_new_stext_page_from_page sites, two of which are
// the redaction scrubber and the redaction VERIFIER — where a forgotten consult
// silently recreates the A3-48 false negative #23 exists to remove. And
// fz_stext_char does not carry the source byte code, so such a table could only
// be keyed on a lossy reverse map.
//
// Keeping it is only defensible if the bounds are MEASURED rather than
// reasoned about, which is what this file is for. Four bounds, four cases:
//
//   1. It never contradicts the file  — an authored /ToUnicode survives
//      byte-identical.
//   2. It never invents               — a font whose glyph names carry no
//      Unicode information gains nothing.
//   3. It never causes a save         — opening does not set the modified flag.
//   4. The enrichment is real         — the SAVED BYTES extract correctly in a
//      reader that has never heard of PDFE.
//
// WHY THE ORACLE IN CASE 4 USES RAW MuPDF
// ---------------------------------------
// Re-opening the saved file through core::Document would run the recovery AGAIN
// on the reopened document, so "the text extracts" would be true whether or not
// anything was written to disk. Every inspection here therefore goes through
// raw MuPDF (fz_open_document / pdf_lookup_page_obj / fz_new_stext_page_from_
// page), which is the same view any other PDF reader has of the file.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::core;

namespace {

fs::path fixture(const char* name) { return fs::path(TEST_FIXTURES_DIR) / name; }

fs::path repoRoot()
{
    return fs::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

/// One page-level /Font entry, as a reader other than PDFE sees it.
struct FontEntry {
    std::string key;          ///< the name in the page's /Font dict ("T3", "F1")
    bool hasToUnicode = false;
    std::string cmap;         ///< decoded /ToUnicode stream, empty when absent
};

/// Read page 0's /Font dictionary out of @p file with raw MuPDF.
///
/// The containers are declared by the CALLER and filled through references, so
/// nothing with a non-trivial destructor is created on the stack inside the
/// guarded region. `buf` and `doc` are raw pointers dropped in fz_always for
/// the same reason (MuPdfContext::execute's own fz_catch translates a throw
/// into a C++ exception AFTER the longjmp has already unwound past us).
void inspectPageFonts(const MuPdfContext& ctx, const fs::path& file,
                      std::vector<FontEntry>& out)
{
    const std::string path = file.string();
    ctx.execute([&](fz_context* c) {
        fz_document* doc = nullptr;
        fz_buffer* buf = nullptr;
        fz_try(c) {
            doc = fz_open_document(c, path.c_str());
            pdf_document* pdoc = pdf_document_from_fz_document(c, doc);
            if (pdoc) {
                pdf_obj* pageObj = pdf_lookup_page_obj(c, pdoc, 0);
                pdf_obj* res = pdf_resolve_indirect(
                    c, pdf_dict_get(c, pageObj, PDF_NAME(Resources)));
                pdf_obj* fonts = pdf_resolve_indirect(
                    c, pdf_dict_get(c, res, PDF_NAME(Font)));
                const int n = pdf_is_dict(c, fonts) ? pdf_dict_len(c, fonts) : 0;
                for (int i = 0; i < n; ++i) {
                    const char* key = pdf_to_name(c, pdf_dict_get_key(c, fonts, i));
                    pdf_obj* font =
                        pdf_resolve_indirect(c, pdf_dict_get_val(c, fonts, i));
                    pdf_obj* tu = pdf_dict_get(c, font, PDF_NAME(ToUnicode));

                    // No MuPDF call happens between here and the push_back, so
                    // no longjmp can strand the temporaries.
                    out.push_back(FontEntry{key ? key : "", tu != nullptr, ""});

                    if (!tu) continue;
                    buf = pdf_load_stream(c, tu);
                    unsigned char* data = nullptr;
                    const size_t len = fz_buffer_storage(c, buf, &data);
                    out.back().cmap.assign(reinterpret_cast<const char*>(data), len);
                    fz_drop_buffer(c, buf);
                    buf = nullptr;
                }
            }
        }
        fz_always(c) {
            fz_drop_buffer(c, buf);
            fz_drop_document(c, doc);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
    });
}

const FontEntry* findFont(const std::vector<FontEntry>& fonts, const char* key)
{
    for (const auto& f : fonts)
        if (f.key == key) return &f;
    return nullptr;
}

/// Extract page 0's text with raw MuPDF and MuPDF's own default stext options —
/// i.e. exactly what a reader that is not PDFE gets from the bytes on disk.
std::string rawExtractPageText(const MuPdfContext& ctx, const fs::path& file)
{
    const std::string path = file.string();
    std::string result;
    ctx.execute([&](fz_context* c) {
        fz_document* doc = nullptr;
        fz_page* page = nullptr;
        fz_stext_page* stext = nullptr;
        fz_buffer* buf = nullptr;
        fz_try(c) {
            doc = fz_open_document(c, path.c_str());
            page = fz_load_page(c, doc, 0);
            fz_stext_options opts = {0};
            stext = fz_new_stext_page_from_page(c, page, &opts);
            buf = fz_new_buffer_from_stext_page(c, stext);
            unsigned char* data = nullptr;
            const size_t len = fz_buffer_storage(c, buf, &data);
            result.assign(reinterpret_cast<const char*>(data), len);
        }
        fz_always(c) {
            fz_drop_buffer(c, buf);
            fz_drop_stext_page(c, stext);
            fz_drop_page(c, page);
            fz_drop_document(c, doc);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
    });
    return result;
}

/// Write a copy of @p in to @p out in which page 0's /<fontKey> font carries an
/// AUTHORED /ToUnicode whose content is @p cmapText.
///
/// Built at run time with raw MuPDF rather than added to the corpus: the file
/// exists only to supply bound 1's control, and a corpus fixture carries
/// obligations (sidecars, FIXTURE-COUNTS, drift guards) that a two-line
/// derivation does not need.
void authorToUnicodeOnFont(const MuPdfContext& ctx, const fs::path& in,
                           const fs::path& out, const char* fontKey,
                           const std::string& cmapText)
{
    const std::string inPath = in.string();
    const std::string outPath = out.string();
    ctx.execute([&](fz_context* c) {
        pdf_document* doc = nullptr;
        fz_buffer* buf = nullptr;
        fz_try(c) {
            doc = pdf_open_document(c, inPath.c_str());
            pdf_obj* pageObj = pdf_lookup_page_obj(c, doc, 0);
            pdf_obj* res = pdf_resolve_indirect(
                c, pdf_dict_get(c, pageObj, PDF_NAME(Resources)));
            pdf_obj* fonts = pdf_resolve_indirect(
                c, pdf_dict_get(c, res, PDF_NAME(Font)));
            pdf_obj* font =
                pdf_resolve_indirect(c, pdf_dict_gets(c, fonts, fontKey));
            if (!font)
                fz_throw(c, FZ_ERROR_ARGUMENT, "no such font key on page 0");
            buf = fz_new_buffer_from_copied_data(
                c, reinterpret_cast<const unsigned char*>(cmapText.data()),
                cmapText.size());
            pdf_obj* streamRef = pdf_add_stream(c, doc, buf, nullptr, 0);
            pdf_dict_put_drop(c, font, PDF_NAME(ToUnicode), streamRef);
            pdf_write_options wopts = {0};
            pdf_save_document(c, doc, outPath.c_str(), &wopts);
        }
        fz_always(c) {
            fz_drop_buffer(c, buf);
            pdf_drop_document(c, doc);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
    });
}

std::string readAllBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

class ToUnicodeRecoveryWriteSide : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();
        work_ = fs::temp_directory_path() /
                ("pdfe-188-" + std::to_string(::testing::UnitTest::GetInstance()
                                                  ->random_seed()) +
                 "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(work_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(work_, ec);
    }

    /// Open through core::Document, materialise page 0 (which is where the
    /// recovery runs), then save to @p out. Returns the Document so the caller
    /// can ask it questions.
    std::unique_ptr<Document> openTouchAndSave(const fs::path& in,
                                               const fs::path& out)
    {
        auto doc = Document::open(ctx_, in);
        EXPECT_NE(doc, nullptr);
        if (!doc) return nullptr;
        auto page = doc->page(0);
        EXPECT_NE(page, nullptr);
        EXPECT_NO_THROW(doc->saveAs(out));
        return doc;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    fs::path work_;
};

// ─────────────────────────────────────────────────────────────────────────
// BOUND 3 — opening never causes a save the user did not ask for.
//
// This is the bound that makes the whole decision tolerable: the enrichment
// can only reach disk inside a save the user initiated for their own reasons.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ToUnicodeRecoveryWriteSide, OpeningAnExpertEncodedDocumentDoesNotMarkItModified)
{
    const fs::path in = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(in));

    auto doc = Document::open(ctx_, in);
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->isModified())
        << "the document is modified before any page was even materialised";

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_FALSE(page->extractPlainText().empty())
        << "the page produced no text, so the recovery path cannot have run — "
           "this case would then be asserting nothing";

    EXPECT_FALSE(doc->isModified())
        << "constructing a Page ran the /ToUnicode recovery and left the "
           "document MODIFIED. PDFE would then be able to prompt for (or "
           "autosave) a change the user never made (GitHub #188)";
}

// ─────────────────────────────────────────────────────────────────────────
// BOUNDS 2 + the decision itself — exactly the improvable font is enriched.
//
// The fixture is built for this contrast: /T3 and /T4 share the same six
// glyph procedures and the same /Widths, and differ ONLY in their /Encoding
// /Differences names — Adobe Expert small-caps versus opaque /gx1../gx6.
// Neither carries a /ToUnicode, which is asserted here as an INPUT fact
// before anything is opened.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ToUnicodeRecoveryWriteSide, OnlyTheImprovableFontGainsAToUnicodeOnSave)
{
    const fs::path in = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(in));

    // INPUT fact: the file states nothing about Unicode for any of its fonts.
    std::vector<FontEntry> before;
    inspectPageFonts(*ctx_, in, before);
    ASSERT_GE(before.size(), 3u) << "the fixture must expose /T3, /T4 and /F1";
    for (const auto& f : before) {
        ASSERT_FALSE(f.hasToUnicode)
            << "fixture drift: /" << f.key << " already carries a /ToUnicode, "
            << "so this case can no longer distinguish an enrichment from the "
               "file's own content";
    }

    const fs::path out = work_ / "enriched.pdf";
    ASSERT_NE(openTouchAndSave(in, out), nullptr);
    ASSERT_TRUE(fs::exists(out));

    std::vector<FontEntry> after;
    inspectPageFonts(*ctx_, out, after);

    const FontEntry* t3 = findFont(after, "T3");   // Expert names: recoverable
    const FontEntry* t4 = findFont(after, "T4");   // /gx1../gx6: no information
    ASSERT_NE(t3, nullptr);
    ASSERT_NE(t4, nullptr);

    EXPECT_TRUE(t3->hasToUnicode)
        << "the Expert-set font gained no /ToUnicode on save. Either the "
           "recovery no longer runs, or it no longer reaches the saved bytes — "
           "the behaviour docs/tounicode-recovery-write-side-decision.md "
           "commits to (GitHub #188)";

    // The recovery's own CMap name, so this is the SYNTHESIZED stream and not
    // something a save path invented.
    EXPECT_NE(t3->cmap.find("PDFE-Glyphname-Recovery-UCS"), std::string::npos)
        << "/T3 has a /ToUnicode but it is not PDFE's synthesized CMap";
    // <53> is 'S' — /Ssmall, the first glyph of the authored "SECRET" run.
    EXPECT_NE(t3->cmap.find("<53>"), std::string::npos)
        << "the synthesized CMap maps no code to U+0053, so it cannot be the "
           "recovery for a run that begins with /Ssmall";

    // BOUND 2. This is what separates "recovery" from "invention": the font
    // PDFE cannot improve must be left exactly as the file wrote it.
    EXPECT_FALSE(t4->hasToUnicode)
        << "the opaque /gx1../gx6 font gained a /ToUnicode. Its glyph names "
           "carry no Unicode information, so anything written there is "
           "fabricated (GitHub #188 / #23)";
}

// ─────────────────────────────────────────────────────────────────────────
// BOUND 1 — the recovery never contradicts what the file states.
//
// THE FONT HAS TO BE ONE THE RECOVERY WOULD OTHERWISE HAVE REWRITTEN.
// An ordinary WinAnsi font with an authored /ToUnicode (text_soft_hyphen.pdf's
// /F1, the obvious candidate) proves nothing here: MuPDF resolves its glyph
// names unaided, so plan.improved is false and the font is never a candidate
// at all. Deleting planFont's authored-/ToUnicode early return would leave
// such a case GREEN — a guard no mutation can redden is decoration.
//
// So the control is built: exactly the /T3 that OnlyTheImprovableFontGains...
// shows PDFE DOES rewrite, given an authored table first. Byte-identical
// through open-and-save is the assertion; "still has one" would pass a
// recovery that replaced the author's table with its own.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ToUnicodeRecoveryWriteSide, AuthoredToUnicodeIsNeverReplacedByTheRecovery)
{
    const fs::path base = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(base));

    // Deliberately WRONG as a mapping (<41> is 'A', not the 'S' of /Ssmall) and
    // distinctively named. If PDFE ever "improves" this font, the difference is
    // unmistakable and cannot be confused with a coincidentally equal table.
    const std::string authored =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CMapName /TEST-AUTHORED-188-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "1 beginbfchar\n<01> <0041>\nendbfchar\n"
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";

    const fs::path authoredPdf = work_ / "authored.pdf";
    ASSERT_NO_THROW(
        authorToUnicodeOnFont(*ctx_, base, authoredPdf, "T3", authored));

    // INPUT fact: /T3 now states its own answer, and /T4 still states none.
    std::vector<FontEntry> before;
    inspectPageFonts(*ctx_, authoredPdf, before);
    const FontEntry* t3Before = findFont(before, "T3");
    ASSERT_NE(t3Before, nullptr);
    ASSERT_TRUE(t3Before->hasToUnicode);
    ASSERT_NE(t3Before->cmap.find("TEST-AUTHORED-188-UCS"), std::string::npos);

    const fs::path out = work_ / "authored-resaved.pdf";
    ASSERT_NE(openTouchAndSave(authoredPdf, out), nullptr);

    std::vector<FontEntry> after;
    inspectPageFonts(*ctx_, out, after);
    const FontEntry* t3After = findFont(after, "T3");
    ASSERT_NE(t3After, nullptr);
    ASSERT_TRUE(t3After->hasToUnicode)
        << "the authored /ToUnicode was REMOVED across open-and-save";

    // Compared against the string this test authored, not against another
    // output of the same run.
    EXPECT_EQ(t3After->cmap, authored)
        << "the authored /ToUnicode changed across open-and-save — PDFE is "
           "second-guessing what the file explicitly states (GitHub #188)";
    EXPECT_EQ(t3After->cmap.find("PDFE-Glyphname-Recovery-UCS"),
              std::string::npos)
        << "PDFE's synthesized CMap replaced the author's table on the very "
           "font it is best at 'improving'";
}

// ─────────────────────────────────────────────────────────────────────────
// BOUND 4 — the enrichment is real for someone other than PDFE.
//
// Both sides of this comparison are raw MuPDF on bytes that exist on disk.
// The ORIGINAL is the control: it still yields Corporate-Use PUA codepoints,
// which is precisely the A3-48 shape. If the saved copy reads the same as the
// original, the save wrote nothing useful and the decision in
// docs/tounicode-recovery-write-side-decision.md bought nothing.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ToUnicodeRecoveryWriteSide, SavedFileCarriesTheRecoveryForAReaderThatIsNotPDFE)
{
    const fs::path in = fixture("f18_no_tounicode.pdf");
    ASSERT_TRUE(fs::exists(in));

    const std::string originalText = rawExtractPageText(*ctx_, in);
    ASSERT_FALSE(originalText.empty());
    ASSERT_EQ(originalText.find("SECRET"), std::string::npos)
        << "raw MuPDF already reads SECRET out of the UNMODIFIED fixture, so "
           "this case cannot attribute anything to PDFE. Extracted: "
        << originalText;

    const fs::path out = work_ / "roundtrip.pdf";
    ASSERT_NE(openTouchAndSave(in, out), nullptr);

    const std::string savedText = rawExtractPageText(*ctx_, out);
    EXPECT_NE(savedText.find("SECRET"), std::string::npos)
        << "the saved file does not extract SECRET in a plain MuPDF reader — "
           "the synthesized /ToUnicode did not survive the save. Extracted: "
        << savedText;

    // And nothing was invented for the opaque run: SECRET is authored ONCE.
    size_t occurrences = 0;
    for (size_t p = savedText.find("SECRET"); p != std::string::npos;
         p = savedText.find("SECRET", p + 1))
        ++occurrences;
    EXPECT_EQ(occurrences, 1u)
        << "SECRET appears " << occurrences << " times in the saved file; the "
           "fixture authors it once (the /T3 run). More than once means the "
           "opaque /T4 run was 'recovered' too";
}

// ─────────────────────────────────────────────────────────────────────────
// The decision record must keep existing, and must keep saying what the code
// does. A decision that quietly loses its document is a decision nobody made.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ToUnicodeRecoveryWriteSide, TheWriteSideDecisionIsRecordedAlongsideTheCode)
{
    const fs::path doc =
        repoRoot() / "docs" / "tounicode-recovery-write-side-decision.md";
    ASSERT_TRUE(fs::exists(doc))
        << "docs/tounicode-recovery-write-side-decision.md is missing. GitHub "
           "#188 asked for a RECORDED decision; the tests in this file enforce "
           "bounds whose rationale lives only there.";

    const std::string text = readAllBytes(doc);
    EXPECT_NE(text.find("GitHub #188"), std::string::npos);
    EXPECT_NE(text.find("keep the mutation"), std::string::npos)
        << "the decision record no longer states the option that was chosen";

    // The mutation the decision is ABOUT must still be the one in the code.
    // If it is ever removed (option (b) adopted later), this reds and forces
    // the document to be revisited in the same change.
    const std::string src =
        readAllBytes(repoRoot() / "src" / "core" / "ToUnicodeRecovery.cpp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("pdf_dict_put_drop"), std::string::npos)
        << "ToUnicodeRecovery.cpp no longer mutates the document graph, but "
           "docs/tounicode-recovery-write-side-decision.md still records "
           "'keep the mutation' as the live decision. Update both together.";
}

}  // namespace
