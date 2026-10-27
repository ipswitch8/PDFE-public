#include "TileCache.h"
#include <algorithm>

namespace pdfe::rendering {

TileCache::TileCache(size_t maxMemoryBytes)
    : m_maxMemory(maxMemoryBytes)
{
}

std::optional<Tile> TileCache::get(const TileId& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cacheMap.find(id);
    if (it == m_cacheMap.end()) {
        ++m_missCount;
        return std::nullopt;
    }

    ++m_hitCount;

    // Move to front of LRU list (most recently used)
    m_lruList.splice(m_lruList.begin(), m_lruList, it->second);

    return it->second->tile;
}

void TileCache::put(const TileId& id, const QImage& image)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ++m_putCount;

    // Check if tile already exists
    auto it = m_cacheMap.find(id);
    if (it != m_cacheMap.end()) {
        // Update existing entry
        size_t oldSize = it->second->memorySize;
        size_t newSize = image.sizeInBytes();

        it->second->tile.image = image;
        it->second->tile.state = TileState::Ready;
        it->second->tile.timestamp = std::chrono::steady_clock::now();
        it->second->memorySize = newSize;

        // Update memory usage
        updateMemoryUsage(static_cast<ptrdiff_t>(newSize) - static_cast<ptrdiff_t>(oldSize));

        // Move to front of LRU list
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    } else {
        // Create new entry
        size_t tileSize = image.sizeInBytes();

        CacheEntry entry;
        entry.id = id;
        entry.tile.id = id;
        entry.tile.image = image;
        entry.tile.state = TileState::Ready;
        entry.tile.timestamp = std::chrono::steady_clock::now();
        entry.memorySize = tileSize;

        // Add to front of LRU list
        m_lruList.push_front(entry);
        m_cacheMap[id] = m_lruList.begin();

        updateMemoryUsage(tileSize);
    }

    // Evict LRU entries if over memory limit
    while (m_currentMemory > m_maxMemory && !m_lruList.empty()) {
        evictLRU();
    }
}

void TileCache::invalidate(int pageIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_lruList.begin();
    while (it != m_lruList.end()) {
        if (it->id.pageIndex == pageIndex) {
            updateMemoryUsage(-static_cast<ptrdiff_t>(it->memorySize));
            m_cacheMap.erase(it->id);
            it = m_lruList.erase(it);
        } else {
            ++it;
        }
    }
}

void TileCache::invalidateAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lruList.clear();
    m_cacheMap.clear();
    m_currentMemory = 0;
}

void TileCache::clear()
{
    invalidateAll();
}

size_t TileCache::memoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentMemory;
}

size_t TileCache::tileCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lruList.size();
}

void TileCache::setMaxMemory(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxMemory = bytes;

    // Evict LRU entries if over new memory limit
    while (m_currentMemory > m_maxMemory && !m_lruList.empty()) {
        evictLRU();
    }
}

bool TileCache::contains(const TileId& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cacheMap.find(id) != m_cacheMap.end();
}

void TileCache::evictLRU()
{
    // Remove least recently used (back of list)
    if (m_lruList.empty()) {
        return;
    }

    ++m_evictionCount;

    auto& lru = m_lruList.back();
    updateMemoryUsage(-static_cast<ptrdiff_t>(lru.memorySize));
    m_cacheMap.erase(lru.id);
    m_lruList.pop_back();
}

void TileCache::updateMemoryUsage(ptrdiff_t delta)
{
    if (delta >= 0) {
        m_currentMemory += static_cast<size_t>(delta);
    } else {
        size_t absDelta = static_cast<size_t>(-delta);
        if (absDelta > m_currentMemory) {
            m_currentMemory = 0;
        } else {
            m_currentMemory -= absDelta;
        }
    }
}

CacheStats TileCache::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CacheStats s;
    s.hitCount = m_hitCount;
    s.missCount = m_missCount;
    s.evictionCount = m_evictionCount;
    s.putCount = m_putCount;
    s.currentMemoryBytes = m_currentMemory;
    s.maxMemoryBytes = m_maxMemory;
    s.tileCount = m_lruList.size();
    return s;
}

void TileCache::resetStats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hitCount = m_missCount = m_evictionCount = m_putCount = 0;
}

} // namespace pdfe::rendering
