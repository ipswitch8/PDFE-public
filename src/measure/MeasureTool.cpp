#include "MeasureTool.h"
#include "MeasureCalc.h"
#include "../annotations/AnnotationManager.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QPolygonF>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace pdfe::measure {

MeasureTool::MeasureTool(pdfe::annotations::AnnotationManager* manager,
                         MeasureMode mode)
    : manager_(manager), mode_(mode)
{
}

pdfe::annotations::AnnotationType MeasureTool::toolType() const
{
    // Distance stores as Line; perimeter + area as FreeText with a tagged
    // contents string.
    return (mode_ == MeasureMode::Distance)
        ? pdfe::annotations::AnnotationType::Line
        : pdfe::annotations::AnnotationType::FreeText;
}

// -------- Helpers --------

namespace {

const char* modeToString(MeasureMode m) {
    switch (m) {
        case MeasureMode::Distance:  return "distance";
        case MeasureMode::Perimeter: return "perimeter";
        case MeasureMode::Area:      return "area";
    }
    return "distance";
}

MeasureMode modeFromString(const std::string& s) {
    if (s == "perimeter") return MeasureMode::Perimeter;
    if (s == "area")      return MeasureMode::Area;
    return MeasureMode::Distance;
}

} // namespace

std::string MeasureTool::buildContents(const MeasureResult& r)
{
    std::ostringstream os;
    os << "[PDFE-MEASURE:mode=" << modeToString(r.mode)
       << ";value=" << r.displayedValue
       << ";unit=" << r.unitSuffix
       << "]\n"
       << r.displayedValue << " " << r.unitSuffix;
    return os.str();
}

bool MeasureTool::parseContents(const std::string& s,
                                std::string& outMode,
                                float& outValue,
                                std::string& outUnit)
{
    const std::string prefix = "[PDFE-MEASURE:";
    const auto start = s.find(prefix);
    if (start == std::string::npos) return false;
    const auto end = s.find(']', start);
    if (end == std::string::npos) return false;

    const std::string body = s.substr(start + prefix.size(),
                                      end - start - prefix.size());
    // Split by ';' into key=value pairs.
    size_t pos = 0;
    while (pos < body.size()) {
        const auto semi = body.find(';', pos);
        const auto token = body.substr(pos, semi == std::string::npos
                                              ? std::string::npos
                                              : semi - pos);
        const auto eq = token.find('=');
        if (eq != std::string::npos) {
            const auto key = token.substr(0, eq);
            const auto val = token.substr(eq + 1);
            if (key == "mode") outMode = val;
            else if (key == "value") {
                try { outValue = std::stof(val); } catch (...) { return false; }
            }
            else if (key == "unit") outUnit = val;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return !outMode.empty();
}

Scale MeasureTool::scaleAt(QPointF pdfPoint) const
{
    if (!viewports_.empty()) {
        auto vp = MeasureDictReader::viewportAt(viewports_, pdfPoint);
        if (vp) return MeasureDictReader::scaleFrom(vp->measure, mode_);
    }
    return scale_;
}

MeasureResult MeasureTool::currentReadout() const
{
    float raw = 0.0f;
    switch (mode_) {
        case MeasureMode::Distance:
            if (points_.size() >= 2) {
                raw = MeasureCalc::distance(points_[0], points_[1]);
            } else if (points_.size() == 1) {
                raw = MeasureCalc::distance(points_[0], livePoint_);
            }
            break;
        case MeasureMode::Perimeter: {
            auto pts = points_;
            if (!pts.empty()) pts.push_back(livePoint_);
            raw = MeasureCalc::perimeter(pts);
            break;
        }
        case MeasureMode::Area: {
            auto pts = points_;
            if (!pts.empty()) pts.push_back(livePoint_);
            raw = MeasureCalc::area(pts);
            break;
        }
    }
    // GitHub #21: the scale is resolved from the FIRST clicked point, not the
    // live hover point — a measurement belongs to the viewport it started in,
    // and letting the readout flip scale mid-drag as the cursor crossed a
    // viewport boundary would make the number unreadable.
    return MeasureCalc::format(
        mode_, raw, scaleAt(points_.empty() ? livePoint_ : points_.front()));
}

// -------- Mouse handling --------

void MeasureTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton) return;
    if (!page_) return;
    if (pageIndex != currentPageIndex_ && !points_.empty()) {
        // crossing page mid-measurement — reset
        points_.clear();
    }
    currentPageIndex_ = pageIndex;
    points_.push_back(pdfPoint);
    livePoint_ = pdfPoint;

    // Distance is 2-click; on second click, commit.
    if (mode_ == MeasureMode::Distance && points_.size() == 2) {
        const MeasureResult r = MeasureCalc::format(
            MeasureMode::Distance,
            MeasureCalc::distance(points_[0], points_[1]),
            scaleAt(points_[0]));   // GitHub #21 — see scaleAt()
        if (manager_) {
            // Line annotation bounding box is the two-point bbox.
            qreal left = std::min(points_[0].x(), points_[1].x());
            qreal top  = std::min(points_[0].y(), points_[1].y());
            qreal right= std::max(points_[0].x(), points_[1].x());
            qreal bot  = std::max(points_[0].y(), points_[1].y());
            QRectF rect(left, top, right - left, bot - top);
            auto data = manager_->createAnnotation(
                page_, pdfe::annotations::AnnotationType::Line, rect);
            data.pageIndex = currentPageIndex_;
            data.color = color_;
            data.contents = buildContents(r);
            manager_->setColor(page_, data.annotIndex, color_);
            manager_->setContents(page_, data.annotIndex, data.contents);
            // CAUTION: annotationCreated runs SYNCHRONOUSLY and may DESTROY
            // this tool (PdfCanvas::annotationCreated ->
            // MainWindow::onAnnotationCreatedByTool -> onAnnotationToolSelected
            // -> PdfCanvas::clearCurrentTool -> ~MeasureTool). Reset our own
            // state FIRST and touch no member afterwards. This is the DCA 0009
            // use-after-free: it survived that fix, and phase-42's first sweep,
            // because both were scoped to src/annotations/ and MeasureTool
            // lives in src/measure/. Found by the corpus guard once it was
            // derived from the fire sites in the source instead of a directory.
            points_.clear();
            if (annotationCreated) {
                auto cb = annotationCreated;  // copy: the call may destroy this
                cb(currentPageIndex_, data);
            }
            // Do NOT access any member here -- `this` may already be destroyed.
            return;
        }
        points_.clear();
    }
}

void MeasureTool::onMouseMove(QMouseEvent* /*event*/, int pageIndex, QPointF pdfPoint)
{
    if (pageIndex != currentPageIndex_) return;
    livePoint_ = pdfPoint;
}

void MeasureTool::onMouseRelease(QMouseEvent* /*event*/, int /*pageIndex*/, QPointF /*pdfPoint*/)
{
    // Clicks are committed on press; release is a no-op.
}

void MeasureTool::onMouseDoubleClick(QMouseEvent* /*event*/, int /*pageIndex*/, QPointF /*pdfPoint*/)
{
    // Perimeter and area commit on double-click.
    if (mode_ == MeasureMode::Distance) return;
    if (points_.size() < (mode_ == MeasureMode::Area ? 3u : 2u)) {
        points_.clear();
        return;
    }
    const MeasureResult r = currentReadout();

    if (manager_ && page_) {
        // Bounding box of all points.
        QRectF rect = QPolygonF(QVector<QPointF>(points_.begin(), points_.end()))
                        .boundingRect();
        auto data = manager_->createAnnotation(
            page_, pdfe::annotations::AnnotationType::FreeText, rect);
        data.pageIndex = currentPageIndex_;
        data.color = color_;
        data.contents = buildContents(r);
        manager_->setColor(page_, data.annotIndex, color_);
        manager_->setContents(page_, data.annotIndex, data.contents);
        // Same hazard as the distance path above: reset before firing, and
        // make the callback the last thing that happens (DCA 0009).
        points_.clear();
        if (annotationCreated) {
            auto cb = annotationCreated;  // copy: the call may destroy this
            cb(currentPageIndex_, data);
        }
        // Do NOT access any member here -- `this` may already be destroyed.
        return;
    }
    points_.clear();
}

// -------- Overlay --------

void MeasureTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                 float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (pageIndex != currentPageIndex_ || points_.empty()) return;

    painter.save();
    QPen pen(color_);
    pen.setWidthF(2.0f * zoom);
    painter.setPen(pen);
    painter.setBrush(QBrush(color_, Qt::Dense6Pattern));

    auto toScreen = [&](QPointF p) {
        return pdfe::core::CoordinateTransform::pdfToScreen(p, zoom, pageOrigin, pageHeightPt);
    };

    // Build screen-space point list: committed points + hover livePoint_.
    QVector<QPointF> screenPts;
    screenPts.reserve(static_cast<int>(points_.size()) + 1);
    std::transform(points_.begin(), points_.end(),
                   std::back_inserter(screenPts), toScreen);
    screenPts.push_back(toScreen(livePoint_));

    if (mode_ == MeasureMode::Area) {
        QPolygonF poly(screenPts);
        painter.drawPolygon(poly);
    } else {
        // Distance + Perimeter draw as polyline.
        for (int i = 1; i < screenPts.size(); ++i) {
            painter.drawLine(screenPts[i - 1], screenPts[i]);
        }
    }

    // Live readout text near the cursor.
    const MeasureResult r = currentReadout();
    QFont f = painter.font();
    f.setBold(true);
    painter.setFont(f);
    const QString label = QString("%1 %2")
        .arg(r.displayedValue, 0, 'f', 2)
        .arg(QString::fromStdString(r.unitSuffix));
    painter.setBrush(Qt::white);
    painter.setPen(Qt::black);
    const QPointF cursorScreen = toScreen(livePoint_) + QPointF(8, -8);
    painter.drawText(cursorScreen, label);

    painter.restore();
}

} // namespace pdfe::measure
