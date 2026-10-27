#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::search {

/// A single ranked hit returned by TantivyIndex::search.
struct SearchHit {
    int         pageIndex;
    float       score;
    std::string snippet;  // short excerpt
};

/// RAII C++ wrapper around the Tantivy Rust bridge.
///
/// Single-threaded; callers must serialize access (the rest of PDFE holds
/// the document mutex while editing, which is sufficient). upsertPage /
/// deletePage are buffered in the underlying Tantivy writer — call commit()
/// to make changes visible to searches.
class TantivyIndex {
public:
    /// Open or create an index at `indexDir`. Throws std::runtime_error on
    /// failure (dir not writable, crate init failure, etc.).
    explicit TantivyIndex(const std::filesystem::path& indexDir);
    ~TantivyIndex();

    TantivyIndex(const TantivyIndex&) = delete;
    TantivyIndex& operator=(const TantivyIndex&) = delete;
    TantivyIndex(TantivyIndex&&) noexcept;
    TantivyIndex& operator=(TantivyIndex&&) noexcept;

    /// Replace the text for `pageIndex`. Existing entries for this page
    /// are deleted before the new one is added.
    bool upsertPage(int pageIndex, const std::string& text);

    /// Remove any entry for `pageIndex`.
    bool deletePage(int pageIndex);

    /// Commit buffered writes + refresh the reader so subsequent searches
    /// see the latest state.
    bool commit();

    /// Total documents currently visible.
    std::int64_t documentCount() const;

    /// Search using Tantivy QueryParser syntax:
    ///   plain words      → all terms (AND by default)
    ///   "quoted phrase"  → phrase match
    ///   prefix*          → prefix search
    ///   +must  -mustNot  → boolean operators
    ///   foo OR bar       → explicit disjunction
    std::vector<SearchHit> search(const std::string& query, int maxHits) const;

    /// True if the underlying Rust handle was opened successfully.
    bool valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::search
