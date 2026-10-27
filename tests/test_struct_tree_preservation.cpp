// R3-03 (Med-High): PDF/UA structure-tree preservation across edit families.
//
// Acceptance: a class test round-trips a tagged / PDF-UA fixture through each
// edit family (text edit, page delete, redaction), then re-runs the PDF/UA
// validator and asserts the StructTreeRoot and marked-content sequence survive
// with /StructParents mappings intact.
//
// Fixture: tests/fixtures/test_tagged_multipage.pdf (generator
// generate_tagged_multipage_pdf.py) — a 3-page tagged PDF where every page has
// a real marked-content sequence "/P <</MCID 0>> BDC ... EMC", a /StructParents
// key, a /StructElem with /Pg + /K(MCID), and a real /ParentTree number tree.
//
// The oracles are STRUCTURAL — they inspect the actual PDF objects (via MuPDF)
// and the actual page content streams, not EXPECT_NO_THROW:
//   (a) /StructTreeRoot present and non-empty (/K resolves)
//   (b) /MarkInfo/Marked still true
//   (c) each surviving page's /StructParents indexes a live /ParentTree /Nums
//       entry whose value references a /StructElem whose /Pg is that page, and
//       every /StructElem /Pg resolves to a live page
//   (d) the in-stream marked-content sequence (BDC/EMC + /MCID) survives on the
//       edited (and neighbour) pages
//   (e) the PDF/UA validator still reports the doc as tagged (no rule-7.1 error)

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "compliance/PdfUAConverter.h"
#include "compliance/StructureTreeBuilder.h"
#include "core/PageOperations.h"
#include "ui/CombineFilesService.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

using namespace pdfe::core;
namespace fs = std::filesystem;

namespace {

// ── Object-level structure snapshot ────────────────────────────────────────
struct StructInfo {
    bool hasStructTreeRoot = false;
    bool structTreeRootHasK = false;      // /K present and resolves
    bool markInfoMarked = false;
    int  numsPairCount = 0;               // /ParentTree /Nums key/val pairs
    int  pageCount = 0;
    std::vector<int> pageStructParents;   // per page: /StructParents or -1
    // Every page that carries a /StructParents key indexes a /Nums entry whose
    // value references a /StructElem whose /Pg is that same page.
    bool allStructParentsResolve = true;
    int  structElemWithPgCount = 0;       // # /StructElem nodes carrying a /Pg
    bool allPgResolveToLivePage = true;   // every /Pg resolves to a live page
    int  pagesWithStructParents = 0;
};

// Recursively walk the struct tree /K graph, collecting every /Pg seen and
// checking it against the live-page set.
static void walkStructK(fz_context* ctx, pdf_obj* node,
                        const std::vector<pdf_obj*>& pageObjs,
                        StructInfo& out, int depth) {
    if (!node || depth > 64) return;
    if (pdf_is_array(ctx, node)) {
        int n = pdf_array_len(ctx, node);
        for (int i = 0; i < n; ++i)
            walkStructK(ctx, pdf_array_get(ctx, node, i), pageObjs, out, depth + 1);
        return;
    }
    if (pdf_is_dict(ctx, node)) {
        pdf_obj* pg = pdf_dict_gets(ctx, node, "Pg");
        if (pg) {
            out.structElemWithPgCount++;
            bool resolved = false;
            for (pdf_obj* po : pageObjs) {
                if (po && pdf_objcmp(ctx, pg, po) == 0) { resolved = true; break; }
            }
            if (!resolved) out.allPgResolveToLivePage = false;
        }
        pdf_obj* k = pdf_dict_get(ctx, node, PDF_NAME(K));
        if (k) walkStructK(ctx, k, pageObjs, out, depth + 1);
    }
    // int leaf (MCID) → nothing to do
}

// Inspect a PDF on disk with an INDEPENDENT MuPDF handle (Document::fzDocument
// is friend-private), so the oracle reads the real saved bytes.
static StructInfo inspectStruct(MuPdfContext* ctx, const fs::path& path) {
    StructInfo info;
    ctx->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        pdf_document* pdfDoc = nullptr;
        fz_try(c) {
            fzDoc = fz_open_document(c, path.string().c_str());
            pdfDoc = pdf_document_from_fz_document(c, fzDoc);
        }
        fz_catch(c) { fzDoc = nullptr; pdfDoc = nullptr; }
        if (!pdfDoc) { if (fzDoc) fz_drop_document(c, fzDoc); return; }
        pdf_obj* catalog = pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
        if (!catalog) { fz_drop_document(c, fzDoc); return; }

        pdf_obj* str = pdf_dict_get(c, catalog, PDF_NAME(StructTreeRoot));
        info.hasStructTreeRoot = (str != nullptr);

        pdf_obj* markInfo = pdf_dict_gets(c, catalog, "MarkInfo");
        if (markInfo) {
            pdf_obj* marked = pdf_dict_gets(c, markInfo, "Marked");
            info.markInfoMarked = marked && pdf_to_bool(c, marked);
        }

        info.pageCount = pdf_count_pages(c, pdfDoc);
        std::vector<pdf_obj*> pageObjs;
        pageObjs.reserve(info.pageCount);
        for (int i = 0; i < info.pageCount; ++i)
            pageObjs.push_back(pdf_lookup_page_obj(c, pdfDoc, i));

        if (!str) { fz_drop_document(c, fzDoc); return; }

        pdf_obj* rootK = pdf_dict_get(c, str, PDF_NAME(K));
        info.structTreeRootHasK = (rootK != nullptr);

        // Walk the whole tree for /Pg validation.
        if (rootK) walkStructK(c, rootK, pageObjs, info, 0);

        // Build the ParentTree /Nums key→value map.
        pdf_obj* parentTree = pdf_dict_get(c, str, PDF_NAME(ParentTree));
        pdf_obj* nums = parentTree ? pdf_dict_get(c, parentTree, PDF_NAME(Nums)) : nullptr;
        std::vector<std::pair<int, pdf_obj*>> numsMap;
        if (nums && pdf_is_array(c, nums)) {
            int n = pdf_array_len(c, nums);
            info.numsPairCount = n / 2;
            for (int i = 0; i + 1 < n; i += 2) {
                int key = pdf_to_int(c, pdf_array_get(c, nums, i));
                pdf_obj* val = pdf_array_get(c, nums, i + 1);
                numsMap.emplace_back(key, val);
            }
        }

        // Per-page /StructParents → /Nums entry → /StructElem whose /Pg is page.
        for (int i = 0; i < info.pageCount; ++i) {
            pdf_obj* pageObj = pageObjs[i];
            int sp = -1;
            if (pageObj) {
                pdf_obj* spObj = pdf_dict_get(c, pageObj, PDF_NAME(StructParents));
                if (spObj && pdf_is_int(c, spObj)) sp = pdf_to_int(c, spObj);
            }
            info.pageStructParents.push_back(sp);
            if (sp < 0) continue;  // page carries no marked content → skip
            info.pagesWithStructParents++;

            // Find the Nums value for this key.
            pdf_obj* val = nullptr;
            for (auto& kv : numsMap) if (kv.first == sp) { val = kv.second; break; }
            if (!val) { info.allStructParentsResolve = false; continue; }

            // val is an array indexed by MCID (→ StructElem) OR a single elem.
            bool matched = false;
            auto checkElem = [&](pdf_obj* elem) {
                if (!elem || !pdf_is_dict(c, elem)) return;
                pdf_obj* pg = pdf_dict_gets(c, elem, "Pg");
                if (pg && pageObj && pdf_objcmp(c, pg, pageObj) == 0) matched = true;
            };
            if (pdf_is_array(c, val)) {
                int m = pdf_array_len(c, val);
                for (int j = 0; j < m; ++j) checkElem(pdf_array_get(c, val, j));
            } else {
                checkElem(val);
            }
            if (!matched) info.allStructParentsResolve = false;
        }
        fz_drop_document(c, fzDoc);
    });
    return info;
}

// ── Reading-order / alt-text oracle (GitHub #11, #12) ──────────────────────
//
// The ordered sequence of LEAF structure elements as a consumer would read
// them: depth-first through /K in array order, recording the page the element
// points at, its marked-content id, its structure type and its /Alt.
//
// #12's brief is explicit that "reading order unchanged" must be compared
// against the order the fixture was BUILT with — an INPUT — and never against
// a second read-back of the same edited document, which would pass for a
// permutation applied consistently to both sides.
struct ReadingEntry {
    int pageIndex = -1;
    int mcid = -1;
    std::string tag;
    std::string alt;

    bool operator==(const ReadingEntry& o) const {
        return pageIndex == o.pageIndex && mcid == o.mcid &&
               tag == o.tag && alt == o.alt;
    }
};

static std::ostream& operator<<(std::ostream& os, const ReadingEntry& e) {
    return os << "{pg=" << e.pageIndex << " mcid=" << e.mcid
              << " /S=" << (e.tag.empty() ? "<none>" : e.tag)
              << " alt=\"" << e.alt << "\"}";
}

static std::string describeOrder(const std::vector<ReadingEntry>& v) {
    std::ostringstream os;
    for (const auto& e : v) os << "\n  " << e;
    if (v.empty()) os << "\n  <empty>";
    return os.str();
}

// Does this /K value denote MARKED CONTENT directly (rather than further
// structure elements)? Per PDF 32000-1 14.7.2, a structure element's /K may be
// an integer MCID, a marked-content reference dict (/Type /MCR with /MCID), a
// child element dict, or an ARRAY of any of those.
//
// The first version of this oracle recognised only the bare integer, because
// that is the single shape the checked-in fixtures are authored with. That made
// it silently blind to a document where the same structure had been rewritten
// into the array form -- it reported an EMPTY reading order, which reads as
// "the tagging was destroyed" rather than "the oracle cannot see this shape".
// GitHub #13's split path produces exactly that rewrite (MuPDF's graft
// normalises /K), so the oracle is corrected here rather than the assertion
// weakened. Bare-int behaviour is unchanged, so the pre-existing tests that
// depend on it are unaffected.
static bool directMcid(fz_context* c, pdf_obj* k, int* mcid) {
    if (!k) return false;
    if (pdf_is_int(c, k)) { *mcid = pdf_to_int(c, k); return true; }
    if (pdf_is_dict(c, k)) {
        // An /MCR carries /MCID; a child STRUCT ELEMENT does not (it has /S),
        // so this does not mistake an interior node for a leaf.
        pdf_obj* m = pdf_dict_get(c, k, PDF_NAME(MCID));
        return (m && pdf_is_int(c, m)) ? (*mcid = pdf_to_int(c, m), true) : false;
    }
    if (pdf_is_array(c, k)) {
        const int n = pdf_array_len(c, k);
        for (int i = 0; i < n; ++i)
            if (directMcid(c, pdf_array_get(c, k, i), mcid)) return true;
    }
    return false;
}

static void walkReadingOrder(fz_context* c, pdf_obj* node, int depth,
                             const std::vector<pdf_obj*>& pageObjs,
                             std::vector<ReadingEntry>& out) {
    if (!node || depth > 64) return;
    if (pdf_is_array(c, node)) {
        const int n = pdf_array_len(c, node);
        for (int i = 0; i < n; ++i)
            walkReadingOrder(c, pdf_array_get(c, node, i), depth, pageObjs, out);
        return;
    }
    if (!pdf_is_dict(c, node)) return;   // bare MCID integer

    pdf_obj* k = pdf_dict_get(c, node, PDF_NAME(K));
    int leafMcid = -1;
    const bool leaf = directMcid(c, k, &leafMcid);

    if (leaf) {
        ReadingEntry e;
        e.mcid = leafMcid;
        const char* s = pdf_to_name(c, pdf_dict_get(c, node, PDF_NAME(S)));
        if (s) e.tag = s;
        pdf_obj* alt = pdf_dict_get(c, node, PDF_NAME(Alt));
        if (alt) {
            const char* a = pdf_to_text_string(c, alt);
            if (a) e.alt = a;
        }
        pdf_obj* pg = pdf_dict_gets(c, node, "Pg");
        if (pg) {
            for (size_t i = 0; i < pageObjs.size(); ++i) {
                if (pageObjs[i] && pdf_objcmp(c, pg, pageObjs[i]) == 0) {
                    e.pageIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        out.push_back(e);
        return;   // never follow /P (parent back-reference)
    }
    if (k) walkReadingOrder(c, k, depth + 1, pageObjs, out);
}

static std::vector<ReadingEntry> readReadingOrder(MuPdfContext* ctx,
                                                  const fs::path& path) {
    std::vector<ReadingEntry> out;
    const std::string pathStr = path.string();
    ctx->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        std::vector<pdf_obj*> pageObjs;
        fz_var(fzDoc);
        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
            if (pdfDoc) {
                const int pageCount = pdf_count_pages(c, pdfDoc);
                for (int i = 0; i < pageCount; ++i)
                    pageObjs.push_back(pdf_lookup_page_obj(c, pdfDoc, i));
                pdf_obj* catalog =
                    pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
                pdf_obj* str =
                    catalog ? pdf_dict_get(c, catalog, PDF_NAME(StructTreeRoot))
                            : nullptr;
                if (str)
                    walkReadingOrder(c, pdf_dict_get(c, str, PDF_NAME(K)), 0,
                                     pageObjs, out);
            }
        }
        fz_always(c) {
            if (fzDoc) fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
            // Leave whatever was collected; the caller asserts on the contents.
        }
    });
    return out;
}

// The AUTHORED reading order of tests/fixtures/test_tagged_rolemap.pdf, taken
// from generate_tagged_rolemap_pdf.py — the INPUT, not a read-back.
static const char* kFigureAlt =
    "Red barn at dusk, Vermont, 1974 archival print";

static std::vector<ReadingEntry> authoredRoleMapFixtureOrder() {
    return {
        { 0, 0, "H1",        "" },
        { 0, 1, "PullQuote", "" },
        { 0, 2, "Figure",    kFigureAlt },
        { 1, 0, "P",         "" },
    };
}

// ── Content-stream marked-content oracle ───────────────────────────────────
struct PageMcInfo {
    bool hasBDC = false;
    bool hasEMC = false;
    bool hasMCID = false;
    bool hasContent = false;
};

static std::string loadPageContentBytes(fz_context* c, pdf_document* pdfDoc, int pageIndex) {
    std::string bytes;
    pdf_obj* pageObj = pdf_lookup_page_obj(c, pdfDoc, pageIndex);
    if (!pageObj) return bytes;
    pdf_obj* contents = pdf_dict_get(c, pageObj, PDF_NAME(Contents));
    if (!contents) return bytes;

    auto appendStream = [&](pdf_obj* streamObj) {
        if (!streamObj || !pdf_is_stream(c, streamObj)) return;
        fz_buffer* buf = pdf_load_stream(c, streamObj);
        if (!buf) return;
        unsigned char* data = nullptr;
        size_t len = fz_buffer_storage(c, buf, &data);
        if (data && len) bytes.append(reinterpret_cast<char*>(data), len);
        bytes.push_back('\n');
        fz_drop_buffer(c, buf);
    };

    if (pdf_is_array(c, contents)) {
        int n = pdf_array_len(c, contents);
        for (int i = 0; i < n; ++i) appendStream(pdf_array_get(c, contents, i));
    } else {
        appendStream(contents);
    }
    return bytes;
}

static PageMcInfo inspectPageMarkedContent(MuPdfContext* ctx, const fs::path& path, int pageIndex) {
    PageMcInfo mc;
    ctx->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        pdf_document* pdfDoc = nullptr;
        fz_try(c) {
            fzDoc = fz_open_document(c, path.string().c_str());
            pdfDoc = pdf_document_from_fz_document(c, fzDoc);
        }
        fz_catch(c) { fzDoc = nullptr; pdfDoc = nullptr; }
        if (!pdfDoc) { if (fzDoc) fz_drop_document(c, fzDoc); return; }
        std::string bytes = loadPageContentBytes(c, pdfDoc, pageIndex);
        mc.hasContent = !bytes.empty();
        mc.hasBDC  = bytes.find("BDC") != std::string::npos;
        mc.hasEMC  = bytes.find("EMC") != std::string::npos;
        mc.hasMCID = bytes.find("/MCID") != std::string::npos;
        fz_drop_document(c, fzDoc);
    });
    return mc;
}

// Does the PDF/UA validator still see the doc as tagged (no "7.1" error)?
static bool validatorSeesTagged(std::shared_ptr<MuPdfContext> ctx, Document* doc) {
    pdfe::compliance::PdfUAConverter conv(ctx);
    auto result = conv.validatePdfUA(doc);
    for (const auto& issue : result.issues)
        if (issue.rule == "7.1") return false;   // "No structure tree found"
    return true;
}

// ── Test fixture ───────────────────────────────────────────────────────────
class StructTreePreservationTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        srcFixture_ = fs::path(TEST_FIXTURES_DIR) / "test_tagged_multipage.pdf";
        if (!fs::exists(srcFixture_))
            GTEST_SKIP() << "test_tagged_multipage.pdf not found";

        // Unique working + output paths per test to avoid cross-run collisions.
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string tag = std::string(ti->test_suite_name()) + "_" + ti->name();
        workPath_ = fs::temp_directory_path() / ("r303_work_" + tag + ".pdf");
        outPath_  = fs::temp_directory_path() / ("r303_out_" + tag + ".pdf");
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);
        fs::copy_file(srcFixture_, workPath_,
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "copy fixture failed: " << ec.message();
    }

    void TearDown() override {
        context_.reset();
        std::error_code ec;
        fs::remove(workPath_, ec);
        fs::remove(outPath_, ec);
    }

    // Swap the working copy to a different checked-in fixture. Used by the
    // GitHub #11/#12/#13 tests, which need the richer
    // test_tagged_rolemap.pdf (3 MCIDs on one page, a distinctive /Alt, a
    // /RoleMap and a real /Artifact). test_tagged_multipage.pdf is left alone
    // because test_accessibility_tree.cpp asserts exact counts against it.
    [[nodiscard]] bool useFixture(const std::string& name) {
        const fs::path src = fs::path(TEST_FIXTURES_DIR) / name;
        if (!fs::exists(src)) return false;
        std::error_code ec;
        fs::copy_file(src, workPath_, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        srcFixture_ = src;
        return true;
    }

    // Edit the first >=3-char span on `pageIndex` through the real
    // ContentStreamEditor path. Returns false when there is no such span.
    bool editFirstSpan(Document* doc, int pageIndex,
                       const std::u32string& replacement) {
        auto page = doc->page(pageIndex);
        if (!page) return false;
        auto blocks = page->extractText();
        pdfe::textedit::TextEditModel model(blocks);
        std::vector<pdfe::textedit::TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
        int target = -1;
        for (int i = 0; i < static_cast<int>(spans.size()); ++i)
            if (spans[i].text.size() >= 3) { target = i; break; }
        if (target < 0) return false;
        std::vector<std::pair<int, std::u32string>> mods;
        mods.push_back({target, replacement});
        pdfe::textedit::ContentStreamEditor editor(context_.get());
        editor.applyEdits(page.get(), doc, spans, mods);
        return true;
    }

    std::shared_ptr<MuPdfContext> context_;
    fs::path srcFixture_, workPath_, outPath_;
};

// ── Baseline: the fixture itself is a valid tagged multi-page doc ──────────
TEST_F(StructTreePreservationTest, FixtureIsWellFormedTagged) {
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);

    auto info = inspectStruct(context_.get(), workPath_);
    EXPECT_TRUE(info.hasStructTreeRoot);
    EXPECT_TRUE(info.structTreeRootHasK);
    EXPECT_TRUE(info.markInfoMarked);
    EXPECT_GT(info.numsPairCount, 0);
    EXPECT_EQ(info.pagesWithStructParents, 3);
    EXPECT_TRUE(info.allStructParentsResolve);
    EXPECT_TRUE(info.allPgResolveToLivePage);
    EXPECT_EQ(info.structElemWithPgCount, 3);

    for (int p = 0; p < 3; ++p) {
        auto mc = inspectPageMarkedContent(context_.get(), workPath_, p);
        EXPECT_TRUE(mc.hasBDC)  << "page " << p << " lost BDC";
        EXPECT_TRUE(mc.hasEMC)  << "page " << p << " lost EMC";
        EXPECT_TRUE(mc.hasMCID) << "page " << p << " lost /MCID";
    }
    EXPECT_TRUE(validatorSeesTagged(context_, doc.get()));
}

// ── Family 1: TEXT EDIT ────────────────────────────────────────────────────
TEST_F(StructTreePreservationTest, TextEditPreservesStructTree) {
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    // Edit page 0's text via the real ContentStreamEditor path.
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        auto blocks = page->extractText();
        pdfe::textedit::TextEditModel model(blocks);
        std::vector<pdfe::textedit::TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
        ASSERT_GT(spans.size(), 0u) << "fixture page 0 must have an editable span";

        int target = -1;
        for (int i = 0; i < static_cast<int>(spans.size()); ++i)
            if (spans[i].text.size() >= 3) { target = i; break; }
        ASSERT_GE(target, 0);

        std::vector<std::pair<int, std::u32string>> mods;
        mods.push_back({target, U"Edited alpha heading first page"});

        pdfe::textedit::ContentStreamEditor editor(context_.get());
        editor.applyEdits(page.get(), doc.get(), spans, mods);
    }

    doc->saveAs(outPath_);
    doc.reset();

    auto doc2 = Document::open(context_, outPath_);
    ASSERT_NE(doc2, nullptr);
    EXPECT_EQ(doc2->pageCount(), 3);

    auto info = inspectStruct(context_.get(), outPath_);
    EXPECT_TRUE(info.hasStructTreeRoot)   << "text edit dropped /StructTreeRoot";
    EXPECT_TRUE(info.structTreeRootHasK)  << "text edit emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)      << "text edit dropped /MarkInfo/Marked";
    EXPECT_GT(info.numsPairCount, 0)      << "text edit emptied /ParentTree";
    EXPECT_TRUE(info.allStructParentsResolve)
        << "text edit broke a page's /StructParents → /ParentTree mapping";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "text edit left a /StructElem /Pg dangling";
    EXPECT_EQ(info.structElemWithPgCount, 3);

    // The edited page must keep its marked-content sequence + MCID (criterion d).
    auto mc0 = inspectPageMarkedContent(context_.get(), outPath_, 0);
    EXPECT_TRUE(mc0.hasContent);
    EXPECT_TRUE(mc0.hasBDC)  << "text edit dropped BDC on the edited page";
    EXPECT_TRUE(mc0.hasEMC)  << "text edit dropped EMC on the edited page";
    EXPECT_TRUE(mc0.hasMCID) << "text edit dropped /MCID on the edited page";
    // Neighbour page untouched.
    auto mc1 = inspectPageMarkedContent(context_.get(), outPath_, 1);
    EXPECT_TRUE(mc1.hasBDC && mc1.hasEMC && mc1.hasMCID)
        << "text edit disturbed a neighbour page's marked content";

    EXPECT_TRUE(validatorSeesTagged(context_, doc2.get()))
        << "PDF/UA validator no longer sees the edited doc as tagged";
}

// ── Family 2: PAGE DELETE ──────────────────────────────────────────────────
TEST_F(StructTreePreservationTest, PageDeletePreservesStructTree) {
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);

    doc->deletePage(1);          // drop the middle page (original index 1)
    ASSERT_EQ(doc->pageCount(), 2);

    doc->saveAs(outPath_);
    doc.reset();

    auto doc2 = Document::open(context_, outPath_);
    ASSERT_NE(doc2, nullptr);
    EXPECT_EQ(doc2->pageCount(), 2);

    auto info = inspectStruct(context_.get(), outPath_);
    EXPECT_TRUE(info.hasStructTreeRoot)  << "page delete dropped /StructTreeRoot";
    EXPECT_TRUE(info.structTreeRootHasK) << "page delete emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)     << "page delete dropped /MarkInfo/Marked";
    EXPECT_GT(info.numsPairCount, 0)     << "page delete emptied /ParentTree";
    EXPECT_TRUE(info.allStructParentsResolve)
        << "page delete broke a surviving page's /StructParents → /ParentTree mapping";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "page delete left a /StructElem /Pg dangling to a removed page";
    // Two struct elements should survive, one per remaining page.
    EXPECT_EQ(info.structElemWithPgCount, 2)
        << "page delete did not prune/keep the right number of /StructElem /Pg";

    // Both surviving pages keep their marked-content sequences.
    for (int p = 0; p < 2; ++p) {
        auto mc = inspectPageMarkedContent(context_.get(), outPath_, p);
        EXPECT_TRUE(mc.hasBDC && mc.hasEMC && mc.hasMCID)
            << "page delete disturbed surviving page " << p << " marked content";
    }

    EXPECT_TRUE(validatorSeesTagged(context_, doc2.get()))
        << "PDF/UA validator no longer sees the doc as tagged after page delete";
}

// ── Family 3: REDACTION ────────────────────────────────────────────────────
TEST_F(StructTreePreservationTest, RedactionPreservesStructTree) {
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    {
        pdfe::redaction::RedactionManager mgr(context_);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        // Cover a broad band over the page-0 text (baseline ≈ user-space y=700).
        mgr.createRedactionMark(page.get(), QRectF(0, 600, 612, 160));
        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = false;   // we drive the save ourselves below
        bool applied = mgr.applyPageRedactions(page.get(), opts);
        EXPECT_TRUE(applied) << "redaction did not apply to page 0";
    }

    doc->saveAs(outPath_);
    doc.reset();

    auto doc2 = Document::open(context_, outPath_);
    ASSERT_NE(doc2, nullptr);
    EXPECT_EQ(doc2->pageCount(), 3);

    auto info = inspectStruct(context_.get(), outPath_);
    EXPECT_TRUE(info.hasStructTreeRoot)  << "redaction dropped /StructTreeRoot";
    EXPECT_TRUE(info.structTreeRootHasK) << "redaction emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)     << "redaction dropped /MarkInfo/Marked";
    EXPECT_GT(info.numsPairCount, 0)     << "redaction emptied /ParentTree";
    EXPECT_TRUE(info.allStructParentsResolve)
        << "redaction broke a page's /StructParents → /ParentTree mapping";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "redaction left a /StructElem /Pg dangling";
    EXPECT_EQ(info.structElemWithPgCount, 3);

    // The redacted page must keep its marked-content sequence + MCID.
    auto mc0 = inspectPageMarkedContent(context_.get(), outPath_, 0);
    EXPECT_TRUE(mc0.hasContent);
    EXPECT_TRUE(mc0.hasBDC)  << "redaction dropped BDC on the redacted page";
    EXPECT_TRUE(mc0.hasEMC)  << "redaction dropped EMC on the redacted page";
    EXPECT_TRUE(mc0.hasMCID) << "redaction dropped /MCID on the redacted page";

    EXPECT_TRUE(validatorSeesTagged(context_, doc2.get()))
        << "PDF/UA validator no longer sees the doc as tagged after redaction";
}

// ===========================================================================
// GitHub #12 — reading order, and #11 — alternative text
//
// Both run on tests/fixtures/test_tagged_rolemap.pdf, which has THREE marked
// content ids on page 0. The existing test_tagged_multipage.pdf has exactly
// one per page, so an intra-page permutation is invisible in it: any
// rearrangement of a single element is the identity.
// ===========================================================================

// Baseline / oracle validation. The fixture on disk must already match the
// order the generator authored. If this fails, every "unchanged after edit"
// assertion below is comparing against the wrong ground truth.
TEST_F(StructTreePreservationTest, ReadingOrderMatchesTheFixtureAsBuilt) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"))
        << "test_tagged_rolemap.pdf missing (generate_tagged_rolemap_pdf.py)";
    const auto authored = authoredRoleMapFixtureOrder();
    const auto actual = readReadingOrder(context_.get(), workPath_);
    ASSERT_EQ(actual.size(), authored.size())
        << "fixture reading order:" << describeOrder(actual);
    EXPECT_EQ(actual, authored) << "fixture:" << describeOrder(actual);
}

// #12. The comparison is against `authored` — the sequence the generator
// WROTE — not against a second read of the edited file.
TEST_F(StructTreePreservationTest, ReadingOrderSequenceUnchangedAfterEdit) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(editFirstSpan(doc.get(), 0, U"Edited alpha heading text"))
        << "fixture page 0 must expose an editable span";
    doc->saveAs(outPath_);
    doc.reset();

    const auto authored = authoredRoleMapFixtureOrder();
    const auto after = readReadingOrder(context_.get(), outPath_);

    ASSERT_EQ(after.size(), authored.size())
        << "the edit added or dropped structure elements. after:"
        << describeOrder(after);
    EXPECT_EQ(after, authored)
        << "the ordered /K MCID sequence changed across a text edit."
           "\nauthored (input):" << describeOrder(authored)
        << "\nafter edit:" << describeOrder(after);
}

// #11. Asserts the fixture's DISTINCTIVE alt text, never the AutoTagger
// placeholder — the phase brief's point: a test that accepts "Image" cannot
// tell "the author's description survived" from "a default was re-emitted".
TEST_F(StructTreePreservationTest, AltTextSurvivesTextEdit) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));

    // The alt text is genuinely in the fixture to begin with.
    {
        const auto before = readReadingOrder(context_.get(), workPath_);
        bool found = false;
        for (const auto& e : before)
            if (e.tag == "Figure" && e.alt == kFigureAlt) found = true;
        ASSERT_TRUE(found) << "fixture lost its /Figure /Alt:"
                           << describeOrder(before);
    }

    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(editFirstSpan(doc.get(), 0, U"Edited alpha heading text"));
    doc->saveAs(outPath_);
    doc.reset();

    const auto after = readReadingOrder(context_.get(), outPath_);
    int figures = 0;
    std::string figureAlt;
    for (const auto& e : after)
        if (e.tag == "Figure") { ++figures; figureAlt = e.alt; }

    ASSERT_EQ(figures, 1) << "the edit changed the figure count:"
                          << describeOrder(after);
    EXPECT_EQ(figureAlt, kFigureAlt)
        << "the figure's alternative text did not survive the text edit";
    EXPECT_NE(figureAlt, "Image")
        << "the surviving /Alt is the generic AutoTagger placeholder, which "
           "would mean the description was re-derived rather than preserved";
}

// ===========================================================================
// GitHub #13 — page-structure edits
//
// The prep survey PREDICTED a defect here: StructureTreeBuilder writes
// /StructParents = the page index at build time (StructureTreeBuilder.cpp,
// "Set StructParents on pages that have marked content"), and nothing in
// PageOperations.cpp mentions /StructParents or /StructTreeRoot, so inserting
// a page ahead of a tagged one looked like it would leave a stale key.
//
// Reproduced-or-not is recorded in the phase notes; the tests below are the
// experiment. They assert the invariant that actually matters — /StructParents
// resolves through /ParentTree to a /StructElem whose /Pg IS that page — which
// is an object-identity property, not an index one.
// ===========================================================================

TEST_F(StructTreePreservationTest, PageInsertPreservesStructTree) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 2);

    // Insert AHEAD of the tagged second page: original page 1 becomes page 2.
    doc->insertBlankPage(1, 612.0f, 792.0f);
    ASSERT_EQ(doc->pageCount(), 3);

    doc->saveAs(outPath_);
    doc.reset();

    auto info = inspectStruct(context_.get(), outPath_);
    EXPECT_EQ(info.pageCount, 3);
    EXPECT_TRUE(info.hasStructTreeRoot)  << "page insert dropped /StructTreeRoot";
    EXPECT_TRUE(info.structTreeRootHasK) << "page insert emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)     << "page insert dropped /MarkInfo/Marked";
    EXPECT_GT(info.numsPairCount, 0)     << "page insert emptied /ParentTree";
    EXPECT_TRUE(info.allStructParentsResolve)
        << "page insert left a page's /StructParents pointing at a /ParentTree "
           "entry that does not own that page — content is now associated with "
           "the wrong page";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "page insert left a /StructElem /Pg dangling";
    EXPECT_EQ(info.structElemWithPgCount, 4)
        << "all four original elements should survive an insert";
    // The blank page carries no marked content, so it must NOT have acquired a
    // /StructParents key.
    EXPECT_EQ(info.pagesWithStructParents, 2)
        << "the inserted blank page was given a /StructParents key it has no "
           "marked content for";

    // Reading order is relative to the CONTENT, so the elements keep their
    // order; only the page the last one lives on shifts 1 -> 2.
    auto expected = authoredRoleMapFixtureOrder();
    expected.back().pageIndex = 2;
    const auto after = readReadingOrder(context_.get(), outPath_);
    EXPECT_EQ(after, expected)
        << "structure elements no longer track their pages after an insert:"
        << describeOrder(after);
}

TEST_F(StructTreePreservationTest, PageMovePreservesStructTree) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 2);

    doc->rearrangePages({1, 0});     // swap the two pages
    ASSERT_EQ(doc->pageCount(), 2);

    doc->saveAs(outPath_);
    doc.reset();

    auto info = inspectStruct(context_.get(), outPath_);
    EXPECT_EQ(info.pageCount, 2);
    EXPECT_TRUE(info.hasStructTreeRoot)  << "page move dropped /StructTreeRoot";
    EXPECT_TRUE(info.structTreeRootHasK) << "page move emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)     << "page move dropped /MarkInfo/Marked";
    EXPECT_TRUE(info.allStructParentsResolve)
        << "page move broke a page's /StructParents -> /ParentTree mapping";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "page move left a /StructElem /Pg dangling";
    EXPECT_EQ(info.structElemWithPgCount, 4);

    // Each element must follow ITS page: the three page-0 elements are now on
    // page 1 and the single page-1 element is now on page 0. The /K sequence
    // itself is unchanged — moving pages does not rewrite the structure tree.
    auto expected = authoredRoleMapFixtureOrder();
    expected[0].pageIndex = 1;
    expected[1].pageIndex = 1;
    expected[2].pageIndex = 1;
    expected[3].pageIndex = 0;
    const auto after = readReadingOrder(context_.get(), outPath_);
    EXPECT_EQ(after, expected)
        << "structure elements did not follow their pages across a move:"
        << describeOrder(after);
}

// ===========================================================================
// GitHub #13 — MERGE and SPLIT
//
// #13 asks for three page-structure families. PageInsertPreservesStructTree
// (above) landed in pipeline p82 phase-15; the merge and split halves were
// never written, which is why #13 stayed open. These are they.
//
// THE INVARIANT, and why it is not "/StructParents gets renumbered".
// /StructParents is a KEY into the /ParentTree number tree, and what makes it
// correct is that the entry it selects references a /StructElem whose /Pg is
// that same page OBJECT. It is an object-identity property, not a page
// ordinal. A test asserting that a merge "renumbers /StructParents to the new
// page index" would be asserting something the spec does not require and the
// implementation has no reason to do; it would pass for a document whose tree
// describes the wrong pages and fail for a correct one.
//
// So both tests below assert the same thing the insert and move tests do:
//   - the tree still resolves (/StructTreeRoot, /K, /ParentTree non-empty)
//   - every page's /StructParents selects an entry owning THAT page
//     (inspectStruct::allStructParentsResolve)
//   - no /StructElem /Pg dangles (allPgResolveToLivePage)
// plus, per family, a CONTROL that the operation actually happened, so an
// operation that silently did nothing cannot satisfy the bounds trivially.
//
// MERGE is declared WontFix for /StructTreeRoot in
// tests/test_page_op_preservation_matrix.cpp (kCombineStructRationale): two
// /ParentTree number trees both keyed from 0 cannot be concatenated without
// renumbering every /StructParents and /StructParent on one side, so the
// destination's tree is kept and the incoming one dropped. That disposition is
// about COMPLETENESS -- the merged file is not expected to describe the
// appended pages. It is NOT a licence for the surviving tree to be wrong, and
// the test below draws exactly that line: whatever structure remains must
// describe the pages it claims to describe, and must not have acquired a
// mapping that associates content with the wrong page.
// ===========================================================================

TEST_F(StructTreePreservationTest, MergePreservesStructTree) {
    // Base = the rolemap fixture (2 pages, 4 elements, both pages tagged).
    // Appended = the multipage fixture (3 pages, 3 elements, all pages tagged).
    // Two DIFFERENT tagged documents, so both /ParentTree number trees are
    // populated and both key from 0 -- the collision the rationale describes.
    const fs::path baseSrc = fs::path(TEST_FIXTURES_DIR) / "test_tagged_rolemap.pdf";
    const fs::path appendSrc = fs::path(TEST_FIXTURES_DIR) / "test_tagged_multipage.pdf";
    ASSERT_TRUE(fs::exists(baseSrc)) << "missing " << baseSrc.string();
    ASSERT_TRUE(fs::exists(appendSrc)) << "missing " << appendSrc.string();

    // CONTROL on the INPUTS. If either input were untagged or malformed, every
    // assertion below would hold vacuously for the wrong reason. Establish that
    // both sides really are tagged and really do resolve BEFORE the merge, so a
    // failure afterwards is attributable to the merge.
    const auto baseIn = inspectStruct(context_.get(), baseSrc);
    ASSERT_TRUE(baseIn.hasStructTreeRoot) << "base input is not tagged";
    ASSERT_TRUE(baseIn.allStructParentsResolve) << "base input already broken";
    ASSERT_EQ(baseIn.pageCount, 2);
    ASSERT_EQ(baseIn.structElemWithPgCount, 4);
    const auto appendIn = inspectStruct(context_.get(), appendSrc);
    ASSERT_TRUE(appendIn.hasStructTreeRoot) << "appended input is not tagged";
    ASSERT_TRUE(appendIn.allStructParentsResolve) << "appended input already broken";
    ASSERT_EQ(appendIn.pageCount, 3);
    ASSERT_EQ(appendIn.pagesWithStructParents, 3)
        << "the appended fixture must carry /StructParents on every page, or "
           "this test cannot exercise the key-collision it exists for";

    pdfe::ui::CombineFilesService svc(context_);
    const auto result = svc.combine({baseSrc, appendSrc}, outPath_);
    ASSERT_TRUE(result.ok) << "combine failed: " << result.errorMessage;

    // CONTROL on the OPERATION. A merge that produced only the base document,
    // or refused and left the base untouched, would satisfy every structural
    // bound below while having merged nothing.
    ASSERT_TRUE(fs::exists(outPath_));
    ASSERT_EQ(result.outputPageCount, 5)
        << "combine did not append the second document's pages";

    const auto info = inspectStruct(context_.get(), outPath_);
    ASSERT_EQ(info.pageCount, 5)
        << "the saved merge does not contain all five pages -- the structural "
           "assertions below would be measuring a document that was never merged";

    // The destination's own tree survives the merge.
    EXPECT_TRUE(info.hasStructTreeRoot)
        << "merge dropped /StructTreeRoot -- the destination's own tagging was "
           "destroyed by appending pages to it";
    EXPECT_TRUE(info.structTreeRootHasK) << "merge emptied the struct tree";
    EXPECT_TRUE(info.markInfoMarked)     << "merge dropped /MarkInfo/Marked";
    EXPECT_GT(info.numsPairCount, 0)     << "merge emptied /ParentTree";

    // THE INVARIANT. Every page that claims a /StructParents key must select a
    // /ParentTree entry that owns THAT page. This is the assertion that catches
    // the mis-association the WontFix rationale warns about: if the appended
    // pages arrive still carrying their source /StructParents values (0, 1, 2)
    // while the destination's /ParentTree keys 0 and 1 belong to the BASE
    // document's pages, then page 2 resolves to a structure element describing
    // page 0 -- content attributed to the wrong page, which the rationale calls
    // "worse for assistive technology than an untagged document".
    EXPECT_TRUE(info.allStructParentsResolve)
        << "after the merge a page's /StructParents selects a /ParentTree entry "
           "that does not own that page: the structure tree now associates "
           "content with the WRONG page. Per kCombineStructRationale in "
           "tests/test_page_op_preservation_matrix.cpp the incoming tree is "
           "dropped by design -- but a dropped tree must also mean the pages it "
           "described no longer claim a key in the surviving one.";
    EXPECT_TRUE(info.allPgResolveToLivePage)
        << "merge left a /StructElem /Pg pointing at a page that is not in the "
           "merged document";

    // Per the WontFix, only the BASE document's pages are expected to remain
    // described. Assert that explicitly rather than leaving it implied: it is
    // what distinguishes "incoming tree dropped cleanly" from "incoming tree
    // half-merged".
    EXPECT_LE(info.pagesWithStructParents, 2)
        << "pages from the appended document still claim a /StructParents key "
           "even though their structure tree was dropped ("
        << info.pagesWithStructParents << " tagged pages, expected at most the "
           "base document's 2)";
    EXPECT_EQ(info.structElemWithPgCount, 4)
        << "the surviving tree should be exactly the base document's four "
           "elements -- more means the incoming tree was partially merged, "
           "fewer means the destination's own tagging was damaged";

    // The base document's reading order is untouched: merging appends pages, it
    // does not reorder or re-tag what was already there. Compared against the
    // AUTHORED order (an input), never against a second read-back.
    const auto after = readReadingOrder(context_.get(), outPath_);
    EXPECT_EQ(after, authoredRoleMapFixtureOrder())
        << "the base document's structure elements no longer describe their own "
           "pages after a merge:" << describeOrder(after);
}

TEST_F(StructTreePreservationTest, SplitPreservesStructTree) {
    // Split the rolemap fixture into its two pages. Page 0 carries three
    // elements (H1, PullQuote, Figure); page 1 carries one (P). That asymmetry
    // is the point: it makes "each part kept ITS OWN structure" distinguishable
    // from "each part kept the whole tree" and from "each part kept nothing".
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 2);

    // PDFE_STRUCT_SPLIT_DUMP_DIR: when set, the two parts are written there and
    // left behind, so the saved bytes can be inspected with an independent
    // tool. Unset (the normal case) they go to a temp path and are removed.
    fs::path partDir = outPath_.parent_path();
    if (const char* dump = std::getenv("PDFE_STRUCT_SPLIT_DUMP_DIR")) {
        std::error_code dec;
        fs::create_directories(dump, dec);
        if (!dec) partDir = fs::path(dump);
    }
    const fs::path part0 = partDir / (outPath_.stem().string() + "_part0.pdf");
    const fs::path part1 = partDir / (outPath_.stem().string() + "_part1.pdf");
    std::error_code rmec;
    fs::remove(part0, rmec);
    fs::remove(part1, rmec);

    pdfe::core::extractPagesToFile(context_, doc.get(), {0}, part0);
    pdfe::core::extractPagesToFile(context_, doc.get(), {1}, part1);
    doc.reset();

    // CONTROL on the OPERATION. Both parts must exist and hold exactly one
    // page. A split that wrote the whole document into each part, or wrote an
    // empty document, would otherwise satisfy the structural bounds below.
    ASSERT_TRUE(fs::exists(part0)) << "split produced no first part";
    ASSERT_TRUE(fs::exists(part1)) << "split produced no second part";

    struct PartExpectation {
        const fs::path& path;
        const char* label;
        int elems;              // /StructElem nodes carrying a /Pg
        std::vector<ReadingEntry> order;  // authored order, rebased on this part
    };

    // The authored order, sliced per part and rebased to page 0 -- derived from
    // the fixture's INPUT order, not from reading either part back.
    auto authored = authoredRoleMapFixtureOrder();
    std::vector<ReadingEntry> order0, order1;
    for (const auto& e : authored) {
        ReadingEntry r = e;
        if (e.pageIndex == 0) { r.pageIndex = 0; order0.push_back(r); }
        else if (e.pageIndex == 1) { r.pageIndex = 0; order1.push_back(r); }
    }
    ASSERT_EQ(order0.size(), 3u) << "fixture assumption broken: page 0 should "
                                   "author three structure elements";
    ASSERT_EQ(order1.size(), 1u) << "fixture assumption broken: page 1 should "
                                   "author one structure element";

    const PartExpectation parts[] = {
        {part0, "part 0 (source page 0)", 3, order0},
        {part1, "part 1 (source page 1)", 1, order1},
    };

    for (const auto& p : parts) {
        SCOPED_TRACE(p.label);
        const auto info = inspectStruct(context_.get(), p.path);

        ASSERT_EQ(info.pageCount, 1)
            << "this part does not hold exactly one page, so everything below "
               "would be measuring the wrong document";

        EXPECT_TRUE(info.hasStructTreeRoot)
            << "split dropped /StructTreeRoot -- the part is untagged";
        EXPECT_TRUE(info.structTreeRootHasK) << "split emptied the struct tree";
        EXPECT_TRUE(info.markInfoMarked)     << "split dropped /MarkInfo/Marked";
        EXPECT_GT(info.numsPairCount, 0)     << "split emptied /ParentTree";

        // THE INVARIANT. The part's single page keeps a /StructParents key that
        // resolves, through this part's OWN /ParentTree, to a /StructElem whose
        // /Pg is this part's page. A split that copied the source /ParentTree
        // wholesale would leave keys pointing at elements describing pages that
        // are not in the part.
        EXPECT_TRUE(info.allStructParentsResolve)
            << "this part's page has a /StructParents key that selects a "
               "/ParentTree entry not owning it -- the part's content is "
               "associated with a page it does not contain";
        EXPECT_TRUE(info.allPgResolveToLivePage)
            << "split left a /StructElem /Pg pointing at a source page that is "
               "not in this part -- a dangling structure reference";
        EXPECT_EQ(info.pagesWithStructParents, 1)
            << "the part's only page lost its /StructParents key";

        // NON-VACUITY / the asymmetry. Each part must carry ITS OWN elements
        // and only those. Equal counts across the two parts would mean the
        // whole tree was copied into both.
        EXPECT_EQ(info.structElemWithPgCount, p.elems)
            << "expected this part to carry exactly its own " << p.elems
            << " structure element(s); carrying all four would mean the split "
               "copied the entire tree into every part, and carrying none "
               "would mean it dropped the tagging";

        const auto after = readReadingOrder(context_.get(), p.path);
        EXPECT_EQ(after, p.order)
            << "this part's structure elements are not the ones authored for "
               "its source page, in the authored order:" << describeOrder(after);
    }

    // The two parts must not be identical: distinct element counts prove the
    // split discriminated between the source pages rather than duplicating.
    const auto i0 = inspectStruct(context_.get(), part0);
    const auto i1 = inspectStruct(context_.get(), part1);
    EXPECT_NE(i0.structElemWithPgCount, i1.structElemWithPgCount)
        << "both parts carry the same number of structure elements, so the "
           "split did not separate the source pages' structure";

    if (!std::getenv("PDFE_STRUCT_SPLIT_DUMP_DIR")) {
        fs::remove(part0, rmec);
        fs::remove(part1, rmec);
    }
}

} // namespace
