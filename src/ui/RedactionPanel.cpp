#include "RedactionPanel.h"
#include "redaction/RedactionData.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTreeWidgetItem>

namespace pdfe::ui {

RedactionPanel::RedactionPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("redactionPanel");
    setAccessibleName(tr("Redaction Panel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Mark count label at the top
    markCountLabel_ = new QLabel(tr("No redaction marks"), this);
    markCountLabel_->setObjectName("markCountLabel");
    markCountLabel_->setAccessibleName(tr("Redaction Mark Count"));
    layout->addWidget(markCountLabel_);

    // Tree widget showing pending marks
    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setObjectName("redactionMarksList");
    treeWidget_->setAccessibleName(tr("Redaction Marks List"));
    treeWidget_->setHeaderLabels({
        tr("Page"),
        tr("Region (x, y, w, h)"),
        tr("Color"),
        tr("Overlay Text")
    });
    treeWidget_->setRootIsDecorated(false);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setSelectionMode(QAbstractItemView::SingleSelection);

    // Adjust column widths
    treeWidget_->header()->setStretchLastSection(true);
    treeWidget_->setColumnWidth(0, 60);   // Page column
    treeWidget_->setColumnWidth(1, 160);  // Region column
    treeWidget_->setColumnWidth(2, 80);   // Color column

    connect(treeWidget_, &QTreeWidget::itemClicked,
            this, &RedactionPanel::onItemClicked);

    layout->addWidget(treeWidget_);

    // Button layout at the bottom
    auto* buttonLayout = new QHBoxLayout();

    deleteButton_ = new QPushButton(tr("Delete Selected"), this);
    deleteButton_->setObjectName("deleteRedactionButton");
    deleteButton_->setAccessibleName(tr("Delete Selected Redaction"));
    deleteButton_->setEnabled(false);
    connect(deleteButton_, &QPushButton::clicked,
            this, &RedactionPanel::onDeleteClicked);
    buttonLayout->addWidget(deleteButton_);

    // GitHub #253. Edit a pending mark's fill colour / overlay text. Until this
    // existed there was no modify operation at all, which is why
    // ModifyRedactionCommand was unreachable from main().
    propertiesButton_ = new QPushButton(tr("Properties..."), this);
    propertiesButton_->setObjectName("redactionPropertiesButton");
    propertiesButton_->setAccessibleName(tr("Edit Redaction Mark Properties"));
    propertiesButton_->setEnabled(false);
    connect(propertiesButton_, &QPushButton::clicked,
            this, &RedactionPanel::onPropertiesClicked);
    buttonLayout->addWidget(propertiesButton_);

    buttonLayout->addStretch();

    applyButton_ = new QPushButton(tr("Apply All Redactions"), this);
    applyButton_->setObjectName("applyRedactionsButton");
    applyButton_->setAccessibleName(tr("Apply All Redactions"));
    applyButton_->setEnabled(false);
    connect(applyButton_, &QPushButton::clicked,
            this, &RedactionPanel::onApplyAllClicked);
    buttonLayout->addWidget(applyButton_);

    layout->addLayout(buttonLayout);
}

RedactionPanel::~RedactionPanel() = default;

void RedactionPanel::setMarks(const std::vector<pdfe::redaction::RedactionData>& marks)
{
    treeWidget_->clear();

    for (const auto& mark : marks) {
        // Skip disabled marks
        if (!mark.enabled) {
            continue;
        }

        auto* item = new QTreeWidgetItem(treeWidget_);

        // Column 0: Page number (1-based)
        item->setText(0, QString::number(mark.pageIndex + 1));

        // Column 1: Region (formatted as x, y, w, h)
        item->setText(1, formatRect(mark.rect));

        // Column 2: Color
        item->setText(2, formatColor(mark.fillColor));

        // Column 3: Overlay text (truncated if too long)
        QString overlayText = QString::fromStdString(mark.overlayText);
        if (overlayText.length() > 30) {
            overlayText = overlayText.left(30) + "...";
        }
        item->setText(3, overlayText);

        // Store pageIndex and annotIndex in UserRole data for retrieval
        item->setData(0, Qt::UserRole, mark.pageIndex);
        item->setData(0, Qt::UserRole + 1, mark.annotIndex);

        treeWidget_->addTopLevelItem(item);
    }

    updateMarkCount();
}

void RedactionPanel::clear()
{
    treeWidget_->clear();
    updateMarkCount();
}

int RedactionPanel::topLevelItemCount() const
{
    return treeWidget_ ? treeWidget_->topLevelItemCount() : 0;
}

void RedactionPanel::onItemClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);

    if (!item) {
        deleteButton_->setEnabled(false);
        propertiesButton_->setEnabled(false);  // GitHub #253
        return;
    }

    // Enable the per-mark actions when an item is selected.
    deleteButton_->setEnabled(true);
    propertiesButton_->setEnabled(true);  // GitHub #253

    // Emit signal for navigation
    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int annotIndex = item->data(0, Qt::UserRole + 1).toInt();
    emit markSelected(pageIndex, annotIndex);
}

void RedactionPanel::onApplyAllClicked()
{
    emit applyAllRequested();
}

void RedactionPanel::onDeleteClicked()
{
    QTreeWidgetItem* item = treeWidget_->currentItem();
    if (!item) {
        return;
    }

    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int annotIndex = item->data(0, Qt::UserRole + 1).toInt();

    emit deleteMarkRequested(pageIndex, annotIndex);
}

void RedactionPanel::onPropertiesClicked()
{
    // GitHub #253. Same selection resolution as onDeleteClicked -- the indices
    // live on the tree item, so there is no second copy of "what is selected"
    // to fall out of step with the tree.
    QTreeWidgetItem* item = treeWidget_->currentItem();
    if (!item) {
        return;
    }

    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int annotIndex = item->data(0, Qt::UserRole + 1).toInt();

    emit modifyMarkRequested(pageIndex, annotIndex);
}

void RedactionPanel::updateMarkCount()
{
    int count = treeWidget_->topLevelItemCount();

    if (count == 0) {
        markCountLabel_->setText(tr("No redaction marks"));
        applyButton_->setEnabled(false);
        deleteButton_->setEnabled(false);
        propertiesButton_->setEnabled(false);  // GitHub #253
    } else if (count == 1) {
        markCountLabel_->setText(tr("1 redaction mark pending"));
        applyButton_->setEnabled(true);
    } else {
        markCountLabel_->setText(tr("%1 redaction marks pending").arg(count));
        applyButton_->setEnabled(true);
    }
}

QString RedactionPanel::formatRect(const QRectF& rect) const
{
    // Format as: x, y, w, h with 1 decimal place
    return QString("(%1, %2, %3, %4)")
        .arg(rect.x(), 0, 'f', 1)
        .arg(rect.y(), 0, 'f', 1)
        .arg(rect.width(), 0, 'f', 1)
        .arg(rect.height(), 0, 'f', 1);
}

QString RedactionPanel::formatColor(const QColor& color) const
{
    // Return color name if it's a named color, otherwise RGB hex
    if (color == Qt::black) {
        return tr("Black");
    } else if (color == Qt::white) {
        return tr("White");
    } else if (color == Qt::red) {
        return tr("Red");
    } else if (color == Qt::blue) {
        return tr("Blue");
    } else if (color == Qt::green) {
        return tr("Green");
    } else if (color == Qt::yellow) {
        return tr("Yellow");
    } else {
        // Return as hex string
        return QString("#%1%2%3")
            .arg(color.red(), 2, 16, QChar('0'))
            .arg(color.green(), 2, 16, QChar('0'))
            .arg(color.blue(), 2, 16, QChar('0'))
            .toUpper();
    }
}

} // namespace pdfe::ui
