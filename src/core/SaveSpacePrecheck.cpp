// -*- coding: utf-8 -*-
#include "SaveSpacePrecheck.h"

#include <QtGlobal>

#include <mutex>
#include <system_error>
#include <utility>

namespace pdfe::core {

namespace {

std::mutex& handlerMutex()
{
    static std::mutex m;
    return m;
}

SaveSpaceWarningHandler& handlerSlot()
{
    static SaveSpaceWarningHandler handler;
    return handler;
}

/// Guarded by handlerMutex() — one lock for two small pieces of process state
/// touched on the same call path is cheaper and harder to get wrong than two.
SaveSpacePrecheckRecord& recordSlot()
{
    static SaveSpacePrecheckRecord record;
    return record;
}

/// Guarded by handlerMutex() as well — see recordSlot(). GitHub #100.
FreeSpaceProbe& probeSlot()
{
    static FreeSpaceProbe probe;
    return probe;
}

/// Default sink: a qWarning.
///
/// IT LOGS THE FILENAME, NOT THE PATH, ON PURPOSE. qWarning is above
/// LogHandler's default minimum level, so anything emitted here lands in
/// pdfe.log on every install (GitHub #151). A full save path is a directory
/// tree of the user's making — usernames, client names, matter numbers — and
/// putting it in a retained file to report a disk-space condition would be
/// gratuitous. The filename plus the two byte counts is everything needed to
/// act on the warning. A UI handler installed by the GUI has the full path
/// available and shows it on screen, where it is not retained.
void defaultWarn(const SaveSpaceEstimate& est, const std::filesystem::path& target)
{
    qWarning("Low disk space before save of \"%s\": %llu bytes free, "
             "estimated output %llu bytes",
             target.filename().string().c_str(),
             static_cast<unsigned long long>(est.freeBytes),
             static_cast<unsigned long long>(est.estimatedBytes));
}

}  // namespace

std::uintmax_t estimateSaveOutputBytes(const std::filesystem::path& sourcePath) noexcept
{
    if (sourcePath.empty()) return 0;
    std::error_code ec;
    const auto size = std::filesystem::file_size(sourcePath, ec);
    if (ec) return 0;
    return size;
}

SaveSpaceEstimate checkFreeSpaceForSave(const std::filesystem::path& target,
                                        std::uintmax_t estimatedBytes) noexcept
{
    SaveSpaceEstimate out;
    out.estimatedBytes = estimatedBytes;

    if (estimatedBytes == 0) {
        // No estimate means no opinion. Reporting "sufficient" here would let a
        // caller treat an unanswerable question as a clean bill of health.
        return out;
    }

    // The staged copy is written into the TARGET's directory, so the target's
    // volume is the one that has to hold it. Fall back to the parent chain for
    // a target that does not exist yet (the usual Save As case): space() on a
    // missing path fails, but its directory is present.
    std::error_code ec;
    std::filesystem::path probe = target;
    if (!std::filesystem::exists(probe, ec)) {
        ec.clear();
        probe = target.parent_path();
        if (probe.empty()) probe = std::filesystem::current_path(ec);
        if (ec) return out;
    }

    // GitHub #100: the ONE reading of the volume, and the only thing the test
    // seam replaces. Taken under the same lock as the rest of the module's
    // process state, then released before it is CALLED — a probe that itself
    // touched this module (or simply took its time) must not hold the lock.
    FreeSpaceProbe injected;
    {
        std::lock_guard<std::mutex> g(handlerMutex());
        injected = probeSlot();
    }
    if (injected) {
        std::uintmax_t availableBytes = 0;
        if (!injected(probe, availableBytes)) return out;  // "cannot answer"
        out.determined = true;
        out.freeBytes = availableBytes;
        out.insufficient = availableBytes < estimatedBytes;
        return out;
    }

    ec.clear();
    const std::filesystem::space_info info = std::filesystem::space(probe, ec);
    if (ec) return out;

    // A driver that cannot report returns -1 in every field.
    if (info.available == static_cast<std::uintmax_t>(-1)) return out;

    out.determined = true;
    out.freeBytes = info.available;
    out.insufficient = info.available < estimatedBytes;
    return out;
}

void setFreeSpaceProbeForTesting(FreeSpaceProbe probe)
{
    std::lock_guard<std::mutex> g(handlerMutex());
    probeSlot() = std::move(probe);
}

bool freeSpaceProbeInstalled()
{
    std::lock_guard<std::mutex> g(handlerMutex());
    return static_cast<bool>(probeSlot());
}

void setSaveSpaceWarningHandler(SaveSpaceWarningHandler handler)
{
    std::lock_guard<std::mutex> g(handlerMutex());
    handlerSlot() = std::move(handler);
}

SaveSpacePrecheckRecord lastSaveSpacePrecheck()
{
    std::lock_guard<std::mutex> g(handlerMutex());
    return recordSlot();
}

void resetSaveSpacePrecheckRecord()
{
    std::lock_guard<std::mutex> g(handlerMutex());
    recordSlot() = SaveSpacePrecheckRecord{};
}

SaveSpaceEstimate warnIfInsufficientSpaceForSave(
    const std::filesystem::path& target,
    const std::filesystem::path& sourcePath) noexcept
{
    return warnIfInsufficientSpaceForSave(target, estimateSaveOutputBytes(sourcePath));
}

SaveSpaceEstimate warnIfInsufficientSpaceForSave(
    const std::filesystem::path& target,
    std::uintmax_t estimatedBytes) noexcept
{
    SaveSpaceEstimate est;
    try {
        est = checkFreeSpaceForSave(target, estimatedBytes);

        {
            std::lock_guard<std::mutex> g(handlerMutex());
            SaveSpacePrecheckRecord& rec = recordSlot();
            ++rec.sequence;
            rec.target = target;
            rec.estimate = est;
        }

        if (!est.determined || !est.insufficient) return est;

        SaveSpaceWarningHandler handler;
        {
            std::lock_guard<std::mutex> g(handlerMutex());
            handler = handlerSlot();
        }
        if (handler) {
            handler(est, target);
        } else {
            defaultWarn(est, target);
        }
    } catch (...) {
        // A pre-check that throws would turn an advisory into a save failure.
    }
    return est;
}

}  // namespace pdfe::core
