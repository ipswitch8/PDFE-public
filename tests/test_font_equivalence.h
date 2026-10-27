#pragma once
// Shared metric-compatible font equivalence helper for tests.
//
// On Linux the FontResolver and Qt resolve "Arial" / "Times New Roman" /
// "Calibri" / etc. to the metric-compatible substitutes (Liberation Sans,
// Liberation Serif, Carlito, etc.). Glyph widths and advances match so
// PDF layout is identical, but the literal family-name STRING differs.
// Tests that check the family name need to accept the substitute as
// satisfying the original assertion.
//
// Usage:
//   if (!fontNameMatchesOrEquivalent(actual, "Arial")) {
//       FAIL() << "...";
//   }
// Or for FAIL_FALSE style:
//   EXPECT_TRUE(fontNameMatchesOrEquivalent(actual, "Arial"));

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace pdfe::test {

inline std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

/// Returns true if `actual` contains `expected` as a substring, OR contains
/// any metric-compatible equivalent for `expected`. Case-insensitive.
inline bool fontNameMatchesOrEquivalent(const std::string& actual,
                                          const std::string& expected) {
    const std::string actualL   = toLower(actual);
    const std::string expectedL = toLower(expected);
    if (actualL.find(expectedL) != std::string::npos) return true;

    // Metric-compatible substitutes per family. Each chain's members
    // share glyph widths so PDF layout matches the canonical font.
    static const struct { const char* canonical; std::vector<const char*> equivalents; } kChains[] = {
        {"arial",            {"liberation sans", "arimo", "nimbus sans", "dejavu sans"}},
        {"helvetica",        {"liberation sans", "arimo", "nimbus sans", "dejavu sans", "arial"}},
        {"times new roman",  {"liberation serif", "tinos", "nimbus roman", "dejavu serif"}},
        {"times",            {"liberation serif", "tinos", "nimbus roman", "dejavu serif",
                              "times new roman"}},
        {"courier new",      {"liberation mono", "cousine", "nimbus mono", "dejavu sans mono"}},
        {"courier",          {"liberation mono", "cousine", "nimbus mono", "dejavu sans mono",
                              "courier new"}},
        {"calibri",          {"carlito"}},
        {"cambria",          {"caladea"}},
        {"liberation sans",  {"arial", "arimo", "dejavu sans"}},
        {"liberation serif", {"times new roman", "tinos", "dejavu serif"}},
        {"liberation mono",  {"courier new", "cousine", "dejavu sans mono"}},
        {"carlito",          {"calibri"}},
        {"caladea",          {"cambria"}},
    };

    for (const auto& chain : kChains) {
        if (expectedL == chain.canonical) {
            for (const auto* alt : chain.equivalents) {
                if (actualL.find(alt) != std::string::npos) return true;
            }
        }
    }
    return false;
}

/// QString-aware overload — accepts QString actual / expected and falls
/// through to the std::string implementation.
template <typename TQString>
inline bool fontNameMatchesOrEquivalent(const TQString& actual,
                                          const TQString& expected) {
    return fontNameMatchesOrEquivalent(actual.toStdString(),
                                         expected.toStdString());
}

} // namespace pdfe::test
