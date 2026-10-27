// -*- coding: utf-8 -*-
#include "MeasureDictReader.h"

#include "../core/MuPdfContext.h"
#include "../core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────
// THE fz_try DISCIPLINE IN THIS FILE, AND THE BUG THAT MADE IT EXPLICIT
// ─────────────────────────────────────────────────────────────────────────
//
// `MuPdfContext::execute`/`executeReturn` run their lambda body inside MuPDF's
// `fz_try`, which is setjmp/longjmp. A `longjmp` out of that region does NOT
// run destructors for the frames it skips, so ANY local with a non-trivial
// destructor that is alive across a call which can `fz_throw` leaks whatever
// it owns. `tools/gate/fz_try_raii_scan.py` reports exactly that shape, and it
// resolves execute-lambda bodies THROUGH THE CALLEE — so a helper function
// called from the lambda is inside the region just as much as the lambda body
// is.
//
// The first version of this file got that wrong in a way worth recording,
// because the mistake looked like compliance. The lambda body was scrupulously
// clean — one raw `pdf_page*` local, the out-vector constructed by the caller
// outside the guard — but it then called `collectViewports`, which declared
//
//     Viewport view;                                   // std::string + MeasureDict
//     ...
//     view.name = textOr(ctx, pdf_dict_gets(ctx, v, "Name"), "");   // can fz_throw
//     readMeasureDict(ctx, pdf_dict_gets(ctx, v, "Measure"), view.measure);
//
// i.e. a non-trivially-destructible local alive across throwing MuPDF calls,
// one call deep. `readNumberFormat` had the identical shape with
// `NumberFormat f;` held across ten `pdf_dict_gets` calls. Auditing only the
// lambda body is not auditing the guarded region.
//
// THE STRUCTURE THAT FIXES IT, rather than a baseline entry. There are now two
// strictly separated layers:
//
//   1. `Reader` — the ONLY code in this file that touches MuPDF. Every method
//      is a COMPLETE guarded region on its own, and every one returns a value
//      with a TRIVIAL destructor: `pdf_obj*`, `int`, `bool`, `const char*`, or
//      a POD aggregate. Nothing that owns memory is ever alive inside an
//      `fz_try` frame here, so there is nothing for a longjmp to leak.
//
//   2. Everything below `Reader` — UNGUARDED. This is where every
//      `std::string`, `std::vector`, `NumberFormat`, `MeasureDict` and
//      `Viewport` is built. No `fz_try` is anywhere on the stack, so ordinary
//      C++ lifetime rules apply and a `MuPdfException` thrown out of a
//      `Reader` call unwinds normally.
//
// The cost is one setjmp per field read instead of one per page. That is paid
// once when the measure tool is activated on a page, against a `/VP` array
// that is single digits long in any real document, and it buys a rule that can
// be checked BY READING: if a function names `pdf_` anything, it is in
// `Reader`; if it names `std::string`, it is not.
//
// Keys are read with `pdf_dict_gets(ctx, dict, "VP")` rather than
// `PDF_NAME(VP)`: `/VP`, `/RD`, `/RT`, `/PS` and `/SS` are not all in MuPDF's
// static name table, and a mix of the two spellings for one dictionary is the
// kind of inconsistency that reads as deliberate later.
// ─────────────────────────────────────────────────────────────────────────

namespace pdfe::measure {
namespace {

// ── Trivially-destructible carriers ──────────────────────────────────────
//
// Both are aggregates of scalars, so `T result{}` inside `executeReturn` is a
// zero-init and there is nothing to destroy on a longjmp.

struct NumOpt {
    bool has = false;
    double value = 0.0;
};

struct RectOpt {
    bool has = false;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// ── LAYER 1: the guarded primitives. The only MuPDF in this file. ────────

class Reader {
public:
    explicit Reader(const pdfe::core::MuPdfContext& context) : c_(context) {}

    /// The page DICTIONARY (`/VP` lives on it), or null for a non-PDF page.
    pdf_obj* pageDict(::fz_page* page) const
    {
        return c_.executeReturn<pdf_obj*>(
            [page](fz_context* ctx) -> pdf_obj* {
                ::pdf_page* p = pdf_page_from_fz_page(ctx, page);
                return p ? p->obj : nullptr;
            });
    }

    pdf_obj* dictGet(pdf_obj* dict, const char* key) const
    {
        return c_.executeReturn<pdf_obj*>(
            [dict, key](fz_context* ctx) -> pdf_obj* {
                if (!dict || !pdf_is_dict(ctx, dict)) return nullptr;
                return pdf_dict_gets(ctx, dict, key);
            });
    }

    bool isDict(pdf_obj* o) const
    {
        return c_.executeReturn<bool>([o](fz_context* ctx) -> bool {
            return o && pdf_is_dict(ctx, o);
        });
    }

    /// Length of `arr`, or 0 when it is not an array.
    int arrayLen(pdf_obj* arr) const
    {
        return c_.executeReturn<int>([arr](fz_context* ctx) -> int {
            if (!arr || !pdf_is_array(ctx, arr)) return 0;
            return pdf_array_len(ctx, arr);
        });
    }

    pdf_obj* arrayGet(pdf_obj* arr, int i) const
    {
        return c_.executeReturn<pdf_obj*>(
            [arr, i](fz_context* ctx) -> pdf_obj* {
                if (!arr || !pdf_is_array(ctx, arr)) return nullptr;
                return pdf_array_get(ctx, arr, i);
            });
    }

    /// UTF-8 text of a PDF string object, or **null when `o` is not a
    /// string**. The empty string is returned as `""`, NOT as null, so a
    /// caller can still tell an authored `()` from an absent key.
    ///
    /// SAFE TO USE AFTER THE GUARD RETURNS. `pdf_to_text_string` caches its
    /// UTF-8 conversion in `STRING(obj)->text`
    /// (MuPDF 1.26.10 `source/pdf/pdf-object.c:466`), which the OBJECT owns,
    /// so the pointer stays valid for as long as the document does — and the
    /// document outlives every call in `readViewports`. The caller copies it
    /// into a `std::string` outside the guard, which is the whole point.
    const char* textOf(pdf_obj* o) const
    {
        return c_.executeReturn<const char*>(
            [o](fz_context* ctx) -> const char* {
                if (!o || !pdf_is_string(ctx, o)) return nullptr;
                return pdf_to_text_string(ctx, o);
            });
    }

    /// Name of a PDF name object, or null when `o` is not a name. Same
    /// lifetime argument as textOf(): the name table owns the bytes.
    const char* nameOf(pdf_obj* o) const
    {
        return c_.executeReturn<const char*>(
            [o](fz_context* ctx) -> const char* {
                if (!o || !pdf_is_name(ctx, o)) return nullptr;
                return pdf_to_name(ctx, o);
            });
    }

    NumOpt numberOf(pdf_obj* o) const
    {
        return c_.executeReturn<NumOpt>([o](fz_context* ctx) -> NumOpt {
            NumOpt n;
            if (o && pdf_is_number(ctx, o)) {
                n.has = true;
                n.value = static_cast<double>(pdf_to_real(ctx, o));
            }
            return n;
        });
    }

    /// A four-element PDF rectangle, normalised by `pdf_to_rect`.
    RectOpt rectOf(pdf_obj* o) const
    {
        return c_.executeReturn<RectOpt>([o](fz_context* ctx) -> RectOpt {
            RectOpt r;
            if (!o || !pdf_is_array(ctx, o) || pdf_array_len(ctx, o) != 4) {
                return r;
            }
            const fz_rect box = pdf_to_rect(ctx, o);
            r.has = true;
            r.x0 = box.x0; r.y0 = box.y0; r.x1 = box.x1; r.y1 = box.y1;
            return r;
        });
    }

private:
    const pdfe::core::MuPdfContext& c_;
};

// ── LAYER 2: UNGUARDED. Everything that owns memory lives below here. ────

/// A string object's text, or @p fallback when the key was absent or was not
/// a string. An authored EMPTY string is preserved as empty — `/PS ()` is a
/// deliberate "no prefix", not a missing key.
std::string textOr(const char* s, const char* fallback)
{
    if (s) return std::string(s);
    return fallback ? std::string(fallback) : std::string();
}

/// A name object's name, or @p fallback when absent, not a name, OR empty.
/// Unlike a string, an empty name carries no meaning worth preserving.
std::string nameOr(const char* s, const char* fallback)
{
    if (s && *s) return std::string(s);
    return fallback ? std::string(fallback) : std::string();
}

NumberFormat readNumberFormat(const Reader& r, pdf_obj* nf)
{
    NumberFormat f;
    if (!r.isDict(nf)) return f;

    f.unitLabel = textOr(r.textOf(r.dictGet(nf, "U")), "");

    const NumOpt c = r.numberOf(r.dictGet(nf, "C"));
    if (c.has) {
        f.conversionFactor = c.value;
        f.hasConversionFactor = true;
    }

    f.fractionMode = nameOr(r.nameOf(r.dictGet(nf, "F")), "D");

    const NumOpt d = r.numberOf(r.dictGet(nf, "D"));
    if (d.has) f.precisionDenominator = d.value;

    f.decimalSeparator   = textOr(r.textOf(r.dictGet(nf, "RD")), ".");
    f.thousandsSeparator = textOr(r.textOf(r.dictGet(nf, "RT")), ",");
    f.prefix             = textOr(r.textOf(r.dictGet(nf, "PS")), "");
    f.suffix             = textOr(r.textOf(r.dictGet(nf, "SS")), "");
    f.labelPosition      = nameOr(r.nameOf(r.dictGet(nf, "O")), "S");
    return f;
}

void readNumberFormatArray(const Reader& r, pdf_obj* arr,
                           std::vector<NumberFormat>& out)
{
    out.clear();
    const int n = r.arrayLen(arr);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        pdf_obj* nf = r.arrayGet(arr, i);
        if (!r.isDict(nf)) continue;
        out.push_back(readNumberFormat(r, nf));
    }
}

MeasureDict readMeasureDict(const Reader& r, pdf_obj* m)
{
    MeasureDict out;
    if (!r.isDict(m)) return out;

    out.present = true;
    out.subtype = nameOr(r.nameOf(r.dictGet(m, "Subtype")), "RL");
    out.ratio   = textOr(r.textOf(r.dictGet(m, "R")), "");
    readNumberFormatArray(r, r.dictGet(m, "X"), out.x);
    readNumberFormatArray(r, r.dictGet(m, "D"), out.d);
    readNumberFormatArray(r, r.dictGet(m, "A"), out.a);

    // `valid` is the "everything scaleFrom() needs is here" predicate, and it
    // is deliberately strict about /C: a number format whose /C is ABSENT is
    // not the same as one that says 1.0. §12.9 makes /C required, and
    // defaulting a missing conversion factor to 1 would silently measure an
    // architectural drawing at 1:1 — the exact defect this closes.
    out.valid = (out.subtype == "RL")
                && !out.ratio.empty()
                && !out.x.empty() && out.x.front().hasConversionFactor
                && !out.x.front().unitLabel.empty()
                && !out.d.empty() && out.d.front().hasConversionFactor
                && !out.d.front().unitLabel.empty();
    return out;
}

} // namespace

MeasureDictReader::MeasureDictReader(
    std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context))
{
}

MeasureDictReader::~MeasureDictReader() = default;

std::vector<Viewport> MeasureDictReader::readViewports(
    const pdfe::core::Page* page) const
{
    std::vector<Viewport> out;
    if (!page || !context_) return out;

    ::fz_page* fzPage = page->fzPage();
    if (!fzPage) return out;

    // From here down NOTHING is inside an fz_try frame: every MuPDF touch goes
    // through `Reader`, which opens and closes its own guarded region per
    // call. `view` and everything it owns are therefore ordinary C++ locals.
    const Reader r(*context_);

    pdf_obj* pageObj = r.pageDict(fzPage);
    if (!pageObj) return out;

    pdf_obj* vp = r.dictGet(pageObj, "VP");
    const int n = r.arrayLen(vp);
    out.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        pdf_obj* v = r.arrayGet(vp, i);
        if (!r.isDict(v)) continue;

        Viewport view;
        const RectOpt bbox = r.rectOf(r.dictGet(v, "BBox"));
        if (bbox.has) {
            // pdf_to_rect already normalised it (x0 <= x1, y0 <= y1).
            view.bbox = QRectF(QPointF(bbox.x0, bbox.y0),
                               QPointF(bbox.x1, bbox.y1));
        }
        view.name = textOr(r.textOf(r.dictGet(v, "Name")), "");
        view.measure = readMeasureDict(r, r.dictGet(v, "Measure"));
        out.push_back(std::move(view));
    }
    return out;
}

std::optional<Viewport> MeasureDictReader::viewportAt(
    const std::vector<Viewport>& viewports, QPointF pointInUserSpace)
{
    // §12.9: when a point falls in more than one viewport, the LAST one in the
    // array wins. Scanning forwards would return the first, which is the
    // opposite answer on precisely the documents where it matters (a detail
    // callout drawn on top of a site plan at a different scale).
    for (auto it = viewports.rbegin(); it != viewports.rend(); ++it) {
        if (!it->measure.valid) continue;
        if (it->bbox.contains(pointInUserSpace)) return *it;
    }
    return std::nullopt;
}

const NumberFormat* MeasureDictReader::formatForMode(const MeasureDict& m,
                                                     MeasureMode mode)
{
    // Distance and Perimeter are both "distance in any direction" -> /D.
    // Area -> /A. /X is change along the x axis and is reached through
    // xAxisFormat(), not through a measurement mode.
    const std::vector<NumberFormat>& arr =
        (mode == MeasureMode::Area) ? m.a : m.d;
    return arr.empty() ? nullptr : &arr.front();
}

const NumberFormat* MeasureDictReader::xAxisFormat(const MeasureDict& m)
{
    return m.x.empty() ? nullptr : &m.x.front();
}

Scale MeasureDictReader::scaleFrom(const MeasureDict& m, MeasureMode mode)
{
    Scale s;
    if (!m.valid) return s;   // 1:1, no label — the pre-#21 behaviour.

    const NumberFormat* nf = formatForMode(m, mode);
    if (!nf || !nf->hasConversionFactor) return s;

    // displayUnit stays Points: /C converts DEFAULT USER SPACE UNITS (which
    // are points) directly into /U's unit, so any additional pt->in step here
    // would double-convert. See the header.
    s.displayUnit = Unit::Points;
    s.scaleFactor = static_cast<float>(nf->conversionFactor);
    s.unitLabel = nf->unitLabel;
    s.ratioLabel = m.ratio;
    s.precisionDenominator = nf->precisionDenominator;
    // /A's /C converts SQUARE user-space units directly to /U, so it is
    // already areal and MeasureCalc::format() must not square it again.
    s.scaleFactorIsAreal = (mode == MeasureMode::Area);
    return s;
}

} // namespace pdfe::measure
