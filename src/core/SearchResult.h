#pragma once

#include <vector>
#include <QRectF>
#include <string>

namespace pdfe::core {

/// A single search hit on a page.
struct SearchHit {
    int pageIndex;                      ///< Zero-based page index
    std::vector<QRectF> quads;          ///< Highlight rectangles in PDF coordinates
    size_t matchStart;                  ///< Offset in page plain text
    size_t matchEnd;                    ///< End offset in page plain text
    std::string context;                ///< ~50 chars surrounding match for display
};

/// Result of a search operation containing all hits and metadata.
struct SearchResult {
    std::string query;                  ///< Original search query
    std::vector<SearchHit> hits;        ///< All matching hits across pages
    bool caseSensitive;                 ///< Whether search was case-sensitive

    /// Get total number of matches.
    int totalMatches() const { return static_cast<int>(hits.size()); }
};

} // namespace pdfe::core
