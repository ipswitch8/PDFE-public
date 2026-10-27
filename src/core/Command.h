#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pdfe::core { class Page; }

namespace pdfe::commands {

/**
 * @brief Abstract base class for undoable commands.
 *
 * The Command pattern is used for all operations that can be undone/redone.
 * Derive from this class and implement execute(), undo(), and description().
 *
 * Commands can optionally support merging via mergeWith() and mergeId().
 * For example, consecutive character typing commands can be merged into a
 * single undo step.
 */
class Command {
public:
    virtual ~Command() = default;

    /**
     * @brief Execute the command for the first time.
     *
     * This is called when the command is initially executed via UndoManager::execute().
     */
    virtual void execute() = 0;

    /**
     * @brief Undo the effects of this command.
     *
     * After calling undo(), the application state should be as it was before execute().
     */
    virtual void undo() = 0;

    /**
     * @brief Redo the command after it has been undone.
     *
     * Default implementation calls execute(). Override if redo logic differs.
     */
    virtual void redo() { execute(); }

    /**
     * @brief Get a human-readable description of this command.
     *
     * Used for displaying undo/redo menu items (e.g., "Undo Insert Text").
     *
     * @return Description string
     */
    [[nodiscard]] virtual std::string description() const = 0;

    /**
     * @brief Attempt to merge this command with another command.
     *
     * If successful, this command should absorb the effects of `other`.
     * For example, typing "H" then "e" could merge into typing "He".
     *
     * @param other The command to merge with (typically the most recent)
     * @return true if merge was successful, false otherwise
     */
    virtual bool mergeWith([[maybe_unused]] const Command* other) { return false; }

    /**
     * @brief Get the merge identifier for this command.
     *
     * Commands with the same non-zero mergeId can potentially be merged.
     * Return 0 (default) to disable merging for this command.
     *
     * @return Merge identifier (0 = no merging)
     */
    [[nodiscard]] virtual int mergeId() const { return 0; }

    /**
     * @brief Index of the page this command operates on, or -1 if none.
     *
     * Used by the save-path page-rebind orchestration
     * (pdfe::core::saveWithCommandPageRebind) to snapshot which page each
     * command references BEFORE all Page holders are released for a
     * full-rewrite save. Commands that cache a raw Page* must override this to
     * return that page's index while it is still valid. Commands that resolve
     * pages by index transiently can leave the default.
     *
     * @return Zero-based page index, or -1 if the command holds no page.
     */
    [[nodiscard]] virtual int pageIndex() const { return -1; }

    /**
     * @brief Re-point this command at a freshly-resolved Page after a save.
     *
     * After a full-rewrite save drops and re-resolves the document's pages,
     * the orchestration calls this so a command that cached a raw Page* is
     * rebound to the live object and its undo/redo stays valid. Default is a
     * no-op (commands that resolve pages by index transiently need nothing).
     *
     * @param page The freshly-resolved page for this command's pageIndex().
     */
    virtual void rebindPage([[maybe_unused]] pdfe::core::Page* page) {}
};

class CompositeCommand : public Command {
public:
    explicit CompositeCommand(std::string desc) : description_(std::move(desc)) {}

    void addCommand(std::unique_ptr<Command> cmd) {
        commands_.push_back(std::move(cmd));
    }

    void execute() override {
        for (auto& cmd : commands_)
            cmd->execute();
    }

    void undo() override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it)
            (*it)->undo();
    }

    void redo() override {
        for (auto& cmd : commands_)
            cmd->redo();
    }

    [[nodiscard]] std::string description() const override { return description_; }
    [[nodiscard]] bool empty() const { return commands_.empty(); }

private:
    std::vector<std::unique_ptr<Command>> commands_;
    std::string description_;
};

} // namespace pdfe::commands
