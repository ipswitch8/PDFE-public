#include "TilePrefetcher.h"
#include "TileCache.h"
#include <unordered_set>
#include <algorithm>

namespace pdfe::rendering {

TilePrefetcher::TilePrefetcher(TileCache* cache)
    : cache_(cache)
{
}

void TilePrefetcher::setCache(TileCache* cache)
{
    cache_ = cache;
}

void TilePrefetcher::setRingSize(int size)
{
    // Clamp to 1-3
    ringSize_ = std::clamp(size, 1, 3);
}

int TilePrefetcher::ringSize() const
{
    return ringSize_;
}

std::vector<TileId> TilePrefetcher::computePrefetchTiles(const std::vector<TileId>& visibleTiles, int pageCount) const
{
    // Use unordered_set for efficient deduplication and lookup
    std::unordered_set<TileId> visibleSet(visibleTiles.begin(), visibleTiles.end());
    std::unordered_set<TileId> prefetchSet;

    for (const auto& tile : visibleTiles) {
        // Generate tiles in the ring around this visible tile
        for (int dRow = -ringSize_; dRow <= ringSize_; ++dRow) {
            for (int dCol = -ringSize_; dCol <= ringSize_; ++dCol) {
                // Skip the center tile (it's already visible)
                if (dRow == 0 && dCol == 0) {
                    continue;
                }

                int newRow = tile.row + dRow;
                int newCol = tile.col + dCol;

                // Skip negative indices
                if (newRow < 0 || newCol < 0) {
                    continue;
                }

                TileId candidateId{tile.pageIndex, newCol, newRow, tile.zoom};

                // Skip if already visible
                if (visibleSet.find(candidateId) != visibleSet.end()) {
                    continue;
                }

                // Skip if already cached
                if (cache_ && cache_->contains(candidateId)) {
                    continue;
                }

                prefetchSet.insert(candidateId);
            }
        }

        // Also include same-col/row tiles on adjacent pages (page +/- 1)
        for (int dPage : {-1, 1}) {
            int newPageIndex = tile.pageIndex + dPage;

            // Skip if page is out of bounds
            if (newPageIndex < 0 || newPageIndex >= pageCount) {
                continue;
            }

            // Same column and row, different page
            TileId adjacentPageTile{newPageIndex, tile.col, tile.row, tile.zoom};

            // Skip if already visible
            if (visibleSet.find(adjacentPageTile) != visibleSet.end()) {
                continue;
            }

            // Skip if already cached
            if (cache_ && cache_->contains(adjacentPageTile)) {
                continue;
            }

            prefetchSet.insert(adjacentPageTile);
        }
    }

    // Convert set to vector
    return std::vector<TileId>(prefetchSet.begin(), prefetchSet.end());
}

std::vector<TileId> TilePrefetcher::lastPrefetchedTiles() const
{
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    return lastPrefetched_;
}

void TilePrefetcher::prefetchAround(const std::vector<TileId>& visibleTiles, int pageCount)
{
    // Compute OUTSIDE the lock (it only reads the cache, which self-synchronizes),
    // then take the lock only for the store so readers never observe a torn write.
    std::vector<TileId> computed = computePrefetchTiles(visibleTiles, pageCount);
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    lastPrefetched_ = std::move(computed);
}

} // namespace pdfe::rendering
