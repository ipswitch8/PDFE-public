#include "AboutDialog.h"
#include "MainWindow.h"

// Forward-declare the Qt RCC-generated resource init function in the
// global namespace BEFORE entering pdfe::ui below. The Q_INIT_RESOURCE
// macro expands to `extern int qInitResources_branding();
// qInitResources_branding();` — when expanded inside a namespace, both
// the declaration and the call resolve as namespace-scoped. MSVC tolerates
// this with namespace fallback; clang-cl is strict (per ISO C++) and
// emits the namespaced symbol as undefined. Explicit global-scope decl
// + global-scope call below fixes it for both compilers.
extern int qInitResources_branding();

#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtGlobal>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

namespace {

QString readResourceText(const char* path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QTextStream(&file).readAll();
}

} // anonymous namespace

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    // Pin the static-lib qInitResources_branding() symbol so the
    // linker keeps the .qrc data tables in pdfe.exe. Call directly
    // via the global-scope forward declaration at the top of this
    // file rather than via Q_INIT_RESOURCE, which mangles into the
    // enclosing namespace under strict (clang-cl) lookup rules.
    ::qInitResources_branding();

    setObjectName("aboutDialog");
    setWindowTitle("About PDFE");
    // Match the main window's title-bar icon (theme-aware SVG with
    // .ico fallback). Helper lives in MainWindow.cpp so all top-
    // level windows share one source of truth.
    setWindowIcon(pdfeAppIcon());
    // Resizable, with a sensible minimum that accommodates the logo
    // header + scrollable license. The fixed 450x380 from the
    // pre-overhaul dialog is intentionally removed.
    setMinimumSize(600, 700);
    resize(620, 760);

    setupUi();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

AboutDialog::~AboutDialog() = default;

void AboutDialog::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // ---- Marketing content (Markdown) -------------------------
    // All visible branding/marketing content lives in
    // src/ui/about.md (baked into the binary via branding.qrc as
    // :/branding/about.md). Marketing edits the .md file and
    // rebuilds — no code change needed for text/link/image/font
    // tweaks.
    aboutContent_ = new QTextBrowser(this);
    aboutContent_->setObjectName("aboutContent");
    aboutContent_->setAccessibleName(tr("PDFE about content"));
    aboutContent_->setOpenExternalLinks(true);
    aboutContent_->setFrameShape(QFrame::NoFrame);
    {
        QString md = readResourceText(":/branding/about.md");
        // Placeholder substitution. {{VERSION}} -> app version from
        // QCoreApplication; {{QT_VERSION}} -> Qt build version.
        // Keeping these dynamic means marketing's .md doesn't have
        // to be re-edited every release.
        md.replace(QStringLiteral("{{VERSION}}"),
                   QCoreApplication::applicationVersion());
        md.replace(QStringLiteral("{{QT_VERSION}}"),
                   QString::fromLatin1(QT_VERSION_STR));
        aboutContent_->setMarkdown(md);
        // Center every paragraph. Markdown has no native alignment
        // syntax and wrapping content in <div align="center"> causes
        // Qt's parser to treat the inner content as raw HTML
        // (paragraphs collapse, inline images fail to load), so we
        // post-process the document and apply center alignment to
        // every block instead. Marketing's .md stays plain Markdown.
        QTextCursor cur(aboutContent_->document());
        cur.select(QTextCursor::Document);
        QTextBlockFormat blockFmt;
        blockFmt.setAlignment(Qt::AlignCenter);
        cur.mergeBlockFormat(blockFmt);
    }
    // Marketing content gets the larger share — it has the logo +
    // attribution + donation block. License is reference material;
    // it's still scrollable on its own.
    mainLayout->addWidget(aboutContent_, /*stretch=*/5);

    // ---- License text (AGPL + addendum), scrollable, read-only -
    licenseTextEdit_ = new QTextEdit(this);
    licenseTextEdit_->setObjectName("licenseTextEdit");
    licenseTextEdit_->setAccessibleName(
        tr("AGPL-3.0 license and Commercial Licensing Addendum"));
    licenseTextEdit_->setReadOnly(true);
    licenseTextEdit_->setLineWrapMode(QTextEdit::WidgetWidth);
    {
        // Use a monospace font for the AGPL/addendum text so the
        // fixed-width formatting (bullet alignment, divider rules)
        // renders as authored.
        QFont f = licenseTextEdit_->font();
        f.setStyleHint(QFont::Monospace);
        f.setFamily("Consolas");
        f.setPointSize(9);
        licenseTextEdit_->setFont(f);
    }
    {
        const QString agpl = readResourceText(":/legal/LICENSE.txt");
        const QString addendum = readResourceText(
            ":/legal/commercial_addendum.txt");
        QString combined = agpl;
        if (!combined.isEmpty() && !combined.endsWith('\n')) {
            combined += '\n';
        }
        combined += '\n';
        combined += addendum;
        licenseTextEdit_->setPlainText(combined);
    }
    mainLayout->addWidget(licenseTextEdit_, /*stretch=*/2);

    // ---- OK button ---------------------------------------------
    okButton_ = new QPushButton(tr("OK"), this);
    okButton_->setObjectName("okButton");
    okButton_->setAccessibleName(tr("OK"));
    okButton_->setDefault(true);
    okButton_->setFixedWidth(100);
    connect(okButton_, &QPushButton::clicked, this, &QDialog::accept);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton_);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);
}

QString AboutDialog::versionString() const
{
    return QCoreApplication::applicationVersion();
}

QStringList AboutDialog::libraryVersions() const
{
    QStringList versions;
    versions << QString("Qt %1").arg(QT_VERSION_STR);
    versions << "MuPDF 1.26.10";
    return versions;
}

} // namespace pdfe::ui
