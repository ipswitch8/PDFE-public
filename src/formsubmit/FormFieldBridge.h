// -*- coding: utf-8 -*-
//
// FormFieldBridge.h — PDF-48 / GitHub #18.
//
// WHY THIS FILE EXISTS
// --------------------
// `FormManager` already parses the AcroForm `Ff` **Required** bit into
// `forms::FormFieldData::isRequired` (src/forms/FormManager.cpp, PDF_FIELD_
// IS_REQUIRED). Before this change that flag went exactly one place: the
// properties panel appended the word "Required" to a label. `src/formsubmit/`
// contained ZERO references to it, so an empty required field submitted like
// any other and the flag was decoration.
//
// The enforcement lives in `FormSubmitter` (where submitting happens). This
// header is the missing half — the conversion that carries the parsed flag
// from the document model to the submit path. Without it the enforcement would
// be a rule no data ever reaches, which is its own kind of fake fix.
//
// HEADER-ONLY AND DEPENDENCY-FREE ON PURPOSE. `forms/FormFieldData.h` is a
// plain value struct (FormFieldType, QRectF, std::string) with no MuPDF in it
// and nothing to link, so `pdfe_formsubmit` gains a compile-time include and
// no library dependency. The arrow points formsubmit -> forms, never back.

#pragma once

#include "FormEncoder.h"
#include "forms/FormFieldData.h"

#include <vector>

namespace pdfe::formsubmit {

/// One document field as it should be submitted.
inline FormField toSubmitField(const pdfe::forms::FormFieldData& data)
{
    FormField f;
    f.name = data.fieldName;
    f.value = data.value;
    f.required = data.isRequired;
    return f;
}

/// Convert a page's (or document's) fields for submission.
///
/// READ-ONLY fields are carried through unchanged rather than dropped: a
/// read-only field with a value is still part of the form's data, and
/// dropping it would silently change what the receiving backend sees. The
/// Required rule is applied by `FormSubmitter::missingRequiredFields`, not
/// here, so there is one place that decides what "missing" means.
inline std::vector<FormField> toSubmitFields(
    const std::vector<pdfe::forms::FormFieldData>& fields)
{
    std::vector<FormField> out;
    out.reserve(fields.size());
    for (const auto& d : fields) out.push_back(toSubmitField(d));
    return out;
}

}  // namespace pdfe::formsubmit
