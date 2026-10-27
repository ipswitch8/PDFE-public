#include "BundledFonts.h"

#include <algorithm>

namespace pdfe::core {

std::vector<FontMetrics> BundledFonts::initFonts() {
    std::vector<FontMetrics> fonts;

    // DejaVu Sans (Regular)
    {
        FontMetrics fm;
        fm.postScriptName = "DejaVuSans";
        fm.familyName = "DejaVu Sans";
        fm.styleName = "Book";
        fm.weight = FontWeight::Normal;
        fm.style = FontStyle::Normal;
        fm.isSerif = false;
        fm.isMonospace = false;
        fm.filePath = ":/fonts/DejaVuSans.ttf";
        fm.unitsPerEm = 2048;
        fm.ascender = 1901;
        fm.descender = 483;
        fm.xHeight = 1120;
        fm.capHeight = 1493;
        fonts.push_back(fm);
    }

    // DejaVu Sans Bold
    {
        FontMetrics fm;
        fm.postScriptName = "DejaVuSans-Bold";
        fm.familyName = "DejaVu Sans";
        fm.styleName = "Bold";
        fm.weight = FontWeight::Bold;
        fm.style = FontStyle::Normal;
        fm.isSerif = false;
        fm.isMonospace = false;
        fm.filePath = ":/fonts/DejaVuSans-Bold.ttf";
        fm.unitsPerEm = 2048;
        fm.ascender = 1901;
        fm.descender = 483;
        fm.xHeight = 1120;
        fm.capHeight = 1493;
        fonts.push_back(fm);
    }

    // DejaVu Serif (Regular)
    {
        FontMetrics fm;
        fm.postScriptName = "DejaVuSerif";
        fm.familyName = "DejaVu Serif";
        fm.styleName = "Book";
        fm.weight = FontWeight::Normal;
        fm.style = FontStyle::Normal;
        fm.isSerif = true;
        fm.isMonospace = false;
        fm.filePath = ":/fonts/DejaVuSerif.ttf";
        fm.unitsPerEm = 2048;
        fm.ascender = 1901;
        fm.descender = 483;
        fm.xHeight = 1063;
        fm.capHeight = 1493;
        fonts.push_back(fm);
    }

    // DejaVu Serif Bold
    {
        FontMetrics fm;
        fm.postScriptName = "DejaVuSerif-Bold";
        fm.familyName = "DejaVu Serif";
        fm.styleName = "Bold";
        fm.weight = FontWeight::Bold;
        fm.style = FontStyle::Normal;
        fm.isSerif = true;
        fm.isMonospace = false;
        fm.filePath = ":/fonts/DejaVuSerif-Bold.ttf";
        fm.unitsPerEm = 2048;
        fm.ascender = 1901;
        fm.descender = 483;
        fm.xHeight = 1063;
        fm.capHeight = 1493;
        fonts.push_back(fm);
    }

    // DejaVu Sans Mono
    {
        FontMetrics fm;
        fm.postScriptName = "DejaVuSansMono";
        fm.familyName = "DejaVu Sans Mono";
        fm.styleName = "Book";
        fm.weight = FontWeight::Normal;
        fm.style = FontStyle::Normal;
        fm.isSerif = false;
        fm.isMonospace = true;
        fm.filePath = ":/fonts/DejaVuSansMono.ttf";
        fm.unitsPerEm = 2048;
        fm.ascender = 1901;
        fm.descender = 483;
        fm.xHeight = 1120;
        fm.capHeight = 1493;
        fonts.push_back(fm);
    }

    return fonts;
}

const std::vector<FontMetrics>& BundledFonts::getFonts() {
    static const std::vector<FontMetrics> fonts = initFonts();
    return fonts;
}

std::optional<FontMetrics> BundledFonts::findByFamily(const std::string& family) {
    std::string lowerFamily = family;
    std::transform(lowerFamily.begin(), lowerFamily.end(), lowerFamily.begin(), ::tolower);

    for (const auto& fm : getFonts()) {
        std::string lowerFm = fm.familyName;
        std::transform(lowerFm.begin(), lowerFm.end(), lowerFm.begin(), ::tolower);
        if (lowerFm == lowerFamily) {
            return fm;
        }
    }
    return std::nullopt;
}

FontMetrics BundledFonts::getDefaultSansSerif() {
    return getFonts()[0]; // DejaVu Sans Regular
}

FontMetrics BundledFonts::getDefaultSerif() {
    return getFonts()[2]; // DejaVu Serif Regular
}

FontMetrics BundledFonts::getDefaultMonospace() {
    return getFonts()[4]; // DejaVu Sans Mono
}

} // namespace pdfe::core
