#pragma once

#include "FormFieldData.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <QColor>
#include <QPointF>
#include <QColor>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::forms {

/// Manager for querying and modifying PDF form fields (widgets).
///
/// This class provides a high-level interface to MuPDF's form widget API,
/// handling all the low-level details and ensuring proper error handling.
/// Mirrors the AnnotationManager pattern from the annotations module.
class FormManager {
public:
    explicit FormManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~FormManager();

    // Non-copyable
    FormManager(const FormManager&) = delete;
    FormManager& operator=(const FormManager&) = delete;

    // Movable
    FormManager(FormManager&&) noexcept;
    FormManager& operator=(FormManager&&) noexcept;

    /// Get all form fields on a page.
    std::vector<FormFieldData> getFormFields(const pdfe::core::Page* page);

    /// Get a specific form field by widget index.
    FormFieldData getFormField(const pdfe::core::Page* page, int widgetIndex);

    /// Find a form field at the specified point (hit test).
    /// @return The widget index if found, nullopt otherwise.
    std::optional<int> getFormFieldAt(const pdfe::core::Page* page, QPointF pdfPoint);

    /// Set the string value of a text/combo/list field.
    /// @return The old value (for undo purposes).
    std::string setFieldValue(const pdfe::core::Page* page, int widgetIndex,
                              const std::string& value);

    /// Read a field's current /DA string (inheritable lookup: the widget's
    /// own /DA, else its /Parent chain's). Returns "" when the field has no
    /// /DA or the index does not resolve. (Master bug 3; the setter is the
    /// validated setFieldDefaultAppearance below.)
    std::string getFieldDefaultAppearance(const pdfe::core::Page* page,
                                          int widgetIndex);

    /// Toggle a checkbox or radio button.
    void toggleField(const pdfe::core::Page* page, int widgetIndex);

    /// Set selected options for a choice widget (combo/list).
    void setChoiceSelection(const pdfe::core::Page* page, int widgetIndex,
                            const std::vector<int>& selectedIndices);

    /// Get tab order (widget indices sorted by reading order,
    /// or custom order if setTabOrder was called).
    std::vector<int> getTabOrder(const pdfe::core::Page* page);

    /// Set custom tab order for the page's form fields.
    /// @param newOrder Widget indices in the desired tab order.
    /// Persists by reordering /Annots array entries in the document.
    void setTabOrder(const pdfe::core::Page* page, const std::vector<int>& newOrder);

    /// Get the form-wide calculation order (/AcroForm /CO). Returns
    /// an ordered list of field names as stored in the PDF — empty
    /// when no /CO entry is present. Calc order is a DOCUMENT-level
    /// concern (unlike tab order which is per-page), so this operates
    /// on the Document rather than a Page.
    std::vector<std::string> getCalculationOrder(const pdfe::core::Document* doc);

    /// Set the form-wide calculation order. `fieldNames` must match
    /// existing field names in the AcroForm; unknown names are
    /// silently skipped. Passing an empty vector clears /CO.
    void setCalculationOrder(const pdfe::core::Document* doc,
                             const std::vector<std::string>& fieldNames);

    /// Return every field name in the AcroForm (document-level), in
    /// the order they appear in /Fields. Used to populate the
    /// calculation-order editor with the full universe of candidates.
    std::vector<std::string> listFieldNames(const pdfe::core::Document* doc);

    /// F15 / PDF-41: read the AcroForm's /NeedAppearances flag. Returns false
    /// when the document has no AcroForm or no /NeedAppearances entry (the
    /// spec default).
    bool needAppearances(const pdfe::core::Document* doc);

    /// F15 / PDF-41: write the AcroForm's /NeedAppearances flag. Setting it
    /// true tells a viewer it must synthesise widget appearance streams
    /// itself; PDFE sets it automatically when it fills a field whose
    /// appearance it could not generate, so the value is never invisible.
    void setNeedAppearances(const pdfe::core::Document* doc, bool value);

    /// Reset all form fields to default values.
    void resetForm(const pdfe::core::Page* page);

    /// Flatten/bake form widgets into static content.
    /// WARNING: This is destructive and cannot be undone.
    void flattenForm(const pdfe::core::Page* page);

    /// Check if the page has any form fields.
    bool hasFormFields(const pdfe::core::Page* page);

    /// Create a new form field on the page.
    ///
    /// GitHub #458: the widget gets defaultBorderAppearance(type) -- a border
    /// colour, a light background (/MK /BC, /MK /BG) and a 1pt solid /BS --
    /// written before its appearance stream is generated, so it is visible.
    /// @return The widget index of the created field
    int createFormField(const pdfe::core::Page* page, FormFieldType type,
                        QRectF rect, const std::string& fieldName);

    /// GitHub #458: the border and background a field created by the app
    /// gets: a dark slate-blue 1pt solid border and a very light blue
    /// background. Every type except Signature gets it; an unsigned signature
    /// widget draws MuPDF's own "sign here" appearance, which ignores /MK, so
    /// Signature gets an empty appearance (no /MK, no /BS).
    static FormFieldBorderAppearance defaultBorderAppearance(FormFieldType type);

    /// Delete a form field (widget annotation) from the page.
    /// @return The data of the deleted field (for undo)
    FormFieldData deleteFormField(const pdfe::core::Page* page, int widgetIndex);

    /// Restore a previously deleted form field.
    ///
    /// GitHub #458: the border and background are re-applied -- the ones
    /// captured from the deleted widget when data.borderAppearanceCaptured,
    /// else defaultBorderAppearance(data.type) -- so undo/redo of create and
    /// of delete keeps the field visible, and keeps a loaded field's own /MK.
    ///
    /// GitHub #460: for a text, combo or list field the /DA in effect for the
    /// deleted widget (data.defaultAppearanceString, when
    /// data.defaultAppearanceStringCaptured) is written back byte for byte as
    /// the restored widget's own /DA, instead of the hard-coded
    /// "/Helv 12 Tf 0 g", and a /Q of 1 or 2 (data.quadding) is written back.
    /// Both land before the appearance stream is generated.
    /// @return The widget index of the restored field
    int restoreFormField(const pdfe::core::Page* page, const FormFieldData& data);

    /// GitHub #460: set a text, combo or list field's default appearance
    /// (/DA font, size and colour) and regenerate its appearance stream.
    ///
    /// Writes through pdf_set_annot_default_appearance (as
    /// "/<font> <size> Tf <r> <g> <b> rg") then pdf_update_annot. MuPDF
    /// 1.26.10's setter also deletes the widget's /DS and /RC.
    ///
    /// Rejected -- false, and the document is NOT modified -- when:
    ///  - `page` is null or `widgetIndex` does not name a widget;
    ///  - the widget is not a text, combo or list field (a button has no
    ///    variable text to apply a /DA to);
    ///  - `font` is not a base-14 key (isBase14DefaultAppearanceFont: Helv,
    ///    TiRo, Cour, Symb, ZaDb);
    ///  - `size` is not 0 (auto) or within 1..1000 pt
    ///    (isAcceptedDefaultAppearanceSize): NaN, inf, negative, (0, 1) and
    ///    > 1000 are all rejected rather than clamped;
    ///  - `color` is invalid, not opaque (a /DA colour has no alpha), or an
    ///    extended-RGB colour with a component outside [0, 1] or non-finite.
    /// Any other QColor spec is converted to RGB; the /DA always carries an
    /// RGB triple. getFormField() then reads the new /DA back (daFontName,
    /// daFontSize, daColor).
    ///
    /// This is the ONE /DA setter (pipeline form-reconcile, which merged it
    /// with origin/master's string-returning setter of the same name; the
    /// undo snapshot that master's return value carried is
    /// getFieldOwnDefaultAppearance, which also keeps /DS and /RC).
    ///
    /// Bold and italic are NOT accepted, deliberately. The base-14 style
    /// variants have /DA keys (HeBo, HeOb, HeBO, TiBo, TiIt, TiBI, CoBo, CoOb,
    /// CoBO), but MuPDF 1.26.10 cannot render them: its appearance writer
    /// reads the /DA through pdf_annot_default_appearance, whose
    /// pdf_parse_default_appearance maps every name other than Helv, TiRo,
    /// Cour, Symb and ZaDb to "Helv" (pdf-annot.c), and write_variable_text
    /// loads the font through full_font_name, which does the same
    /// (pdf-appearance.c). A /HeBo field would therefore render, and read
    /// back, as regular Helvetica while its /DA claimed bold. Every variant
    /// key is rejected like any other non-base-14 name.
    /// FormFieldDefaultAppearanceReconcile.StyleVariantKeysAreRejected... and
    /// ...MuPdfRendersAStyleVariantDaAsTheRegularFace pin both halves.
    ///
    /// A MuPDF error while writing propagates as an exception, like the other
    /// mutators here.
    bool setFieldDefaultAppearance(const pdfe::core::Page* page, int widgetIndex,
                                   const std::string& font, float size,
                                   const QColor& color);

    /// GitHub #460: the widget's own /DA, /DS and /RC strings, byte for byte
    /// (SetFieldAppearanceCommand's undo snapshot). nullopt when `page` is
    /// null or `widgetIndex` does not name a widget.
    std::optional<FormFieldOwnDefaultAppearance> getFieldOwnDefaultAppearance(
        const pdfe::core::Page* page, int widgetIndex);

    /// GitHub #460: put back a snapshot taken by getFieldOwnDefaultAppearance:
    /// each entry is written byte for byte when present in `state` and removed
    /// when absent, then the appearance stream is regenerated. This is the
    /// undo/redo path, so it writes the captured bytes WITHOUT the setter's
    /// base-14 validation -- it only ever puts back what the widget held.
    /// False (nothing written) when `page` is null or the index is invalid.
    bool restoreFieldOwnDefaultAppearance(const pdfe::core::Page* page,
                                          int widgetIndex,
                                          const FormFieldOwnDefaultAppearance& state);

    // Pipeline b3d7f942 phase-1 remediation (GAP-16 AC3): test seam.
    // Returns the pdf_obj* (as void*) for the first widget on the page, or
    // nullptr if there are no widgets.  The void* avoids pulling MuPDF
    // headers into this public header.  Tests cast to pdf_obj* after
    // including <mupdf/pdf.h>.  Defined in FormManager.cpp.
    // Public so GoogleTests can call it without friend declarations.
    static void* firstWidgetObjForPageSeam(
        pdfe::core::MuPdfContext* ctx,
        const pdfe::core::Page* page);

private:
    /// createFormField with an explicit border appearance. restoreFormField
    /// passes the appearance captured from the deleted widget, and (GitHub
    /// #460) the deleted widget's data as `restored`, whose /DA string and /Q
    /// are written before the appearance stream is generated. `restored` is
    /// null for a new field.
    int createFormFieldWithAppearance(const pdfe::core::Page* page,
                                      FormFieldType type, QRectF rect,
                                      const std::string& fieldName,
                                      const FormFieldBorderAppearance& appearance,
                                      const FormFieldData* restored = nullptr);

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::forms
