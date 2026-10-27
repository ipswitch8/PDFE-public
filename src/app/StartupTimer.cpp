#include "StartupTimer.h"

namespace pdfe::app {

StartupTimer::StartupTimer() {
    timer_.start();
}

void StartupTimer::checkpoint(const QString& label) {
    checkpoints_.emplace_back(label, timer_.elapsed());
}

const std::vector<std::pair<QString, qint64>>& StartupTimer::checkpoints() const {
    return checkpoints_;
}

qint64 StartupTimer::totalElapsedMs() const {
    return timer_.elapsed();
}

QString StartupTimer::summary() const {
    QString result = "Startup timing:\n";
    qint64 prev = 0;
    for (const auto& [label, elapsed] : checkpoints_) {
        qint64 delta = elapsed - prev;
        result += QString("  [%1 ms] (+%2 ms) %3\n").arg(elapsed).arg(delta).arg(label);
        prev = elapsed;
    }
    result += QString("  Total: %1 ms\n").arg(timer_.elapsed());
    return result;
}

int StartupTimer::count() const {
    return static_cast<int>(checkpoints_.size());
}

} // namespace pdfe::app
