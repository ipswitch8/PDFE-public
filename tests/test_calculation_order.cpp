// -*- coding: utf-8 -*-
// Calculation-order editor (/AcroForm /CO) — FormManager read/write
// pair + CalculationOrderDialog UI.
#include <gtest/gtest.h>
#include "forms/FormManager.h"
#include "ui/CalculationOrderDialog.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>

#include <filesystem>
#include <memory>

namespace {
std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
} // namespace

class CalculationOrderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto path = fixturesDir() / "test_form.pdf";
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "test_form.pdf not available";
        }
        ctx = std::make_shared<pdfe::core::MuPdfContext>();
        doc = pdfe::core::Document::open(ctx, path);
        mgr = std::make_unique<pdfe::forms::FormManager>(ctx);
    }
    std::shared_ptr<pdfe::core::MuPdfContext> ctx;
    std::unique_ptr<pdfe::core::Document> doc;
    std::unique_ptr<pdfe::forms::FormManager> mgr;
};

TEST_F(CalculationOrderTest, ListFieldNamesExposesFormFields)
{
    auto names = mgr->listFieldNames(doc.get());
    // test_form.pdf is known to have multiple fields — exact set varies
    // with the fixture but at least one must be present.
    EXPECT_GE(names.size(), 1u);
    for (const auto& n : names) {
        EXPECT_FALSE(n.empty());
    }
}

TEST_F(CalculationOrderTest, DefaultCalculationOrderIsEmpty)
{
    // test_form.pdf doesn't ship with /CO set; getCalculationOrder
    // should return an empty vector rather than a fabrication.
    auto co = mgr->getCalculationOrder(doc.get());
    EXPECT_TRUE(co.empty());
}

TEST_F(CalculationOrderTest, SetCalculationOrderRoundTrips)
{
    auto names = mgr->listFieldNames(doc.get());
    if (names.size() < 2) GTEST_SKIP() << "need ≥2 fields to reorder";

    std::vector<std::string> want = {names[1], names[0]};
    mgr->setCalculationOrder(doc.get(), want);

    auto got = mgr->getCalculationOrder(doc.get());
    EXPECT_EQ(got, want);
}

TEST_F(CalculationOrderTest, EmptySetClearsCalculationOrder)
{
    auto names = mgr->listFieldNames(doc.get());
    if (names.empty()) GTEST_SKIP();
    mgr->setCalculationOrder(doc.get(), {names[0]});
    ASSERT_EQ(mgr->getCalculationOrder(doc.get()).size(), 1u);
    mgr->setCalculationOrder(doc.get(), {});
    EXPECT_TRUE(mgr->getCalculationOrder(doc.get()).empty());
}

TEST_F(CalculationOrderTest, UnknownFieldNamesAreSilentlyDropped)
{
    auto names = mgr->listFieldNames(doc.get());
    if (names.empty()) GTEST_SKIP();
    std::vector<std::string> mix = {names[0], "does-not-exist"};
    mgr->setCalculationOrder(doc.get(), mix);
    auto got = mgr->getCalculationOrder(doc.get());
    EXPECT_EQ(got.size(), 1u);
    if (!got.empty()) EXPECT_EQ(got[0], names[0]);
}

TEST_F(CalculationOrderTest, NullDocReturnsEmpty)
{
    EXPECT_TRUE(mgr->listFieldNames(nullptr).empty());
    EXPECT_TRUE(mgr->getCalculationOrder(nullptr).empty());
    // setCalculationOrder on null doc is a no-op (must not crash).
    mgr->setCalculationOrder(nullptr, {"anything"});
}

TEST_F(CalculationOrderTest, SaveAsPersistsCalculationOrder)
{
    // Round-trip via disk: setCalculationOrder, saveAs, reopen, verify
    // that the /CO entry is still present and correct. This guards
    // against the failure mode where setCalculationOrder updates the
    // in-memory dict but pdf_save_document drops the /CO array because
    // the field references aren't indirect. (Console-session retry
    // 2026-05-06: matrix row calc_order_dialog_apply_persists.)
    auto names = mgr->listFieldNames(doc.get());
    if (names.size() < 2) GTEST_SKIP() << "need ≥2 fields to reorder";

    std::vector<std::string> want = {names[1], names[0]};
    mgr->setCalculationOrder(doc.get(), want);

    auto tmp = std::filesystem::temp_directory_path() /
        "pdfe_calc_order_roundtrip.pdf";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    doc->saveAs(tmp);
    ASSERT_TRUE(std::filesystem::exists(tmp));

    // Reopen and verify /CO survived the save.
    auto ctx2 = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc2 = pdfe::core::Document::open(ctx2, tmp);
    pdfe::forms::FormManager mgr2(ctx2);
    auto got = mgr2.getCalculationOrder(doc2.get());
    EXPECT_EQ(got, want)
        << "/CO did not survive saveAs roundtrip: setCalculationOrder "
        << "must store indirect references, not resolved field dicts";

    std::filesystem::remove(tmp, ec);
}

TEST(CalculationOrderDialogTest, ExposesNamedControlsForAutomation)
{
    pdfe::ui::CalculationOrderDialog dlg(nullptr, nullptr);
    EXPECT_EQ(dlg.objectName(), QString("calculationOrderDialog"));
    EXPECT_NE(dlg.findChild<QListWidget*>("calculationOrderFieldList"),
              nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("calculationOrderMoveUpButton"),
              nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("calculationOrderMoveDownButton"),
              nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("calculationOrderApplyButton"),
              nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("calculationOrderCancelButton"),
              nullptr);
}

TEST(CalculationOrderDialogTest, FieldListAllowsDragReorder)
{
    pdfe::ui::CalculationOrderDialog dlg(nullptr, nullptr);
    auto* list = dlg.findChild<QListWidget*>("calculationOrderFieldList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->dragDropMode(), QAbstractItemView::InternalMove);
    EXPECT_EQ(list->defaultDropAction(), Qt::MoveAction);
}

TEST(CalculationOrderDialogTest, NewCalculationOrderRespectsCheckState)
{
    pdfe::ui::CalculationOrderDialog dlg(nullptr, nullptr);
    auto* list = dlg.findChild<QListWidget*>("calculationOrderFieldList");
    ASSERT_NE(list, nullptr);

    // Populate manually — null FormManager means populateList() did
    // nothing, so we simulate the state the dialog would build from
    // a real form.
    auto addItem = [list](const QString& name, Qt::CheckState state) {
        auto* item = new QListWidgetItem(name, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(state);
        item->setData(Qt::UserRole, name);
    };
    addItem("FieldA", Qt::Checked);
    addItem("FieldB", Qt::Unchecked);
    addItem("FieldC", Qt::Checked);

    const auto out = dlg.newCalculationOrder();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "FieldA");
    EXPECT_EQ(out[1], "FieldC");
}

TEST(CalculationOrderDialogTest, MoveUpReordersSelectedRow)
{
    pdfe::ui::CalculationOrderDialog dlg(nullptr, nullptr);
    auto* list = dlg.findChild<QListWidget*>("calculationOrderFieldList");
    auto addItem = [list](const QString& name) {
        auto* item = new QListWidgetItem(name, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, name);
    };
    addItem("A");
    addItem("B");
    addItem("C");
    list->setCurrentRow(2);
    auto* btn = dlg.findChild<QPushButton*>("calculationOrderMoveUpButton");
    btn->click();
    // After Up, C is now at row 1; order should be A, C, B.
    const auto out = dlg.newCalculationOrder();
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "A");
    EXPECT_EQ(out[1], "C");
    EXPECT_EQ(out[2], "B");
}
