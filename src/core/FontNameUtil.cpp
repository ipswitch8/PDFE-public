#include "FontNameUtil.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <set>
#include <utility>

namespace pdfe::core {
namespace {

/// Record *baseFont* under its 31-char key (matching ``fz_font_name``'s
/// truncation), once with the subset prefix and once without it, so a lookup
/// with whatever MuPDF kept resolves. On a key collision between two distinct
/// BaseFonts, store the truncated key itself as a sentinel so
/// :func:`resolveUntruncatedFontName` sees no length gain and keeps the
/// truncated name rather than mis-attributing.
void addKey(BaseFontMap& out, const std::string& baseFont) {
    if (baseFont.empty())
        return;
    auto add1 = [&](const std::string& candidate) {
        if (candidate.empty())
            return;
        std::string key = candidate.size() > 31 ? candidate.substr(0, 31)
                                                 : candidate;
        auto it = out.find(key);
        if (it == out.end())
            out.emplace(std::move(key), baseFont);
        else if (it->second != baseFont)
            it->second = key;  // ambiguous 31-char prefix -> no-gain sentinel
    };
    add1(baseFont);
    const auto plus = baseFont.find('+');
    if (plus != std::string::npos && plus + 1 < baseFont.size())
        add1(baseFont.substr(plus + 1));
}

/// Recursively collect every ``/BaseFont`` reachable from a resources dict,
/// descending one level per Form XObject's own resources. *visited* guards
/// against resource cycles; *depth* bounds pathological nesting.
void collectFonts(fz_context* ctx, pdf_obj* resources, BaseFontMap& out,
                  std::set<pdf_obj*>& visited, int depth) {
    if (!resources || depth > 8)
        return;
    pdf_obj* res = pdf_resolve_indirect(ctx, resources);
    if (!res || visited.count(res))
        return;
    visited.insert(res);

    pdf_obj* fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    if (fonts && pdf_is_dict(ctx, fonts)) {
        const int n = pdf_dict_len(ctx, fonts);
        for (int i = 0; i < n; ++i) {
            pdf_obj* fdict =
                pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, fonts, i));
            if (!fdict)
                continue;
            // For a Type0 composite font the /BaseFont on the top-level dict is
            // exactly what fz_font_name reflects, so the descendant CIDFont need
            // not be walked.
            pdf_obj* bf = pdf_dict_get(ctx, fdict, PDF_NAME(BaseFont));
            const char* name = bf ? pdf_to_name(ctx, bf) : nullptr;
            if (name && name[0])
                addKey(out, std::string(name));
        }
    }

    pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (xobjs && pdf_is_dict(ctx, xobjs)) {
        const int n = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < n; ++i) {
            pdf_obj* xo =
                pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, xobjs, i));
            if (!xo)
                continue;
            pdf_obj* xres = pdf_dict_get(ctx, xo, PDF_NAME(Resources));
            if (xres)
                collectFonts(ctx, xres, out, visited, depth + 1);
        }
    }
}

/// Read italic/bold from a font dict's /FontDescriptor. For a Type0 composite
/// the descriptor lives on the descendant CIDFont, so fall through to that.
/// Uses C-string key lookups (pdf_dict_gets) so it needs no PDF_NAME macro for
/// the less-common descriptor keys. Absent descriptor => {false, false}.
FontStyleFlags readDescriptorStyle(fz_context* ctx, pdf_obj* fontDict) {
    FontStyleFlags fs;
    if (!fontDict)
        return fs;
    pdf_obj* fd = pdf_dict_gets(ctx, fontDict, "FontDescriptor");
    if (!fd || !pdf_is_dict(ctx, fd)) {
        pdf_obj* desc = pdf_dict_gets(ctx, fontDict, "DescendantFonts");
        if (desc && pdf_is_array(ctx, desc) && pdf_array_len(ctx, desc) > 0) {
            pdf_obj* cid =
                pdf_resolve_indirect(ctx, pdf_array_get(ctx, desc, 0));
            if (cid)
                fd = pdf_dict_gets(ctx, cid, "FontDescriptor");
        }
    }
    fd = pdf_resolve_indirect(ctx, fd);
    if (!fd || !pdf_is_dict(ctx, fd))
        return fs;
    const int flags = pdf_to_int(ctx, pdf_dict_gets(ctx, fd, "Flags"));
    const double italicAngle =
        pdf_to_real(ctx, pdf_dict_gets(ctx, fd, "ItalicAngle"));
    // /Flags bit 7 (Italic, value 64); /ItalicAngle non-zero also => italic.
    fs.isItalic = (flags & (1 << 6)) != 0 || italicAngle != 0.0;
    // The descriptor has no direct "bold" flag: /Flags bit 19 (ForceBold,
    // value 0x40000) signals bold intent, and a heavy /StemV is the classic
    // bold proxy.
    const bool forceBold = (flags & (1 << 18)) != 0;
    const double stemV = pdf_to_real(ctx, pdf_dict_gets(ctx, fd, "StemV"));
    fs.isBold = forceBold || stemV >= 140.0;
    return fs;
}

/// Record *fs* under *baseFont*'s 31-char key(s) (with and without the subset
/// prefix), matching the BaseFont keying. First entry wins on the rare 31-char
/// prefix collision (ambiguous styles are not worth distinguishing).
void addStyleKey(FontStyleMap& out, const std::string& baseFont,
                 FontStyleFlags fs) {
    if (baseFont.empty())
        return;
    auto add1 = [&](const std::string& candidate) {
        if (candidate.empty())
            return;
        std::string key = candidate.size() > 31 ? candidate.substr(0, 31)
                                                 : candidate;
        out.emplace(std::move(key), fs);
    };
    add1(baseFont);
    const auto plus = baseFont.find('+');
    if (plus != std::string::npos && plus + 1 < baseFont.size())
        add1(baseFont.substr(plus + 1));
}

/// Mirror of collectFonts that records descriptor style flags instead of the
/// untruncated name. (Kept as a separate walk for clarity; the per-page cost is
/// negligible next to full text extraction.)
void collectFontStyles(fz_context* ctx, pdf_obj* resources, FontStyleMap& out,
                       std::set<pdf_obj*>& visited, int depth) {
    if (!resources || depth > 8)
        return;
    pdf_obj* res = pdf_resolve_indirect(ctx, resources);
    if (!res || visited.count(res))
        return;
    visited.insert(res);

    pdf_obj* fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    if (fonts && pdf_is_dict(ctx, fonts)) {
        const int n = pdf_dict_len(ctx, fonts);
        for (int i = 0; i < n; ++i) {
            pdf_obj* fdict =
                pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, fonts, i));
            if (!fdict)
                continue;
            pdf_obj* bf = pdf_dict_get(ctx, fdict, PDF_NAME(BaseFont));
            const char* name = bf ? pdf_to_name(ctx, bf) : nullptr;
            if (name && name[0])
                addStyleKey(out, std::string(name),
                            readDescriptorStyle(ctx, fdict));
        }
    }

    pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (xobjs && pdf_is_dict(ctx, xobjs)) {
        const int n = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < n; ++i) {
            pdf_obj* xo =
                pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, xobjs, i));
            if (!xo)
                continue;
            pdf_obj* xres = pdf_dict_get(ctx, xo, PDF_NAME(Resources));
            if (xres)
                collectFontStyles(ctx, xres, out, visited, depth + 1);
        }
    }
}

}  // namespace

BaseFontMap buildBaseFontMap(struct fz_context* ctx, struct fz_page* page) {
    BaseFontMap out;
    if (!ctx || !page)
        return out;
    // Declared before fz_try so a longjmp during the MuPDF walk does not skip
    // their C++ destructors. Every pdf_* call that can longjmp runs BEFORE any
    // container mutation, so the containers are never left half-updated.
    std::set<pdf_obj*> visited;
    fz_try(ctx) {
        pdf_page* ppage = pdf_page_from_fz_page(ctx, page);
        if (ppage && ppage->obj) {
            pdf_obj* resources =
                pdf_dict_get(ctx, ppage->obj, PDF_NAME(Resources));
            collectFonts(ctx, resources, out, visited, 0);
        }
    }
    fz_catch(ctx) {
        // Best-effort: return whatever was collected; callers fall back to the
        // truncated fz_font_name for any font not in the map.
    }
    return out;
}

std::string resolveUntruncatedFontName(const BaseFontMap& map,
                                       const std::string& fzName) {
    const auto it = map.find(fzName);
    if (it != map.end() && it->second.size() > fzName.size())
        return it->second;
    return fzName;
}

FontStyleMap buildFontStyleMap(struct fz_context* ctx, struct fz_page* page) {
    FontStyleMap out;
    if (!ctx || !page)
        return out;
    std::set<pdf_obj*> visited;
    fz_try(ctx) {
        pdf_page* ppage = pdf_page_from_fz_page(ctx, page);
        if (ppage && ppage->obj) {
            pdf_obj* resources =
                pdf_dict_get(ctx, ppage->obj, PDF_NAME(Resources));
            collectFontStyles(ctx, resources, out, visited, 0);
        }
    }
    fz_catch(ctx) {
        // Best-effort: callers fall back to name-keyword parsing.
    }
    return out;
}

FontStyleFlags resolveFontStyle(const FontStyleMap& map,
                                const std::string& fzName) {
    const auto it = map.find(fzName);
    return it != map.end() ? it->second : FontStyleFlags{};
}

}  // namespace pdfe::core
