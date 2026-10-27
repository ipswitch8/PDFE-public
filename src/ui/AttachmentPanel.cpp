#include "AttachmentPanel.h"
#include "TestDialogSeam.h"
#include "attachments/AttachmentManager.h"
#include "attachments/AttachmentData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace pdfe::ui {

AttachmentPanel::AttachmentPanel(QWidget* parent) : QWidget(parent)
{
    setupUi();
}

AttachmentPanel::~AttachmentPanel() = default;

void AttachmentPanel::setupUi()
{
    setObjectName("attachmentPanel");
    setAccessibleName(tr("Attachments Panel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName("attachmentTree");
    tree_->setAccessibleName(tr("Attachment List"));
    tree_->setHeaderLabels({tr("Name"), tr("Size"), tr("Type")});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(tree_, 1);

    auto* btnRow = new QHBoxLayout();
    addButton_ = new QPushButton(tr("Add..."), this);
    addButton_->setObjectName("attachmentAddButton");
    addButton_->setAccessibleName(tr("Add Attachment"));
    extractButton_ = new QPushButton(tr("Extract..."), this);
    extractButton_->setObjectName("attachmentExtractButton");
    extractButton_->setAccessibleName(tr("Extract Attachment"));
    extractButton_->setEnabled(false);
    deleteButton_ = new QPushButton(tr("Delete"), this);
    deleteButton_->setObjectName("attachmentDeleteButton");
    deleteButton_->setAccessibleName(tr("Delete Attachment"));
    deleteButton_->setEnabled(false);
    btnRow->addWidget(addButton_);
    btnRow->addWidget(extractButton_);
    btnRow->addWidget(deleteButton_);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("attachmentStatusLabel");
    layout->addWidget(statusLabel_);

    truncationLabel_ = new QLabel(this);
    truncationLabel_->setObjectName("attachmentTruncationWarning");
    truncationLabel_->setAccessibleName(tr("Attachment List Incomplete Warning"));
    // Qt::PlainText explicitly: QLabel defaults to Qt::AutoText, which renders
    // anything rich-text-shaped — including <img src=...>, which resolves. This
    // text is ours today, but the phase-69 finding was that a QLabel fed a
    // document-derived string is an injection path, and the cheap way not to
    // re-learn it is to never leave one on AutoText.
    truncationLabel_->setTextFormat(Qt::PlainText);
    truncationLabel_->setWordWrap(true);
    truncationLabel_->setVisible(false);
    layout->addWidget(truncationLabel_);

    connect(addButton_,     &QPushButton::clicked,
            this, &AttachmentPanel::onAddClicked);
    connect(extractButton_, &QPushButton::clicked,
            this, &AttachmentPanel::onExtractClicked);
    connect(deleteButton_,  &QPushButton::clicked,
            this, &AttachmentPanel::onDeleteClicked);
    connect(tree_, &QTreeWidget::itemSelectionChanged,
            this, &AttachmentPanel::onSelectionChanged);
}

void AttachmentPanel::setDocument(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    pdfe::core::Document* document)
{
    context_ = std::move(context);
    document_ = document;
    if (context_) {
        mgr_ = std::make_unique<pdfe::attachments::AttachmentManager>(context_);
    } else {
        mgr_.reset();
    }
    addButton_->setEnabled(document_ != nullptr && mgr_ != nullptr);
    refresh();
}

void AttachmentPanel::clearDocument()
{
    document_ = nullptr;
    mgr_.reset();
    context_.reset();
    addButton_->setEnabled(false);
    tree_->clear();
    onSelectionChanged();
    statusLabel_->clear();
    truncationLabel_->clear();
    truncationLabel_->setVisible(false);
}

void AttachmentPanel::refresh()
{
    tree_->clear();
    if (!document_ || !mgr_) {
        onSelectionChanged();
        return;
    }
    const auto entries = mgr_->list(document_);
    for (const auto& e : entries) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(e.name));
        item->setData(0, Qt::UserRole, QString::fromStdString(e.name));
        if (e.size >= 0) {
            item->setText(1, QLocale().formattedDataSize(e.size));
        } else {
            item->setText(1, QStringLiteral("-"));
        }
        item->setText(2, QString::fromStdString(e.mimeType));
    }

    // The list may be a PREFIX of what the document claims. AttachmentManager
    // stops walking /EmbeddedFiles when the /Kids nesting exceeds its depth cap
    // or references a node already on the path — a shape a document has to be
    // built deliberately to produce. Presenting that as a complete list is
    // exactly what such a document is built to make us do, so say so.
    //
    // Its own label rather than statusLabel_: onAddClicked/onDeleteClicked set
    // a transient status and then call refresh(), so sharing one label would
    // make this either wipe their message or be wiped by it depending on call
    // order. A warning that disappears because something else succeeded is not
    // a warning.
    if (mgr_->lastTraversalTruncated()) {
        truncationLabel_->setText(
            tr("This document's attachment list is incomplete: its "
               "/EmbeddedFiles structure is circular or too deeply nested, so "
               "only the entries shown could be read safely."));
        truncationLabel_->setVisible(true);
    } else {
        truncationLabel_->clear();
        truncationLabel_->setVisible(false);
    }

    onSelectionChanged();
}

QString AttachmentPanel::truncationWarningForTesting() const
{
    // isHidden(), NOT isVisible(). isVisible() is false for every child of a
    // window that has not been shown yet, so it would report "no warning" for
    // a panel that is about to display one — and, worse, would make a control
    // test asserting the ABSENCE of a warning pass without exercising
    // anything. isHidden() reports this widget's own explicit state, which is
    // what the warning logic actually sets.
    if (!truncationLabel_ || truncationLabel_->isHidden()) return {};
    return truncationLabel_->text();
}

QString AttachmentPanel::currentSelectionName() const
{
    auto* item = tree_->currentItem();
    if (!item) return {};
    return item->data(0, Qt::UserRole).toString();
}

std::vector<QString> AttachmentPanel::attachmentNamesForTesting() const
{
    std::vector<QString> names;
    if (!tree_) return names;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item) {
            names.push_back(item->data(0, Qt::UserRole).toString());
        }
    }
    return names;
}

bool AttachmentPanel::selectAttachmentForTesting(const QString& name)
{
    if (!tree_) return false;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item && item->data(0, Qt::UserRole).toString() == name) {
            tree_->setCurrentItem(item);
            onSelectionChanged();
            return true;
        }
    }
    return false;
}

void AttachmentPanel::triggerAddForTesting()
{
    onAddClicked();
}

void AttachmentPanel::triggerExtractForTesting()
{
    onExtractClicked();
}

void AttachmentPanel::onSelectionChanged()
{
    const bool has = tree_->currentItem() != nullptr;
    extractButton_->setEnabled(has && mgr_ && document_);
    deleteButton_->setEnabled(has && mgr_ && document_);
}

void AttachmentPanel::onAddClicked()
{
    if (!document_ || !mgr_) return;
    // SKIP=FAIL: honour a test-injected path before the native open dialog.
    QString path = pdfe::ui::testseam::takeFileDialogPath();
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(this,
            tr("Choose a file to attach"));
    }
    if (path.isEmpty()) return;
    const std::filesystem::path p(path.toStdString());
    const std::string name = p.filename().string();
    if (!mgr_->add(document_, p, name)) {
        QMessageBox::warning(this, tr("Add Failed"),
            tr("Could not embed %1.").arg(path));
        return;
    }
    statusLabel_->setText(tr("Added %1").arg(QString::fromStdString(name)));
    refresh();
    emit attachmentsChanged();
}

void AttachmentPanel::onExtractClicked()
{
    if (!document_ || !mgr_) return;
    const QString name = currentSelectionName();
    if (name.isEmpty()) return;
    // SKIP=FAIL: honour a test-injected directory before the native dialog.
    QString dir = pdfe::ui::testseam::takeFileDialogPath();
    if (dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(this,
            tr("Choose a directory to extract to"));
    }
    if (dir.isEmpty()) return;
    const auto out = mgr_->extract(document_, name.toStdString(),
                                   std::filesystem::path(dir.toStdString()));
    if (out.empty()) {
        QMessageBox::warning(this, tr("Extract Failed"),
            tr("Could not extract %1.").arg(name));
        return;
    }
    statusLabel_->setText(tr("Extracted to %1")
                              .arg(QString::fromStdString(out.string())));
}

void AttachmentPanel::onDeleteClicked()
{
    if (!document_ || !mgr_) return;
    const QString name = currentSelectionName();
    if (name.isEmpty()) return;
    const auto ans = QMessageBox::question(this, tr("Delete Attachment"),
        tr("Remove %1 from the document?").arg(name));
    if (ans != QMessageBox::Yes) return;
    if (!mgr_->remove(document_, name.toStdString())) {
        QMessageBox::warning(this, tr("Delete Failed"),
            tr("Could not remove %1.").arg(name));
        return;
    }
    statusLabel_->setText(tr("Removed %1").arg(name));
    refresh();
    emit attachmentsChanged();
}

} // namespace pdfe::ui
