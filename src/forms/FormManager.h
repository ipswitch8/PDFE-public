#pragma once

#include "FormFieldData.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <QPointF>

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
    /// @return The widget index of the created field
    int createFormField(const pdfe::core::Page* page, FormFieldType type,
                        QRectF rect, const std::string& fieldName);

    /// Delete a form field (widget annotation) from the page.
    /// @return The data of the deleted field (for undo)
    FormFieldData deleteFormField(const pdfe::core::Page* page, int widgetIndex);

    /// Restore a previously deleted form field.
    /// @return The widget index of the restored field
    int restoreFormField(const pdfe::core::Page* page, const FormFieldData& data);

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
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::forms
