#pragma once
#include "AnnotationTool.h"

#include <QImage>
#include <QString>

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for placing stamp annotations (PDF_ANNOT_STAMP).
///
/// Drag to define the stamp rectangle. On release, creates a stamp
/// annotation with a configurable text/icon name (e.g., "Approved",
/// "Confidential", "Draft"). The stamp appearance is rendered by MuPDF
/// based on the standard stamp names.
class StampTool : public AnnotationTool {
public:
    explicit StampTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::Stamp; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;


    /// Stamp name or text. Common values: "Approved", "Draft", "Confidential",
    /// "NotApproved", "AsIs", "Experimental", "Expired", "Final", "Sold",
    /// "TopSecret", "ForComment", "ForPublicRelease", "NotForPublicRelease".
    /// If the name isn't a standard stamp, it's stored as the annotation's contents.
    ///
    /// Phase 4f: the string may contain {date} and {author} tokens; they
    /// are resolved at placement time and the resolved text is baked into
    /// the annotation's contents (so re-opening the file shows the resolved
    /// value, not the token).
    void setStampName(const QString& name) { stampName_ = name; }
    QString stampName() const { return stampName_; }

    /// Phase 4f: author string for {author} token resolution. Defaults to
    /// empty; MainWindow populates from AppSettings::author().
    void setAuthor(const QString& author) { author_ = author; }
    QString author() const { return author_; }

    void setColor(QColor color) override { color_ = color; }

    /// Phase 4f: built-in dynamic stamp types.
    enum class DynamicType { Draft, Approved, Reviewed };

    /// Phase 4f: template string for each built-in dynamic stamp.
    /// Exposed for testing.
    static QString builtInTemplate(DynamicType type);

    /// Phase 4f: resolve {date} and {author} tokens in a stamp template.
    /// `isoDate` is typically today's date in YYYY-MM-DD form.
    /// Exposed for testing.
    static QString resolveTokens(const QString& raw,
                                 const QString& author,
                                 const QString& isoDate);

    /// Pipeline E phase-4: switch the tool into image-stamp mode. When
    /// set to a non-null QImage, releasing the drag creates a /Stamp
    /// annotation whose appearance is the supplied image (rendered as
    /// a Form XObject with an embedded Image XObject) instead of a
    /// text label. `name` is recorded in the annotation's /Name entry
    /// (via Contents) for retrieval. Pass a null QImage to revert to
    /// the default text-stamp behavior.
    void setImageStamp(const QString& name, const QImage& image);

    /// True when an image stamp has been set via setImageStamp().
    bool isImageStamp() const { return !stampImage_.isNull(); }

    /// The current stamp image, or null QImage if not in image mode.
    QImage stampImage() const { return stampImage_; }

private:
    AnnotationManager* manager_ = nullptr;
    QColor color_{200, 0, 0};  // Default red
    QString stampName_ = "Draft";
    QString author_;
    QImage stampImage_;        // empty for text-only stamps

    int currentPageIndex_ = -1;
    bool isDrawing_ = false;
    QPointF startPoint_;
    QPointF endPoint_;
};

} // namespace pdfe::annotations
