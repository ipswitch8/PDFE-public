#pragma once
#include <QMenu>
#include <QStringList>

namespace pdfe::app { class AppSettings; }

namespace pdfe::ui {

class RecentFilesMenu : public QMenu {
    Q_OBJECT

public:
    explicit RecentFilesMenu(const QString& title, pdfe::app::AppSettings* settings, QWidget* parent = nullptr);
    ~RecentFilesMenu() override;

    void addFile(const QString& filePath);
    void refresh();

signals:
    void fileTriggered(const QString& filePath);

private slots:
    void onActionTriggered(QAction* action);
    void onClearRecent();

private:
    void rebuild();
    pdfe::app::AppSettings* settings_;
};

} // namespace pdfe::ui
