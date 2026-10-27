#include "BookmarkManager.h"
#include "Document.h"
#include "MuPdfContext.h"
#include "Error.h"
#include "BookmarkExtractor.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <sstream>

namespace pdfe::core {

namespace {
// Build URI from a BookmarkDest (matches the format BookmarkExtractor parses)
std::string destToUri(const BookmarkDest& dest) {
    if (dest.type == BookmarkDestType::URI) {
        return dest.uri;
    }
    if (dest.type == BookmarkDestType::NamedDest) {
        return dest.namedDest;
    }
    if (dest.type == BookmarkDestType::Page) {
        std::ostringstream oss;
        oss << "#page=" << (dest.pageIndex + 1);
        return oss.str();
    }
    return "";
}
}

class BookmarkManager::Impl {
public:
    std::shared_ptr<MuPdfContext> context;
    std::vector<std::shared_ptr<BookmarkNode>> tree;

    explicit Impl(std::shared_ptr<MuPdfContext> ctx) : context(std::move(ctx)) {}

    // Navigate to a node by path, returning reference to the containing vector
    // and the index within it. Returns true on success.
    bool locate(const BookmarkPath& path,
                std::vector<std::shared_ptr<BookmarkNode>>*& parentVec,
                int& index) {
        if (path.empty()) return false;
        auto* vec = &tree;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            int idx = path[i];
            if (idx < 0 || idx >= static_cast<int>(vec->size())) return false;
            vec = &((*vec)[idx]->children);
        }
        int last = path.back();
        if (last < 0 || last >= static_cast<int>(vec->size())) return false;
        parentVec = vec;
        index = last;
        return true;
    }

    std::shared_ptr<BookmarkNode> nodeAt(const BookmarkPath& path) const {
        const auto* vec = &tree;
        std::shared_ptr<BookmarkNode> result;
        for (size_t i = 0; i < path.size(); ++i) {
            int idx = path[i];
            if (idx < 0 || idx >= static_cast<int>(vec->size())) return nullptr;
            result = (*vec)[idx];
            vec = &result->children;
        }
        return result;
    }

    // Recursively write a tree to the outline iterator.
    void writeNode(fz_context* ctx, fz_outline_iterator* iter,
                   const std::shared_ptr<BookmarkNode>& node) {
        fz_outline_item item{};
        // fz_outline_item has non-const char* fields — copy to tmp storage
        std::string title = node->title;
        std::string uri = destToUri(node->destination);
        item.title = title.empty() ? nullptr : title.data();
        item.uri = uri.empty() ? nullptr : uri.data();
        item.is_open = node->isOpen ? 1 : 0;
        item.flags = 0;
        item.r = item.g = item.b = 0.0f;

        fz_outline_iterator_insert(ctx, iter, &item);

        if (!node->children.empty()) {
            // Descend into first child position
            fz_outline_iterator_prev(ctx, iter);  // Move to just-inserted item
            fz_outline_iterator_down(ctx, iter);  // Go into children
            for (const auto& child : node->children) {
                writeNode(ctx, iter, child);
            }
            fz_outline_iterator_up(ctx, iter);    // Back out
            fz_outline_iterator_next(ctx, iter);  // Skip past the item we just processed
        }
    }

    int countRecursive(const std::vector<std::shared_ptr<BookmarkNode>>& nodes) const {
        int count = 0;
        for (const auto& n : nodes) {
            count++;
            count += countRecursive(n->children);
        }
        return count;
    }
};

BookmarkManager::BookmarkManager(std::shared_ptr<MuPdfContext> context)
    : pImpl(std::make_unique<Impl>(std::move(context)))
{
}

BookmarkManager::~BookmarkManager() = default;

std::vector<std::shared_ptr<BookmarkNode>> BookmarkManager::load(Document* doc) {
    BookmarkExtractor extractor(pImpl->context);
    pImpl->tree = extractor.extract(doc);
    return pImpl->tree;
}

const std::vector<std::shared_ptr<BookmarkNode>>& BookmarkManager::tree() const {
    return pImpl->tree;
}

BookmarkPath BookmarkManager::createBookmark(
    const BookmarkPath& parentPath, const std::string& title,
    const BookmarkDest& destination)
{
    auto node = std::make_shared<BookmarkNode>();
    node->title = title;
    node->destination = destination;

    BookmarkPath newPath = parentPath;

    if (parentPath.empty()) {
        pImpl->tree.push_back(node);
        newPath.push_back(static_cast<int>(pImpl->tree.size()) - 1);
    } else {
        auto parent = pImpl->nodeAt(parentPath);
        if (!parent) {
            pImpl->tree.push_back(node);
            return {static_cast<int>(pImpl->tree.size()) - 1};
        }
        parent->children.push_back(node);
        newPath.push_back(static_cast<int>(parent->children.size()) - 1);
    }
    return newPath;
}

void BookmarkManager::renameBookmark(const BookmarkPath& path, const std::string& newTitle) {
    auto node = pImpl->nodeAt(path);
    if (node) node->title = newTitle;
}

void BookmarkManager::deleteBookmark(const BookmarkPath& path) {
    std::vector<std::shared_ptr<BookmarkNode>>* vec = nullptr;
    int idx = 0;
    if (pImpl->locate(path, vec, idx)) {
        vec->erase(vec->begin() + idx);
    }
}

void BookmarkManager::setDestination(const BookmarkPath& path, const BookmarkDest& destination) {
    auto node = pImpl->nodeAt(path);
    if (node) node->destination = destination;
}

BookmarkPath BookmarkManager::moveBookmarkUp(const BookmarkPath& path) {
    std::vector<std::shared_ptr<BookmarkNode>>* vec = nullptr;
    int idx = 0;
    if (!pImpl->locate(path, vec, idx) || idx == 0) return path;
    std::swap((*vec)[idx], (*vec)[idx - 1]);
    BookmarkPath newPath = path;
    newPath.back() = idx - 1;
    return newPath;
}

BookmarkPath BookmarkManager::moveBookmarkDown(const BookmarkPath& path) {
    std::vector<std::shared_ptr<BookmarkNode>>* vec = nullptr;
    int idx = 0;
    if (!pImpl->locate(path, vec, idx)) return path;
    if (idx + 1 >= static_cast<int>(vec->size())) return path;
    std::swap((*vec)[idx], (*vec)[idx + 1]);
    BookmarkPath newPath = path;
    newPath.back() = idx + 1;
    return newPath;
}

BookmarkPath BookmarkManager::indentBookmark(const BookmarkPath& path) {
    std::vector<std::shared_ptr<BookmarkNode>>* vec = nullptr;
    int idx = 0;
    if (!pImpl->locate(path, vec, idx) || idx == 0) return path;
    // Move into previous sibling's children as last child
    auto node = (*vec)[idx];
    const auto& prevSibling = (*vec)[idx - 1];
    prevSibling->children.push_back(node);
    vec->erase(vec->begin() + idx);

    BookmarkPath newPath = path;
    newPath.back() = idx - 1;
    newPath.push_back(static_cast<int>(prevSibling->children.size()) - 1);
    return newPath;
}

BookmarkPath BookmarkManager::outdentBookmark(const BookmarkPath& path) {
    if (path.size() < 2) return path;  // Already at root

    std::vector<std::shared_ptr<BookmarkNode>>* vec = nullptr;
    int idx = 0;
    if (!pImpl->locate(path, vec, idx)) return path;

    // Find parent's containing vec
    BookmarkPath parentPath = path;
    parentPath.pop_back();
    int parentIdx = parentPath.back();
    parentPath.pop_back();

    std::vector<std::shared_ptr<BookmarkNode>>* grandparentVec;
    if (parentPath.empty()) {
        grandparentVec = &pImpl->tree;
    } else {
        auto grandparent = pImpl->nodeAt(parentPath);
        if (!grandparent) return path;
        grandparentVec = &grandparent->children;
    }

    auto node = (*vec)[idx];
    vec->erase(vec->begin() + idx);

    grandparentVec->insert(grandparentVec->begin() + parentIdx + 1, node);

    BookmarkPath newPath = parentPath;
    newPath.push_back(parentIdx + 1);
    return newPath;
}

void BookmarkManager::commit(Document* doc) {
    if (!doc) return;
    // Committing the outline rebuilds /Outlines, orphaning the previous tree;
    // force a full-rewrite save (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();
    pImpl->context->execute([this, doc](fz_context* ctx) {
        fz_document* fzDoc = doc->fzDocument();
        if (!fzDoc) return;

        fz_outline_iterator* iter = fz_new_outline_iterator(ctx, fzDoc);
        if (!iter) return;

        // Drain any existing outline entries. fz_outline_iterator_item
        // returns NULL on an empty outline (or when positioned past the
        // last entry); calling _delete in that state throws ("Can't
        // delete a non-existent outline item"). Guard with item().
        while (fz_outline_iterator_item(ctx, iter) != nullptr) {
            int r = fz_outline_iterator_delete(ctx, iter);
            if (r < 0) break;
        }

        // Write fresh tree
        for (const auto& root : pImpl->tree) {
            pImpl->writeNode(ctx, iter, root);
        }

        fz_drop_outline_iterator(ctx, iter);
    });
}

std::shared_ptr<BookmarkNode> BookmarkManager::nodeAt(const BookmarkPath& path) const {
    return pImpl->nodeAt(path);
}

int BookmarkManager::totalBookmarkCount() const {
    return pImpl->countRecursive(pImpl->tree);
}

// ---------- Commands ----------

CreateBookmarkCommand::CreateBookmarkCommand(
    BookmarkManager* mgr, const BookmarkPath& parentPath,
    const std::string& title, const BookmarkDest& dest)
    : mgr_(mgr), parentPath_(parentPath), title_(title), dest_(dest) {}

void CreateBookmarkCommand::execute() {
    createdPath_ = mgr_->createBookmark(parentPath_, title_, dest_);
}

void CreateBookmarkCommand::undo() {
    if (!createdPath_.empty()) {
        mgr_->deleteBookmark(createdPath_);
    }
}

RenameBookmarkCommand::RenameBookmarkCommand(
    BookmarkManager* mgr, const BookmarkPath& path, const std::string& newTitle)
    : mgr_(mgr), path_(path), newTitle_(newTitle)
{
    auto node = mgr_->nodeAt(path_);
    if (node) oldTitle_ = node->title;
}

void RenameBookmarkCommand::execute() {
    mgr_->renameBookmark(path_, newTitle_);
}

void RenameBookmarkCommand::undo() {
    mgr_->renameBookmark(path_, oldTitle_);
}

DeleteBookmarkCommand::DeleteBookmarkCommand(
    BookmarkManager* mgr, const BookmarkPath& path)
    : mgr_(mgr), path_(path)
{
    auto node = mgr_->nodeAt(path_);
    if (node) deletedSubtree_ = node;
}

void DeleteBookmarkCommand::execute() {
    mgr_->deleteBookmark(path_);
}

void DeleteBookmarkCommand::undo() {
    if (!deletedSubtree_) return;
    // Reinsert at the same parent path (best-effort — use original path)
    BookmarkPath parentPath = path_;
    if (!parentPath.empty()) parentPath.pop_back();
    BookmarkDest dest = deletedSubtree_->destination;
    // Reinserts the subtree's root; the returned path is unused
    // because children of the deleted subtree are not restored in
    // this minimal undo.
    (void)mgr_->createBookmark(parentPath, deletedSubtree_->title, dest);
}

MoveBookmarkCommand::MoveBookmarkCommand(
    BookmarkManager* mgr, const BookmarkPath& path, Direction dir)
    : mgr_(mgr), path_(path), direction_(dir) {}

void MoveBookmarkCommand::execute() {
    switch (direction_) {
    case Direction::Up:      newPath_ = mgr_->moveBookmarkUp(path_); break;
    case Direction::Down:    newPath_ = mgr_->moveBookmarkDown(path_); break;
    case Direction::Indent:  newPath_ = mgr_->indentBookmark(path_); break;
    case Direction::Outdent: newPath_ = mgr_->outdentBookmark(path_); break;
    }
}

void MoveBookmarkCommand::undo() {
    if (newPath_.empty()) return;
    switch (direction_) {
    case Direction::Up:      mgr_->moveBookmarkDown(newPath_); break;
    case Direction::Down:    mgr_->moveBookmarkUp(newPath_); break;
    case Direction::Indent:  mgr_->outdentBookmark(newPath_); break;
    case Direction::Outdent: mgr_->indentBookmark(newPath_); break;
    }
}

std::string MoveBookmarkCommand::description() const {
    switch (direction_) {
    case Direction::Up:      return "Move Bookmark Up";
    case Direction::Down:    return "Move Bookmark Down";
    case Direction::Indent:  return "Indent Bookmark";
    case Direction::Outdent: return "Outdent Bookmark";
    }
    return "Move Bookmark";
}

} // namespace pdfe::core
