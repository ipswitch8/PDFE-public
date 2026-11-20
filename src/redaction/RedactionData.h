#pragma once

#include <QRectF>
#include <QColor>
#include <optional>
#include <string>
#include <vector>

namespace pdfe::redaction {

/// Image handling method during redaction application.
enum class RedactionImageMethod {
    None,                    ///< Do not touch images
    Remove,                  ///< Remove every image the mark touches, entirely
    Pixels,                  ///< Blank the image pixels under the mark; an image the
                             ///< mark covers completely is removed (the default)
    RemoveUnlessInvisible    ///< Remove an image the mark touches where it is visible
};

/// Text handling method during redaction application.
enum class RedactionTextMethod {
    Remove,    ///< Remove text (default, secure)
    None       ///< Do not touch text (INSECURE)
};

/// Line art handling method during redaction application.
enum class RedactionLineArtMethod {
    None,               ///< Do not touch line art
    RemoveIfCovered,    ///< Remove if fully covered by redaction
    RemoveIfTouched     ///< Remove if any part touches redaction
};

/// Data describing a single redaction mark (annotation).
struct RedactionData {
    QRectF rect;                    ///< Region to redact in PDF coordinates
    int pageIndex = -1;             ///< Page containing this redaction
    int annotIndex = -1;            ///< Annotation index on page
    QColor fillColor{Qt::black};    ///< Fill color of the redaction box
    std::string overlayText;        ///< Optional text to show in redacted area
    bool enabled = true;            ///< Whether this mark will be applied

    // Per-mark option overrides. Unset: the mark is applied with the
    // RedactionApplyOptions method. Set: this mark is applied with its own
    // method, whatever the options say (RedactionManager::applyPageRedactions
    // applies each mark on its own when the marks of a page differ).
    // RedactionManager::restoreRedactionMark stores them on the mark and the
    // getters read them back. They are honoured only for marks made in this
    // session: a value found in a file is ignored, so a document cannot ship
    // marks that quietly leave their content in place when applied.
    std::optional<RedactionImageMethod> imageMethod;
    std::optional<RedactionTextMethod> textMethod;
    std::optional<RedactionLineArtMethod> lineArtMethod;
};

/// Options controlling the apply-all operation.
struct RedactionApplyOptions {
    bool blackBoxes = true;
    /// Pixels (GitHub #440): a mark over part of an image destroys only the
    /// pixels under it and keeps the rest; a mark covering the whole image
    /// removes the image.
    RedactionImageMethod imageMethod = RedactionImageMethod::Pixels;
    RedactionTextMethod textMethod = RedactionTextMethod::Remove;
    RedactionLineArtMethod lineArtMethod = RedactionLineArtMethod::RemoveIfCovered;
    bool rewriteDocument = true;     ///< QPDF single-revision rewrite to eliminate data ghosts

    // --- Confidentiality-surface scrubbing (see RedactionScrubber.h) ---
    // `pdf_redact_page` only clears page content, images and the redaction
    // annotations. These options control the surfaces MuPDF leaves behind.

    /// Scrub PAGE-scoped surfaces: text inside form XObjects, annotations
    /// overlapping the redacted region, and struct-tree /ActualText + /Alt
    /// entries for the removed spans. Unconditional by design — the user asked
    /// for that region to be gone. Default ON (secure).
    bool scrubPageSurfaces = true;

    /// Scrub DOCUMENT-scoped surfaces — metadata + XMP, embedded files,
    /// outlines/bookmarks, document JavaScript — WHERE THEY CARRY THE REDACTED
    /// TEXT. Unrelated entries survive (blanket-deleting every bookmark
    /// because one page was redacted would be its own defect). Default ON.
    bool scrubDocumentSurfaces = true;

    /// Opt-in: remove the document-scoped surfaces WHOLESALE rather than only
    /// the entries carrying the redacted text. Default OFF (the safe default
    /// is content-matched removal).
    bool fullSanitize = false;
};

/// Result of applying redactions.
struct RedactionApplyResult {
    bool success = false;
    int marksApplied = 0;
    int pagesAffected = 0;
    std::string errorMessage;
    bool documentRewritten = false;
};

/// Result of verifying redaction application.
struct RedactionVerifyResult {
    bool success = false;
    bool textRemoved = true;         ///< No redacted text remains
    bool noOrphanStreams = true;     ///< No orphan content streams
    bool singleRevision = true;     ///< Only one revision (no previous data)
    std::string errorMessage;
    std::vector<std::string> warnings;
};

} // namespace pdfe::redaction
