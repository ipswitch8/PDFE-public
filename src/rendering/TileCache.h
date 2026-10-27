#pragma once
#include "Tile.h"
#include "TileId.h"
#include "CacheStats.h"
#include <QImage>
#include <unordered_map>
#include <list>
#include <optional>
#include <mutex>
#include <cstddef>

namespace pdfe::rendering {

class TileCache {
public:
    explicit TileCache(size_t maxMemoryBytes = 512 * 1024 * 1024); // 512MB default

    std::optional<Tile> get(const TileId& id);
    void put(const TileId& id, const QImage& image);
    void invalidate(int pageIndex);
    void invalidateAll();
    void clear();

    size_t memoryUsage() const;
    size_t tileCount() const;
    void setMaxMemory(size_t bytes);

    CacheStats stats() const;
    void resetStats();
    bool contains(const TileId& id) const;

private:
    struct CacheEntry {
        TileId id;
        Tile tile;
        size_t memorySize;
    };

    using CacheList = std::list<CacheEntry>;
    using CacheMap = std::unordered_map<TileId, CacheList::iterator>;

    void evictLRU();
    void updateMemoryUsage(ptrdiff_t delta);

    mutable std::mutex m_mutex;
    CacheList m_lruList;
    CacheMap m_cacheMap;
    size_t m_currentMemory{0};
    size_t m_maxMemory;

    uint64_t m_hitCount{0};
    uint64_t m_missCount{0};
    uint64_t m_evictionCount{0};
    uint64_t m_putCount{0};
};

} // namespace pdfe::rendering
