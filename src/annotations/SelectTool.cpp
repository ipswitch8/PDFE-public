#include "SelectTool.h"
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCoreApplication>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

namespace pdfe::annotations {

SelectTool::SelectTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void SelectTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                 float zoom, QPointF pageOrigin, float /*pageHeightPt*/)
{
    lastZoom_ = zoom;

    auto pdfToScreen = [&](const QRectF& r) -> QRectF {
        float x = static_cast<float>(pageOrigin.x()) + static_cast<float>(r.left()) * zoom;
        float y = static_cast<float>(pageOrigin.y()) + static_cast<float>(r.top()) * zoom;
        float w = static_cast<float>(r.width()) * zoom;
        float h = static_cast<float>(r.height()) * zoom;
        return QRectF(x, y, w, h);
    };

    // Draw rubber-band rectangle
    if (isRubberBanding_ && pageIndex == currentPageIndex_) {
        QRectF rbPdf = QRectF(rubberBandStart_, rubberBandEnd_).normalized();
        QRectF rbScreen = pdfToScreen(rbPdf);
        QPen rbPen(QColor(0, 120, 215, 180), 1.0f, Qt::DashLine);
        painter.setPen(rbPen);
        painter.setBrush(QColor(0, 120, 215, 40));
        painter.drawRect(rbScreen);
        return;
    }

    // Draw multi-selection highlights. The page_ guard is defensive: this is the
    // only paint path that re-queries the manager with the cached raw Page*, so
    // never touch it once the document has gone (onDocumentClosing nulls page_).
    if (page_ && !selectedAnnotIndices_.empty() && pageIndex == currentPageIndex_) {
        QPen selPen(QColor(0, 120, 215), 2.0f, Qt::DashLine);
        painter.setPen(selPen);
        painter.setBrush(Qt::NoBrush);
        // GitHub #234: THIS LOOP MUST NOT THROW.
        //
        // getAnnotation() -> MuPdfContext::execute() converts any fz error into
        // a C++ MuPdfException, and an index that no longer resolves produces
        // exactly that (`fz_throw(FZ_ERROR_ARGUMENT, "Annotation index out of
        // range")`). We are inside QWidget::paintEvent; Qt does not permit an
        // exception to unwind through an event handler, so letting one escape
        // terminated the process. Measured deterministic: three consecutive
        // runs of tests/test_ui_annotations.py crashed at 98 s, 99 s, 99 s.
        //
        // onAnnotationsChanged() is the fix for the KNOWN mutation paths. This
        // is the invariant that holds for the ones nobody has wired yet: a
        // paint handler re-querying a model it does not own must tolerate the
        // model having moved. Drawing a selection box for an annotation that no
        // longer exists is meaningless anyway, so skipping is also the
        // semantically correct answer, not merely the safe one.
        //
        // The bad index is PRUNED rather than skipped-and-kept: retaining it
        // would re-throw on every single repaint, turning one stale index into
        // an unbounded stream of exceptions behind a catch.
        std::vector<int> stillValid;
        stillValid.reserve(selectedAnnotIndices_.size());
        for (int idx : selectedAnnotIndices_) {
            QRectF rect;
            try {
                rect = manager_->getAnnotation(page_, idx).rect;
            } catch (const std::exception&) {
                // Stale index: the annotation set changed under us without
                // onAnnotationsChanged() being raised. Drop it and carry on.
                continue;
            }
            stillValid.push_back(idx);
            painter.drawRect(pdfToScreen(rect));
        }
        if (stillValid.size() != selectedAnnotIndices_.size()) {
            selectedAnnotIndices_ = std::move(stillValid);
            if (selectedAnnotIndices_.empty())
                selectedAnnotIndex_ = -1;
        }
        // DELIBERATELY no selectionCleared() callback here. It is tempting --
        // the selection really did just change, and the toolbars would like to
        // know. But that callback runs MainWindow code that may call update(),
        // and we are inside paintEvent: a re-entrant repaint is exactly the
        // class of hazard this whole fix exists to avoid, and trading a crash
        // for a paint recursion is not a fix. State is corrected here; the UI
        // catches up on the next interaction, which is also when a user could
        // first act on it.
        return;
    }

    if (selectedAnnotIndex_ < 0 || pageIndex != currentPageIndex_)
        return;

    QRectF screenRect = pdfToScreen(selectedAnnotRect_);

    // Apply rotation transform for visual feedback
    if (std::abs(rotationDegrees_) > 0.5) {
        painter.save();
        painter.translate(screenRect.center());
        painter.rotate(rotationDegrees_);
        painter.translate(-screenRect.center());
    }

    // Selection border
    QPen selPen(QColor(0, 120, 215), 2.0f, Qt::DashLine);
    painter.setPen(selPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(screenRect);

    // Corner handles
    const float hs = 6.0f;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 120, 215));

    const QPointF corners[] = {
        screenRect.topLeft(), screenRect.topRight(),
        screenRect.bottomLeft(), screenRect.bottomRight()
    };
    for (const auto& c : corners) {
        painter.drawRect(QRectF(c.x() - hs / 2, c.y() - hs / 2, hs, hs));
    }

    // Edge midpoint handles
    const QPointF edges[] = {
        QPointF(screenRect.center().x(), screenRect.top()),
        QPointF(screenRect.right(), screenRect.center().y()),
        QPointF(screenRect.center().x(), screenRect.bottom()),
        QPointF(screenRect.left(), screenRect.center().y())
    };
    for (const auto& e : edges) {
        painter.drawRect(QRectF(e.x() - hs / 2, e.y() - hs / 2, hs, hs));
    }

    // Rotation handle: circle above top center, connected by a line
    QPointF rotHandleScreen(screenRect.center().x(), screenRect.top() - 25.0f);
    painter.setPen(QPen(QColor(0, 120, 215), 1.5f));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(screenRect.center().x(), screenRect.top()), rotHandleScreen);
    painter.setBrush(QColor(0, 180, 80));
    painter.drawEllipse(rotHandleScreen, 5.0, 5.0);

    if (std::abs(rotationDegrees_) > 0.5) {
        painter.restore();
    }
}

SelectTool::Handle SelectTool::hitTestHandle(QPointF pdfPoint) const
{
    if (selectedAnnotIndex_ < 0)
        return Handle::None;

    float tolerance = 8.0f / lastZoom_;

    auto testPoint = [&](QPointF point, Handle handle) -> Handle {
        double dist = std::abs(pdfPoint.x() - point.x()) + std::abs(pdfPoint.y() - point.y());
        return (dist <= tolerance) ? handle : Handle::None;
    };

    // Rotation handle (above top center, ~25/zoom points above)
    float rotOffset = 25.0f / lastZoom_;
    QPointF rotPoint(selectedAnnotRect_.center().x(), selectedAnnotRect_.top() - rotOffset);
    if (auto h = testPoint(rotPoint, Handle::Rotate); h != Handle::None)
        return h;

    // Corner handles
    if (auto h = testPoint(selectedAnnotRect_.topLeft(), Handle::TopLeft); h != Handle::None)
        return h;
    if (auto h = testPoint(selectedAnnotRect_.topRight(), Handle::TopRight); h != Handle::None)
        return h;
    if (auto h = testPoint(selectedAnnotRect_.bottomLeft(), Handle::BottomLeft); h != Handle::None)
        return h;
    if (auto h = testPoint(selectedAnnotRect_.bottomRight(), Handle::BottomRight); h != Handle::None)
        return h;

    // Edge midpoint handles
    QPointF tc(selectedAnnotRect_.center().x(), selectedAnnotRect_.top());
    QPointF rc(selectedAnnotRect_.right(), selectedAnnotRect_.center().y());
    QPointF bc(selectedAnnotRect_.center().x(), selectedAnnotRect_.bottom());
    QPointF lc(selectedAnnotRect_.left(), selectedAnnotRect_.center().y());

    if (auto h = testPoint(tc, Handle::TopCenter); h != Handle::None)
        return h;
    if (auto h = testPoint(rc, Handle::RightCenter); h != Handle::None)
        return h;
    if (auto h = testPoint(bc, Handle::BottomCenter); h != Handle::None)
        return h;
    if (auto h = testPoint(lc, Handle::LeftCenter); h != Handle::None)
        return h;

    return Handle::None;
}

void SelectTool::selectByIndex(int pageIndex, int annotIndex)
{
    // c82a1f37 P4: set the same selection state a real click-select produces
    // (see onMousePress: selectedAnnotRect_ = getAnnotation(...).rect), so
    // hitTestHandle()/handleCentroidsDebug() operate on a live selection
    // without a synthetic canvas click (which SelectTool ignores, P-GAP-75).
    if (!manager_ || !page_ || annotIndex < 0)
        return;
    selectedAnnotIndex_ = annotIndex;
    selectedAnnotIndices_ = {annotIndex};
    currentPageIndex_ = pageIndex;
    auto data = manager_->getAnnotation(page_, annotIndex);
    selectedAnnotRect_ = data.rect;
    // R3-12: surface the selected annotation's rect as the copy-selection
    // region so onCopy() renders just this region (see MainWindow wiring).
    if (regionSelected) {
        regionSelected(pageIndex, selectedAnnotRect_);
    }
}

QString SelectTool::handleCentroidsDebug() const
{
    // c82a1f37 P4: mirror hitTestHandle()'s geometry exactly so a test can
    // drag from the true handle centroid. PDF/fitz coords. Rotate handle
    // sits rotOffset(=25/zoom) points above the top-center, matching the
    // hit-test. Empty when nothing is selected.
    if (selectedAnnotIndex_ < 0)
        return {};
    const QRectF& r = selectedAnnotRect_;
    const float rotOffset = 25.0f / lastZoom_;
    auto seg = [](const char* name, QPointF p) {
        return QString::fromLatin1(name) + QLatin1Char(':')
             + QString::number(p.x()) + QLatin1Char(',')
             + QString::number(p.y()) + QLatin1Char(';');
    };
    QString out;
    out += seg("TopLeft", r.topLeft());
    out += seg("TopRight", r.topRight());
    out += seg("BottomLeft", r.bottomLeft());
    out += seg("BottomRight", r.bottomRight());
    out += seg("TopCenter", QPointF(r.center().x(), r.top()));
    out += seg("RightCenter", QPointF(r.right(), r.center().y()));
    out += seg("BottomCenter", QPointF(r.center().x(), r.bottom()));
    out += seg("LeftCenter", QPointF(r.left(), r.center().y()));
    out += seg("Rotate", QPointF(r.center().x(), r.top() - rotOffset));
    return out;  // trailing ';' is harmless (Python split drops empties)
}

QRectF SelectTool::computeResizedRect(Handle handle, QPointF currentPdf) const
{
    QPointF delta = currentPdf - dragStartPdf_;
    QRectF newRect = dragStartRect_;

    switch (handle) {
    case Handle::TopLeft:
        newRect.setLeft(dragStartRect_.left() + delta.x());
        newRect.setTop(dragStartRect_.top() + delta.y());
        break;
    case Handle::TopRight:
        newRect.setRight(dragStartRect_.right() + delta.x());
        newRect.setTop(dragStartRect_.top() + delta.y());
        break;
    case Handle::BottomLeft:
        newRect.setLeft(dragStartRect_.left() + delta.x());
        newRect.setBottom(dragStartRect_.bottom() + delta.y());
        break;
    case Handle::BottomRight:
        newRect.setRight(dragStartRect_.right() + delta.x());
        newRect.setBottom(dragStartRect_.bottom() + delta.y());
        break;
    case Handle::TopCenter:
        newRect.setTop(dragStartRect_.top() + delta.y());
        break;
    case Handle::BottomCenter:
        newRect.setBottom(dragStartRect_.bottom() + delta.y());
        break;
    case Handle::LeftCenter:
        newRect.setLeft(dragStartRect_.left() + delta.x());
        break;
    case Handle::RightCenter:
        newRect.setRight(dragStartRect_.right() + delta.x());
        break;
    case Handle::Rotate:
    case Handle::None:
        break;
    }

    newRect = newRect.normalized();
    if (newRect.width() < 10.0)
        newRect.setWidth(10.0);
    if (newRect.height() < 10.0)
        newRect.setHeight(10.0);

    return newRect;
}

double SelectTool::computeRotation(QPointF currentPdf) const
{
    QPointF center = selectedAnnotRect_.center();
    double angle = std::atan2(currentPdf.x() - center.x(),
                              -(currentPdf.y() - center.y()));
    return angle * 180.0 / M_PI;
}

void SelectTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !manager_ || !page_)
        return;

    currentPageIndex_ = pageIndex;

    // If an annotation is already selected, check for handle hit first
    if (selectedAnnotIndex_ >= 0) {
        Handle handle = hitTestHandle(pdfPoint);
        if (handle != Handle::None) {
            if (handle == Handle::Rotate) {
                activeHandle_ = Handle::Rotate;
                isDragging_ = true;
                dragStartPdf_ = pdfPoint;
                dragStartRotation_ = rotationDegrees_;
                return;
            }
            activeHandle_ = handle;
            isDragging_ = true;
            dragStartPdf_ = pdfPoint;
            dragStartRect_ = selectedAnnotRect_;
            return;
        }
    }

    activeHandle_ = Handle::None;

    // Hit-test: find annotation at click point
    auto hitIndex = manager_->getAnnotationAt(page_, pdfPoint);

    if (hitIndex.has_value()) {
        // Clear multi-select
        selectedAnnotIndices_.clear();

        selectedAnnotIndex_ = hitIndex.value();
        auto data = manager_->getAnnotation(page_, selectedAnnotIndex_);
        selectedAnnotRect_ = data.rect;
        // Pick up the angle the annotation already carries, rather than
        // resetting to 0. Resetting drew an upright outline over rotated text,
        // and -- worse -- made dragStartRotation_ 0, so the next drag of the
        // rotate handle applied its delta from upright and the box jumped.
        // Before this commit there was no stored angle to read.
        rotationDegrees_ = data.rotationDegrees;

        isDragging_ = true;
        dragStartPdf_ = pdfPoint;
        dragStartRect_ = selectedAnnotRect_;

        if (annotationSelected) {
            annotationSelected(pageIndex, selectedAnnotIndex_);
        }
        // R3-12: single-click select → copy-selection region = the annot rect.
        if (regionSelected) {
            regionSelected(pageIndex, selectedAnnotRect_);
        }
        if ((data.type == AnnotationType::Text || data.type == AnnotationType::FreeText)
            && annotationPreviewRequested) {
            annotationPreviewRequested(pageIndex, selectedAnnotIndex_, data);
        }
    } else {
        // Clicked empty space — start rubber-band selection
        if (selectedAnnotIndex_ >= 0) {
            selectedAnnotIndex_ = -1;
            rotationDegrees_ = 0.0;
            if (selectionCleared) {
                selectionCleared();
            }
        }
        selectedAnnotIndices_.clear();
        isDragging_ = false;

        isRubberBanding_ = true;
        rubberBandStart_ = pdfPoint;
        rubberBandEnd_ = pdfPoint;
    }
}

void SelectTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)pageIndex;

    if (!(event->buttons() & Qt::LeftButton))
        return;

    if (isRubberBanding_) {
        rubberBandEnd_ = pdfPoint;
        return;
    }

    if (!isDragging_ || selectedAnnotIndex_ < 0 || !manager_ || !page_)
        return;

    if (activeHandle_ == Handle::Rotate) {
        double currentAngle = computeRotation(pdfPoint);
        double startAngle = computeRotation(dragStartPdf_);
        rotationDegrees_ = dragStartRotation_ + (currentAngle - startAngle);
        return;
    }

    if (activeHandle_ != Handle::None) {
        selectedAnnotRect_ = computeResizedRect(activeHandle_, pdfPoint);
    } else {
        double dx = pdfPoint.x() - dragStartPdf_.x();
        double dy = pdfPoint.y() - dragStartPdf_.y();
        selectedAnnotRect_ = dragStartRect_.translated(dx, dy);
    }
}

void SelectTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;

    // Rubber-band release: select all annotations within the rectangle
    if (isRubberBanding_) {
        isRubberBanding_ = false;
        rubberBandEnd_ = pdfPoint;

        QRectF bandRect = QRectF(rubberBandStart_, rubberBandEnd_).normalized();
        if (bandRect.width() < 3.0 && bandRect.height() < 3.0) {
            return;
        }

        selectedAnnotIndices_.clear();
        selectedAnnotIndex_ = -1;

        if (manager_ && page_) {
            auto annotations = manager_->getAnnotations(page_);
            for (int i = 0; i < static_cast<int>(annotations.size()); ++i) {
                if (bandRect.intersects(annotations[i].rect)) {
                    selectedAnnotIndices_.push_back(i);
                }
            }
        }

        if (!selectedAnnotIndices_.empty()) {
            // R3-12: copy-selection region = union bounding box of the
            // annotations the marquee captured (PDF user-space).
            QRectF unionRect;
            if (manager_) {
                for (int i : selectedAnnotIndices_) {
                    const QRectF r = manager_->getAnnotation(page_, i).rect;
                    unionRect = unionRect.isNull() ? r : unionRect.united(r);
                }
            }
            if (multipleAnnotationsSelected) {
                multipleAnnotationsSelected(pageIndex, selectedAnnotIndices_);
            }
            if (regionSelected && !unionRect.isNull()) {
                regionSelected(pageIndex, unionRect);
            }
            if (selectedAnnotIndices_.size() == 1) {
                selectedAnnotIndex_ = selectedAnnotIndices_[0];
                auto data = manager_->getAnnotation(page_, selectedAnnotIndex_);
                selectedAnnotRect_ = data.rect;
                // Same as the click-select path: carry the stored angle so the
                // outline matches the text and the next drag of the rotate
                // handle continues from it instead of from upright.
                rotationDegrees_ = data.rotationDegrees;
                selectedAnnotIndices_.clear();
                if (annotationSelected) {
                    annotationSelected(pageIndex, selectedAnnotIndex_);
                }
            }
        } else if (regionSelected) {
            // R3-12: an empty marquee (a real drag over no annotations) is a
            // page-region snapshot — copy that region of the page as an image.
            regionSelected(pageIndex, bandRect);
        }
        return;
    }

    if (!isDragging_ || selectedAnnotIndex_ < 0 || !manager_ || !page_) {
        isDragging_ = false;
        activeHandle_ = Handle::None;
        return;
    }

    if (activeHandle_ == Handle::Rotate) {
        double currentAngle = computeRotation(pdfPoint);
        double startAngle = computeRotation(dragStartPdf_);
        rotationDegrees_ = dragStartRotation_ + (currentAngle - startAngle);

        if (annotationRotated) {
            annotationRotated(pageIndex, selectedAnnotIndex_, rotationDegrees_);
        }

        isDragging_ = false;
        activeHandle_ = Handle::None;
        return;
    }

    if (activeHandle_ != Handle::None) {
        QRectF newRect = computeResizedRect(activeHandle_, pdfPoint);

        if (annotationMoved) {
            annotationMoved(pageIndex, selectedAnnotIndex_, dragStartRect_, newRect);
        }

        selectedAnnotRect_ = newRect;
    } else {
        double dx = pdfPoint.x() - dragStartPdf_.x();
        double dy = pdfPoint.y() - dragStartPdf_.y();

        if (std::abs(dx) > 2.0 || std::abs(dy) > 2.0) {
            QRectF newRect = dragStartRect_.translated(dx, dy);

            if (annotationMoved) {
                annotationMoved(pageIndex, selectedAnnotIndex_, dragStartRect_, newRect);
            }

            selectedAnnotRect_ = newRect;
        }
    }

    isDragging_ = false;
    activeHandle_ = Handle::None;
}

void SelectTool::onKeyPress(QKeyEvent* event)
{
    // GitHub #186. A bare `return` here is the SAME defect the tail of this
    // function exists to fix, and it was left behind when that fix was made.
    //
    // Qt delivers key events already ACCEPTED. PdfCanvas::keyPressEvent asks
    // the tool first and takes its early-out on `event->isAccepted()`, so a
    // tool that returns without touching the flag has silently CONSUMED the
    // key — PageDown, PageUp, Home, End and the arrows all stop working. The
    // `event->ignore()` at the bottom of this function is the contract, and
    // an unhandled key must reach it by every path, including the guard.
    //
    // Whether this is reachable in the live app is a separate question and is
    // NOT being claimed here: setPage() runs on every tool install and
    // PdfCanvas::setDocument() precedes it, so `page_` is normally bound. But
    // the guard is what stands between a transient unbound state and a
    // swallowed navigation key, and "probably never null" is not a reason to
    // keep a guard that fails in the wrong direction. Cost is one branch;
    // pinned by SelectToolKeyFallthrough.* .
    if (!event)
        return;
    if (!manager_ || !page_) {
        event->ignore();
        return;
    }

    // Delete/Backspace: delete selected annotation(s)
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // Multi-selection delete
        if (!selectedAnnotIndices_.empty()) {
            auto indices = selectedAnnotIndices_;
            selectedAnnotIndices_.clear();
            if (selectionCleared)
                selectionCleared();
            // Delete in reverse order to keep indices valid
            std::sort(indices.rbegin(), indices.rend());
            for (int idx : indices) {
                if (annotationDeleted)
                    annotationDeleted(currentPageIndex_, idx);
            }
            event->accept();
            return;
        }
        if (selectedAnnotIndex_ >= 0) {
            int pageIndex = currentPageIndex_;
            int annotIndex = selectedAnnotIndex_;
            selectedAnnotIndex_ = -1;
            rotationDegrees_ = 0.0;
            if (selectionCleared)
                selectionCleared();
            if (annotationDeleted)
                annotationDeleted(pageIndex, annotIndex);
            event->accept();
            return;
        }
    }

    // Ctrl+C: copy to clipboard
    if (event->matches(QKeySequence::Copy) && selectedAnnotIndex_ >= 0) {
        clipboard_ = manager_->getAnnotation(page_, selectedAnnotIndex_);
        event->accept();
        return;
    }

    // Ctrl+V: paste from clipboard
    if (event->matches(QKeySequence::Paste) && clipboard_.has_value()) {
        auto pasteData = clipboard_.value();
        pasteData.rect = pasteData.rect.translated(20.0, 20.0);

        if (annotationPasted) {
            annotationPasted(currentPageIndex_, pasteData);
        }

        event->accept();
        return;
    }

    // Anything this tool does not handle must be IGNORED so PdfCanvas can
    // fall through to its own navigation. Qt delivers key events already
    // ACCEPTED, so simply returning here left isAccepted() true and the
    // canvas took its early-out -- silently swallowing PageDown, PageUp,
    // Home, End and the arrow keys for as long as the Select tool was
    // active. PdfCanvas::keyPressEvent's TextEditTool branch documents this
    // contract ("Key was explicitly ignored ... fall through"); this tool
    // was not honouring it.
    //
    // The defect was invisible until the Select tool actually got installed
    // at startup: before that currentTool_ was null, the canvas never
    // consulted a tool, and navigation worked by accident.
    event->ignore();
}

void SelectTool::onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !manager_ || !page_)
        return;

    auto hitIndex = manager_->getAnnotationAt(page_, pdfPoint);

    if (hitIndex.has_value()) {
        auto data = manager_->getAnnotation(page_, hitIndex.value());

        if ((data.type == AnnotationType::Text || data.type == AnnotationType::FreeText)
            && stickyNoteDoubleClicked) {
            stickyNoteDoubleClicked(pageIndex, hitIndex.value(), data);
        }
    }
}

QMenu* SelectTool::onContextMenu(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    (void)pdfPoint;

    if (selectedAnnotIndex_ < 0 || !manager_ || !page_)
        return nullptr;

    auto* menu = new QMenu();
    menu->setObjectName("annotationContextMenu");

    auto* copyAction = menu->addAction(QCoreApplication::translate("SelectTool", "Copy"));
    copyAction->setObjectName("contextCopyAction");
    QObject::connect(copyAction, &QAction::triggered, [this]() {
        if (selectedAnnotIndex_ >= 0 && page_ && manager_) {
            clipboard_ = manager_->getAnnotation(page_, selectedAnnotIndex_);
        }
    });

    auto* deleteAction = menu->addAction(QCoreApplication::translate("SelectTool", "Delete"));
    deleteAction->setObjectName("contextDeleteAction");
    QObject::connect(deleteAction, &QAction::triggered, [this, pageIndex]() {
        if (selectedAnnotIndex_ >= 0) {
            int annotIndex = selectedAnnotIndex_;
            selectedAnnotIndex_ = -1;
            rotationDegrees_ = 0.0;

            if (selectionCleared) {
                selectionCleared();
            }

            if (annotationDeleted) {
                annotationDeleted(pageIndex, annotIndex);
            }
        }
    });

    menu->addSeparator();

    auto* propertiesAction = menu->addAction(QCoreApplication::translate("SelectTool", "Properties..."));
    propertiesAction->setObjectName("contextPropertiesAction");
    QObject::connect(propertiesAction, &QAction::triggered, [this, pageIndex]() {
        if (selectedAnnotIndex_ >= 0 && annotationSelected) {
            annotationSelected(pageIndex, selectedAnnotIndex_);
        }
    });

    // Reply (UI-CTX-03) — only for comment-style annotations.
    // Text = sticky note, FreeText = on-page text box. Both carry
    // contents and accept /IRT replies; other types (Square, Ink,
    // Highlight, etc.) cannot be replied to in PDF semantics.
    AnnotationData selected = manager_->getAnnotation(page_, selectedAnnotIndex_);
    if (selected.type == AnnotationType::Text
        || selected.type == AnnotationType::FreeText) {
        auto* replyAction = menu->addAction(
            QCoreApplication::translate("SelectTool", "Reply..."));
        replyAction->setObjectName("contextReplyAction");
        QObject::connect(replyAction, &QAction::triggered, [this, pageIndex]() {
            if (selectedAnnotIndex_ >= 0 && replyRequested) {
                replyRequested(pageIndex, selectedAnnotIndex_);
            }
        });
    }

    return menu;
}

} // namespace pdfe::annotations
