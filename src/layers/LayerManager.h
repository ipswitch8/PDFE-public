#pragma once

#include "core/Command.h"
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::layers {

struct LayerInfo {
    int index = -1;
    std::string name;
    bool visible = true;
    bool locked = false;
};

class LayerManager {
public:
    explicit LayerManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~LayerManager();

    LayerManager(const LayerManager&) = delete;
    LayerManager& operator=(const LayerManager&) = delete;

    // Read operations
    std::vector<LayerInfo> listLayers(pdfe::core::Document* doc);
    int layerCount(pdfe::core::Document* doc);
    bool isVisible(pdfe::core::Document* doc, int layerIndex);
    std::string layerName(pdfe::core::Document* doc, int layerIndex);

    // Mutation operations
    void setVisible(pdfe::core::Document* doc, int layerIndex, bool visible);
    int createLayer(pdfe::core::Document* doc, const std::string& name);
    void renameLayer(pdfe::core::Document* doc, int layerIndex, const std::string& newName);
    void deleteLayer(pdfe::core::Document* doc, int layerIndex);
    void reorderLayer(pdfe::core::Document* doc, int fromIndex, int toIndex);

    // Check if document has OCProperties
    bool hasLayers(pdfe::core::Document* doc);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Undo commands for layer operations

class SetLayerVisibilityCommand : public pdfe::commands::Command {
public:
    SetLayerVisibilityCommand(LayerManager* mgr, pdfe::core::Document* doc,
                              int layerIndex, bool visible);
    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    LayerManager* mgr_;
    pdfe::core::Document* doc_;
    int layerIndex_;
    bool newVisible_;
    bool oldVisible_;
};

class CreateLayerCommand : public pdfe::commands::Command {
public:
    CreateLayerCommand(LayerManager* mgr, pdfe::core::Document* doc,
                       const std::string& name);
    void execute() override;
    void undo() override;
    std::string description() const override;
    int createdIndex() const { return createdIndex_; }

private:
    LayerManager* mgr_;
    pdfe::core::Document* doc_;
    std::string name_;
    int createdIndex_ = -1;
};

class RenameLayerCommand : public pdfe::commands::Command {
public:
    RenameLayerCommand(LayerManager* mgr, pdfe::core::Document* doc,
                       int layerIndex, const std::string& newName);
    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    LayerManager* mgr_;
    pdfe::core::Document* doc_;
    int layerIndex_;
    std::string newName_;
    std::string oldName_;
};

class DeleteLayerCommand : public pdfe::commands::Command {
public:
    DeleteLayerCommand(LayerManager* mgr, pdfe::core::Document* doc, int layerIndex);
    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    LayerManager* mgr_;
    pdfe::core::Document* doc_;
    int layerIndex_;
    std::string deletedName_;
    bool deletedVisible_ = true;
};

} // namespace pdfe::layers
