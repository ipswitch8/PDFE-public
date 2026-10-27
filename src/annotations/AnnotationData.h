#pragma once
#include "AnnotationType.h"
#include "AnnotationFlags.h"
#include <QRectF>
#include <QPointF>
#include <QColor>
#include <string>
#include <vector>
#include <cstdint>

namespace pdfe::annotations {

struct AnnotationData {
    AnnotationType type = AnnotationType::Unknown;
    QRectF rect;                              // Bounding rectangle in PDF coords
    std::string contents;                     // Text content
    std::string author;                       // Author name
    QColor color{255, 255, 0};               // Default yellow
    QColor interiorColor{Qt::transparent};    // Fill color
    float opacity = 1.0f;                     // 0.0 to 1.0
    float borderWidth = 1.0f;
    // PDF annotation flags (/F).  F15 / PDF-51: typed + named, never a bare
    // int — see AnnotationFlags.h.
    AnnotationFlags flags{};

    // Text markup (highlight, underline, strikeout)
    std::vector<QRectF> quadPoints;

    // Ink annotation (freehand drawing)
    std::vector<std::vector<QPointF>> inkStrokes;

    // Line annotation endpoints
    QPointF lineStart;
    QPointF lineEnd;

    // Timestamps
    int64_t creationDate = 0;
    int64_t modificationDate = 0;

    // Internal tracking
    int pageIndex = -1;
    int annotIndex = -1;    // Index in page's annotation list

    // Icon name (for stamp/text annotations)
    std::string iconName;

    bool isOpen = false;  // Whether the popup is open (PDF /Open flag)

    // Phase 8a: reply-chain threading. When this annotation is a reply,
    // inReplyToIndex holds the index (within the same page's annotation
    // list) of the annotation it replies to. -1 means "not a reply".
    int inReplyToIndex = -1;

    // Default appearance (for FreeText annotations)
    std::string daFontName;   // e.g. "Helv", "TiRo", "Cour"
    float daFontSize = 12.0f;
    QColor daColor{0, 0, 0};  // Text color for /DA string

    // Rotation of the annotation's content, in degrees counter-clockwise
    // about the centre of `rect`. 0 for everything that has never been
    // rotated, so existing and default-constructed data is unaffected.
    //
    // Field-reported 2026-08-28: the rotate handle turned the selection
    // outline but not the text, because the angle existed only as UI state in
    // SelectTool and was never stored anywhere. `rect` was widened to the
    // axis-aligned bounds of the rotated shape and the angle itself thrown
    // away, so the renderer could not have drawn rotated text even in
    // principle. See docs/audit/field-report-2026-08-28-scanned-manual.md (2).
    //
    // `rect` REMAINS the axis-aligned bounding box of the rotated content --
    // that is what a PDF /Rect is, and the widening the old code already did
    // is exactly right for it. This is the angle that was missing beside it.
    double rotationDegrees = 0.0;

    // The box the content is laid out in BEFORE rotating. Empty when the
    // annotation has never been rotated.
    //
    // Carried separately from `rect` because it cannot be recovered from it:
    // inverting  W = hw|cos| + hh|sin|,  H = hw|sin| + hh|cos|  is singular at
    // 45 degrees. It is needed by any path that REBUILDS an annotation from
    // this struct rather than mutating one in place -- restoreAnnotation (the
    // undo-after-delete path) being the one that exists today. Without it,
    // deleting a rotated text box and pressing Ctrl+Z gave the text back
    // upright.
    QRectF rotationContentRect;
};

} // namespace pdfe::annotations
