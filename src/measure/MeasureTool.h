#pragma once

#include "MeasureTypes.h"
#include "MeasureDictReader.h"
#include "../annotations/AnnotationTool.h"

#include <QColor>
#include <QPointF>
#include <vector>

namespace pdfe::core {
class Page;
}
namespace pdfe::annotations {
class AnnotationManager;
}

namespace pdfe::measure {

/// Measurement tool supporting distance / perimeter / area with a live
/// readout overlay. On completion, creates an annotation that embeds the
/// computed value + unit suffix in the annotation's /Contents so the value
/// survives save/reload.
///
/// Persistence strategy: distance → Line annotation (a,b endpoints);
/// perimeter + area → FreeText bounding-box annotation whose contents
/// starts with a machine-readable tag "[PDFE-MEASURE:mode=...; value=...;
/// unit=...]" followed by a human summary.
class MeasureTool : public pdfe::annotations::AnnotationTool {
public:
    MeasureTool(pdfe::annotations::AnnotationManager* manager, MeasureMode mode);

    pdfe::annotations::AnnotationType toolType() const override;

    void setScale(const Scale& s) { scale_ = s; }

    /// GitHub #21 / PDF-57 — hand the tool the page's OWN measurement
    /// properties, read from `/VP` by MeasureDictReader.
    ///
    /// Without this the tool measured every document at whatever scale the
    /// user had typed in, ignoring the scale the document itself declares —
    /// which is the whole defect PDF-57 records. Callers get the list from
    /// `MeasureDictReader::readViewports(page)`; passing an empty list
    /// restores the previous behaviour exactly.
    void setPageMeasures(std::vector<Viewport> viewports) {
        viewports_ = std::move(viewports);
    }
    [[nodiscard]] const std::vector<Viewport>& pageMeasures() const {
        return viewports_;
    }

    /// The scale in force at @p pdfPoint.
    ///
    /// A `/Measure` the DOCUMENT states is authoritative over the scale the
    /// user typed: the document author knows the drawing's scale and the user
    /// is guessing. `setScale()` remains the fallback for the documents —
    /// the overwhelming majority — that declare nothing. When the point falls
    /// in more than one viewport the LAST one in `/VP` wins (§12.9), which is
    /// `MeasureDictReader::viewportAt`'s job.
    ///
    /// Pure and point-in/scale-out precisely so it can be tested without
    /// synthesising mouse events.
    [[nodiscard]] Scale scaleAt(QPointF pdfPoint) const;
    void setMode(MeasureMode m) { mode_ = m; points_.clear(); }
    MeasureMode mode() const { return mode_; }
    void setColor(QColor c) override { color_ = c; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Expose the current live readout for testing / status bar display.
    MeasureResult currentReadout() const;

    /// Build the tagged contents string stored in the annotation.
    /// Format: "[PDFE-MEASURE:mode=<mode>;value=<val>;unit=<suffix>]\n<human>"
    static std::string buildContents(const MeasureResult& r);

    /// Parse a previously-built contents string. Returns true on success.
    /// Populates outMode, outValue, outUnit.
    static bool parseContents(const std::string& s,
                              std::string& outMode,
                              float& outValue,
                              std::string& outUnit);

private:
    pdfe::annotations::AnnotationManager* manager_ = nullptr;
    MeasureMode mode_;
    Scale scale_;
    /// The page's `/VP` viewports, empty unless setPageMeasures() was called.
    std::vector<Viewport> viewports_;
    QColor color_{0, 120, 200};

    std::vector<QPointF> points_;  // accumulated clicks in PDF coords
    int currentPageIndex_ = -1;
    QPointF livePoint_;            // mouse position between clicks (hover)
};

} // namespace pdfe::measure
