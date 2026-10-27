#pragma once

#include "FormFieldType.h"
#include <QRectF>
#include <string>
#include <vector>

namespace pdfe::forms {

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

    // Tracking
    int pageIndex = -1;
    int widgetIndex = -1;                        // Index in page's widget list
};

} // namespace pdfe::forms
