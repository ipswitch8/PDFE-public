#include <gtest/gtest.h>
#include "core/UndoManager.h"
#include "core/Command.h"
#include <QtTest/QSignalSpy>

using namespace pdfe::commands;

/**
 * @brief Test command that modifies a counter.
 *
 * Supports command merging for testing merge functionality.
 */
class TestCommand : public Command {
public:
    TestCommand(int& counter, int delta, int merge = 0)
        : counter_(counter), delta_(delta), mergeId_(merge) {}

    void execute() override {
        counter_ += delta_;
    }

    void undo() override {
        counter_ -= delta_;
    }

    std::string description() const override {
        return "test";
    }

    int mergeId() const override {
        return mergeId_;
    }

    bool mergeWith(const Command* other) override {
        if (mergeId_ == 0) {
            return false;
        }

        auto* tc = dynamic_cast<const TestCommand*>(other);
        if (!tc || tc->mergeId_ != mergeId_) {
            return false;
        }

        delta_ += tc->delta_;
        return true;
    }

private:
    int& counter_;
    int delta_;
    int mergeId_;
};

class UndoManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = new UndoManager();
        counter_ = 0;
    }

    void TearDown() override {
        delete manager_;
    }

    UndoManager* manager_;
    int counter_;
};

TEST_F(UndoManagerTest, ExecuteAndUndo) {
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());
    EXPECT_FALSE(manager_->canRedo());

    // Execute a command
    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    EXPECT_EQ(counter_, 5);
    EXPECT_TRUE(manager_->canUndo());
    EXPECT_FALSE(manager_->canRedo());

    // Undo the command
    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());
    EXPECT_TRUE(manager_->canRedo());
}

TEST_F(UndoManagerTest, ExecuteUndoRedo) {
    // Execute command
    manager_->execute(std::make_unique<TestCommand>(counter_, 10));
    EXPECT_EQ(counter_, 10);

    // Undo
    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_TRUE(manager_->canRedo());

    // Redo
    manager_->redo();
    EXPECT_EQ(counter_, 10);
    EXPECT_TRUE(manager_->canUndo());
    EXPECT_FALSE(manager_->canRedo());
}

TEST_F(UndoManagerTest, RedoStackClearedOnNewCommand) {
    // Execute, undo, then execute new command
    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    manager_->execute(std::make_unique<TestCommand>(counter_, 3));
    EXPECT_EQ(counter_, 8);

    manager_->undo();
    EXPECT_EQ(counter_, 5);
    EXPECT_TRUE(manager_->canRedo());

    // Execute new command - should clear redo stack
    manager_->execute(std::make_unique<TestCommand>(counter_, 7));
    EXPECT_EQ(counter_, 12);
    EXPECT_FALSE(manager_->canRedo());

    // Undo twice
    manager_->undo();
    EXPECT_EQ(counter_, 5);
    manager_->undo();
    EXPECT_EQ(counter_, 0);
}

TEST_F(UndoManagerTest, MultipleUndoRedo) {
    // Execute multiple commands
    manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    manager_->execute(std::make_unique<TestCommand>(counter_, 2));
    manager_->execute(std::make_unique<TestCommand>(counter_, 3));
    EXPECT_EQ(counter_, 6);

    // Undo all
    manager_->undo();
    EXPECT_EQ(counter_, 3);
    manager_->undo();
    EXPECT_EQ(counter_, 1);
    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());

    // Redo all
    manager_->redo();
    EXPECT_EQ(counter_, 1);
    manager_->redo();
    EXPECT_EQ(counter_, 3);
    manager_->redo();
    EXPECT_EQ(counter_, 6);
    EXPECT_FALSE(manager_->canRedo());
}

TEST_F(UndoManagerTest, StackDepthLimit) {
    // Set max depth to 500 (default)
    manager_->setMaxDepth(500);

    // Execute 600 commands
    for (int i = 0; i < 600; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }

    EXPECT_EQ(counter_, 600);

    // Undo as many times as possible
    int undoCount = 0;
    while (manager_->canUndo()) {
        manager_->undo();
        ++undoCount;
    }

    // Should only have 500 commands in stack
    EXPECT_EQ(undoCount, 500);
    EXPECT_EQ(counter_, 100);  // 600 - 500
}

TEST_F(UndoManagerTest, CustomStackDepthLimit) {
    // Set custom max depth
    manager_->setMaxDepth(10);

    // Execute 20 commands
    for (int i = 0; i < 20; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }

    EXPECT_EQ(counter_, 20);

    // Undo as many times as possible
    int undoCount = 0;
    while (manager_->canUndo()) {
        manager_->undo();
        ++undoCount;
    }

    // Should only have 10 commands in stack
    EXPECT_EQ(undoCount, 10);
    EXPECT_EQ(counter_, 10);  // 20 - 10
}

TEST_F(UndoManagerTest, CommandMerging) {
    // Execute two commands with the same merge ID
    const int mergeId = 42;

    manager_->execute(std::make_unique<TestCommand>(counter_, 3, mergeId));
    EXPECT_EQ(counter_, 3);

    manager_->execute(std::make_unique<TestCommand>(counter_, 5, mergeId));
    EXPECT_EQ(counter_, 8);

    // Should only need one undo (commands were merged)
    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());
}

TEST_F(UndoManagerTest, CommandMergingDifferentIds) {
    // Execute two commands with different merge IDs
    manager_->execute(std::make_unique<TestCommand>(counter_, 3, 1));
    manager_->execute(std::make_unique<TestCommand>(counter_, 5, 2));
    EXPECT_EQ(counter_, 8);

    // Should need two undos (different merge IDs)
    manager_->undo();
    EXPECT_EQ(counter_, 3);
    EXPECT_TRUE(manager_->canUndo());

    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());
}

TEST_F(UndoManagerTest, CommandMergingZeroId) {
    // Commands with mergeId=0 should not merge
    manager_->execute(std::make_unique<TestCommand>(counter_, 3, 0));
    manager_->execute(std::make_unique<TestCommand>(counter_, 5, 0));
    EXPECT_EQ(counter_, 8);

    // Should need two undos
    manager_->undo();
    EXPECT_EQ(counter_, 3);
    manager_->undo();
    EXPECT_EQ(counter_, 0);
}

TEST_F(UndoManagerTest, CleanStateInitial) {
    // Initially clean
    EXPECT_TRUE(manager_->isClean());
}

TEST_F(UndoManagerTest, CleanStateAfterCommand) {
    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    EXPECT_FALSE(manager_->isClean());
}

TEST_F(UndoManagerTest, CleanStateTracking) {
    QSignalSpy spy(manager_, &UndoManager::cleanStateChanged);

    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(spy.at(0).at(0).toBool());
    EXPECT_FALSE(manager_->isClean());

    // Mark as clean
    manager_->setClean();
    EXPECT_EQ(spy.count(), 2);
    EXPECT_TRUE(spy.at(1).at(0).toBool());
    EXPECT_TRUE(manager_->isClean());

    // Execute another command - should become dirty
    manager_->execute(std::make_unique<TestCommand>(counter_, 3));
    EXPECT_EQ(spy.count(), 3);
    EXPECT_FALSE(spy.at(2).at(0).toBool());
    EXPECT_FALSE(manager_->isClean());

    // Undo to clean position
    manager_->undo();
    EXPECT_EQ(spy.count(), 4);
    EXPECT_TRUE(spy.at(3).at(0).toBool());
    EXPECT_TRUE(manager_->isClean());
}

TEST_F(UndoManagerTest, CleanStateAfterUndoPastClean) {
    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    manager_->setClean();
    EXPECT_TRUE(manager_->isClean());

    manager_->execute(std::make_unique<TestCommand>(counter_, 3));
    EXPECT_FALSE(manager_->isClean());

    // Undo to clean position
    manager_->undo();
    EXPECT_TRUE(manager_->isClean());

    // Undo past clean position
    manager_->undo();
    EXPECT_FALSE(manager_->isClean());
}

TEST_F(UndoManagerTest, Clear) {
    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    manager_->execute(std::make_unique<TestCommand>(counter_, 3));
    manager_->undo();

    EXPECT_TRUE(manager_->canUndo());
    EXPECT_TRUE(manager_->canRedo());

    // Clear all
    manager_->clear();
    EXPECT_FALSE(manager_->canUndo());
    EXPECT_FALSE(manager_->canRedo());
    EXPECT_TRUE(manager_->isClean());
}

TEST_F(UndoManagerTest, UndoDescription) {
    EXPECT_TRUE(manager_->undoDescription().isEmpty());

    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    EXPECT_EQ(manager_->undoDescription(), QString("test"));

    manager_->undo();
    EXPECT_TRUE(manager_->undoDescription().isEmpty());
}

TEST_F(UndoManagerTest, RedoDescription) {
    EXPECT_TRUE(manager_->redoDescription().isEmpty());

    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    manager_->undo();

    EXPECT_EQ(manager_->redoDescription(), QString("test"));

    manager_->redo();
    EXPECT_TRUE(manager_->redoDescription().isEmpty());
}

TEST_F(UndoManagerTest, UndoStateSignal) {
    QSignalSpy spy(manager_, &UndoManager::undoStateChanged);

    manager_->execute(std::make_unique<TestCommand>(counter_, 5));
    EXPECT_EQ(spy.count(), 1);

    manager_->undo();
    EXPECT_EQ(spy.count(), 2);

    manager_->redo();
    EXPECT_EQ(spy.count(), 3);

    manager_->clear();
    EXPECT_EQ(spy.count(), 4);
}

TEST_F(UndoManagerTest, NoUndoWhenEmpty) {
    // Undo on empty stack should do nothing
    manager_->undo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canUndo());
}

TEST_F(UndoManagerTest, NoRedoWhenEmpty) {
    // Redo on empty stack should do nothing
    manager_->redo();
    EXPECT_EQ(counter_, 0);
    EXPECT_FALSE(manager_->canRedo());
}

TEST_F(UndoManagerTest, ExecuteNullCommand) {
    // Execute null command should do nothing
    manager_->execute(nullptr);
    EXPECT_FALSE(manager_->canUndo());
}

TEST_F(UndoManagerTest, MergingWithRedoStack) {
    // Test that merging works correctly and doesn't interfere with redo
    const int mergeId = 100;

    manager_->execute(std::make_unique<TestCommand>(counter_, 2, mergeId));
    manager_->execute(std::make_unique<TestCommand>(counter_, 3, mergeId));
    EXPECT_EQ(counter_, 5);

    // Undo the merged command
    manager_->undo();
    EXPECT_EQ(counter_, 0);

    // Redo should restore both deltas
    manager_->redo();
    EXPECT_EQ(counter_, 5);
}

TEST_F(UndoManagerTest, CleanStateWithDepthLimit) {
    manager_->setMaxDepth(5);

    // Execute commands and mark clean
    manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    manager_->setClean();
    EXPECT_TRUE(manager_->isClean());

    // Execute enough commands to exceed depth and remove clean position
    for (int i = 0; i < 10; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }

    // Clean position should be lost
    while (manager_->canUndo()) {
        manager_->undo();
    }
    EXPECT_FALSE(manager_->isClean());
}

// ── GitHub #296: the SHIPPED undo depth was never tested ─────────────────────
//
// `maxDepth_` defaults to 500 (UndoManager.h:177) and UndoManager.h:16
// documents that default. Every test above that touches the limit calls
// setMaxDepth() first, so all of them exercise a depth the product never ships
// with. The only test that could have reached 500 at scale —
// test_ui_stress.py::test_stress_1000_annotation_undo_redo — now raises the
// limit to N+100 through a test pipe before creating anything, because with the
// default its own oracle ("undo everything, expect 0 annotations") is
// unsatisfiable by arithmetic: the floor is 1000 - 500. Correct fix there, but
// it left the shipped behaviour as the one behaviour not under test.
//
// The literal 500 below is deliberate. #296's criterion is that the test fails
// if the default changes without the expectation being updated, so a
// change-detector is the point here rather than a smell: silently tracking
// maxDepth() would assert that the code equals itself.

namespace {
/// The documented, shipped default. Hard-coded on purpose — see above.
constexpr int kShippedMaxDepth = 500;
}  // namespace

TEST_F(UndoManagerTest, DefaultMaxDepthIsTheDocumentedFiveHundred) {
    // Pin the default itself, before any command is pushed, so a change to it
    // reddens here with an unambiguous message rather than only showing up as a
    // confusing off-by-many in the behavioural tests below.
    manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    EXPECT_EQ(manager_->undoStackSize(), 1);

    for (int i = 1; i < kShippedMaxDepth; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }
    EXPECT_EQ(manager_->undoStackSize(), kShippedMaxDepth)
        << "the default depth no longer holds " << kShippedMaxDepth
        << " commands. If the default was changed deliberately, update "
           "kShippedMaxDepth here AND the \"default 500\" statements in "
           "UndoManager.h:16 and UndoManager.h:177 — GitHub #296 exists because "
           "the shipped default had no test at all.";
}

TEST_F(UndoManagerTest, PushingOneMoreThanTheDefaultDiscardsTheOldest) {
    // The oldest command carries a distinctive delta, so what survives is
    // asserted against a known INPUT rather than against another output.
    constexpr int kOldestDelta = 1000;
    manager_->execute(std::make_unique<TestCommand>(counter_, kOldestDelta));
    for (int i = 0; i < kShippedMaxDepth; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }

    // 501 pushed, 500 retained.
    EXPECT_EQ(manager_->undoStackSize(), kShippedMaxDepth);
    EXPECT_EQ(counter_, kOldestDelta + kShippedMaxDepth);

    int undone = 0;
    while (manager_->canUndo()) {
        manager_->undo();
        ++undone;
    }
    EXPECT_EQ(undone, kShippedMaxDepth)
        << "exactly the retained commands should be undoable";

    // THE user-visible consequence #296 asks for: the discarded command's change
    // is unrecoverable. Undoing everything available cannot take the counter
    // back to 0, because the command that would have subtracted kOldestDelta was
    // dropped off the front of the stack.
    EXPECT_EQ(counter_, kOldestDelta)
        << "the OLDEST command must be the one discarded, and its effect must "
           "survive undoing everything that remains. A counter of 0 would mean "
           "nothing was dropped; a counter of " << (kOldestDelta + 1)
        << " would mean a NEWER command was dropped instead of the oldest.";
}

TEST_F(UndoManagerTest, UndoStackSaturatesRatherThanGrowing) {
    for (int i = 0; i < kShippedMaxDepth * 2; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }
    EXPECT_EQ(manager_->undoStackSize(), kShippedMaxDepth)
        << "undoStackSize() tracked the push count instead of saturating at the "
           "limit";
    // Every command still executed; only the ability to undo them is bounded.
    EXPECT_EQ(counter_, kShippedMaxDepth * 2);
}

TEST_F(UndoManagerTest, SetMaxDepthTrimsAnAlreadyDeeperStack) {
    // UndoManager.cpp:142 — setMaxDepth() calls enforceMaxDepth() rather than
    // only governing future pushes. Nothing covered that until now.
    constexpr int kDeep = 40;
    constexpr int kTrimmed = 10;
    constexpr int kOldestDelta = 1000;

    manager_->execute(std::make_unique<TestCommand>(counter_, kOldestDelta));
    for (int i = 1; i < kDeep; ++i) {
        manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    }
    ASSERT_EQ(manager_->undoStackSize(), kDeep);

    manager_->setMaxDepth(kTrimmed);
    EXPECT_EQ(manager_->undoStackSize(), kTrimmed)
        << "setMaxDepth() left an already-deeper stack untrimmed";

    while (manager_->canUndo()) {
        manager_->undo();
    }
    // kDeep pushes totalled kOldestDelta + (kDeep - 1); only kTrimmed of them
    // remained undoable, so that many 1s came back off.
    EXPECT_EQ(counter_, kOldestDelta + (kDeep - 1) - kTrimmed)
        << "the trim must discard from the OLDEST end";

    // A non-positive depth is ignored (UndoManager.cpp:137), not treated as 0.
    manager_->execute(std::make_unique<TestCommand>(counter_, 1));
    manager_->setMaxDepth(0);
    EXPECT_EQ(manager_->undoStackSize(), 1)
        << "setMaxDepth(0) must be ignored rather than emptying the stack";
    manager_->setMaxDepth(-5);
    EXPECT_EQ(manager_->undoStackSize(), 1)
        << "setMaxDepth(-5) must be ignored rather than emptying the stack";
}
