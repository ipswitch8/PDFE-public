#pragma once
#include "AnnotationData.h"
#include "AnnotationType.h"
#include "core/Command.h"
#include <memory>
#include <QRectF>

namespace pdfe::core { class Page; }

namespace pdfe::annotations {

class AnnotationManager;

/**
 * @brief Command to create a new annotation on a page.
 *
 * Supports undo by deleting the annotation, and redo by restoring it.
 */
class CreateAnnotationCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a create annotation command.
     *
     * @param manager The annotation manager
     * @param page The page to create the annotation on
     * @param type The type of annotation to create
     * @param rect The bounding rectangle for the annotation
     */
    CreateAnnotationCommand(
        AnnotationManager* manager,
        pdfe::core::Page* page,
        AnnotationType type,
        QRectF rect
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

    /**
     * @brief Get the data for the created annotation.
     *
     * Only valid after execute() has been called.
     *
     * @return The annotation data
     */
    [[nodiscard]] const AnnotationData& createdAnnotation() const;

private:
    AnnotationManager* manager_;
    pdfe::core::Page* page_;
    AnnotationType type_;
    QRectF rect_;
    AnnotationData createdData_;
    bool executed_ = false;
};

/**
 * @brief Command to delete an annotation from a page.
 *
 * Supports undo by restoring the annotation with all its properties.
 */
class DeleteAnnotationCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a delete annotation command.
     *
     * @param manager The annotation manager
     * @param page The page containing the annotation
     * @param annotIndex The index of the annotation to delete
     */
    DeleteAnnotationCommand(
        AnnotationManager* manager,
        pdfe::core::Page* page,
        int annotIndex
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    AnnotationManager* manager_;
    pdfe::core::Page* page_;
    int annotIndex_;
    AnnotationData savedData_;
    bool executed_ = false;
};

/**
 * @brief Command to modify properties of an existing annotation.
 *
 * Supports merging consecutive edits to the same property (e.g., typing text).
 */
class ModifyAnnotationCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Properties that can be modified.
     */
    enum class Property {
        Contents,
        Author,
        Color,
        Opacity,
        Rect,
        QuadPoints,
        InkStrokes,
        DefaultAppearance,
        Rotation
    };

    /**
     * @brief Construct a modify annotation command.
     *
     * @param manager The annotation manager
     * @param page The page containing the annotation
     * @param annotIndex The index of the annotation to modify
     * @param property The property to modify
     * @param oldData The annotation data before modification
     * @param newData The annotation data after modification
     */
    ModifyAnnotationCommand(
        AnnotationManager* manager,
        pdfe::core::Page* page,
        int annotIndex,
        Property property,
        AnnotationData oldData,
        AnnotationData newData
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    bool mergeWith(const pdfe::commands::Command* other) override;
    [[nodiscard]] int mergeId() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    void applyData(const AnnotationData& data);

    AnnotationManager* manager_;
    pdfe::core::Page* page_;
    int annotIndex_;
    Property property_;
    AnnotationData oldData_;
    AnnotationData newData_;
    bool executed_ = false;
};

} // namespace pdfe::annotations
