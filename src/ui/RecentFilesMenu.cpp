#include "RecentFilesMenu.h"
#include "app/AppSettings.h"
#include <QAction>
#include <QFileInfo>

namespace pdfe::ui {

RecentFilesMenu::RecentFilesMenu(const QString& title, pdfe::app::AppSettings* settings, QWidget* parent)
    : QMenu(title, parent)
    , settings_(settings)
{
    setObjectName("recentFilesMenu");
    setAccessibleName(tr("Recent Files Menu"));

    // Connect the menu's triggered signal to our handler
    connect(this, &QMenu::triggered, this, &RecentFilesMenu::onActionTriggered);

    rebuild();
}

RecentFilesMenu::~RecentFilesMenu() = default;

void RecentFilesMenu::addFile(const QString& filePath) {
    if (settings_) {
        settings_->addRecentFile(filePath);
        rebuild();
    }
}

void RecentFilesMenu::refresh() {
    rebuild();
}

void RecentFilesMenu::rebuild() {
    // Clear all existing actions
    clear();

    if (!settings_) {
        setEnabled(false);
        return;
    }

    QStringList recentFiles = settings_->recentFiles();

    if (recentFiles.isEmpty()) {
        // Show disabled placeholder action
        QAction* noFilesAction = addAction(tr("(No Recent Files)"));
        noFilesAction->setObjectName("recentFilesNoneAction");
        noFilesAction->setEnabled(false);
        setEnabled(false);
    } else {
        setEnabled(true);

        // Add numbered actions for each recent file
        for (int i = 0; i < recentFiles.size(); ++i) {
            const QString& filePath = recentFiles.at(i);
            QFileInfo fileInfo(filePath);

            // Create numbered action text (e.g., "&1. C:\path\file.pdf")
            QString actionText = QString("&%1. %2").arg(i + 1).arg(filePath);

            QAction* fileAction = addAction(actionText);
            fileAction->setObjectName(QString("recentFile_%1").arg(i));
            fileAction->setData(filePath);
        }

        // Add separator and clear action
        addSeparator();

        QAction* clearAction = addAction(tr("Clear Recent Files"));
        clearAction->setObjectName("recentFilesClearAction");
        connect(clearAction, &QAction::triggered, this, &RecentFilesMenu::onClearRecent);
    }
}

void RecentFilesMenu::onActionTriggered(QAction* action) {
    if (!action) {
        return;
    }

    QString filePath = action->data().toString();
    if (!filePath.isEmpty()) {
        emit fileTriggered(filePath);
    }
}

void RecentFilesMenu::onClearRecent() {
    if (settings_) {
        settings_->clearRecentFiles();
        rebuild();
    }
}

} // namespace pdfe::ui
