#pragma once

#include <QWidget>
#include <memory>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::commands {
class UndoManager;
}

namespace pdfe::layers {
class LayerManager;
}

class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;

namespace pdfe::ui {

class LayerPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayerPanel(QWidget* parent = nullptr);
    ~LayerPanel() override;

    void setContext(std::shared_ptr<pdfe::core::MuPdfContext> context);
    void setUndoManager(pdfe::commands::UndoManager* undoManager);
    void setDocument(pdfe::core::Document* doc);
    void clearDocument();
    void refresh();
    /// GitHub #379 test oracle — the Document* this panel is bound to, so a
    /// regression test can assert every panel was rebound after a sign-reopen.
    pdfe::core::Document* documentForTesting() const { return document_; }

signals:
    void layerVisibilityChanged();

private slots:
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onCreateLayer();
    void onDeleteLayer();
    void onRenameLayer();
    void onContextMenu(const QPoint& pos);

private:
    void populateTree();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::commands::UndoManager* undoManager_ = nullptr;
    pdfe::core::Document* document_ = nullptr;
    std::unique_ptr<pdfe::layers::LayerManager> layerManager_;

    QTreeWidget* treeWidget_ = nullptr;
    QPushButton* createButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* renameButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    bool suppressItemChange_ = false;
};

} // namespace pdfe::ui
