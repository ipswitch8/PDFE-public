#pragma once
#include <string>

namespace pdfe::core {

struct PageInfo {
    int index = 0;
    float widthPt = 0.0f;   // Width in points (72 points per inch)
    float heightPt = 0.0f;  // Height in points
    int rotation = 0;        // 0, 90, 180, or 270
    std::string label;       // Page label (e.g., "iii", "42")
};

} // namespace pdfe::core
