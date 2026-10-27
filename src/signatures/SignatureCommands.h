#pragma once

#include "SignatureData.h"
#include "core/Command.h"
#include <QRectF>
#include <memory>
#include <string>

namespace pdfe::core {
class Page;
}

namespace pdfe::signatures {

class SignatureManager;

/**
 * @brief Command to create a new signature field (undoable).
 *
 * execute() creates the field; undo() removes it; redo() restores it.
 * Note: Signing a field is NOT undoable.
 */
class CreateSignatureFieldCommand : public pdfe::commands::Command {
public:
    CreateSignatureFieldCommand(SignatureManager* manager,
                                 pdfe::core::Page* page,
                                 const std::string& fieldName,
                                 QRectF rect);
    ~CreateSignatureFieldCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    SignatureManager* manager_;
    pdfe::core::Page* page_;
    std::string fieldName_;
    QRectF rect_;
    SignatureData createdData_;
    bool executed_ = false;
};

} // namespace pdfe::signatures
