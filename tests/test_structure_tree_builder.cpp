#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/StructureTreeBuilder.h"
#include "compliance/StructureTypes.h"
#include <filesystem>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// MuPDF is visible through core/MuPdfContext.h (fitz.h + pdf.h, each with its
// own extern "C" guard). The A1-49 read-back oracle below opens the SAVED bytes
// with an INDEPENDENT handle because Document::fzDocument() is friend-private.

using namespace pdfe::compliance;

namespace {

// ---------------------------------------------------------------------------
// A1-49: structure-tree read-back oracle.
//
// BuildMultiLevelTree's only assertion used to be
// `EXPECT_TRUE(builder.hasStructureTree(doc2.get()))`, which merely checks that
// /Root/StructTreeRoot is a non-null object. A builder that dropped every
// heading, flattened the nesting, or silently discarded /Alt would still leave
// a StructTreeRoot behind and pass. These helpers flatten the saved tree into
// an ordered (depth, tag, mcid, page, alt) list so the AUTHORED hierarchy can
// be compared element by element.
// ---------------------------------------------------------------------------

struct StructElemView {
    int depth = 0;             ///< 0 = direct child of StructTreeRoot
    std::string tag;           ///< /S
    int mcid = -1;             ///< /K when /K is an integer leaf, else -1
    bool hasPg = false;        ///< /Pg present
    int pgIndex = -1;          ///< index of the page /Pg resolves to, else -1
    std::string altText;       ///< /Alt (empty when absent)

    bool operator==(const StructElemView& o) const {
        return depth == o.depth && tag == o.tag && mcid == o.mcid &&
               hasPg == o.hasPg && pgIndex == o.pgIndex && altText == o.altText;
    }
};

// Readable gtest output for EXPECT_EQ on StructElemView.
std::ostream& operator<<(std::ostream& os, const StructElemView& e) {
    return os << "{depth=" << e.depth
              << " /S=" << (e.tag.empty() ? "<none>" : e.tag)
              << " mcid=" << e.mcid
              << " pg=" << (e.hasPg ? std::to_string(e.pgIndex) : std::string("<none>"))
              << " alt=\"" << e.altText << "\"}";
}

std::string describe(const std::vector<StructElemView>& elems) {
    std::ostringstream os;
    for (const auto& e : elems) {
        os << "\n  depth=" << e.depth << " /S=" << (e.tag.empty() ? "<none>" : e.tag)
           << " mcid=" << e.mcid
           << " pg=" << (e.hasPg ? std::to_string(e.pgIndex) : std::string("<none>"))
           << " alt=\"" << e.altText << "\"";
    }
    if (elems.empty()) os << "\n  <empty>";
    return os.str();
}

void walkStructTree(fz_context* c, pdf_obj* node, int depth,
                    const std::vector<pdf_obj*>& pageObjs,
                    std::vector<StructElemView>& out) {
    if (!node || depth > 32) return;

    if (pdf_is_array(c, node)) {
        const int n = pdf_array_len(c, node);
        for (int i = 0; i < n; ++i)
            walkStructTree(c, pdf_array_get(c, node, i), depth, pageObjs, out);
        return;
    }
    if (!pdf_is_dict(c, node))
        return;  // bare MCID integer — recorded on the owning element's /K

    StructElemView view;
    view.depth = depth;

    const char* s = pdf_to_name(c, pdf_dict_get(c, node, PDF_NAME(S)));
    if (s) view.tag = s;

    pdf_obj* alt = pdf_dict_get(c, node, PDF_NAME(Alt));
    if (alt) {
        const char* a = pdf_to_text_string(c, alt);
        if (a) view.altText = a;
    }

    pdf_obj* pg = pdf_dict_gets(c, node, "Pg");
    if (pg) {
        view.hasPg = true;
        for (size_t i = 0; i < pageObjs.size(); ++i) {
            if (pageObjs[i] && pdf_objcmp(c, pg, pageObjs[i]) == 0) {
                view.pgIndex = static_cast<int>(i);
                break;
            }
        }
    }

    pdf_obj* k = pdf_dict_get(c, node, PDF_NAME(K));
    const bool leaf = (k != nullptr) && pdf_is_int(c, k);
    if (leaf) view.mcid = pdf_to_int(c, k);

    out.push_back(view);

    // Never follow /P (parent back-reference) — only /K descends.
    if (k && !leaf)
        walkStructTree(c, k, depth + 1, pageObjs, out);
}

struct SavedStructTree {
    bool opened = false;
    bool hasStructTreeRoot = false;
    bool markInfoMarked = false;
    int numsPairCount = -1;              ///< /ParentTree /Nums length / 2
    std::vector<int> numsKeys;           ///< the MCIDs the ParentTree indexes
    std::vector<int> pageStructParents;  ///< per page: /StructParents or -1
    std::vector<StructElemView> elements;///< depth-first, document order
};

// Read the tree back out of @p path with an independent MuPDF handle.
SavedStructTree readSavedStructTree(pdfe::core::MuPdfContext* mu,
                                    const std::filesystem::path& path) {
    SavedStructTree info;
    const std::string pathStr = path.string();

    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        // Container built BEFORE the fz_try frame (MuPDF setjmp/longjmp rule).
        std::vector<pdf_obj*> pageObjs;
        // Assigned inside fz_try, read in fz_always -> must be fz_var'd.
        fz_var(fzDoc);
        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
            if (pdfDoc) {
                info.opened = true;

                const int pageCount = pdf_count_pages(c, pdfDoc);
                for (int i = 0; i < pageCount; ++i)
                    pageObjs.push_back(pdf_lookup_page_obj(c, pdfDoc, i));

                for (pdf_obj* po : pageObjs) {
                    pdf_obj* sp = po ? pdf_dict_get(c, po, PDF_NAME(StructParents))
                                     : nullptr;
                    const bool isInt = (sp != nullptr) && pdf_is_int(c, sp);
                    info.pageStructParents.push_back(isInt ? pdf_to_int(c, sp) : -1);
                }

                pdf_obj* catalog = pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
                pdf_obj* treeRoot = pdf_dict_get(c, catalog, PDF_NAME(StructTreeRoot));
                info.hasStructTreeRoot = (treeRoot != nullptr);

                pdf_obj* markInfo = pdf_dict_gets(c, catalog, "MarkInfo");
                if (markInfo) {
                    pdf_obj* marked = pdf_dict_gets(c, markInfo, "Marked");
                    info.markInfoMarked = (marked != nullptr) && pdf_to_bool(c, marked);
                }

                if (treeRoot) {
                    pdf_obj* parentTree = pdf_dict_get(c, treeRoot, PDF_NAME(ParentTree));
                    pdf_obj* nums = parentTree
                                        ? pdf_dict_get(c, parentTree, PDF_NAME(Nums))
                                        : nullptr;
                    if (nums) {
                        const int len = pdf_array_len(c, nums);
                        info.numsPairCount = len / 2;
                        for (int i = 0; i + 1 < len; i += 2)
                            info.numsKeys.push_back(
                                pdf_to_int(c, pdf_array_get(c, nums, i)));
                    }

                    walkStructTree(c, pdf_dict_get(c, treeRoot, PDF_NAME(K)), 0,
                                   pageObjs, info.elements);
                }
            }
        }
        fz_always(c) {
            if (fzDoc) fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
            // Leave whatever was collected; the test asserts on the flags.
        }
    });

    return info;
}

} // namespace

class StructureTreeBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(StructureTreeBuilderTest, ConstructDestroy) {
    StructureTreeBuilder builder(context_);
    SUCCEED();
}

TEST_F(StructureTreeBuilderTest, HasStructureTree_FreshDoc_False) {
    StructureTreeBuilder builder(context_);
    EXPECT_FALSE(builder.hasStructureTree(document_.get()));
}

TEST_F(StructureTreeBuilderTest, BuildMinimalTree) {
    StructureTreeBuilder builder(context_);

    StructureNode root;
    root.tag = StructureTag::Document;
    auto para = std::make_shared<StructureNode>();
    para->tag = StructureTag::P;
    para->pageIndex = 0;
    para->mcid = 0;
    root.children.push_back(para);

    builder.buildTree(document_.get(), root);

    // Save and reopen to verify
    auto tempPath = std::filesystem::temp_directory_path() / "test_struct_build.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    EXPECT_TRUE(builder.hasStructureTree(doc2.get()));

    doc2.reset();
    std::filesystem::remove(tempPath);
}

// A1-49: the built tree is read back and compared to the AUTHORED hierarchy.
//
// AUTHORED GROUND TRUTH — the StructureNode graph constructed below:
//
//   StructTreeRoot
//     └─ Sect                       (depth 0, container, no /Pg, no MCID)
//          ├─ H1   mcid 0  page 0   (depth 1)
//          └─ H2   mcid 1  page 0   (depth 1)
//     ├─ P      mcid 2  page 0      (depth 0)
//     └─ Figure mcid 3  page 0  /Alt = "Test image"   (depth 0)
//
// The old oracle was a single `hasStructureTree(doc2)` — true whenever
// /Root/StructTreeRoot exists at all. Dropping the H2, flattening Sect's
// children up to the root, reordering the siblings, losing every /Pg, or
// discarding /Alt all left that assertion green.
//
// Mutations killed:
//   * dropping a heading level — the element list loses its {depth 1, "H2"}
//     entry and the size/equality check fails;
//   * flattening nesting (pushing children onto the root's /K instead of the
//     parent's) — every depth becomes 0;
//   * reordering /K — the ordered comparison fails;
//   * not writing /Alt (or writing it on the wrong element) — altText mismatch;
//   * not writing /Pg or pointing it at the wrong page — pgIndex mismatch;
//   * not writing the leaf MCID (or writing the kids array over it) — mcid -1;
//   * skipping the ParentTree/StructParents wiring — nums/StructParents checks.
TEST_F(StructureTreeBuilderTest, BuildMultiLevelTree) {
    StructureTreeBuilder builder(context_);

    StructureNode root;
    root.tag = StructureTag::Document;

    // Nested section holding two heading levels.
    auto section = std::make_shared<StructureNode>();
    section->tag = StructureTag::Sect;

    auto h1 = std::make_shared<StructureNode>();
    h1->tag = StructureTag::H1;
    h1->pageIndex = 0;
    h1->mcid = 0;
    section->children.push_back(h1);

    auto h2 = std::make_shared<StructureNode>();
    h2->tag = StructureTag::H2;
    h2->pageIndex = 0;
    h2->mcid = 1;
    section->children.push_back(h2);

    root.children.push_back(section);

    auto para = std::make_shared<StructureNode>();
    para->tag = StructureTag::P;
    para->pageIndex = 0;
    para->mcid = 2;
    root.children.push_back(para);

    auto figure = std::make_shared<StructureNode>();
    figure->tag = StructureTag::Figure;
    figure->pageIndex = 0;
    figure->mcid = 3;
    figure->altText = "Test image";
    root.children.push_back(figure);

    builder.buildTree(document_.get(), root);

    auto tempPath = std::filesystem::temp_directory_path() / "test_struct_multi.pdf";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    document_->saveAs(tempPath);
    ASSERT_TRUE(std::filesystem::exists(tempPath));

    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);
    EXPECT_TRUE(builder.hasStructureTree(doc2.get()));
    doc2.reset();

    // ---- read the SAVED bytes back with an independent MuPDF handle --------
    auto probeContext = std::make_shared<pdfe::core::MuPdfContext>();
    const SavedStructTree saved = readSavedStructTree(probeContext.get(), tempPath);

    ASSERT_TRUE(saved.opened) << "Saved tagged PDF could not be re-opened";
    ASSERT_TRUE(saved.hasStructTreeRoot) << "/Root/StructTreeRoot missing";
    EXPECT_TRUE(saved.markInfoMarked) << "/MarkInfo /Marked must be true";

    // Element-by-element comparison against the authored hierarchy.
    const std::vector<StructElemView> expected = {
        // depth, tag,      mcid, hasPg, pgIndex, altText
        {0, "Sect",   -1, false, -1, ""},
        {1, "H1",      0, true,   0, ""},
        {1, "H2",      1, true,   0, ""},
        {0, "P",       2, true,   0, ""},
        {0, "Figure",  3, true,   0, "Test image"},
    };

    ASSERT_EQ(saved.elements.size(), expected.size())
        << "Saved structure tree has " << saved.elements.size()
        << " StructElem node(s), authored " << expected.size()
        << "." << describe(saved.elements);

    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(saved.elements[i], expected[i])
            << "StructElem #" << i << " does not match the authored node.\n"
               "  expected: depth=" << expected[i].depth
            << " /S=" << expected[i].tag
            << " mcid=" << expected[i].mcid
            << " pg=" << expected[i].pgIndex
            << " alt=\"" << expected[i].altText << "\"\n"
               "  actual  : depth=" << saved.elements[i].depth
            << " /S=" << saved.elements[i].tag
            << " mcid=" << saved.elements[i].mcid
            << " pg=" << saved.elements[i].pgIndex
            << " alt=\"" << saved.elements[i].altText << "\""
            << "\nfull tree:" << describe(saved.elements);
    }

    // The alt text specifically — called out by the audit note.
    ASSERT_EQ(saved.elements.size(), expected.size());
    EXPECT_EQ(saved.elements[4].altText, "Test image")
        << "The Figure's authored /Alt must survive the build+save round trip; "
           "without it the figure is unreadable to assistive technology.";

    // ParentTree: one /Nums pair per MCID leaf, keyed by the authored MCIDs.
    EXPECT_EQ(saved.numsPairCount, 4)
        << "Four MCID leaves were authored, so /ParentTree /Nums must hold "
           "four key/value pairs.";
    const std::vector<int> expectedNumsKeys = {0, 1, 2, 3};
    EXPECT_EQ(saved.numsKeys, expectedNumsKeys)
        << "/ParentTree /Nums must index exactly the authored MCIDs 0..3.";

    // Page 0 carries marked content, so it must be wired into the ParentTree.
    ASSERT_FALSE(saved.pageStructParents.empty());
    EXPECT_EQ(saved.pageStructParents[0], 0)
        << "Page 0 holds every marked-content leaf, so it must carry "
           "/StructParents 0.";

    std::filesystem::remove(tempPath, ec);
}

TEST_F(StructureTreeBuilderTest, RemoveStructureTree) {
    StructureTreeBuilder builder(context_);

    // Build a tree first
    StructureNode root;
    root.tag = StructureTag::Document;
    auto para = std::make_shared<StructureNode>();
    para->tag = StructureTag::P;
    para->pageIndex = 0;
    para->mcid = 0;
    root.children.push_back(para);

    builder.buildTree(document_.get(), root);
    EXPECT_TRUE(builder.hasStructureTree(document_.get()));

    builder.removeStructureTree(document_.get());
    EXPECT_FALSE(builder.hasStructureTree(document_.get()));
}

TEST_F(StructureTreeBuilderTest, SetDocumentLanguage) {
    StructureTreeBuilder builder(context_);
    // Should not throw
    builder.setDocumentLanguage(document_.get(), "en-US");
    SUCCEED();
}

TEST_F(StructureTreeBuilderTest, SetDocumentLanguage_Roundtrip) {
    StructureTreeBuilder builder(context_);
    builder.setDocumentLanguage(document_.get(), "en-US");

    auto tempPath = std::filesystem::temp_directory_path() / "test_struct_lang.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);

    // Just verify the file is still valid
    EXPECT_GT(doc2->pageCount(), 0);

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(StructureTreeBuilderTest, NullDoc_NoThrow) {
    StructureTreeBuilder builder(context_);
    EXPECT_FALSE(builder.hasStructureTree(nullptr));

    StructureNode root;
    root.tag = StructureTag::Document;
    builder.buildTree(nullptr, root); // should not throw
    builder.removeStructureTree(nullptr); // should not throw
    builder.setDocumentLanguage(nullptr, "en-US"); // should not throw
}

TEST_F(StructureTreeBuilderTest, BuildTree_EmptyRoot) {
    StructureTreeBuilder builder(context_);

    StructureNode root;
    root.tag = StructureTag::Document;
    // No children

    builder.buildTree(document_.get(), root);

    auto tempPath = std::filesystem::temp_directory_path() / "test_struct_empty.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);
    ASSERT_NE(doc2, nullptr);
    EXPECT_TRUE(builder.hasStructureTree(doc2.get()));

    doc2.reset();
    std::filesystem::remove(tempPath);
}
