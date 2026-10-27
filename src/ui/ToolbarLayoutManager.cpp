#include "ToolbarLayoutManager.h"

#include <QAction>
#include <QSet>
#include <QSettings>
#include <QToolBar>
#include <QtGlobal>

#include <memory>

namespace pdfe::ui {

namespace {

constexpr const char* kRootGroup       = "pdfe/toolbarLayout";
constexpr const char* kToolbarsKey     = "toolbars";
constexpr const char* kActionsSubgroup = "actions";
// GitHub #309. The action ids that EXISTED when the layout was saved.
constexpr const char* kKnownKey        = "known";

std::unique_ptr<QSettings> openSettings(const QString& filePath) {
    if (filePath.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(filePath, QSettings::IniFormat);
}

} // namespace

ToolbarLayoutManager::ToolbarLayoutManager() = default;

std::vector<ToolbarLayout> ToolbarLayoutManager::defaultLayouts() {
    // Mirrors the MainWindow factory toolbar contents. The IDs match
    // QAction::objectName values set in MainWindow::createActions.
    // Empty string == separator.
    return {
        ToolbarLayout{
            "mainToolBar",
            QStringList{
                "openAction",
                "saveAction",
                "",
                "undoAction",
                "redoAction",
                "",
                "zoomInAction",
                "zoomOutAction",
                "fitWidthAction",
                "fitPageAction",
            }
        },
        ToolbarLayout{
            "annotationToolBar",
            QStringList{
                "selectToolAction",
                "stickyNoteToolAction",
                "highlightToolAction",
                "underlineToolAction",
                "strikeOutToolAction",
                "",
                "freeTextToolAction",
                "rectangleToolAction",
                "circleToolAction",
                "lineToolAction",
                "inkToolAction",
                // GitHub #253. THIS LIST, not MainWindow, decides what is on the
                // toolbar: applyLayoutToToolbar() rebuilds each toolbar from it,
                // so an action MainWindow adds but this list omits is silently
                // dropped. Measured while wiring LinkTool -- the live UIA tree
                // held exactly the sixteen entries named here (fourteen buttons
                // and two separators) in THIS order rather than in MainWindow's
                // creation order, which is what identified this list as the
                // authority.
                //
                // CORRECTION (measured after the above was written). An earlier
                // version of this comment said `stampToolAction` and the
                // file-attachment action were "still invisible to users". They
                // are NOT: createAnnotationToolBar() adds both, so they ship on
                // the toolbar -- `build/gtest_p97p7/probe_i253_overflow_popup.py`
                // finds `stampToolActionButton` and
                // `fileAttachmentToolActionButton` in the live UIA tree once the
                // overflow chevron is expanded. The false claim came from
                // reasoning about THIS list as though it were the startup path;
                // it is not, because loadSavedToolbarLayout() returns early when
                // no layout is saved, leaving MainWindow's own order in place.
                //
                // What IS true: this list omits `stampToolAction`,
                // `fileAttachmentToolAction` and `annotColorPicker`, so it does
                // not mirror the factory toolbar as the comment at the top of
                // this function claims. Anything that APPLIES this list -- a
                // saved layout, or Customize/Reset -- therefore drops those
                // three. That is the real, narrower defect. It is GitHub #309
                // (second section) rather than fixed here, because adding them
                // changes what ships on a reset toolbar, which is a product
                // call -- and because #309's primary half, a saved layout
                // freezing the toolbar against later releases, needs deciding
                // first: the schema cannot currently tell "never seen" from
                // "the user removed this on purpose".
                "linkToolAction",
                "",
                "imageToolAction",
                "textEditToolAction",
                "redactToolAction",
                "formToolAction",
            }
        },
    };
}

void ToolbarLayoutManager::setSettingsFilePath(const QString& iniPath) {
    settingsFilePath_ = iniPath;
}

bool ToolbarLayoutManager::hasSavedLayout() const {
    auto s = openSettings(settingsFilePath_);
    s->beginGroup(kRootGroup);
    const bool present = s->contains(kToolbarsKey);
    s->endGroup();
    return present;
}

void ToolbarLayoutManager::saveLayouts(
    const std::vector<ToolbarLayout>& layouts) const
{
    auto s = openSettings(settingsFilePath_);
    s->beginGroup(kRootGroup);
    s->remove(""); // clear any prior layout under the group
    QStringList toolbarNames;
    for (const auto& l : layouts) {
        toolbarNames.push_back(l.toolbarName);
        s->beginGroup(QString("%1/%2").arg(kActionsSubgroup, l.toolbarName));
        s->setValue("ids", l.actionIds);
        // GitHub #309. Recorded even when empty, so a later load can tell a
        // schema-2 layout with nothing known (impossible in practice) from a
        // schema-1 layout that predates the field.
        s->setValue(kKnownKey, l.knownActionIds);
        s->endGroup();
    }
    s->setValue(kToolbarsKey, toolbarNames);
    s->endGroup();
    s->sync();
}

int ToolbarLayoutManager::reconcileWithRegistry(
    std::vector<ToolbarLayout>& layouts,
    const std::vector<QAction*>& registry)
{
    int appended = 0;
    for (auto& layout : layouts) {
        // A set of everything this layout can account for. Separators are the
        // empty string and are not action ids, so they never match.
        QSet<QString> accounted;
        for (const QString& id : layout.actionIds) {
            if (!id.isEmpty()) accounted.insert(id);
        }
        for (const QString& id : layout.knownActionIds) {
            if (!id.isEmpty()) accounted.insert(id);
        }

        for (QAction* a : registry) {
            if (!a) continue;
            const QString id = a->objectName();
            if (id.isEmpty()) continue;          // unnamed: not addressable
            if (accounted.contains(id)) continue; // kept, or deliberately removed
            // Never seen by this layout -> it postdates the save. Append.
            layout.actionIds.push_back(id);
            accounted.insert(id);                // no duplicates if the
                                                 // registry lists it twice
            ++appended;
        }
    }
    return appended;
}

std::vector<ToolbarLayout> ToolbarLayoutManager::loadLayouts() const {
    std::vector<ToolbarLayout> out;
    auto s = openSettings(settingsFilePath_);
    s->beginGroup(kRootGroup);
    if (!s->contains(kToolbarsKey)) {
        s->endGroup();
        return out;
    }
    const QStringList toolbarNames =
        s->value(kToolbarsKey).toStringList();
    for (const QString& name : toolbarNames) {
        s->beginGroup(QString("%1/%2").arg(kActionsSubgroup, name));
        QStringList ids = s->value("ids").toStringList();
        // GitHub #309. Absent for layouts saved before the field existed
        // (schema 1); reconcileWithRegistry documents what that implies.
        QStringList known = s->value(kKnownKey).toStringList();
        s->endGroup();
        out.push_back({name, ids, known});
    }
    s->endGroup();
    return out;
}

void ToolbarLayoutManager::resetToDefaults() const {
    saveLayouts(defaultLayouts());
}

void ToolbarLayoutManager::applyLayoutToToolbar(
    QToolBar* toolbar,
    const ToolbarLayout& layout,
    const std::vector<QAction*>& registry)
{
    if (!toolbar) return;
    toolbar->clear();
    for (const QString& id : layout.actionIds) {
        if (id.isEmpty()) {
            toolbar->addSeparator();
            continue;
        }
        QAction* match = nullptr;
        for (QAction* a : registry) {
            if (a && a->objectName() == id) { match = a; break; }
        }
        if (match) {
            toolbar->addAction(match);
        } else {
            qWarning("ToolbarLayoutManager: unknown action id '%s' for toolbar '%s'",
                     qUtf8Printable(id), qUtf8Printable(layout.toolbarName));
        }
    }
}

ToolbarLayout ToolbarLayoutManager::captureFromToolbar(const QToolBar* toolbar) {
    ToolbarLayout out;
    if (!toolbar) return out;
    out.toolbarName = toolbar->objectName();
    for (QAction* a : toolbar->actions()) {
        if (!a) continue;
        if (a->isSeparator()) {
            out.actionIds.push_back(QString());
        } else {
            out.actionIds.push_back(a->objectName());
        }
    }
    return out;
}

} // namespace pdfe::ui
