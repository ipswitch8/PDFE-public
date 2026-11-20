// -*- coding: utf-8 -*-
#include "image/ImagePlacementScanner.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace pdfe::image::placement {

namespace {

// ---------------------------------------------------------------------------
// Tokeniser (PDF 32000-1 7.2). Only what a content stream needs; every token
// carries its byte range so a rewrite can splice exactly one operator.
// ---------------------------------------------------------------------------
bool isWS(unsigned char c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
bool isDelim(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

enum class Tk { Number, Name, String, Open, Close, Keyword, End };

struct Token {
    Tk kind = Tk::End;
    std::size_t start = 0, end = 0;
    double num = 0;
    std::string text;  // Name: raw bytes after '/'; Keyword: the keyword
};

Token nextToken(const unsigned char* d, std::size_t n, std::size_t& pos) {
    for (;;) {
        while (pos < n && isWS(d[pos])) ++pos;
        if (pos < n && d[pos] == '%') {
            while (pos < n && d[pos] != '\n' && d[pos] != '\r') ++pos;
            continue;
        }
        break;
    }
    Token t;
    t.start = pos;
    if (pos >= n) {
        t.kind = Tk::End;
        t.end = pos;
        return t;
    }
    const unsigned char c = d[pos];
    if (c == '/') {
        ++pos;
        while (pos < n && !isWS(d[pos]) && !isDelim(d[pos])) ++pos;
        t.kind = Tk::Name;
        t.text.assign(reinterpret_cast<const char*>(d) + t.start + 1, pos - t.start - 1);
    } else if (c == '(') {
        int depth = 0;
        while (pos < n) {
            const unsigned char ch = d[pos++];
            if (ch == '\\') {
                if (pos < n) ++pos;
                continue;
            }
            if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                if (--depth == 0) break;
            }
        }
        t.kind = Tk::String;
    } else if (c == '<') {
        if (pos + 1 < n && d[pos + 1] == '<') {
            pos += 2;
            t.kind = Tk::Open;
        } else {
            ++pos;
            while (pos < n && d[pos] != '>') ++pos;
            if (pos < n) ++pos;
            t.kind = Tk::String;
        }
    } else if (c == '>') {
        pos += (pos + 1 < n && d[pos + 1] == '>') ? 2 : 1;
        t.kind = Tk::Close;
    } else if (c == '[' || c == '{') {
        ++pos;
        t.kind = Tk::Open;
    } else if (c == ']' || c == '}' || c == ')') {
        ++pos;
        t.kind = Tk::Close;
    } else {
        while (pos < n && !isWS(d[pos]) && !isDelim(d[pos])) ++pos;
        t.text.assign(reinterpret_cast<const char*>(d) + t.start, pos - t.start);
        t.kind = Tk::Keyword;
        const char f = t.text[0];
        if ((f >= '0' && f <= '9') || f == '+' || f == '-' || f == '.') {
            char* endp = nullptr;
            const double v = std::strtod(t.text.c_str(), &endp);
            if (endp && *endp == '\0') {
                t.kind = Tk::Number;
                t.num = v;
            }
        }
    }
    t.end = pos;
    return t;
}

// Name bytes with #xx escapes decoded, for the resource lookup.
std::string decodeName(const std::string& raw) {
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' && i + 2 < raw.size()) {
            const int hi = hex(raw[i + 1]);
            const int lo = hex(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

// Byte length of an inline image's data from its dictionary tokens (between
// BI and ID), or -1 when it cannot be known: an explicit /L (PDF 2.0), else
// H * ceil(W * components * BPC / 8) for an UNFILTERED image in a device
// colour space, Indexed space or image mask.
long long inlineImageDataLength(const std::vector<Token>& dict) {
    long long w = -1, h = -1, bpc = -1, comps = -1, explicitLen = -1;
    bool filtered = false, mask = false;
    for (std::size_t i = 0; i + 1 < dict.size(); ++i) {
        if (dict[i].kind != Tk::Name) continue;
        const std::string& k = dict[i].text;
        const Token& v = dict[i + 1];
        auto num = [&]() { return v.kind == Tk::Number ? static_cast<long long>(v.num) : -1; };
        if (k == "W" || k == "Width") w = num();
        else if (k == "H" || k == "Height") h = num();
        else if (k == "BPC" || k == "BitsPerComponent") bpc = num();
        else if (k == "L" || k == "Length") explicitLen = num();
        else if (k == "IM" || k == "ImageMask") mask = (v.kind == Tk::Keyword && v.text == "true");
        else if (k == "F" || k == "Filter") {
            // An empty filter array is no filter.
            filtered = !(v.kind == Tk::Open && i + 2 < dict.size() && dict[i + 2].kind == Tk::Close);
        } else if (k == "CS" || k == "ColorSpace") {
            std::string cs;
            if (v.kind == Tk::Name) cs = v.text;
            else if (v.kind == Tk::Open && i + 2 < dict.size() && dict[i + 2].kind == Tk::Name)
                cs = dict[i + 2].text;
            if (cs == "G" || cs == "DeviceGray" || cs == "I" || cs == "Indexed") comps = 1;
            else if (cs == "RGB" || cs == "DeviceRGB") comps = 3;
            else if (cs == "CMYK" || cs == "DeviceCMYK") comps = 4;
        }
    }
    if (explicitLen >= 0) return explicitLen;
    if (filtered) return -1;
    if (mask) {
        comps = 1;
        bpc = 1;
    }
    if (w <= 0 || h <= 0 || bpc <= 0 || comps <= 0) return -1;
    if (w > (1 << 20) || h > (1 << 20)) return -1;
    return h * ((w * comps * bpc + 7) / 8);
}

// ---------------------------------------------------------------------------
// Interpreter: q/Q/cm tracking plus the few operators a wrapper is made of.
// ---------------------------------------------------------------------------
enum class OpCode { q, Q, cm, re, W, n, Do, Other };

struct Op {
    OpCode code = OpCode::Other;
    double v[6] = {0, 0, 0, 0, 0, 0};
    std::size_t start = 0, end = 0;
    std::size_t nameStart = 0, nameEnd = 0;  // a Do's name operand
    int stream = -1;
    Affine ctmBefore;
};

constexpr int kMaxFormDepth = 8;

struct Scanner {
    fz_context* ctx = nullptr;
    bool recurseForms = false;
    std::vector<DoSite>* out = nullptr;
    std::vector<pdf_obj*> formStack;  // cycle guard

    // One content scope: the page's /Contents, or one Form XObject.
    struct Scope {
        pdf_obj* resources = nullptr;
        int depth = 0;
        std::vector<Op> ops;
        std::vector<Affine> stack;
        Affine ctm;
        std::map<std::string, int> counts;
        std::vector<std::pair<std::size_t, std::size_t>> sites;  // (out index, op index)
    };

    static fz_buffer* loadStream(fz_context* ctx, pdf_obj* obj) {
        fz_buffer* buf = nullptr;
        fz_var(buf);
        fz_try(ctx) {
            buf = pdf_load_stream(ctx, obj);
        }
        fz_catch(ctx) {
            // An unreadable stream draws nothing; scan the rest of the page.
            fz_rethrow_if(ctx, FZ_ERROR_SYSTEM);
            fz_report_error(ctx);
            buf = nullptr;
        }
        return buf;
    }

    void runBuffer(Scope& s, fz_buffer* buf, int streamIndex) {
        unsigned char* data = nullptr;
        const std::size_t len = buf ? fz_buffer_storage(ctx, buf, &data) : 0;
        if (!data || len == 0) return;
        std::size_t pos = 0;
        std::vector<Token> operands;
        for (;;) {
            Token t = nextToken(data, len, pos);
            if (t.kind == Tk::End) break;
            if (t.kind != Tk::Keyword) {
                operands.push_back(std::move(t));
                continue;
            }
            if (t.text == "true" || t.text == "false" || t.text == "null") {
                operands.push_back(std::move(t));
                continue;
            }
            Op op;
            op.start = operands.empty() ? t.start : operands.front().start;
            op.end = t.end;
            op.stream = streamIndex;
            op.ctmBefore = s.ctm;
            if (t.text == "BI") {
                // Inline image: dictionary up to ID, then binary data, then EI.
                // The binary data may contain any bytes - including "EI",
                // "Do" or "/Im0" - so it is skipped by its computed length
                // when the dictionary allows it, and only otherwise by the
                // first whitespace-delimited EI.
                std::vector<Token> dict;
                for (;;) {
                    Token k = nextToken(data, len, pos);
                    if (k.kind == Tk::End) break;
                    if (k.kind == Tk::Keyword && k.text == "ID") break;
                    dict.push_back(std::move(k));
                }
                if (pos < len) ++pos;  // the single whitespace after ID
                const std::size_t dataStart = pos;
                const long long known = inlineImageDataLength(dict);
                bool skipped = false;
                if (known >= 0 && dataStart + static_cast<std::size_t>(known) <= len) {
                    std::size_t j = dataStart + static_cast<std::size_t>(known);
                    while (j < len && isWS(data[j])) ++j;
                    if (j + 1 < len && data[j] == 'E' && data[j + 1] == 'I' &&
                        (j + 2 >= len || isWS(data[j + 2]) || isDelim(data[j + 2]))) {
                        pos = j + 2;
                        skipped = true;
                    }
                }
                if (!skipped) {
                    std::size_t i = dataStart;
                    for (; i + 1 < len; ++i) {
                        if (data[i] == 'E' && data[i + 1] == 'I' && (i == 0 || isWS(data[i - 1])) &&
                            (i + 2 >= len || isWS(data[i + 2]) || isDelim(data[i + 2])))
                            break;
                    }
                    pos = (i + 1 < len) ? i + 2 : len;
                }
                op.end = pos;
                s.ops.push_back(op);
                operands.clear();
                continue;
            }
            auto numbers = [&](std::size_t count) {
                if (operands.size() < count) return false;
                for (std::size_t k = operands.size() - count; k < operands.size(); ++k)
                    if (operands[k].kind != Tk::Number) return false;
                for (std::size_t k = 0; k < count; ++k)
                    op.v[k] = operands[operands.size() - count + k].num;
                return true;
            };
            if (t.text == "q") {
                op.code = OpCode::q;
                s.stack.push_back(s.ctm);
            } else if (t.text == "Q") {
                op.code = OpCode::Q;
                if (!s.stack.empty()) {
                    s.ctm = s.stack.back();
                    s.stack.pop_back();
                }
            } else if (t.text == "cm") {
                if (numbers(6)) {
                    op.code = OpCode::cm;
                    const Affine m{op.v[0], op.v[1], op.v[2], op.v[3], op.v[4], op.v[5]};
                    s.ctm = concat(m, s.ctm);
                }
            } else if (t.text == "re") {
                if (numbers(4)) op.code = OpCode::re;
            } else if (t.text == "W") {
                op.code = OpCode::W;
            } else if (t.text == "n") {
                op.code = OpCode::n;
            } else if (t.text == "Do") {
                if (!operands.empty() && operands.back().kind == Tk::Name) {
                    op.code = OpCode::Do;
                    op.nameStart = operands.back().start;
                    op.nameEnd = operands.back().end;
                    const std::string raw = operands.back().text;
                    s.ops.push_back(op);
                    operands.clear();
                    onDo(s, raw, s.ops.size() - 1);
                    continue;
                }
            }
            s.ops.push_back(op);
            operands.clear();
        }
    }

    void onDo(Scope& s, const std::string& rawName, std::size_t opIndex) {
        DoSite site;
        site.name = rawName;
        // Identity (GitHub #421): occurrence counts the page's OWN Do's of
        // this name only. A Do inside a Form XObject gets -1, so it can never
        // alias a page-level placement of the same name (the form has its own
        // name space); such placements are not addressable for editing.
        site.occurrence = (s.depth == 0) ? s.counts[rawName]++ : -1;
        site.formDepth = s.depth;
        site.ctm = s.ctm;
        site.ctmOuter = s.ctm;
        site.stream = s.ops[opIndex].stream;
        site.replaceStart = s.ops[opIndex].start;
        site.replaceEnd = s.ops[opIndex].end;
        site.nameStart = s.ops[opIndex].nameStart;
        site.nameEnd = s.ops[opIndex].nameEnd;

        pdf_obj* xobjects = s.resources ? pdf_dict_get(ctx, s.resources, PDF_NAME(XObject)) : nullptr;
        pdf_obj* xobj = xobjects ? pdf_dict_gets(ctx, xobjects, decodeName(rawName).c_str()) : nullptr;
        site.xobject = xobj;
        site.resources = s.resources;
        pdf_obj* subtype = xobj ? pdf_dict_get(ctx, xobj, PDF_NAME(Subtype)) : nullptr;
        site.isImage = pdf_name_eq(ctx, subtype, PDF_NAME(Image));

        out->push_back(site);
        s.sites.emplace_back(out->size() - 1, opIndex);

        if (recurseForms && xobj && pdf_name_eq(ctx, subtype, PDF_NAME(Form)) &&
            s.depth < kMaxFormDepth &&
            std::find(formStack.begin(), formStack.end(), xobj) == formStack.end()) {
            runForm(s, xobj);
        }
    }

    void runForm(Scope& parent, pdf_obj* form) {
        Scope child;
        child.depth = parent.depth + 1;
        pdf_obj* res = pdf_dict_get(ctx, form, PDF_NAME(Resources));
        child.resources = res ? res : parent.resources;
        const fz_matrix fm = pdf_dict_get_matrix(ctx, form, PDF_NAME(Matrix));
        child.ctm = concat(Affine{fm.a, fm.b, fm.c, fm.d, fm.e, fm.f}, parent.ctm);
        fz_buffer* buf = loadStream(ctx, form);
        formStack.push_back(form);
        runBuffer(child, buf, -1);
        formStack.pop_back();
        fz_drop_buffer(ctx, buf);
        recognise(child);
    }

    // Classify the wrapper around each Do of the scope.
    void recognise(Scope& s) {
        const auto& ops = s.ops;
        auto is = [&](std::size_t i, OpCode c, int stream) {
            return i < ops.size() && ops[i].code == c && ops[i].stream == stream;
        };
        for (const auto& [siteIdx, i] : s.sites) {
            DoSite& site = (*out)[siteIdx];
            const int st = ops[i].stream;
            if (!is(i + 1, OpCode::Q, st)) continue;
            // q F cm /Name Do Q
            if (i >= 2 && is(i - 1, OpCode::cm, st) && is(i - 2, OpCode::q, st)) {
                site.wrapper = WrapperKind::Pdfe;
                site.ctmOuter = ops[i - 2].ctmBefore;
                site.replaceStart = ops[i - 2].start;
                site.replaceEnd = ops[i + 1].end;
                continue;
            }
            if (i < 5 || !is(i - 5, OpCode::q, st)) continue;
            // q F cm u0 v0 du dv re W n /Name Do Q
            if (is(i - 1, OpCode::n, st) && is(i - 2, OpCode::W, st) &&
                is(i - 3, OpCode::re, st) && is(i - 4, OpCode::cm, st)) {
                const double* r = ops[i - 3].v;
                UnitCrop c{std::min(r[0], r[0] + r[2]), std::min(r[1], r[1] + r[3]),
                           std::max(r[0], r[0] + r[2]), std::max(r[1], r[1] + r[3])};
                site.wrapper = WrapperKind::Pdfe;
                site.crop = c.clamped();
                site.cropped = !site.crop.isFull();
                site.ctmOuter = ops[i - 5].ctmBefore;
                site.replaceStart = ops[i - 5].start;
                site.replaceEnd = ops[i + 1].end;
                continue;
            }
            // Legacy: q x y w h re W n M cm /Name Do Q (clip in the outer
            // space), as the editor before the fix wrote it: always with an
            // AXIS-ALIGNED M, for which the clip maps exactly onto a unit-square
            // rect. Any other producer's shape - a rotated or skewed M, where
            // the inverse-mapped clip is not a unit-square rect - is NOT
            // converted: it stays as enclosing context (WrapperKind::None) and
            // an edit wraps only the Do, so the third-party clip is untouched.
            if (is(i - 1, OpCode::cm, st) && is(i - 2, OpCode::n, st) &&
                is(i - 3, OpCode::W, st) && is(i - 4, OpCode::re, st) &&
                std::abs(ops[i - 1].v[1]) < 1e-9 && std::abs(ops[i - 1].v[2]) < 1e-9) {
                const double* m = ops[i - 1].v;
                Affine inv;
                if (!invert(Affine{m[0], m[1], m[2], m[3], m[4], m[5]}, inv)) continue;
                const double* r = ops[i - 4].v;
                double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
                for (int k = 0; k < 4; ++k) {
                    const QPointF p = inv.apply(r[0] + ((k & 1) ? r[2] : 0.0),
                                                r[1] + ((k & 2) ? r[3] : 0.0));
                    u0 = std::min(u0, p.x()); u1 = std::max(u1, p.x());
                    v0 = std::min(v0, p.y()); v1 = std::max(v1, p.y());
                }
                site.wrapper = WrapperKind::LegacyPageClip;
                site.crop = UnitCrop{u0, v0, u1, v1}.clamped();
                site.cropped = !site.crop.isFull();
                site.ctmOuter = ops[i - 5].ctmBefore;
                site.replaceStart = ops[i - 5].start;
                site.replaceEnd = ops[i + 1].end;
            }
        }
    }
};

// The page's content streams in /Contents order (null entries kept so the
// indices match the array).
std::vector<pdf_obj*> contentStreams(fz_context* ctx, pdf_page* page) {
    std::vector<pdf_obj*> v;
    pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
    if (pdf_is_array(ctx, contents)) {
        for (int i = 0, n = pdf_array_len(ctx, contents); i < n; ++i)
            v.push_back(pdf_array_get(ctx, contents, i));
    } else if (contents) {
        v.push_back(contents);
    }
    return v;
}

} // namespace

std::string formatNumber(double v) {
    if (!std::isfinite(v) || std::abs(v) < 5e-7) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

Affine pageTransform(fz_context* ctx, pdf_page* page) {
    fz_rect box;
    fz_matrix t;
    pdf_page_transform(ctx, page, &box, &t);
    return Affine{t.a, t.b, t.c, t.d, t.e, t.f};
}

std::vector<DoSite> scanPage(fz_context* ctx, pdf_page* page, bool recurseForms) {
    std::vector<DoSite> out;
    if (!ctx || !page) return out;
    Scanner sc;
    sc.ctx = ctx;
    sc.recurseForms = recurseForms;
    sc.out = &out;
    Scanner::Scope scope;
    scope.resources = pdf_page_resources(ctx, page);
    const auto streams = contentStreams(ctx, page);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (!streams[i] || !pdf_is_stream(ctx, streams[i])) continue;
        fz_buffer* buf = Scanner::loadStream(ctx, streams[i]);
        sc.runBuffer(scope, buf, static_cast<int>(i));
        fz_drop_buffer(ctx, buf);
    }
    sc.recognise(scope);
    return out;
}

bool rewriteSite(fz_context* ctx, pdf_document* doc, pdf_page* page,
                 const DoSite& site, const Affine& placement, const UnitCrop& crop) {
    if (site.formDepth != 0 || site.stream < 0) return false;
    Affine outerInv;
    if (!invert(site.ctmOuter, outerInv)) return false;
    // cm pre-multiplies: CTM_at_Do = F x CTM_outer, so F = placement x CTM_outer^-1.
    const Affine F = concat(placement, outerInv);
    if (!std::isfinite(F.a) || !std::isfinite(F.b) || !std::isfinite(F.c) ||
        !std::isfinite(F.d) || !std::isfinite(F.e) || !std::isfinite(F.f) ||
        std::abs(F.det()) < 1e-12)
        return false;

    std::string wrapper = "q " + formatNumber(F.a) + " " + formatNumber(F.b) + " " +
                          formatNumber(F.c) + " " + formatNumber(F.d) + " " +
                          formatNumber(F.e) + " " + formatNumber(F.f) + " cm ";
    if (!crop.isFull()) {
        wrapper += formatNumber(crop.u0) + " " + formatNumber(crop.v0) + " " +
                   formatNumber(crop.u1 - crop.u0) + " " + formatNumber(crop.v1 - crop.v0) +
                   " re W n ";
    }
    wrapper += "/" + site.name + " Do Q";
    return spliceSite(ctx, doc, page, site, wrapper);
}

bool removeSite(fz_context* ctx, pdf_document* doc, pdf_page* page, const DoSite& site) {
    if (site.formDepth != 0 || site.stream < 0) return false;
    return spliceSite(ctx, doc, page, site, std::string());
}

namespace {

// Replace bytes [start, end) of the site's content stream with `prefix` (when
// not NUL) followed by `wrapper`, in a NEW stream object. With `pad`, a space
// is put between the replacement and a neighbour that is not white space (a
// wrapper or a removal may otherwise glue two operators together); without it
// the bytes go in exactly as given.
bool spliceRange(fz_context* ctx, pdf_document* doc, pdf_page* page, const DoSite& site,
                 std::size_t start, std::size_t end, char prefix, const std::string& wrapper,
                 bool pad) {
    if (site.formDepth != 0 || site.stream < 0) return false;
    const auto streams = contentStreams(ctx, page);
    if (site.stream >= static_cast<int>(streams.size())) return false;
    pdf_obj* target = streams[site.stream];
    if (!target || !pdf_is_stream(ctx, target)) return false;

    // Every MuPDF call that can throw is inside one fz_try, and no C++ object
    // is created inside it (a longjmp would skip its destructor): the new
    // content is assembled straight into an fz_buffer. The page dict is
    // changed last, so a failure part-way leaves the page as it was.
    //
    // A NEW stream object, and a new /Contents array: the old stream or array
    // may be shared with another page.
    const char* repl = wrapper.c_str();
    const std::size_t replLen = wrapper.size();
    const int padding = pad ? 1 : 0;
    fz_buffer* src = nullptr;
    fz_buffer* nb = nullptr;
    pdf_obj* stm = nullptr;
    pdf_obj* arr = nullptr;
    int ok = 0;
    fz_var(src);
    fz_var(nb);
    fz_var(stm);
    fz_var(arr);
    fz_var(ok);
    fz_try(ctx) {
        src = pdf_load_stream(ctx, target);
        unsigned char* data = nullptr;
        const std::size_t len = fz_buffer_storage(ctx, src, &data);
        if (data && end <= len && start < end) {
            nb = fz_new_buffer(ctx, len + replLen + 4);
            fz_append_data(ctx, nb, data, start);
            if (padding && replLen > 0 && start > 0 && !isWS(data[start - 1]))
                fz_append_byte(ctx, nb, ' ');
            if (prefix) fz_append_byte(ctx, nb, prefix);
            fz_append_data(ctx, nb, repl, replLen);
            if (padding && end < len && !isWS(data[end])) fz_append_byte(ctx, nb, ' ');
            fz_append_data(ctx, nb, data + end, len - end);

            stm = pdf_add_stream(ctx, doc, nb, nullptr, 0);
            pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
            if (pdf_is_array(ctx, contents)) {
                arr = pdf_copy_array(ctx, contents);
                pdf_array_put(ctx, arr, site.stream, stm);
                pdf_dict_put(ctx, page->obj, PDF_NAME(Contents), arr);
            } else {
                pdf_dict_put(ctx, page->obj, PDF_NAME(Contents), stm);
            }
            ok = 1;
        }
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, arr);
        pdf_drop_obj(ctx, stm);
        fz_drop_buffer(ctx, nb);
        fz_drop_buffer(ctx, src);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return ok != 0;
}

} // namespace

bool spliceSite(fz_context* ctx, pdf_document* doc, pdf_page* page,
                const DoSite& site, const std::string& wrapper) {
    return spliceRange(ctx, doc, page, site, site.replaceStart, site.replaceEnd, '\0', wrapper,
                       /*pad=*/true);
}

bool renameSite(fz_context* ctx, pdf_document* doc, pdf_page* page, const DoSite& site,
                const std::string& newName) {
    if (site.formDepth != 0 || site.stream < 0) return false;
    if (newName.empty() || site.nameEnd <= site.nameStart) return false;
    for (const char ch : newName) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 32 || c >= 127 || c == '#' || isDelim(c)) return false;
    }
    // A name starts with the delimiter '/', and the old name ended at white
    // space or a delimiter, so no byte around it needs to change.
    return spliceRange(ctx, doc, page, site, site.nameStart, site.nameEnd, '/', newName,
                       /*pad=*/false);
}

} // namespace pdfe::image::placement
