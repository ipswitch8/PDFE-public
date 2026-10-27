#pragma once
//
// Shared clipboard helpers for Windows/RDP-hosted test runs.
//
// WHY THIS EXISTS (meta-audit follow-up F14):
// The Windows clipboard is a machine-wide singleton with no ownership
// guarantee for any one process. Under an RDP session the session manager
// (rdpclip.exe) periodically takes ownership to mirror the clipboard to the
// client, and while it holds it, OpenClipboard() fails for everyone else. Qt's
// QClipboard::setImage()/image() do NOT report this: the setter silently
// no-ops and the getter silently returns a null QImage.
//
// So a clipboard round-trip in a test is inherently racy against machine
// state, and the race widens under load — which is why it surfaces in a
// 16-shard suite run but never in isolation (MediaBoxOriginCrop.
// RedSelectionCropsRedNotBlue passed 12/12 standalone while failing in-shard).
//
// THE IMPORTANT PART: retrying the READ alone is not sufficient, and that was
// the actual defect. If the WRITE was the call that got swallowed, the
// clipboard simply does not hold the image and re-reading it forever cannot
// recover. The copy action itself has to be re-driven. Two prior copies of
// these helpers had diverged (8x20ms read-only vs 6x20ms read+write); this
// header is the single definition so that cannot drift again.
//
// These helpers widen the TIMING tolerance only. They never relax an
// assertion: callers still assert on the image being non-null and on its
// size and pixel content.

#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>

namespace pdfe::testsupport {

// Total retry budget. Sized for a loaded 16-shard sequential run on a
// Server-2019 RDP host, where an rdpclip ownership window has been observed to
// exceed the previous 160ms budget. Overshooting costs nothing on the happy
// path: every helper returns on the FIRST successful attempt.
inline constexpr int kClipboardAttempts = 60;
inline constexpr int kClipboardDelayMs = 50;   // => ~3s worst case

// Pump the Qt event loop and sleep, so a clipboard ownership flip has both a
// chance to be delivered to Qt and wall-clock time to resolve.
inline void clipboardSettle(int delayMs = kClipboardDelayMs) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                    delayMs);
    QThread::msleep(static_cast<unsigned long>(delayMs));
}

// Read an image back from the clipboard, tolerating transient unavailability.
inline QImage rdpSafeClipboardImage(QClipboard* cb,
                                    int maxAttempts = kClipboardAttempts,
                                    int delayMs = kClipboardDelayMs) {
    if (!cb) return {};
    for (int i = 0; i < maxAttempts; ++i) {
        QImage img = cb->image();
        if (!img.isNull()) return img;
        clipboardSettle(delayMs);
    }
    return {};
}

// Write an image to the clipboard and confirm it actually landed.
inline bool rdpSafeSetClipboardImage(QClipboard* cb, const QImage& img,
                                     int maxAttempts = kClipboardAttempts,
                                     int delayMs = kClipboardDelayMs) {
    if (!cb || img.isNull()) return false;
    for (int i = 0; i < maxAttempts; ++i) {
        cb->setImage(img);
        clipboardSettle(delayMs);
        if (!cb->image().isNull()) return true;
    }
    return false;
}

// Re-drive a COPY ACTION until its result is observable on the clipboard.
//
// `copyAction` must be the production call under test (e.g.
// MainWindow::copySelectionAsImage). It is invoked afresh on each attempt,
// because a swallowed write is only recoverable by performing the write again.
// Returns a null QImage if the action never reported success, or never
// produced an observable clipboard image within the budget.
template <typename CopyAction>
QImage rdpSafeCopyToClipboardImage(QClipboard* cb, CopyAction&& copyAction,
                                   int maxAttempts = kClipboardAttempts,
                                   int delayMs = kClipboardDelayMs) {
    if (!cb) return {};
    for (int i = 0; i < maxAttempts; ++i) {
        cb->clear();
        clipboardSettle(delayMs);
        if (!copyAction()) {
            clipboardSettle(delayMs);
            continue;
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                        delayMs);
        QImage img = cb->image();
        if (!img.isNull()) return img;
        clipboardSettle(delayMs);
    }
    return {};
}

} // namespace pdfe::testsupport
