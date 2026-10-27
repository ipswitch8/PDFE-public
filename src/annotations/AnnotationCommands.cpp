#include "AnnotationCommands.h"
#include "AnnotationManager.h"
#include "core/Page.h"
#include <functional>

namespace pdfe::annotations {

// ============================================================================
// CreateAnnotationCommand
// ============================================================================

CreateAnnotationCommand::CreateAnnotationCommand(
    AnnotationManager* manager,
    pdfe::core::Page* page,
    AnnotationType type,
    QRectF rect
)
    : manager_(manager)
    , page_(page)
    , type_(type)
    , rect_(rect)
{
}

void CreateAnnotationCommand::execute()
{
    createdData_ = manager_->createAnnotation(page_, type_, rect_);
    executed_ = true;
}

void CreateAnnotationCommand::undo()
{
    if (!executed_) {
        return;
    }
    // page_ may be nullptr if the save-path rebind (SaveWithRebind.h step 5)
    // could not re-resolve this command's page — e.g. the page was deleted
    // before a full-rewrite save, so its cached index is now out of range and
    // resolves to nothing. A null-rebound command has no live page to act on;
    // undo is a safe no-op rather than a null dereference / crash.
    if (!page_) {
        return;
    }
    manager_->deleteAnnotation(page_, createdData_.annotIndex);
}

void CreateAnnotationCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }
    if (!page_) {
        return;  // rebound to null (page gone) → safe no-op. See undo().
    }
    int newIndex = manager_->restoreAnnotation(page_, createdData_);
    createdData_.annotIndex = newIndex;
}

std::string CreateAnnotationCommand::description() const
{
    return "Create " + annotationTypeName(type_) + " Annotation";
}

const AnnotationData& CreateAnnotationCommand::createdAnnotation() const
{
    return createdData_;
}

int CreateAnnotationCommand::pageIndex() const
{
    return page_ ? page_->index() : -1;
}

void CreateAnnotationCommand::rebindPage(pdfe::core::Page* page)
{
    page_ = page;
}

// ============================================================================
// DeleteAnnotationCommand
// ============================================================================

DeleteAnnotationCommand::DeleteAnnotationCommand(
    AnnotationManager* manager,
    pdfe::core::Page* page,
    int annotIndex
)
    : manager_(manager)
    , page_(page)
    , annotIndex_(annotIndex)
{
}

void DeleteAnnotationCommand::execute()
{
    // Save the annotation data before deleting (for undo)
    savedData_ = manager_->getAnnotation(page_, annotIndex_);
    manager_->deleteAnnotation(page_, annotIndex_);
    executed_ = true;
}

void DeleteAnnotationCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) {
        return;  // rebound to null (page gone) → safe no-op. See
                 // CreateAnnotationCommand::undo() for the rebind rationale.
    }
    // Restore the annotation and update the index
    int newIndex = manager_->restoreAnnotation(page_, savedData_);
    annotIndex_ = newIndex;
    savedData_.annotIndex = newIndex;
}

void DeleteAnnotationCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }
    if (!page_) {
        return;  // rebound to null (page gone) → safe no-op.
    }
    // Delete using the current annotIndex from savedData_
    manager_->deleteAnnotation(page_, savedData_.annotIndex);
}

std::string DeleteAnnotationCommand::description() const
{
    if (!executed_) {
        return "Delete Annotation";
    }
    return "Delete " + annotationTypeName(savedData_.type) + " Annotation";
}

int DeleteAnnotationCommand::pageIndex() const
{
    return page_ ? page_->index() : -1;
}

void DeleteAnnotationCommand::rebindPage(pdfe::core::Page* page)
{
    page_ = page;
}

// ============================================================================
// ModifyAnnotationCommand
// ============================================================================

ModifyAnnotationCommand::ModifyAnnotationCommand(
    AnnotationManager* manager,
    pdfe::core::Page* page,
    int annotIndex,
    Property property,
    AnnotationData oldData,
    AnnotationData newData
)
    : manager_(manager)
    , page_(page)
    , annotIndex_(annotIndex)
    , property_(property)
    , oldData_(std::move(oldData))
    , newData_(std::move(newData))
{
}

void ModifyAnnotationCommand::execute()
{
    applyData(newData_);
    executed_ = true;
}

void ModifyAnnotationCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) {
        return;  // rebound to null (page gone) → safe no-op. See
                 // CreateAnnotationCommand::undo() for the rebind rationale.
    }
    applyData(oldData_);
}

void ModifyAnnotationCommand::redo()
{
    if (!page_) {
        return;  // rebound to null (page gone) → safe no-op.
    }
    applyData(newData_);
}

std::string ModifyAnnotationCommand::description() const
{
    std::string propertyName;
    switch (property_) {
        case Property::Contents:
            propertyName = "Contents";
            break;
        case Property::Author:
            propertyName = "Author";
            break;
        case Property::Color:
            propertyName = "Color";
            break;
        case Property::Opacity:
            propertyName = "Opacity";
            break;
        case Property::Rect:
            propertyName = "Rectangle";
            break;
        case Property::QuadPoints:
            propertyName = "Quad Points";
            break;
        case Property::InkStrokes:
            propertyName = "Ink Strokes";
            break;
        case Property::DefaultAppearance:
            propertyName = "Text Appearance";
            break;
        case Property::Rotation:
            propertyName = "Rotation";
            break;
        default:
            propertyName = "Property";
            break;
    }

    std::string typeName = annotationTypeName(newData_.type);
    return "Modify " + propertyName + " of " + typeName + " Annotation";
}

bool ModifyAnnotationCommand::mergeWith(const pdfe::commands::Command* other)
{
    // Only merge with other ModifyAnnotationCommand instances
    const auto* otherModify = dynamic_cast<const ModifyAnnotationCommand*>(other);
    if (!otherModify) {
        return false;
    }

    // Cluster #5 fix: identify the annotation by (pageIndex, annotIndex,
    // property) instead of raw Page*. The caller (MainWindow::
    // onAnnotationPropertiesChanged) calls Document::page(idx) on every
    // property edit, which returns a NEW std::unique_ptr<Page> wrapper
    // each time, so the raw page_ pointers differ across consecutive
    // ModifyAnnotationCommands even when they target the same logical
    // page. Comparing by pageIndex (stored in AnnotationData) makes the
    // typing-coalescing merge actually fire, so typing N characters in
    // the StickyNotePopup produces 1 merged Modify command instead of N
    // separate commands. Without this fix, the undo stack grows
    // unboundedly on each keystroke, and a fixed-budget Undo (e.g. the
    // 4-press loop in test_annotations_p1.py::_op_delete) cannot return
    // the document to clean state.
    if (otherModify->newData_.pageIndex != newData_.pageIndex ||
        otherModify->annotIndex_ != annotIndex_ ||
        otherModify->property_ != property_) {
        return false;
    }

    // Absorb the new data from the other command
    newData_ = otherModify->newData_;
    return true;
}

int ModifyAnnotationCommand::pageIndex() const
{
    return page_ ? page_->index() : -1;
}

void ModifyAnnotationCommand::rebindPage(pdfe::core::Page* page)
{
    page_ = page;
}

int ModifyAnnotationCommand::mergeId() const
{
    // Only merge Contents edits (for typing coalescing)
    if (property_ != Property::Contents) {
        return 0;
    }

    // Cluster #5 fix: hash on pageIndex (stable across Document::page()
    // calls) rather than the volatile raw Page* pointer. See mergeWith
    // above for the full root-cause analysis.
    size_t h1 = std::hash<int>{}(newData_.pageIndex);
    size_t h2 = std::hash<int>{}(annotIndex_);
    size_t h3 = std::hash<int>{}(static_cast<int>(property_));

    // Combine hashes
    size_t combined = h1 ^ (h2 << 1) ^ (h3 << 2);
    return static_cast<int>(combined & 0x7FFFFFFF); // Keep positive
}

void ModifyAnnotationCommand::applyData(const AnnotationData& data)
{
    switch (property_) {
        case Property::Contents:
            manager_->setContents(page_, annotIndex_, data.contents);
            break;

        case Property::Author:
            manager_->setAuthor(page_, annotIndex_, data.author);
            break;

        case Property::Color:
            manager_->setColor(page_, annotIndex_, data.color);
            break;

        case Property::Opacity:
            manager_->setOpacity(page_, annotIndex_, data.opacity);
            break;

        case Property::Rect:
            manager_->setRect(page_, annotIndex_, data.rect);
            break;

        case Property::QuadPoints:
            manager_->setQuadPoints(page_, annotIndex_, data.quadPoints);
            break;

        case Property::InkStrokes:
            manager_->setInkStrokes(page_, annotIndex_, data.inkStrokes);
            break;

        case Property::DefaultAppearance:
            manager_->setDefaultAppearance(page_, annotIndex_,
                data.daFontName, data.daFontSize, data.daColor);
            break;

        case Property::Rotation:
            // Rect and angle travel together: rotating widens the axis-aligned
            // bounds, and applying one without the other leaves the annotation
            // describing a shape it does not have. setRotation takes both so
            // undo restores the pair atomically.
            manager_->setRotation(page_, annotIndex_,
                                  data.rotationDegrees, data.rect);
            break;

        default:
            break;
    }
}

} // namespace pdfe::annotations
