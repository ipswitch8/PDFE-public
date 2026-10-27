/**
 * @file test_signature_commands.cpp
 * @brief Unit tests for SignatureCommands.
 */

#include <gtest/gtest.h>
#include "signatures/SignatureCommands.h"
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>

using namespace pdfe::signatures;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

class SignatureCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<SignatureManager>(context_);

        std::string path = fixturesDir() + "/test_single.pdf";
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        document_ = pdfe::core::Document::open(context_, path);
        page_ = document_->page(0);
    }

    void TearDown() override {
        page_.reset();
        document_.reset();
        manager_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<SignatureManager> manager_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::core::Page> page_;
};

// Test 1: Execute creates field
TEST_F(SignatureCommandsTest, Execute_CreatesField) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "CmdSig1", QRectF(100, 500, 200, 50));

    EXPECT_EQ(0, manager_->countSignatures(document_.get()));
    cmd.execute();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));
}

// Test 2: Undo removes field
TEST_F(SignatureCommandsTest, Undo_RemovesField) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "CmdSig2", QRectF(100, 500, 200, 50));

    cmd.execute();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    cmd.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));
}

// Test 3: Redo restores field
TEST_F(SignatureCommandsTest, Redo_RestoresField) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "CmdSig3", QRectF(100, 500, 200, 50));

    cmd.execute();
    cmd.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));

    cmd.redo();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));
}

// Test 4: Description
TEST_F(SignatureCommandsTest, Description) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "CmdSig4", QRectF(100, 500, 200, 50));

    EXPECT_EQ("Create Signature Field", cmd.description());
}

// Test 5: Multiple execute/undo/redo cycles
TEST_F(SignatureCommandsTest, MultipleCycles) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "CycleSig", QRectF(100, 500, 200, 50));

    cmd.execute();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    cmd.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));

    cmd.redo();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    cmd.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));

    cmd.redo();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));
}

// Test 6: Two commands in sequence
TEST_F(SignatureCommandsTest, TwoCommands) {
    CreateSignatureFieldCommand cmd1(
        manager_.get(), page_.get(), "Seq1", QRectF(100, 500, 200, 50));
    CreateSignatureFieldCommand cmd2(
        manager_.get(), page_.get(), "Seq2", QRectF(100, 400, 200, 50));

    cmd1.execute();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    cmd2.execute();
    EXPECT_EQ(2, manager_->countSignatures(document_.get()));

    cmd2.undo();
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    cmd1.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));
}

// Test 7: Undo before execute is harmless
TEST_F(SignatureCommandsTest, Undo_BeforeExecute) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "NeverExec", QRectF(100, 500, 200, 50));

    // Should not crash -- executed_ flag is false
    cmd.undo();
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));
}

// Test 8: Construct with different rect values
TEST_F(SignatureCommandsTest, DifferentRectValues) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "SmallSig", QRectF(10, 10, 50, 20));

    cmd.execute();
    auto sigs = manager_->listSignatures(document_.get());
    ASSERT_EQ(1u, sigs.size());
    EXPECT_EQ("SmallSig", sigs[0].fieldName);
}

// Test 9: MergeId is zero (no merging)
TEST_F(SignatureCommandsTest, MergeId_Zero) {
    CreateSignatureFieldCommand cmd(
        manager_.get(), page_.get(), "NoMerge", QRectF(100, 500, 200, 50));
    EXPECT_EQ(0, cmd.mergeId());
}

// Test 10: Destructor is clean
TEST_F(SignatureCommandsTest, Destructor) {
    auto cmd = std::make_unique<CreateSignatureFieldCommand>(
        manager_.get(), page_.get(), "Destruct", QRectF(100, 500, 200, 50));
    cmd->execute();
    cmd.reset(); // Should not crash
    SUCCEED();
}
