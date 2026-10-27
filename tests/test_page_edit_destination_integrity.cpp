// -*- coding: utf-8 -*-
//
// R3-01 (High) — destination integrity across structural page edits.
//
// A class-level CONTENT oracle: a page delete or reorder must remap every
// internal destination (bookmark /Outlines dest, /Link GoTo annotation, named
// dest) so it keeps pointing at the SAME target page, and must drop/clear a
// destination whose target page was deleted (never leave it dangling to the
// wrong page). Targets are resolved BY CONTENT (a unique per-page text marker)
// so the assertions survive object/page renumbering.
//
// Fixture: tests/fixtures/test_dest_integrity.pdf (generator
// tests/fixtures/generate_dest_integrity_pdf.py) — 5 pages each drawing a
// unique marker "PDFEPAGEMARK0".."PDFEPAGEMARK4"; three bookmarks targeting
// pages 1/2/3; two GoTo link annotations on page 0 targeting pages 2/3.
//
// RED on unpatched HEAD (bare pdf_delete_page + retain/delete/graft move):
// destinations mis-resolve or dangle after the edit. GREEN once delete + move
// route through pdf_rearrange_pages (Document::rearrangePages/reorderPages).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/BookmarkNode.h"
#include "core/PageCommands.h"

#include "annotations/AnnotationManager.h"
#include "annotations/LinkAction.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#  error "TEST_FIXTURES_DIR must be defined"
#endif

using namespace pdfe::core;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::LinkAction;
using pdfe::annotations::LinkActionType;

namespace {

std::filesystem::path fixturesDir() { return std::filesystem::path(TEST_FIXTURES_DIR); }

std::filesystem::path workDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_dest_integrity";
    std::filesystem::create_directories(d);
    return d;
}

std::filesystem::path workCopy(const char* name)
{
    auto p = workDir() / name;
    std::filesystem::copy_file(fixturesDir() / "test_dest_integrity.pdf", p,
                               std::filesystem::copy_options::overwrite_existing);
    return p;
}

// The unique content marker printed on page `idx`, or "" if none / out of range.
std::string markerOnPage(Document* doc, int idx)
{
    if (idx < 0 || idx >= doc->pageCount()) return "";
    auto page = doc->page(idx);
    if (!page) return "";
    std::string text = page->extractPlainText();
    auto pos = text.find("PDFEPAGEMARK");
    if (pos == std::string::npos) return "";
    // marker is "PDFEPAGEMARK" + one digit
    return text.substr(pos, std::string("PDFEPAGEMARK").size() + 1);
}

int findPageByMarker(Document* doc, const std::string& marker)
{
    for (int i = 0; i < doc->pageCount(); ++i) {
        if (markerOnPage(doc, i) == marker) return i;
    }
    return -1;
}

void flatten(const std::vector<std::shared_ptr<BookmarkNode>>& nodes,
             std::vector<const BookmarkNode*>& out)
{
    for (const auto& n : nodes) {
        if (!n) continue;
        out.push_back(n.get());
        flatten(n->children, out);
    }
}

// Map bookmark title -> the content marker its destination currently resolves
// to. Only Page-type, in-range destinations are included; a bookmark whose dest
// was cleared / points nowhere is deliberately omitted (so the caller can see
// it stopped resolving to any page).
std::map<std::string, std::string> bookmarkTitleToMarker(Document* doc)
{
    std::map<std::string, std::string> m;
    // Keep the returned tree alive: it OWNS the shared_ptr nodes; flatten stores
    // raw pointers into them, so the vector must outlive their use.
    auto tree = doc->extractBookmarks();
    std::vector<const BookmarkNode*> flat;
    flatten(tree, flat);
    for (const auto* b : flat) {
        if (b->destination.type == BookmarkDestType::Page &&
            b->destination.pageIndex >= 0 &&
            b->destination.pageIndex < doc->pageCount()) {
            std::string mk = markerOnPage(doc, b->destination.pageIndex);
            if (!mk.empty()) m[b->title] = mk;
        }
    }
    return m;
}

// All GoTo link-annotation target markers on the page at `pageIdx`.
std::vector<std::string> linkTargetMarkers(Document* doc, AnnotationManager& mgr,
                                           int pageIdx)
{
    std::vector<std::string> out;
    if (pageIdx < 0 || pageIdx >= doc->pageCount()) return out;
    auto page = doc->page(pageIdx);
    if (!page) return out;
    // Iterate a generous range of annotation indices; getLinkAction returns a
    // default (Uri, empty) for out-of-range / non-link indices.
    for (int i = 0; i < 32; ++i) {
        LinkAction a = mgr.getLinkAction(page.get(), i);
        if (a.type == LinkActionType::GoTo && a.pageIndex >= 0 &&
            a.pageIndex < doc->pageCount()) {
            std::string mk = markerOnPage(doc, a.pageIndex);
            if (!mk.empty()) out.push_back(mk);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool fixtureAvailable()
{
    return std::filesystem::exists(fixturesDir() / "test_dest_integrity.pdf");
}

// Multiset of markers minus one occurrence of `remove` (if present), sorted.
std::vector<std::string> without(std::vector<std::string> v, const std::string& remove)
{
    auto it = std::find(v.begin(), v.end(), remove);
    if (it != v.end()) v.erase(it);
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

class DestIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!fixtureAvailable()) {
            GTEST_SKIP() << "test_dest_integrity.pdf fixture not available";
        }
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

// ---- helper that runs a page-delete case end to end -------------------------
// deleteAtIndex: the physical page index to delete.
void runDeleteCase(std::shared_ptr<MuPdfContext> ctx, const char* tag, int deleteAtIndex)
{
    auto src = workCopy((std::string("di_") + tag + "_in.pdf").c_str());
    auto doc = Document::open(ctx, src);
    ASSERT_EQ(doc->pageCount(), 5);

    // Record intended targets BEFORE the edit, resolved by content.
    auto bmBefore = bookmarkTitleToMarker(doc.get());
    int linkPage = findPageByMarker(doc.get(), "PDFEPAGEMARK0");
    ASSERT_GE(linkPage, 0);
    AnnotationManager mgr(ctx);
    auto linksBefore = linkTargetMarkers(doc.get(), mgr, linkPage);

    ASSERT_FALSE(bmBefore.empty()) << "fixture must carry resolvable bookmarks";

    std::string deletedMarker = markerOnPage(doc.get(), deleteAtIndex);
    ASSERT_FALSE(deletedMarker.empty());

    // Apply the delete through the real product command, then full-rewrite save
    // + reopen from disk.
    {
        pdfe::commands::DeletePageCommand cmd(doc.get(), deleteAtIndex);
        cmd.execute();
    }
    ASSERT_EQ(doc->pageCount(), 4);

    auto out = workDir() / (std::string("di_") + tag + "_out.pdf");
    std::filesystem::remove(out);
    doc->saveAs(out);
    doc.reset();

    auto doc2 = Document::open(ctx, out);
    ASSERT_EQ(doc2->pageCount(), 4);

    // The deleted marker must no longer exist anywhere in the file.
    EXPECT_EQ(findPageByMarker(doc2.get(), deletedMarker), -1)
        << "deleted page content should be gone";

    // Every surviving bookmark must resolve to the SAME marker; a bookmark that
    // targeted the deleted page must NOT resolve to any (wrong) page.
    auto bmAfter = bookmarkTitleToMarker(doc2.get());
    for (const auto& [title, marker] : bmBefore) {
        if (marker == deletedMarker) {
            auto it = bmAfter.find(title);
            EXPECT_TRUE(it == bmAfter.end())
                << "bookmark '" << title << "' targeted the deleted page and must "
                << "not dangle to a page (now resolves to " << (it==bmAfter.end()?std::string("<gone>"):it->second) << ")";
        } else {
            auto it = bmAfter.find(title);
            ASSERT_TRUE(it != bmAfter.end())
                << "surviving bookmark '" << title << "' lost its destination";
            EXPECT_EQ(it->second, marker)
                << "bookmark '" << title << "' now points at the WRONG page";
        }
    }
    // Belt-and-braces: no surviving bookmark resolves to the deleted marker.
    for (const auto& [title, marker] : bmAfter) {
        EXPECT_NE(marker, deletedMarker)
            << "bookmark '" << title << "' dangles to deleted content";
    }

    // Links: those targeting a surviving page keep their marker; a link to the
    // deleted page is stripped. (When page 0 itself was deleted the link page is
    // gone entirely — nothing to check.)
    int linkPage2 = findPageByMarker(doc2.get(), "PDFEPAGEMARK0");
    if (linkPage2 >= 0) {
        AnnotationManager mgr2(ctx);
        auto linksAfter = linkTargetMarkers(doc2.get(), mgr2, linkPage2);
        auto expected = without(linksBefore, deletedMarker);
        EXPECT_EQ(linksAfter, expected)
            << "link GoTo destinations were not remapped/stripped correctly";
        for (const auto& mk : linksAfter) {
            EXPECT_NE(mk, deletedMarker) << "link dangles to deleted page";
        }
    }
}

TEST_F(DestIntegrityTest, DeleteMiddlePreservesDestinations)
{
    runDeleteCase(ctx, "del_mid", 2);  // deletes PDFEPAGEMARK2 (bookmark + link target)
}

TEST_F(DestIntegrityTest, DeleteFirstPreservesDestinations)
{
    runDeleteCase(ctx, "del_first", 0);  // deletes PDFEPAGEMARK0 (the links page)
}

TEST_F(DestIntegrityTest, DeleteLastPreservesDestinations)
{
    runDeleteCase(ctx, "del_last", 4);  // deletes PDFEPAGEMARK4 (unreferenced)
}

TEST_F(DestIntegrityTest, ReorderPreservesDestinations)
{
    auto src = workCopy("di_reorder_in.pdf");
    auto doc = Document::open(ctx, src);
    ASSERT_EQ(doc->pageCount(), 5);

    auto bmBefore = bookmarkTitleToMarker(doc.get());
    int linkPage = findPageByMarker(doc.get(), "PDFEPAGEMARK0");
    ASSERT_GE(linkPage, 0);
    AnnotationManager mgr(ctx);
    auto linksBefore = linkTargetMarkers(doc.get(), mgr, linkPage);
    ASSERT_FALSE(bmBefore.empty());
    ASSERT_FALSE(linksBefore.empty());

    // ---- GitHub #195: THE CONTROL THAT WAS MISSING -------------------------
    //
    // Everything else in this test is a PRESERVATION oracle: "each bookmark
    // still resolves to the marker it resolved to before". That property holds
    // just as well for a reorderPages() that did nothing at all -- pages
    // unchanged, so every destination still resolves to its original marker --
    // which is why this test survived an identity-order mutant.
    //
    // The fix is to assert the operation HAPPENED, against the INPUT rather
    // than against another output: `kOrder` below is the same vector handed to
    // reorderPages, and `markersBefore` is the physical page order read off the
    // document before the edit. Page k afterwards must carry the marker that
    // was on page kOrder[k]. A no-op reorder fails that at k = 0, before any
    // preservation assertion is reached.
    static const std::vector<int> kOrder = {4, 3, 2, 1, 0};
    std::vector<std::string> markersBefore;
    for (int i = 0; i < 5; ++i) markersBefore.push_back(markerOnPage(doc.get(), i));
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(markersBefore[i].empty())
            << "page " << i << " carries no content marker, so the positional "
               "control below would compare empty against empty";
    }
    {
        std::set<std::string> distinct(markersBefore.begin(), markersBefore.end());
        ASSERT_EQ(distinct.size(), markersBefore.size())
            << "the fixture's page markers are not unique, so a permutation "
               "cannot be distinguished from a no-op by content";
    }

    // Full reversal — the strongest permutation. reorderPages routes through
    // pdf_rearrange_pages, remapping every destination.
    doc->reorderPages(kOrder);
    ASSERT_EQ(doc->pageCount(), 5);

    // The control, in memory, before the save can launder anything.
    for (int k = 0; k < 5; ++k) {
        EXPECT_EQ(markerOnPage(doc.get(), k), markersBefore[kOrder[k]])
            << "in-memory page " << k << " must carry the marker that was on "
            << "page " << kOrder[k] << " before the reorder. If every page "
               "still carries its original marker, reorderPages() did nothing "
               "and the destination assertions below are vacuous.";
    }

    auto out = workDir() / "di_reorder_out.pdf";
    std::filesystem::remove(out);
    doc->saveAs(out);
    doc.reset();

    auto doc2 = Document::open(ctx, out);
    ASSERT_EQ(doc2->pageCount(), 5);

    // GitHub #195, again after the round trip: the reordered page order must
    // have SURVIVED the save, not just existed in memory. Same input-anchored
    // comparison, so an identity-order mutant reddens here too.
    for (int k = 0; k < 5; ++k) {
        EXPECT_EQ(markerOnPage(doc2.get(), k), markersBefore[kOrder[k]])
            << "saved+reopened page " << k << " must carry the marker that was "
            << "on page " << kOrder[k] << " before the reorder";
    }

    // No page was deleted: EVERY bookmark must still resolve to its marker.
    auto bmAfter = bookmarkTitleToMarker(doc2.get());
    ASSERT_EQ(bmAfter.size(), bmBefore.size())
        << "reorder must not lose any bookmark destination";
    for (const auto& [title, marker] : bmBefore) {
        auto it = bmAfter.find(title);
        ASSERT_TRUE(it != bmAfter.end()) << "bookmark '" << title << "' vanished";
        EXPECT_EQ(it->second, marker)
            << "bookmark '" << title << "' points at the WRONG page after reorder";
    }

    int linkPage2 = findPageByMarker(doc2.get(), "PDFEPAGEMARK0");
    ASSERT_GE(linkPage2, 0);
    AnnotationManager mgr2(ctx);
    auto linksAfter = linkTargetMarkers(doc2.get(), mgr2, linkPage2);
    std::sort(linksBefore.begin(), linksBefore.end());
    EXPECT_EQ(linksAfter, linksBefore)
        << "link GoTo destinations point at the WRONG pages after reorder";
}

TEST_F(DestIntegrityTest, MoveCommandRemapsDestinations)
{
    auto src = workCopy("di_move_in.pdf");
    auto doc = Document::open(ctx, src);
    ASSERT_EQ(doc->pageCount(), 5);

    auto bmBefore = bookmarkTitleToMarker(doc.get());
    int linkPage = findPageByMarker(doc.get(), "PDFEPAGEMARK0");
    ASSERT_GE(linkPage, 0);
    AnnotationManager mgr(ctx);
    auto linksBefore = linkTargetMarkers(doc.get(), mgr, linkPage);
    ASSERT_FALSE(linksBefore.empty());

    // GitHub #195, sibling instance found by the sweep the issue asked for.
    // This test is the same shape as ReorderPreservesDestinations was: every
    // assertion below is "the destination still resolves to the marker it
    // resolved to before", and PDF destinations are page-OBJECT references, so
    // a MovePageCommand that did nothing satisfies all of them. Same remedy:
    // a positional control against the INPUT.
    std::vector<std::string> markersBefore;
    for (int i = 0; i < 5; ++i) markersBefore.push_back(markerOnPage(doc.get(), i));
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(markersBefore[i].empty()) << "page " << i << " has no marker";
    }
    {
        std::set<std::string> distinct(markersBefore.begin(), markersBefore.end());
        ASSERT_EQ(distinct.size(), markersBefore.size())
            << "fixture markers are not unique; a move cannot be distinguished "
               "from a no-op by content";
    }

    // Move the links page (index 0) elsewhere via the real product command.
    {
        pdfe::commands::MovePageCommand cmd(doc.get(), 0, 3);
        cmd.execute();
    }
    ASSERT_EQ(doc->pageCount(), 5);

    // MovePageCommand's own documented semantics (buildMovePermutation,
    // src/core/PageCommands.cpp:183): "moving forward the moved page's final
    // position is to-1 (removal shifts the tail down)". So MovePageCommand(0, 3)
    // lands the page at index 2, not 3, and the resulting page at k holds what
    // was page kMoveOrder[k]. This expectation is derived from that rule, not
    // read back off the product -- a mis-stated rule fails here.
    static const std::vector<int> kMoveOrder = {1, 2, 0, 3, 4};
    for (int k = 0; k < 5; ++k) {
        EXPECT_EQ(markerOnPage(doc.get(), k), markersBefore[kMoveOrder[k]])
            << "in-memory page " << k << " must carry the marker that was on "
            << "page " << kMoveOrder[k] << " before MovePageCommand(0, 3). If "
               "every page still carries its original marker the move did "
               "nothing and everything below is vacuous.";
    }

    auto out = workDir() / "di_move_out.pdf";
    std::filesystem::remove(out);
    doc->saveAs(out);
    doc.reset();

    auto doc2 = Document::open(ctx, out);
    ASSERT_EQ(doc2->pageCount(), 5);

    for (int k = 0; k < 5; ++k) {
        EXPECT_EQ(markerOnPage(doc2.get(), k), markersBefore[kMoveOrder[k]])
            << "saved+reopened page " << k << " must carry the marker that was "
            << "on page " << kMoveOrder[k] << " before the move";
    }

    auto bmAfter = bookmarkTitleToMarker(doc2.get());
    ASSERT_EQ(bmAfter.size(), bmBefore.size());
    for (const auto& [title, marker] : bmBefore) {
        auto it = bmAfter.find(title);
        ASSERT_TRUE(it != bmAfter.end()) << "bookmark '" << title << "' vanished after move";
        EXPECT_EQ(it->second, marker)
            << "bookmark '" << title << "' points at the WRONG page after move";
    }

    int linkPage2 = findPageByMarker(doc2.get(), "PDFEPAGEMARK0");
    ASSERT_GE(linkPage2, 0);
    AnnotationManager mgr2(ctx);
    auto linksAfter = linkTargetMarkers(doc2.get(), mgr2, linkPage2);
    std::sort(linksBefore.begin(), linksBefore.end());
    EXPECT_EQ(linksAfter, linksBefore)
        << "link GoTo destinations dangle/mis-resolve after MovePageCommand";
}
