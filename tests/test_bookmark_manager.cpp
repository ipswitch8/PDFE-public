// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include <functional>
#include "core/BookmarkManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>

using namespace pdfe::core;
using namespace pdfe::commands;

class BookmarkManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        pdfPath = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, pdfPath);
        mgr = std::make_unique<BookmarkManager>(ctx);
        mgr->load(doc.get());
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path pdfPath;
    std::unique_ptr<Document> doc;
    std::unique_ptr<BookmarkManager> mgr;
};

TEST_F(BookmarkManagerTest, LoadProducesTree) {
    // A3-19/A3-41 (over-tolerant-threshold): `EXPECT_GE(tree.size(), 0u)` is
    // always true for an unsigned size. The real, fixture-independent invariant
    // is that the tree the manager exposes and the count it reports describe the
    // SAME structure: totalBookmarkCount() must equal the number of nodes
    // reachable from the roots.
    const auto& tree = mgr->tree();
    std::function<int(const std::shared_ptr<BookmarkNode>&)> countNodes =
        [&](const std::shared_ptr<BookmarkNode>& n) -> int {
            if (!n) return 0;
            int total = 1;
            for (const auto& kid : n->children) total += countNodes(kid);
            return total;
        };
    int reachable = 0;
    for (const auto& root : tree) {
        ASSERT_NE(root, nullptr) << "the tree must not expose a null root";
        reachable += countNodes(root);
    }
    EXPECT_EQ(mgr->totalBookmarkCount(), reachable)
        << "totalBookmarkCount() disagrees with the tree it is counting";
}

TEST_F(BookmarkManagerTest, CreateAtRootAppends) {
    int before = mgr->totalBookmarkCount();
    BookmarkDest dest;
    dest.type = BookmarkDestType::Page;
    dest.pageIndex = 0;
    auto path = mgr->createBookmark({}, "Test Bookmark", dest);
    EXPECT_EQ(path.size(), 1u);
    EXPECT_EQ(mgr->totalBookmarkCount(), before + 1);

    auto node = mgr->nodeAt(path);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->title, "Test Bookmark");
    EXPECT_EQ(node->destination.pageIndex, 0);
}

TEST_F(BookmarkManagerTest, CreateChildBookmark) {
    BookmarkDest dest;
    dest.type = BookmarkDestType::Page;
    dest.pageIndex = 0;
    auto rootPath = mgr->createBookmark({}, "Root", dest);

    dest.pageIndex = 1;
    auto childPath = mgr->createBookmark(rootPath, "Child", dest);

    EXPECT_EQ(childPath.size(), 2u);
    EXPECT_EQ(childPath[0], rootPath[0]);
    EXPECT_EQ(childPath[1], 0);

    auto rootNode = mgr->nodeAt(rootPath);
    ASSERT_NE(rootNode, nullptr);
    EXPECT_EQ(rootNode->children.size(), 1u);
    EXPECT_EQ(rootNode->children[0]->title, "Child");
}

TEST_F(BookmarkManagerTest, RenameBookmark) {
    BookmarkDest dest;
    auto path = mgr->createBookmark({}, "Original", dest);
    mgr->renameBookmark(path, "Renamed");
    auto node = mgr->nodeAt(path);
    EXPECT_EQ(node->title, "Renamed");
}

TEST_F(BookmarkManagerTest, DeleteBookmark) {
    BookmarkDest dest;
    auto path = mgr->createBookmark({}, "ToDelete", dest);
    int before = mgr->totalBookmarkCount();
    mgr->deleteBookmark(path);
    EXPECT_EQ(mgr->totalBookmarkCount(), before - 1);
}

TEST_F(BookmarkManagerTest, MoveUpDown) {
    BookmarkDest dest;
    auto p1 = mgr->createBookmark({}, "First", dest);
    auto p2 = mgr->createBookmark({}, "Second", dest);

    // Move second up
    auto newP = mgr->moveBookmarkUp(p2);
    EXPECT_EQ(newP.back(), p2.back() - 1);

    // Check order
    auto first = mgr->nodeAt({0});
    auto second = mgr->nodeAt({1});
    EXPECT_EQ(first->title, "Second");
    EXPECT_EQ(second->title, "First");
}

TEST_F(BookmarkManagerTest, IndentOutdent) {
    BookmarkDest dest;
    auto p1 = mgr->createBookmark({}, "First", dest);
    auto p2 = mgr->createBookmark({}, "Second", dest);

    // Indent second — becomes child of first
    auto indented = mgr->indentBookmark(p2);
    EXPECT_EQ(indented.size(), 2u);
    EXPECT_EQ(indented[0], 0);

    auto first = mgr->nodeAt({0});
    EXPECT_EQ(first->children.size(), 1u);
    EXPECT_EQ(first->children[0]->title, "Second");

    // Outdent back out
    auto outdented = mgr->outdentBookmark(indented);
    EXPECT_EQ(outdented.size(), 1u);
    auto rootCheck = mgr->nodeAt(outdented);
    EXPECT_EQ(rootCheck->title, "Second");
}

TEST_F(BookmarkManagerTest, SetDestination) {
    BookmarkDest dest;
    dest.type = BookmarkDestType::Page;
    dest.pageIndex = 0;
    auto path = mgr->createBookmark({}, "Test", dest);

    BookmarkDest newDest;
    newDest.type = BookmarkDestType::URI;
    newDest.uri = "https://example.com";
    mgr->setDestination(path, newDest);

    auto node = mgr->nodeAt(path);
    EXPECT_EQ(node->destination.type, BookmarkDestType::URI);
    EXPECT_EQ(node->destination.uri, "https://example.com");
}

// ========== Undo Command Tests ==========

TEST_F(BookmarkManagerTest, CreateCommandUndoable) {
    UndoManager undoMgr;
    int before = mgr->totalBookmarkCount();

    BookmarkDest dest;
    dest.type = BookmarkDestType::Page;
    dest.pageIndex = 0;
    auto cmd = std::make_unique<CreateBookmarkCommand>(mgr.get(), BookmarkPath{}, "New", dest);
    undoMgr.execute(std::move(cmd));
    EXPECT_EQ(mgr->totalBookmarkCount(), before + 1);

    undoMgr.undo();
    EXPECT_EQ(mgr->totalBookmarkCount(), before);
}

TEST_F(BookmarkManagerTest, RenameCommandUndoable) {
    BookmarkDest dest;
    auto path = mgr->createBookmark({}, "Original", dest);

    UndoManager undoMgr;
    auto cmd = std::make_unique<RenameBookmarkCommand>(mgr.get(), path, "Renamed");
    undoMgr.execute(std::move(cmd));
    EXPECT_EQ(mgr->nodeAt(path)->title, "Renamed");

    undoMgr.undo();
    EXPECT_EQ(mgr->nodeAt(path)->title, "Original");
}

TEST_F(BookmarkManagerTest, MoveCommandUndoable) {
    BookmarkDest dest;
    auto p1 = mgr->createBookmark({}, "First", dest);
    auto p2 = mgr->createBookmark({}, "Second", dest);

    UndoManager undoMgr;
    auto cmd = std::make_unique<MoveBookmarkCommand>(
        mgr.get(), p2, MoveBookmarkCommand::Direction::Up);
    undoMgr.execute(std::move(cmd));

    // After up: First and Second swapped
    EXPECT_EQ(mgr->nodeAt({0})->title, "Second");

    undoMgr.undo();
    // After undo: original order
    EXPECT_EQ(mgr->nodeAt({0})->title, "First");
}

// Pipeline G phase-2: round-trip bookmarks through commit + saveAs +
// reopen. Verifies that BookmarkPanel::commitToDocument() (now wired
// into MainWindow::onSaveFile/onSaveAsFile) actually flushes the
// in-memory tree to /Outlines so edits survive a save/reload cycle.
TEST_F(BookmarkManagerTest, CommitSurvivesSaveReload) {
    // Build a bookmark tree on the in-memory manager.
    BookmarkDest pageDest;
    pageDest.type = BookmarkDestType::Page;
    pageDest.pageIndex = 0;
    mgr->createBookmark({}, "Chapter 1", pageDest);

    pageDest.pageIndex = 1;
    auto ch2 = mgr->createBookmark({}, "Chapter 2", pageDest);

    pageDest.pageIndex = 2;
    mgr->createBookmark(ch2, "Section 2.1", pageDest);

    // Flush to the document and saveAs to a temp path.
    auto tmpDir = std::filesystem::temp_directory_path();
    auto outPath = tmpDir / "pipeline_g_phase2_roundtrip.pdf";
    std::filesystem::remove(outPath);

    mgr->commit(doc.get());
    doc->saveAs(outPath);

    // Reopen the saved file and read its outline.
    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);

    BookmarkManager mgr2(ctx);
    mgr2.load(reopened.get());
    auto reloaded = mgr2.tree();

    ASSERT_EQ(reloaded.size(), 2u) << "expected 2 root bookmarks after reload";
    EXPECT_EQ(reloaded[0]->title, "Chapter 1");
    EXPECT_EQ(reloaded[1]->title, "Chapter 2");
    ASSERT_EQ(reloaded[1]->children.size(), 1u)
        << "expected child bookmark under Chapter 2 to round-trip";
    EXPECT_EQ(reloaded[1]->children[0]->title, "Section 2.1");

    reopened.reset();
    std::filesystem::remove(outPath);
}

// Pipeline G phase-2: rename also persists across save/reload. This
// exercises the rename mutation path through commit() — distinct from
// the create-only path covered by CommitSurvivesSaveReload.
TEST_F(BookmarkManagerTest, RenameSurvivesSaveReload) {
    BookmarkDest pageDest;
    pageDest.type = BookmarkDestType::Page;
    pageDest.pageIndex = 0;
    auto path = mgr->createBookmark({}, "Original Title", pageDest);
    mgr->renameBookmark(path, "Renamed Title");

    auto tmpDir = std::filesystem::temp_directory_path();
    auto outPath = tmpDir / "pipeline_g_phase2_rename_roundtrip.pdf";
    std::filesystem::remove(outPath);

    mgr->commit(doc.get());
    doc->saveAs(outPath);

    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    BookmarkManager mgr2(ctx);
    mgr2.load(reopened.get());
    auto reloaded = mgr2.tree();
    ASSERT_GE(reloaded.size(), 1u);
    EXPECT_EQ(reloaded[0]->title, "Renamed Title")
        << "rename should survive save/reload";

    reopened.reset();
    std::filesystem::remove(outPath);
}

// ========== BookmarkPath Tests ==========

TEST(BookmarkPathTest, EmptyIsRoot) {
    BookmarkPath p;
    EXPECT_TRUE(p.empty());
}

TEST(BookmarkDestTest, DefaultIsUnknown) {
    BookmarkDest d;
    EXPECT_EQ(d.type, BookmarkDestType::Unknown);
    EXPECT_EQ(d.pageIndex, -1);
    EXPECT_TRUE(d.uri.empty());
}

TEST(BookmarkNodeTest, DefaultConstructs) {
    BookmarkNode n;
    EXPECT_TRUE(n.title.empty());
    EXPECT_TRUE(n.children.empty());
    EXPECT_FALSE(n.isOpen);
}
