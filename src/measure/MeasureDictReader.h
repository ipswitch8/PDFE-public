// -*- coding: utf-8 -*-
#pragma once

// GitHub #19's sibling: GitHub #21 / gap-register PDF-57 — the READ side of
// ISO 32000-1 §12.9 "Measurement Properties".
//
// WHAT WAS THERE BEFORE. Nothing. `src/measure/` implemented an interactive
// ruler whose scale the USER types in (MeasureTool::setScale), and a grep of
// every .cpp/.h in the tree for `/VP`, `/Viewport` or `/Measure` returned only
// the English word "measured" in comments. A PDF that states its own scale —
// an architectural drawing, a site plan, a map — was measured at 1:1 against
// default user space, silently, because nothing ever asked the page what its
// units were.
//
// WHAT §12.9 SAYS, in the order this file reads it.
//
//   * A page may carry `/VP`, an array of VIEWPORT dictionaries (Table 265).
//     Each has a `/BBox` (the region of the page it applies to, in default
//     user space), an optional `/Name`, and an optional `/Measure`.
//   * A `/Measure` dictionary (Table 264) of `/Subtype /RL` (rectilinear —
//     the only subtype ISO 32000-1 defines) carries:
//       - `/R`, a REQUIRED text string giving the scale ratio in a form a
//         human reads, e.g. "1 in = 10 ft". Display-only by specification.
//       - `/X`, a REQUIRED array of number format dictionaries for measuring
//         change along the x axis (and the y axis too, when `/Y` is absent).
//       - `/D`, a REQUIRED array for measuring DISTANCE in any direction.
//       - `/A`, a REQUIRED array for measuring AREA.
//   * A NUMBER FORMAT dictionary (Table 266) carries `/U` (the unit label),
//     `/C` (the conversion factor from the previous element's unit — from
//     default user space units, for the FIRST element of an array), `/F` (the
//     fraction display mode), `/D` (the precision, as the denominator of the
//     smallest fraction), and the presentational `/RD`, `/RT`, `/PS`, `/SS`,
//     `/O`.
//
// THE POINT OF THE READ SIDE IS THE DISPLAYED VALUE, so this header's real
// product surface is `scaleFrom()`: it turns a `/Measure` into the
// `pdfe::measure::Scale` that `MeasureCalc::format()` already uses to compute
// `MeasureResult::displayedValue`. A reader that parsed `/C` into a struct
// nobody consulted would leave the defect exactly where it was.

#include "MeasureTypes.h"

#include <QRectF>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// No MuPDF forward declarations here, deliberately. Nothing in this interface
// names a `fz_context` or a `pdf_obj` any more — every MuPDF touch is confined
// to the guarded `Reader` layer inside MeasureDictReader.cpp. A caller that
// cannot name a MuPDF type cannot be tempted to run this parse inside an
// `fz_try` region, which is the bug this file already had once.

namespace pdfe::core {
class MuPdfContext;
class Page;
}

namespace pdfe::measure {

/// One number format dictionary — ISO 32000-1 Table 266.
struct NumberFormat {
    /// `/U` — the label for the unit this element measures in ("ft", "m").
    /// Required by the specification; empty means it was absent or not a
    /// string, which `MeasureDict::valid` reports.
    std::string unitLabel;

    /// `/C` — the conversion factor. For the FIRST element of an array this
    /// multiplies DEFAULT USER SPACE UNITS to give `unitLabel` units; for a
    /// later element it multiplies the PREVIOUS element's unit.
    double conversionFactor = 1.0;
    bool hasConversionFactor = false;

    /// `/F` — fraction display: "D" decimal (the default), "F" fraction,
    /// "R" round to nearest, "T" truncate.
    std::string fractionMode = "D";

    /// `/D` — precision, as the DENOMINATOR of the smallest fraction shown.
    /// 100 means hundredths. Specification default is 100.
    double precisionDenominator = 100.0;

    /// `/RD` decimal separator, `/RT` thousands separator, `/PS` prefix,
    /// `/SS` suffix. Presentational; read so a round-trip can be proved on
    /// the whole dictionary rather than only the parts the maths uses.
    std::string decimalSeparator = ".";
    std::string thousandsSeparator = ",";
    std::string prefix;
    std::string suffix;

    /// `/O` — label position: "S" suffix (default) or "P" prefix.
    std::string labelPosition = "S";

    bool operator==(const NumberFormat&) const = default;
};

/// A `/Measure` dictionary — ISO 32000-1 Table 264.
struct MeasureDict {
    bool present = false;
    /// `/Subtype`. "RL" (rectilinear) is the only one ISO 32000-1 defines and
    /// is the default when the key is absent.
    std::string subtype = "RL";
    /// `/R` — the scale ratio, verbatim.
    std::string ratio;
    std::vector<NumberFormat> x;  ///< `/X`
    std::vector<NumberFormat> d;  ///< `/D`
    std::vector<NumberFormat> a;  ///< `/A`

    /// True iff this is a rectilinear measure carrying a non-empty `/R` and a
    /// usable first element in both `/X` and `/D` — i.e. everything
    /// `scaleFrom()` needs. Deliberately NOT the same thing as `present`: a
    /// malformed `/Measure` must be distinguishable from an absent one, or a
    /// caller cannot tell "the author said 1:1" from "the author said
    /// nothing".
    bool valid = false;

    bool operator==(const MeasureDict&) const = default;
};

/// A page `/Viewport` — ISO 32000-1 Table 265.
struct Viewport {
    QRectF bbox;        ///< `/BBox`, in default user space.
    std::string name;   ///< `/Name`, may be empty.
    MeasureDict measure;

    bool operator==(const Viewport&) const = default;
};

/// Reads the measurement properties a page states about itself.
///
/// Read-only: nothing here writes a `/VP` or a `/Measure`. The criterion this
/// closes is about the READ side driving the displayed value and surviving a
/// save/reopen; PDFE's existing save preserves the page dictionary, so a
/// writer is not needed to prove the round-trip and would be untested surface.
class MeasureDictReader {
public:
    explicit MeasureDictReader(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~MeasureDictReader();

    MeasureDictReader(const MeasureDictReader&) = delete;
    MeasureDictReader& operator=(const MeasureDictReader&) = delete;

    /// Every `/Viewport` on @p page, in `/VP` order. Empty when the page has
    /// no `/VP`, when it is not a PDF page, or when @p page is null.
    [[nodiscard]] std::vector<Viewport> readViewports(
        const pdfe::core::Page* page) const;

    /// The first viewport whose `/BBox` contains @p pointInUserSpace and which
    /// carries a valid `/Measure`. §12.9: "If the point is in more than one
    /// viewport, the one that appears LAST in the array shall be used" — so
    /// this scans the array in reverse, which is not the obvious direction and
    /// is the reason this helper exists rather than being open-coded.
    [[nodiscard]] static std::optional<Viewport> viewportAt(
        const std::vector<Viewport>& viewports, QPointF pointInUserSpace);

    // ── The bridge to the displayed value ───────────────────────────────

    /// The number format array that governs @p mode:
    /// Distance and Perimeter measure distance -> `/D`; Area -> `/A`.
    /// Returns nullptr when the governing array is empty.
    [[nodiscard]] static const NumberFormat* formatForMode(
        const MeasureDict& m, MeasureMode mode);

    /// The number format governing change along the x axis -> `/X`.
    [[nodiscard]] static const NumberFormat* xAxisFormat(const MeasureDict& m);

    /// Turn @p m into the Scale that `MeasureCalc::format()` consumes, for a
    /// measurement of @p mode.
    ///
    /// `displayUnit` is left at `Unit::Points` DELIBERATELY: `/C` already
    /// converts default user space units (which ARE points, by the definition
    /// of default user space) straight into `/U`'s unit, so applying a second
    /// pt->in conversion on top would double-convert. `/U` supplies the
    /// suffix, `/D` the precision, `/R` the ratio label.
    ///
    /// Returns a default (1:1, no label) Scale when @p m is not valid, so a
    /// caller that does not check `valid` degrades to the pre-#21 behaviour
    /// rather than to nonsense.
    [[nodiscard]] static Scale scaleFrom(const MeasureDict& m, MeasureMode mode);

    // A `static NumberFormat readNumberFormat(fz_context*, pdf_obj*)` was
    // exported here at first, "for the round-trip test". It was WITHDRAWN, and
    // for two reasons worth stating so it is not reintroduced:
    //
    //   1. It had zero callers. The round-trip oracle compares whole `Viewport`
    //      values and never needed it. Dead public API that looks maintained is
    //      what the next caller reaches for — the same argument
    //      `SignatureManager.h` records for deleting `saveIncremental()`.
    //   2. Its documented contract was "safe to call from inside a
    //      `MuPdfContext::execute` lambda", and that contract was the bug. Any
    //      function that builds a `NumberFormat` — which owns five
    //      `std::string`s — while MuPDF's setjmp/longjmp `fz_try` is on the
    //      stack leaks all of them on an `fz_throw`. Parsing now happens
    //      strictly OUTSIDE the guarded region; see the discipline block at the
    //      top of MeasureDictReader.cpp.

private:
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace pdfe::measure
