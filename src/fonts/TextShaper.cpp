#include "fonts/TextShaper.h"
#include "fonts/FontMetricProfile.h"  // full FontScript definition

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#include <cmath>
#include <numeric>

namespace pdfe::fonts {

// ---------------------------------------------------------------------------
// Script / direction mapping (R2-20/R2-21)
// ---------------------------------------------------------------------------

static hb_script_t hbScriptFor(FontScript s)
{
    switch (s) {
        case FontScript::Latin:      return HB_SCRIPT_LATIN;
        case FontScript::Arabic:     return HB_SCRIPT_ARABIC;
        case FontScript::Cyrillic:   return HB_SCRIPT_CYRILLIC;
        case FontScript::CJK:        return HB_SCRIPT_HAN;
        case FontScript::Devanagari: return HB_SCRIPT_DEVANAGARI;
        case FontScript::Hebrew:     return HB_SCRIPT_HEBREW;
        case FontScript::Thai:       return HB_SCRIPT_THAI;
        case FontScript::Unknown:
        case FontScript::Other:
        default:                     return HB_SCRIPT_COMMON;
    }
}

static const char* defaultLanguageFor(FontScript s)
{
    switch (s) {
        case FontScript::Arabic:     return "ar";
        case FontScript::Hebrew:     return "he";
        case FontScript::Cyrillic:   return "ru";
        case FontScript::CJK:        return "ja";
        case FontScript::Devanagari: return "hi";
        case FontScript::Thai:       return "th";
        case FontScript::Latin:
        case FontScript::Unknown:
        case FontScript::Other:
        default:                     return "en";
    }
}

ShapeParams ShapeParams::forScript(FontScript s, bool verticalMode)
{
    ShapeParams p;
    p.script = s;
    p.language = defaultLanguageFor(s);
    p.vertical = verticalMode;
    if (verticalMode) {
        p.direction = ShapeDirection::TTB;
    } else if (s == FontScript::Arabic || s == FontScript::Hebrew) {
        p.direction = ShapeDirection::RTL;
    } else {
        p.direction = ShapeDirection::LTR;
    }
    return p;
}

static hb_direction_t hbDirectionFor(const ShapeParams& params)
{
    if (params.vertical || params.direction == ShapeDirection::TTB)
        return HB_DIRECTION_TTB;
    if (params.direction == ShapeDirection::RTL)
        return HB_DIRECTION_RTL;
    return HB_DIRECTION_LTR;
}

// ---------------------------------------------------------------------------
// Bidi (UAX#9-lite): strong direction classification by Unicode range
// ---------------------------------------------------------------------------

// Returns +1 for strong LTR, -1 for strong RTL (Hebrew R + Arabic AL), 0 for
// neutral/weak (spaces, digits, punctuation, symbols).
static int strongDir(char32_t ch)
{
    // Strong RTL: Hebrew (R) and Arabic (AL).
    if ((ch >= 0x0590 && ch <= 0x05FF) ||   // Hebrew
        (ch >= 0xFB1D && ch <= 0xFB4F) ||   // Hebrew Presentation
        (ch >= 0x0600 && ch <= 0x06FF) ||   // Arabic
        (ch >= 0x0750 && ch <= 0x077F) ||   // Arabic Supplement
        (ch >= 0xFB50 && ch <= 0xFDFF) ||   // Arabic Presentation-A
        (ch >= 0xFE70 && ch <= 0xFEFF)) {   // Arabic Presentation-B
        return -1;
    }
    // Strong LTR: Latin, Cyrillic, CJK, Devanagari, Thai letters.
    if ((ch >= 0x0041 && ch <= 0x005A) ||   // A-Z
        (ch >= 0x0061 && ch <= 0x007A) ||   // a-z
        (ch >= 0x00C0 && ch <= 0x024F) ||   // Latin Extended
        (ch >= 0x0400 && ch <= 0x052F) ||   // Cyrillic
        (ch >= 0x0900 && ch <= 0x097F) ||   // Devanagari
        (ch >= 0x0E00 && ch <= 0x0E7F) ||   // Thai
        (ch >= 0x4E00 && ch <= 0x9FFF) ||   // CJK Unified
        (ch >= 0x3400 && ch <= 0x4DBF) ||   // CJK Ext A
        (ch >= 0x3040 && ch <= 0x309F) ||   // Hiragana
        (ch >= 0x30A0 && ch <= 0x30FF) ||   // Katakana
        (ch >= 0xAC00 && ch <= 0xD7AF)) {   // Hangul
        return +1;
    }
    return 0;  // neutral / weak
}

std::vector<DirectionalRun> TextShaper::bidiRuns(const std::u32string& logical)
{
    std::vector<DirectionalRun> runs;
    if (logical.empty())
        return runs;

    const size_t n = logical.size();

    // Paragraph base level: first strong character (default LTR).
    int baseLevel = 0;
    for (char32_t ch : logical) {
        int d = strongDir(ch);
        if (d != 0) { baseLevel = (d < 0) ? 1 : 0; break; }
    }

    // Resolve a level (0=LTR, 1=RTL) per character: strong chars take their own
    // direction; neutral/weak chars inherit the previous strong level, or the
    // base level before any strong char is seen.
    std::vector<int> level(n, baseLevel);
    int prevStrong = baseLevel;
    for (size_t i = 0; i < n; ++i) {
        int d = strongDir(logical[i]);
        if (d > 0)       { level[i] = 0; prevStrong = 0; }
        else if (d < 0)  { level[i] = 1; prevStrong = 1; }
        else             { level[i] = prevStrong; }
    }

    // Coalesce equal-level neighbours into runs.
    size_t start = 0;
    for (size_t i = 1; i <= n; ++i) {
        if (i == n || level[i] != level[start]) {
            DirectionalRun r;
            r.start = start;
            r.end = i;
            r.rtl = (level[start] == 1);
            runs.push_back(r);
            start = i;
        }
    }
    return runs;
}

std::u32string TextShaper::reorderForDisplay(const std::u32string& logical)
{
    if (logical.empty())
        return logical;

    const auto runs = bidiRuns(logical);

    // Determine base level from the first run's origin: if the whole paragraph
    // is RTL (all runs rtl), reverse the run ORDER too (UAX#9 L2 with base 1).
    bool baseRtl = false;
    for (char32_t ch : logical) {
        int d = strongDir(ch);
        if (d != 0) { baseRtl = (d < 0); break; }
    }

    std::u32string out;
    out.reserve(logical.size());

    if (!baseRtl) {
        // LTR base: keep run order, reverse the glyphs within each RTL run.
        for (const auto& r : runs) {
            if (r.rtl) {
                for (size_t i = r.end; i > r.start; --i)
                    out.push_back(logical[i - 1]);
            } else {
                for (size_t i = r.start; i < r.end; ++i)
                    out.push_back(logical[i]);
            }
        }
    } else {
        // RTL base: emit runs in reverse order; reverse chars within RTL runs.
        for (size_t ri = runs.size(); ri > 0; --ri) {
            const auto& r = runs[ri - 1];
            if (r.rtl) {
                for (size_t i = r.end; i > r.start; --i)
                    out.push_back(logical[i - 1]);
            } else {
                for (size_t i = r.start; i < r.end; ++i)
                    out.push_back(logical[i]);
            }
        }
    }
    return out;
}

CaretAdvance TextShaper::caretAdvance(const ShapedGlyph& glyph, bool vertical)
{
    CaretAdvance c;
    if (vertical) {
        c.dx = 0.0f;
        c.dy = glyph.yAdvance;   // HarfBuzz TTB yields the vertical advance
    } else {
        c.dx = glyph.xAdvance;
        c.dy = 0.0f;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

class TextShaper::Impl {
public:
    Impl() = default;
    ~Impl() = default;

    std::vector<ShapedGlyph> shape(void* ftFaceVoid, const std::u32string& text,
                                   float fontSize, const ShapeParams& params)
    {
        std::vector<ShapedGlyph> result;
        if (!ftFaceVoid || text.empty())
            return result;

        auto* face = static_cast<FT_Face>(ftFaceVoid);

        // Set size for shaping
        FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(fontSize * 64.0f), 72, 72);

        // Create HarfBuzz font from FreeType face
        hb_font_t* hbFont = hb_ft_font_create_referenced(face);
        if (!hbFont)
            return result;

        // Create buffer and add text
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_set_direction(buf, hbDirectionFor(params));
        hb_buffer_set_script(buf, hbScriptFor(params.script));
        hb_buffer_set_language(
            buf, hb_language_from_string(params.language.c_str(),
                                         static_cast<int>(params.language.size())));

        // Convert u32string to uint32_t array for HarfBuzz
        std::vector<uint32_t> codepoints(text.begin(), text.end());
        hb_buffer_add_codepoints(buf, codepoints.data(),
                                  static_cast<int>(codepoints.size()),
                                  0, static_cast<int>(codepoints.size()));

        // Shape
        hb_shape(hbFont, buf, nullptr, 0);

        // Extract results
        unsigned int glyphCount = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &glyphCount);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &glyphCount);

        result.reserve(glyphCount);
        for (unsigned int i = 0; i < glyphCount; ++i) {
            ShapedGlyph glyph;
            glyph.glyphId = info[i].codepoint; // After shaping, this is glyph ID
            glyph.cluster = info[i].cluster;   // Logical codepoint index
            glyph.xAdvance = static_cast<float>(pos[i].x_advance) / 64.0f;
            glyph.yAdvance = static_cast<float>(pos[i].y_advance) / 64.0f;
            glyph.xOffset = static_cast<float>(pos[i].x_offset) / 64.0f;
            glyph.yOffset = static_cast<float>(pos[i].y_offset) / 64.0f;
            result.push_back(glyph);
        }

        hb_buffer_destroy(buf);
        hb_font_destroy(hbFont);

        return result;
    }

    float measureWidth(void* ftFaceVoid, const std::u32string& text,
                       float fontSize, const ShapeParams& params)
    {
        auto glyphs = shape(ftFaceVoid, text, fontSize, params);
        if (params.vertical || params.direction == ShapeDirection::TTB) {
            return std::accumulate(glyphs.begin(), glyphs.end(), 0.0f,
                [](float acc, const auto& g) { return acc + std::fabs(g.yAdvance); });
        }
        return std::accumulate(glyphs.begin(), glyphs.end(), 0.0f,
            [](float acc, const auto& g) { return acc + g.xAdvance; });
    }

    bool canRender(void* ftFaceVoid, const std::u32string& text)
    {
        if (!ftFaceVoid)
            return false;

        auto* face = static_cast<FT_Face>(ftFaceVoid);

        for (char32_t ch : text) {
            FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
            if (gidx == 0)
                return false;
        }
        return true;
    }
};

TextShaper::TextShaper()
    : pImpl(std::make_unique<Impl>())
{
}

TextShaper::~TextShaper() = default;

// Backward-compatible LTR/Latin/en default (existing callers).
static ShapeParams defaultLatinParams()
{
    ShapeParams p;
    p.script = FontScript::Latin;
    p.direction = ShapeDirection::LTR;
    p.language = "en";
    p.vertical = false;
    return p;
}

std::vector<ShapedGlyph> TextShaper::shape(void* ftFace, const std::u32string& text, float fontSize)
{
    return pImpl->shape(ftFace, text, fontSize, defaultLatinParams());
}

std::vector<ShapedGlyph> TextShaper::shape(void* ftFace, const std::u32string& text,
                                           float fontSize, const ShapeParams& params)
{
    return pImpl->shape(ftFace, text, fontSize, params);
}

float TextShaper::measureWidth(void* ftFace, const std::u32string& text, float fontSize)
{
    return pImpl->measureWidth(ftFace, text, fontSize, defaultLatinParams());
}

float TextShaper::measureWidth(void* ftFace, const std::u32string& text,
                               float fontSize, const ShapeParams& params)
{
    return pImpl->measureWidth(ftFace, text, fontSize, params);
}

bool TextShaper::canRender(void* ftFace, const std::u32string& text)
{
    return pImpl->canRender(ftFace, text);
}

} // namespace pdfe::fonts
