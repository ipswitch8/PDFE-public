#include "app/WindowManager.h"

#include "ui/MainWindow.h"

#include <QFileInfo>

namespace pdfe::app {

WindowManager::WindowManager(QObject* parent) : QObject(parent) {}

WindowManager::~WindowManager()
{
    // Windows are WA_DeleteOnClose and unparented, so they are not owned via
    // the QObject tree. Anything still open at shutdown is ours to destroy.
    const QList<pdfe::ui::MainWindow*> remaining = windows_;
    windows_.clear();
    byPath_.clear();
    for (pdfe::ui::MainWindow* window : remaining) {
        delete window;
    }
}

QString WindowManager::canonicalKey(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return QString();
    }
    const QFileInfo info(filePath);

    // canonicalFilePath() resolves symlinks and `..`, and is empty for a path
    // that does not exist — a file the user names but that is missing still
    // deserves a stable identity, so fall back to the absolute form.
    const QString canonical = info.canonicalFilePath();
    const QString resolved = canonical.isEmpty() ? info.absoluteFilePath() : canonical;

#ifdef Q_OS_WIN
    // Windows paths are case-insensitive, so C:\A.pdf and c:\a.pdf are one
    // file. Folding here keeps "already open" a question about the FILE.
    return resolved.toLower();
#else
    return resolved;
#endif
}

pdfe::ui::MainWindow* WindowManager::windowForPath(const QString& filePath) const
{
    const QString key = canonicalKey(filePath);
    if (key.isEmpty()) {
        return nullptr;
    }
    return byPath_.value(key, nullptr);
}

void WindowManager::raiseWindow(pdfe::ui::MainWindow* window)
{
    if (!window) {
        return;
    }
    window->setWindowState((window->windowState() & ~Qt::WindowMinimized) |
                           Qt::WindowActive);
    window->show();
    window->raise();
    window->activateWindow();
}

pdfe::ui::MainWindow* WindowManager::createWindow()
{
    auto* window = new pdfe::ui::MainWindow();

    // Unparented and self-deleting: closing a window must destroy it, or a
    // "closed" document would keep its MuPDF handles and its lock on the file.
    window->setAttribute(Qt::WA_DeleteOnClose);

    // Drop it from our books when it goes, however it goes — a user close, a
    // WA_DeleteOnClose teardown, or a destroy from somewhere else entirely.
    // Keyed on the QObject::destroyed signal rather than closeEvent so there is
    // no path that leaves a dangling pointer in windows_ or byPath_.
    connect(window, &QObject::destroyed, this, [this, window]() { forget(window); });

    // GitHub #241 — a drop of several PDFs asks for several windows, so route
    // every dropped file through openPath() and let it decide where each goes.
    connect(window, &pdfe::ui::MainWindow::openPathRequested, this,
            [this](const QString& path) { openPath(path); });

    // Say so explicitly rather than leaving the window to work it out.
    //
    // The first version had dropEvent() ask isSignalConnected() whether anyone
    // was listening. That reported NO receiver with this very connection in
    // place, so the drop quietly fell back to opening one file and discarding
    // the rest — #241's defect, restored by the mechanism meant to avoid it, and
    // silently. A window that is managed is told it is managed.
    window->setDropRoutingDelegated(true);

    windows_.append(window);
    return window;
}

void WindowManager::forget(pdfe::ui::MainWindow* window)
{
    windows_.removeAll(window);
    for (auto it = byPath_.begin(); it != byPath_.end();) {
        if (it.value() == window) {
            it = byPath_.erase(it);
        } else {
            ++it;
        }
    }
}

pdfe::ui::MainWindow* WindowManager::openEmptyWindow()
{
    pdfe::ui::MainWindow* window = createWindow();
    window->show();
    return window;
}

pdfe::ui::MainWindow* WindowManager::openPath(const QString& filePath)
{
    if (filePath.isEmpty()) {
        // A bare activate request: raise what we have rather than opening an
        // extra empty window the user did not ask for.
        if (windows_.isEmpty()) {
            return openEmptyWindow();
        }
        raiseWindow(windows_.last());
        return windows_.last();
    }

    const QString key = canonicalKey(filePath);

    // 1. Already open → raise it. This is R3-02: never a second editor on one
    //    file, because both could save it.
    if (pdfe::ui::MainWindow* existing = byPath_.value(key, nullptr)) {
        raiseWindow(existing);
        return existing;
    }

    // 2. A single empty window (the one startup shows) absorbs the first file.
    pdfe::ui::MainWindow* target = nullptr;
    if (windows_.size() == 1 && !windows_.first()->documentForTesting()) {
        target = windows_.first();
    } else {
        // 3. Otherwise this document gets its own window.
        target = createWindow();
    }

    // show() BEFORE openFile(), and the order is load-bearing.
    //
    // openFile() reports a failure with a modal QMessageBox, and exec() blocks
    // until it is dismissed. Opening first therefore means that for a path that
    // cannot be opened, the error dialog appears while the window behind it has
    // never been shown — so the application presents a bare modal and no main
    // window at all. The MainWindow that main() used to own called show() first;
    // keeping that order preserves the behaviour.
    //
    // GUARDED, and it took two attempts to get an oracle that could see it.
    // Swapping these two lines is mutation `M5`, and it is killed by
    // MultiDocumentOpenTest.TheWindowIsOnScreenWhileTheOpenFailureDialogIsUp.
    //
    // Any assertion made AFTER the dialog closes passes either way — show() runs
    // regardless, so the end state is identical and the defect is invisible. The
    // question has to be asked from INSIDE the modal loop, which the project's
    // dialog seam can do: openFile()'s failure path already registers
    // registerDialogIntrospection() on this very dialog, and it answers queries
    // synchronously while exec() runs. The test drives a `parentVisible` query
    // from a timer that fires inside that loop.
    //
    // This was briefly recorded as "unguardable". It was not — the mechanism was
    // already registered one line above box.exec() and had simply never been
    // asked.
    target->show();
    target->openFile(filePath);

    // Only claim the key if the document actually opened. A failed open leaves
    // the window empty and reusable, and must not make the path look taken.
    if (target->documentForTesting()) {
        byPath_.insert(key, target);
    }

    raiseWindow(target);
    return target;
}

}  // namespace pdfe::app
