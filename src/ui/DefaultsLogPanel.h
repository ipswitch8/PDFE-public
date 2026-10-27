#pragma once

#include <QWidget>
#include <QString>
#include <QDateTime>

class QTextEdit;
class QPushButton;

namespace pdfe::ui {

/// Panel that logs any time a hard-coded default value is used due to error
/// or failure to find/determine something at runtime. Displayed in the right
/// sidebar alongside Properties and Search tabs.
class DefaultsLogPanel : public QWidget {
    Q_OBJECT

public:
    explicit DefaultsLogPanel(QWidget* parent = nullptr);
    ~DefaultsLogPanel() override;

    /// Log a fallback event.
    /// @param context  Short description of what was being determined (e.g. "Overlay font")
    /// @param requested What was requested/expected (e.g. "GlyphLessFont")
    /// @param fallback  The default value that was used instead (e.g. "Arial")
    /// @param reason   Why the fallback was needed (e.g. "Font not found on system")
    void logDefault(const QString& context, const QString& requested,
                    const QString& fallback, const QString& reason);

    /// Clear all log entries.
    void clearLog();

    /// Get the number of logged entries.
    [[nodiscard]] int entryCount() const { return entryCount_; }

private:
    QTextEdit* logView_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    int entryCount_ = 0;
};

/// Global accessor for the DefaultsLogPanel singleton.
/// Set by MainWindow during construction. May be nullptr before MainWindow is created.
DefaultsLogPanel* defaultsLogPanel();
void setDefaultsLogPanel(DefaultsLogPanel* panel);

/// Convenience function to log a default from anywhere in the codebase.
/// No-op if the panel hasn't been created yet.
void logDefaultUsed(const QString& context, const QString& requested,
                    const QString& fallback, const QString& reason);

} // namespace pdfe::ui
