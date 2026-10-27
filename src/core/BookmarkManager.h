#pragma once

#include "BookmarkNode.h"
#include "Command.h"
#include <memory>
#include <vector>
#include <string>

namespace pdfe::core {

class Document;
class MuPdfContext;

/// Indices into a bookmark tree. Empty path = root. Each element is the
/// child index at that level of the tree.
using BookmarkPath = std::vector<int>;

/// Writable bookmark management via MuPDF's outline iterator API.
/// Mutations are tree-rebuild based: operate on the in-memory tree,
/// then commit back to the PDF outline on save.
class BookmarkManager {
public:
    explicit BookmarkManager(std::shared_ptr<MuPdfContext> context);
    ~BookmarkManager();

    BookmarkManager(const BookmarkManager&) = delete;
    BookmarkManager& operator=(const BookmarkManager&) = delete;

    /// Load the outline tree from the document. Must be called before any
    /// mutations. Returns the root-level bookmarks.
    std::vector<std::shared_ptr<BookmarkNode>> load(Document* doc);

    /// Get the current in-memory tree (after load + any mutations).
    const std::vector<std::shared_ptr<BookmarkNode>>& tree() const;

    /// Create a new bookmark at the given path. Empty path appends at root.
    /// Path represents the parent; new node appended as last child.
    /// Returns the path to the newly-created node (parent + [last child index]).
    BookmarkPath createBookmark(
        const BookmarkPath& parentPath,
        const std::string& title,
        const BookmarkDest& destination
    );

    /// Rename the bookmark at the given path.
    void renameBookmark(const BookmarkPath& path, const std::string& newTitle);

    /// Delete the bookmark at the given path and all its descendants.
    void deleteBookmark(const BookmarkPath& path);

    /// Change the destination of the bookmark at the given path.
    void setDestination(const BookmarkPath& path, const BookmarkDest& destination);

    /// Move a bookmark up/down among its siblings. Returns new path.
    BookmarkPath moveBookmarkUp(const BookmarkPath& path);
    BookmarkPath moveBookmarkDown(const BookmarkPath& path);

    /// Indent (become last child of previous sibling) / outdent (become next
    /// sibling of parent). Returns new path, or original if operation invalid.
    BookmarkPath indentBookmark(const BookmarkPath& path);
    BookmarkPath outdentBookmark(const BookmarkPath& path);

    /// Commit the current in-memory tree to the PDF outline. Called on save.
    /// Uses MuPDF's outline iterator API.
    void commit(Document* doc);

    /// Retrieve a node by path (or nullptr if path is invalid).
    std::shared_ptr<BookmarkNode> nodeAt(const BookmarkPath& path) const;

    /// Count of bookmarks at all levels (for testing/diagnostics).
    int totalBookmarkCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

// ---------- Undo commands ----------

class CreateBookmarkCommand : public pdfe::commands::Command {
public:
    CreateBookmarkCommand(BookmarkManager* mgr, const BookmarkPath& parentPath,
                          const std::string& title, const BookmarkDest& dest);
    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override { return "Create Bookmark"; }
    const BookmarkPath& createdPath() const { return createdPath_; }

private:
    BookmarkManager* mgr_;
    BookmarkPath parentPath_;
    std::string title_;
    BookmarkDest dest_;
    BookmarkPath createdPath_;
};

class RenameBookmarkCommand : public pdfe::commands::Command {
public:
    RenameBookmarkCommand(BookmarkManager* mgr, const BookmarkPath& path,
                          const std::string& newTitle);
    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override { return "Rename Bookmark"; }

private:
    BookmarkManager* mgr_;
    BookmarkPath path_;
    std::string newTitle_;
    std::string oldTitle_;
};

class DeleteBookmarkCommand : public pdfe::commands::Command {
public:
    DeleteBookmarkCommand(BookmarkManager* mgr, const BookmarkPath& path);
    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override { return "Delete Bookmark"; }

private:
    BookmarkManager* mgr_;
    BookmarkPath path_;
    std::shared_ptr<BookmarkNode> deletedSubtree_;
};

class MoveBookmarkCommand : public pdfe::commands::Command {
public:
    enum class Direction { Up, Down, Indent, Outdent };

    MoveBookmarkCommand(BookmarkManager* mgr, const BookmarkPath& path, Direction dir);
    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    BookmarkManager* mgr_;
    BookmarkPath path_;
    BookmarkPath newPath_;
    Direction direction_;
};

} // namespace pdfe::core
