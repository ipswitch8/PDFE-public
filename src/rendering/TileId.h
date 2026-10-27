#pragma once
#include <cstddef>
#include <functional>

namespace pdfe::rendering {

struct TileId {
    int pageIndex = 0;
    int col = 0;
    int row = 0;
    float zoom = 1.0f;

    bool operator==(const TileId& other) const = default;
};

} // namespace pdfe::rendering

// Hash specialization
template <>
struct std::hash<pdfe::rendering::TileId> {
    std::size_t operator()(const pdfe::rendering::TileId& id) const noexcept
    {
        std::size_t h = std::hash<int>{}(id.pageIndex);
        h ^= std::hash<int>{}(id.col) << 1;
        h ^= std::hash<int>{}(id.row) << 2;
        // Quantize zoom to avoid floating point hash issues
        h ^= std::hash<int>{}(static_cast<int>(id.zoom * 10000)) << 3;
        return h;
    }
};
