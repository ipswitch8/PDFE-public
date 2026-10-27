#pragma once
#include <QImage>
#include <chrono>
#include "TileId.h"

namespace pdfe::rendering {

enum class TileState {
    Empty,
    Rendering,
    Ready,
    Stale
};

struct Tile {
    TileId id;
    QImage image;
    TileState state = TileState::Empty;
    std::chrono::steady_clock::time_point timestamp;
};

constexpr int TILE_SIZE = 512;

} // namespace pdfe::rendering
