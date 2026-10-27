#pragma once

#include "SearchResult.h"
#include <memory>
#include <string>
#include <functional>
#include <atomic>

extern "C" {
struct fz_page;
}

namespace pdfe::core {

class Document;
class Page;

/// Flags for controlling search behavior.
enum class SearchFlags : unsigned {
    None = 0,
    CaseSensitive = 1 << 0,
    WholeWord = 1 << 1,
    UseRegex = 1 << 2
};

inline SearchFlags operator|(SearchFlags a, SearchFlags b) {
    return static_cast<SearchFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

inline bool hasFlag(SearchFlags flags, SearchFlags flag) {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
}

/// Engine for searching text within PDF documents using MuPDF's fz_search_page API.
///
/// The search engine finds text matches and returns their positions as quads (rectangles)
/// for highlighting. It supports both synchronous and asynchronous search with progress
/// callbacks and cancellation.
class SearchEngine {
public:
    /// Create a search engine for the given document.
    /// @param document The document to search (must remain valid during search operations)
    explicit SearchEngine(Document* document);
    ~SearchEngine();

    /// Perform a synchronous search across all pages.
    /// @param query The text to search for
    /// @param flags Search options (case sensitivity, whole word, etc.)
    /// @return Search results with all hits
    SearchResult search(const std::string& query, SearchFlags flags = SearchFlags::None);

    /// Phase 6 hybrid path: search only the specified pages (typically
    /// the candidate set returned by the Tantivy index). Pages are walked
    /// in the given order; duplicate indices are visited once in input
    /// order. Returns a SearchResult containing MuPDF quads for each hit.
    SearchResult searchSpecificPages(const std::string& query,
                                     const std::vector<int>& pageIndices,
                                     SearchFlags flags = SearchFlags::None);

    /// Perform an asynchronous search with progress and completion callbacks.
    /// @param query The text to search for
    /// @param flags Search options
    /// @param progressCb Called after each page: (currentPage, totalPages)
    /// @param completionCb Called when search completes with results
    void searchAsync(const std::string& query, SearchFlags flags,
                     std::function<void(int, int)> progressCb,
                     std::function<void(SearchResult)> completionCb);

    /// Cancel an ongoing asynchronous search.
    void cancelSearch();

    SearchEngine(const SearchEngine&) = delete;
    SearchEngine& operator=(const SearchEngine&) = delete;

    /// Access Page's internal fz_page handle (SearchEngine is a friend of Page).
    /// Exposed so helper classes (e.g. async tasks) can use it.
    static ::fz_page* getPageHandle(const Page* page);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
