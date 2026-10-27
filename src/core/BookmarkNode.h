#pragma once

#include <string>
#include <vector>
#include <memory>

namespace pdfe::core {

enum class BookmarkDestType {
    Page,
    URI,
    NamedDest,
    Unknown
};

struct BookmarkDest {
    BookmarkDestType type = BookmarkDestType::Unknown;
    int pageIndex = -1;
    std::string uri;
    std::string namedDest;
    float x = 0.0f;
    float y = 0.0f;
    float zoom = 0.0f;
};

struct BookmarkNode {
    std::string title;
    BookmarkDest destination;
    std::vector<std::shared_ptr<BookmarkNode>> children;
    bool isOpen = false;

    BookmarkNode() = default;
    explicit BookmarkNode(const std::string& t) : title(t) {}
};

} // namespace pdfe::core
