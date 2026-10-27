#pragma once

#include "fonts/FontMetricProfile.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace pdfe::fonts {

class FontAnalysisCache {
public:
    // Open or create cache at the specified path.
    // Pass empty path for in-memory-only cache (for testing).
    explicit FontAnalysisCache(const std::filesystem::path& dbPath = {});
    ~FontAnalysisCache();

    // Look up a cached profile by PostScript name.
    std::optional<FontMetricProfile> get(const std::string& postScriptName);

    // Store a profile (both in-memory LRU and SQLite).
    void put(const FontMetricProfile& profile);

    // Check if a profile exists.
    bool contains(const std::string& postScriptName) const;

    // Clear all cached entries.
    void clear();

    // Cache statistics.
    size_t size() const;     // Total entries in SQLite
    size_t memSize() const;  // Entries in LRU

    /// True if this cache is backed by a FILE rather than by ":memory:".
    ///
    /// SEC-103: every construction site in the product passes no path, so
    /// this is false throughout the shipping application. It is exposed so
    /// that fact is ASSERTABLE — an absence nothing checks is one commit
    /// deep, and the commit that introduces an on-disk cache should have to
    /// answer the ACL question deliberately rather than inherit whatever the
    /// containing directory grants. When a path IS supplied, the file is
    /// created with an owner-only DACL and a wider pre-existing file is
    /// refused, falling back to in-memory. See
    /// pdfe::core::createOwnerOnlyFile.
    bool isOnDisk() const noexcept;

    // Non-copyable
    FontAnalysisCache(const FontAnalysisCache&) = delete;
    FontAnalysisCache& operator=(const FontAnalysisCache&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::fonts
