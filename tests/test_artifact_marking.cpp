// GitHub #10 (gap-register PDF-13): header/footer content must be emitted as a
// PDF ARTIFACT, asserting "/Artifact ... BDC" in the SAVED page content stream.
//
// This is a PRODUCT GAP, not a coverage gap. Before the accompanying change:
//
//   * `StructureTag::Artifact` existed in StructureTypes.h:20 and nothing in
//     the tagging path ever produced or consumed it. Grep for "Artifact" across
//     StructureTreeBuilder.cpp, AutoTagger.cpp and ContentStreamTagger.cpp
//     returned nothing. The occurrences in Document.cpp / PageOperations.cpp /
//     PageArtifactGraft.cpp / CombineFilesService.cpp are page-level
//     graft-and-PRESERVE paths; preserving an artifact that already exists is a
//     different capability from EMITTING page furniture as one.
//   * Worse, feeding StructureTag::Artifact through the one path that would
//     accept it produced "/Artifact <</MCID n>> BDC" — an artifact carrying a
//     marked-content id, i.e. page furniture handed to the ParentTree and read
//     out to a screen-reader user as document text. That is the exact defect
//     artifact marking exists to prevent.
//
// The change adds ContentStreamTagger::markArtifacts (+ ArtifactRegion,
// ArtifactType, ArtifactSubtype) and makes tagPageContent emit an Artifact
// region without an /MCID.
//
// Oracle: the saved bytes are reopened with an independent MuPDF handle and the
// page's concatenated content streams are inspected as text. Nothing here
// asserts on the in-memory objects that were just written.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "compliance/ContentStreamTagger.h"
#include "compliance/StructureTypes.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

using namespace pdfe::compliance;
namespace fs = std::filesystem;

namespace {

// Concatenate every content stream of `pageIndex` in the file at `path`.
std::string savedPageContent(pdfe::core::MuPdfContext* mu, const fs::path& path,
                             int pageIndex) {
    std::string bytes;
    const std::string pathStr = path.string();

    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        fz_var(fzDoc);
        // No break/continue/return inside an fz_try body (setjmp/longjmp frame)
        // — the whole body is a chain of nested ifs for that reason.
        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
            pdf_obj* pageObj =
                pdfDoc ? pdf_lookup_page_obj(c, pdfDoc, pageIndex) : nullptr;
            pdf_obj* contents =
                pageObj ? pdf_dict_get(c, pageObj, PDF_NAME(Contents)) : nullptr;
            if (contents) {
                const int n =
                    pdf_is_array(c, contents) ? pdf_array_len(c, contents) : 1;
                for (int i = 0; i < n; ++i) {
                    pdf_obj* s = pdf_is_array(c, contents)
                                     ? pdf_array_get(c, contents, i)
                                     : contents;
                    if (s && pdf_is_stream(c, s)) {
                        fz_buffer* buf = pdf_load_stream(c, s);
                        if (buf) {
                            unsigned char* data = nullptr;
                            const size_t len = fz_buffer_storage(c, buf, &data);
                            if (data && len)
                                bytes.append(reinterpret_cast<char*>(data), len);
                            bytes.push_back('\n');
                            fz_drop_buffer(c, buf);
                        }
                    }
                }
            }
        }
        fz_always(c) {
            if (fzDoc) fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
            // Leave whatever was collected; assertions cover emptiness.
        }
    });
    return bytes;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Whitespace-insensitive containment. A content stream that has been through
// MuPDF's rewriter comes back re-serialised: the fixture's
// "/Artifact <</Type /Pagination /Subtype /Footer>> BDC" is written out as
// "/Artifact<</Type/Pagination/Subtype/Footer>>BDC". Both are the same
// operator, so an assertion about WHAT the artifact declares must not be an
// assertion about how it was spaced.
std::string squeeze(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') out.push_back(ch);
    return out;
}

bool hasSqueezed(const std::string& hay, const char* needle) {
    return squeeze(hay).find(squeeze(needle)) != std::string::npos;
}

int countOf(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The substring of the artifact BDC operator starting at `/Artifact` and
// ending at the first "BDC" after it — the dictionary the artifact declares.
std::string artifactBdcOperator(const std::string& content, size_t from = 0) {
    const size_t a = content.find("/Artifact", from);
    if (a == std::string::npos) return {};
    const size_t b = content.find("BDC", a);
    if (b == std::string::npos) return {};
    return content.substr(a, b + 3 - a);
}

ArtifactRegion header() {
    ArtifactRegion r;
    r.type = ArtifactType::Pagination;
    r.subtype = ArtifactSubtype::Header;
    r.hasBBox = true;
    r.bbox[0] = 72.f; r.bbox[1] = 740.f; r.bbox[2] = 540.f; r.bbox[3] = 772.f;
    r.contentOperators =
        "BT\n/F1 9 Tf\n72 750 Td\n(PDFE ARTIFACT RUNNING HEAD) Tj\nET\n";
    return r;
}

ArtifactRegion footer() {
    ArtifactRegion r;
    r.type = ArtifactType::Pagination;
    r.subtype = ArtifactSubtype::Footer;
    r.contentOperators =
        "BT\n/F1 9 Tf\n300 40 Td\n(PDFE ARTIFACT FOLIO) Tj\nET\n";
    return r;
}

} // namespace

class ArtifactMarkingTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag =
            std::string(ti->test_suite_name()) + "_" + ti->name();
        workPath_ = fs::temp_directory_path() / ("artifact_work_" + tag + ".pdf");
        outPath_  = fs::temp_directory_path() / ("artifact_out_"  + tag + ".pdf");
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);
    }

    void TearDown() override {
        context_.reset();
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);
    }

    bool useFixture(const std::string& name) {
        const fs::path src = fs::path(TEST_FIXTURES_DIR) / name;
        if (!fs::exists(src)) return false;
        std::error_code ec;
        fs::copy_file(src, workPath_, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    fs::path workPath_, outPath_;
};

// ===========================================================================
// The named criterion
// ===========================================================================

TEST_F(ArtifactMarkingTest, HeaderFooterEmittedAsArtifact) {
    ASSERT_TRUE(useFixture("test_single.pdf")) << "test_single.pdf missing";
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    ContentStreamTagger tagger(context_);
    const int emitted = tagger.markArtifacts(doc.get(), 0, {header(), footer()});
    ASSERT_EQ(emitted, 2)
        << "markArtifacts reported doing nothing; a silent no-op would make "
           "every assertion below a statement about the untouched fixture";

    doc->saveAs(outPath_);
    doc.reset();

    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    ASSERT_FALSE(content.empty()) << "saved page 0 has no content stream";

    EXPECT_EQ(countOf(content, "/Artifact"), 2)
        << "expected exactly one /Artifact per region in:\n" << content;
    EXPECT_TRUE(has(content, "BDC")) << content;
    EXPECT_TRUE(has(content, "EMC")) << content;
    EXPECT_TRUE(hasSqueezed(content, "/Type /Pagination")) << content;
    EXPECT_TRUE(hasSqueezed(content, "/Subtype /Header")) << content;
    EXPECT_TRUE(hasSqueezed(content, "/Subtype /Footer")) << content;
    EXPECT_TRUE(has(content, "/BBox [")) << "the header's bbox was dropped";

    // The furniture itself made it into the stream, inside the marking.
    EXPECT_TRUE(has(content, "PDFE ARTIFACT RUNNING HEAD")) << content;
    EXPECT_TRUE(has(content, "PDFE ARTIFACT FOLIO")) << content;
}

// An artifact must NOT carry an /MCID: it has no place in the ParentTree, and
// giving it one is what hands page furniture to a screen reader as content.
TEST_F(ArtifactMarkingTest, ArtifactSequenceCarriesNoMcid) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    ContentStreamTagger tagger(context_);
    ASSERT_EQ(tagger.markArtifacts(doc.get(), 0, {header()}), 1);
    doc->saveAs(outPath_);
    doc.reset();

    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    const std::string op = artifactBdcOperator(content);
    ASSERT_FALSE(op.empty()) << "no /Artifact ... BDC operator found in:\n"
                             << content;
    EXPECT_EQ(op.find("/MCID"), std::string::npos)
        << "the artifact declared a marked-content id: " << op;
}

// Marking appends; it must not rewrite or drop what the page already had.
TEST_F(ArtifactMarkingTest, ArtifactMarkingPreservesExistingContent) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"))
        << "test_tagged_rolemap.pdf missing (generate_tagged_rolemap_pdf.py)";
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    const std::string before = savedPageContent(context_.get(), workPath_, 0);
    ASSERT_TRUE(has(before, "Alpha heading on the first page")) << before;
    ASSERT_TRUE(hasSqueezed(before, "/H1 <</MCID 0>> BDC")) << before;

    ContentStreamTagger tagger(context_);
    ASSERT_EQ(tagger.markArtifacts(doc.get(), 0, {header()}), 1);
    doc->saveAs(outPath_);
    doc.reset();

    const std::string after = savedPageContent(context_.get(), outPath_, 0);
    EXPECT_TRUE(has(after, "Alpha heading on the first page"))
        << "artifact marking destroyed the page's real content";
    EXPECT_TRUE(hasSqueezed(after, "/H1 <</MCID 0>> BDC"))
        << "artifact marking rewrote the page's existing marked content";
    EXPECT_TRUE(has(after, "PDFE ARTIFACT RUNNING HEAD"));
}

// Guard against a "returns success regardless" implementation: an out-of-range
// page, a null document and an empty region list must all report 0 and change
// nothing.
TEST_F(ArtifactMarkingTest, MarkArtifactsReportsZeroWhenItDoesNothing) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    ContentStreamTagger tagger(context_);
    EXPECT_EQ(tagger.markArtifacts(nullptr, 0, {header()}), 0);
    EXPECT_EQ(tagger.markArtifacts(doc.get(), 0, {}), 0);
    EXPECT_EQ(tagger.markArtifacts(doc.get(), 99, {header()}), 0);
    EXPECT_EQ(tagger.markArtifacts(doc.get(), -1, {header()}), 0);

    doc->saveAs(outPath_);
    doc.reset();
    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    EXPECT_FALSE(has(content, "/Artifact"))
        << "a rejected call still wrote an artifact into the page";
}

// The second half of the product fix: routing an Artifact region through
// tagPageContent must not give it an /MCID either.
TEST_F(ArtifactMarkingTest, TagPageContentEmitsArtifactWithoutMcid) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    ContentStreamTagger tagger(context_);
    std::vector<MarkedContentRegion> regions;
    regions.push_back({StructureTag::Artifact, 7, 0, 0});
    regions.push_back({StructureTag::P, 8, 0, 0});
    tagger.tagPageContent(doc.get(), 0, regions);

    doc->saveAs(outPath_);
    doc.reset();

    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    ASSERT_TRUE(has(content, "/Artifact")) << content;
    EXPECT_FALSE(hasSqueezed(content, "/Artifact <</MCID 7>>"))
        << "the artifact was given marked-content id 7:\n" << content;
    const std::string op = artifactBdcOperator(content);
    ASSERT_FALSE(op.empty()) << content;
    EXPECT_EQ(op.find("/MCID"), std::string::npos) << op;
    // CONTROL: a real structure region on the same call still gets its MCID.
    EXPECT_TRUE(hasSqueezed(content, "/P <</MCID 8>> BDC"))
        << "the fix suppressed /MCID for non-artifact regions too:\n" << content;
}

// ===========================================================================
// Preservation: the fixture's own footer artifact through an edit
// ===========================================================================

TEST_F(ArtifactMarkingTest, FixtureFooterArtifactSurvivesTextEdit) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        auto blocks = page->extractText();
        pdfe::textedit::TextEditModel model(blocks);
        std::vector<pdfe::textedit::TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));

        int target = -1;
        for (int i = 0; i < static_cast<int>(spans.size()); ++i)
            if (spans[i].text.size() >= 3) { target = i; break; }
        ASSERT_GE(target, 0) << "fixture page 0 must expose an editable span";

        std::vector<std::pair<int, std::u32string>> mods;
        mods.push_back({target, U"Edited alpha heading text"});
        pdfe::textedit::ContentStreamEditor editor(context_.get());
        editor.applyEdits(page.get(), doc.get(), spans, mods);
    }

    doc->saveAs(outPath_);
    doc.reset();

    const std::string after = savedPageContent(context_.get(), outPath_, 0);
    ASSERT_FALSE(after.empty());
    EXPECT_TRUE(has(after, "/Artifact"))
        << "the text edit dropped the page's artifact marking, promoting the "
           "footer to document content:\n" << after;
    EXPECT_TRUE(hasSqueezed(after, "/Subtype /Footer")) << after;
    const std::string op = artifactBdcOperator(after);
    ASSERT_FALSE(op.empty()) << after;
    EXPECT_EQ(op.find("/MCID"), std::string::npos)
        << "the edit gave the surviving artifact an MCID: " << op;
}
