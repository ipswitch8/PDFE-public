#pragma once

#include "RedactionData.h"
#include "core/Command.h"
#include <QRectF>
#include <QColor>

namespace pdfe::core { class Page; }

namespace pdfe::redaction {

class RedactionManager;

/// Command to create a new redaction mark on a page.
class CreateRedactionCommand : public pdfe::commands::Command {
public:
    CreateRedactionCommand(
        RedactionManager* manager,
        pdfe::core::Page* page,
        QRectF rect
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

    /// Get the data for the created redaction mark.
    /// Only valid after execute() has been called.
    [[nodiscard]] const RedactionData& createdMark() const;

private:
    RedactionManager* manager_;
    pdfe::core::Page* page_;
    QRectF rect_;
    RedactionData createdData_;
    bool executed_ = false;
};

/// Command to delete a redaction mark from a page.
class DeleteRedactionCommand : public pdfe::commands::Command {
public:
    DeleteRedactionCommand(
        RedactionManager* manager,
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
    RedactionManager* manager_;
    pdfe::core::Page* page_;
    int annotIndex_;
    RedactionData savedData_;
    bool executed_ = false;
};

/// Command to modify properties of a redaction mark.
class ModifyRedactionCommand : public pdfe::commands::Command {
public:
    enum class Property {
        FillColor,
        OverlayText,
        Rect
    };

    ModifyRedactionCommand(
        RedactionManager* manager,
        pdfe::core::Page* page,
        int annotIndex,
        Property property,
        RedactionData oldData,
        RedactionData newData
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
    void applyData(const RedactionData& data);

    RedactionManager* manager_;
    pdfe::core::Page* page_;
    int annotIndex_;
    Property property_;
    RedactionData oldData_;
    RedactionData newData_;
    bool executed_ = false;
};

} // namespace pdfe::redaction
