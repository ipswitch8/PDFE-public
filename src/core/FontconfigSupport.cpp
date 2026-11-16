// -*- coding: utf-8 -*-
// See FontconfigSupport.h (GitHub #390).
#include "FontconfigSupport.h"

#if !defined(_WIN32)

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>

namespace pdfe::core::fontconfig {
namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// fontconfig is linked statically from vcpkg, whose compiled-in configuration
// points into the build machine's vcpkg tree. That tree does not exist on a
// user's machine (or inside the AppImage), so the HOST configuration is
// preferred: it carries the distribution's font directories and its metric
// alias rules. FONTCONFIG_FILE, if set, still wins, as it does for every other
// fontconfig client.
FcConfig* config()
{
    static std::once_flag once;
    static FcConfig* cfg = nullptr;
    std::call_once(once, [] {
        if (!std::getenv("FONTCONFIG_FILE")) {
            FcConfig* host = FcConfigCreate();
            const auto* path = reinterpret_cast<const FcChar8*>("/etc/fonts/fonts.conf");
            if (host && FcConfigParseAndLoad(host, path, FcFalse) && FcConfigBuildFonts(host)) {
                cfg = host;
                return;
            }
            if (host) FcConfigDestroy(host);
        }
        cfg = FcInitLoadConfigAndFonts();
    });
    return cfg;
}

std::string getString(FcPattern* p, const char* object)
{
    FcChar8* v = nullptr;
    return FcPatternGetString(p, object, 0, &v) == FcResultMatch
               ? std::string(reinterpret_cast<const char*>(v))
               : std::string();
}

int getInt(FcPattern* p, const char* object, int fallback)
{
    int v = 0;
    return FcPatternGetInteger(p, object, 0, &v) == FcResultMatch ? v : fallback;
}

FontWeight toWeight(int fc)
{
    // FcWeightToOpenType maps fontconfig's 0..215 scale onto OpenType 1..1000.
    const int ot = FcWeightToOpenType(fc);
    const int buckets[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};
    int best = 400;
    for (int b : buckets) {
        if (std::abs(b - ot) < std::abs(best - ot)) best = b;
    }
    return static_cast<FontWeight>(best);
}

FontStyle toStyle(int slant)
{
    if (slant >= FC_SLANT_OBLIQUE) return FontStyle::Oblique;
    if (slant >= FC_SLANT_ITALIC) return FontStyle::Italic;
    return FontStyle::Normal;
}

FontStretch toStretch(int width)
{
    if (width <= FC_WIDTH_ULTRACONDENSED) return FontStretch::UltraCondensed;
    if (width <= FC_WIDTH_EXTRACONDENSED) return FontStretch::ExtraCondensed;
    if (width <= FC_WIDTH_CONDENSED) return FontStretch::Condensed;
    if (width <= FC_WIDTH_SEMICONDENSED) return FontStretch::SemiCondensed;
    if (width <= FC_WIDTH_NORMAL) return FontStretch::Normal;
    if (width <= FC_WIDTH_SEMIEXPANDED) return FontStretch::SemiExpanded;
    if (width <= FC_WIDTH_EXPANDED) return FontStretch::Expanded;
    if (width <= FC_WIDTH_EXTRAEXPANDED) return FontStretch::ExtraExpanded;
    return FontStretch::UltraExpanded;
}

bool isGeneric(const std::string& lowered)
{
    return lowered == "sans-serif" || lowered == "serif" || lowered == "monospace" ||
           lowered == "sans" || lowered == "mono" || lowered == "system-ui" ||
           lowered == "cursive" || lowered == "fantasy" || lowered == "emoji" ||
           lowered == "math";
}

}  // namespace

std::vector<FontMetrics> listFonts()
{
    std::vector<FontMetrics> out;
    FcConfig* cfg = config();
    if (!cfg) return out;

    FcPattern* pat = FcPatternCreate();
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_POSTSCRIPT_NAME, FC_FILE,
                                       FC_WEIGHT, FC_SLANT, FC_WIDTH, FC_SPACING, FC_LANG,
                                       FC_FONTFORMAT,
                                       static_cast<char*>(nullptr));
    FcFontSet* fs = FcFontList(cfg, pat, os);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    if (!fs) return out;

    out.reserve(static_cast<size_t>(fs->nfont));
    for (int i = 0; i < fs->nfont; ++i) {
        FcPattern* f = fs->fonts[i];
        FontMetrics m;
        m.familyName = getString(f, FC_FAMILY);
        m.styleName = getString(f, FC_STYLE);
        m.postScriptName = getString(f, FC_POSTSCRIPT_NAME);
        m.filePath = getString(f, FC_FILE);
        if (m.familyName.empty() || m.filePath.empty()) continue;
        // TrueType and CFF/OpenType only -- the set DirectWrite enumerates on
        // Windows. Type 1 (.t1/.pfb), PCF and the other legacy formats carry
        // no OS/2 table, so every metric this enumeration feeds (PANOSE serif
        // class, x-height, weight class) is unknowable for them, and a Type 1
        // Nimbus Roman was being scored as a SANS candidate.
        const std::string format = getString(f, FC_FONTFORMAT);
        if (format != "TrueType" && format != "CFF") continue;
        m.weight = toWeight(getInt(f, FC_WEIGHT, FC_WEIGHT_REGULAR));
        m.style = toStyle(getInt(f, FC_SLANT, FC_SLANT_ROMAN));
        m.stretch = toStretch(getInt(f, FC_WIDTH, FC_WIDTH_NORMAL));
        m.isMonospace = getInt(f, FC_SPACING, FC_PROPORTIONAL) >= FC_MONO;
        // fontconfig's "en" orthography needs the full Latin letter set, so a
        // face that only carries ASCII alongside another script does not claim
        // it. A face with no language data at all is given the benefit of the
        // doubt rather than excluded.
        FcLangSet* ls = nullptr;
        if (FcPatternGetLangSet(f, FC_LANG, 0, &ls) == FcResultMatch && ls) {
            m.designedForLatin =
                FcLangSetHasLang(ls, reinterpret_cast<const FcChar8*>("en")) != FcLangDifferentLang;
        }
        // fontconfig has no serif property. The family name is the only
        // cheap signal: "serif" alone misses the classic serif families,
        // whose names never say so. A structural pre-filter only; the deep
        // score reads PANOSE from the file.
        const std::string fam = lower(m.familyName);
        static const char* const kSerifNames[] = {
            "serif", "roman", "times", "tinos", "georgia", "garamond", "bookman",
            "schoolbook", "palatino", "palladio", "baskerville", "caslon", "bodoni",
            "century", "cambria", "caladea", "charter", "gentium", "mincho", "song",
            "ming", "batang", "myungjo", "naskh"};
        bool serif = false;
        for (const char* n : kSerifNames) {
            if (fam.find(n) != std::string::npos) { serif = true; break; }
        }
        m.isSerif = serif && fam.find("sans") == std::string::npos;
        out.push_back(std::move(m));
    }
    FcFontSetDestroy(fs);
    return out;
}

std::string matchFamilyFile(const std::string& family, bool bold, bool italic)
{
    if (family.empty()) return {};
    FcConfig* cfg = config();
    if (!cfg) return {};

    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(cfg, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    // The families configuration bound to the request. fontconfig records a
    // BINDING on every substituted value: the requested family and the aliases
    // the configuration declares for it (Arial -> Arimo, Liberation Sans, ...)
    // are bound strong, while the generic fallbacks appended for "anything
    // sans" (Noto Sans and its script siblings, DejaVu ...) are bound weak.
    // Only strong/same bindings count. Position alone is not enough: the weak
    // fallbacks come straight after an unknown family, with no generic name in
    // between to stop at.
    std::vector<std::string> accepted;
    FcValue fv;
    FcValueBinding binding = FcValueBindingWeak;
    for (int i = 0;
         FcPatternGetWithBinding(pat, FC_FAMILY, i, &fv, &binding) == FcResultMatch; ++i) {
        if (fv.type != FcTypeString) continue;
        const std::string f = lower(reinterpret_cast<const char*>(fv.u.s));
        if (isGeneric(f)) break;
        if (binding == FcValueBindingStrong || binding == FcValueBindingSame) {
            accepted.push_back(f);
        }
    }

    FcResult res = FcResultNoMatch;
    FcPattern* match = FcFontMatch(cfg, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return {};

    std::string file;
    const std::string got = lower(getString(match, FC_FAMILY));
    if (std::find(accepted.begin(), accepted.end(), got) != accepted.end()) {
        file = getString(match, FC_FILE);
    }
    FcPatternDestroy(match);
    return file;
}

}  // namespace pdfe::core::fontconfig

#endif  // !_WIN32
