#pragma once

#include "Command.h"
#include <QObject>
#include <QString>
#include <deque>
#include <functional>
#include <memory>

namespace pdfe::commands {

/**
 * @brief Manages undo/redo stacks for command-based editing.
 *
 * Features:
 * - Stack-based undo/redo with configurable depth limit (default 500)
 * - Command merging for consecutive similar operations
 * - Clean state tracking (marks when document is saved)
 * - Thread-safe for single-threaded Qt applications
 *
 * Usage:
 * @code
 * auto manager = new UndoManager(this);
 * manager->execute(std::make_unique<MyCommand>());
 * manager->undo();
 * manager->redo();
 * @endcode
 */
class UndoManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Construct an UndoManager with default settings.
     *
     * @param parent Parent QObject
     */
    explicit UndoManager(QObject* parent = nullptr);

    /**
     * @brief Execute a command and add it to the undo stack.
     *
     * The command is executed immediately. If it has the same mergeId as the
     * top command on the undo stack and merging succeeds, they are combined.
     * Otherwise, the command is pushed onto the undo stack.
     *
     * Executing a new command clears the redo stack.
     *
     * @param cmd The command to execute (ownership transferred)
     */
    void execute(std::unique_ptr<Command> cmd);

    /**
     * @brief Undo the most recent command.
     *
     * Does nothing if the undo stack is empty.
     */
    void undo();

    /**
     * @brief Redo the most recently undone command.
     *
     * Does nothing if the redo stack is empty.
     */
    void redo();

    /**
     * @brief Check if undo is available.
     *
     * @return true if there are commands on the undo stack
     */
    [[nodiscard]] bool canUndo() const;

    /**
     * @brief Check if redo is available.
     *
     * @return true if there are commands on the redo stack
     */
    [[nodiscard]] bool canRedo() const;

    /**
     * @brief Get description of the command that would be undone.
     *
     * @return Description string, or empty if canUndo() is false
     */
    [[nodiscard]] QString undoDescription() const;

    /**
     * @brief Get description of the command that would be redone.
     *
     * @return Description string, or empty if canRedo() is false
     */
    [[nodiscard]] QString redoDescription() const;

    /**
     * @brief Clear all undo/redo history.
     *
     * Resets the undo and redo stacks to empty state.
     */
    void clear();

    /**
     * @brief Set the maximum depth of the undo stack.
     *
     * When the undo stack exceeds this depth, the oldest commands are removed.
     *
     * @param depth Maximum number of commands to keep (must be > 0)
     */
    void setMaxDepth(int depth);

    /**
     * @brief Get the current size of the undo stack.
     *
     * Used to synchronize parallel tracking (e.g., image edit history)
     * with the undo/redo position.
     *
     * @return Number of commands currently on the undo stack
     */
    [[nodiscard]] int undoStackSize() const;

    /**
     * @brief Check if the document is in a clean (saved) state.
     *
     * @return true if the current position matches the last setClean() call
     */
    [[nodiscard]] bool isClean() const;

    /**
     * @brief Mark the current position as clean (saved).
     *
     * This is typically called after saving the document.
     */
    void setClean();

    /**
     * @brief Apply @p fn to every command on both the undo and redo stacks.
     *
     * Non-owning access used by the save-path page-rebind orchestration
     * (pdfe::core::saveWithCommandPageRebind) to snapshot each command's
     * pageIndex() before a full-rewrite save and rebindPage() them to
     * freshly-resolved pages afterwards, so undo/redo survive the save. The
     * callback must not mutate the stacks.
     *
     * @param fn Callback invoked with each command (undo stack first, then redo).
     */
    void forEachCommand(const std::function<void(Command*)>& fn) const;

signals:
    /**
     * @brief Emitted when undo/redo availability changes.
     *
     * Connect to this signal to update UI elements (menu items, toolbar buttons).
     */
    void undoStateChanged();

    /**
     * @brief Emitted when the clean state changes.
     *
     * @param clean true if the document is now clean (saved)
     */
    void cleanStateChanged(bool clean);

private:
    /**
     * @brief Enforce maximum stack depth by removing oldest commands.
     */
    void enforceMaxDepth();

    /**
     * @brief Update the clean state and emit signal if changed.
     */
    void updateCleanState();

    std::deque<std::unique_ptr<Command>> undoStack_;
    std::deque<std::unique_ptr<Command>> redoStack_;

    int maxDepth_ = 500;
    int cleanIndex_ = 0;  ///< Index in undo stack where clean state exists (-1 if none)
    bool isClean_ = true;
};

} // namespace pdfe::commands
