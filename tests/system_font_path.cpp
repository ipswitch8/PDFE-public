// -*- coding: utf-8 -*-
// See system_font_path.h (GitHub #390).
#include "system_font_path.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fontconfig/fontconfig.h>
#include <mutex>
#endif

namespace pdfe_test {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#if !defined(_WIN32)

// How a Windows font file is matched on POSIX. `pattern` is a fontconfig
// pattern string. A match is accepted when its family is in `families`, or,
// for the script fonts where any covering font will do, when it covers `lang`.
struct Substitute {
    const char* windowsFile;
    const char* pattern;
    std::vector<const char*> families;  // accepted family names (exact, case-insensitive)
    const char* lang;                   // or: accepted if the font covers this language
};

const std::vector<Substitute>& table() {
    static const std::vector<Substitute> t = {
        // Metric-compatible substitutes: same advance widths as the original.
        {"arial.ttf",          "Arial",                {"Arial", "Liberation Sans", "Arimo"}, nullptr},
        {"arialbd.ttf",        "Arial:bold",           {"Arial", "Liberation Sans", "Arimo"}, nullptr},
        {"times.ttf",          "Times New Roman",      {"Times New Roman", "Liberation Serif", "Tinos"}, nullptr},
        {"timesnewroman.ttf",  "Times New Roman",      {"Times New Roman", "Liberation Serif", "Tinos"}, nullptr},
        {"cour.ttf",           "Courier New",          {"Courier New", "Liberation Mono", "Cousine"}, nullptr},
        {"couriernew.ttf",     "Courier New",          {"Courier New", "Liberation Mono", "Cousine"}, nullptr},
        {"calibri.ttf",        "Calibri",              {"Calibri", "Carlito"}, nullptr},
        // Closest-in-kind substitutes: same class of face, not identical metrics.
        {"consola.ttf",        "Consolas:spacing=mono", {"Consolas", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono"}, nullptr},
        {"tahoma.ttf",         "Tahoma",               {"Tahoma", "DejaVu Sans", "Noto Sans"}, nullptr},
        {"segoeui.ttf",        "Segoe UI",             {"Segoe UI", "Noto Sans", "DejaVu Sans", "Open Sans"}, nullptr},
        {"symbol.ttf",         "Symbol",               {"Symbol", "Standard Symbols PS"}, nullptr},
        // Not a Windows file: the Latin-primary MULTI-SCRIPT sans a Linux
        // desktop ships (Latin, Greek, Cyrillic, Arabic, Hebrew ...). Tests
        // that need "a real font whose script set comes from its glyphs" use
        // it where Windows uses Arial, whose Microsoft build carries Arabic and
        // Hebrew; Liberation Sans, Arial's metric twin, does not.
        {"dejavusans.ttf",     "DejaVu Sans",          {"DejaVu Sans"}, nullptr},
        // Wingdings has no free metric twin. The property the tests choosing it
        // rely on is "pictographs only, NO Latin letters", so a plain "x" is
        // unencodable in it. Noto Sans Symbols2 (TrueType, fonts-noto-core) has
        // that property; URW's Dingbats clone and Standard Symbols PS do NOT,
        // both map ASCII, which was measured, not assumed.
        {"wingding.ttf",       "Noto Sans Symbols2",   {"Noto Sans Symbols2"}, nullptr},
        // Webdings: a Symbol-cmap pictograph font (~230 glyphs) whose glyphs
        // sit in the Private Use Area (U+F020..U+F0FF). FontAwesome
        // (fonts-font-awesome) is the same shape: PUA pictographs only, no
        // ASCII letters, ~700 glyphs, and it maps U+F030, the code point
        // AutoMissingGlyphResolverFixtureTest uses as "encodable". Size
        // matters here: the resolver rasterises the whole embedded font into
        // its synthesis atlas, so the 2,270-glyph Noto Sans Symbols2 made the
        // keystroke timing test measure ten times the work Webdings is.
        {"webdings.ttf",       "FontAwesome",          {"FontAwesome"}, nullptr},
        // Script fonts: any installed face that covers the script.
        {"notonaskharabic-regular.ttf", "Noto Naskh Arabic", {"Noto Naskh Arabic"}, "ar"},
        {"scheherazade-bold.ttf", ":lang=ar:bold",     {"Scheherazade New", "Scheherazade"}, "ar"},
        {"davidlibre-regular.ttf", ":lang=he",         {"David Libre"}, "he"},
        {"davidclm-bold.otf",  ":lang=he:bold",        {"David CLM"}, "he"},
        {"msgothic.ttc",       ":lang=ja",             {"MS Gothic"}, "ja"},
        {"yugothm.ttc",        ":lang=ja",             {"Yu Gothic"}, "ja"},
        {"msjh.ttc",           ":lang=zh-tw",          {"Microsoft JhengHei"}, "zh-tw"},
        {"simsun.ttc",         ":lang=zh-cn",          {"SimSun"}, "zh-cn"},
        {"malgun.ttf",         ":lang=ko",             {"Malgun Gothic"}, "ko"},
        // webdings.ttf: no free face of the same kind is commonly installed, so
        // it is deliberately absent and resolves to nothing.
    };
    return t;
}

struct Resolved {
    std::string path;
    std::string family;
};

FcConfig* config() {
    // The vcpkg fontconfig is linked statically and would otherwise read its
    // own build-tree configuration. Prefer the host's, which carries the
    // distribution's font directories and metric-alias rules.
    static std::once_flag once;
    static FcConfig* cfg = nullptr;
    std::call_once(once, [] {
        cfg = FcConfigCreate();
        const char* sys = "/etc/fonts/fonts.conf";
        if (!(FcConfigParseAndLoad(cfg, reinterpret_cast<const FcChar8*>(sys), FcFalse) &&
              FcConfigBuildFonts(cfg))) {
            FcConfigDestroy(cfg);
            cfg = FcInitLoadConfigAndFonts();
        }
    });
    return cfg;
}

Resolved resolve(const std::string& windowsFile) {
    const std::string key = lower(windowsFile);
    const auto& t = table();
    const auto it = std::find_if(t.begin(), t.end(),
                                 [&](const Substitute& s) { return key == s.windowsFile; });
    if (it == t.end()) return {};

    FcConfig* cfg = config();
    if (!cfg) return {};
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(it->pattern));
    if (!pat) return {};
    FcConfigSubstitute(cfg, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcPattern* match = FcFontMatch(cfg, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return {};

    Resolved out;
    FcChar8* file = nullptr;
    FcChar8* family = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch) {
        const std::string fam = reinterpret_cast<const char*>(family);
        bool ok = std::any_of(it->families.begin(), it->families.end(),
                              [&](const char* f) { return lower(fam) == lower(f); });
        if (!ok && it->lang) {
            FcLangSet* ls = nullptr;
            ok = FcPatternGetLangSet(match, FC_LANG, 0, &ls) == FcResultMatch &&
                 FcLangSetHasLang(ls, reinterpret_cast<const FcChar8*>(it->lang)) == FcLangEqual;
        }
        if (ok) {
            out.path = reinterpret_cast<const char*>(file);
            out.family = fam;
        }
    }
    FcPatternDestroy(match);
    return out;
}

#endif  // !_WIN32

}  // namespace

std::string systemFontPath(const std::string& windowsFile) {
#if defined(_WIN32)
    return "C:\\Windows\\Fonts\\" + windowsFile;
#else
    const Resolved r = resolve(windowsFile);
    // A path that cannot exist, so an exists() check fails as it did before.
    return r.path.empty() ? "/nonexistent-font/" + windowsFile : r.path;
#endif
}

std::string systemFontFamily(const std::string& windowsFile) {
#if defined(_WIN32)
    (void)windowsFile;
    return {};
#else
    return resolve(windowsFile).family;
#endif
}

bool isMetricCompatibleFont(const std::string& path, const std::string& windowsStem)
{
    const std::string p = lower(path);
#if defined(_WIN32)
    return p.find(windowsStem) != std::string::npos;
#else
    std::vector<std::string> accepted;
    if (windowsStem == "arial")      accepted = {"arial", "liberationsans", "arimo", "nimbussans"};
    else if (windowsStem == "times") accepted = {"times", "liberationserif", "tinos", "nimbusroman"};
    else if (windowsStem == "cour")  accepted = {"cour", "liberationmono", "cousine", "nimbusmono"};
    else                             accepted = {windowsStem};
    for (const auto& a : accepted)
        if (p.find(a) != std::string::npos) return true;
    return false;
#endif
}

}  // namespace pdfe_test
