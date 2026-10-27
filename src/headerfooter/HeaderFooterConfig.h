#pragma once

#include <string>
#include <vector>

namespace pdfe::headerfooter {

enum class PageRangeMode {
    All,
    OddPagesOnly,
    EvenPagesOnly,
    Explicit
};

struct HeaderFooterConfig {
    // Six slots. Empty = no text in that slot. Tokens {date},{author},
    // {page},{pages},{filename} are resolved per-page at apply time.
    std::string leftHeader;
    std::string centerHeader;
    std::string rightHeader;
    std::string leftFooter;
    std::string centerFooter;
    std::string rightFooter;

    float marginPt = 36.0f;    // 0.5" — distance from page edge to text baseline
    float fontSize = 10.0f;
    std::string fontName = "Helvetica";

    std::string author;        // resolves {author}
    std::string filename;      // resolves {filename}; typically basename of saved path

    PageRangeMode rangeMode = PageRangeMode::All;
    // Used only when rangeMode == Explicit. 0-based page indices.
    std::vector<int> explicitPages;
};

} // namespace pdfe::headerfooter
