#pragma once
#include <string>

// MuPDF annotation type enum - must match pdf_annot_type in mupdf/pdf/annot.h
// Forward-declare to avoid including MuPDF headers here
namespace pdfe::annotations {

enum class AnnotationType {
    Text,         // PDF_ANNOT_TEXT - sticky notes
    Highlight,    // PDF_ANNOT_HIGHLIGHT
    Underline,    // PDF_ANNOT_UNDERLINE
    StrikeOut,    // PDF_ANNOT_STRIKE_OUT
    Ink,          // PDF_ANNOT_INK - freehand drawing
    FreeText,     // PDF_ANNOT_FREE_TEXT - text box
    Square,       // PDF_ANNOT_SQUARE
    Circle,       // PDF_ANNOT_CIRCLE
    Line,         // PDF_ANNOT_LINE
    Stamp,        // PDF_ANNOT_STAMP
    Unknown,
    Link,         // PDF_ANNOT_LINK - hyperlink (URI or internal GoTo)
                  // Keep AFTER Unknown to preserve enum ABI for existing builds
    FileAttachment // PDF_ANNOT_FILE_ATTACHMENT - on-page clip/icon with embedded file
                  // Pipeline E phase-5: kept at end so the existing enum ABI is unchanged
};

/// Convert AnnotationType to MuPDF pdf_annot_type integer
int toPdfAnnotType(AnnotationType type);

/// Convert MuPDF pdf_annot_type integer to AnnotationType
AnnotationType fromPdfAnnotType(int pdfType);

/// Get display name for annotation type
std::string annotationTypeName(AnnotationType type);

} // namespace pdfe::annotations
