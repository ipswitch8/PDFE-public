#pragma once
#include <QString>
#include <QElapsedTimer>
#include <QStringList>
#include <vector>
#include <utility>

namespace pdfe::app {

class StartupTimer {
public:
    StartupTimer();

    /// Record a labeled checkpoint (stores elapsed time since construction).
    void checkpoint(const QString& label);

    /// Get all checkpoints as (label, elapsedMs) pairs.
    const std::vector<std::pair<QString, qint64>>& checkpoints() const;

    /// Get total elapsed time since construction.
    qint64 totalElapsedMs() const;

    /// Format all checkpoints as a human-readable summary string.
    QString summary() const;

    /// Get count of checkpoints.
    int count() const;

private:
    QElapsedTimer timer_;
    std::vector<std::pair<QString, qint64>> checkpoints_;
};

} // namespace pdfe::app
