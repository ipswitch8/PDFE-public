#pragma once

#include "FormFieldType.h"
#include <QRectF>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace pdfe::forms {

/// A widget's border and background, as PDF 32000-1 stores them: the colours
/// in the appearance-characteristics dictionary (/MK /BC and /MK /BG, table
/// 189) and the border style dictionary (/BS /W and /BS /S, table 166).
///
/// GitHub #458: MuPDF synthesises a widget's border and background ONLY from
/// these entries, so a field without them renders as nothing at all.
///
/// Every member is trivially destructible, so the struct may be captured into
/// a MuPdfContext::execute lambda (fz_try discipline).
struct FormFieldBorderAppearance {
    /// /MK /BC length: 0 = absent (no border colour, so no border is drawn),
    /// 1 = gray, 3 = RGB, 4 = CMYK.
    int borderColorComponents = 0;
    std::array<float, 4> borderColor{};
    /// /MK /BG length, same encoding as borderColorComponents.
    int backgroundColorComponents = 0;
    std::array<float, 4> backgroundColor{};
    /// Whether the widget has a /BS dictionary at all.
    bool hasBorderStyle = false;
    /// /BS /W (PDF default 1 when absent from a present /BS).
    float borderWidth = 1.0f;
    /// /BS /S: 'S' solid, 'D' dashed, 'B' beveled, 'I' inset, 'U' underline.
    char borderStyle = 'S';
};

// ─── Hostile-input limits (pipeline form-textfield, phase-3 security audit) ──
//
// Every number below comes from the PDF and is attacker-controlled. They are
// clamped where they are READ (FormManager readDefaultAppearance /
// readBorderAppearance) and again where they are USED (FormTool's live edit
// paint), so neither a crafted file nor a hand-built FormFieldData can push a
// non-finite or absurd value into a float->int conversion or a layout loop.

/// Largest /DA Tf size kept, in points. MuPDF's own appearance writer takes
/// the size as given; 1000 pt is far beyond any real field (a 1000 pt line is
/// almost 14 inches tall) and keeps size x zoom comfortably inside int range.
inline constexpr float kMaxDefaultAppearanceSizePt = 1000.0f;

/// The /DA Tf size as the live paint may use it. Non-finite values (NaN and
/// +/-inf are all reachable: MuPDF's /DA parser runs fz_strtof, which accepts
/// "nan" and "inf" and overflows "1e50" to inf) and negative sizes become 0,
/// i.e. auto-size, the treatment PDF 32000-1 12.7.3.3 gives an explicit 0.
/// Positive sizes are capped at kMaxDefaultAppearanceSizePt.
inline float sanitizeDefaultAppearanceSize(float size) {
    if (!std::isfinite(size) || size <= 0.0f) return 0.0f;
    return std::min(size, kMaxDefaultAppearanceSizePt);
}

/// Largest /BS /W kept, in points.
inline constexpr float kMaxBorderWidthPt = 100.0f;

/// /BS /W clamped to [0, kMaxBorderWidthPt]; non-finite becomes the PDF
/// default of 1 (PDF 32000-1 table 166).
inline float sanitizeBorderWidth(float width) {
    if (!std::isfinite(width)) return 1.0f;
    return std::clamp(width, 0.0f, kMaxBorderWidthPt);
}

/// One /MK colour component clamped to [0, 1]; non-finite becomes 0.
inline float sanitizeColorComponent(float c) {
    if (!std::isfinite(c)) return 0.0f;
    return std::clamp(c, 0.0f, 1.0f);
}

/// A colour component count as /MK and /DA allow it: 1 (gray), 3 (RGB) or
/// 4 (CMYK); anything else is 0 (absent). It is also the bound that keeps a
/// loop over a std::array<float, 4> in range.
inline int sanitizeColorComponentCount(int n) {
    return (n == 1 || n == 3 || n == 4) ? n : 0;
}

// ─── GitHub #460: setting a field's default appearance ──────────────────────

/// The /DA Tf size FormManager::setFieldDefaultAppearance accepts: 0 (auto
/// size, PDF 32000-1 12.7.3.3) or 1..kMaxDefaultAppearanceSizePt points, the
/// same cap the phase-3 read sanitiser applies. Anything else -- NaN, +/-inf,
/// negative, between 0 and 1, above the cap -- is rejected, not clamped: a
/// setter that silently wrote a different size than asked would be a lie.
inline bool isAcceptedDefaultAppearanceSize(float size) {
    if (!std::isfinite(size)) return false;
    if (size == 0.0f) return true;
    return size >= 1.0f && size <= kMaxDefaultAppearanceSizePt;
}

/// The /DA font keys FormManager::setFieldDefaultAppearance accepts: the
/// base-14 resource names MuPDF 1.26.10 resolves without a /DR entry
/// (pdf_add_simple_font's standard names, as pdf-appearance.c maps them):
/// Helv (Helvetica), TiRo (Times-Roman), Cour (Courier), Symb (Symbol) and
/// ZaDb (ZapfDingbats). Exact, case-sensitive match; anything else, including
/// a name with a '/' or a space in it, is rejected.
///
/// The bold/italic variant keys (HeBo, HeOb, HeBO, TiBo, TiIt, TiBI, CoBo,
/// CoOb, CoBO) are rejected too: MuPDF 1.26.10 renders each of them as the
/// regular face (see FormManager::setFieldDefaultAppearance), so accepting
/// one would write a /DA that claims a style the field never shows.
inline bool isBase14DefaultAppearanceFont(const std::string& font) {
    return font == "Helv" || font == "TiRo" || font == "Cour" ||
           font == "Symb" || font == "ZaDb";
}

/// Longest /DA string FormFieldData::defaultAppearanceString carries verbatim.
/// getFormField() runs on every live-edit repaint, so a crafted multi-megabyte
/// /DA must not be copied each time. MuPDF 1.26.10's own /DA parser reads only
/// the first 99 bytes (pdf_parse_default_appearance_unmapped copies into a
/// char[100]), so a longer string renders exactly like its parsed values;
/// past this cap the parsed values are captured instead, re-printed as
/// "/<font> <size> Tf <colour>".
inline constexpr std::size_t kMaxCapturedDefaultAppearanceBytes = 4096;

/// A widget's OWN default-appearance entries, byte for byte, as
/// SetFieldAppearanceCommand captures them for undo. pdf_set_annot_default_
/// appearance (MuPDF 1.26.10) writes /DA and DELETES /DS and /RC, so all three
/// are kept: undo puts back exactly what the widget dictionary held. Only
/// string values are captured; a non-string /DA, /DS or /RC (not valid PDF)
/// is recorded as absent, so undo removes the entry.
struct FormFieldOwnDefaultAppearance {
    bool hasDa = false;
    std::string da;
    bool hasDs = false;
    std::string ds;
    bool hasRc = false;
    std::string rc;
};

/// Qt-friendly value object representing a PDF form field's state.
///
/// Analogous to AnnotationData in the annotations module.
/// No MuPDF types leak through this interface.
struct FormFieldData {
    FormFieldType type = FormFieldType::Unknown;
    QRectF rect;                                 // Bounding rectangle in PDF coords
    std::string fieldName;                       // Field name (pdf_annot_field_label)
    std::string value;                           // Current value (pdf_annot_field_value)
    int fieldFlags = 0;                          // Raw PDF field flags

    // Choice widget data (combo/list)
    std::vector<std::string> options;            // Display strings
    std::vector<std::string> exportValues;       // Export strings
    std::vector<int> selectedIndices;            // Currently selected options
    bool isMultiSelect = false;

    // Derived convenience flags (from fieldFlags)
    bool isReadOnly = false;
    bool isRequired = false;
    bool isMultiline = false;                    // Text field
    bool isPassword = false;                     // Text field
    bool isComb = false;                         // Text field with fixed-width cells
    bool isEditable = false;                     // Combo box with edit capability

    // PDF-44 / GitHub #16. /MaxLen for a text field (0 = absent, i.e. no
    // limit). Inheritable, so it is read with pdf_dict_get_inheritable_int.
    // For a COMB field this is not merely a length cap: PDF 32000-1 12.7.4.3
    // makes it the CELL COUNT — the field width is divided into /MaxLen equal
    // cells with one character in each — so a value longer than /MaxLen has
    // no defined position and overflows its own comb.
    int maxLen = 0;                              // Text field

    // GitHub #458. The widget's /MK /BC, /MK /BG and /BS as read from the
    // document. borderAppearanceCaptured is true for data read from a widget
    // (getFormField / deleteFormField); restoreFormField then rebuilds exactly
    // that border and background, so undo of a delete keeps the appearance of
    // a field loaded from a file. Data assembled by hand leaves it false, and
    // restoreFormField falls back to the appearance a new field gets.
    bool borderAppearanceCaptured = false;
    FormFieldBorderAppearance borderAppearance;

    // GitHub #459. The widget's default appearance (/DA, PDF 32000-1 12.7.3.3),
    // as MuPDF's pdf_annot_default_appearance parses it: inherited from the
    // field's parents, else the AcroForm /DA, else "Helv 12" with no colour.
    // FormTool paints the in-progress edit with these while the field is
    // being typed into, so the text on screen matches what commit will write.
    //
    // The BACKGROUND colour of that live paint is borderAppearance's
    // backgroundColor (/MK /BG, captured above for GitHub #458);
    // backgroundColorComponents == 0 means the widget has none (paint white).
    //
    // daFontName is MuPDF's mapped base-14 resource name: "Helv", "TiRo",
    // "Cour", "Symb" or "ZaDb" (an unknown name maps to "Helv").
    std::string daFontName = "Helv";
    /// /DA Tf size in points. 0 means auto-size (fit the field). Stored as
    /// sanitizeDefaultAppearanceSize() leaves it: finite, in [0, 1000].
    float daFontSize = 12.0f;
    /// /DA colour component count: 0 = none (black), 1 = g, 3 = rg, 4 = k.
    int daColorComponents = 0;
    std::array<float, 4> daColor{};
    /// /Q quadding: 0 left, 1 centred, 2 right. Read with MuPDF's
    /// pdf_annot_quadding -- the same accessor its appearance writer uses
    /// for the committed value, so the live edit aligns exactly as commit
    /// will. (MuPDF 1.26.10 reads /Q from the widget dictionary itself, not
    /// through /Parent; following the writer is what prevents a jump.
    /// Pinned by FormFieldEditPaintParentQuadding.*, which also fails if a
    /// MuPDF update starts inheriting /Q in the committed appearance.)
    int quadding = 0;

    // GitHub #460. The /DA string in effect for the widget, byte for byte: the
    // widget's own /DA, else the nearest /Parent's, else the AcroForm /DA --
    // the same lookup pdf_annot_default_appearance makes. Captured for text,
    // combo and list widgets that have one (defaultAppearanceStringCaptured).
    // restoreFormField writes it back as the restored widget's own /DA, so
    // delete+undo and undo/redo of create keep the font, size and colour --
    // including a /DA naming a non-base-14 /DR font, which the parsed
    // daFontName above cannot carry (MuPDF maps an unknown name to "Helv").
    // Data assembled by hand leaves it false, and restoreFormField writes the
    // app default "/Helv 12 Tf 0 g". A /DA longer than
    // kMaxCapturedDefaultAppearanceBytes is captured as its parsed values
    // (see that constant).
    bool defaultAppearanceStringCaptured = false;
    std::string defaultAppearanceString;

    // Tracking
    int pageIndex = -1;
    int widgetIndex = -1;                        // Index in page's widget list
};

} // namespace pdfe::forms
