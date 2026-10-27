// GitHub #172 and #173 — the two accessibility capabilities PDFE had built and
// then wired to nothing.
//
// #172  AutoTagger sets StructureNode::needsAltText on a /Figure it cannot
//       describe, and deliberately refuses to fabricate a placeholder. The flag
//       appeared in exactly two files: the one that set it and the one that
//       declared it. No document ever failed validation for an undescribed
//       image. validatePdfUA now has a 7.3 (Graphics) rule that reads it.
//
// #173  ContentStreamTagger::markArtifacts emits "/Artifact ... BDC" and returns
//       a count, and had ZERO callers. Nothing in PDFE decided that a block was
//       a running head, so page furniture stayed tagged as content and was read
//       aloud. PageFurnitureDetector makes that decision, BEFORE MCIDs are
//       assigned, and PdfUAConverter::convertAndSave calls markArtifacts.
//
// Every assertion below is preceded by a non-vacuity guard. The test this
// replaces the reasoning of — the old AutoTaggerTest.FiguresHaveAltText — was
// green for its entire life because it looped over an empty set.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/StextOptions.h"
#include "compliance/AutoTagger.h"
#include "compliance/ContentStreamTagger.h"
#include "compliance/PageFurnitureDetector.h"
#include "compliance/PdfUAConverter.h"
#include "compliance/StructureTreeBuilder.h"
#include "compliance/StructureTypes.h"
#include "compliance/ComplianceTypes.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
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

// ---------------------------------------------------------------------------
// A purpose-built three-page document, written as literal PDF bytes to a temp
// file. Built here rather than committed as a fixture because every coordinate
// in it is load-bearing for what is being tested, and because a new fixture file
// reddens tests in other suites.
//
// Page height 792; the detector's default band is 15% = 118.8pt.
//
//   user y  fitz y  role
//   ------  ------  -------------------------------------------------------
//     750      42   RUNNING HEAD, identical on all 3 pages   -> FURNITURE
//     680     112   top band, page 1 ONLY                    -> control: in
//                                                               the band but
//                                                               does not repeat
//     500     292   identical body line on all 3 pages       -> control:
//                                                               repeats but is
//                                                               not in a band
//     300     492   body text unique to each page            -> content
//      40     752   "Page N" folio                           -> FURNITURE
//                                                               (digit fold)
// ---------------------------------------------------------------------------
const char* const kRunningHead = "CONFIDENTIAL DRAFT DO NOT CIRCULATE";
const char* const kOnceOnlyTopLine = "UNIQUE TOP LINE PAGE ONE ONLY";
const char* const kRepeatedBodyLine = "REPEATED BODY SENTENCE ON EVERY PAGE";

std::string pageContentFor(int pageNumber) {
    std::string s;
    s += "BT /F1 10 Tf 72 750 Td (";
    s += kRunningHead;
    s += ") Tj ET\n";
    if (pageNumber == 1) {
        s += "BT /F1 10 Tf 72 680 Td (";
        s += kOnceOnlyTopLine;
        s += ") Tj ET\n";
    }
    s += "BT /F1 12 Tf 72 500 Td (";
    s += kRepeatedBodyLine;
    s += ") Tj ET\n";
    s += "BT /F1 12 Tf 72 300 Td (Body text unique to page " +
         std::to_string(pageNumber) + " of this document) Tj ET\n";
    s += "BT /F1 10 Tf 290 40 Td (Page " + std::to_string(pageNumber) +
         ") Tj ET\n";
    return s;
}

// Serialise 1-based `objects` as a minimal uncompressed PDF with a correct
// xref table. Object 1 must be the catalog.
bool writePdfObjects(const fs::path& path,
                     const std::vector<std::string>& objects) {
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const size_t xrefPos = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefPos) +
           "\n%%EOF\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    return f.good();
}

// Emit a minimal, uncompressed, 3-page PDF with a correct xref table.
bool writeFurnitureDocument(const fs::path& path) {
    const int kPages = 3;
    std::vector<std::string> objects;   // 1-based: objects[i] is object i+1

    // 1 Catalog, 2 Pages, then per page: page dict + contents, then the font.
    const int fontObj = 3 + kPages * 2;

    std::string kids;
    for (int i = 0; i < kPages; ++i) {
        kids += std::to_string(3 + i * 2) + " 0 R ";
    }

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Count " + std::to_string(kPages) +
                      " /Kids [ " + kids + "] >>");

    for (int i = 0; i < kPages; ++i) {
        const int contentsObj = 4 + i * 2;
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 " + std::to_string(fontObj) +
            " 0 R >> >> /Contents " + std::to_string(contentsObj) + " 0 R >>");

        const std::string content = pageContentFor(i + 1);
        objects.push_back("<< /Length " + std::to_string(content.size()) +
                          " >>\nstream\n" + content + "endstream");
    }

    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding >>");

    return writePdfObjects(path, objects);
}

// Concatenate every content stream of `pageIndex` in the file at `path`.
std::string savedPageContent(pdfe::core::MuPdfContext* mu, const fs::path& path,
                             int pageIndex) {
    std::string bytes;
    const std::string pathStr = path.string();

    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        fz_var(fzDoc);
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
        }
    });
    return bytes;
}

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

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// SYNTHETIC ROTATED-WATERMARK FIXTURE (GitHub #173, second heuristic).
//
// ONE page, 612 x 792, deliberately: with a single page the band-plus-repeat
// detector returns empty by construction (pageCount < minPages), so anything
// found here was found by the watermark rule and by nothing else.
//
// Contents, and the stext geometry each produces (MuPDF device space, y-down,
// measured — not guessed):
//
//   text                             baseline dir      bbox            centre
//   -------------------------------  ----------------  --------------  ---------
//   "CONFIDENTIAL" 48pt at 45 deg    (0.7071,-0.7071)  137,243-433,538  285,390
//   heading 36pt, user y=396         (1,0)              96,357-596,407  346,382
//   axis label 9pt at -45 deg        (0.7071, 0.7071)  278,345-341,408  310,377
//   sidebar 40pt at 90 deg, left     (0,-1)              2,296- 57,592   30,444
//   body line 1, 12pt, user y=600    (1,0)             100,179-379,196  240,187
//   body line 2, 12pt, user y=450    (1,0)             100,329-377,346  238,337
//   body line 3, 12pt, user y=250    (1,0)             100,529-313,546  207,537
//
// The page centre is (306, 396), so the watermark centre is 3.4% of the page
// width and 0.7% of its height away from it -- comfortably "centred".
//
// Four things in here are deliberately adversarial, and each pins one part of
// the rule so that dropping that part alone is caught:
//
//  * The HEADING is large (36pt, over the 5%-of-short-side floor) and centred
//    (6.5% / 1.8% off centre) -- it fails ONLY the rotation test. Without it, a
//    build that stopped checking rotation would still pass every assertion,
//    because at 12pt the body text is too small to trip the size arm.
//
//  * The AXIS LABEL is rotated (-45 degrees) and centred (0.6% / 2.4% off
//    centre) -- it fails ONLY the size test. Without it, dropping the size
//    conjunct would change nothing, because nothing else on the page is
//    rotated.
//
//  * The SIDEBAR TITLE is rotated (90 degrees) and large (40pt) -- it fails
//    ONLY the centring test, sitting 45% of the page width off centre in the
//    left margin. Without it, dropping the centring conjunct would change
//    nothing.
//
//  * The two lower BODY LINES have centres, (238,337) and (207,537), that fall
//    INSIDE the watermark's axis-aligned bounding box. A watermark rule that
//    reused the header/footer centre-containment match would therefore artifact
//    them, and "the watermark is excluded" would be indistinguishable from
//    "everything is excluded".
//
// Both directions are asserted explicitly below.
// ---------------------------------------------------------------------------
const char* const kWatermarkText = "CONFIDENTIAL";
const char* const kWmHeading = "ANNUAL REPORT SUMMARY";
const char* const kWmAxisLabel = "sideways axis label";
const char* const kWmSidebar = "SIDEBAR TITLE";
const char* const kWmBodyLine1 =
    "First body paragraph of the document body text here";
const char* const kWmBodyLine2 =
    "Second body paragraph sitting near the page centre";
const char* const kWmBodyLine3 = "Third body paragraph lower on the page";

bool writeRotatedWatermarkDocument(const fs::path& path) {
    std::string content;
    // q/Q so the grey fill cannot leak; 45 degrees == cos/sin 0.7071.
    content += "q 0.85 0.85 0.85 rg BT /F1 48 Tf "
               "0.7071 0.7071 -0.7071 0.7071 174 264 Tm (";
    content += kWatermarkText;
    content += ") Tj ET Q\n";
    // Large, centred, horizontal: fails the rotation test and nothing else.
    content += std::string("BT /F1 36 Tf 96 396 Td (") + kWmHeading +
               ") Tj ET\n";
    // Rotated and centred but only 9pt: fails the size test and nothing else.
    content += std::string("BT /F1 9 Tf 0.7071 -0.7071 0.7071 0.7071 "
                           "280 440 Tm (") + kWmAxisLabel + ") Tj ET\n";
    // Rotated and large but 45% of the page width off centre: fails the
    // centring test and nothing else.
    content += std::string("BT /F1 40 Tf 0 1 -1 0 45 200 Tm (") +
               kWmSidebar + ") Tj ET\n";
    content += std::string("BT /F1 12 Tf 100 600 Td (") + kWmBodyLine1 +
               ") Tj ET\n";
    content += std::string("BT /F1 12 Tf 100 450 Td (") + kWmBodyLine2 +
               ") Tj ET\n";
    content += std::string("BT /F1 12 Tf 100 250 Td (") + kWmBodyLine3 +
               ") Tj ET\n";

    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) +
                      " >>\nstream\n" + content + "endstream");
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding >>");
    return writePdfObjects(path, objects);
}

// The text of each stext block of `pageIndex`, IN THE ORDER AutoTagger::tagPage
// walks them, extracted with the same stext options tagPage uses. tagPage emits
// exactly one StructureNode per block in this order, so index i here names the
// same block as root.children[i] -- which is what lets the tests below say
// "THIS text was artifacted and THAT text was not" instead of only counting.
std::vector<std::string> stextBlockTexts(pdfe::core::MuPdfContext* mu,
                                         const fs::path& path,
                                         int pageIndex) {
    std::vector<std::string> texts;
    const std::string pathStr = path.string();
    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        fz_page* page = nullptr;
        fz_stext_page* sp = nullptr;
        fz_var(fzDoc);
        fz_var(page);
        fz_var(sp);
        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            page = fz_load_page(c, fzDoc, pageIndex);
            fz_stext_options opts = pdfe::core::pdfeStextOptions(
                FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_PRESERVE_IMAGES);
            sp = fz_new_stext_page_from_page(c, page, &opts);
            for (fz_stext_block* b = sp->first_block; b; b = b->next) {
                std::string t;
                if (b->type == FZ_STEXT_BLOCK_TEXT) {
                    for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
                        for (fz_stext_char* ch = l->first_char; ch;
                             ch = ch->next) {
                            char buf[8] = {0};
                            const int n = fz_runetochar(buf, ch->c);
                            if (n > 0) t.append(buf, static_cast<size_t>(n));
                        }
                        t.push_back(' ');
                    }
                }
                texts.push_back(t);
            }
        }
        fz_always(c) {
            fz_drop_stext_page(c, sp);
            fz_drop_page(c, page);
            fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
        }
    });
    return texts;
}

// The "/Artifact ... BDC" operator text starting at `from`.
std::string artifactBdcOperator(const std::string& content, size_t from = 0) {
    const size_t a = content.find("/Artifact", from);
    if (a == std::string::npos) return {};
    const size_t b = content.find("BDC", a);
    if (b == std::string::npos) return {};
    return content.substr(a, b + 3 - a);
}

} // namespace

// ===========================================================================
// GitHub #173 — page furniture detection and the markArtifacts call site
// ===========================================================================

class PageFurnitureTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag =
            std::string(ti->test_suite_name()) + "_" + ti->name();
        srcPath_ = fs::temp_directory_path() / ("furniture_src_" + tag + ".pdf");
        outPath_ = fs::temp_directory_path() / ("furniture_out_" + tag + ".pdf");
        std::error_code ec;
        fs::remove(srcPath_, ec);
        fs::remove(outPath_, ec);
        ASSERT_TRUE(writeFurnitureDocument(srcPath_))
            << "could not stage the three-page furniture document";
        doc_ = pdfe::core::Document::open(context_, srcPath_);
        ASSERT_NE(doc_, nullptr) << "staged document did not open";
        ASSERT_EQ(doc_->pageCount(), 3);
    }

    void TearDown() override {
        doc_.reset();
        context_.reset();
        std::error_code ec;
        fs::remove(srcPath_, ec);
        fs::remove(outPath_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    fs::path srcPath_, outPath_;
};

TEST_F(PageFurnitureTest, DetectorFindsTheRunningHeadAndTheFolio) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());

    // NON-VACUITY: everything below is a statement about members of this set.
    ASSERT_FALSE(furniture.empty())
        << "no page furniture detected at all; every assertion about what was "
           "marked would be a statement about an empty set";

    std::set<int> headerPages, footerPages;
    for (const auto& r : furniture) {
        if (r.subtype == ArtifactSubtype::Header) headerPages.insert(r.pageIndex);
        if (r.subtype == ArtifactSubtype::Footer) footerPages.insert(r.pageIndex);
    }

    EXPECT_EQ(headerPages.size(), 3u)
        << "the running head repeats on all three pages but was detected on "
        << headerPages.size();
    EXPECT_EQ(footerPages.size(), 3u)
        << "the folio (\"Page 1\" / \"Page 2\" / \"Page 3\") must be detected "
           "through the digit fold; detected on "
        << footerPages.size() << " page(s)";

    // The folio's normalized form is the evidence the digit fold ran: without
    // it "page 1" and "page 2" are different strings and never repeat.
    bool sawFoldedFolio = false;
    for (const auto& r : furniture) {
        if (r.subtype == ArtifactSubtype::Footer &&
            r.normalizedText.find('#') != std::string::npos) {
            sawFoldedFolio = true;
        }
    }
    EXPECT_TRUE(sawFoldedFolio)
        << "no detected footer carried a digit-folded page number";
}

// CONTROL: repetition alone is not furniture. An identical sentence in the
// MIDDLE of every page is body content.
TEST_F(PageFurnitureTest, RepeatedBodyTextOutsideTheBandsIsNotFurniture) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());
    ASSERT_FALSE(furniture.empty())
        << "detector found nothing, so this control proves nothing -- see "
           "DetectorFindsTheRunningHeadAndTheFolio";

    const std::string needle =
        normalizeForRepeatMatching(kRepeatedBodyLine);
    ASSERT_FALSE(needle.empty());

    for (const auto& r : furniture) {
        EXPECT_EQ(r.normalizedText.find(needle), std::string::npos)
            << "a body sentence that merely repeats was marked as page "
               "furniture: " << r.normalizedText;
    }
}

// CONTROL: being in the band alone is not furniture either. A line that appears
// in the header band of page 1 and nowhere else is content.
TEST_F(PageFurnitureTest, InBandTextThatDoesNotRepeatIsNotFurniture) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());
    ASSERT_FALSE(furniture.empty())
        << "detector found nothing, so this control proves nothing";

    const std::string needle =
        normalizeForRepeatMatching(kOnceOnlyTopLine);
    ASSERT_FALSE(needle.empty());

    for (const auto& r : furniture) {
        EXPECT_EQ(r.normalizedText.find(needle), std::string::npos)
            << "a one-off line in the header band was marked as furniture: "
            << r.normalizedText;
    }
}

// The ordering fix: furniture is decided BEFORE MCIDs are handed out, so an
// artifact node never receives one.
TEST_F(PageFurnitureTest, FurnitureIsTaggedArtifactWithNoMcidAndBodyKeepsIts) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());
    ASSERT_FALSE(furniture.empty());

    const auto root = detector.tagDocument(doc_.get(), furniture);

    int artifactNodes = 0, contentNodes = 0;
    for (const auto& child : root.children) {
        ASSERT_NE(child, nullptr);
        if (child->tag == StructureTag::Artifact) {
            ++artifactNodes;
            EXPECT_EQ(child->mcid, -1)
                << "an artifact was given marked-content id " << child->mcid
                << ", which puts page furniture in the ParentTree and hands it "
                   "to a screen reader as document text";
        } else {
            ++contentNodes;
            EXPECT_GE(child->mcid, 0)
                << "a body content node lost its MCID";
        }
    }

    ASSERT_GT(artifactNodes, 0)
        << "tagging produced no artifact nodes, so the MCID assertion above "
           "iterated an empty set";
    // CONTROL: the fix did not simply artifact the whole document.
    ASSERT_GT(contentNodes, 0)
        << "every node was tagged as an artifact; body content was swallowed";
}

// THE CALL SITE, end to end: PdfUAConverter::convertAndSave ->
// ContentStreamTagger::markArtifacts, asserted against the SAVED bytes.
TEST_F(PageFurnitureTest, ConvertAndSaveEmitsArtifactMarkingForPageFurniture) {
    PdfUAConverter converter(context_);
    converter.convertAndSave(doc_.get(), outPath_, "en-US");
    ASSERT_TRUE(fs::exists(outPath_));

    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    ASSERT_FALSE(content.empty()) << "saved page 0 has no content stream";

    EXPECT_TRUE(has(content, "/Artifact"))
        << "the production tagging pass emitted no artifact marking at all:\n"
        << content;
    EXPECT_TRUE(hasSqueezed(content, "/Type /Pagination")) << content;
    EXPECT_TRUE(hasSqueezed(content, "/Subtype /Header")) << content;
    EXPECT_TRUE(hasSqueezed(content, "/Subtype /Footer")) << content;
    EXPECT_TRUE(has(content, "/BBox [")) << "the artifact's bounding box was "
                                            "dropped:\n" << content;

    const std::string op = artifactBdcOperator(content);
    ASSERT_FALSE(op.empty()) << content;
    EXPECT_EQ(op.find("/MCID"), std::string::npos)
        << "the emitted artifact carried a marked-content id: " << op;

    // CONTROL: real content on the same page still carries an /MCID, so the
    // absence above is a property of artifacts and not of the whole stream.
    EXPECT_TRUE(has(content, "/MCID"))
        << "no marked-content id anywhere on the page; body content was not "
           "tagged either, which would make the artifact assertion vacuous:\n"
        << content;
}

// Furniture must be absent from the structure tree, not merely marked.
TEST_F(PageFurnitureTest, FurnitureIsAbsentFromTheSavedStructureTree) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());
    ASSERT_FALSE(furniture.empty());

    const auto tagged = detector.tagDocument(doc_.get(), furniture);
    int artifactNodes = 0, contentNodes = 0;
    for (const auto& child : tagged.children) {
        if (child && child->tag == StructureTag::Artifact) ++artifactNodes;
        else if (child) ++contentNodes;
    }
    ASSERT_GT(artifactNodes, 0) << "nothing was artifacted";
    ASSERT_GT(contentNodes, 0) << "nothing was left as content";

    PdfUAConverter converter(context_);
    converter.convertAndSave(doc_.get(), outPath_, "en-US");
    ASSERT_TRUE(fs::exists(outPath_));

    auto saved = pdfe::core::Document::open(context_, outPath_);
    ASSERT_NE(saved, nullptr);
    PdfUAConverter reader(context_);
    const StructureNode structRoot = reader.readStructureTree(saved.get());

    ASSERT_FALSE(structRoot.children.empty())
        << "the saved document has no structure elements at all, so 'furniture "
           "is not among them' would be trivially true";
    EXPECT_EQ(static_cast<int>(structRoot.children.size()), contentNodes)
        << "the structure tree holds " << structRoot.children.size()
        << " elements but only " << contentNodes
        << " nodes were content; the " << artifactNodes
        << " artifact node(s) leaked into it";
}

// ===========================================================================
// GitHub #173 — the SECOND, distinct signal: rotated centred watermarks
// ===========================================================================

class RotatedWatermarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag =
            std::string(ti->test_suite_name()) + "_" + ti->name();
        srcPath_ = fs::temp_directory_path() / ("watermark_src_" + tag + ".pdf");
        outPath_ = fs::temp_directory_path() / ("watermark_out_" + tag + ".pdf");
        std::error_code ec;
        fs::remove(srcPath_, ec);
        fs::remove(outPath_, ec);
        ASSERT_TRUE(writeRotatedWatermarkDocument(srcPath_))
            << "could not stage the rotated-watermark document";
        doc_ = pdfe::core::Document::open(context_, srcPath_);
        ASSERT_NE(doc_, nullptr) << "staged watermark document did not open";
        ASSERT_EQ(doc_->pageCount(), 1);
    }

    void TearDown() override {
        doc_.reset();
        context_.reset();
        std::error_code ec;
        fs::remove(srcPath_, ec);
        fs::remove(outPath_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    fs::path srcPath_, outPath_;
};

// DISTINCTNESS. The band-plus-repeat detector cannot see this watermark: the
// document has one page, so its repeat test returns before looking at anything.
// Whatever the watermark test finds below was found by the new rule alone.
TEST_F(RotatedWatermarkTest, BandAndRepeatSignalFindsNothingOnThisDocument) {
    AutoTagger detector(context_);
    const auto furniture = detector.detectPageFurniture(doc_.get());
    EXPECT_TRUE(furniture.empty())
        << "the header/footer band-plus-repeat detector claimed "
        << furniture.size()
        << " region(s) on a one-page document; the watermark result would then "
           "not be attributable to the watermark rule";
}

TEST_F(RotatedWatermarkTest, DetectorFindsTheRotatedCentredWatermark) {
    AutoTagger detector(context_);
    const auto marks = detector.detectWatermarks(doc_.get());

    // NON-VACUITY.
    ASSERT_FALSE(marks.empty())
        << "no rotated centred watermark detected at all; every assertion "
           "below would be a statement about an empty set";

    EXPECT_EQ(marks.size(), 1u)
        << "exactly one block on this page is a watermark; " << marks.size()
        << " were claimed, so horizontal body text was swept in too";

    const std::string needle = normalizeForRepeatMatching(kWatermarkText);
    ASSERT_FALSE(needle.empty());
    bool sawWatermarkText = false;
    for (const auto& r : marks) {
        EXPECT_EQ(r.pageIndex, 0);
        EXPECT_EQ(r.subtype, ArtifactSubtype::Watermark)
            << "a watermark was recorded with a non-watermark artifact subtype";
        EXPECT_EQ(r.match, FurnitureMatch::BlockBoxEquals)
            << "the watermark region asked to be matched by centre "
               "containment, which would artifact every body paragraph under "
               "its bounding box";
        if (r.normalizedText.find(needle) != std::string::npos)
            sawWatermarkText = true;
    }
    EXPECT_TRUE(sawWatermarkText)
        << "something was detected, but it was not the watermark text";
}

// CONTROL: horizontal body text is not a watermark, however central it is.
// Body line 2 sits within 40pt of the page centre.
TEST_F(RotatedWatermarkTest, CentredHorizontalBodyTextIsNotAWatermark) {
    AutoTagger detector(context_);
    const auto marks = detector.detectWatermarks(doc_.get());
    ASSERT_FALSE(marks.empty())
        << "detector found nothing, so this control proves nothing -- see "
           "DetectorFindsTheRotatedCentredWatermark";

    for (const char* body :
         {kWmHeading, kWmAxisLabel, kWmSidebar, kWmBodyLine1, kWmBodyLine2,
          kWmBodyLine3}) {
        const std::string needle = normalizeForRepeatMatching(body);
        ASSERT_FALSE(needle.empty());
        for (const auto& r : marks) {
            EXPECT_EQ(r.normalizedText.find(needle), std::string::npos)
                << "horizontal text was called a watermark: "
                << r.normalizedText;
        }
    }
}

// THE BOTH-DIRECTIONS ASSERTION, by TEXT rather than by count.
//
// tagPage emits exactly one StructureNode per stext block, in stext order, so
// block i and root.children[i] are the same block. That pairing is what lets
// this say "the watermark text was excluded AND this specific body text was
// not", instead of a count that a rule excluding everything could also satisfy.
TEST_F(RotatedWatermarkTest, WatermarkIsExcludedFromStructureAndBodyTextIsNot) {
    AutoTagger detector(context_);
    // NO "something was detected" guard here, deliberately. That guard would be
    // the FIRST thing to fire if the watermark rule were switched off, and the
    // test would then die before ever asking whether the watermark reached the
    // structure tree -- the property this test exists for. Non-vacuity is
    // established below against the FIXTURE (which blocks exist) instead of
    // against the detector's own answer.
    const auto marks = detector.detectWatermarks(doc_.get());

    const auto blockTexts = stextBlockTexts(context_.get(), srcPath_, 0);
    ASSERT_GE(blockTexts.size(), 7u)
        << "the staged page did not segment into the expected blocks; got "
        << blockTexts.size();

    const auto root = detector.tagDocument(doc_.get(), marks);
    ASSERT_EQ(root.children.size(), blockTexts.size())
        << "tagPage did not emit one node per stext block, so the index "
           "pairing this test relies on does not hold";

    int watermarkBlocks = 0, bodyBlocks = 0, headingBlocks = 0,
        axisLabelBlocks = 0, sidebarBlocks = 0;
    for (size_t i = 0; i < blockTexts.size(); ++i) {
        const std::string& text = blockTexts[i];
        const auto& node = root.children[i];
        ASSERT_NE(node, nullptr);

        if (text.find(kWatermarkText) != std::string::npos) {
            ++watermarkBlocks;
            EXPECT_EQ(node->tag, StructureTag::Artifact)
                << "the rotated centred watermark \"" << text
                << "\" was left in the structure tree as "
                << structureTagToName(node->tag)
                << ", so a screen reader reads it aloud as document text";
            EXPECT_EQ(node->mcid, -1)
                << "the watermark was given marked-content id " << node->mcid
                << ", which puts it in the ParentTree";
        } else if (text.find("body paragraph") != std::string::npos) {
            ++bodyBlocks;
            EXPECT_NE(node->tag, StructureTag::Artifact)
                << "genuine body text \"" << text
                << "\" was excluded from the structure tree as an artifact; "
                   "the watermark rule swallowed real content";
            EXPECT_GE(node->mcid, 0)
                << "genuine body text \"" << text << "\" lost its MCID";
        } else if (text.find(kWmHeading) != std::string::npos) {
            ++headingBlocks;
            EXPECT_NE(node->tag, StructureTag::Artifact)
                << "the large centred HORIZONTAL heading \"" << text
                << "\" was excluded from the structure tree as an artifact; "
                   "it is display-sized and centred but it is not rotated, so "
                   "the rotation half of the watermark rule is not being "
                   "applied";
            EXPECT_GE(node->mcid, 0)
                << "the heading \"" << text << "\" lost its MCID";
        } else if (text.find(kWmAxisLabel) != std::string::npos) {
            ++axisLabelBlocks;
            EXPECT_NE(node->tag, StructureTag::Artifact)
                << "the SMALL rotated centred axis label \"" << text
                << "\" was excluded from the structure tree as an artifact; "
                   "it is rotated and centred but it is 9pt, so the "
                   "display-size half of the watermark rule is not being "
                   "applied";
            EXPECT_GE(node->mcid, 0)
                << "the axis label \"" << text << "\" lost its MCID";
        } else if (text.find(kWmSidebar) != std::string::npos) {
            ++sidebarBlocks;
            EXPECT_NE(node->tag, StructureTag::Artifact)
                << "the OFF-CENTRE rotated sidebar title \"" << text
                << "\" was excluded from the structure tree as an artifact; "
                   "it is rotated and display-sized but it sits in the left "
                   "margin, so the centring half of the watermark rule is "
                   "not being applied";
            EXPECT_GE(node->mcid, 0)
                << "the sidebar title \"" << text << "\" lost its MCID";
        }
    }

    ASSERT_EQ(watermarkBlocks, 1)
        << "the watermark block was not found among the page's blocks, so the "
           "exclusion assertion never ran";
    ASSERT_EQ(bodyBlocks, 3)
        << "expected three body paragraphs to check against, found "
        << bodyBlocks << "; the 'body text survives' assertion is that weak";
    ASSERT_EQ(headingBlocks, 1)
        << "the large centred horizontal heading was not found among the "
           "page's blocks, so the rotation-half assertion never ran";
    ASSERT_EQ(axisLabelBlocks, 1)
        << "the small rotated centred axis label was not found among the "
           "page's blocks, so the size-half assertion never ran";
    ASSERT_EQ(sidebarBlocks, 1)
        << "the off-centre rotated sidebar title was not found among the "
           "page's blocks, so the centring-half assertion never ran";
}

// End to end through the production path: convertAndSave must emit the
// watermark as "/Artifact <</Type /Pagination /Subtype /Watermark ...>> BDC",
// and the body must still be in the saved structure tree.
TEST_F(RotatedWatermarkTest, ConvertAndSaveMarksTheWatermarkAndKeepsTheBody) {
    AutoTagger detector(context_);
    // Again no "something was detected" guard: switching the rule off must be
    // caught by the /Subtype /Watermark assertion below, not swallowed by a
    // setup ASSERT.
    const auto marks = detector.detectWatermarks(doc_.get());

    // The expected structure-element count comes from the FIXTURE, not from
    // this run's tagging. Comparing the saved tree against a count derived from
    // the same tagging pass would let both sides move together: a rule that
    // artifacted the body would produce fewer nodes AND a smaller expectation,
    // and the equality would still hold. The page has five text blocks; exactly
    // one of them, the watermark, is furniture.
    const auto blockTexts = stextBlockTexts(context_.get(), srcPath_, 0);
    ASSERT_EQ(blockTexts.size(), 7u)
        << "the staged page did not segment into the expected seven blocks";
    const int kExpectedStructureElements = 6;

    PdfUAConverter converter(context_);
    converter.convertAndSave(doc_.get(), outPath_, "en-US");
    ASSERT_TRUE(fs::exists(outPath_));

    const std::string content = savedPageContent(context_.get(), outPath_, 0);
    ASSERT_FALSE(content.empty()) << "saved page 0 has no content stream";
    EXPECT_TRUE(hasSqueezed(content, "/Subtype /Watermark"))
        << "convertAndSave emitted no watermark artifact marking:\n" << content;
    EXPECT_TRUE(hasSqueezed(content, "/Type /Pagination")) << content;

    const std::string op = artifactBdcOperator(content);
    ASSERT_FALSE(op.empty()) << content;
    EXPECT_EQ(op.find("/MCID"), std::string::npos)
        << "the emitted watermark artifact carried a marked-content id: " << op;

    // CONTROL on the saved bytes: the body is still marked structure content,
    // so "no /MCID on the artifact" is a property of the artifact and not of a
    // stream in which nothing was tagged.
    EXPECT_TRUE(has(content, "/MCID"))
        << "no marked-content id anywhere on the saved page; the body was not "
           "tagged either:\n" << content;

    auto saved = pdfe::core::Document::open(context_, outPath_);
    ASSERT_NE(saved, nullptr);
    PdfUAConverter reader(context_);
    const StructureNode structRoot = reader.readStructureTree(saved.get());
    ASSERT_FALSE(structRoot.children.empty())
        << "the saved document has no structure elements at all, so "
           "'the watermark is not among them' would be trivially true";
    EXPECT_EQ(static_cast<int>(structRoot.children.size()),
              kExpectedStructureElements)
        << "the saved structure tree holds " << structRoot.children.size()
        << " elements; the page's six non-watermark blocks (one heading, one "
           "rotated axis label, one rotated sidebar title, three body "
           "paragraphs) must each be there and the watermark must not";
}

// ===========================================================================
// GitHub #172 — the PDF/UA 7.3 (Graphics) alternative-text rule
// ===========================================================================

class FigureAltTextRuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag =
            std::string(ti->test_suite_name()) + "_" + ti->name();
        workPath_ = fs::temp_directory_path() / ("altrule_work_" + tag + ".pdf");
        outPath_ = fs::temp_directory_path() / ("altrule_out_" + tag + ".pdf");
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);

        const fs::path src = fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!fs::exists(src)) GTEST_SKIP() << "test_single.pdf not found";
        fs::copy_file(src, workPath_, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "could not stage test_single.pdf";
    }

    void TearDown() override {
        context_.reset();
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);
    }

    // Write a structure tree holding one /P and one /Figure, the figure given
    // `altText` (empty means no /Alt is emitted at all), and save it.
    void stageTaggedDocument(const std::string& altText) {
        auto doc = pdfe::core::Document::open(context_, workPath_);
        ASSERT_NE(doc, nullptr);

        StructureNode root;
        root.tag = StructureTag::Document;

        auto para = std::make_shared<StructureNode>();
        para->tag = StructureTag::P;
        para->pageIndex = 0;
        para->mcid = 0;
        root.children.push_back(para);

        auto figure = std::make_shared<StructureNode>();
        figure->tag = StructureTag::Figure;
        figure->pageIndex = 0;
        figure->mcid = 1;
        figure->altText = altText;
        figure->needsAltText = altText.empty();
        root.children.push_back(figure);

        StructureTreeBuilder builder(context_);
        builder.setDocumentLanguage(doc.get(), "en-US");
        builder.buildTree(doc.get(), root);
        doc->saveAs(outPath_);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    fs::path workPath_, outPath_;
};

TEST_F(FigureAltTextRuleTest, FigureWithoutAlternativeTextFailsValidation) {
    stageTaggedDocument("");
    ASSERT_TRUE(fs::exists(outPath_));

    auto saved = pdfe::core::Document::open(context_, outPath_);
    ASSERT_NE(saved, nullptr);

    PdfUAConverter converter(context_);

    // NON-VACUITY: the figure survived the save/reload round trip. Without this
    // guard "the rule fired" could be satisfied by a document with no figures
    // at all -- exactly how the old FiguresHaveAltText stayed green.
    const StructureNode structRoot = converter.readStructureTree(saved.get());
    int figures = 0;
    for (const auto& c : structRoot.children)
        if (c && c->tag == StructureTag::Figure) ++figures;
    ASSERT_GT(figures, 0)
        << "no /Figure element was read back from the saved structure tree";

    const auto missing = PdfUAConverter::figuresMissingAltText(structRoot);
    ASSERT_FALSE(missing.empty())
        << "the figure carries no /Alt but needsAltText was not raised on "
           "ingestion, so the rule has nothing to read";

    const auto result = converter.validatePdfUA(saved.get());

    bool foundAltError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "7.3") {
            foundAltError = true;
            EXPECT_EQ(issue.severity, IssueSeverity::Error)
                << "a figure with no alternative text is a PDF/UA failure, not "
                   "a warning";
            EXPECT_NE(issue.description.find("alternative text"), std::string::npos)
                << "rule 7.3 fired with an unrelated message: " << issue.description
                << " -- the encryption warning used to squat on this id";
        }
    }
    EXPECT_TRUE(foundAltError)
        << "validatePdfUA accepted a document whose only figure has no "
           "alternative text";
    EXPECT_FALSE(result.success)
        << "validation reported success despite an undescribed figure";
}

// CONTROL: the same document, same code path, with /Alt present. If this also
// failed, the rule would be firing on the figure's existence rather than on the
// missing description.
TEST_F(FigureAltTextRuleTest, FigureWithAlternativeTextPassesTheGraphicsRule) {
    stageTaggedDocument("Bar chart of quarterly revenue, rising each quarter.");
    ASSERT_TRUE(fs::exists(outPath_));

    auto saved = pdfe::core::Document::open(context_, outPath_);
    ASSERT_NE(saved, nullptr);

    PdfUAConverter converter(context_);

    const StructureNode structRoot = converter.readStructureTree(saved.get());
    int figures = 0;
    for (const auto& c : structRoot.children)
        if (c && c->tag == StructureTag::Figure) ++figures;
    ASSERT_GT(figures, 0)
        << "the control document has no figure, so 'no 7.3 issue' is vacuous";

    EXPECT_TRUE(PdfUAConverter::figuresMissingAltText(structRoot).empty())
        << "a figure carrying /Alt was still flagged as needing it";

    const auto result = converter.validatePdfUA(saved.get());
    for (const auto& issue : result.issues) {
        EXPECT_NE(issue.rule, "7.3")
            << "rule 7.3 fired on a described figure: " << issue.description;
    }
}

// The renumbering half of #172: the encryption warning no longer occupies the
// Graphics clause. An unencrypted document raises neither, so the assertion is
// about the id the encryption check would use if it fired.
TEST_F(FigureAltTextRuleTest, GraphicsClauseIsNotSharedWithTheEncryptionCheck) {
    stageTaggedDocument("");
    auto saved = pdfe::core::Document::open(context_, outPath_);
    ASSERT_NE(saved, nullptr);

    PdfUAConverter converter(context_);
    const auto result = converter.validatePdfUA(saved.get());
    ASSERT_FALSE(result.issues.empty()) << "validation produced no issues";

    for (const auto& issue : result.issues) {
        if (issue.rule != "7.3") continue;
        EXPECT_EQ(issue.description.find("encrypted"), std::string::npos)
            << "the encryption warning is still filed under the Graphics "
               "clause: " << issue.description;
    }
}
