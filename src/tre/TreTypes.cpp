#include "TreTypes.h"

#include <algorithm>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// PixelRect
// ---------------------------------------------------------------------------

double PixelRect::width() const {
    return std::abs(x1 - x0);
}

double PixelRect::height() const {
    return std::abs(y1 - y0);
}

double PixelRect::area() const {
    return width() * height();
}

bool PixelRect::contains(PixelPoint pt) const {
    const double minX = std::min(x0, x1);
    const double maxX = std::max(x0, x1);
    const double minY = std::min(y0, y1);
    const double maxY = std::max(y0, y1);
    return pt.x >= minX && pt.x <= maxX && pt.y >= minY && pt.y <= maxY;
}

bool PixelRect::intersects(const PixelRect& other) const {
    const double aMinX = std::min(x0, x1);
    const double aMaxX = std::max(x0, x1);
    const double aMinY = std::min(y0, y1);
    const double aMaxY = std::max(y0, y1);

    const double bMinX = std::min(other.x0, other.x1);
    const double bMaxX = std::max(other.x0, other.x1);
    const double bMinY = std::min(other.y0, other.y1);
    const double bMaxY = std::max(other.y0, other.y1);

    return aMinX < bMaxX && aMaxX > bMinX && aMinY < bMaxY && aMaxY > bMinY;
}

PixelRect PixelRect::intersection(const PixelRect& other) const {
    const double aMinX = std::min(x0, x1);
    const double aMaxX = std::max(x0, x1);
    const double aMinY = std::min(y0, y1);
    const double aMaxY = std::max(y0, y1);

    const double bMinX = std::min(other.x0, other.x1);
    const double bMaxX = std::max(other.x0, other.x1);
    const double bMinY = std::min(other.y0, other.y1);
    const double bMaxY = std::max(other.y0, other.y1);

    const double ix0 = std::max(aMinX, bMinX);
    const double iy0 = std::max(aMinY, bMinY);
    const double ix1 = std::min(aMaxX, bMaxX);
    const double iy1 = std::min(aMaxY, bMaxY);

    // If no intersection, return a zero-area rect
    if (ix0 >= ix1 || iy0 >= iy1) {
        return PixelRect{0, 0, 0, 0};
    }

    return PixelRect{ix0, iy0, ix1, iy1};
}

PixelPoint PixelRect::center() const {
    return PixelPoint{(x0 + x1) / 2.0, (y0 + y1) / 2.0};
}

// ---------------------------------------------------------------------------
// PdfRect
// ---------------------------------------------------------------------------

double PdfRect::width() const {
    return std::abs(x1 - x0);
}

double PdfRect::height() const {
    return std::abs(y1 - y0);
}

double PdfRect::area() const {
    return width() * height();
}

bool PdfRect::contains(PdfPoint pt) const {
    const double minX = std::min(x0, x1);
    const double maxX = std::max(x0, x1);
    const double minY = std::min(y0, y1);
    const double maxY = std::max(y0, y1);
    return pt.x >= minX && pt.x <= maxX && pt.y >= minY && pt.y <= maxY;
}

bool PdfRect::intersects(const PdfRect& other) const {
    const double aMinX = std::min(x0, x1);
    const double aMaxX = std::max(x0, x1);
    const double aMinY = std::min(y0, y1);
    const double aMaxY = std::max(y0, y1);

    const double bMinX = std::min(other.x0, other.x1);
    const double bMaxX = std::max(other.x0, other.x1);
    const double bMinY = std::min(other.y0, other.y1);
    const double bMaxY = std::max(other.y0, other.y1);

    return aMinX < bMaxX && aMaxX > bMinX && aMinY < bMaxY && aMaxY > bMinY;
}

PdfRect PdfRect::intersection(const PdfRect& other) const {
    const double aMinX = std::min(x0, x1);
    const double aMaxX = std::max(x0, x1);
    const double aMinY = std::min(y0, y1);
    const double aMaxY = std::max(y0, y1);

    const double bMinX = std::min(other.x0, other.x1);
    const double bMaxX = std::max(other.x0, other.x1);
    const double bMinY = std::min(other.y0, other.y1);
    const double bMaxY = std::max(other.y0, other.y1);

    const double ix0 = std::max(aMinX, bMinX);
    const double iy0 = std::max(aMinY, bMinY);
    const double ix1 = std::min(aMaxX, bMaxX);
    const double iy1 = std::min(aMaxY, bMaxY);

    // If no intersection, return a zero-area rect
    if (ix0 >= ix1 || iy0 >= iy1) {
        return PdfRect{0, 0, 0, 0};
    }

    return PdfRect{ix0, iy0, ix1, iy1};
}

PdfPoint PdfRect::center() const {
    return PdfPoint{(x0 + x1) / 2.0, (y0 + y1) / 2.0};
}

// ---------------------------------------------------------------------------
// RegionTextData
// ---------------------------------------------------------------------------

float RegionTextData::meanWordConfidence() const {
    int totalConfidence = 0;
    int wordCount = 0;

    for (const auto& line : lines) {
        for (const auto& word : line.words) {
            totalConfidence += word.confidence;
            ++wordCount;
        }
    }

    if (wordCount == 0) {
        return 0.0f;
    }

    return static_cast<float>(totalConfidence) / static_cast<float>(wordCount);
}

float RegionTextData::minWordConfidence() const {
    int minConf = 100;
    bool hasWords = false;

    for (const auto& line : lines) {
        for (const auto& word : line.words) {
            hasWords = true;
            if (word.confidence < minConf) {
                minConf = word.confidence;
            }
        }
    }

    if (!hasWords) {
        return 100.0f;
    }

    return static_cast<float>(minConf);
}

bool RegionTextData::containsOnlyText() const {
    return std::all_of(lines.begin(), lines.end(), [](const auto& line) {
        return std::all_of(line.words.begin(), line.words.end(),
            [](const auto& word) { return word.confidence > 0; });
    });
}

// ---------------------------------------------------------------------------
// UTF-8 Utilities
// ---------------------------------------------------------------------------

std::u32string utf8ToU32(const std::string& utf8) {
    std::u32string result;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        int extra = 0;

        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }

        for (int j = 0; j < extra; ++j) {
            ++i;
            if (i >= utf8.size()) break;
            unsigned char cont = static_cast<unsigned char>(utf8[i]);
            if ((cont & 0xC0) != 0x80) break;
            cp = (cp << 6) | (cont & 0x3F);
        }

        result.push_back(static_cast<char32_t>(cp));
        ++i;
    }
    return result;
}

std::vector<uint32_t> utf8ToCodepoints(const std::string& utf8) {
    std::vector<uint32_t> result;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        int extra = 0;

        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }

        for (int j = 0; j < extra; ++j) {
            ++i;
            if (i >= utf8.size()) break;
            unsigned char cont = static_cast<unsigned char>(utf8[i]);
            if ((cont & 0xC0) != 0x80) break;
            cp = (cp << 6) | (cont & 0x3F);
        }

        result.push_back(cp);
        ++i;
    }
    return result;
}

} // namespace pdfe::tre
