#pragma once
#include <cstdint>
#include <cstddef>

namespace pdfe::rendering {

struct CacheStats {
    uint64_t hitCount = 0;
    uint64_t missCount = 0;
    uint64_t evictionCount = 0;
    uint64_t putCount = 0;
    size_t currentMemoryBytes = 0;
    size_t maxMemoryBytes = 0;
    size_t tileCount = 0;

    double hitRatio() const {
        uint64_t total = hitCount + missCount;
        if (total == 0) return 0.0;
        return static_cast<double>(hitCount) / static_cast<double>(total);
    }

    double memoryUtilization() const {
        if (maxMemoryBytes == 0) return 0.0;
        return static_cast<double>(currentMemoryBytes) / static_cast<double>(maxMemoryBytes);
    }

    void reset() {
        hitCount = missCount = evictionCount = putCount = 0;
    }
};

} // namespace pdfe::rendering
