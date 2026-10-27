// -*- coding: utf-8 -*-
//
// Regression tests for bugfix-3e91a7c4 UI cluster #1/#2/#3 — the toolbars
// shipped as a "series of words" (no icons, no field grouping) and the left
// dock came up showing only Layers instead of Pages. These are REALIZATION
// oracles (not reachability): they assert the concrete visual structure the
// user reported missing, which the prior suite never checked.
//
//   #2  every action on the main + annotation toolbars carries a non-null icon.
//   #1  the Form-fields toolbar groups its field types with separators.
//   #3  Pages is forced to the default/raised left-dock tab even after a stale
//       saved windowState (Layers raised / Pages hidden) is restored.
//
// RED before the Phase-4 fix (no setIcon calls; formToolBar has no separators;
// ensurePagesDockDefault() is a no-op). GREEN after.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QAction>
#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QToolBar>
#include <map>
#include <string>
#include <vector>

using namespace pdfe::ui;

namespace {
// The zoom combo is added to the main toolbar as a QWidgetAction: not a
// separator, but it carries no text and legitimately has no icon. Skip it.
bool isIconBearingAction(QAction* a) {
    return a && !a->isSeparator() && !a->text().isEmpty();
}

// ─────────────────────────────────────────────────────────────────────
// A1-59 REPAIR (meta-audit-2026-07 status register).
//
// `EXPECT_FALSE(a->icon().isNull())` is blind in exactly the way that
// matters here: QIcon(":/does/not/exist.svg") is NOT null — it is a live
// icon backed by a resource that fails to load, and it rasterizes to
// nothing. Likewise, assigning the SAME icon to every action (the shipped
// "wrong icon" regression class this file exists to guard) leaves all 24
// icons non-null. The suite could not see either failure.
//
// The repair rasterizes each icon and asserts, per action, that the
// pixmap actually carries pixels, and across actions, that no two
// DIFFERENT actions rasterize to the same image.
// ─────────────────────────────────────────────────────────────────────

// Nominal raster size for icon comparison. Large enough that two distinct
// line-art glyphs cannot collide, small enough to stay cheap.
constexpr int kIconPx = 32;

// Rasterize an icon to a fixed-size ARGB32 image. Returns a null image when
// the icon cannot produce a pixmap at all.
QImage rasterizeIcon(const QIcon& icon) {
    QPixmap pm = icon.pixmap(QSize(kIconPx, kIconPx));
    if (pm.isNull()) return QImage();
    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    if (img.isNull()) return img;
    // Normalise for any device-pixel-ratio scaling so digests are comparable.
    if (img.width() != kIconPx || img.height() != kIconPx) {
        img = img.scaled(kIconPx, kIconPx, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    return img;
}

// Number of pixels that are actually painted: non-transparent AND not the
// pure-white/pure-transparent background an empty raster would carry.
// A QIcon whose resource failed to load rasterizes to a fully transparent
// square and scores 0.
int visiblePixelCount(const QImage& img) {
    if (img.isNull()) return 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) > 24) ++n;
        }
    }
    return n;
}

// Content digest of a rasterized icon, for cross-action distinctness.
QByteArray iconDigest(const QImage& img) {
    if (img.isNull()) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (int y = 0; y < img.height(); ++y) {
        h.addData(QByteArrayView(
            reinterpret_cast<const char*>(img.constScanLine(y)),
            static_cast<qsizetype>(img.width() * 4)));
    }
    return h.result();
}
}  // namespace

// #2 — toolbar actions must have icons. Pre-fix: 0 of 124 QActions had setIcon,
// so every button rendered as bare text ("a series of words").
TEST(UiStructureBugs, MainAndAnnotationToolbarActionsHaveIcons) {
    MainWindow mw;

    auto* mainTb = mw.findChild<QToolBar*>("mainToolBar");
    ASSERT_NE(mainTb, nullptr) << "main toolbar not found";
    auto* annotTb = mw.annotationToolBarForTesting();
    ASSERT_NE(annotTb, nullptr) << "annotation toolbar not found";

    for (QAction* a : mainTb->actions()) {
        if (!isIconBearingAction(a)) continue;
        EXPECT_FALSE(a->icon().isNull())
            << "main toolbar action has no icon: "
            << a->objectName().toStdString() << " ('" << a->text().toStdString() << "')";
    }
    for (QAction* a : annotTb->actions()) {
        if (!isIconBearingAction(a)) continue;
        EXPECT_FALSE(a->icon().isNull())
            << "annotation toolbar action has no icon: "
            << a->objectName().toStdString() << " ('" << a->text().toStdString() << "')";
    }
}

// A1-59 (a) — every toolbar icon must RASTERIZE to something. `!isNull()`
// says an icon object was assigned; this says the user can see it.
//
// MUTATIONS KILLED:
//   * point actionIcon() at a resource prefix that does not exist (or drop
//     the ::qInitResources_action_icons() call, which is the documented
//     static-lib .qrc failure mode for this project) -> QIcon stays
//     non-null, every pixmap comes back empty, this fails while the
//     `!isNull()` test above stays green.
//   * assign QIcon() to any single action -> that action's pixmap is null.
TEST(UiStructureBugs, ToolbarIconsRasterizeToVisiblePixels) {
    MainWindow mw;

    auto* mainTb = mw.findChild<QToolBar*>("mainToolBar");
    ASSERT_NE(mainTb, nullptr) << "main toolbar not found";
    auto* annotTb = mw.annotationToolBarForTesting();
    ASSERT_NE(annotTb, nullptr) << "annotation toolbar not found";

    int checked = 0;
    for (QToolBar* tb : {mainTb, annotTb}) {
        for (QAction* a : tb->actions()) {
            if (!isIconBearingAction(a)) continue;
            ++checked;
            const QImage img = rasterizeIcon(a->icon());
            const std::string who =
                a->objectName().toStdString() + " ('" +
                a->text().toStdString() + "')";
            ASSERT_FALSE(img.isNull())
                << "icon produced NO pixmap at all for " << who
                << " — the QIcon is non-null but its backing resource does "
                   "not load, so the button renders as bare text.";
            EXPECT_GT(visiblePixelCount(img), 8)
                << "icon rasterized to a blank square for " << who
                << " — nothing is drawn for the user.";
        }
    }
    // Guard against the assertions above becoming vacuous if the toolbars
    // ever come up empty.
    EXPECT_GE(checked, 10)
        << "expected at least ten icon-bearing toolbar actions; found "
        << checked;
}

// A1-59 (b) — distinct actions must carry DISTINCT icons. This is the
// direct guard for the shipped wrong-icon regression: a copy-paste that
// gives Redo the Undo glyph, or a fallback that hands every action the
// same placeholder, leaves all icons non-null and all of them visible —
// only comparing them across actions catches it.
//
// There is no documented intentional-duplicate exception in this project:
// src/ui/MainWindow.cpp maps every action to its own
// `:/icons/actions/<name>.svg`, and every file in
// src/ui/action_icons.qrc is distinct. If a deliberate shared icon is ever
// introduced, add it to kSharedIconAllowlist WITH the reason.
TEST(UiStructureBugs, DistinctToolbarActionsHaveDistinctIcons) {
    // Pairs of objectNames that are ALLOWED to share one icon, each with a
    // documented reason. Empty by design — see the comment above.
    const std::vector<std::pair<const char*, const char*>> kSharedIconAllowlist{};

    MainWindow mw;
    auto* mainTb = mw.findChild<QToolBar*>("mainToolBar");
    ASSERT_NE(mainTb, nullptr);
    auto* annotTb = mw.annotationToolBarForTesting();
    ASSERT_NE(annotTb, nullptr);

    auto isAllowedPair = [&](const std::string& a, const std::string& b) {
        for (const auto& p : kSharedIconAllowlist) {
            if ((a == p.first && b == p.second) || (a == p.second && b == p.first))
                return true;
        }
        return false;
    };

    // digest -> first action objectName that produced it.
    std::map<QByteArray, std::string> seen;
    int compared = 0;
    for (QToolBar* tb : {mainTb, annotTb}) {
        for (QAction* a : tb->actions()) {
            if (!isIconBearingAction(a)) continue;
            const QImage img = rasterizeIcon(a->icon());
            if (img.isNull()) continue;  // reported by the test above
            const QByteArray digest = iconDigest(img);
            ASSERT_FALSE(digest.isEmpty());
            const std::string me = a->objectName().isEmpty()
                                       ? a->text().toStdString()
                                       : a->objectName().toStdString();
            auto it = seen.find(digest);
            if (it == seen.end()) {
                seen.emplace(digest, me);
                ++compared;
                continue;
            }
            if (it->second == me) continue;  // same action on both toolbars
            EXPECT_TRUE(isAllowedPair(it->second, me))
                << "two DIFFERENT toolbar actions rasterize to the same icon: '"
                << it->second << "' and '" << me
                << "'. That is the shipped wrong-icon regression this file "
                   "guards; add a documented entry to kSharedIconAllowlist "
                   "only if the sharing is intentional.";
        }
    }
    EXPECT_GE(compared, 10)
        << "expected at least ten distinct toolbar icons; found " << compared;
}

// #1 — the Form-fields toolbar must group its seven field types with at least
// one separator (pre-fix it was a flat run of text buttons with no grouping).
TEST(UiStructureBugs, FormToolbarGroupsFieldsWithSeparators) {
    MainWindow mw;

    auto* formTb = mw.formToolBarForTesting();
    ASSERT_NE(formTb, nullptr) << "form toolbar not found";

    int separators = 0;
    for (QAction* a : formTb->actions())
        if (a->isSeparator()) ++separators;

    EXPECT_GE(separators, 1)
        << "Form-fields toolbar must visually group its field types with "
           "separators; found none (the '#1 series of words' complaint).";
}

// #3 — Pages is the primary navigation panel and must be the visible/raised
// left-dock tab on every launch, even when a stale saved windowState would
// otherwise leave the left dock showing only Layers. Simulate that stale state
// (Layers shown/raised, Pages hidden) and verify the enforcement corrects it.
TEST(UiStructureBugs, PagesForcedAsDefaultLeftDockTabAfterStaleRestore) {
    MainWindow mw;

    auto* pages = mw.pagePanelDockForTesting();
    auto* layers = mw.layerPanelDockForTesting();
    auto* bookmarks = mw.bookmarkPanelDockForTesting();
    ASSERT_NE(pages, nullptr);
    ASSERT_NE(layers, nullptr);
    ASSERT_NE(bookmarks, nullptr);

    // Reproduce the reported end-state a stale restoreState() leaves behind:
    // Layers is the active/visible left tab and Pages is hidden.
    layers->show();
    layers->raise();
    bookmarks->hide();
    pages->hide();
    ASSERT_TRUE(pages->isHidden()) << "precondition: Pages hidden";

    // The fix: enforce Pages as the default left-dock tab.
    mw.ensurePagesDockDefaultForTesting();

    EXPECT_FALSE(pages->isHidden())
        << "Pages dock must be forced visible as the default left-dock tab";
    EXPECT_TRUE(layers->isHidden())
        << "Layers must yield to Pages as the default (shown on demand only)";
    EXPECT_TRUE(bookmarks->isHidden())
        << "Bookmarks must remain hidden by default";
    EXPECT_EQ(mw.dockWidgetArea(pages), Qt::LeftDockWidgetArea)
        << "Pages must live in the left dock area";
}
