#pragma once

#include <string>

namespace pdfe::forms {

enum class FormFieldType {
    Text,
    Checkbox,
    RadioButton,
    ComboBox,
    ListBox,
    PushButton,
    Signature,
    Unknown
};

/// Convert FormFieldType to MuPDF PDF_WIDGET_TYPE_* constant.
int toPdfWidgetType(FormFieldType type);

/// Convert MuPDF PDF_WIDGET_TYPE_* constant to FormFieldType.
FormFieldType fromPdfWidgetType(int pdfType);

/// Get display name for a form field type.
std::string formFieldTypeName(FormFieldType type);

} // namespace pdfe::forms
