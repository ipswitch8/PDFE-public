#include "FormFieldType.h"

#include <mupdf/pdf.h>

namespace pdfe::forms {

int toPdfWidgetType(FormFieldType type) {
    switch (type) {
        case FormFieldType::Text:        return PDF_WIDGET_TYPE_TEXT;
        case FormFieldType::Checkbox:    return PDF_WIDGET_TYPE_CHECKBOX;
        case FormFieldType::RadioButton: return PDF_WIDGET_TYPE_RADIOBUTTON;
        case FormFieldType::ComboBox:    return PDF_WIDGET_TYPE_COMBOBOX;
        case FormFieldType::ListBox:     return PDF_WIDGET_TYPE_LISTBOX;
        case FormFieldType::PushButton:  return PDF_WIDGET_TYPE_BUTTON;
        case FormFieldType::Signature:   return PDF_WIDGET_TYPE_SIGNATURE;
        default:                         return PDF_WIDGET_TYPE_UNKNOWN;
    }
}

FormFieldType fromPdfWidgetType(int pdfType) {
    switch (pdfType) {
        case PDF_WIDGET_TYPE_TEXT:        return FormFieldType::Text;
        case PDF_WIDGET_TYPE_CHECKBOX:    return FormFieldType::Checkbox;
        case PDF_WIDGET_TYPE_RADIOBUTTON: return FormFieldType::RadioButton;
        case PDF_WIDGET_TYPE_COMBOBOX:    return FormFieldType::ComboBox;
        case PDF_WIDGET_TYPE_LISTBOX:     return FormFieldType::ListBox;
        case PDF_WIDGET_TYPE_BUTTON:      return FormFieldType::PushButton;
        case PDF_WIDGET_TYPE_SIGNATURE:   return FormFieldType::Signature;
        default:                          return FormFieldType::Unknown;
    }
}

std::string formFieldTypeName(FormFieldType type) {
    switch (type) {
        case FormFieldType::Text:        return "Text";
        case FormFieldType::Checkbox:    return "Checkbox";
        case FormFieldType::RadioButton: return "Radio Button";
        case FormFieldType::ComboBox:    return "Combo Box";
        case FormFieldType::ListBox:     return "List Box";
        case FormFieldType::PushButton:  return "Push Button";
        case FormFieldType::Signature:   return "Signature";
        default:                         return "Unknown";
    }
}

} // namespace pdfe::forms
