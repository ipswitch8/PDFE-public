#pragma once
#include "AnnotationData.h"
#include "AnnotationType.h"
#include "LinkAction.h"
#include <memory>
#include <vector>
#include <optional>
#include <filesystem>
#include <QRectF>
#include <QPointF>
#include <QColor>
#include <QImage>

// Forward declarations for MuPDF C types (must be at global scope, with C linkage)
extern "C" {
struct pdf_page;
struct pdf_annot;
}

namespace pdfe::core {
class MuPdfContext;
class Page;
}

namespace pdfe::annotations {

/// Manager for creating, modifying, and querying PDF annotations.
///
/// This class provides a high-level interface to MuPDF's annotation API,
/// handling all the low-level details and ensuring proper error handling.
class AnnotationManager {
public:
    explicit AnnotationManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~AnnotationManager();

    // Non-copyable
    AnnotationManager(const AnnotationManager&) = delete;
    AnnotationManager& operator=(const AnnotationManager&) = delete;

    // Movable
    AnnotationManager(AnnotationManager&&) noexcept;
    AnnotationManager& operator=(AnnotationManager&&) noexcept;

    /// Create a new annotation on the specified page.
    /// @return The newly created annotation's data (with annotIndex set)
    AnnotationData createAnnotation(
        const pdfe::core::Page* page,
        AnnotationType type,
        QRectF rect
    );

    /// Delete an annotation from a page.
    /// @return The deleted annotation's data (for undo purposes)
    AnnotationData deleteAnnotation(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Restore a previously deleted annotation.
    /// @return The index of the restored annotation
    int restoreAnnotation(
        const pdfe::core::Page* page,
        const AnnotationData& data
    );

    /// Set the text contents of an annotation.
    void setContents(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& text
    );

    /// Set the author (T key) of an annotation.
    void setAuthor(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& author
    );

    /// Set the color of an annotation.
    void setColor(
        const pdfe::core::Page* page,
        int annotIndex,
        QColor color
    );

    /// Set the opacity of an annotation.
    void setOpacity(
        const pdfe::core::Page* page,
        int annotIndex,
        float opacity
    );

    /// Set the bounding rectangle of an annotation.
    void setRect(
        const pdfe::core::Page* page,
        int annotIndex,
        QRectF rect
    );

    /// Rotate an annotation's content about the centre of its bounding box.
    ///
    /// `degrees` is counter-clockwise; `rect` is the axis-aligned bounding box
    /// the rotated content occupies, which the caller has already computed
    /// (a PDF /Rect is by definition axis-aligned, so it has to widen).
    ///
    /// Implemented by rotating the appearance stream rather than by
    /// regenerating it. MuPDF synthesises a FreeText appearance with
    /// `/Matrix` identity and `/BBox` equal to the rect, and
    /// `pdf_annot_transform` implements PDF 32000-1 12.5.5: it transforms the
    /// BBox by the Matrix and maps the result onto /Rect. Setting the Matrix
    /// to a rotation and /Rect to the rotated bounds therefore renders the
    /// whole appearance -- text, border and background together -- rotated in
    /// place and unscaled, at ANY angle, without reimplementing text layout.
    ///
    /// Also writes `/Rotate` for interoperability: Adobe reads it. It is NOT
    /// the mechanism here, because MuPDF's own handling of `/Rotate`
    /// positions correctly only for 0/90/180/270 and clips the text to the
    /// axis-aligned box, so an arbitrary angle would render cut off.
    ///
    /// Throws if the annotation's /F flags lock its geometry, matching
    /// setRect: rotating is a geometry change.
    void setRotation(
        const pdfe::core::Page* page,
        int annotIndex,
        double degrees,
        QRectF rect
    );

    /// Set the quad points for text markup annotations (highlight, underline, strikeout).
    void setQuadPoints(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::vector<QRectF>& quads
    );

    /// Set the ink strokes for ink annotations.
    void setInkStrokes(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::vector<std::vector<QPointF>>& strokes
    );

    /// Set the line endpoints for line annotations.
    void setLineEndpoints(
        const pdfe::core::Page* page,
        int annotIndex,
        QPointF start,
        QPointF end
    );

    /// Set the default appearance for FreeText annotations.
    /// @param font Font name (e.g. "Helv", "TiRo", "Cour")
    /// @param size Font size in points
    /// @param color Text color
    void setDefaultAppearance(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& font,
        float size,
        QColor color
    );

    /// F15 / PDF-51: read the annotation's /F flag mask, typed and named.
    /// Throws if annotIndex is out of range.
    AnnotationFlags getAnnotationFlags(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// F15 / PDF-51: write the annotation's /F flag mask. Named flags only —
    /// callers build the mask with AnnotationFlag enumerators.
    void setAnnotationFlags(
        const pdfe::core::Page* page,
        int annotIndex,
        AnnotationFlags flags
    );

    /// F15 / PDF-51 convenience: set/clear ONE named flag, leaving the rest
    /// of the mask untouched.
    void setAnnotationFlag(
        const pdfe::core::Page* page,
        int annotIndex,
        AnnotationFlag flag,
        bool on
    );

    /// Phase 8c: set the /Alt (alt text) on an annotation. Writes an
    /// empty string to clear it. Throws if annotIndex is out of range.
    void setAltText(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& altText
    );

    /// Phase 8c: get the /Alt value for an annotation; empty string if
    /// unset.
    std::string getAltText(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Phase 8c: set the /ActualText entry on an annotation.
    void setActualText(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& actualText
    );

    /// Phase 8c: get /ActualText; empty string if unset.
    std::string getActualText(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Phase 8a: set the /IRT (in-reply-to) reference on `replyAnnotIndex`
    /// pointing at `parentAnnotIndex` on the same page. Use -1 for
    /// parentAnnotIndex to clear the IRT reference.
    void setInReplyTo(
        const pdfe::core::Page* page,
        int replyAnnotIndex,
        int parentAnnotIndex
    );

    /// Phase 8a: get the annotIndex of the annotation that
    /// `replyAnnotIndex` replies to, or -1 if it has no /IRT.
    int getInReplyTo(
        const pdfe::core::Page* page,
        int replyAnnotIndex
    );

    /// Write a link action to a /Link annotation. Replaces any
    /// existing /A dict with the requested action type. Clears the
    /// legacy /Contents-as-URI convention from the annotation since
    /// /A takes precedence per PDF spec.
    void setLinkAction(
        const pdfe::core::Page* page,
        int annotIndex,
        const LinkAction& action
    );

    /// Read the current action from a /Link annotation's /A dict.
    /// Returns type=Uri with empty uri when no action is set.
    LinkAction getLinkAction(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Pipeline E phase-5: create a /FileAttachment annotation that
    /// embeds the given file's bytes as a PDF embedded file stream
    /// and references it via the annotation's /FS entry. The icon
    /// shown on the page is one of "PaperClip", "PushPin" (default),
    /// "Graph", or "Tag". The annotation rect should typically be
    /// small (~24x24 pt) since /FileAttachment renders as a fixed icon.
    ///
    /// Returns the created annotation's data with annotIndex set.
    /// Throws std::invalid_argument if the file cannot be read.
    AnnotationData createFileAttachmentAnnotation(
        const pdfe::core::Page* page,
        QRectF rect,
        const std::filesystem::path& filePath,
        const std::string& iconName = "PushPin",
        const std::string& displayName = ""
    );

    /// Pipeline E phase-5: extract the embedded payload of a
    /// /FileAttachment annotation to `outDir`. Filename is taken from
    /// the filespec's /UF (or /F) entry, sanitized via
    /// AttachmentManager::sanitizeFilename so traversal can't escape
    /// outDir. Returns the written path on success, empty path on
    /// failure or if the annotation is not a /FileAttachment.
    std::filesystem::path extractFileAttachmentPayload(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::filesystem::path& outDir
    );

    /// Pipeline E phase-5: read the embedded file bytes from a
    /// /FileAttachment annotation directly (without writing to disk).
    /// Returns empty vector on failure or wrong annotation type.
    std::vector<unsigned char> readFileAttachmentBytes(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Set a Form XObject image appearance on an existing /Stamp
    /// annotation. The image is added as a PDF Image XObject and
    /// referenced from a Form XObject installed as the annotation's
    /// /AP/N normal appearance, scaled to fill the annotation rect.
    ///
    /// The annotation must already exist (typically created via
    /// createAnnotation with type=Stamp). After this call the stamp
    /// renders the image at the annotation's location, and the
    /// appearance survives save/reopen round-trips.
    void setStampImageAppearance(
        const pdfe::core::Page* page,
        int annotIndex,
        const QImage& image
    );

    /// Get all annotations on a page.
    std::vector<AnnotationData> getAnnotations(const pdfe::core::Page* page);

    /// Get a specific annotation by index.
    AnnotationData getAnnotation(const pdfe::core::Page* page, int annotIndex);

    /// Find an annotation at the specified point (hit test).
    /// @return The index of the annotation if found, nullopt otherwise
    std::optional<int> getAnnotationAt(
        const pdfe::core::Page* page,
        QPointF pdfPoint
    );

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::annotations
