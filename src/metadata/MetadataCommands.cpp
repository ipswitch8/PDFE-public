#include "metadata/MetadataCommands.h"

namespace pdfe::metadata {

EditMetadataCommand::EditMetadataCommand(MetadataManager* manager, pdfe::core::Document* doc,
                                         const DocumentMetadata& newMetadata)
    : manager_(manager), doc_(doc), newMetadata_(newMetadata) {
}

EditMetadataCommand::~EditMetadataCommand() = default;

void EditMetadataCommand::execute() {
    if (!manager_ || !doc_) {
        return;
    }

    // Save current metadata before making changes
    if (!executed_) {
        oldMetadata_ = manager_->readMetadata(doc_);
        executed_ = true;
    }

    // Write new metadata
    manager_->writeMetadata(doc_, newMetadata_);
}

void EditMetadataCommand::undo() {
    if (!manager_ || !doc_ || !executed_) {
        return;
    }

    // Restore old metadata
    manager_->writeMetadata(doc_, oldMetadata_);
}

void EditMetadataCommand::redo() {
    if (!manager_ || !doc_ || !executed_) {
        return;
    }

    // Reapply new metadata
    manager_->writeMetadata(doc_, newMetadata_);
}

std::string EditMetadataCommand::description() const {
    return "Edit document metadata";
}

} // namespace pdfe::metadata
