#include "SystemFontDiscovery.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#define NOMINMAX
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#pragma comment(lib, "dwrite.lib")
using Microsoft::WRL::ComPtr;
#endif

namespace pdfe::core {

// Pipeline G phase-9 PANOSE serif classifier lives in
// SystemFontDiscovery.h so tests can lock the byte→serif decision
// without going through DirectWrite. The DirectWrite enumeration path
// below calls SystemFontDiscoveryPanose::serifFromPanoseByte1().

// ─── Helpers ─────────────────────────────────────────────────────────────────

#ifdef _WIN32

/// Convert a wide string to UTF-8.
static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
        static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
        static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}

/// Get a localized string from a DWrite string collection (prefer en-us, fallback to index 0).
static std::string getDWriteString(IDWriteLocalizedStrings* strings) {
    if (!strings) return {};

    UINT32 index = 0;
    BOOL exists = FALSE;
    strings->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) {
        index = 0;
    }

    UINT32 length = 0;
    strings->GetStringLength(index, &length);
    if (length == 0) return {};

    std::wstring buf(length + 1, L'\0');
    strings->GetString(index, buf.data(), length + 1);
    buf.resize(length);
    return wideToUtf8(buf);
}

/// Map DWRITE_FONT_WEIGHT to our FontWeight enum.
static FontWeight mapWeight(DWRITE_FONT_WEIGHT dw) {
    int val = static_cast<int>(dw);
    // Round to nearest 100
    val = ((val + 50) / 100) * 100;
    val = std::clamp(val, 100, 900);
    return static_cast<FontWeight>(val);
}

/// Map DWRITE_FONT_STYLE to our FontStyle enum.
static FontStyle mapStyle(DWRITE_FONT_STYLE dw) {
    switch (dw) {
        case DWRITE_FONT_STYLE_ITALIC:  return FontStyle::Italic;
        case DWRITE_FONT_STYLE_OBLIQUE: return FontStyle::Oblique;
        default:                        return FontStyle::Normal;
    }
}

/// Map DWRITE_FONT_STRETCH to our FontStretch enum.
static FontStretch mapStretch(DWRITE_FONT_STRETCH dw) {
    switch (dw) {
        case DWRITE_FONT_STRETCH_ULTRA_CONDENSED:  return FontStretch::UltraCondensed;
        case DWRITE_FONT_STRETCH_EXTRA_CONDENSED:  return FontStretch::ExtraCondensed;
        case DWRITE_FONT_STRETCH_CONDENSED:        return FontStretch::Condensed;
        case DWRITE_FONT_STRETCH_SEMI_CONDENSED:   return FontStretch::SemiCondensed;
        case DWRITE_FONT_STRETCH_NORMAL:           return FontStretch::Normal;
        case DWRITE_FONT_STRETCH_SEMI_EXPANDED:    return FontStretch::SemiExpanded;
        case DWRITE_FONT_STRETCH_EXPANDED:         return FontStretch::Expanded;
        case DWRITE_FONT_STRETCH_EXTRA_EXPANDED:   return FontStretch::ExtraExpanded;
        case DWRITE_FONT_STRETCH_ULTRA_EXPANDED:   return FontStretch::UltraExpanded;
        default:                                    return FontStretch::Normal;
    }
}

/// Get the file path for a DWrite font face.
static std::string getFontFilePath(IDWriteFontFace* face) {
    if (!face) return {};

    UINT32 fileCount = 0;
    face->GetFiles(&fileCount, nullptr);
    if (fileCount == 0) return {};

    std::vector<ComPtr<IDWriteFontFile>> files(fileCount);
    std::vector<IDWriteFontFile*> rawFiles(fileCount);
    face->GetFiles(&fileCount, rawFiles.data());
    for (UINT32 i = 0; i < fileCount; ++i) {
        files[i].Attach(rawFiles[i]);
    }

    if (!files[0]) return {};

    const void* refKey = nullptr;
    UINT32 refKeySize = 0;
    files[0]->GetReferenceKey(&refKey, &refKeySize);

    ComPtr<IDWriteFontFileLoader> loader;
    files[0]->GetLoader(&loader);
    if (!loader) return {};

    ComPtr<IDWriteLocalFontFileLoader> localLoader;
    if (FAILED(loader.As(&localLoader))) return {};

    UINT32 pathLength = 0;
    localLoader->GetFilePathLengthFromKey(refKey, refKeySize, &pathLength);
    if (pathLength == 0) return {};

    std::wstring path(pathLength + 1, L'\0');
    localLoader->GetFilePathFromKey(refKey, refKeySize, path.data(), pathLength + 1);
    path.resize(pathLength);
    return wideToUtf8(path);
}

#endif // _WIN32

// ─── Impl ────────────────────────────────────────────────────────────────────

class SystemFontDiscovery::Impl {
public:
    std::vector<FontMetrics> fonts;
    bool enumerated = false;
    std::mutex mutex;

    void enumerate() {
        if (enumerated) return;

#ifdef _WIN32
        enumerateWindows();
#else
        // Linux/macOS: placeholder for fontconfig enumeration
        // Will be implemented when we target those platforms
#endif

        enumerated = true;
    }

#ifdef _WIN32
    void enumerateWindows() {
        ComPtr<IDWriteFactory> factory;
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf())
        );
        if (FAILED(hr)) return;

        ComPtr<IDWriteFontCollection> collection;
        hr = factory->GetSystemFontCollection(&collection);
        if (FAILED(hr)) return;

        UINT32 familyCount = collection->GetFontFamilyCount();

        for (UINT32 fi = 0; fi < familyCount; ++fi) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(fi, &family))) continue;

            // Get family name
            ComPtr<IDWriteLocalizedStrings> familyNames;
            if (FAILED(family->GetFamilyNames(&familyNames))) continue;
            std::string familyName = getDWriteString(familyNames.Get());

            UINT32 fontCount = family->GetFontCount();
            for (UINT32 j = 0; j < fontCount; ++j) {
                ComPtr<IDWriteFont> font;
                if (FAILED(family->GetFont(j, &font))) continue;

                FontMetrics fm;
                fm.familyName = familyName;
                fm.weight = mapWeight(font->GetWeight());
                fm.style = mapStyle(font->GetStyle());
                fm.stretch = mapStretch(font->GetStretch());

                // Get face name (style name)
                ComPtr<IDWriteLocalizedStrings> faceNames;
                if (SUCCEEDED(font->GetFaceNames(&faceNames))) {
                    fm.styleName = getDWriteString(faceNames.Get());
                }

                // Check if monospace or serif via PANOSE
                BOOL hasInfo = FALSE;
                ComPtr<IDWriteFont1> font1;
                if (SUCCEEDED(font->QueryInterface(IID_PPV_ARGS(&font1)))) {
                    hasInfo = font1->IsMonospacedFont();
                    fm.isMonospace = (hasInfo == TRUE);
                }

                // Get font face for file path and metrics
                ComPtr<IDWriteFontFace> face;
                if (SUCCEEDED(font->CreateFontFace(&face))) {
                    fm.filePath = getFontFilePath(face.Get());

                    // Get design metrics
                    DWRITE_FONT_METRICS metrics;
                    face->GetDesignGlyphMetrics(nullptr, 0, nullptr, FALSE);
                    font->GetMetrics(&metrics);
                    fm.unitsPerEm = metrics.designUnitsPerEm;
                    fm.ascender = metrics.ascent;
                    fm.descender = metrics.descent;
                    fm.xHeight = metrics.xHeight;
                    fm.capHeight = metrics.capHeight;
                }

                // Pipeline G phase-9: PANOSE byte 1 (bSerifStyle) from
                // the OS/2 table. DirectWrite's IDWriteFont1::GetPanose
                // returns the OS/2 PANOSE bytes verbatim, so we can use
                // it as the source of truth and fall back to a name
                // heuristic only when the font has no PANOSE
                // classification (byte 1 = 0 or 1, or the API call
                // fails — typically Type 1 / unmapped fonts).
                using SystemFontDiscoveryPanose::SerifKind;
                using SystemFontDiscoveryPanose::serifFromPanoseByte1;
                SerifKind serifFromPanose = SerifKind::Unknown;
                if (font1) {
                    DWRITE_PANOSE panose{};
                    font1->GetPanose(&panose);
                    serifFromPanose = serifFromPanoseByte1(
                        panose.text.serifStyle);
                }

                if (serifFromPanose == SerifKind::Serif) {
                    fm.isSerif = true;
                } else if (serifFromPanose == SerifKind::Sans) {
                    fm.isSerif = false;
                } else {
                    // PANOSE absent or unclassified — fall back to the
                    // legacy family-name heuristic. Logged at debug
                    // level via OutputDebugString so test runs and
                    // production stay quiet.
                    OutputDebugStringA(("[font-discovery] PANOSE byte1 unclassified "
                                        "for family \"" + familyName +
                                        "\"; falling back to name heuristic\n").c_str());
                    std::string lowerFamily = familyName;
                    std::transform(lowerFamily.begin(), lowerFamily.end(),
                        lowerFamily.begin(), ::tolower);
                    fm.isSerif = (lowerFamily.find("serif") != std::string::npos &&
                                  lowerFamily.find("sans") == std::string::npos) ||
                                 lowerFamily.find("times") != std::string::npos ||
                                 lowerFamily.find("georgia") != std::string::npos ||
                                 lowerFamily.find("garamond") != std::string::npos ||
                                 lowerFamily.find("palatino") != std::string::npos ||
                                 lowerFamily.find("gentium") != std::string::npos ||
                                 lowerFamily.find("cambria") != std::string::npos ||
                                 lowerFamily.find("century") != std::string::npos ||
                                 lowerFamily.find("sitka") != std::string::npos ||
                                 lowerFamily.find("caladea") != std::string::npos ||
                                 lowerFamily.find("constantia") != std::string::npos ||
                                 lowerFamily.find("book antiqua") != std::string::npos;
                }

                // Build PostScript name approximation
                // (Exact PS name requires reading the name table; use family+style for now)
                fm.postScriptName = familyName;
                if (!fm.styleName.empty() && fm.styleName != "Regular") {
                    // Remove spaces for PS name convention
                    std::string styleNoSpaces = fm.styleName;
                    styleNoSpaces.erase(
                        std::remove(styleNoSpaces.begin(), styleNoSpaces.end(), ' '),
                        styleNoSpaces.end());
                    fm.postScriptName += "-" + styleNoSpaces;
                }

                fonts.push_back(std::move(fm));
            }
        }
    }
#endif // _WIN32
};

// ─── Public interface ────────────────────────────────────────────────────────

SystemFontDiscovery::SystemFontDiscovery()
    : pImpl(std::make_unique<Impl>())
{
}

SystemFontDiscovery::~SystemFontDiscovery() = default;

const std::vector<FontMetrics>& SystemFontDiscovery::enumerateFonts() {
    std::lock_guard lock(pImpl->mutex);
    pImpl->enumerate();
    return pImpl->fonts;
}

std::optional<FontMetrics> SystemFontDiscovery::findByPostScriptName(const std::string& name) {
    const auto& fonts = enumerateFonts();

    // Case-insensitive comparison
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

    for (const auto& fm : fonts) {
        std::string lowerPs = fm.postScriptName;
        std::transform(lowerPs.begin(), lowerPs.end(), lowerPs.begin(), ::tolower);
        if (lowerPs == lowerName) {
            return fm;
        }
    }
    return std::nullopt;
}

std::optional<FontMetrics> SystemFontDiscovery::findByFamilyStyle(
    const std::string& family,
    FontWeight weight,
    FontStyle style)
{
    const auto& fonts = enumerateFonts();

    std::string lowerFamily = family;
    std::transform(lowerFamily.begin(), lowerFamily.end(), lowerFamily.begin(), ::tolower);

    // First pass: exact match
    for (const auto& fm : fonts) {
        std::string lowerFm = fm.familyName;
        std::transform(lowerFm.begin(), lowerFm.end(), lowerFm.begin(), ::tolower);
        if (lowerFm == lowerFamily && fm.weight == weight && fm.style == style) {
            return fm;
        }
    }

    // Second pass: family match with closest weight
    const FontMetrics* best = nullptr;
    int bestDist = std::numeric_limits<int>::max();
    for (const auto& fm : fonts) {
        std::string lowerFm = fm.familyName;
        std::transform(lowerFm.begin(), lowerFm.end(), lowerFm.begin(), ::tolower);
        if (lowerFm != lowerFamily) continue;
        if (fm.style != style) continue;

        int dist = std::abs(fm.weightValue() - static_cast<int>(weight));
        if (dist < bestDist) {
            bestDist = dist;
            best = &fm;
        }
    }

    if (best) return *best;
    return std::nullopt;
}

} // namespace pdfe::core
