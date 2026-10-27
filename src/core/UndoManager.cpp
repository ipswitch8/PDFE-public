#include "UndoManager.h"
#include <algorithm>

namespace pdfe::commands {

UndoManager::UndoManager(QObject* parent)
    : QObject(parent)
    , cleanIndex_(0)
    , isClean_(true)
{
}

void UndoManager::execute(std::unique_ptr<Command> cmd) {
    if (!cmd) {
        return;
    }

    // Try to merge with the top command on the undo stack
    bool merged = false;
    if (!undoStack_.empty() && cmd->mergeId() != 0) {
        Command* topCmd = undoStack_.back().get();
        if (topCmd->mergeId() == cmd->mergeId()) {
            // Execute the new command first
            cmd->execute();

            // Try to merge
            if (topCmd->mergeWith(cmd.get())) {
                merged = true;
                // Command was merged, no need to push to stack
            }
        }
    }

    if (!merged) {
        // Execute the command
        cmd->execute();

        // Clear redo stack when a new command is executed
        redoStack_.clear();

        // Push to undo stack
        undoStack_.push_back(std::move(cmd));

        // Enforce maximum depth
        enforceMaxDepth();

        // Update clean index if necessary
        if (cleanIndex_ >= 0 && cleanIndex_ < static_cast<int>(undoStack_.size()) - 1) {
            // Clean index is before the new command, keep it valid
        } else if (cleanIndex_ == static_cast<int>(undoStack_.size()) - 1) {
            // Clean index was at the end, still valid
        } else {
            // Clean index is no longer valid
            cleanIndex_ = -1;
        }

        emit undoStateChanged();
    }

    updateCleanState();
}

void UndoManager::undo() {
    if (undoStack_.empty()) {
        return;
    }

    // Pop from undo stack
    std::unique_ptr<Command> cmd = std::move(undoStack_.back());
    undoStack_.pop_back();

    // Undo the command
    cmd->undo();

    // Push to redo stack
    redoStack_.push_back(std::move(cmd));

    emit undoStateChanged();
    updateCleanState();
}

void UndoManager::redo() {
    if (redoStack_.empty()) {
        return;
    }

    // Pop from redo stack
    std::unique_ptr<Command> cmd = std::move(redoStack_.back());
    redoStack_.pop_back();

    // Redo the command
    cmd->redo();

    // Push to undo stack
    undoStack_.push_back(std::move(cmd));

    // Enforce maximum depth
    enforceMaxDepth();

    emit undoStateChanged();
    updateCleanState();
}

bool UndoManager::canUndo() const {
    return !undoStack_.empty();
}

bool UndoManager::canRedo() const {
    return !redoStack_.empty();
}

QString UndoManager::undoDescription() const {
    if (undoStack_.empty()) {
        return QString();
    }
    return QString::fromStdString(undoStack_.back()->description());
}

QString UndoManager::redoDescription() const {
    if (redoStack_.empty()) {
        return QString();
    }
    return QString::fromStdString(redoStack_.back()->description());
}

void UndoManager::clear() {
    undoStack_.clear();
    redoStack_.clear();
    cleanIndex_ = 0;
    isClean_ = true;

    emit undoStateChanged();
    emit cleanStateChanged(true);
}

void UndoManager::setMaxDepth(int depth) {
    if (depth <= 0) {
        return;
    }

    maxDepth_ = depth;
    enforceMaxDepth();

    emit undoStateChanged();
    updateCleanState();
}

int UndoManager::undoStackSize() const {
    return static_cast<int>(undoStack_.size());
}

bool UndoManager::isClean() const {
    return isClean_;
}

void UndoManager::setClean() {
    cleanIndex_ = static_cast<int>(undoStack_.size());
    updateCleanState();
}

void UndoManager::forEachCommand(const std::function<void(Command*)>& fn) const {
    for (const auto& cmd : undoStack_) {
        if (cmd) {
            fn(cmd.get());
        }
    }
    for (const auto& cmd : redoStack_) {
        if (cmd) {
            fn(cmd.get());
        }
    }
}

void UndoManager::enforceMaxDepth() {
    while (static_cast<int>(undoStack_.size()) > maxDepth_) {
        // Remove oldest command from the front
        undoStack_.pop_front();

        // Adjust clean index
        if (cleanIndex_ > 0) {
            --cleanIndex_;
        } else if (cleanIndex_ == 0) {
            // Clean position was removed
            cleanIndex_ = -1;
        }
    }
}

void UndoManager::updateCleanState() {
    const bool newCleanState = (cleanIndex_ == static_cast<int>(undoStack_.size()));

    if (newCleanState != isClean_) {
        isClean_ = newCleanState;
        emit cleanStateChanged(isClean_);
    }
}

} // namespace pdfe::commands
