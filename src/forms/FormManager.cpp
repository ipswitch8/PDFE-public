#include "FormManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <stdexcept>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace pdfe::forms {

// Pipeline b3d7f942 phase-1 remediation (GAP-16 AC3): per-widget Tx reset
// logic extracted into a named function with external linkage so that a
// GoogleTest can call it directly with a null fieldObj, exercising the null
// guard on the code path that would otherwise only be reachable via a
// malformed PDF (where pdf_annot_obj() returns NULL).
//
// The function lives in pdfe::forms::detail so it does not pollute the public
// FormManager API.  Tests include FormManagerTestSeams.h to get the
// declaration without touching FormManager.h.
namespace detail {

// Returns true if fieldObj was non-null and the widget was processed (even if
// no /DV reset was needed), false if the null guard fired and processing was
// skipped.  The bool return is used by the GoogleTest seam to verify which
// branch was taken, making the guard mechanically testable without relying on
// a crash.
bool resetTxWidgetFromObj(fz_context* ctx, pdf_document* pdfDoc, pdf_obj* fieldObj) {
    // Guard against widgets whose annotation object pointer is null
    // (e.g. malformed PDFs with a /Annots entry that has no corresponding
    // annotation dictionary).  Without this guard pdf_dict_get is called with
    // a null pdf_obj*; MuPDF 1.26 happens to be null-safe there, but the
    // behaviour is undefined by the MuPDF API contract and may change in
    // future versions.  Returning false here lets the test seam verify the
    // guard fires and skips processing.
    if (!fieldObj) {
        return false;
    }
    pdf_obj* ftObj = pdf_dict_get(ctx, fieldObj, PDF_NAME(FT));
    const char* ftName = pdf_is_name(ctx, ftObj) ? pdf_to_name(ctx, ftObj) : "";
    if (strcmp(ftName, "Tx") == 0) {
        // Text field: copy /DV (a string) to /V via pdf_set_field_value.
        // MuPDF 1.26's pdf_reset_form does not reliably restore /Tx /V to
        // /DV when the value was mutated via pdf_set_field_value.
        pdf_obj* dvObj = pdf_dict_get(ctx, fieldObj, PDF_NAME(DV));
        if (dvObj) {
            const char* dvStr = pdf_is_string(ctx, dvObj)
                ? pdf_to_str_buf(ctx, dvObj)
                : "";
            pdf_set_field_value(ctx, pdfDoc, fieldObj,
                                 dvStr ? dvStr : "", 1);
        }
    } else if (strcmp(ftName, "Btn") == 0) {
        // Button field (checkbox / radio): /DV and /V are Names (e.g. /Yes,
        // /Off). MuPDF 1.26's pdf_reset_form resets buttons to /Off rather
        // than to their /DV, so apply /DV → /V manually when /DV is present.
        // pdf_set_field_value with a name string updates /V and the widget
        // /AS appearance state. Skip pushbuttons (no /DV, transient).
        pdf_obj* dvObj = pdf_dict_get(ctx, fieldObj, PDF_NAME(DV));
        if (dvObj && pdf_is_name(ctx, dvObj)) {
            const char* dvName = pdf_to_name(ctx, dvObj);
            if (dvName && dvName[0] != '\0') {
                pdf_set_field_value(ctx, pdfDoc, fieldObj, dvName, 1);
            }
        }
    }
    return true;
}

} // namespace detail

// F15 / PDF-41 + PDF-42 helpers ------------------------------------------
//
// A filled field whose widget carries no appearance stream renders BLANK in
// every viewer that does not synthesise appearances itself. The two halves of
// the contract are: (a) regenerate the appearance when the value changes, and
// (b) if we could not, tell the viewer to do it by writing
// /AcroForm /NeedAppearances true. Neither half existed before F15.

// True when the widget has a non-empty /AP /N appearance (either a stream, or
// an appearance sub-dictionary with at least one stream state).
static bool widgetHasNonEmptyAppearance(fz_context* ctx, pdf_annot* widget) {
    pdf_obj* obj = pdf_annot_obj(ctx, widget);
    if (!obj) return false;
    pdf_obj* ap = pdf_dict_get(ctx, obj, PDF_NAME(AP));
    if (!ap) return false;
    pdf_obj* n = pdf_dict_get(ctx, ap, PDF_NAME(N));
    if (!n) return false;

    if (!pdf_is_stream(ctx, n)) {
        const int len = pdf_dict_len(ctx, n);
        for (int i = 0; i < len; ++i) {
            if (pdf_is_stream(ctx, pdf_dict_get_val(ctx, n, i))) {
                return true;
            }
        }
        return false;
    }

    fz_buffer* buf = NULL;
    size_t len = 0;
    fz_try(ctx) {
        buf = pdf_load_stream(ctx, n);
        len = fz_buffer_storage(ctx, buf, NULL);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        len = 0;
    }
    return len > 0;
}

// Write /AcroForm /NeedAppearances. Creates the AcroForm dict if absent only
// when setting true (there is nothing to say otherwise).
static void writeNeedAppearances(fz_context* ctx, pdf_document* pdfDoc, int value) {
    if (!pdfDoc) return;
    pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    if (!root) return;
    pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
    if (!acroForm) {
        if (!value) return;
        acroForm = pdf_dict_put_dict(ctx, root, PDF_NAME(AcroForm), 2);
    }
    pdf_dict_puts(ctx, acroForm, "NeedAppearances",
                  value ? PDF_TRUE : PDF_FALSE);
}

// ─── Pathological /RV (rich text) guard ─────────────────────────────────────
//
// A widget's /RV value is an XHTML fragment. MuPDF's Gumbo HTML parser builds
// and tears down the /RV DOM with UNBOUNDED native recursion, so a value nested
// thousands of tags deep (e.g. 20000 `<b>`) or many megabytes long overflows
// the stack (STATUS_STACK_OVERFLOW) the moment pdf_update_annot tries to
// generate a rich-text appearance. There is no MuPDF-side depth limit to lean
// on, so we must refuse the content ourselves BEFORE it reaches the parser.
//
// Caps are deliberately generous: real rich-text field values are a handful of
// nested spans and a few kilobytes. On breach we skip appearance generation and
// fall back to /NeedAppearances (the same fail-safe used when an appearance
// cannot be produced), which is exactly what a viewer needs to render the field.
static constexpr size_t kMaxRichTextValueBytes = 1u << 20;  // 1 MiB
static constexpr int kMaxRichTextTagDepth = 256;            // nesting depth cap

// True when the field's /RV rich-text value is too large or too deeply nested
// to hand to MuPDF's recursive HTML parser safely. Reads /RV as a C string and
// walks it once, tracking tag-nesting depth; performs no C++ allocation that
// could throw inside the surrounding fz_try region.
static bool richTextValueIsPathological(fz_context* ctx, pdf_obj* fieldObj) {
    if (!fieldObj) return false;
    pdf_obj* rvObj = pdf_dict_get(ctx, fieldObj, PDF_NAME(RV));
    if (!rvObj) return false;
    const char* rv = pdf_dict_get_text_string(ctx, fieldObj, PDF_NAME(RV));
    if (!rv) return false;

    int depth = 0;
    size_t i = 0;
    for (; rv[i] != '\0'; ++i) {
        if (i >= kMaxRichTextValueBytes) {
            return true;  // length cap breached
        }
        if (rv[i] != '<') continue;
        const char next = rv[i + 1];
        if (next == '/') {
            if (depth > 0) --depth;  // closing tag
        } else if (next == '!' || next == '?') {
            // comment / declaration / processing instruction: not a nesting tag
        } else {
            // Opening tag; a self-closing "<.../>" does not change net depth, so
            // only count it if it is not self-closed before the next '>'.
            bool selfClosing = false;
            for (size_t j = i + 1; rv[j] != '\0' && rv[j] != '>'; ++j) {
                if (rv[j] == '/' && rv[j + 1] == '>') { selfClosing = true; break; }
            }
            if (!selfClosing && ++depth > kMaxRichTextTagDepth) {
                return true;  // depth cap breached
            }
        }
    }
    return false;
}

// Helper: Get the nth widget from a page
static pdf_annot* getNthWidget(fz_context* ctx, pdf_page* page, int index) {
    // DEFECT FIX (meta-audit follow-up F17, gap A1-34): a NEGATIVE index used
    // to fall straight out of the loop and return widget 0, so the
    // `if (!widget) fz_throw(...)` guards in getFormField / setFieldValue /
    // toggleField / deleteFormField never fired and all four silently operated
    // on the FIRST field on the page. That is reachable in production:
    // FormTool carries `int selectedWidget_ = -1` as its "nothing selected"
    // sentinel, so a call made before a selection could read, overwrite,
    // toggle or DELETE an unrelated field.
    if (index < 0) {
        return nullptr;
    }
    pdf_annot* widget = pdf_first_widget(ctx, page);
    for (int i = 0; i < index && widget; i++) {
        widget = pdf_next_widget(ctx, widget);
    }
    return widget;
}

// GitHub #458: read a colour array (/MK /BC or /MK /BG) into a fixed-size
// buffer. Returns the component count, or 0 when the entry is absent or is not
// a 1-, 3- or 4-component array. POD locals only (fz_try discipline).
//
// Phase-3 security audit: each component is clamped to [0, 1], a non-finite
// one becomes 0 (sanitizeColorComponent). A non-numeric entry already reads
// as 0 through pdf_array_get_real.
static int readColorArray(fz_context* ctx, pdf_obj* arr, float* out) {
    if (!pdf_is_array(ctx, arr)) {
        return 0;
    }
    const int n = sanitizeColorComponentCount(pdf_array_len(ctx, arr));
    for (int i = 0; i < n; ++i) {
        out[i] = sanitizeColorComponent(pdf_array_get_real(ctx, arr, i));
    }
    return n;
}

// GitHub #458: read a widget's /MK /BC, /MK /BG and /BS. POD out-parameter,
// no C++ objects with destructors (fz_try discipline).
static void readBorderAppearance(fz_context* ctx, pdf_obj* obj,
                                 FormFieldBorderAppearance* out) {
    pdf_obj* mk = pdf_dict_get(ctx, obj, PDF_NAME(MK));
    if (pdf_is_dict(ctx, mk)) {
        out->borderColorComponents = readColorArray(
            ctx, pdf_dict_get(ctx, mk, PDF_NAME(BC)), out->borderColor.data());
        out->backgroundColorComponents = readColorArray(
            ctx, pdf_dict_get(ctx, mk, PDF_NAME(BG)), out->backgroundColor.data());
    }
    pdf_obj* bs = pdf_dict_get(ctx, obj, PDF_NAME(BS));
    if (pdf_is_dict(ctx, bs)) {
        out->hasBorderStyle = true;
        pdf_obj* w = pdf_dict_get(ctx, bs, PDF_NAME(W));
        // PDF 32000-1 table 166: /W defaults to 1 when absent. Clamped to
        // [0, 100] pt, non-finite -> 1 (phase-3 security audit).
        out->borderWidth = pdf_is_number(ctx, w)
                               ? sanitizeBorderWidth(pdf_to_real(ctx, w))
                               : 1.0f;
        const char* style = pdf_to_name(ctx, pdf_dict_get(ctx, bs, PDF_NAME(S)));
        out->borderStyle = (style && style[0]) ? style[0] : 'S';
    }
}

// GitHub #458: write /MK /BC, /MK /BG and /BS onto a widget the app creates.
// Only called from createFormFieldWithAppearance, i.e. never on a widget
// loaded from a file, so an existing widget's /MK is never touched. Entries
// that are absent in `ap` are not written. Any other /MK entry is kept.
//
// Phase-3 security audit: `ap` can be hand-built (restore data, a caller's
// struct), so it is sanitised on the way out exactly as readBorderAppearance
// sanitises on the way in: a component count other than 1, 3 or 4 writes
// nothing (and can never index past the 4-element arrays), components are
// clamped to [0, 1], and /W to [0, 100].
static void writeBorderAppearance(fz_context* ctx, pdf_obj* obj,
                                  const FormFieldBorderAppearance* ap) {
    const int nBc = sanitizeColorComponentCount(ap->borderColorComponents);
    const int nBg = sanitizeColorComponentCount(ap->backgroundColorComponents);
    if (nBc > 0 || nBg > 0) {
        pdf_obj* mk = pdf_dict_get(ctx, obj, PDF_NAME(MK));
        if (!pdf_is_dict(ctx, mk)) {
            mk = pdf_dict_put_dict(ctx, obj, PDF_NAME(MK), 2);
        }
        if (nBc > 0) {
            pdf_obj* bc = pdf_dict_put_array(ctx, mk, PDF_NAME(BC), nBc);
            for (int i = 0; i < nBc; ++i) {
                pdf_array_push_real(ctx, bc, sanitizeColorComponent(ap->borderColor[i]));
            }
        }
        if (nBg > 0) {
            pdf_obj* bg = pdf_dict_put_array(ctx, mk, PDF_NAME(BG), nBg);
            for (int i = 0; i < nBg; ++i) {
                pdf_array_push_real(ctx, bg,
                                    sanitizeColorComponent(ap->backgroundColor[i]));
            }
        }
    }
    if (ap->hasBorderStyle) {
        pdf_obj* bs = pdf_dict_put_dict(ctx, obj, PDF_NAME(BS), 2);
        pdf_dict_put_real(ctx, bs, PDF_NAME(W), sanitizeBorderWidth(ap->borderWidth));
        const char styleName[2] = {ap->borderStyle, '\0'};
        pdf_dict_put_name(ctx, bs, PDF_NAME(S), styleName);
    }
}

// GitHub #459: the widget's /DA and /Q, parsed by MuPDF. POD locals only, and the
// std::string is assigned OUTSIDE the fz_try region (fz_try discipline). A
// widget whose /DA cannot be read keeps MuPDF's own defaults (Helv 12, no
// colour) rather than failing the whole read.
static void readDefaultAppearance(fz_context* ctx, pdf_annot* widget,
                                  FormFieldData* out) {
    const char* font = "Helv";
    float size = 12.0f;
    int n = 0;
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int q = 0;
    fz_try(ctx) {
        pdf_annot_default_appearance(ctx, widget, &font, &size, &n, color);
    }
    fz_catch(ctx) {
        font = "Helv";
        size = 12.0f;
        n = 0;
    }
    // /Q, through the accessor MuPDF's appearance writer itself uses, so the
    // live edit aligns as the committed value will (already clamped to 0..2).
    fz_try(ctx) {
        q = pdf_annot_quadding(ctx, widget);
    }
    fz_catch(ctx) {
        q = 0;
    }
    out->quadding = (q == 1 || q == 2) ? q : 0;
    out->daFontName = font ? font : "Helv";
    // Phase-3 security audit: the Tf operand is whatever fz_strtof made of
    // the file's text -- 1e30, inf, nan and negatives all get through. Keep
    // it finite and sane here (non-finite or negative -> 0 = auto-size, cap
    // 1000 pt); FormTool's paint clamps again before any int conversion.
    out->daFontSize = sanitizeDefaultAppearanceSize(size);
    out->daColorComponents = sanitizeColorComponentCount(n);
    for (int i = 0; i < 4; ++i) {
        out->daColor[static_cast<std::size_t>(i)] = sanitizeColorComponent(color[i]);
    }

    // GitHub #460: the /DA STRING in effect, byte for byte, so restoreFormField
    // can rebuild exactly it (including a /DR font name, which the parsed
    // font above maps to Helv). Same lookup as pdf_annot_default_appearance:
    // own or inherited /DA, else the AcroForm /DA. POD locals only; the
    // std::string is assigned after the fz_try region (fz_try discipline).
    const char* raw = nullptr;
    size_t rawLen = 0;
    fz_try(ctx) {
        pdf_obj* obj = pdf_annot_obj(ctx, widget);
        pdf_obj* da = pdf_dict_get_inheritable(ctx, obj, PDF_NAME(DA));
        if (!da) {
            pdf_page* annotPage = pdf_annot_page(ctx, widget);
            if (annotPage && annotPage->doc) {
                da = pdf_dict_getl(ctx, pdf_trailer(ctx, annotPage->doc),
                                   PDF_NAME(Root), PDF_NAME(AcroForm),
                                   PDF_NAME(DA), nullptr);
            }
        }
        if (pdf_is_string(ctx, da)) {
            raw = pdf_to_str_buf(ctx, da);
            rawLen = pdf_to_str_len(ctx, da);
        }
    }
    fz_catch(ctx) {
        raw = nullptr;
        rawLen = 0;
    }
    if (raw && rawLen <= kMaxCapturedDefaultAppearanceBytes) {
        out->defaultAppearanceString.assign(raw, rawLen);
        out->defaultAppearanceStringCaptured = true;
    } else if (raw) {
        // Over the cap: capture the parsed (mapped, sanitised) values, which
        // is all MuPDF's parser ever reads of the string anyway.
        //
        // Phase-4 security audit: the re-print must never truncate. The font
        // is forced to a base-14 key (MuPDF's mapping already guarantees one;
        // this does not rely on it). MuPDF's %g (fitz/printf.c fmtfloat)
        // NEVER uses exponent form: a denormal component such as 1e-45
        // prints as "." + 44 zeros + up to 9 digits, ~54 chars. Worst case:
        // "/ZaDb " (6) + size in [0, 1000] (<= ~14) + " Tf " (4) + four
        // components of <= 55 chars each with their space (220) + "k" (1):
        // about 250 bytes, so 512 has a 2x margin. If the output still filled
        // the buffer, nothing is captured and restore falls back to the
        // default rather than writing a truncated /DA.
        char printed[512] = {};
        const char* fontKey = isBase14DefaultAppearanceFont(out->daFontName)
                                  ? out->daFontName.c_str()
                                  : "Helv";
        const float c[4] = {out->daColor[0], out->daColor[1], out->daColor[2],
                            out->daColor[3]};
        pdf_print_default_appearance(ctx, printed, static_cast<int>(sizeof printed),
                                     fontKey, out->daFontSize,
                                     out->daColorComponents, c);
        const size_t printedLen = strnlen(printed, sizeof printed);
        if (printedLen > 0 && printedLen < sizeof printed - 1) {
            out->defaultAppearanceString.assign(printed, printedLen);
            out->defaultAppearanceStringCaptured = true;
        }
    }
}

// GitHub #460: one string entry of a widget dictionary, as pointers into the
// MuPDF object (valid until the dictionary is next modified). POD only.
struct RawStringEntry {
    bool present = false;
    const char* data = nullptr;
    size_t len = 0;
};

static RawStringEntry readRawStringEntry(fz_context* ctx, pdf_obj* dict, pdf_obj* key) {
    RawStringEntry e;
    pdf_obj* v = pdf_dict_get(ctx, dict, key);
    if (pdf_is_string(ctx, v)) {
        e.present = true;
        e.data = pdf_to_str_buf(ctx, v);
        e.len = pdf_to_str_len(ctx, v);
    }
    return e;
}

// Write a captured string entry back, or remove it when it was absent.
static void writeRawStringEntry(fz_context* ctx, pdf_obj* dict, pdf_obj* key,
                                bool present, const char* data, size_t len) {
    if (present) {
        pdf_dict_put_string(ctx, dict, key, data ? data : "", data ? len : 0);
    } else {
        pdf_dict_del(ctx, dict, key);
    }
}

// GitHub #460: the colour setFieldDefaultAppearance writes, as an RGB triple
// in [0, 1]. False -- reject -- for an invalid or non-opaque colour, and for
// an extended-RGB colour (QColor::fromRgbF with a component outside [0, 1])
// whose components are out of range or non-finite. Any other spec (Rgb, Hsv,
// Hsl, Cmyk) is converted with toRgb(), whose components are in range by
// construction.
static bool defaultAppearanceRgb(const QColor& color, float out[3]) {
    if (!color.isValid()) return false;
    if (color.spec() == QColor::ExtendedRgb) {
        const float c[4] = {color.redF(), color.greenF(), color.blueF(), color.alphaF()};
        for (float v : c) {
            if (!std::isfinite(v) || v < 0.0f || v > 1.0f) return false;
        }
    }
    if (color.alpha() != 255) return false;
    const QColor rgb = color.toRgb();
    out[0] = sanitizeColorComponent(rgb.redF());
    out[1] = sanitizeColorComponent(rgb.greenF());
    out[2] = sanitizeColorComponent(rgb.blueF());
    return true;
}

// Helper: Read form field data into FormFieldData struct
static FormFieldData readWidgetData(
    fz_context* ctx,
    pdf_page* pdfPage,
    pdf_annot* widget,
    int pageIndex,
    int widgetIndex
) {
    FormFieldData data;
    data.pageIndex = pageIndex;
    data.widgetIndex = widgetIndex;

    // Type
    int pdfType = pdf_widget_type(ctx, widget);
    data.type = fromPdfWidgetType(pdfType);

    // Bounding rectangle
    fz_rect r = pdf_annot_rect(ctx, widget);
    data.rect = QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);

    // Field name.
    //
    // F15 / PDF-45 DEFECT FIX: this used pdf_annot_field_label(), which returns
    // the field's PARTIAL name (its own /T). For a nested AcroForm — the shape
    // every real government/enterprise form uses — that is the WRONG
    // identifier: two children under different parents ("billing.name" and
    // "shipping.name") both reported "name", so an XFDF/FDF import addressed by
    // the correct fully-qualified name matched nothing, and an export produced
    // names no other reader can resolve. pdf_load_field_name() walks /Parent and
    // returns the fully-qualified name; it allocates, so it is freed here.
    {
        char* qualified = NULL;
        fz_try(ctx) {
            qualified = pdf_load_field_name(ctx, pdf_annot_obj(ctx, widget));
        }
        fz_catch(ctx) {
            qualified = NULL;
        }
        // All C++ work happens OUTSIDE the fz_try region (setjmp/longjmp
        // discipline — see docs/mupdf-gotchas.md).
        if (qualified) {
            data.fieldName = qualified;
            fz_free(ctx, qualified);
        } else {
            // Fall back to the partial name rather than failing the whole read.
            const char* label = pdf_annot_field_label(ctx, widget);
            if (label) {
                data.fieldName = label;
            }
        }
    }

    // Field value
    const char* val = pdf_annot_field_value(ctx, widget);
    if (val) {
        data.value = val;
    }

    // Field flags
    data.fieldFlags = pdf_annot_field_flags(ctx, widget);
    data.isReadOnly = (data.fieldFlags & PDF_FIELD_IS_READ_ONLY) != 0;
    data.isRequired = (data.fieldFlags & PDF_FIELD_IS_REQUIRED) != 0;

    // Text field flags
    if (data.type == FormFieldType::Text) {
        data.isMultiline = (data.fieldFlags & PDF_TX_FIELD_IS_MULTILINE) != 0;
        data.isPassword = (data.fieldFlags & PDF_TX_FIELD_IS_PASSWORD) != 0;
        data.isComb = (data.fieldFlags & PDF_TX_FIELD_IS_COMB) != 0;
        // PDF-44 / GitHub #16: /MaxLen. Inheritable (a nested AcroForm can
        // put it on the parent), absent -> 0 -> "no limit".
        data.maxLen = pdf_dict_get_inheritable_int(
            ctx, pdf_annot_obj(ctx, widget), PDF_NAME(MaxLen));
    }

    // Choice widget flags
    if (data.type == FormFieldType::ComboBox) {
        data.isEditable = (data.fieldFlags & PDF_CH_FIELD_IS_EDIT) != 0;
    }

    // GitHub #458: border and background, so restoreFormField can rebuild
    // exactly what the widget had.
    readBorderAppearance(ctx, pdf_annot_obj(ctx, widget), &data.borderAppearance);
    data.borderAppearanceCaptured = true;

    // GitHub #459: the /DA font, size and colour, for the live edit paint.
    // A default appearance is a variable-text entry (PDF 32000-1 12.7.3.3):
    // text and choice fields. Buttons keep the defaults; nothing paints their
    // text live.
    if (data.type == FormFieldType::Text ||
        data.type == FormFieldType::ComboBox ||
        data.type == FormFieldType::ListBox) {
        readDefaultAppearance(ctx, widget, &data);
    }

    // Choice widget options
    if (data.type == FormFieldType::ComboBox || data.type == FormFieldType::ListBox) {
        data.isMultiSelect = pdf_choice_widget_is_multiselect(ctx, widget) != 0;

        // Get display options
        int optCount = pdf_choice_widget_options(ctx, widget, 0, nullptr);
        if (optCount > 0) {
            std::vector<const char*> opts(optCount);
            pdf_choice_widget_options(ctx, widget, 0, opts.data());
            for (int i = 0; i < optCount; i++) {
                data.options.push_back(opts[i] ? opts[i] : "");
            }

            // Get export values
            std::vector<const char*> exports(optCount);
            pdf_choice_widget_options(ctx, widget, 1, exports.data());
            for (int i = 0; i < optCount; i++) {
                data.exportValues.push_back(exports[i] ? exports[i] : "");
            }
        }

        // Get currently selected values
        int selCount = pdf_choice_widget_value(ctx, widget, nullptr);
        if (selCount > 0) {
            std::vector<const char*> selVals(selCount);
            pdf_choice_widget_value(ctx, widget, selVals.data());
            // Map selected values to indices
            for (int i = 0; i < selCount; i++) {
                if (selVals[i]) {
                    for (int j = 0; j < static_cast<int>(data.options.size()); j++) {
                        if (data.options[j] == selVals[i] || data.exportValues[j] == selVals[i]) {
                            data.selectedIndices.push_back(j);
                            break;
                        }
                    }
                }
            }
        }
    }

    return data;
}

// Pimpl implementation
class FormManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    // Custom tab orders set by setTabOrder(), keyed by page index
    std::map<int, std::vector<int>> customTabOrders;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) {
        fz_page* fzPage = page->fzPage();
        return context->executeReturn<pdf_page*>([fzPage](fz_context* ctx) {
            return pdf_page_from_fz_page(ctx, fzPage);
        });
    }
};

// FormManager implementation

FormManager::FormManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx))) {}

FormManager::~FormManager() = default;

FormManager::FormManager(FormManager&&) noexcept = default;
FormManager& FormManager::operator=(FormManager&&) noexcept = default;

std::vector<FormFieldData> FormManager::getFormFields(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();

    return pImpl->context->executeReturn<std::vector<FormFieldData>>(
        [pdfPage, pageIndex](fz_context* ctx) -> std::vector<FormFieldData> {
            std::vector<FormFieldData> fields;
            int widgetIndex = 0;
            pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
            while (widget) {
                fields.push_back(readWidgetData(ctx, pdfPage, widget, pageIndex, widgetIndex));
                widget = pdf_next_widget(ctx, widget);
                widgetIndex++;
            }
            return fields;
        }
    );
}

FormFieldData FormManager::getFormField(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();

    return pImpl->context->executeReturn<FormFieldData>(
        [pdfPage, pageIndex, widgetIndex](fz_context* ctx) -> FormFieldData {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }
            return readWidgetData(ctx, pdfPage, widget, pageIndex, widgetIndex);
        }
    );
}

std::optional<int> FormManager::getFormFieldAt(const pdfe::core::Page* page, QPointF pdfPoint) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    float px = static_cast<float>(pdfPoint.x());
    float py = static_cast<float>(pdfPoint.y());

    return pImpl->context->executeReturn<std::optional<int>>(
        [pdfPage, px, py](fz_context* ctx) -> std::optional<int> {
            int widgetIndex = 0;
            pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
            while (widget) {
                fz_rect r = pdf_annot_rect(ctx, widget);
                if (px >= r.x0 && px <= r.x1 && py >= r.y0 && py <= r.y1) {
                    return widgetIndex;
                }
                widget = pdf_next_widget(ctx, widget);
                widgetIndex++;
            }
            return std::nullopt;
        }
    );
}

// PDF-44 / GitHub #16 — truncate `s` to at most `maxChars` UTF-8 CODEPOINTS.
//
// By codepoint, not by byte: /MaxLen counts characters (PDF 32000-1 12.7.4.3),
// and a byte-wise cut would split a multi-byte sequence and leave invalid
// UTF-8 in /V — the same class of defect phase-74 fixed in FormTool.
// Returns `s` unchanged when it is already short enough.
static std::string truncateToCodepoints(const std::string& s, int maxChars) {
    if (maxChars <= 0) return s;
    size_t i = 0;
    int seen = 0;
    while (i < s.size()) {
        if (seen == maxChars) return s.substr(0, i);
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80u) == 0x00u)      len = 1;
        else if ((c & 0xE0u) == 0xC0u) len = 2;
        else if ((c & 0xF0u) == 0xE0u) len = 3;
        else if ((c & 0xF8u) == 0xF0u) len = 4;
        // A stray continuation byte / invalid lead advances by one so this
        // always terminates; it is counted as one character.
        if (i + len > s.size()) len = s.size() - i;
        i += len;
        ++seen;
    }
    return s;
}

std::string FormManager::setFieldValue(const pdfe::core::Page* page, int widgetIndex,
                                        const std::string& value) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    // PDF-44 / GitHub #16: a COMB field's /MaxLen is its CELL COUNT, not a
    // soft hint — the appearance generator divides the field width into
    // exactly /MaxLen cells. A longer value has nowhere to go: MuPDF keeps
    // stepping one cell width per character, so the overflow is drawn OUTSIDE
    // the widget (and outside the clip) while the stored /V claims the whole
    // string. Cap it on the one path that writes /V and regenerates the
    // appearance, so the two can never disagree.
    //
    // Deliberately scoped to comb fields. A plain text field's /MaxLen is an
    // input-length hint for an interactive editor; silently truncating a
    // programmatic fill (FDF/XFDF import, undo restore) there would DESTROY
    // data rather than protect a layout invariant.
    //
    // The cap is READ in its own context call and the truncated string is
    // built OUTSIDE any fz_try region: fz_try/fz_catch is setjmp/longjmp, so
    // a C++ local with a non-trivial destructor inside the lambda is a
    // leak-on-longjmp hazard (see docs/mupdf-gotchas.md and
    // tools/gate/fz_try_raii_scan.py). A widget index that does not resolve
    // yields 0 here — no cap — and the real diagnostic is raised by the
    // fz_throw in the main call below.
    const int combCap = pImpl->context->executeReturn<int>(
        [pdfPage, widgetIndex](fz_context* ctx) -> int {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) return 0;
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);
            if ((pdf_field_flags(ctx, fieldObj) & PDF_TX_FIELD_IS_COMB) == 0) {
                return 0;
            }
            return pdf_dict_get_inheritable_int(ctx, fieldObj, PDF_NAME(MaxLen));
        }
    );
    const std::string stored = truncateToCodepoints(value, combCap);

    return pImpl->context->executeReturn<std::string>(
        [pdfPage, widgetIndex, &stored](fz_context* ctx) -> std::string {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            // Save old value for undo
            std::string oldValue;
            const char* oldVal = pdf_annot_field_value(ctx, widget);
            if (oldVal) {
                oldValue = oldVal;
            }

            // Set the new value via pdf_obj.
            // ignore_trigger_events=1: skip JS validation/keystroke events
            // so the value is accepted unconditionally (mirrors what
            // pdf_set_text_field_value does internally after validation).
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);

            // `stored` is the caller's value already capped at the comb
            // /MaxLen (computed above, outside this fz_try region).
            pdf_set_field_value(ctx, pdfDoc, fieldObj, stored.c_str(), 1);

            // F15 / PDF-42: regenerate the widget's appearance stream so the
            // new value is actually VISIBLE. Without this the value lives only
            // in /V and the form renders with the OLD (or no) text in any
            // viewer that trusts /AP — the classic "my form saved empty".
            pdf_update_annot(ctx, widget);

            // F15 / PDF-41: if the appearance still could not be produced
            // (e.g. the field's /DA names a font absent from /DR), fail safe by
            // telling the viewer to synthesise appearances itself.
            if (!widgetHasNonEmptyAppearance(ctx, widget)) {
                writeNeedAppearances(ctx, pdfDoc, 1);
            }

            return oldValue;
        }
    );
}

// Bug 3 (origin/master 596d613f): read a field's /DA string.

std::string FormManager::getFieldDefaultAppearance(const pdfe::core::Page* page,
                                                   int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<std::string>(
        [pdfPage, widgetIndex](fz_context* ctx) -> std::string {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                return std::string();
            }
            pdf_obj* daObj = pdf_dict_get_inheritable(
                ctx, pdf_annot_obj(ctx, widget), PDF_NAME(DA));
            if (!daObj) {
                return std::string();
            }
            const char* da = pdf_to_text_string(ctx, daObj);
            // pdf_to_text_string returns a borrowed pointer; the std::string is
            // built as the return value, so no C++ local is live across a call
            // that could fz_throw (see docs/mupdf-gotchas.md).
            return da ? std::string(da) : std::string();
        });
}

void FormManager::toggleField(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex](fz_context* ctx) {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }
            pdf_toggle_widget(ctx, widget);
        }
    );
}

void FormManager::setChoiceSelection(const pdfe::core::Page* page, int widgetIndex,
                                      const std::vector<int>& selectedIndices) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex, &selectedIndices](fz_context* ctx) {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            // Get all option strings
            int optCount = pdf_choice_widget_options(ctx, widget, 0, nullptr);
            std::vector<const char*> allOpts(optCount);
            pdf_choice_widget_options(ctx, widget, 0, allOpts.data());

            // Build the selected options array
            std::vector<const char*> selOpts;
            for (int idx : selectedIndices) {
                if (idx >= 0 && idx < optCount) {
                    selOpts.push_back(allOpts[idx]);
                }
            }

            pdf_choice_widget_set_value(ctx, widget,
                static_cast<int>(selOpts.size()), selOpts.data());
        }
    );
}

std::vector<int> FormManager::getTabOrder(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    int pageIdx = page->index();

    // Return custom tab order if one was set
    auto it = pImpl->customTabOrders.find(pageIdx);
    if (it != pImpl->customTabOrders.end()) {
        return it->second;
    }

    // Default: widget indices in document order
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<std::vector<int>>(
        [pdfPage](fz_context* ctx) -> std::vector<int> {
            std::vector<int> order;
            int widgetIndex = 0;
            pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
            while (widget) {
                order.push_back(widgetIndex);
                widget = pdf_next_widget(ctx, widget);
                widgetIndex++;
            }
            return order;
        }
    );
}

void FormManager::setTabOrder(const pdfe::core::Page* page,
                               const std::vector<int>& newOrder) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    int pageIdx = page->index();

    // Count widgets to validate
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int widgetCount = pImpl->context->executeReturn<int>(
        [pdfPage](fz_context* ctx) -> int {
            int count = 0;
            pdf_annot* w = pdf_first_widget(ctx, pdfPage);
            while (w) { count++; w = pdf_next_widget(ctx, w); }
            return count;
        }
    );

    if (static_cast<int>(newOrder.size()) != widgetCount) return;

    // Validate: must be a permutation of [0, widgetCount)
    std::set<int> seen;
    for (int idx : newOrder) {
        if (idx < 0 || idx >= widgetCount || !seen.insert(idx).second)
            return;
    }

    // Store custom tab order
    pImpl->customTabOrders[pageIdx] = newOrder;

    // Persist to document as custom /PDFE_TabOrder array on the page dict
    pImpl->context->execute(
        [pdfPage, &newOrder](fz_context* ctx) {
            pdf_document* doc = pdfPage->doc;
            pdf_obj* pageObj = pdfPage->obj;

            pdf_obj* key = pdf_new_name(ctx, "PDFE_TabOrder");
            pdf_obj* arr = pdf_new_array(ctx, doc,
                                          static_cast<int>(newOrder.size()));
            for (int idx : newOrder) {
                pdf_array_push_int(ctx, arr, idx);
            }
            pdf_dict_put(ctx, pageObj, key, arr);
            pdf_drop_obj(ctx, arr);
            pdf_drop_obj(ctx, key);
        }
    );
}

namespace {

// Walk /AcroForm /Fields recursively (fields can be nested via /Kids)
// and invoke visitor(fieldObj, fullyQualifiedName) for each leaf.
void walkAcroFormFields(
    fz_context* ctx, pdf_obj* fields, const std::string& prefix,
    const std::function<void(pdf_obj*, const std::string&)>& visitor)
{
    if (!fields || !pdf_is_array(ctx, fields)) return;
    const int n = pdf_array_len(ctx, fields);
    for (int i = 0; i < n; ++i) {
        pdf_obj* field = pdf_array_get(ctx, fields, i);
        if (!field) continue;
        pdf_obj* t = pdf_dict_get(ctx, field, PDF_NAME(T));
        std::string name = prefix;
        if (t) {
            const char* s = pdf_to_text_string(ctx, t);
            if (s && *s) {
                if (!name.empty()) name += ".";
                name += s;
            }
        }
        // F15 / PDF-45 DEFECT FIX. The old rule was "has /FT -> leaf", which is
        // wrong: ISO 32000-1 §12.7.3.1 makes a field TERMINAL only when its
        // /Kids are widget annotations rather than child FIELDS, and a
        // non-terminal parent legitimately carries /FT for its children to
        // inherit. With the old rule a nested form reported only the parent
        // ("grp"), so its children were invisible to listFieldNames and could
        // never be named in a /CO calculation order.
        pdf_obj* kids = pdf_dict_get(ctx, field, PDF_NAME(Kids));
        bool kidsAreFields = false;
        if (kids && pdf_is_array(ctx, kids)) {
            const int kn = pdf_array_len(ctx, kids);
            for (int k = 0; k < kn; ++k) {
                pdf_obj* kid = pdf_array_get(ctx, kids, k);
                if (!kid) continue;
                // A kid carrying its own /T (or its own /Kids) is a child
                // FIELD; a kid with neither is just this field's widget.
                if (pdf_dict_get(ctx, kid, PDF_NAME(T)) ||
                    pdf_dict_get(ctx, kid, PDF_NAME(Kids))) {
                    kidsAreFields = true;
                    break;
                }
            }
        }
        if (kidsAreFields) {
            walkAcroFormFields(ctx, kids, name, visitor);
        } else {
            visitor(field, name);
        }
    }
}

// ---- Page-scoped flatten (GitHub #442) -------------------------------------
//
// pdf_bake_document bakes the widgets of EVERY page; MuPDF's per-page baker is
// internal. bakePageWidgets does the same work for one page: each widget's
// normal appearance becomes a Form XObject drawn at the widget's place in a
// content stream appended to the page, and the widget is deleted from the
// page and from the AcroForm field tree. Other pages are not touched: an
// inherited or shared /Resources (or /XObject) dictionary is copied onto this
// page before a name is added to it. The AcroForm itself is removed only when
// no field is left.

// The appearance stream a widget shows (MuPDF's rule: /AP /N, or the /AS
// state of it).
pdf_obj* widgetAppearance(fz_context* ctx, pdf_obj* annot)
{
    pdf_obj* ap = pdf_dict_get(ctx, annot, PDF_NAME(AP));
    if (!ap) return nullptr;
    pdf_obj* n = pdf_dict_get(ctx, ap, PDF_NAME(N));
    if (pdf_is_stream(ctx, n)) return n;
    pdf_obj* as = pdf_resolve_indirect_chain(ctx, pdf_dict_get(ctx, annot, PDF_NAME(AS)));
    pdf_obj* st = pdf_dict_get(ctx, n, as);
    return pdf_is_stream(ctx, st) ? st : nullptr;
}

// MuPDF's placement of an appearance: its BBox (through its Matrix) mapped
// onto the annotation's Rect.
fz_matrix appearanceToRect(fz_context* ctx, pdf_obj* annot, pdf_obj* ap)
{
    const fz_rect rect = pdf_dict_get_rect(ctx, annot, PDF_NAME(Rect));
    const fz_rect bbox = fz_transform_rect(pdf_dict_get_rect(ctx, ap, PDF_NAME(BBox)),
                                           pdf_dict_get_matrix(ctx, ap, PDF_NAME(Matrix)));
    const float bw = bbox.x1 - bbox.x0, bh = bbox.y1 - bbox.y0;
    const float w = bw != 0 ? (rect.x1 - rect.x0) / bw : 1;
    const float h = bh != 0 ? (rect.y1 - rect.y0) / bh : 1;
    return fz_make_matrix(w, 0, 0, h, rect.x0 - bbox.x0 * w, rect.y0 - bbox.y0 * h);
}

// Removes, recursively, every field whose /Kids became empty (a parent whose
// last widget was baked). Bounded depth: a cyclic tree ends.
void pruneEmptyFields(fz_context* ctx, pdf_obj* arr, int depth)
{
    if (!pdf_is_array(ctx, arr) || depth > 32) return;
    for (int i = pdf_array_len(ctx, arr) - 1; i >= 0; --i) {
        pdf_obj* kids = pdf_dict_get(ctx, pdf_array_get(ctx, arr, i), PDF_NAME(Kids));
        if (!pdf_is_array(ctx, kids)) continue;
        pruneEmptyFields(ctx, kids, depth + 1);
        if (pdf_array_len(ctx, kids) == 0) pdf_array_delete(ctx, arr, i);
    }
}

// A dictionary of `parent` under `key` that this page may change: a shallow
// copy (pdf_copy_dict) of the one it has or inherits, so that a dictionary
// shared with another page, or a direct one inside a shared parent, is never
// written to.
pdf_obj* ownedDict(fz_context* ctx, pdf_obj* parent, pdf_obj* key, pdf_obj* inherited)
{
    pdf_obj* d = pdf_dict_get(ctx, parent, key);
    if (!d) d = inherited;
    if (!pdf_is_dict(ctx, d)) return pdf_dict_put_dict(ctx, parent, key, 8);
    pdf_obj* copy = pdf_copy_dict(ctx, d);
    pdf_dict_put_drop(ctx, parent, key, copy);
    return copy;
}

void bakePageWidgets(fz_context* ctx, pdf_page* page)
{
    pdf_document* doc = page->doc;
    pdf_obj* pageObj = page->obj;
    fz_buffer* buf = nullptr;
    pdf_obj* prologue = nullptr;
    fz_var(buf);
    fz_var(prologue);

    pdf_begin_operation(ctx, doc, "Flatten form fields on page");
    fz_try(ctx) {
        for (pdf_annot* w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w))
            pdf_annot_request_synthesis(ctx, w);
        pdf_update_page(ctx, page);

        if (pdf_first_widget(ctx, page)) {
            pdf_obj* inheritedRes = pdf_dict_get(ctx, pageObj, PDF_NAME(Resources))
                                        ? nullptr
                                        : pdf_dict_get_inheritable(ctx, pageObj, PDF_NAME(Resources));
            pdf_obj* res = ownedDict(ctx, pageObj, PDF_NAME(Resources), inheritedRes);
            pdf_obj* xobjects = ownedDict(ctx, res, PDF_NAME(XObject), nullptr);

            // Keep the graphics state balanced, as pdf_bake_document does.
            pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
            int prepend = 0, append = 0;
            pdf_count_q_balance(ctx, doc, res, contents, &prepend, &append);
            if (prepend > 0) {
                buf = fz_new_buffer(ctx, 64);
                while (prepend-- > 0) fz_append_string(ctx, buf, "q\n");
                prologue = pdf_add_stream(ctx, doc, buf, nullptr, 0);
                fz_drop_buffer(ctx, buf);
                buf = nullptr;
            }
            buf = fz_new_buffer(ctx, 1024);
            while (append-- > 0) fz_append_string(ctx, buf, "Q\n");

            pdf_annot* w = pdf_first_widget(ctx, page);
            while (w) {
                pdf_annot* next = pdf_next_widget(ctx, w);
                pdf_obj* annotObj = pdf_annot_obj(ctx, w);
                if (pdf_obj* ap = widgetAppearance(ctx, annotObj)) {
                    char name[32];
                    fz_snprintf(name, sizeof name, "Annot%d", pdf_to_num(ctx, annotObj));
                    pdf_dict_puts(ctx, xobjects, name, ap);
                    pdf_dict_put(ctx, ap, PDF_NAME(Type), PDF_NAME(XObject));
                    pdf_dict_put(ctx, ap, PDF_NAME(Subtype), PDF_NAME(Form));
                    const fz_matrix m = appearanceToRect(ctx, annotObj, ap);
                    fz_append_printf(ctx, buf, "q\n%g %g %g %g %g %g cm\n/%s Do\nQ\n",
                                     m.a, m.b, m.c, m.d, m.e, m.f, name);
                }
                // Removes it from /Annots and from the AcroForm field tree.
                pdf_delete_annot(ctx, page, w);
                w = next;
            }

            if (!pdf_is_array(ctx, contents)) {
                pdf_obj* arr = pdf_new_array(ctx, doc, 3);
                pdf_dict_put_drop(ctx, pageObj, PDF_NAME(Contents), arr);
                if (prologue) pdf_array_push(ctx, arr, prologue);
                if (contents) pdf_array_push(ctx, arr, contents);
                contents = arr;
            } else if (prologue) {
                pdf_array_insert(ctx, contents, prologue, 0);
            }
            pdf_array_push_drop(ctx, contents, pdf_add_stream(ctx, doc, buf, nullptr, 0));

            pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
            pdf_obj* acroform = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
            pdf_obj* fields = pdf_dict_get(ctx, acroform, PDF_NAME(Fields));
            pruneEmptyFields(ctx, fields, 0);
            if (acroform && pdf_array_len(ctx, fields) == 0)
                pdf_dict_del(ctx, root, PDF_NAME(AcroForm));
        }
        pdf_end_operation(ctx, doc);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        pdf_drop_obj(ctx, prologue);
    }
    fz_catch(ctx) {
        pdf_abandon_operation(ctx, doc);
        fz_rethrow(ctx);
    }
}

} // namespace

std::vector<std::string> FormManager::listFieldNames(const pdfe::core::Document* doc)
{
    std::vector<std::string> out;
    if (!doc) return out;
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* trailer = pdf_trailer(ctx, pdoc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        if (!root) return;
        pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        if (!acroForm) return;
        pdf_obj* fields = pdf_dict_get(ctx, acroForm, PDF_NAME(Fields));
        walkAcroFormFields(ctx, fields, "",
            [&out](pdf_obj*, const std::string& name) {
                if (!name.empty()) out.push_back(name);
            });
    });
    return out;
}

// ─── F15 / PDF-41: /NeedAppearances ──────────────────────────────────────

bool FormManager::needAppearances(const pdfe::core::Document* doc) {
    if (!doc) return false;
    return pImpl->context->executeReturn<bool>(
        [doc](fz_context* ctx) -> bool {
            pdf_document* pdoc =
                pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdoc) return false;
            pdf_obj* trailer = pdf_trailer(ctx, pdoc);
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            if (!root) return false;
            pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
            if (!acroForm) return false;
            pdf_obj* na = pdf_dict_gets(ctx, acroForm, "NeedAppearances");
            if (!na) return false;   // spec default is false
            return pdf_to_bool(ctx, na) != 0;
        });
}

void FormManager::setNeedAppearances(const pdfe::core::Document* doc, bool value) {
    if (!doc) return;
    const int v = value ? 1 : 0;
    pImpl->context->execute([doc, v](fz_context* ctx) {
        pdf_document* pdoc =
            pdf_document_from_fz_document(ctx, doc->fzDocument());
        writeNeedAppearances(ctx, pdoc, v);
    });
}

std::vector<std::string> FormManager::getCalculationOrder(
    const pdfe::core::Document* doc)
{
    std::vector<std::string> out;
    if (!doc) return out;
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* trailer = pdf_trailer(ctx, pdoc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        if (!root) return;
        pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        if (!acroForm) return;
        pdf_obj* co = pdf_dict_get(ctx, acroForm, PDF_NAME(CO));
        if (!co || !pdf_is_array(ctx, co)) return;
        const int n = pdf_array_len(ctx, co);
        for (int i = 0; i < n; ++i) {
            pdf_obj* entry = pdf_array_get(ctx, co, i);
            if (!entry) continue;
            pdf_obj* t = pdf_dict_get(ctx, entry, PDF_NAME(T));
            if (!t) continue;
            const char* s = pdf_to_text_string(ctx, t);
            if (s && *s) out.push_back(s);
        }
    });
    return out;
}

void FormManager::setCalculationOrder(
    const pdfe::core::Document* doc,
    const std::vector<std::string>& fieldNames)
{
    if (!doc) return;
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* trailer = pdf_trailer(ctx, pdoc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        if (!root) return;
        pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        if (!acroForm) {
            acroForm = pdf_dict_put_dict(ctx, root, PDF_NAME(AcroForm), 2);
        }

        if (fieldNames.empty()) {
            pdf_dict_del(ctx, acroForm, PDF_NAME(CO));
            return;
        }

        // Resolve each requested name to the matching field object.
        //
        // fix-tre-writetarget (sweep Finding 6) — DOCUMENTED, left as-is.
        // This map is keyed by fully-qualified field name. PDF 32000-1 §12.7.3.2
        // requires fully-qualified field names to be UNIQUE within a document, so
        // this key cannot collide in a well-formed AcroForm. A malformed form with
        // two fields sharing a qualified name is the only way to collide, and for
        // that case there is no spec-defined "correct" field to put in /CO — the
        // calculation order of a duplicate-named field is undefined input. The
        // behaviour here is at least DETERMINISTIC (walkAcroFormFields visits the
        // Fields array in a fixed order, so the last-encountered duplicate wins
        // every run), which is the most we can promise for malformed input; /CO is
        // a recalculation-order hint, not a data-integrity structure, so a
        // cosmetic arbitrary-among-duplicates pick is acceptable. Not worth a
        // reject-the-form or first-wins change that would still be arbitrary.
        std::map<std::string, pdf_obj*> nameToField;
        pdf_obj* fields = pdf_dict_get(ctx, acroForm, PDF_NAME(Fields));
        walkAcroFormFields(ctx, fields, "",
            [&nameToField](pdf_obj* f, const std::string& name) {
                if (!name.empty()) nameToField[name] = f;
            });

        pdf_obj* coArr = pdf_new_array(ctx, pdoc,
                                       static_cast<int>(fieldNames.size()));
        for (const auto& name : fieldNames) {
            auto it = nameToField.find(name);
            if (it != nameToField.end() && it->second) {
                pdf_array_push(ctx, coArr, it->second);
            }
        }
        pdf_dict_put_drop(ctx, acroForm, PDF_NAME(CO), coArr);
    });
}

void FormManager::resetForm(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;

            // First, attempt the MuPDF built-in form reset (handles buttons,
            // checkboxes, and choice fields correctly).
            pdf_reset_form(ctx, pdfDoc, nullptr, 0);

            // Pipeline c91e7f3a phase-5: MuPDF 1.26 does not reliably reset
            // text-field /V back to /DV when the value was mutated via
            // pdf_set_field_value. Apply a manual fallback for /Tx widgets
            // only: copy /DV → /V when /DV is present.
            // Pipeline b3d7f942 phase-1 remediation: per-widget logic
            // extracted into detail::resetTxWidgetFromObj so tests can call
            // it with a null fieldObj to exercise the null guard directly.
            pdf_annot* w = pdf_first_widget(ctx, pdfPage);
            while (w) {
                pdf_obj* fieldObj = pdf_annot_obj(ctx, w);
                detail::resetTxWidgetFromObj(ctx, pdfDoc, fieldObj);
                w = pdf_next_widget(ctx, w);
            }
        }
    );
}

void FormManager::flattenForm(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    // Only THIS page's widgets (GitHub #442). pdf_bake_document(.., 0, 1),
    // used before, baked the widgets of every page of the document.
    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            bakePageWidgets(ctx, pdfPage);
        }
    );

    // Flattening bakes widgets into static content and removes the form
    // fields — a destructive structural change. Force a full-rewrite save with
    // do_garbage=3 so the orphaned widget objects are collected/de-duplicated
    // (pipeline b2e47d91, Tiers 1+2).
    if (auto* doc = page->ownerDocument()) {
        doc->markNeedsFullGc();
    }
}

bool FormManager::hasFormFields(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<bool>(
        [pdfPage](fz_context* ctx) -> bool {
            return pdf_first_widget(ctx, pdfPage) != nullptr;
        }
    );
}

FormFieldBorderAppearance FormManager::defaultBorderAppearance(FormFieldType type) {
    FormFieldBorderAppearance ap;
    if (type == FormFieldType::Signature) {
        // An unsigned signature widget is drawn by MuPDF's own "sign here"
        // display list (pdf_signature_appearance_unsigned), which ignores /MK.
        return ap;
    }
    // Border: dark slate blue, luminance ~74 of 255 -- an obvious outline.
    ap.borderColorComponents = 3;
    ap.borderColor = {0.20f, 0.30f, 0.50f, 0.0f};
    // Background: very light blue, luminance ~233 of 255 -- visibly shaded,
    // and light enough that black text stays readable over it.
    ap.backgroundColorComponents = 3;
    ap.backgroundColor = {0.88f, 0.92f, 1.00f, 0.0f};
    // 1pt solid border.
    ap.hasBorderStyle = true;
    ap.borderWidth = 1.0f;
    ap.borderStyle = 'S';
    return ap;
}

int FormManager::createFormField(const pdfe::core::Page* page, FormFieldType type,
                                  QRectF rect, const std::string& fieldName) {
    // GitHub #458: a field the app creates must be visible after placement.
    return createFormFieldWithAppearance(page, type, rect, fieldName,
                                         defaultBorderAppearance(type));
}

int FormManager::createFormFieldWithAppearance(const pdfe::core::Page* page,
                                               FormFieldType type, QRectF rect,
                                               const std::string& fieldName,
                                               const FormFieldBorderAppearance& appearance,
                                               const FormFieldData* restored) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    const FormFieldBorderAppearance* ap = &appearance;

    // GitHub #460: what a restore puts back on a variable-text widget -- the
    // captured /DA string (byte for byte) and a /Q of 1 or 2. Resolved here,
    // outside the fz_try region, into raw pointers and ints.
    const bool variableText = type == FormFieldType::Text ||
                              type == FormFieldType::ComboBox ||
                              type == FormFieldType::ListBox;
    const bool restoreDa = restored && variableText &&
                           restored->defaultAppearanceStringCaptured;
    const char* daData = restoreDa ? restored->defaultAppearanceString.data() : nullptr;
    const size_t daLen = restoreDa ? restored->defaultAppearanceString.size() : 0;
    const int quadding = (restored && variableText &&
                          (restored->quadding == 1 || restored->quadding == 2))
                             ? restored->quadding
                             : 0;

    return pImpl->context->executeReturn<int>(
        [pdfPage, type, rect, &fieldName, ap, daData, daLen, quadding](fz_context* ctx) -> int {
            pdf_document* pdfDoc = pdfPage->doc;

            // DEFECT FIX (meta-audit follow-up F17, gap A1-34): the field type
            // used to be validated only in the switch BELOW, i.e. after
            // pdf_create_annot had already attached a widget annotation to the
            // page. Rejecting an unsupported type therefore left a stray,
            // half-built widget behind: the call reported failure and the page
            // gained an annotation anyway. Validate first, create second.
            switch (type) {
                case FormFieldType::Text:
                case FormFieldType::Checkbox:
                case FormFieldType::RadioButton:
                case FormFieldType::ComboBox:
                case FormFieldType::ListBox:
                case FormFieldType::PushButton:
                case FormFieldType::Signature:
                    break;
                default:
                    fz_throw(ctx, FZ_ERROR_ARGUMENT,
                             "Unsupported form field type");
            }

            // Create widget annotation
            pdf_annot* annot = pdf_create_annot(ctx, pdfPage, PDF_ANNOT_WIDGET);
            pdf_obj* obj = pdf_annot_obj(ctx, annot);

            // Set field type
            switch (type) {
                case FormFieldType::Text:
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Tx));
                    pdf_dict_put_text_string(ctx, obj, PDF_NAME(DA), "/Helv 12 Tf 0 g");
                    break;

                case FormFieldType::Checkbox:
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Btn));
                    pdf_dict_put(ctx, obj, PDF_NAME(V), PDF_NAME(Off));
                    pdf_dict_put(ctx, obj, PDF_NAME(AS), PDF_NAME(Off));
                    break;

                case FormFieldType::RadioButton: {
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Btn));
                    // Set radio button flag (bit 15)
                    int flags = pdf_dict_get_int(ctx, obj, PDF_NAME(Ff));
                    flags |= (1 << 15); // PDF_BTN_FIELD_IS_RADIO
                    pdf_dict_put_int(ctx, obj, PDF_NAME(Ff), flags);
                    pdf_dict_put(ctx, obj, PDF_NAME(V), PDF_NAME(Off));
                    pdf_dict_put(ctx, obj, PDF_NAME(AS), PDF_NAME(Off));
                    break;
                }

                case FormFieldType::ComboBox: {
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Ch));
                    pdf_dict_put_text_string(ctx, obj, PDF_NAME(DA), "/Helv 12 Tf 0 g");
                    // Set combo box flag (bit 17)
                    int flags = pdf_dict_get_int(ctx, obj, PDF_NAME(Ff));
                    flags |= (1 << 17); // PDF_CH_FIELD_IS_COMBO
                    pdf_dict_put_int(ctx, obj, PDF_NAME(Ff), flags);
                    break;
                }

                case FormFieldType::ListBox:
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Ch));
                    pdf_dict_put_text_string(ctx, obj, PDF_NAME(DA), "/Helv 12 Tf 0 g");
                    break;

                case FormFieldType::PushButton: {
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Btn));
                    // Set pushbutton flag (bit 16)
                    int flags = pdf_dict_get_int(ctx, obj, PDF_NAME(Ff));
                    flags |= (1 << 16); // PDF_BTN_FIELD_IS_PUSHBUTTON
                    pdf_dict_put_int(ctx, obj, PDF_NAME(Ff), flags);
                    break;
                }

                case FormFieldType::Signature:
                    pdf_dict_put(ctx, obj, PDF_NAME(FT), PDF_NAME(Sig));
                    break;

                default:
                    fz_throw(ctx, FZ_ERROR_ARGUMENT, "Unsupported form field type");
            }

            // Set field name
            pdf_dict_put_text_string(ctx, obj, PDF_NAME(T), fieldName.c_str());

            // Set rectangle
            fz_rect fzRect;
            fzRect.x0 = static_cast<float>(rect.left());
            fzRect.y0 = static_cast<float>(rect.top());
            fzRect.x1 = static_cast<float>(rect.right());
            fzRect.y1 = static_cast<float>(rect.bottom());
            pdf_set_annot_rect(ctx, annot, fzRect);

            // Ensure AcroForm exists
            pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
            if (!acroForm) {
                acroForm = pdf_dict_put_dict(ctx, root, PDF_NAME(AcroForm), 2);
                pdf_dict_put_array(ctx, acroForm, PDF_NAME(Fields), 4);
            }
            pdf_obj* fields = pdf_dict_get(ctx, acroForm, PDF_NAME(Fields));
            if (!fields) {
                fields = pdf_dict_put_array(ctx, acroForm, PDF_NAME(Fields), 4);
            }

            // Add field to AcroForm Fields array
            pdf_array_push(ctx, fields, obj);

            // GitHub #458: border and background BEFORE the appearance stream
            // is generated. MuPDF's text/choice writer and its check/radio/
            // push button writer draw the background and border only from
            // /MK /BG, /MK /BC and /BS; without them the field is invisible.
            writeBorderAppearance(ctx, obj, ap);

            // GitHub #460: a restored text/combo/list widget gets back the /DA
            // it had (replacing the default written above) and its /Q, before
            // the appearance stream is generated from them.
            if (daData) {
                pdf_dict_put_string(ctx, obj, PDF_NAME(DA), daData, daLen);
            }
            if (quadding != 0) {
                pdf_dict_put_int(ctx, obj, PDF_NAME(Q), quadding);
            }

            // Update appearance stream
            pdf_update_annot(ctx, annot);

            // Count widgets to determine index
            int widgetCount = 0;
            pdf_annot* widget = pdf_first_widget(ctx, pdfPage);
            while (widget) {
                widgetCount++;
                widget = pdf_next_widget(ctx, widget);
            }

            return widgetCount - 1;
        }
    );
}

FormFieldData FormManager::deleteFormField(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    // First, capture the field data before deletion
    FormFieldData data = getFormField(page, widgetIndex);

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            const pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);

            // Remove from AcroForm Fields array
            pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
            if (acroForm) {
                pdf_obj* fields = pdf_dict_get(ctx, acroForm, PDF_NAME(Fields));
                if (fields) {
                    int fieldCount = pdf_array_len(ctx, fields);
                    for (int i = 0; i < fieldCount; i++) {
                        const pdf_obj* field = pdf_array_get(ctx, fields, i);
                        if (field == fieldObj) {
                            pdf_array_delete(ctx, fields, i);
                            break;
                        }
                    }
                }
            }

            // Delete the annotation
            pdf_delete_annot(ctx, pdfPage, widget);
        }
    );

    return data;
}

int FormManager::restoreFormField(const pdfe::core::Page* page, const FormFieldData& data) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    // GitHub #458: re-apply the border and background. Data read from a widget
    // (the delete that this restore undoes) carries the widget's own /MK and
    // /BS -- the app default for a field the app created, and whatever a field
    // loaded from a file had. Hand-built data falls back to the app default.
    const FormFieldBorderAppearance appearance =
        data.borderAppearanceCaptured ? data.borderAppearance
                                      : defaultBorderAppearance(data.type);

    // Create the field with the saved data. GitHub #460: `data` also supplies
    // the captured /DA string and /Q (text, combo and list fields only).
    int newIndex = createFormFieldWithAppearance(page, data.type, data.rect,
                                                 data.fieldName, appearance, &data);

    // Restore the value
    if (!data.value.empty()) {
        if (data.type == FormFieldType::Text ||
            data.type == FormFieldType::ComboBox ||
            data.type == FormFieldType::ListBox) {
            setFieldValue(page, newIndex, data.value);
        }
    }

    // Restore choice selection
    if (!data.selectedIndices.empty() &&
        (data.type == FormFieldType::ComboBox || data.type == FormFieldType::ListBox)) {
        setChoiceSelection(page, newIndex, data.selectedIndices);
    }

    // For checkboxes/radio buttons, if the value was "Yes" or "On", toggle them
    if ((data.type == FormFieldType::Checkbox || data.type == FormFieldType::RadioButton) &&
        (data.value == "Yes" || data.value == "On")) {
        toggleField(page, newIndex);
    }

    return newIndex;
}

// ─── GitHub #460: the default appearance of a text/combo/list field ─────────

bool FormManager::setFieldDefaultAppearance(const pdfe::core::Page* page, int widgetIndex,
                                            const std::string& font, float size,
                                            const QColor& color) {
    // Every argument is validated BEFORE the document is touched, so a
    // rejected call leaves it byte-identical.
    if (!page || widgetIndex < 0) return false;
    if (!isBase14DefaultAppearanceFont(font)) return false;
    if (!isAcceptedDefaultAppearanceSize(size)) return false;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    if (!defaultAppearanceRgb(color, rgb)) return false;

    // The key is one of five 4-letter literals; copy it into a POD buffer so
    // nothing with a destructor is referenced inside the fz_try region.
    char fontKey[8] = {};
    std::copy_n(font.c_str(), std::min<size_t>(font.size(), sizeof fontKey - 1), fontKey);
    const char* key = fontKey;
    // -0 compares equal to 0 and is accepted as auto-size; write it as "0",
    // not "-0" (pdf_print_default_appearance prints with %g).
    const float writeSize = (size == 0.0f) ? 0.0f : size;
    const float r = rgb[0];
    const float g = rgb[1];
    const float b = rgb[2];

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<bool>(
        [pdfPage, widgetIndex, key, writeSize, r, g, b](fz_context* ctx) -> bool {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) return false;
            const int t = pdf_widget_type(ctx, widget);
            if (t != PDF_WIDGET_TYPE_TEXT && t != PDF_WIDGET_TYPE_COMBOBOX &&
                t != PDF_WIDGET_TYPE_LISTBOX) {
                return false;
            }
            const float c[3] = {r, g, b};
            pdf_set_annot_default_appearance(ctx, widget, key, writeSize, 3, c);

            // Cap pathological /RV before MuPDF's recursive HTML parser sees it.
            // A deeply-nested or huge rich-text value would overflow the stack
            // inside pdf_update_annot (STATUS_STACK_OVERFLOW). When it is
            // pathological, skip appearance generation and raise
            // /NeedAppearances so the viewer synthesises the appearance itself.
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);
            if (richTextValueIsPathological(ctx, fieldObj)) {
                writeNeedAppearances(ctx, pdfPage->doc, 1);
                return true;
            }

            pdf_update_annot(ctx, widget);
            // origin/master 596d613f's fail-safe, kept in the reconciled
            // setter: if no appearance could be produced, tell the viewer to
            // synthesise one rather than show a stale or empty /AP.
            if (!widgetHasNonEmptyAppearance(ctx, widget)) {
                writeNeedAppearances(ctx, pdfPage->doc, 1);
            }
            return true;
        });
}

std::optional<FormFieldOwnDefaultAppearance> FormManager::getFieldOwnDefaultAppearance(
    const pdfe::core::Page* page, int widgetIndex) {
    if (!page || widgetIndex < 0) return std::nullopt;

    struct Raw {
        bool found = false;
        RawStringEntry da;
        RawStringEntry ds;
        RawStringEntry rc;
    };
    Raw raw;
    Raw* out = &raw;
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([pdfPage, widgetIndex, out](fz_context* ctx) {
        pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
        if (!widget) return;
        pdf_obj* obj = pdf_annot_obj(ctx, widget);
        out->found = true;
        out->da = readRawStringEntry(ctx, obj, PDF_NAME(DA));
        out->ds = readRawStringEntry(ctx, obj, PDF_NAME(DS));
        out->rc = readRawStringEntry(ctx, obj, PDF_NAME(RC));
    });
    if (!raw.found) return std::nullopt;

    // Copied out of the MuPDF objects here, outside the fz_try region; nothing
    // has modified the dictionary since the pointers were taken.
    FormFieldOwnDefaultAppearance s;
    s.hasDa = raw.da.present;
    if (raw.da.present) s.da.assign(raw.da.data, raw.da.len);
    s.hasDs = raw.ds.present;
    if (raw.ds.present) s.ds.assign(raw.ds.data, raw.ds.len);
    s.hasRc = raw.rc.present;
    if (raw.rc.present) s.rc.assign(raw.rc.data, raw.rc.len);
    return s;
}

bool FormManager::restoreFieldOwnDefaultAppearance(
    const pdfe::core::Page* page, int widgetIndex,
    const FormFieldOwnDefaultAppearance& state) {
    if (!page || widgetIndex < 0) return false;

    // Raw pointers and sizes only inside the fz_try region.
    const bool hasDa = state.hasDa;
    const char* da = state.da.data();
    const size_t daLen = state.da.size();
    const bool hasDs = state.hasDs;
    const char* ds = state.ds.data();
    const size_t dsLen = state.ds.size();
    const bool hasRc = state.hasRc;
    const char* rc = state.rc.data();
    const size_t rcLen = state.rc.size();

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    return pImpl->context->executeReturn<bool>(
        [pdfPage, widgetIndex, hasDa, da, daLen, hasDs, ds, dsLen, hasRc, rc,
         rcLen](fz_context* ctx) -> bool {
            pdf_annot* widget = getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) return false;
            pdf_obj* obj = pdf_annot_obj(ctx, widget);
            writeRawStringEntry(ctx, obj, PDF_NAME(DA), hasDa, da, daLen);
            writeRawStringEntry(ctx, obj, PDF_NAME(DS), hasDs, ds, dsLen);
            writeRawStringEntry(ctx, obj, PDF_NAME(RC), hasRc, rc, rcLen);
            pdf_dirty_annot(ctx, widget);
            pdf_update_annot(ctx, widget);
            return true;
        });
}

// Pipeline b3d7f942 phase-1 remediation (GAP-16 AC3): test seam.
// Returns the pdf_obj* (as void*) for the first widget on the page, or
// nullptr if the page has no widgets.  Defined here so it can access
// Page::fzPage() via the existing friend declaration.  Tests cast to
// pdf_obj* after including <mupdf/pdf.h>.
void* FormManager::firstWidgetObjForPageSeam(
    pdfe::core::MuPdfContext* ctx,
    const pdfe::core::Page* page)
{
    fz_page* fzPage = page->fzPage();
    return ctx->executeReturn<void*>([fzPage](fz_context* c) -> void* {
        pdf_page* pdfPage = pdf_page_from_fz_page(c, fzPage);
        if (!pdfPage) return nullptr;
        pdf_annot* w = pdf_first_widget(c, pdfPage);
        if (!w) return nullptr;
        return pdf_annot_obj(c, w);
    });
}

} // namespace pdfe::forms
