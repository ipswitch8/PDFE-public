#include "DefaultsLogPanel.h"

#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

namespace pdfe::ui {

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

static DefaultsLogPanel* g_defaultsLogPanel = nullptr;

DefaultsLogPanel* defaultsLogPanel()
{
    return g_defaultsLogPanel;
}

void setDefaultsLogPanel(DefaultsLogPanel* panel)
{
    g_defaultsLogPanel = panel;
}

void logDefaultUsed(const QString& context, const QString& requested,
                    const QString& fallback, const QString& reason)
{
    if (g_defaultsLogPanel) {
        g_defaultsLogPanel->logDefault(context, requested, fallback, reason);
    }
}

// ---------------------------------------------------------------------------
// Status line
// ---------------------------------------------------------------------------
//
// The UI matrix row `defaults_log_widget_audit` audits three widgets by
// objectName: the log view, the clear button, and a status label. The first two
// already existed; this panel had no status label at all, so the row could not
// be satisfied without either adding one or quietly narrowing the row to the
// two widgets that happened to exist. Adding it is the honest option, and it
// also makes this panel consistent with LayerPanel, which has had a
// `layerStatusLabel` all along.
//
// Deliberately kept to the .cpp: the label is found via findChild rather than
// held in a member, so DefaultsLogPanel.h is untouched. Editing that header
// would pull in every translation unit that includes it — on this project a
// widely-included header change costs a multi-hour rebuild, and a partial build
// after a class-layout change has previously presented as heap corruption that
// was not a real defect.
static void updateStatusLabel(QWidget* panel, int entryCount)
{
    if (auto* label = panel->findChild<QLabel*>("defaultsLogStatusLabel")) {
        // The count is the load-bearing part: the audit asserts the text
        // CHANGES on a logged default and RETURNS to its empty-state string on
        // clear, so a static caption would pass "is present" and fail "is live".
        label->setText(QObject::tr("Defaults recorded: %1").arg(entryCount));
    }
}

// ---------------------------------------------------------------------------
// DefaultsLogPanel
// ---------------------------------------------------------------------------

DefaultsLogPanel::DefaultsLogPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("defaultsLogPanel");
    setAccessibleName(tr("Defaults Log Panel"));

    logView_ = new QTextEdit(this);
    logView_->setObjectName("defaultsLogView");
    logView_->setAccessibleName(tr("Defaults Log View"));
    logView_->setReadOnly(true);
    logView_->setPlaceholderText(tr("No defaults used yet."));
    logView_->setFont(QFont("Consolas", 9));

    clearButton_ = new QPushButton(tr("Clear"), this);
    clearButton_->setObjectName("defaultsLogClearButton");
    clearButton_->setAccessibleName(tr("Clear Defaults Log"));
    connect(clearButton_, &QPushButton::clicked, this, &DefaultsLogPanel::clearLog);

    auto* headerLayout = new QHBoxLayout;
    auto* headerLabel = new QLabel(tr("Fallback defaults log:"), this);
    headerLayout->addWidget(headerLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(clearButton_);

    auto* statusLabel = new QLabel(this);
    statusLabel->setObjectName("defaultsLogStatusLabel");
    statusLabel->setAccessibleName(tr("Defaults Log Status"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(headerLayout);
    layout->addWidget(logView_);
    layout->addWidget(statusLabel);
    setLayout(layout);

    updateStatusLabel(this, entryCount_);
}

DefaultsLogPanel::~DefaultsLogPanel()
{
    // Clear global pointer to prevent dangling reference after destruction
    if (g_defaultsLogPanel == this) {
        g_defaultsLogPanel = nullptr;
    }
}

void DefaultsLogPanel::logDefault(const QString& context, const QString& requested,
                                   const QString& fallback, const QString& reason)
{
    ++entryCount_;
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString entry = QStringLiteral("<b>[%1]</b> <u>%2</u>: "
                                    "requested <i>\"%3\"</i> → used <b>\"%4\"</b>"
                                    " (%5)")
        .arg(timestamp, context, requested, fallback, reason);
    logView_->append(entry);
    updateStatusLabel(this, entryCount_);
}

void DefaultsLogPanel::clearLog()
{
    logView_->clear();
    entryCount_ = 0;
    updateStatusLabel(this, entryCount_);
}

} // namespace pdfe::ui
