#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace pdfe::ui {
class MainWindow;
}

namespace pdfe::app {

/// GitHub #241 — owns the set of open editor windows, one per document.
///
/// PDFE was a single-window editor: main() built one MainWindow on the stack
/// and every open request funnelled into it. Multi-selecting several PDFs in
/// Explorer therefore opened exactly one, and the rest were discarded with no
/// message. This class is what makes "N files, N windows" possible without
/// giving up the property the single-instance guard exists to protect.
///
/// THE RULE IT KEEPS (R3-02). Two editors on the SAME file can each save it,
/// and whichever saves last silently clobbers the other's edits. So a request
/// for a path that is already open must RAISE that window, never build a second
/// one. Distinct paths get distinct windows; the same path never does. That is
/// strictly what the guard protected before — it only ever forbade a second
/// editor on one document, not a second document.
///
/// Paths are compared canonically, because "already open" has to mean the same
/// FILE, not the same string. A path that does not exist yet cannot be
/// canonicalised, so it falls back to its absolute form.
///
/// What that is TESTED to collapse: redundant `.` / `..` segments, and
/// case-differing Windows spellings. `QFileInfo::canonicalFilePath()` is also
/// documented to resolve symlinks, and this relies on that, but no test here
/// creates one — making a symlink on Windows needs privileges the test account
/// does not reliably have. So symlink collapsing is inherited behaviour, not a
/// verified property of this class, and the distinction is stated rather than
/// glossed. Junctions, 8.3 short names and UNC-vs-drive-letter aliasing are
/// likewise unverified here.
class WindowManager : public QObject {
    Q_OBJECT

public:
    explicit WindowManager(QObject* parent = nullptr);
    ~WindowManager() override;

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    /// Open @p filePath, reusing an existing window where that is correct.
    ///
    /// Resolution order:
    ///   1. A window already showing this file → raise and activate it.
    ///   2. Exactly one window, with no document open → use it. This is the
    ///      empty window main() shows at startup; opening the first file into
    ///      it rather than beside it is what the user expects.
    ///   3. Otherwise → a new window.
    ///
    /// An empty @p filePath raises the most recently used window without
    /// opening anything (the bare "activate" request a secondary launch sends).
    ///
    /// Returns the window the request resolved to, or nullptr if none could be
    /// created. Ownership stays with this manager.
    pdfe::ui::MainWindow* openPath(const QString& filePath);

    /// Show an empty window — the startup case when no file was named.
    pdfe::ui::MainWindow* openEmptyWindow();

    /// How many editor windows are currently open.
    int windowCount() const { return windows_.size(); }

    /// The window currently showing @p filePath, or nullptr.
    pdfe::ui::MainWindow* windowForPath(const QString& filePath) const;

    /// Canonical key for @p filePath — exposed so tests can assert on the same
    /// identity rule the manager uses rather than reimplementing it.
    static QString canonicalKey(const QString& filePath);

private:
    pdfe::ui::MainWindow* createWindow();
    void forget(pdfe::ui::MainWindow* window);
    void raiseWindow(pdfe::ui::MainWindow* window);

    /// Insertion-ordered; last element is the most recently opened.
    QList<pdfe::ui::MainWindow*> windows_;

    /// canonicalKey(path) → window. Only holds windows with a document.
    QHash<QString, pdfe::ui::MainWindow*> byPath_;
};

}  // namespace pdfe::app
