#pragma once

#include "metadata/MetadataManager.h"
#include "core/Command.h"

namespace pdfe::metadata {

class EditMetadataCommand : public pdfe::commands::Command {
public:
    EditMetadataCommand(MetadataManager* manager, pdfe::core::Document* doc,
                        const DocumentMetadata& newMetadata);
    ~EditMetadataCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    MetadataManager* manager_;
    pdfe::core::Document* doc_;
    DocumentMetadata newMetadata_;
    DocumentMetadata oldMetadata_;
    bool executed_ = false;
};

} // namespace pdfe::metadata
