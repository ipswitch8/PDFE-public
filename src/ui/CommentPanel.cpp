#include "CommentPanel.h"
#include "../core/Document.h"
#include "../core/Page.h"
#include "../annotations/AnnotationManager.h"
#include "../annotations/AnnotationData.h"
#include "../annotations/AnnotationType.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <map>
#include <QHeaderView>
#include <QMenu>
#include <algorithm>
#include <functional>

namespace pdfe::ui {

CommentPanel::CommentPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("commentPanel");
    setAccessibleName(tr("Comments Panel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Filter and sort controls
    auto* controlLayout = new QHBoxLayout();

    auto* filterLabel = new QLabel(tr("Filter:"), this);
    controlLayout->addWidget(filterLabel);

    filterCombo_ = new QComboBox(this);
    filterCombo_->setObjectName("commentFilterCombo");
    filterCombo_->setAccessibleName(tr("Comment Filter"));
    filterCombo_->addItems({
        tr("All"),
        tr("Sticky Notes"),
        tr("Highlights"),
        tr("Markup"),
        tr("Drawing"),
        tr("Shapes")
    });
    connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CommentPanel::onFilterChanged);
    controlLayout->addWidget(filterCombo_);

    controlLayout->addSpacing(10);

    auto* sortLabel = new QLabel(tr("Sort:"), this);
    controlLayout->addWidget(sortLabel);

    sortCombo_ = new QComboBox(this);
    sortCombo_->setObjectName("commentSortCombo");
    sortCombo_->setAccessibleName(tr("Comment Sort Order"));
    sortCombo_->addItems({
        tr("By Page"),
        tr("By Date"),
        tr("By Author")
    });
    connect(sortCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CommentPanel::onSortChanged);
    controlLayout->addWidget(sortCombo_);

    controlLayout->addStretch();

    layout->addLayout(controlLayout);

    // Tree widget
    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setObjectName("commentTreeWidget");
    treeWidget_->setAccessibleName(tr("Comments Tree"));
    treeWidget_->setHeaderLabels({tr("Annotations")});
    treeWidget_->setRootIsDecorated(true);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(treeWidget_, &QTreeWidget::itemDoubleClicked,
            this, &CommentPanel::onItemDoubleClicked);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &CommentPanel::onContextMenu);

    // Phase-C GAP-58-pattern fix + GAP-61 (new finding): persistent
    // context-menu actions registered with the widget via addAction()
    // so QAccessibleWidget enumerates them into the UIA tree even
    // when the context menu is closed. (Just parenting to `this` is
    // insufficient — Qt's accessibility bridge ignores QActions that
    // are not in the widget's actions() list.)
    commentReplyAction_ = new QAction(tr("Reply"), this);
    commentReplyAction_->setObjectName("commentReplyAction");
    addAction(commentReplyAction_);
    commentDeleteAction_ = new QAction(tr("Delete"), this);
    commentDeleteAction_->setObjectName("commentDeleteAction");
    addAction(commentDeleteAction_);

    // Pipeline f7c2a4e8 phase-2 (P-GAP-90 / A1): wire QAction::triggered
    // so direct trigger() — used by tests + keyboard shortcut paths —
    // emits the same deleteAnnotation/replyToAnnotation signals as the
    // context-menu exec() path. Previously only the menu.exec() return-
    // value path emitted these signals (CommentPanel.cpp:197-200), so a
    // test calling `a->trigger()` on the action got a no-op and the
    // wired undo command was never executed.
    connect(commentDeleteAction_, &QAction::triggered, this, [this]() {
        QTreeWidgetItem* item = treeWidget_->currentItem();
        if (!item) return;
        QVariant annotData = item->data(0, Qt::UserRole + 1);
        if (!annotData.isValid()) return;
        const int pageIndex = item->data(0, Qt::UserRole).toInt();
        const int annotIndex = annotData.toInt();
        emit deleteAnnotation(pageIndex, annotIndex);
    });
    connect(commentReplyAction_, &QAction::triggered, this, [this]() {
        QTreeWidgetItem* item = treeWidget_->currentItem();
        if (!item) return;
        QVariant annotData = item->data(0, Qt::UserRole + 1);
        if (!annotData.isValid()) return;
        const int pageIndex = item->data(0, Qt::UserRole).toInt();
        const int annotIndex = annotData.toInt();
        emit replyToAnnotation(pageIndex, annotIndex);
    });

    layout->addWidget(treeWidget_);
}

CommentPanel::~CommentPanel() = default;

void CommentPanel::setDocument(pdfe::core::Document* doc,
                                pdfe::annotations::AnnotationManager* manager)
{
    document_ = doc;
    annotManager_ = manager;
    refresh();
}

void CommentPanel::clearDocument()
{
    document_ = nullptr;
    annotManager_ = nullptr;
    treeWidget_->clear();
}

void CommentPanel::refresh()
{
    buildAnnotationTree();
}

void CommentPanel::onFilterChanged(int index)
{
    currentFilter_ = index;
    buildAnnotationTree();
}

void CommentPanel::onSortChanged(int index)
{
    currentSort_ = index;
    buildAnnotationTree();
}

void CommentPanel::onItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);

    if (!item) {
        return;
    }

    // Get page index from item data (UserRole) and annotation index (UserRole+1).
    // Page-header items store only UserRole; annotation items store both.
    QVariant pageData = item->data(0, Qt::UserRole);
    if (!pageData.isValid()) {
        return;
    }
    int pageIndex = pageData.toInt();

    QVariant annotData = item->data(0, Qt::UserRole + 1);
    int annotIndex = annotData.isValid() ? annotData.toInt() : -1;

    emit navigateToAnnotation(pageIndex, annotIndex);
}

void CommentPanel::populateCommentContextMenu(QMenu* menu)
{
    if (!menu) return;
    menu->setObjectName("commentContextMenu");
    menu->setAccessibleName(tr("Comment Context Menu"));

    // Phase-C GAP-58-pattern fix: borrow persistent member actions
    // created in the constructor (formerly locals — UIA-invisible
    // outside the menu lifecycle).
    menu->addAction(commentReplyAction_);
    menu->addAction(commentDeleteAction_);
}

std::vector<QString> CommentPanel::rowInventoryForTesting() const
{
    std::vector<QString> rows;
    if (!treeWidget_) {
        return rows;
    }
    // Walk every QTreeWidgetItem (top-level + nested under page headers).
    // Annotation rows carry UserRole+1 (the annotation index); page-header
    // rows carry only UserRole.  Include only annotation rows so the
    // inventory matches what the UI tests count.
    std::function<void(QTreeWidgetItem*)> visit =
        [&](QTreeWidgetItem* item) {
        if (!item) return;
        const QVariant annotData = item->data(0, Qt::UserRole + 1);
        if (annotData.isValid() && !annotData.isNull()) {
            rows.push_back(item->text(0));
        }
        for (int i = 0; i < item->childCount(); ++i) {
            visit(item->child(i));
        }
    };
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        visit(treeWidget_->topLevelItem(i));
    }
    return rows;
}

void CommentPanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = treeWidget_->itemAt(pos);
    if (!item) {
        return;
    }

    // Only show menu for annotation items (not page headers)
    QVariant annotData = item->data(0, Qt::UserRole + 1);
    if (!annotData.isValid()) {
        return;
    }

    QMenu menu(this);
    populateCommentContextMenu(&menu);
    QAction* replyAction = nullptr;
    QAction* deleteAction = nullptr;
    for (QAction* a : menu.actions()) {
        if (!a) continue;
        if (a->objectName() == QLatin1String("commentReplyAction"))
            replyAction = a;
        else if (a->objectName() == QLatin1String("commentDeleteAction"))
            deleteAction = a;
    }

    QAction* selected = menu.exec(treeWidget_->mapToGlobal(pos));
    const int pageIndex = item->data(0, Qt::UserRole).toInt();
    const int annotIndex = annotData.toInt();
    if (selected == deleteAction) {
        emit deleteAnnotation(pageIndex, annotIndex);
    } else if (selected == replyAction) {
        emit replyToAnnotation(pageIndex, annotIndex);
    }
}

void CommentPanel::buildAnnotationTree()
{
    treeWidget_->clear();

    if (!document_ || !annotManager_) {
        return;
    }

    // Collect all annotations from all pages
    struct AnnotationEntry {
        int pageIndex;
        int annotIndex;
        pdfe::annotations::AnnotationData data;
    };

    std::vector<AnnotationEntry> allAnnotations;

    for (int pageIdx = 0; pageIdx < document_->pageCount(); ++pageIdx) {
        auto page = document_->page(pageIdx);
        if (!page) {
            continue;
        }

        auto annotations = annotManager_->getAnnotations(page.get());
        for (size_t i = 0; i < annotations.size(); ++i) {
            const auto& annot = annotations[i];

            // Apply filter
            if (!passesFilter(annot.type)) {
                continue;
            }

            AnnotationEntry entry;
            entry.pageIndex = pageIdx;
            entry.annotIndex = static_cast<int>(i);
            entry.data = annot;
            allAnnotations.push_back(entry);
        }
    }

    // Sort annotations
    if (currentSort_ == 1) {
        // By Date
        std::sort(allAnnotations.begin(), allAnnotations.end(),
            [](const AnnotationEntry& a, const AnnotationEntry& b) {
                return a.data.creationDate > b.data.creationDate;
            });
    } else if (currentSort_ == 2) {
        // By Author
        std::sort(allAnnotations.begin(), allAnnotations.end(),
            [](const AnnotationEntry& a, const AnnotationEntry& b) {
                return a.data.author < b.data.author;
            });
    }
    // else: By Page (already in page order)

    // Build tree
    if (currentSort_ == 0) {
        // By Page: Group by page; replies nest under their parent
        // annotation via /IRT (Phase 8a).
        QTreeWidgetItem* currentPageItem = nullptr;
        int currentPageIdx = -1;
        // (pageIndex, annotIndex) → QTreeWidgetItem so replies can
        // find their parent.
        std::map<std::pair<int,int>, QTreeWidgetItem*> annotItemMap;

        for (const auto& entry : allAnnotations) {
            if (entry.pageIndex != currentPageIdx) {
                currentPageItem = new QTreeWidgetItem(treeWidget_);
                currentPageItem->setText(0, tr("Page %1").arg(entry.pageIndex + 1));
                currentPageItem->setData(0, Qt::UserRole, entry.pageIndex);
                currentPageItem->setExpanded(true);
                currentPageIdx = entry.pageIndex;
            }

            // Parent selection: if this is a reply and we've already
            // placed its parent, nest under it; otherwise attach to the
            // page item.
            QTreeWidgetItem* parentItem = currentPageItem;
            if (entry.data.inReplyToIndex >= 0) {
                auto it = annotItemMap.find({entry.pageIndex,
                                             entry.data.inReplyToIndex});
                if (it != annotItemMap.end() && it->second) {
                    parentItem = it->second;
                }
            }

            if (parentItem) {
                auto* annotItem = new QTreeWidgetItem(parentItem);

                // Icon + type + author + contents preview
                QString icon = getAnnotationIcon(entry.data.type);
                QString typeName = QString::fromStdString(
                    pdfe::annotations::annotationTypeName(entry.data.type));
                QString author = QString::fromStdString(entry.data.author);
                QString contents = QString::fromStdString(entry.data.contents);

                // Truncate contents to 30 chars
                if (contents.length() > 30) {
                    contents = contents.left(30) + "...";
                }

                QString label;
                if (!author.isEmpty() && !contents.isEmpty()) {
                    label = QString("%1 [%2] %3: %4")
                        .arg(icon, typeName, author, contents);
                } else if (!contents.isEmpty()) {
                    label = QString("%1 [%2] %3")
                        .arg(icon, typeName, contents);
                } else {
                    label = QString("%1 [%2]")
                        .arg(icon, typeName);
                }

                annotItem->setText(0, label);
                annotItem->setData(0, Qt::UserRole, entry.pageIndex);
                annotItem->setData(0, Qt::UserRole + 1, entry.annotIndex);
                annotItem->setExpanded(true);   // show any replies nested below
                annotItemMap[{entry.pageIndex, entry.annotIndex}] = annotItem;
            }
        }
    } else {
        // Flat list for date/author sorting
        for (const auto& entry : allAnnotations) {
            auto* annotItem = new QTreeWidgetItem(treeWidget_);

            QString icon = getAnnotationIcon(entry.data.type);
            QString typeName = QString::fromStdString(
                pdfe::annotations::annotationTypeName(entry.data.type));
            QString author = QString::fromStdString(entry.data.author);
            QString contents = QString::fromStdString(entry.data.contents);

            if (contents.length() > 30) {
                contents = contents.left(30) + "...";
            }

            QString label;
            if (!author.isEmpty() && !contents.isEmpty()) {
                label = QString("%1 [%2] (Page %3) %4: %5")
                    .arg(icon, typeName)
                    .arg(entry.pageIndex + 1)
                    .arg(author, contents);
            } else if (!contents.isEmpty()) {
                label = QString("%1 [%2] (Page %3) %4")
                    .arg(icon, typeName)
                    .arg(entry.pageIndex + 1)
                    .arg(contents);
            } else {
                label = QString("%1 [%2] (Page %3)")
                    .arg(icon, typeName)
                    .arg(entry.pageIndex + 1);
            }

            annotItem->setText(0, label);
            annotItem->setData(0, Qt::UserRole, entry.pageIndex);
            annotItem->setData(0, Qt::UserRole + 1, entry.annotIndex);
        }
    }
}

bool CommentPanel::passesFilter(pdfe::annotations::AnnotationType type) const
{
    using pdfe::annotations::AnnotationType;

    switch (currentFilter_) {
    case 0: // All
        return true;
    case 1: // Sticky Notes
        return type == AnnotationType::Text;
    case 2: // Highlights
        return type == AnnotationType::Highlight;
    case 3: // Markup
        return type == AnnotationType::Underline ||
               type == AnnotationType::StrikeOut;
    case 4: // Drawing
        return type == AnnotationType::Ink;
    case 5: // Shapes
        return type == AnnotationType::Square ||
               type == AnnotationType::Circle ||
               type == AnnotationType::Line ||
               type == AnnotationType::FreeText;
    default:
        return true;
    }
}

QString CommentPanel::getAnnotationIcon(pdfe::annotations::AnnotationType type) const
{
    using pdfe::annotations::AnnotationType;

    switch (type) {
    case AnnotationType::Text:
        return QString::fromUtf8("\U0001F4CC");  // 📌
    case AnnotationType::Highlight:
        return QString::fromUtf8("\U0001F58D");  // 🖍
    case AnnotationType::Underline:
        return QString::fromUtf8("\U0000005F");  // _
    case AnnotationType::StrikeOut:
        return QString::fromUtf8("\U00002014");  // —
    case AnnotationType::Ink:
        return QString::fromUtf8("\U0001F58A");  // 🖊
    case AnnotationType::FreeText:
        return QString::fromUtf8("\U0001F4DD");  // 📝
    case AnnotationType::Square:
        return QString::fromUtf8("\U000025A1");  // ▢
    case AnnotationType::Circle:
        return QString::fromUtf8("\U000025CB");  // ○
    case AnnotationType::Line:
        return QString::fromUtf8("\U00002014");  // —
    case AnnotationType::Stamp:
        return QString::fromUtf8("\U00002713");  // ✓
    default:
        return QString::fromUtf8("\U00002022");  // •
    }
}

} // namespace pdfe::ui
