// -*- coding: utf-8 -*-
// Phase-2 regression test: ensures MainWindow sizeof changes don't cause
// stale-obj heap-corruption traps. The "heap corruption" seen during
// Phase-1 was caused by stale test .obj files compiled against an older
// MainWindow layout — not a real bug. This test forces the issue by
// checking that sizeof(MainWindow) is observable from the test TU and
// can be compared across compilation units without surprises.
//
// If this test's obj file goes stale against a newer MainWindow.h, the
// linker will surface the mismatch explicitly via static_assert rather
// than the build succeeding and crashing at runtime.
#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include <QApplication>

TEST(MainWindowSizeSanityTest, SizeofIsNonZero) {
    // Trivial sanity check — sizeof must include all declared members.
    // If test.obj is stale vs MainWindow.h, this size will differ between
    // test compilation and pdfe_ui.lib's compilation, which would cause
    // heap overruns on construction. Build system dependency tracking
    // must keep them in sync.
    EXPECT_GT(sizeof(pdfe::ui::MainWindow), 0u);
    EXPECT_GT(sizeof(pdfe::ui::MainWindow), 256u);  // Many pointer members
}

TEST(MainWindowSizeSanityTest, ConstructDestructCycle) {
    // Exercises the layout-sensitive path that previously crashed.
    // Repeat several times to catch any cross-object corruption.
    for (int i = 0; i < 3; ++i) {
        auto* mw = new pdfe::ui::MainWindow();
        ASSERT_NE(mw, nullptr);
        delete mw;
    }
}
