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

    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_bake_document(ctx, pdfDoc, 0, 1);
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

int FormManager::createFormField(const pdfe::core::Page* page, FormFieldType type,
                                  QRectF rect, const std::string& fieldName) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<int>(
        [pdfPage, type, rect, &fieldName](fz_context* ctx) -> int {
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

    // Create the field with the saved data
    int newIndex = createFormField(page, data.type, data.rect, data.fieldName);

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
