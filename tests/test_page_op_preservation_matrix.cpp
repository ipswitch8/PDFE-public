// -*- coding: utf-8 -*-
// F6 — page-operation × document-artifact PRESERVATION MATRIX.
//
// Closes PDF-88..PDF-97 and PDF-99, PDF-101..PDF-105 (see
// docs/audit/meta-audit-2026-07/followup/phase-6-dispositions.md).
//
// 8 operations {extract, combine, split, reorder, rotate, insert, delete,
// optimize} x 8 artifacts {outline, /PageLabels, /AcroForm field VALUES,
// /OCProperties, /StructTreeRoot, /Names /EmbeddedFiles, /Names /Dests,
// annotations} = 64 cells. Every cell is in kMatrix[] below, each one either
// ASSERTED or explicitly WONT_FIX with an in-source rationale. A wont-fix cell
// is still executed: it asserts the behaviour we accept, so it is a lock-in
// NEGATIVE test rather than a hole.
//
// Discrimination: `ControlOracleDetectsTotalLoss` runs the SAME oracle over an
// output produced the way `extractPagesToFile` used to work (a bare
// pdf_create_document + pdf_graft_mapped_page) and asserts all eight artifacts
// read as ABSENT. Every "still present" assertion below is therefore known to
// be capable of failing.
//
// That control proves an artifact CAN go missing. It says nothing about an
// operation that moves pages without losing anything, and the sentinel scheme
// described here only ever applied to the operations that DROP a page --
// extract, split and delete name both the sentinel that must survive and the
// one that must not. Reorder and rotate drop nothing, so for them the sentinel
// argument was never available and the cells rested on presence alone. That
// was #224: sixteen cells whose assertions held equally after the correct
// permutation, the wrong one, and no permutation at all.
//
// checkReorder and checkRotate now anchor each cell on the INPUT -- where a
// bookmark, label, dest, struct role or annotation sat BEFORE the edit, and
// where the permutation says it must sit after -- and carry a precondition
// that the operation happened at all, so a no-op reddens all sixteen. Insert
// and optimize keep checkFullDocumentOp, where presence genuinely is the
// question because nothing is supposed to move.

#include <gtest/gtest.h>

#include "support/PreservationOracle.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageOperations.h"
#include "optimize/OptimizeEngine.h"
#include "optimize/OptimizeTypes.h"
#include "ui/CombineFilesService.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::testsupport;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

// ---------------------------------------------------------------------------
// Matrix definition
// ---------------------------------------------------------------------------

enum class Op { Extract, Combine, Split, Reorder, Rotate, Insert, Delete, Optimize };

const char* opName(Op o) {
    switch (o) {
        case Op::Extract:  return "extract";
        case Op::Combine:  return "combine";
        case Op::Split:    return "split";
        case Op::Reorder:  return "reorder";
        case Op::Rotate:   return "rotate";
        case Op::Insert:   return "insert";
        case Op::Delete:   return "delete";
        case Op::Optimize: return "optimize";
    }
    return "?";
}

enum class Disposition { Asserted, WontFix };

struct Cell {
    Op op;
    Artifact art;
    Disposition disp;
    const char* rationale;   // required, and non-empty, for every WONT_FIX cell
};

// The single wont-fix rationale used below, spelled out once.
//
// COMBINE x /StructTreeRoot: two tagged documents cannot be concatenated by
// appending one /StructTreeRoot to the other. Both carry a /ParentTree number
// tree keyed by each page's /StructParents integer, and both number from 0, so
// a merge requires renumbering every /StructParents on one side and rewriting
// the corresponding /ParentTree keys AND the /StructParent of every annotation
// on those pages. Getting that half-right yields a structure tree that claims
// to describe content it does not describe, which is worse for assistive
// technology than an untagged document. The destination's own tree is therefore
// kept intact and the incoming one is dropped; see the dispositions document
// for the deferral. The cell below LOCKS THAT IN: it asserts the destination
// tree still describes exactly its own pages, so a future half-merge that
// corrupts it fails here.
constexpr const char* kCombineStructRationale =
    "two /ParentTree number trees both keyed from 0 cannot be concatenated "
    "without renumbering every /StructParents and /StructParent on one side; "
    "deferred rather than half-done";

const Cell kMatrix[] = {
    // --- extract (pages {0,2} of a 3-page source) --------------------------
    {Op::Extract,  Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Extract,  Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Extract,  Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Extract,  Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Extract,  Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Extract,  Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Extract,  Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Extract,  Artifact::Annotations,    Disposition::Asserted, ""},
    // --- combine (3-page A + 2-page B) -------------------------------------
    {Op::Combine,  Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Combine,  Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Combine,  Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Combine,  Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Combine,  Artifact::StructTreeRoot, Disposition::WontFix,  kCombineStructRationale},
    {Op::Combine,  Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Combine,  Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Combine,  Artifact::Annotations,    Disposition::Asserted, ""},
    // --- split (3-page A into 3 single-page files) -------------------------
    {Op::Split,    Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Split,    Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Split,    Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Split,    Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Split,    Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Split,    Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Split,    Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Split,    Artifact::Annotations,    Disposition::Asserted, ""},
    // --- reorder ({2,1,0}) --------------------------------------------------
    {Op::Reorder,  Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Reorder,  Artifact::Annotations,    Disposition::Asserted, ""},
    // --- rotate (page 0 by +90) ---------------------------------------------
    {Op::Rotate,   Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Rotate,   Artifact::Annotations,    Disposition::Asserted, ""},
    // --- insert (blank page at index 1) -------------------------------------
    {Op::Insert,   Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Insert,   Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Insert,   Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Insert,   Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Insert,   Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Insert,   Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Insert,   Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Insert,   Artifact::Annotations,    Disposition::Asserted, ""},
    // --- delete (page 1) ----------------------------------------------------
    {Op::Delete,   Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Delete,   Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Delete,   Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Delete,   Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Delete,   Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Delete,   Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Delete,   Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Delete,   Artifact::Annotations,    Disposition::Asserted, ""},
    // --- optimize (garbage=4 + dedup, the setting most likely to prune) -----
    {Op::Optimize, Artifact::Outline,        Disposition::Asserted, ""},
    {Op::Optimize, Artifact::PageLabels,     Disposition::Asserted, ""},
    {Op::Optimize, Artifact::FieldValues,    Disposition::Asserted, ""},
    {Op::Optimize, Artifact::OcProperties,   Disposition::Asserted, ""},
    {Op::Optimize, Artifact::StructTreeRoot, Disposition::Asserted, ""},
    {Op::Optimize, Artifact::EmbeddedFiles,  Disposition::Asserted, ""},
    {Op::Optimize, Artifact::NamedDests,     Disposition::Asserted, ""},
    {Op::Optimize, Artifact::Annotations,    Disposition::Asserted, ""},
};

constexpr size_t kMatrixCells = sizeof(kMatrix) / sizeof(kMatrix[0]);

/// Number of WONT_FIX cells. MUST equal the count stated in
/// docs/audit/meta-audit-2026-07/followup/phase-6-dispositions.md.
constexpr int kWontFixCells = 1;

// ---------------------------------------------------------------------------
// One-time world: fixtures plus one run of each operation.
// ---------------------------------------------------------------------------

struct MatrixWorld {
    fs::path dir;
    ArtifactFixtureNames A;
    ArtifactFixtureNames B;
    fs::path srcA, srcB;
    DocArtifacts baseA;
    DocArtifacts extract, combine, reorder, rotate, insert, del, optimize, control;
    std::vector<DocArtifacts> split;
    std::string error;
};

/// Reproduces the pre-F6 extract: a bare empty document plus
/// pdf_graft_mapped_page, with no artifact carry-over at all. Used as the
/// oracle's discrimination control.
void writeLegacyGraftOnlyExtract(const fs::path& in, const fs::path& out)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return;
    fz_register_document_handlers(ctx);
    const std::string outStr = out.string();
    const std::string inStr = in.string();
    pdf_document* src = nullptr;
    pdf_document* dst = nullptr;
    fz_try(ctx) {
        src = pdf_open_document(ctx, inStr.c_str());
        dst = pdf_create_document(ctx);
        pdf_graft_map* map = pdf_new_graft_map(ctx, dst);
        const int n = pdf_count_pages(ctx, src);
        for (int i = 0; i < n; ++i) pdf_graft_mapped_page(ctx, map, i, src, i);
        pdf_drop_graft_map(ctx, map);
        pdf_write_options opts = pdf_default_write_options;
        opts.do_garbage = 1;
        pdf_save_document(ctx, dst, outStr.c_str(), &opts);
    }
    fz_always(ctx) {
        if (dst) pdf_drop_document(ctx, dst);
        if (src) pdf_drop_document(ctx, src);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
}

MatrixWorld buildWorld()
{
    MatrixWorld w;
    std::error_code ec;
    // Unique per-run directory: a repeat run must never re-use an output path a
    // previous run may still hold a handle on (DCA-0010 / #49 lesson).
    w.dir = fs::temp_directory_path() /
            ("pdfe_f6_matrix_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(w.dir, ec);

    w.srcA = w.dir / "src_a.pdf";
    w.srcB = w.dir / "src_b.pdf";
    w.A = buildArtifactRichPdf(w.srcA, 3, "A");
    w.B = buildArtifactRichPdf(w.srcB, 2, "B");
    if (w.A.tag.empty() || w.B.tag.empty() ||
        !fs::exists(w.srcA) || !fs::exists(w.srcB)) {
        w.error = "fixture construction failed";
        return w;
    }
    w.baseA = readArtifacts(w.srcA);
    if (!w.baseA.valid) { w.error = "source fixture unreadable"; return w; }

    auto ctx = std::make_shared<MuPdfContext>();
    try {
        // extract {0,2}
        {
            auto doc = Document::open(ctx, w.srcA);
            const fs::path out = w.dir / "extract.pdf";
            std::vector<int> pages = {0, 2};
            pdfe::core::extractPagesToFile(ctx, doc.get(), pages, out);
            w.extract = readArtifacts(out);
        }
        // split -> one file per page
        {
            auto doc = Document::open(ctx, w.srcA);
            for (int i = 0; i < 3; ++i) {
                const fs::path out = w.dir / ("split_" + std::to_string(i) + ".pdf");
                std::vector<int> pages = {i};
                pdfe::core::extractPagesToFile(ctx, doc.get(), pages, out);
                w.split.push_back(readArtifacts(out));
            }
        }
        // combine A + B
        {
            pdfe::ui::CombineFilesService svc(ctx);
            const fs::path out = w.dir / "combine.pdf";
            std::vector<fs::path> inputs = {w.srcA, w.srcB};
            auto r = svc.combine(inputs, out, nullptr);
            if (!r.ok) { w.error = "combine failed: " + r.errorMessage; return w; }
            w.combine = readArtifacts(out);
        }
        // reorder {2,1,0}
        {
            auto doc = Document::open(ctx, w.srcA);
            doc->reorderPages({2, 1, 0});
            const fs::path out = w.dir / "reorder.pdf";
            doc->saveAs(out);
            w.reorder = readArtifacts(out);
        }
        // rotate page 0 by +90
        {
            auto doc = Document::open(ctx, w.srcA);
            doc->rotatePage(0, 90);
            const fs::path out = w.dir / "rotate.pdf";
            doc->saveAs(out);
            w.rotate = readArtifacts(out);
        }
        // insert a blank page at index 1
        {
            auto doc = Document::open(ctx, w.srcA);
            doc->insertBlankPage(1, 612.0f, 792.0f);
            const fs::path out = w.dir / "insert.pdf";
            doc->saveAs(out);
            w.insert = readArtifacts(out);
        }
        // delete page 1
        {
            auto doc = Document::open(ctx, w.srcA);
            doc->deletePage(1);
            const fs::path out = w.dir / "delete.pdf";
            doc->saveAs(out);
            w.del = readArtifacts(out);
        }
        // optimize with the most aggressive garbage collection
        {
            pdfe::optimize::OptimizeOptions opts;
            opts.mergeDuplicates = true;
            opts.removeUnused = true;
            pdfe::optimize::OptimizeEngine eng(ctx);
            const fs::path out = w.dir / "optimize.pdf";
            auto r = eng.run(w.srcA, out, opts);
            if (!r.success()) { w.error = "optimize failed: " + r.errorMessage; return w; }
            w.optimize = readArtifacts(out);
        }
        // control: the pre-F6 graft-only shape
        {
            const fs::path out = w.dir / "control_graft_only.pdf";
            writeLegacyGraftOnlyExtract(w.srcA, out);
            w.control = readArtifacts(out);
        }
    } catch (const std::exception& ex) {
        w.error = std::string("operation threw: ") + ex.what();
    }
    return w;
}

const MatrixWorld& world()
{
    static const MatrixWorld w = buildWorld();
    return w;
}

// ---------------------------------------------------------------------------
// Cell assertions
// ---------------------------------------------------------------------------

class PreservationMatrix : public ::testing::TestWithParam<Cell> {
protected:
    void SetUp() override {
        ASSERT_TRUE(world().error.empty()) << world().error;
    }
};

void checkExtract(const Cell& c)
{
    // Pages 0 and 2 kept, page 1 dropped: every artifact anchored to page 1
    // must be GONE, which is what stops "preserved everything by removing
    // nothing" from passing.
    const MatrixWorld& w = world();
    const DocArtifacts& a = w.extract;
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, 2);
    switch (c.art) {
        case Artifact::Outline:
            EXPECT_TRUE(HasOutlineTitles(a, {w.A.bookmark(0), w.A.bookmark(2)}));
            EXPECT_TRUE(LacksOutlineTitles(a, {w.A.bookmark(1)}));
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            EXPECT_TRUE(a.hasPageLabelsKey);
            ASSERT_EQ(a.pageLabels.size(), 2u);
            EXPECT_EQ(a.pageLabels[0], w.A.labelPrefix() + "i");
            EXPECT_EQ(a.pageLabels[1], w.A.labelPrefix() + "iii");
            break;
        case Artifact::FieldValues:
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(2)}));
            EXPECT_FALSE(a.hasFieldValue(w.A.fieldValue(1)));
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            EXPECT_TRUE(a.hasStructTreeRoot);
            EXPECT_EQ(a.structPageRoles.size(), 2u)
                << "one structure element per extracted page, orphans pruned";
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasJavaScript(w.A.javascriptName()));
            break;
        case Artifact::NamedDests:
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(0)));
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(2)));
            EXPECT_FALSE(a.hasNamedDest(w.A.destName(1)));
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations:
            EXPECT_TRUE(HasAnnotContents(a, {w.A.annotContents(0), w.A.annotContents(2)}));
            EXPECT_FALSE(a.hasAnnotContents(w.A.annotContents(1)));
            break;
    }
}

void checkCombine(const Cell& c)
{
    const MatrixWorld& w = world();
    const DocArtifacts& a = w.combine;
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, 5);
    switch (c.art) {
        case Artifact::Outline:
            EXPECT_TRUE(HasOutlineTitles(a, {w.A.bookmark(0), w.A.bookmark(1), w.A.bookmark(2),
                                             w.B.bookmark(0), w.B.bookmark(1)}));
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            EXPECT_TRUE(a.hasPageLabelsKey);
            ASSERT_EQ(a.pageLabels.size(), 5u);
            EXPECT_EQ(a.pageLabels[0], w.A.labelPrefix() + "i");
            EXPECT_EQ(a.pageLabels[3], w.B.labelPrefix() + "i")
                << "the second input's own label series must survive the merge";
            break;
        case Artifact::FieldValues:
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(2),
                                           w.B.fieldValue(0), w.B.fieldValue(1)}));
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            EXPECT_TRUE(a.hasOcg(w.B.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            // WONT-FIX lock-in: see kCombineStructRationale. The base document's
            // tree must remain intact and describe exactly its own three pages;
            // the incoming tree is dropped, not half-merged.
            EXPECT_TRUE(a.hasStructTreeRoot);
            EXPECT_EQ(a.structPageRoles.size(), 3u)
                << "the base document's structure tree must survive unchanged; a "
                   "half-merge that mis-anchors elements would change this count";
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasEmbeddedFile(w.B.embeddedFile()))
                << "the second input's attachment must not be silently dropped";
            EXPECT_TRUE(a.hasJavaScript(w.B.javascriptName()));
            break;
        case Artifact::NamedDests:
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(0)));
            EXPECT_TRUE(a.hasNamedDest(w.B.destName(0)));
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations:
            EXPECT_TRUE(HasAnnotContents(a, {w.A.annotContents(0), w.A.annotContents(2),
                                             w.B.annotContents(0), w.B.annotContents(1)}));
            break;
    }
}

void checkSplit(const Cell& c)
{
    const MatrixWorld& w = world();
    ASSERT_EQ(w.split.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        const DocArtifacts& a = w.split[(size_t)i];
        SCOPED_TRACE("split part " + std::to_string(i));
        ASSERT_TRUE(DocReadable(a));
        ASSERT_EQ(a.pageCount, 1);
        const int other = (i + 1) % 3;
        switch (c.art) {
            case Artifact::Outline:
                EXPECT_TRUE(HasOutlineTitles(a, {w.A.bookmark(i)}));
                EXPECT_TRUE(LacksOutlineTitles(a, {w.A.bookmark(other)}));
                EXPECT_TRUE(NoDanglingDestinations(a));
                break;
            case Artifact::PageLabels: {
                EXPECT_TRUE(a.hasPageLabelsKey);
                ASSERT_EQ(a.pageLabels.size(), 1u);
                static const char* const kRoman[] = {"i", "ii", "iii"};
                const std::string expectedLabel = w.A.labelPrefix() + kRoman[(size_t)i];
                EXPECT_EQ(a.pageLabels[0], expectedLabel);
                break;
            }
            case Artifact::FieldValues:
                EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(i)}));
                EXPECT_FALSE(a.hasFieldValue(w.A.fieldValue(other)));
                break;
            case Artifact::OcProperties:
                EXPECT_TRUE(a.hasOcProperties);
                EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
                break;
            case Artifact::StructTreeRoot:
                EXPECT_TRUE(a.hasStructTreeRoot);
                EXPECT_EQ(a.structPageRoles.size(), 1u);
                break;
            case Artifact::EmbeddedFiles:
                EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
                break;
            case Artifact::NamedDests:
                EXPECT_TRUE(a.hasNamedDest(w.A.destName(i)));
                EXPECT_FALSE(a.hasNamedDest(w.A.destName(other)));
                EXPECT_EQ(a.danglingNamedDests(), 0);
                break;
            case Artifact::Annotations:
                EXPECT_TRUE(HasAnnotContents(a, {w.A.annotContents(i)}));
                EXPECT_FALSE(a.hasAnnotContents(w.A.annotContents(other)));
                break;
        }
    }
}

/// Shared body for the same-document operations that keep every page
/// (reorder / rotate / insert). `expectedPages` is the page count afterwards.
void checkFullDocumentOp(const Cell& c, const DocArtifacts& a, int expectedPages)
{
    const MatrixWorld& w = world();
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, expectedPages);
    switch (c.art) {
        case Artifact::Outline:
            EXPECT_TRUE(HasOutlineTitles(a, {w.A.bookmark(0), w.A.bookmark(1), w.A.bookmark(2)}));
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            EXPECT_TRUE(a.hasPageLabelsKey);
            EXPECT_FALSE(a.pageLabels.empty());
            EXPECT_EQ(a.pageLabels[0], w.A.labelPrefix() + "i");
            break;
        case Artifact::FieldValues:
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(1), w.A.fieldValue(2)}));
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            EXPECT_TRUE(a.hasStructTreeRoot);
            EXPECT_EQ(a.structPageRoles.size(), 3u);
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasJavaScript(w.A.javascriptName()));
            break;
        case Artifact::NamedDests:
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(0)));
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(1)));
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(2)));
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations:
            EXPECT_TRUE(HasAnnotContents(a, {w.A.annotContents(0), w.A.annotContents(1),
                                             w.A.annotContents(2)}));
            break;
    }
}

/// Page index a dest whose name starts with `prefix` resolves to.
/// -1 is MuPDF's "dangling"; -2 means no such dest at all. The two must stay
/// distinguishable: a reorder that drops the dest entirely and one that leaves
/// it pointing nowhere are different defects.
int destPageByPrefix(const DocArtifacts& a, const std::string& prefix)
{
    for (const auto& [name, page] : a.namedDestPage) {
        if (name.rfind(prefix, 0) == 0) return page;
    }
    return -2;
}

/// Page index the outline entry titled `title` resolves to, -2 if absent.
int outlinePageByTitle(const DocArtifacts& a, const std::string& title)
{
    for (const auto& o : a.outline) {
        if (o.title == title) return o.destPage;
    }
    return -2;
}

/// The reorder applied to the world is {2,1,0} over three pages, so whatever
/// sat on page p sits on page 2-p afterwards.
constexpr int kReorderPages = 3;
int reorderedFrom(int oldPage) { return (kReorderPages - 1) - oldPage; }

/// Reorder and rotate USED to share `checkFullDocumentOp` with insert and
/// optimize. That was the #224 defect: the shared oracle asserts only that
/// each artifact is still PRESENT, and presence is equally true after the
/// correct permutation, after the wrong one, and after a no-op. For insert and
/// optimize presence is the right question -- nothing is supposed to move. For
/// the two operations whose entire semantics IS movement it answered nothing.
///
/// Every cell below therefore carries a DISCRIMINATION PRECONDITION that the
/// operation actually happened and happened correctly, anchored on the INPUT
/// (w.baseA) rather than on another output. Three of the eight artifacts --
/// field values, /OCProperties, /EmbeddedFiles -- are genuinely document-level
/// and have no page to move to; for those the precondition is what stops a
/// no-op passing, and the presence assertion is then a real preservation check
/// rather than a tautology.
void checkReorder(const Cell& c)
{
    const MatrixWorld& w = world();
    const DocArtifacts& a = w.reorder;
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, kReorderPages);

    // --- discrimination precondition, every cell -----------------------------
    // Anchored on a named destination read BEFORE the edit. If reorderPages()
    // did nothing, dest 0 still resolves to page 0 and this reddens all eight
    // cells instead of letting them pass on presence alone.
    const int base0 = destPageByPrefix(w.baseA, w.A.destName(0));
    const int base2 = destPageByPrefix(w.baseA, w.A.destName(2));
    ASSERT_NE(base0, -2) << "fixture lost its page-0 named dest; the "
                            "precondition cannot discriminate and every "
                            "reorder cell below would be vacuous";
    ASSERT_NE(base0, base2) << "page-0 and page-2 dests resolve to the same "
                               "page in the FIXTURE, so no permutation can be "
                               "distinguished from any other";
    EXPECT_EQ(destPageByPrefix(a, w.A.destName(0)), reorderedFrom(base0))
        << "reorderPages({2,1,0}) did not move the page-0 destination";

    switch (c.art) {
        case Artifact::Outline:
            // Titles surviving is not the property; where they POINT is.
            for (int p = 0; p < kReorderPages; ++p) {
                const std::string title = w.A.bookmark(p);
                const int before = outlinePageByTitle(w.baseA, title);
                ASSERT_NE(before, -2) << "fixture bookmark missing: " << title;
                EXPECT_EQ(outlinePageByTitle(a, title), reorderedFrom(before))
                    << "bookmark '" << title << "' points at the wrong page "
                    << "after reorder";
            }
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            // POSITIONAL, and deliberately so -- this one is worth reading
            // before "fixing" it in the direction the others go.
            //
            // /PageLabels is a number tree keyed by page INDEX. It does not
            // label pages, it numbers POSITIONS: "the pages from index 0 use
            // lowercase roman". That is the folio-number model -- reorder the
            // leaves of a book and the printed numbers still read i, ii, iii
            // down the stack; it is the content at each position that moved.
            //
            // So unlike the outline, the dests, the struct elements and the
            // annotations above, a label must NOT travel with its page. This
            // was asserted the other way round first and measured: reorder
            // leaves page 0 labelled "i" while the page that arrived there was
            // "iii". That is correct, and asserting the intuitive-but-wrong
            // thing here would have turned a working test into a false red.
            //
            // The cell is still not vacuous: the discrimination precondition
            // above has already established that a real reorder happened.
            EXPECT_TRUE(a.hasPageLabelsKey);
            EXPECT_EQ(a.pageLabels, w.baseA.pageLabels)
                << "reorder changed the positional page-numbering scheme; "
                   "labels number positions, so they should be untouched";
            break;
        case Artifact::FieldValues:
            // Document-level: /V is keyed by field name, not by page. The
            // precondition above is what makes this non-vacuous.
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(1),
                                           w.A.fieldValue(2)}));
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            EXPECT_TRUE(a.hasStructTreeRoot);
            ASSERT_EQ(a.structPageRoles.size(), w.baseA.structPageRoles.size())
                << "reorder changed how many page-anchored struct elements "
                   "exist, which is a loss rather than a move";
            // Page-anchored roles are collected in page order, so a reversal
            // of the pages must reverse the sequence.
            for (size_t i = 0; i < w.baseA.structPageRoles.size(); ++i) {
                const size_t j = w.baseA.structPageRoles.size() - 1 - i;
                EXPECT_EQ(a.structPageRoles[j], w.baseA.structPageRoles[i])
                    << "struct element role at " << i << " did not move to "
                    << j << " with its page";
            }
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasJavaScript(w.A.javascriptName()));
            break;
        case Artifact::NamedDests:
            for (int p = 0; p < kReorderPages; ++p) {
                const int before = destPageByPrefix(w.baseA, w.A.destName(p));
                ASSERT_NE(before, -2) << "fixture dest missing for page " << p;
                EXPECT_EQ(destPageByPrefix(a, w.A.destName(p)),
                          reorderedFrom(before))
                    << "named dest for page " << p << " did not follow it";
            }
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations: {
            // Same anchoring as PreservationAnnotations.Reorder..., applied
            // per-cell so this cell fails on its own.
            std::vector<AnnotTuple> expected;
            for (const AnnotTuple& t : w.baseA.annots) {
                AnnotTuple e = t;
                e.page = reorderedFrom(t.page);
                expected.push_back(e);
            }
            std::sort(expected.begin(), expected.end());
            ASSERT_FALSE(expected.empty())
                << "fixture has no annotations, so this cell cannot fail";
            EXPECT_TRUE(AnnotTuplesEqual(expected, a.annots));
            break;
        }
    }
}

/// The world rotates page 0 by +90 and leaves pages 1 and 2 alone.
void checkRotate(const Cell& c)
{
    const MatrixWorld& w = world();
    const DocArtifacts& a = w.rotate;
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, kReorderPages);

    // --- discrimination precondition, every cell -----------------------------
    // Rotation is the ONLY thing this operation changes, so without reading it
    // no rotate cell can tell a real rotate from a no-op. Anchored on the
    // rotation read before the edit, not on a hardcoded 0.
    ASSERT_EQ(a.pageRotations.size(), static_cast<size_t>(kReorderPages));
    ASSERT_EQ(w.baseA.pageRotations.size(), static_cast<size_t>(kReorderPages));
    EXPECT_EQ(a.pageRotations[0], (w.baseA.pageRotations[0] + 90) % 360)
        << "rotatePage(0, 90) did not change page 0's effective /Rotate";
    for (int p = 1; p < kReorderPages; ++p) {
        EXPECT_EQ(a.pageRotations[p], w.baseA.pageRotations[p])
            << "page " << p << " was rotated but only page 0 should have been";
    }

    // Rotation moves no page and owns no artifact, so every artifact must
    // survive UNCHANGED -- and, thanks to the precondition, a no-op can no
    // longer satisfy that trivially.
    switch (c.art) {
        case Artifact::Outline:
            for (int p = 0; p < kReorderPages; ++p) {
                const std::string title = w.A.bookmark(p);
                EXPECT_EQ(outlinePageByTitle(a, title),
                          outlinePageByTitle(w.baseA, title))
                    << "rotate moved bookmark '" << title << "'";
            }
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            EXPECT_TRUE(a.hasPageLabelsKey);
            EXPECT_EQ(a.pageLabels, w.baseA.pageLabels)
                << "rotate altered page labels";
            break;
        case Artifact::FieldValues:
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(1),
                                           w.A.fieldValue(2)}));
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            EXPECT_TRUE(a.hasStructTreeRoot);
            EXPECT_EQ(a.structPageRoles, w.baseA.structPageRoles)
                << "rotate disturbed the page-anchored struct elements";
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasJavaScript(w.A.javascriptName()));
            break;
        case Artifact::NamedDests:
            for (int p = 0; p < kReorderPages; ++p) {
                EXPECT_EQ(destPageByPrefix(a, w.A.destName(p)),
                          destPageByPrefix(w.baseA, w.A.destName(p)))
                    << "rotate moved the named dest for page " << p;
            }
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations:
            // /Rect is in unrotated default user space, so a page /Rotate
            // change must leave every rect byte-identical.
            ASSERT_FALSE(w.baseA.annots.empty())
                << "fixture has no annotations, so this cell cannot fail";
            EXPECT_TRUE(AnnotTuplesEqual(w.baseA.annots, a.annots));
            break;
    }
}

void checkDelete(const Cell& c)
{
    // Page 1 removed: every page-1 artifact must be gone AND nothing may dangle.
    const MatrixWorld& w = world();
    const DocArtifacts& a = w.del;
    ASSERT_TRUE(DocReadable(a));
    ASSERT_EQ(a.pageCount, 2);
    switch (c.art) {
        case Artifact::Outline:
            EXPECT_TRUE(HasOutlineTitles(a, {w.A.bookmark(0), w.A.bookmark(2)}));
            EXPECT_TRUE(LacksOutlineTitles(a, {w.A.bookmark(1)}));
            EXPECT_TRUE(NoDanglingDestinations(a));
            break;
        case Artifact::PageLabels:
            EXPECT_TRUE(a.hasPageLabelsKey);
            ASSERT_EQ(a.pageLabels.size(), 2u);
            EXPECT_EQ(a.pageLabels[0], w.A.labelPrefix() + "i");
            break;
        case Artifact::FieldValues:
            EXPECT_TRUE(HasFieldValues(a, {w.A.fieldValue(0), w.A.fieldValue(2)}));
            EXPECT_FALSE(a.hasFieldValue(w.A.fieldValue(1)))
                << "the deleted page's field value must not survive";
            break;
        case Artifact::OcProperties:
            EXPECT_TRUE(a.hasOcProperties);
            EXPECT_TRUE(a.hasOcg(w.A.ocgName()));
            break;
        case Artifact::StructTreeRoot:
            EXPECT_TRUE(a.hasStructTreeRoot);
            EXPECT_EQ(a.structPageRoles.size(), 2u);
            break;
        case Artifact::EmbeddedFiles:
            EXPECT_TRUE(a.hasEmbeddedFile(w.A.embeddedFile()));
            EXPECT_TRUE(a.hasJavaScript(w.A.javascriptName()));
            break;
        case Artifact::NamedDests:
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(0)));
            EXPECT_TRUE(a.hasNamedDest(w.A.destName(2)));
            EXPECT_FALSE(a.hasNamedDest(w.A.destName(1)));
            EXPECT_EQ(a.danglingNamedDests(), 0);
            break;
        case Artifact::Annotations:
            EXPECT_TRUE(HasAnnotContents(a, {w.A.annotContents(0), w.A.annotContents(2)}));
            EXPECT_FALSE(a.hasAnnotContents(w.A.annotContents(1)));
            break;
    }
}

TEST_P(PreservationMatrix, Cell)
{
    const Cell c = GetParam();
    if (c.disp == Disposition::WontFix) {
        ASSERT_NE(std::string(c.rationale), "")
            << "every wont-fix cell must carry an in-source rationale";
    }
    switch (c.op) {
        case Op::Extract:  checkExtract(c); break;
        case Op::Combine:  checkCombine(c); break;
        case Op::Split:    checkSplit(c); break;
        case Op::Reorder:  checkReorder(c); break;
        case Op::Rotate:   checkRotate(c); break;
        case Op::Insert:   checkFullDocumentOp(c, world().insert, 4); break;
        case Op::Delete:   checkDelete(c); break;
        case Op::Optimize: checkFullDocumentOp(c, world().optimize, 3); break;
    }
}

std::string cellName(const ::testing::TestParamInfo<Cell>& info)
{
    return std::string(opName(info.param.op)) + "_" + artifactName(info.param.art);
}

INSTANTIATE_TEST_SUITE_P(F6, PreservationMatrix,
                         ::testing::ValuesIn(kMatrix), cellName);

// ---------------------------------------------------------------------------
// Matrix meta-assertions
// ---------------------------------------------------------------------------

TEST(PreservationMatrixShape, CoversAll64CellsExactlyOnce)
{
    EXPECT_EQ(kMatrixCells, 64u);
    std::set<std::pair<int, int>> seen;
    for (const Cell& c : kMatrix) {
        auto key = std::make_pair(static_cast<int>(c.op), static_cast<int>(c.art));
        EXPECT_TRUE(seen.insert(key).second)
            << "duplicate cell " << opName(c.op) << "/" << artifactName(c.art);
    }
    EXPECT_EQ(seen.size(), 64u);
}

TEST(PreservationMatrixShape, WontFixCountMatchesDeclaredCount)
{
    int n = 0;
    for (const Cell& c : kMatrix) {
        if (c.disp == Disposition::WontFix) {
            ++n;
            EXPECT_NE(std::string(c.rationale), "")
                << opName(c.op) << "/" << artifactName(c.art)
                << " is wont-fix with no rationale";
        }
    }
    // This number is quoted in
    // docs/audit/meta-audit-2026-07/followup/phase-6-dispositions.md.
    EXPECT_EQ(n, kWontFixCells);
}

// ---------------------------------------------------------------------------
// Control: the oracle must be able to see total destruction.
// ---------------------------------------------------------------------------

TEST(PreservationMatrixControl, ControlOracleDetectsTotalLoss)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const DocArtifacts& base = world().baseA;
    const DocArtifacts& ctl = world().control;
    ASSERT_TRUE(DocReadable(base));
    ASSERT_TRUE(DocReadable(ctl));

    // The source carries all eight; the graft-only output carries none. If this
    // ever inverts, every "still present" assertion in the matrix above has
    // become vacuous.
    for (Artifact a : {Artifact::Outline, Artifact::PageLabels, Artifact::FieldValues,
                       Artifact::OcProperties, Artifact::StructTreeRoot,
                       Artifact::EmbeddedFiles, Artifact::NamedDests,
                       Artifact::Annotations}) {
        SCOPED_TRACE(artifactName(a));
        EXPECT_TRUE(artifactPresent(base, a)) << "fixture does not carry this artifact";
        EXPECT_FALSE(artifactPresent(ctl, a))
            << "a bare pdf_graft_mapped_page output must carry no document artifact; "
               "if it does, the oracle is not discriminating";
    }
    EXPECT_EQ(ctl.pageCount, base.pageCount);
}

// ---------------------------------------------------------------------------
// PDF-97 / PDF-104: annotation identity and geometry, not annotation COUNT.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PDF-91 / PDF-95: the kPreservedKeys[] contract in Document::rearrangePages.
// ---------------------------------------------------------------------------

namespace {

/// The exact list Document::rearrangePages restores after pdf_rearrange_pages
/// rebuilds the catalog. Kept in the same order as the source array so a drift
/// is obvious on sight.
const char* const kPreservedCatalogKeys[] = {
    "AcroForm", "Metadata", "Lang", "MarkInfo", "ViewerPreferences",
    "PageLabels", "PageLayout", "PageMode", "Perms", "Version",
    "Extensions", "Collection", "AF",
};
constexpr size_t kPreservedCatalogKeyCount =
    sizeof(kPreservedCatalogKeys) / sizeof(kPreservedCatalogKeys[0]);

/// Keys that rearrangePages drops ON PURPOSE because they can dangle to a page
/// that no longer exists (the legacy catalog /Dests dict, /OpenAction) or are
/// documented as unsupported (/Threads, article threads).
const char* const kByDesignDroppedCatalogKeys[] = { "Threads", "OpenAction", "Dests" };

/// Catalog keys that are in NEITHER list above: the general rearrange path drops
/// them as collateral of rebuilding /Root, and the p85 phase-5 permutation fast
/// path leaves them alone because it never rebuilds /Root at all. All are
/// page-position INDEPENDENT, so surviving a permutation is correct.
///
/// These exist to give the fast-path test something to assert that the general
/// path would NOT satisfy — without them, a fast-path test would be
/// indistinguishable from the general-path tests it is meant to complement.
const char* const kFastPathSurvivingCatalogKeys[] = { "AA", "OutputIntents", "PieceInfo" };

/// Stamp a recognisable value on every catalog key in the lists above.
///
/// @param withLegacyDests  when false, the legacy PDF-1.1 catalog /Dests
///        DICTIONARY is not stamped. That one key decides WHICH CODE PATH a
///        reorder takes: `Document::rearrangePages`' permutation fast path
///        (`permutePageTreeInPlace`, GitHub #183) excludes any document carrying
///        it, because `pdf_rearrange_pages` does not drop that key — it MIGRATES
///        it into /Names/Dests, and reproducing that migration is the page-tree
///        semantics rewrite the fast path exists to avoid.
///
///        This parameter exists because every pre-existing catalog-preservation
///        test stamped /Dests, so every one of them exercised the GENERAL path
///        only. The fast path's catalog behaviour rested on an architectural
///        argument and a source comment, with nothing that would go red if
///        either broke. The phase-5 claim-check gate called that out as
///        load-bearing and unmechanised, and it was right to.
void stampCatalogKeys(const fs::path& in, const fs::path& out,
                      bool withLegacyDests = true)
{
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    ASSERT_NE(ctx, nullptr);
    fz_register_document_handlers(ctx);
    const std::string inStr = in.string();
    const std::string outStr = out.string();
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, inStr.c_str());
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        for (const char* key : kPreservedCatalogKeys) {
            if (pdf_dict_gets(ctx, root, key)) continue;   // fixture already has it
            pdf_obj* d = pdf_new_dict(ctx, doc, 1);
            pdf_dict_puts_drop(ctx, d, "PdfeF6Marker", pdf_new_text_string(ctx, key));
            pdf_dict_puts_drop(ctx, root, key, pdf_add_object_drop(ctx, doc, d));
        }
        for (const char* key : kByDesignDroppedCatalogKeys) {
            // The one key that selects the code path -- see withLegacyDests.
            if (!withLegacyDests && std::string(key) == "Dests") continue;
            pdf_obj* d = pdf_new_dict(ctx, doc, 1);
            pdf_dict_puts_drop(ctx, d, "PdfeF6Marker", pdf_new_text_string(ctx, key));
            pdf_dict_puts_drop(ctx, root, key, pdf_add_object_drop(ctx, doc, d));
        }
        for (const char* key : kFastPathSurvivingCatalogKeys) {
            pdf_obj* d = pdf_new_dict(ctx, doc, 1);
            pdf_dict_puts_drop(ctx, d, "PdfeF6Marker", pdf_new_text_string(ctx, key));
            pdf_dict_puts_drop(ctx, root, key, pdf_add_object_drop(ctx, doc, d));
        }
        pdf_write_options wo = pdf_default_write_options;
        wo.do_compress = 0;
        pdf_save_document(ctx, doc, outStr.c_str(), &wo);
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
}

/// Which of the given catalog keys are present in the saved file.
std::set<std::string> presentCatalogKeys(const fs::path& p,
                                         const std::vector<std::string>& keys)
{
    std::set<std::string> found;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return found;
    fz_register_document_handlers(ctx);
    const std::string pStr = p.string();
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, pStr.c_str());
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        for (const auto& k : keys) {
            if (pdf_dict_gets(ctx, root, k.c_str())) found.insert(k);
        }
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return found;
}

/// The /PageLabels number tree as written, entry by entry: the integer key
/// followed by that entry's /S style and /P prefix.
///
/// This reads the STRUCTURE rather than the resolved label, so it observes the
/// spec property directly: /PageLabels is a number tree keyed by page INDEX,
/// i.e. it numbers POSITIONS. Returning the raw keys means a tool that
/// "helpfully" permuted the tree to follow its pages is visible here even if
/// the resolved label strings happened to coincide.
std::vector<std::string> pageLabelNums(const fs::path& p)
{
    std::vector<std::string> out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    fz_register_document_handlers(ctx);
    const std::string pStr = p.string();
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, pStr.c_str());
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* pl = pdf_resolve_indirect(
            ctx, pdf_dict_get(ctx, root, PDF_NAME(PageLabels)));
        pdf_obj* nums = pdf_resolve_indirect(
            ctx, pdf_dict_get(ctx, pl, PDF_NAME(Nums)));
        const int n = pdf_array_len(ctx, nums);
        for (int i = 0; i + 1 < n; i += 2) {
            pdf_obj* key = pdf_array_get(ctx, nums, i);
            pdf_obj* val = pdf_resolve_indirect(ctx, pdf_array_get(ctx, nums, i + 1));
            std::string e = std::to_string(pdf_to_int(ctx, key));
            const char* s = pdf_to_name(ctx, pdf_dict_get(ctx, val, PDF_NAME(S)));
            e += "/S="; e += (s ? s : "");
            pdf_obj* pref = pdf_dict_get(ctx, val, PDF_NAME(P));
            const char* ps = pdf_is_string(ctx, pref)
                           ? pdf_to_text_string(ctx, pref) : "";
            e += "/P="; e += (ps ? ps : "");
            out.push_back(e);
        }
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return out;
}

/// Fails unless `after` really is `before` with the pages permuted {2,1,0}.
///
/// GitHub #224. The catalog-key tests below assert that thirteen /Root keys are
/// still present after a rearrange. That is a true statement about a document
/// that was never rearranged, so without this the whole family passes against a
/// no-op `reorderPages()` -- the same shape as the sixteen matrix cells, in a
/// different file.
///
/// Anchored on annotations because they are the only page-ANCHORED observable
/// both stamped fixtures carry: the fast-path fixture deliberately has no
/// legacy /Dests, which is the very thing that routes it onto the fast path.
/// Page labels would not do -- they are positional and correctly do not move.
::testing::AssertionResult RearrangePermutedThePages(const fs::path& before,
                                                     const fs::path& after)
{
    const DocArtifacts b = readArtifacts(before);
    const DocArtifacts a = readArtifacts(after);
    if (!b.valid) return ::testing::AssertionFailure()
        << "stamped fixture unreadable: " << b.error;
    if (!a.valid) return ::testing::AssertionFailure()
        << "rearranged output unreadable: " << a.error;
    if (b.annots.empty()) return ::testing::AssertionFailure()
        << "the stamped fixture carries no annotations, so no permutation is "
           "observable and this precondition would itself be vacuous";

    std::vector<AnnotTuple> expected;
    for (AnnotTuple t : b.annots) {
        t.page = (b.pageCount - 1) - t.page;
        expected.push_back(t);
    }
    std::sort(expected.begin(), expected.end());
    return AnnotTuplesEqual(expected, a.annots);
}

/// Stamped fixture -> reorder -> save. Shared by the two tests below.
struct StampedRearrangeResult {
    fs::path stamped;
    fs::path reordered;
    std::string error;
};

/// Same shape, but the fixture carries NO legacy /Dests, so the reorder is
/// eligible for `permutePageTreeInPlace` and takes the fast path.
///
/// `{2, 1, 0}` is already a full permutation of the three-page fixture, so the
/// ONLY thing that routed the pre-existing tests onto the general path was the
/// stamped /Dests. Dropping it is the whole difference.
StampedRearrangeResult buildStampedRearrangeFastPath()
{
    StampedRearrangeResult r;
    const MatrixWorld& w = world();
    r.stamped = w.dir / "stamped_fastpath.pdf";
    r.reordered = w.dir / "stamped_fastpath_reordered.pdf";
    stampCatalogKeys(w.srcA, r.stamped, /*withLegacyDests=*/false);
    if (!fs::exists(r.stamped)) { r.error = "stamping failed"; return r; }
    try {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, r.stamped);
        doc->reorderPages({2, 1, 0});
        doc->saveAs(r.reordered);
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

StampedRearrangeResult buildStampedRearrange()
{
    StampedRearrangeResult r;
    const MatrixWorld& w = world();
    r.stamped = w.dir / "stamped.pdf";
    r.reordered = w.dir / "stamped_reordered.pdf";
    stampCatalogKeys(w.srcA, r.stamped);
    if (!fs::exists(r.stamped)) { r.error = "stamping failed"; return r; }
    try {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, r.stamped);
        doc->reorderPages({2, 1, 0});
        doc->saveAs(r.reordered);
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

} // namespace

TEST(PreservationCatalogKeys, RearrangePreservesAllThirteenPreservedKeys)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const auto r = buildStampedRearrange();
    ASSERT_TRUE(r.error.empty()) << r.error;

    std::vector<std::string> keys(kPreservedCatalogKeys,
                                  kPreservedCatalogKeys + kPreservedCatalogKeyCount);
    ASSERT_EQ(keys.size(), 13u) << "kPreservedKeys[] in Document::rearrangePages "
                                   "has 13 entries; this list must mirror it";

    // CONTROL: the stamped input really does carry all thirteen, so the
    // post-rearrange assertion cannot pass vacuously.
    const auto before = presentCatalogKeys(r.stamped, keys);
    ASSERT_EQ(before.size(), keys.size()) << "control failed: the stamped fixture "
                                             "does not carry every preserved key";

    // DISCRIMINATION (#224): the control above proves the keys were THERE, not
    // that anything happened to them. "All thirteen survived" is equally true
    // of a document that was never rearranged.
    ASSERT_TRUE(RearrangePermutedThePages(r.stamped, r.reordered))
        << "rearrangePages did not permute the pages, so the key assertions "
           "below are satisfied by doing nothing";

    const auto after = presentCatalogKeys(r.reordered, keys);
    for (const auto& k : keys) {
        EXPECT_TRUE(after.count(k)) << "/" << k
            << " did not survive rearrangePages, but kPreservedKeys[] claims it does";
    }
}

// ---------------------------------------------------------------------------
// FAST-PATH catalog contract (GitHub #183, p85 phase-5).
//
// WHY THESE EXIST. `Document::rearrangePages` has two code paths. Every test
// above stamps a legacy /Dests, which is the exact key that forces the GENERAL
// path -- so before these three, nothing in the suite ever executed
// `permutePageTreeInPlace`, and `grep -rl permutePageTreeInPlace tests/*.cpp`
// returned nothing at all. Its catalog behaviour was asserted only by a source
// comment reading "PDF-95 parity". Deleting the two /OpenAction+/Threads lines,
// or widening the /Dests eligibility check, would have reddened NOTHING.
//
// The phase-5 claim-check gate classified that as load-bearing and unmechanised
// and failed the phase over it, against two other gates that had called it
// acceptable backlog. It was right: a named behavioural contract with no
// coupling to any check is exactly the shape that rots silently.
// ---------------------------------------------------------------------------

TEST(PreservationCatalogKeys, FastPathPreservesAllThirteenPreservedKeys)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const auto r = buildStampedRearrangeFastPath();
    ASSERT_TRUE(r.error.empty()) << r.error;

    std::vector<std::string> keys(kPreservedCatalogKeys,
                                  kPreservedCatalogKeys + kPreservedCatalogKeyCount);

    // CONTROL 1: the fixture carries all thirteen, so the post-reorder
    // assertion cannot pass vacuously.
    const auto before = presentCatalogKeys(r.stamped, keys);
    ASSERT_EQ(before.size(), keys.size())
        << "control failed: the stamped fixture does not carry every preserved key";

    // CONTROL 2: this fixture really is fast-path eligible. If /Dests were
    // present the reorder would silently take the GENERAL path and this test
    // would duplicate RearrangePreservesAllThirteenPreservedKeys while
    // appearing to cover the fast path -- the precise failure it exists to stop.
    const auto dests = presentCatalogKeys(r.stamped, {"Dests"});
    ASSERT_TRUE(dests.empty())
        << "control failed: the fixture carries a legacy /Dests, so this "
           "reorder took the GENERAL path and the fast path is still untested";

    // CONTROL 3 (#224): the two controls above establish what the fixture IS.
    // Neither establishes that the fast path DID anything, and "all thirteen
    // survived" is true of a document nothing happened to.
    ASSERT_TRUE(RearrangePermutedThePages(r.stamped, r.reordered))
        << "the permutation fast path did not permute the pages";

    const auto after = presentCatalogKeys(r.reordered, keys);
    for (const auto& k : keys) {
        EXPECT_TRUE(after.count(k)) << "/" << k
            << " did not survive the permutation fast path";
    }
}

TEST(PreservationCatalogKeys, FastPathDropsThreadsAndOpenActionForParity)
{
    // The fast path never rebuilds /Root, so it would naturally carry
    // /OpenAction and /Threads over. It deletes them ON PURPOSE so a permutation
    // and a general rearrange agree -- both are page-position DEPENDENT and
    // PDFE has no remapping for either. This test is what makes that deletion a
    // contract instead of two lines someone could tidy away.
    ASSERT_TRUE(world().error.empty()) << world().error;
    const auto r = buildStampedRearrangeFastPath();
    ASSERT_TRUE(r.error.empty()) << r.error;

    const std::vector<std::string> keys{"Threads", "OpenAction"};
    const auto before = presentCatalogKeys(r.stamped, keys);
    ASSERT_EQ(before.size(), keys.size())
        << "control failed: the fixture does not carry /Threads and /OpenAction";

    const auto after = presentCatalogKeys(r.reordered, keys);
    for (const auto& k : keys) {
        EXPECT_FALSE(after.count(k)) << "/" << k
            << " survived the permutation fast path, but the general path drops "
               "it and the two must agree (PDF-95 parity)";
    }
}

TEST(PreservationCatalogKeys, FastPathLeavesPagePositionIndependentKeysAlone)
{
    // The positive half of the same contract, and the one that distinguishes the
    // fast path from the general path rather than merely matching it: keys
    // outside kPreservedKeys[] that the general path drops as COLLATERAL of
    // rebuilding /Root survive a permutation, because nothing rebuilds /Root.
    // All are page-position independent, so this is a reduction in silent loss.
    ASSERT_TRUE(world().error.empty()) << world().error;
    const auto r = buildStampedRearrangeFastPath();
    ASSERT_TRUE(r.error.empty()) << r.error;

    std::vector<std::string> keys(std::begin(kFastPathSurvivingCatalogKeys),
                                  std::end(kFastPathSurvivingCatalogKeys));
    const auto before = presentCatalogKeys(r.stamped, keys);
    ASSERT_EQ(before.size(), keys.size())
        << "control failed: the fixture does not carry the collateral keys";

    // #224: "survived" means nothing unless something happened to survive.
    ASSERT_TRUE(RearrangePermutedThePages(r.stamped, r.reordered))
        << "the permutation fast path did not permute the pages";

    const auto after = presentCatalogKeys(r.reordered, keys);
    for (const auto& k : keys) {
        EXPECT_TRUE(after.count(k)) << "/" << k
            << " was dropped by the permutation fast path. It is page-position "
               "independent and the fast path does not rebuild /Root, so either "
               "the fast path stopped being taken or it started rebuilding the "
               "catalog";
    }
}

TEST(PreservationCatalogKeys, RearrangeDropsThreadsAndOpenActionByDesign)
{
    // PDF-95 lock-in NEGATIVE test. /Threads (article threads), /OpenAction and
    // the legacy PDF-1.1 catalog /Dests dictionary are page-position DEPENDENT:
    // each can point at a page the rearrange removed, and PDFE has no remapping
    // for them. They are dropped deliberately. This test exists so that decision
    // is a recorded, asserted contract rather than a source comment -- if a
    // future change starts carrying them over, it must come with the remapping,
    // and this test is where that conversation starts.
    ASSERT_TRUE(world().error.empty()) << world().error;
    const auto r = buildStampedRearrange();
    ASSERT_TRUE(r.error.empty()) << r.error;

    std::vector<std::string> keys(std::begin(kByDesignDroppedCatalogKeys),
                                  std::end(kByDesignDroppedCatalogKeys));
    const auto before = presentCatalogKeys(r.stamped, keys);
    ASSERT_EQ(before.size(), keys.size())
        << "control failed: the stamped fixture does not carry the by-design-dropped keys";

    const auto after = presentCatalogKeys(r.reordered, keys);
    for (const auto& k : keys) {
        EXPECT_FALSE(after.count(k))
            << "/" << k << " survived rearrangePages. It is page-position dependent "
               "and PDFE does not remap it, so carrying it over leaves a dangling "
               "reference. If this is now intentional, add the remapping first.";
    }
}

TEST(PreservationAnnotations, ReorderMovesAnnotationTuplesWithTheirPages)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const MatrixWorld& w = world();
    ASSERT_TRUE(DocReadable(w.reorder));

    // Pages were reversed, so each annotation must now be on page (2 - old).
    std::vector<AnnotTuple> expected;
    for (const AnnotTuple& t : w.baseA.annots) {
        AnnotTuple e = t;
        e.page = 2 - t.page;
        expected.push_back(e);
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_TRUE(AnnotTuplesEqual(expected, w.reorder.annots));
}

// GitHub #224, claim-check finding. The `reorder x PageLabels` cell asserts
// that labels do NOT travel with their pages, and the only thing defending that
// counter-intuitive direction was a comment explaining it. A comment is not a
// mechanism: nothing stopped a later reader from "correcting" the cell, and then
// "correcting" the product to match.
//
// This test is that mechanism, and it deliberately does NOT restate the cell's
// assertion. It observes the STRUCTURE -- the /PageLabels number tree as
// written -- because the spec property is about the tree's keys, not about the
// strings they resolve to. /PageLabels maps page INDEX to a numbering style, so
// it numbers POSITIONS: reorder the leaves of a book and the printed folio
// numbers still read i, ii, iii down the stack; it is the content at each
// position that moved.
//
// A tool that permuted the tree to follow its pages fails here even if the
// resolved labels happened to coincide, which is the case the cell alone cannot
// see.
TEST(PreservationPageLabels, ReorderLeavesTheNumberTreeKeyedByPosition)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const MatrixWorld& w = world();

    const auto before = pageLabelNums(w.srcA);
    ASSERT_FALSE(before.empty())
        << "the fixture carries no /PageLabels number tree, so this test would "
           "compare two empty vectors and pass without observing anything";

    const fs::path reordered = w.dir / "reorder.pdf";
    ASSERT_TRUE(fs::exists(reordered)) << "the world's reorder output is missing";
    const auto after = pageLabelNums(reordered);

    // The reorder must actually have happened, or "the tree is unchanged" is
    // satisfied by a document nothing was done to.
    ASSERT_TRUE(RearrangePermutedThePages(w.srcA, reordered))
        << "reorderPages did not permute the pages, so this test would pass "
           "vacuously";

    EXPECT_EQ(after, before)
        << "reorder rewrote the /PageLabels number tree. The tree is keyed by "
           "page INDEX and numbers positions, so a permutation must leave it "
           "untouched -- see the reorder x PageLabels cell above";
}

TEST(PreservationAnnotations, RotatePreservesAnnotationGeometry)
{
    ASSERT_TRUE(world().error.empty()) << world().error;
    const MatrixWorld& w = world();
    ASSERT_TRUE(DocReadable(w.rotate));

    // DISCRIMINATION (#224). "The rects are unchanged" is the single most
    // trivially-satisfiable claim in this file: it is true of a rotate that
    // never happened. Until pageRotations existed there was nothing in the
    // observation model that varied with rotation at all, so this test could
    // not have been written any other way -- which is exactly the coverage gap
    // rather than an oversight.
    ASSERT_EQ(w.rotate.pageRotations.size(), w.baseA.pageRotations.size());
    ASSERT_FALSE(w.baseA.pageRotations.empty());
    EXPECT_EQ(w.rotate.pageRotations[0], (w.baseA.pageRotations[0] + 90) % 360)
        << "page 0 was not actually rotated, so 'the geometry is preserved' "
           "below is satisfied by doing nothing";

    ASSERT_FALSE(w.baseA.annots.empty())
        << "the fixture carries no annotations, so there is no geometry to "
           "preserve and this test cannot fail";

    // /Rect is in unrotated default user space, so a page /Rotate change must
    // leave every annotation rect byte-identical. A tool that "helpfully"
    // rewrote rects on rotate would fail here.
    EXPECT_TRUE(AnnotTuplesEqual(w.baseA.annots, w.rotate.annots));
}

} // namespace
