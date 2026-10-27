#include "LayerPanel.h"
#include "layers/LayerManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QHeaderView>

namespace pdfe::ui {

LayerPanel::LayerPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("layerPanel");
    setAccessibleName("Layer Panel");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setObjectName("layerTreeWidget");
    treeWidget_->setAccessibleName("OCG Layer Tree");
    treeWidget_->setHeaderLabels({tr("Layer Name")});
    treeWidget_->setColumnCount(1);
    treeWidget_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);
    mainLayout->addWidget(treeWidget_);

    auto* btnLayout = new QHBoxLayout();
    createButton_ = new QPushButton(tr("Create"), this);
    createButton_->setObjectName("createLayerButton");
    createButton_->setAccessibleName("Create Layer");
    deleteButton_ = new QPushButton(tr("Delete"), this);
    deleteButton_->setObjectName("deleteLayerButton");
    deleteButton_->setAccessibleName("Delete Layer");
    renameButton_ = new QPushButton(tr("Rename"), this);
    renameButton_->setObjectName("renameLayerButton");
    renameButton_->setAccessibleName("Rename Layer");
    btnLayout->addWidget(createButton_);
    btnLayout->addWidget(deleteButton_);
    btnLayout->addWidget(renameButton_);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    statusLabel_ = new QLabel(tr("No document loaded"), this);
    statusLabel_->setObjectName("layerStatusLabel");
    mainLayout->addWidget(statusLabel_);

    connect(treeWidget_, &QTreeWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &LayerPanel::onContextMenu);
    connect(createButton_, &QPushButton::clicked, this, &LayerPanel::onCreateLayer);
    connect(deleteButton_, &QPushButton::clicked, this, &LayerPanel::onDeleteLayer);
    connect(renameButton_, &QPushButton::clicked, this, &LayerPanel::onRenameLayer);
}

LayerPanel::~LayerPanel() = default;

void LayerPanel::setContext(std::shared_ptr<pdfe::core::MuPdfContext> context) {
    context_ = std::move(context);
    if (context_) {
        layerManager_ = std::make_unique<pdfe::layers::LayerManager>(context_);
    }
}

void LayerPanel::setUndoManager(pdfe::commands::UndoManager* undoManager) {
    undoManager_ = undoManager;
}

void LayerPanel::setDocument(pdfe::core::Document* doc) {
    document_ = doc;
    refresh();
}

void LayerPanel::clearDocument() {
    document_ = nullptr;
    treeWidget_->clear();
    statusLabel_->setText(tr("No document loaded"));
}

void LayerPanel::refresh() {
    populateTree();
}

void LayerPanel::populateTree() {
    suppressItemChange_ = true;
    treeWidget_->clear();

    if (!document_ || !layerManager_) {
        statusLabel_->setText(tr("No document loaded"));
        suppressItemChange_ = false;
        return;
    }

    auto layers = layerManager_->listLayers(document_);
    if (layers.empty()) {
        statusLabel_->setText(tr("Document has no layers (OCGs)"));
        suppressItemChange_ = false;
        return;
    }

    for (const auto& layer : layers) {
        auto* item = new QTreeWidgetItem(treeWidget_);
        item->setText(0, QString::fromStdString(layer.name));
        item->setData(0, Qt::UserRole, layer.index);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
    }

    statusLabel_->setText(tr("%1 layer(s)").arg(layers.size()));
    suppressItemChange_ = false;
}

void LayerPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (suppressItemChange_ || !document_ || !layerManager_ || column != 0) return;

    int layerIndex = item->data(0, Qt::UserRole).toInt();
    bool checked = (item->checkState(0) == Qt::Checked);

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::layers::SetLayerVisibilityCommand>(
            layerManager_.get(), document_, layerIndex, checked);
        undoManager_->execute(std::move(cmd));
    } else {
        layerManager_->setVisible(document_, layerIndex, checked);
    }

    emit layerVisibilityChanged();
}

void LayerPanel::onCreateLayer() {
    if (!document_ || !layerManager_) return;

    bool ok;
    QString name = QInputDialog::getText(this, tr("Create Layer"),
        tr("Layer name:"), QLineEdit::Normal, tr("New Layer"), &ok);
    if (!ok || name.isEmpty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::layers::CreateLayerCommand>(
            layerManager_.get(), document_, name.toStdString());
        undoManager_->execute(std::move(cmd));
    } else {
        layerManager_->createLayer(document_, name.toStdString());
    }
    refresh();
}

void LayerPanel::onDeleteLayer() {
    if (!document_ || !layerManager_) return;

    auto* item = treeWidget_->currentItem();
    if (!item) return;

    int layerIndex = item->data(0, Qt::UserRole).toInt();
    QString name = item->text(0);

    auto confirm = QMessageBox::question(this, tr("Delete Layer"),
        tr("Delete layer '%1'?").arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (confirm != QMessageBox::Yes) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::layers::DeleteLayerCommand>(
            layerManager_.get(), document_, layerIndex);
        undoManager_->execute(std::move(cmd));
    } else {
        layerManager_->deleteLayer(document_, layerIndex);
    }
    refresh();
}

void LayerPanel::onRenameLayer() {
    if (!document_ || !layerManager_) return;

    auto* item = treeWidget_->currentItem();
    if (!item) return;

    int layerIndex = item->data(0, Qt::UserRole).toInt();
    QString current = item->text(0);

    bool ok;
    QString newName = QInputDialog::getText(this, tr("Rename Layer"),
        tr("New name:"), QLineEdit::Normal, current, &ok);
    if (!ok || newName.isEmpty() || newName == current) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::layers::RenameLayerCommand>(
            layerManager_.get(), document_, layerIndex, newName.toStdString());
        undoManager_->execute(std::move(cmd));
    } else {
        layerManager_->renameLayer(document_, layerIndex, newName.toStdString());
    }
    refresh();
}

void LayerPanel::onContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.setObjectName("layerContextMenu");

    auto* createAct = menu.addAction(tr("Create Layer..."));
    createAct->setObjectName("createLayerAction");

    auto* item = treeWidget_->itemAt(pos);
    QAction* renameAct = nullptr;
    QAction* deleteAct = nullptr;
    if (item) {
        renameAct = menu.addAction(tr("Rename..."));
        renameAct->setObjectName("renameLayerAction");
        deleteAct = menu.addAction(tr("Delete"));
        deleteAct->setObjectName("deleteLayerAction");
    }

    auto* chosen = menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
    if (chosen == createAct) onCreateLayer();
    else if (renameAct && chosen == renameAct) onRenameLayer();
    else if (deleteAct && chosen == deleteAct) onDeleteLayer();
}

} // namespace pdfe::ui
