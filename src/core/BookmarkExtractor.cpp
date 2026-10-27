#include "BookmarkExtractor.h"
#include "Document.h"
#include "MuPdfContext.h"
#include "Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::core {

class BookmarkExtractor::Impl {
public:
    std::shared_ptr<MuPdfContext> context;

    explicit Impl(std::shared_ptr<MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    std::vector<std::shared_ptr<BookmarkNode>> extract(Document* doc);

private:
    std::shared_ptr<BookmarkNode> processOutline(fz_context* ctx, fz_outline* outline);
    BookmarkDest parseDestination(fz_context* ctx, fz_outline* outline);
};

BookmarkExtractor::BookmarkExtractor(std::shared_ptr<MuPdfContext> context)
    : pImpl(std::make_unique<Impl>(std::move(context)))
{
}

BookmarkExtractor::~BookmarkExtractor() = default;

std::vector<std::shared_ptr<BookmarkNode>> BookmarkExtractor::extract(Document* doc)
{
    return pImpl->extract(doc);
}

std::vector<std::shared_ptr<BookmarkNode>> BookmarkExtractor::Impl::extract(Document* doc)
{
    if (!doc) {
        throw PdfeException("Document is null");
    }

    std::vector<std::shared_ptr<BookmarkNode>> result;

    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) {
        return result; // No document, return empty
    }

    fz_outline* outline = nullptr;

    try {
        outline = context->executeReturn<fz_outline*>(
            [fzDoc](fz_context* ctx) -> fz_outline* {
                return fz_load_outline(ctx, fzDoc);
            }
        );

        if (!outline) {
            return result; // No bookmarks in this document
        }

        // Process the outline tree
        context->execute([this, &result, outline](fz_context* ctx) {
            fz_outline* current = outline;
            while (current) {
                auto node = processOutline(ctx, current);
                if (node) {
                    result.push_back(node);
                }
                current = current->next;
            }
        });

        // Drop the outline
        context->execute([outline](fz_context* ctx) {
            fz_drop_outline(ctx, outline);
        });

    } catch (...) {
        // Clean up outline on exception
        if (outline) {
            try {
                context->execute([outline](fz_context* ctx) {
                    fz_drop_outline(ctx, outline);
                });
            } catch (...) {
                // Ignore cleanup errors
            }
        }
        throw;
    }

    return result;
}

std::shared_ptr<BookmarkNode> BookmarkExtractor::Impl::processOutline(
    fz_context* ctx, fz_outline* outline)
{
    if (!outline) {
        return nullptr;
    }

    auto node = std::make_shared<BookmarkNode>();

    // Extract title
    if (outline->title) {
        node->title = outline->title;
    }

    // Parse destination
    node->destination = parseDestination(ctx, outline);

    // Track open state
    node->isOpen = (outline->is_open != 0);

    // Process children recursively
    fz_outline* child = outline->down;
    while (child) {
        auto childNode = processOutline(ctx, child);
        if (childNode) {
            node->children.push_back(childNode);
        }
        child = child->next;
    }

    return node;
}

BookmarkDest BookmarkExtractor::Impl::parseDestination(
    fz_context* ctx, fz_outline* outline)
{
    BookmarkDest dest;

    if (!outline) {
        return dest;
    }

    // Check for URI
    if (outline->uri) {
        std::string uri = outline->uri;

        // Check if it's an internal page reference (format: "#page=N")
        if (uri.starts_with("#page=")) {
            try {
                int pageNum = std::stoi(uri.substr(6));
                dest.type = BookmarkDestType::Page;
                dest.pageIndex = pageNum - 1; // Convert 1-based to 0-based
            } catch (...) {
                dest.type = BookmarkDestType::URI;
                dest.uri = uri;
            }
        } else if (uri.starts_with("http://") || uri.starts_with("https://") ||
                   uri.starts_with("mailto:") || uri.starts_with("file:")) {
            // External URI
            dest.type = BookmarkDestType::URI;
            dest.uri = uri;
        } else {
            // Could be a named destination or other internal link
            dest.type = BookmarkDestType::NamedDest;
            dest.namedDest = uri;
        }
    }

    // If we have a page structure, use that
    if (outline->page.page >= 0) {
        dest.type = BookmarkDestType::Page;
        dest.pageIndex = outline->page.page;
        dest.x = outline->x;
        dest.y = outline->y;
    }

    return dest;
}

} // namespace pdfe::core
