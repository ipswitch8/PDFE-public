// -*- coding: utf-8 -*-
//
// Pipeline e4c71f3a phase-3: STRESS-1 (UndoStack memory at 1000 ops) +
// STRESS-2 (MuPDF save time at 500 form fields).
//
// Investigation tests that measure and bound memory/time. These serve as
// regression guards for the investigated limits.

#include <gtest/gtest.h>

#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormManager.h"

#include <QRectF>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace pdfe::core;
using namespace pdfe::commands;
using namespace pdfe::annotations;
using namespace pdfe::forms;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir(const char* sub)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_stress" / sub;
    std::filesystem::create_directories(d);
    return d;
}

#ifdef _WIN32
size_t getWorkingSetMB()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
    return 0;
}
#else
size_t getWorkingSetMB() { return 0; }
#endif

class StubCommand : public Command {
public:
    explicit StubCommand(int id) : id_(id) {}
    void execute() override {}
    void undo() override {}
    std::string description() const override { return "Stub " + std::to_string(id_); }
private:
    int id_;
    char padding_[256]{};
};

} // namespace

// =====================================================================
// STRESS-1: UndoStack memory growth at 1000 operations
// =====================================================================

class UndoStackStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "test_single.pdf fixture missing";
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
};

TEST_F(UndoStackStressTest, DefaultDepthLimitCapsAt500)
{
    UndoManager mgr;
    EXPECT_EQ(mgr.undoStackSize(), 0);

    for (int i = 0; i < 1000; ++i) {
        mgr.execute(std::make_unique<StubCommand>(i));
    }

    EXPECT_EQ(mgr.undoStackSize(), 500)
        << "STRESS-1: UndoManager must enforce 500-command default limit";
}

TEST_F(UndoStackStressTest, MemoryGrowthPerNInterval)
{
    size_t baselineMB = getWorkingSetMB();
    if (baselineMB == 0)
        GTEST_SKIP() << "Memory measurement not available on this platform";

    UndoManager mgr;
    mgr.setMaxDepth(1000);

    constexpr int checkpoints[] = {100, 250, 500, 750, 1000};
    int ci = 0;

    for (int i = 1; i <= 1000; ++i) {
        mgr.execute(std::make_unique<StubCommand>(i));
        if (ci < 5 && i == checkpoints[ci]) {
            size_t nowMB = getWorkingSetMB();
            size_t growthMB = (nowMB > baselineMB) ? (nowMB - baselineMB) : 0;
            RecordProperty("memory_at_" + std::to_string(i) + "_mb",
                           static_cast<int>(nowMB));
            RecordProperty("growth_at_" + std::to_string(i) + "_mb",
                           static_cast<int>(growthMB));
            EXPECT_LT(growthMB, 200)
                << "STRESS-1: memory growth at N=" << i
                << " must stay < 200 MB (measured " << growthMB << " MB)";
            ++ci;
        }
    }

    // GitHub #273. Two problems here, both of which left this test able to
    // pass having measured nothing.
    //
    // 1. The per-checkpoint EXPECT_LT above was the test's ONLY assertion, and
    //    it sits inside `if (ci < 5 && i == checkpoints[ci])`. That condition
    //    is a loop checkpoint SELECTOR, not a magnitude threshold — it fires
    //    exactly five times by construction — but nothing proved it fired at
    //    all. If the indexing ever drifted, zero checkpoints would be taken
    //    and the test would still report success.
    // 2. The final total growth was RecordProperty'd but never asserted,
    //    even though it is the headline number the test exists to bound.
    //
    // So: prove the measurement happened, then assert the aggregate
    // unconditionally.
    EXPECT_EQ(ci, 5)
        << "only " << ci << " of 5 memory checkpoints were taken, so the "
           "per-checkpoint growth bound above ran " << ci << " times instead "
           "of 5. A test that measures nothing must not report success.";

    size_t finalMB = getWorkingSetMB();
    size_t totalGrowth = (finalMB > baselineMB) ? (finalMB - baselineMB) : 0;
    RecordProperty("baseline_mb", static_cast<int>(baselineMB));
    RecordProperty("final_mb", static_cast<int>(finalMB));
    RecordProperty("total_growth_mb", static_cast<int>(totalGrowth));

    EXPECT_LT(totalGrowth, 200u)
        << "STRESS-1: total working-set growth across 1000 commands must stay "
           "< 200 MB (measured " << totalGrowth << " MB, baseline "
        << baselineMB << " MB, final " << finalMB << " MB)";
}

TEST_F(UndoStackStressTest, AnnotationCommandsAt1000Ops)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    AnnotationManager annotMgr(ctx_);
    UndoManager undoMgr;

    size_t baselineMB = getWorkingSetMB();

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    for (int i = 0; i < 1000; ++i) {
        QRectF rect(50.0 + (i % 50) * 10, 50.0 + (i / 50) * 10, 30, 20);
        auto cmd = std::make_unique<CreateAnnotationCommand>(
            &annotMgr, page.get(), AnnotationType::Text, rect);
        undoMgr.execute(std::move(cmd));
    }

    EXPECT_EQ(undoMgr.undoStackSize(), 500)
        << "STRESS-1: annotation commands must be capped at default limit";

    size_t afterMB = getWorkingSetMB();
    size_t growthMB = (afterMB > baselineMB) ? (afterMB - baselineMB) : 0;

    if (baselineMB > 0) {
        EXPECT_LT(growthMB, 200)
            << "STRESS-1: annotation undo memory growth must stay < 200 MB";
        RecordProperty("annot_growth_mb", static_cast<int>(growthMB));
    }
}

TEST_F(UndoStackStressTest, UndoRedoCycleAt500Ops)
{
    UndoManager mgr;
    for (int i = 0; i < 500; ++i) {
        mgr.execute(std::make_unique<StubCommand>(i));
    }
    EXPECT_EQ(mgr.undoStackSize(), 500);

    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(mgr.canUndo()) << "Undo must be available at step " << i;
        mgr.undo();
    }
    EXPECT_EQ(mgr.undoStackSize(), 0);
    EXPECT_FALSE(mgr.canUndo());

    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(mgr.canRedo()) << "Redo must be available at step " << i;
        mgr.redo();
    }
    EXPECT_EQ(mgr.undoStackSize(), 500);
    EXPECT_FALSE(mgr.canRedo());
}

// =====================================================================
// STRESS-2: MuPDF save time at 500 form fields
// =====================================================================

class FormFieldSaveStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "test_single.pdf fixture missing";
        td_ = tempDir("form_stress");
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path td_;
};

TEST_F(FormFieldSaveStressTest, SaveTimeAt50Fields)
{
    auto outPath = td_ / "stress_50.pdf";
    std::filesystem::remove(outPath);

    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    FormManager formMgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    for (int i = 0; i < 50; ++i) {
        QRectF rect(10.0 + (i % 10) * 55, 10.0 + (i / 10) * 25, 50, 20);
        std::string name = "field_" + std::to_string(i);
        formMgr.createFormField(page.get(), FormFieldType::Text, rect, name);
    }

    auto start = std::chrono::steady_clock::now();
    doc->saveAs(outPath);
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    EXPECT_LT(sec, 30.0)
        << "STRESS-2: save with 50 fields must complete in < 30s (took " << sec << "s)";
    EXPECT_TRUE(std::filesystem::exists(outPath));

    RecordProperty("save_50_fields_sec", std::to_string(sec));
}

TEST_F(FormFieldSaveStressTest, SaveTimeAt100Fields)
{
    auto outPath = td_ / "stress_100.pdf";
    std::filesystem::remove(outPath);

    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    FormManager formMgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    for (int i = 0; i < 100; ++i) {
        QRectF rect(10.0 + (i % 10) * 55, 10.0 + (i / 10) * 25, 50, 20);
        std::string name = "field_" + std::to_string(i);
        formMgr.createFormField(page.get(), FormFieldType::Text, rect, name);
    }

    auto start = std::chrono::steady_clock::now();
    doc->saveAs(outPath);
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    EXPECT_LT(sec, 30.0)
        << "STRESS-2: save with 100 fields must complete in < 30s (took " << sec << "s)";

    RecordProperty("save_100_fields_sec", std::to_string(sec));
}

TEST_F(FormFieldSaveStressTest, SaveTimeAt250Fields)
{
    auto outPath = td_ / "stress_250.pdf";
    std::filesystem::remove(outPath);

    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    FormManager formMgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    for (int i = 0; i < 250; ++i) {
        QRectF rect(10.0 + (i % 10) * 55, 10.0 + (i / 10) * 25, 50, 20);
        std::string name = "field_" + std::to_string(i);
        formMgr.createFormField(page.get(), FormFieldType::Text, rect, name);
    }

    auto start = std::chrono::steady_clock::now();
    doc->saveAs(outPath);
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    EXPECT_LT(sec, 30.0)
        << "STRESS-2: save with 250 fields must complete in < 30s (took " << sec << "s)";

    RecordProperty("save_250_fields_sec", std::to_string(sec));
}

TEST_F(FormFieldSaveStressTest, SaveTimeAt500Fields)
{
    auto outPath = td_ / "stress_500.pdf";
    std::filesystem::remove(outPath);

    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    FormManager formMgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    for (int i = 0; i < 500; ++i) {
        QRectF rect(10.0 + (i % 25) * 23, 10.0 + (i / 25) * 25, 20, 20);
        std::string name = "field_" + std::to_string(i);
        formMgr.createFormField(page.get(), FormFieldType::Text, rect, name);
    }

    auto start = std::chrono::steady_clock::now();
    doc->saveAs(outPath);
    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    EXPECT_LT(sec, 30.0)
        << "STRESS-2: save with 500 fields must complete in < 30s (took " << sec << "s)";

    auto fileSize = std::filesystem::file_size(outPath);
    RecordProperty("save_500_fields_sec", std::to_string(sec));
    RecordProperty("save_500_fields_size_kb", std::to_string(fileSize / 1024));
}
