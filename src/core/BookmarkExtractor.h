#pragma once

#include "BookmarkNode.h"
#include <memory>
#include <vector>

namespace pdfe::core {

class Document;
class MuPdfContext;

class BookmarkExtractor {
public:
    explicit BookmarkExtractor(std::shared_ptr<MuPdfContext> context);
    ~BookmarkExtractor();

    std::vector<std::shared_ptr<BookmarkNode>> extract(Document* doc);

    BookmarkExtractor(const BookmarkExtractor&) = delete;
    BookmarkExtractor& operator=(const BookmarkExtractor&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
