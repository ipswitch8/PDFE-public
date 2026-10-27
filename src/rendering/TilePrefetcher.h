#pragma once
#include "TileId.h"
#include <mutex>
#include <vector>

namespace pdfe::rendering {

class TileCache;

class TilePrefetcher {
public:
    explicit TilePrefetcher(TileCache* cache = nullptr);

    void setCache(TileCache* cache);
    void setRingSize(int size);
    int ringSize() const;

    /// Given visible tiles and total page count, compute tiles to prefetch.
    /// Returns tiles in the 1-ring (or n-ring) around visible tiles that are NOT already cached.
    std::vector<TileId> computePrefetchTiles(const std::vector<TileId>& visibleTiles, int pageCount) const;

    /// Get the last computed prefetch tile list (for testing/debugging).
    /// Returns BY VALUE (a copy taken while holding the lock): handing back a
    /// reference would let it dangle if prefetchAround() reallocates the backing
    /// vector on another thread. See prefetchMutex_.
    std::vector<TileId> lastPrefetchedTiles() const;

    /// Compute and store prefetch tiles.
    void prefetchAround(const std::vector<TileId>& visibleTiles, int pageCount);

private:
    TileCache* cache_ = nullptr;
    int ringSize_ = 1;
    // Guards lastPrefetched_. prefetchAround() writes it and lastPrefetchedTiles()
    // reads it; production runs both on the UI thread, but nothing structurally
    // prevents a background prefetch, so the shared member is locked either way.
    mutable std::mutex prefetchMutex_;
    std::vector<TileId> lastPrefetched_;
};

} // namespace pdfe::rendering
