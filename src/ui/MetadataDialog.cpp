#include "ui/MetadataDialog.h"
#include "metadata/MetadataManager.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

class MetadataDialog::Impl {
public:
    // Editable fields
    QLineEdit* titleEdit = nullptr;
    QLineEdit* authorEdit = nullptr;
    QLineEdit* subjectEdit = nullptr;
    QLineEdit* keywordsEdit = nullptr;
    QLineEdit* creatorEdit = nullptr;

    // Read-only labels
    QLabel* producerLabel = nullptr;
    QLabel* creationDateLabel = nullptr;
    QLabel* modDateLabel = nullptr;

    // Button box
    QDialogButtonBox* buttonBox = nullptr;

    // Store read-only values
    std::string producer;
    std::string creationDate;
    std::string modDate;
};

MetadataDialog::MetadataDialog(QWidget* parent)
    : QDialog(parent)
    , impl_(std::make_unique<Impl>()) {

    setObjectName("metadataDialog");
    setAccessibleName(tr("Document Properties Dialog"));
    setWindowTitle("Document Properties");
    setMinimumSize(400, 350);

    // Create main layout
    auto* mainLayout = new QVBoxLayout(this);

    // Create form layout for metadata fields
    auto* formLayout = new QFormLayout();
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Editable fields
    impl_->titleEdit = new QLineEdit(this);
    impl_->titleEdit->setObjectName("metadataTitleEdit");
    impl_->titleEdit->setAccessibleName(tr("Title"));
    formLayout->addRow("Title:", impl_->titleEdit);

    impl_->authorEdit = new QLineEdit(this);
    impl_->authorEdit->setObjectName("metadataAuthorEdit");
    impl_->authorEdit->setAccessibleName(tr("Author"));
    formLayout->addRow("Author:", impl_->authorEdit);

    impl_->subjectEdit = new QLineEdit(this);
    impl_->subjectEdit->setObjectName("metadataSubjectEdit");
    impl_->subjectEdit->setAccessibleName(tr("Subject"));
    formLayout->addRow("Subject:", impl_->subjectEdit);

    impl_->keywordsEdit = new QLineEdit(this);
    impl_->keywordsEdit->setObjectName("metadataKeywordsEdit");
    impl_->keywordsEdit->setAccessibleName(tr("Keywords"));
    formLayout->addRow("Keywords:", impl_->keywordsEdit);

    impl_->creatorEdit = new QLineEdit(this);
    impl_->creatorEdit->setObjectName("metadataCreatorEdit");
    impl_->creatorEdit->setAccessibleName(tr("Creator"));
    formLayout->addRow("Creator:", impl_->creatorEdit);

    // Read-only fields.
    //
    // Qt::PlainText IS NOT COSMETIC HERE. Every value shown in these three
    // labels comes from the opened document's /Info dictionary, which is
    // attacker-controlled: a PDF can put anything it likes in /Producer.
    // QLabel's default is Qt::AutoText, which runs Qt::mightBeRichText() on the
    // string and switches to rich text if it looks like markup -- so a crafted
    // /Producer renders as HTML rather than as the literal bytes the document
    // contains. A QLabel rendering rich text will resolve <img src=...>, which
    // turns "open a PDF and look at its properties" into a filesystem/network
    // read chosen by the document.
    //
    // The editable fields above are QLineEdit and were never exposed: QLineEdit
    // has no rich-text mode. That asymmetry is exactly why this was easy to
    // miss -- the fields that LOOK more dangerous (the ones you can type into)
    // were the safe ones.
    impl_->producerLabel = new QLabel(this);
    impl_->producerLabel->setObjectName("metadataProducerLabel");
    impl_->producerLabel->setAccessibleName(tr("Producer"));
    impl_->producerLabel->setTextFormat(Qt::PlainText);
    impl_->producerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    formLayout->addRow("Producer:", impl_->producerLabel);

    impl_->creationDateLabel = new QLabel(this);
    impl_->creationDateLabel->setObjectName("metadataCreationDateLabel");
    impl_->creationDateLabel->setAccessibleName(tr("Creation Date"));
    impl_->creationDateLabel->setTextFormat(Qt::PlainText);
    impl_->creationDateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    formLayout->addRow("Creation Date:", impl_->creationDateLabel);

    impl_->modDateLabel = new QLabel(this);
    impl_->modDateLabel->setObjectName("metadataModDateLabel");
    impl_->modDateLabel->setAccessibleName(tr("Modified Date"));
    impl_->modDateLabel->setTextFormat(Qt::PlainText);
    impl_->modDateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    formLayout->addRow("Modified Date:", impl_->modDateLabel);

    mainLayout->addLayout(formLayout);

    // Add spacer
    mainLayout->addStretch();

    // Create button box
    impl_->buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this
    );
    impl_->buttonBox->setObjectName("metadataButtonBox");
    mainLayout->addWidget(impl_->buttonBox);

    // Connect signals
    connect(impl_->buttonBox, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(impl_->buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

MetadataDialog::~MetadataDialog() = default;

void MetadataDialog::setMetadata(const pdfe::metadata::DocumentMetadata& metadata) {
    // Populate editable fields
    impl_->titleEdit->setText(QString::fromStdString(metadata.title));
    impl_->authorEdit->setText(QString::fromStdString(metadata.author));
    impl_->subjectEdit->setText(QString::fromStdString(metadata.subject));
    impl_->keywordsEdit->setText(QString::fromStdString(metadata.keywords));
    impl_->creatorEdit->setText(QString::fromStdString(metadata.creator));

    // Populate read-only labels and store values
    impl_->producer = metadata.producer;
    impl_->producerLabel->setText(QString::fromStdString(metadata.producer));

    impl_->creationDate = metadata.creationDate;
    impl_->creationDateLabel->setText(QString::fromStdString(metadata.creationDate));

    impl_->modDate = metadata.modDate;
    impl_->modDateLabel->setText(QString::fromStdString(metadata.modDate));
}

pdfe::metadata::DocumentMetadata MetadataDialog::metadata() const {
    pdfe::metadata::DocumentMetadata result;

    // Read editable fields
    result.title = impl_->titleEdit->text().toStdString();
    result.author = impl_->authorEdit->text().toStdString();
    result.subject = impl_->subjectEdit->text().toStdString();
    result.keywords = impl_->keywordsEdit->text().toStdString();
    result.creator = impl_->creatorEdit->text().toStdString();

    // Use stored read-only values
    result.producer = impl_->producer;
    result.creationDate = impl_->creationDate;
    result.modDate = impl_->modDate;

    return result;
}

} // namespace pdfe::ui
