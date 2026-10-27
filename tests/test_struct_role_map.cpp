// GitHub #9 (gap-register PDF-12): /StructTreeRoot /RoleMap coverage.
//
// TWO DIFFERENT CLAIMS live in this file, and the phase brief was explicit that
// satisfying only the easy one is the "already done" shortcut to avoid:
//
//   * RoleMapSurvives*  — a PRESERVATION property. A /RoleMap that a document
//     already carries must still be there, entry for entry, after an edit.
//     Plausibly already true before this phase (PageArtifactGraft.cpp copies
//     {"RoleMap","ClassMap"} across a graft), so these tests MEASURE existing
//     behaviour and are the regression net.
//
//   * RoleMapWrittenForCustomTags — an EMISSION property, and a genuine
//     PRODUCT GAP when this file was written: before the accompanying change,
//     `RoleMap` appeared exactly once in the whole source tree, in that graft
//     preservation loop. StructureTreeBuilder had no notion of a custom
//     structure type at all, so it could not write a /RoleMap and no amount of
//     test-writing would make this pass. StructureNode::customTag and the
//     /RoleMap emission in StructureTreeBuilder::buildTree are that change.
//
// Why it matters: a /StructElem whose /S is a non-standard name with no
// /RoleMap entry mapping it to a standard type is meaningless to assistive
// technology, and is precisely the defect that makes a "tagged" PDF fail
// accessibility validation.
//
// Every oracle reads the SAVED BYTES back with an independent MuPDF handle
// (Document::fzDocument is friend-private), never the in-memory graph it just
// wrote.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "compliance/StructureTreeBuilder.h"
#include "compliance/StructureTypes.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"

#include <filesystem>
#include <map>
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

// ── Read-back oracle ───────────────────────────────────────────────────────

struct SavedRoleInfo {
    bool opened = false;
    bool hasStructTreeRoot = false;
    bool hasRoleMapKey = false;                  // /RoleMap present at all
    std::map<std::string, std::string> roleMap;  // custom name -> standard name
    std::vector<std::string> structElemS;        // every /S in document order
    int pageCount = 0;
};

void collectS(fz_context* c, pdf_obj* node, int depth,
              std::vector<std::string>& out) {
    if (!node || depth > 32) return;
    if (pdf_is_array(c, node)) {
        const int n = pdf_array_len(c, node);
        for (int i = 0; i < n; ++i)
            collectS(c, pdf_array_get(c, node, i), depth, out);
        return;
    }
    if (!pdf_is_dict(c, node)) return;   // bare MCID integer

    const char* s = pdf_to_name(c, pdf_dict_get(c, node, PDF_NAME(S)));
    out.push_back(s ? std::string(s) : std::string());

    pdf_obj* k = pdf_dict_get(c, node, PDF_NAME(K));
    if (k && !pdf_is_int(c, k)) collectS(c, k, depth + 1, out);
}

SavedRoleInfo readSaved(pdfe::core::MuPdfContext* mu, const fs::path& path) {
    SavedRoleInfo info;
    const std::string pathStr = path.string();

    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        fz_var(fzDoc);
        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
            if (pdfDoc) {
                info.opened = true;
                info.pageCount = pdf_count_pages(c, pdfDoc);

                pdf_obj* catalog =
                    pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
                pdf_obj* treeRoot =
                    catalog ? pdf_dict_get(c, catalog, PDF_NAME(StructTreeRoot))
                            : nullptr;
                info.hasStructTreeRoot = (treeRoot != nullptr);

                if (treeRoot) {
                    pdf_obj* rm = pdf_dict_gets(c, treeRoot, "RoleMap");
                    info.hasRoleMapKey = (rm != nullptr);
                    if (rm && pdf_is_dict(c, rm)) {
                        const int n = pdf_dict_len(c, rm);
                        for (int i = 0; i < n; ++i) {
                            const char* k =
                                pdf_to_name(c, pdf_dict_get_key(c, rm, i));
                            const char* v =
                                pdf_to_name(c, pdf_dict_get_val(c, rm, i));
                            if (k && *k) info.roleMap[k] = (v && *v) ? v : "";
                        }
                    }
                    collectS(c, pdf_dict_get(c, treeRoot, PDF_NAME(K)), 0,
                             info.structElemS);
                }
            }
        }
        fz_always(c) {
            if (fzDoc) fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
            // Leave whatever was collected; assertions cover `opened`.
        }
    });
    return info;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

// Edit the first span of >= 3 characters on page 0 through the real
// ContentStreamEditor path. Returns false if the page has no such span.
bool editFirstSpanOnPage0(std::shared_ptr<pdfe::core::MuPdfContext> ctx,
                          pdfe::core::Document* doc,
                          const std::u32string& replacement) {
    auto page = doc->page(0);
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
    pdfe::textedit::ContentStreamEditor editor(ctx.get());
    editor.applyEdits(page.get(), doc, spans, mods);
    return true;
}

} // namespace

// ===========================================================================
// Fixture
// ===========================================================================

class StructRoleMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag =
            std::string(ti->test_suite_name()) + "_" + ti->name();
        workPath_ = fs::temp_directory_path() / ("rolemap_work_" + tag + ".pdf");
        outPath_  = fs::temp_directory_path() / ("rolemap_out_"  + tag + ".pdf");
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

    // Copy a checked-in fixture into this test's private working path.
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
// EMISSION — the product gap (#9, first half)
// ===========================================================================

// A custom structure type gets a /RoleMap entry naming the standard type it
// rolls up to. Before the accompanying StructureTreeBuilder change this could
// not pass at all: nothing in the product ever wrote a /RoleMap.
TEST_F(StructRoleMapTest, RoleMapWrittenForCustomTags) {
    ASSERT_TRUE(useFixture("test_single.pdf")) << "test_single.pdf missing";
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    StructureNode root;
    root.tag = StructureTag::Document;

    auto pull = std::make_shared<StructureNode>();
    pull->tag = StructureTag::P;          // the STANDARD type it rolls up to
    pull->customTag = "PullQuote";        // the custom /S actually emitted
    pull->pageIndex = 0;
    pull->mcid = 0;
    root.children.push_back(pull);

    auto side = std::make_shared<StructureNode>();
    side->tag = StructureTag::Sect;
    side->customTag = "Sidebar";
    side->pageIndex = 0;
    side->mcid = 1;
    root.children.push_back(side);

    StructureTreeBuilder builder(context_);
    builder.buildTree(doc.get(), root);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    ASSERT_TRUE(info.hasStructTreeRoot);

    ASSERT_TRUE(info.hasRoleMapKey)
        << "a custom structure type was written with no /RoleMap at all — "
           "assistive technology cannot interpret /S /PullQuote";
    ASSERT_EQ(info.roleMap.size(), 2u)
        << "expected one /RoleMap entry per custom type";
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect");
}

// The custom name must actually reach /S. A builder that "supported" custom
// tags by writing the STANDARD name and a /RoleMap entry would satisfy the
// test above while losing the author's semantics entirely.
TEST_F(StructRoleMapTest, CustomTagIsEmittedAsTheStructElemSName) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    StructureNode root;
    root.tag = StructureTag::Document;
    auto pull = std::make_shared<StructureNode>();
    pull->tag = StructureTag::P;
    pull->customTag = "PullQuote";
    pull->pageIndex = 0;
    pull->mcid = 0;
    root.children.push_back(pull);

    StructureTreeBuilder builder(context_);
    builder.buildTree(doc.get(), root);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    EXPECT_TRUE(contains(info.structElemS, "PullQuote"))
        << "/S was not the custom name; the element lost its authored type";
    EXPECT_FALSE(contains(info.structElemS, "P"))
        << "the standard name was emitted instead of / as well as the custom "
           "one — /RoleMap exists precisely so /S can stay custom";
}

// CONTROL. Without this, "RoleMap is present" could be satisfied by writing an
// unconditional (and meaningless) /RoleMap on every document.
TEST_F(StructRoleMapTest, StandardTagsAloneWriteNoRoleMap) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    StructureNode root;
    root.tag = StructureTag::Document;
    for (int i = 0; i < 2; ++i) {
        auto p = std::make_shared<StructureNode>();
        p->tag = (i == 0) ? StructureTag::H1 : StructureTag::P;
        p->pageIndex = 0;
        p->mcid = i;
        root.children.push_back(p);
    }

    StructureTreeBuilder builder(context_);
    builder.buildTree(doc.get(), root);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    ASSERT_TRUE(info.hasStructTreeRoot);
    EXPECT_FALSE(info.hasRoleMapKey)
        << "an empty/gratuitous /RoleMap was written for standard types only";
    EXPECT_TRUE(contains(info.structElemS, "H1"));
    EXPECT_TRUE(contains(info.structElemS, "P"));
}

// A rebuild replaces /StructTreeRoot wholesale. The role map the document
// already declared must be carried across, or every surviving custom /S in the
// file becomes uninterpretable.
TEST_F(StructRoleMapTest, RebuildKeepsPreexistingRoleMapEntries) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"))
        << "test_tagged_rolemap.pdf missing (generate_tagged_rolemap_pdf.py)";
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    StructureNode root;
    root.tag = StructureTag::Document;
    auto p = std::make_shared<StructureNode>();
    p->tag = StructureTag::P;
    p->pageIndex = 0;
    p->mcid = 0;
    root.children.push_back(p);

    StructureTreeBuilder builder(context_);
    builder.buildTree(doc.get(), root);      // no custom tags in the NEW tree
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    ASSERT_TRUE(info.hasRoleMapKey)
        << "the rebuild dropped the document's pre-existing /RoleMap";
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect")
        << "a rebuild that re-derives the map only from types it currently "
           "SEES loses the declared-but-unused entry";
}

// An artifact is page furniture marked in the CONTENT STREAM; it is not a
// structure element and must never appear in the tree or the ParentTree.
TEST_F(StructRoleMapTest, ArtifactNodeIsNotEmittedAsAStructElem) {
    ASSERT_TRUE(useFixture("test_single.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);

    StructureNode root;
    root.tag = StructureTag::Document;

    auto para = std::make_shared<StructureNode>();
    para->tag = StructureTag::P;
    para->pageIndex = 0;
    para->mcid = 0;
    root.children.push_back(para);

    auto art = std::make_shared<StructureNode>();
    art->tag = StructureTag::Artifact;
    art->pageIndex = 0;
    art->mcid = 1;
    root.children.push_back(art);

    StructureTreeBuilder builder(context_);
    builder.buildTree(doc.get(), root);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    EXPECT_TRUE(contains(info.structElemS, "P"))
        << "the real content element was dropped along with the artifact";
    EXPECT_FALSE(contains(info.structElemS, "Artifact"))
        << "/S /Artifact is not a structure type; the artifact leaked into the "
           "structure tree and would be read out as document content";
}

// ===========================================================================
// PRESERVATION — RoleMapSurvives* (#9, second half)
// ===========================================================================

// Baseline: the fixture really does carry the two-entry role map the
// survival tests below assert on. Without this, a survival test that read an
// empty map from a broken fixture would pass its "unchanged" comparison
// vacuously.
TEST_F(StructRoleMapTest, FixtureCarriesTheExpectedRoleMap) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto info = readSaved(context_.get(), workPath_);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.pageCount, 2);
    ASSERT_TRUE(info.hasRoleMapKey);
    ASSERT_EQ(info.roleMap.size(), 2u);
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect");
    EXPECT_TRUE(contains(info.structElemS, "PullQuote"));
}

TEST_F(StructRoleMapTest, RoleMapSurvivesTextEdit) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(editFirstSpanOnPage0(context_, doc.get(),
                                     U"Edited alpha heading text"))
        << "fixture page 0 must expose an editable span";
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    ASSERT_TRUE(info.hasStructTreeRoot) << "text edit dropped /StructTreeRoot";
    ASSERT_TRUE(info.hasRoleMapKey)     << "text edit dropped /RoleMap";
    EXPECT_EQ(info.roleMap.size(), 2u);
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect");
    EXPECT_TRUE(contains(info.structElemS, "PullQuote"))
        << "the custom /S the role map explains no longer exists";
}

TEST_F(StructRoleMapTest, RoleMapSurvivesPageDelete) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 2);
    doc->deletePage(1);
    ASSERT_EQ(doc->pageCount(), 1);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.pageCount, 1);
    ASSERT_TRUE(info.hasRoleMapKey) << "page delete dropped /RoleMap";
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect");
    EXPECT_TRUE(contains(info.structElemS, "PullQuote"))
        << "deleting page 1 removed a page-0 custom element";
}

TEST_F(StructRoleMapTest, RoleMapSurvivesPageInsert) {
    ASSERT_TRUE(useFixture("test_tagged_rolemap.pdf"));
    auto doc = pdfe::core::Document::open(context_, workPath_);
    ASSERT_NE(doc, nullptr);
    doc->insertBlankPage(1, 612.0f, 792.0f);
    ASSERT_EQ(doc->pageCount(), 3);
    doc->saveAs(outPath_);
    doc.reset();

    auto info = readSaved(context_.get(), outPath_);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.pageCount, 3);
    ASSERT_TRUE(info.hasRoleMapKey) << "page insert dropped /RoleMap";
    EXPECT_EQ(info.roleMap["PullQuote"], "P");
    EXPECT_EQ(info.roleMap["Sidebar"], "Sect");
}
