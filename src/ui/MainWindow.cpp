#include "MainWindow.h"
#include <atomic>  // GitHub #218: recomposite-completion sequence counter.
#include "PdfCanvas.h"
#include "TestPipeFocusGuard.h"   // GitHub #303
#include "PagePanel.h"
#include "PropertiesPanel.h"
#include "StatusBar.h"
#include "SearchPanel.h"
#include "../core/SearchResult.h"
#include "../core/TempFileSafety.h"
#include "BookmarkPanel.h"
#include "LayerPanel.h"
#include "WatermarkDialog.h"
#include "HeaderFooterDialog.h"
#include "OptimizeDialog.h"
#include "../measure/MeasureTool.h"
#include "../measure/MeasureDictReader.h"
#include "CommentPanel.h"
#include "AnnotationPropertiesWidget.h"
#include "StickyNotePopup.h"
#include "DefaultsLogPanel.h"
#include "BatchDialog.h"
#include "CompareDialog.h"
#include "CustomizeToolbarsDialog.h"
#include "ToolbarLayoutManager.h"
#include "CombineFilesWizard.h"
#include "PrintDialog.h"
#include "PrintPreviewDialog.h"
#include "../core/Document.h"
#include "../core/Error.h"   // PdfeException — thrown by the /DSS write scope
#include "../core/Page.h"
#include "../core/MuPdfContext.h"
#include "../core/JpxSecurityGate.h"
#include "../core/Jbig2SecurityGate.h"
#include "../core/PreflightOpen.h"
#include "../core/UnencryptedWrapperProbe.h"
#include "../core/PathSecurity.h"
#include "../core/PrintEngine.h"
#include "../core/UndoManager.h"
#include "../core/AutosaveManager.h"
#include "../app/PowerEvents.h"
#include "../core/AtomicFileReplace.h"
#include "../core/SaveWithRebind.h"
#include "../rendering/TileCache.h"
#include "../rendering/TileRenderer.h"
#include "../annotations/AnnotationManager.h"
#include "../annotations/AnnotationCommands.h"
#include "../annotations/TextSelector.h"
#include "../core/PageCommands.h"
#include "../core/PageOperations.h"
#include "../annotations/StickyNoteTool.h"
#include "../annotations/TextMarkupTool.h"
#include "../annotations/InkTool.h"
#include "../annotations/ShapeTool.h"
#include "../annotations/FreeTextTool.h"
#include "../annotations/StampTool.h"
// GitHub #253. LinkTool was the one annotation tool of eleven that nothing in
// src/ referenced, so it was compiled in, unit-tested, and unreachable from
// main(). Wired below in setupToolBars().
#include "../annotations/LinkTool.h"
#include "../annotations/ImageStampLibrary.h"
#include "../annotations/FileAttachmentTool.h"
#include "../annotations/CropTool.h"
#include "../core/PageOperations.h"
#include "../optimize/GrayscaleConverter.h"
#include "PageLabelsDialog.h"

#include <QInputDialog>
#include "../annotations/SelectTool.h"
#include "../forms/FormManager.h"
#include "../forms/FormTool.h"
#include "../forms/FormCommands.h"
#include "../forms/JavaScriptEngine.h"
#include "../forms/FormDataIO.h"
// GitHub #18 (PDF-48). This is the ONLY translation unit in src/ui that may
// reference the form-submit module, and tests/test_action_egress_inertness.cpp
// asserts exactly that: /SubmitForm is a network-egress path, so the wiring is
// kept to one reviewable file with one gated call site.
#include "../formsubmit/FormEncoder.h"
#include "../formsubmit/FormFieldBridge.h"
#include "../formsubmit/FormSubmitter.h"
#include "../formsubmit/HostAllowlist.h"
#include "../textedit/TextEditTool.h"
#include "../textedit/TextEditCommands.h"
#include "../reflow/AlignmentPipeline.h"
#include "../reflow/BlockParamsBuilder.h"
#include "../reflow/ReflowTypes.h"

#include <fstream>
#include "TextEditToolBar.h"
#include "FormFieldPropertiesPanel.h"
#include "../fonts/EmbeddedFontAnalyzer.h"
#include "../fonts/FontAnalysisCache.h"
#include "../fonts/FontResolver.h"
#include "../fonts/SubstituteFontSelector.h"
#include "../fonts/PdfFontEmbedder.h"
#include "../fonts/VectorSubsetDetector.h"
#include "../tre/OcrPipeline.h"
#include "../tre/OcrAllPagesCommand.h"
#include <QProgressDialog>
#include "../tre/HocrParser.h"
#include "../tre/CoordMapper.h"
#include "../tre/BgColorDetector.h"
#include "../tre/FontMatcher.h"
#include "../tre/QualityAssessor.h"
#include "../tre/ImageMasker.h"
#include "../tre/BitmapCompositor.h"
#include "../tre/PdfContentWriter.h"
#include "../tre/RegionReconstructor.h"
#include "../tre/ImageReplacer.h"
#include "../tre/AtlasBuilder.h"
#include "../tre/CNNFontClassifier.h"
#include "AtlasComparisonDialog.h"
#include "../tre/TreRegionTool.h"
#include "../tre/TreEditDialog.h"
#include "../tre/TreCommands.h"
#include "RedactionPanel.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionTool.h"
#include "redaction/RedactionData.h"
#include "redaction/RedactionVerifier.h"
#include "../core/QpdfWrapper.h"
#include "encryption/EncryptionManager.h"
#include "PasswordDialog.h"
#include "EncryptionDialog.h"
#include "PreferencesDialog.h"
#include "SignaturePanel.h"
#include "AttachmentPanel.h"
#include "SignDialog.h"
#include "ValidationRequestDialog.h"
#include "signatures/SignatureManager.h"
#include "signatures/ValidationCoordinator.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureCommands.h"
#include "signatures/LtvValidator.h"
#include "signatures/Pkcs7OpenSslBridge.h"
// F24 — the production consumer for DocMdpEnforcer (installed in the ctor).
#include "signatures/DocMdpSaveGuard.h"
#include "TrustAnchorDialog.h"
#include "TestDialogSeam.h"
#include "ImagePropertiesPanel.h"
#include "MetadataDialog.h"
#include "ComplianceDialog.h"
#include "PreflightDialog.h"
// GitHub #60 (SEC-62): the open-path active-content scan.
#include "compliance/ProhibitedFeatureScanner.h"
#include "ImageToPdfWizard.h"
#include "../exports/PlainTextExporter.h"
#include "TabOrderDialog.h"
#include "CalculationOrderDialog.h"
#include "AltTextDialog.h"
#include "RecentFilesMenu.h"
#include "AboutDialog.h"
#include "../app/AppSettings.h"
// GitHub #127 -- opt-in, notify-only version check.
#include "../app/UpdateChecker.h"
#include "../app/VersionInfo.h"
#include <QNetworkAccessManager>
#include "../theme/ThemeManager.h"
#include "../image/ImageManager.h"
#include "../image/ImageContentEditor.h"
#include "../image/ExifStripper.h"
#include "../image/ImageTool.h"
#include "ui/InsertImageDialog.h"
#include <QImageReader>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <atomic>
#include <thread>

#include "../image/ImageCommands.h"
#include "../image/ImageData.h"
#include "../metadata/MetadataManager.h"
#include "../metadata/MetadataCommands.h"

#include <QMenu>
#include <set>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QAction>
#include <QFileDialog>
#include <QTimer>
#include <QMetaMethod>
#include <QMessageBox>
#include <QComboBox>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QApplication>
#include <QKeyEvent>   // GitHub #295: ShortcutTraceFilter inspects key events
#include <QIcon>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QPrinter>
#include <QActionGroup>
#include <QPushButton>
#include <QColorDialog>
#include <QIcon>
#include <QFile>
#include <QImage>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QShortcut>
#include <QDebug>
#include <QDesktopServices>
#include <QAccessible>
#include <QTextStream>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QLineEdit>
#include <QFontComboBox>
#include <QTextEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFrame>       // GitHub #60 -- active-content advisory banner
#include <QHBoxLayout>  // GitHub #60 -- its internal layout
#include <QDialog>
// F10 / WIN-68+WIN-69: session-end (logoff / shutdown) handling.
#include <QGuiApplication>
#include <QStyleHints>
#include <QSessionManager>
#include <QWindow>
#include <filesystem>
#include <io.h>
#include <cstdint>
#include <string_view>

#include "core/SaveSpacePrecheck.h"  // GitHub #100: free-space simulation pipe
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// bugfix-3e91a7c4 #2: forward-declare the RCC-generated init for
// action_icons.qrc at GLOBAL scope (see AboutDialog.cpp for the same pattern
// with branding.qrc). The .qrc lives in the pdfe_ui STATIC lib; nothing
// references its data by symbol, so the linker would drop the resource tables
// and QIcon(":/icons/actions/*.svg") would resolve to a null icon. Pinning the
// init symbol with an explicit global-scope call in createToolBar() keeps the
// tables and registers the resource before the toolbar icons are set.
extern int qInitResources_action_icons();

namespace pdfe::ui {

// PDFE-LIMITATION-UNICODE-PATH (UI boundary): a file path coming from a
// QFileDialog / drag-drop is UTF-16 in the QString. `QString::toStdString()`
// yields UTF-8, and constructing `std::filesystem::path` from a narrow
// `std::string` on Windows re-interprets those bytes as the system ANSI
// code page — corrupting any non-ASCII path before Document's own UTF-8
// handling can help. Build the path losslessly from the wide string on
// Windows (and from UTF-8 on POSIX, where that is already correct).
// F10 / WIN-69: hold off an OS shutdown for the duration of a save.
//
// The save path has a temp-write → atomic-rename window. If Windows terminates
// the process inside that window the original is still intact (R2-04), but the
// user's edits are gone and an orphan staging file is left behind — named
// "<target>.pdfe_tmp" by the full-rewrite save, or "<target>.pdfe_part.<hex>"
// by the staged incremental append (F58-a, phase-53). Windows offers
// exactly one supported way to ask for that window to be respected:
// ShutdownBlockReasonCreate, which shows the reason string on the shutdown-block
// screen and stops the OS from force-closing us while it is registered.
//
// RAII so the block is destroyed on every exit path, including a throwing save.
// Inert (does nothing at all) on non-Windows, and when there is no native window
// handle yet — which is the case for a headless test MainWindow.
class ShutdownBlocker {
public:
    ShutdownBlocker(QWidget* w, const QString& reason)
    {
#if defined(_WIN32)
        if (w && w->windowHandle()) {
            hwnd_ = reinterpret_cast<HWND>(w->winId());
            if (hwnd_ && ::ShutdownBlockReasonCreate(
                    hwnd_, reinterpret_cast<LPCWSTR>(reason.utf16()))) {
                return;
            }
            hwnd_ = nullptr;
        }
#else
        (void)w;
        (void)reason;
#endif
    }
    ~ShutdownBlocker()
    {
#if defined(_WIN32)
        if (hwnd_) {
            ::ShutdownBlockReasonDestroy(hwnd_);
        }
#endif
    }
    ShutdownBlocker(const ShutdownBlocker&) = delete;
    ShutdownBlocker& operator=(const ShutdownBlocker&) = delete;

private:
#if defined(_WIN32)
    HWND hwnd_ = nullptr;
#endif
};

static std::filesystem::path qStringToPath(const QString& s)
{
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}

// Load the PDFE application icon. pdfe-red.ico is the sole source
// — a multi-resolution Windows ICO carrying 16/24/32/48/64/128/256
// PNG-encoded frames; Qt's QIcon picks the right size at each
// rendering site automatically. Exposed so AboutDialog and any
// other top-level dialog can use the same icon helper.
QIcon pdfeAppIcon()
{
    return QIcon(QStringLiteral(":/icons/pdfe.ico"));
}

// Write a machine-readable status file for UI automation (pywinauto/Selenium).
// The file is overwritten at each pipeline stage so tests can poll for readiness.
// The UI-automation sentinel. Eight Python UI tests poll this exact path by
// name, so unlike every other temp write hardened in phase-60 the NAME cannot
// move -- randomising it would retire those tests rather than harden anything.
//
// A fixed name in a world-writable directory is a PLANT-AND-REDIRECT target: an
// attacker who pre-creates it as a hardlink to a victim file, or as a symlink
// to one, turns this innocuous status write into an arbitrary truncate-and-
// overwrite under PDFE's token. So the name stays and the OPEN is hardened --
// core::writeFixedNameTempFile refuses to traverse a reparse point and refuses
// a file with more than one link. A refusal writes nothing and is silent: this
// is a test sentinel, and failing to update it is not worth a user-facing
// error, whereas destroying a file the user owns very much is.
static void writeUiStatus(const QString& status, const QString& detail = {})
{
    const std::filesystem::path path = std::filesystem::path(
        (QDir::tempPath() + QStringLiteral("/pdfe_ui_status.txt")).toStdWString());
    QString payload = status;
    if (!detail.isEmpty())
        payload += QStringLiteral("|") + detail;
    payload += QStringLiteral("\n");
    const QByteArray bytes = payload.toUtf8();
    std::error_code ec;
    core::writeFixedNameTempFile(
        path, std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())),
        /*append=*/false, ec);
}

// GitHub #218: the ImageReplacer recomposite-complete sentinel.
//
// Three UI render probes were xfail(strict=False) for one measured reason: the
// scanned-page recomposite only engages after the OCR -> atlas chain finishes,
// and NOTHING published that it had finished. The harness therefore sampled the
// screenshot after a fixed settle sleep (render_drive.commit()'s 1.5 s), which
// is a race dressed as a delay -- it is either too short (flake) or too long
// (slow and still not guaranteed).
//
// The counter matters as much as the status. writeUiStatus overwrites ONE file,
// so a waiter that merely sees "RECOMPOSITE_DONE" cannot tell this edit's
// recomposite from the previous edit's -- it would return instantly on a stale
// line and reintroduce the very race it replaces. The waiter reads the sequence
// BEFORE the edit and requires a strictly greater one after.
static std::atomic<int> g_recompositeSeq{0};

static void writeRecompositeStatus(const char* status, int pageIndex,
                                   const QString& detail)
{
    const int seq = ++g_recompositeSeq;
    writeUiStatus(QString::fromLatin1(status),
                  QStringLiteral("%1|%2|%3")
                      .arg(pageIndex)
                      .arg(seq)
                      .arg(detail));
}

// Pipeline E phase-13: process-wide hook + counters used by the
// /Launch link-follow tests. Defined here (not in the header) so the
// test seam is opt-in — production never touches the handler.
//
// Pipeline F phase-1: counters made std::atomic and the handler is
// guarded by a mutex so concurrent test execution (parallel GTest,
// TSan stress) can't tear reads or race the std::function copy.
MainWindow::LaunchHandler MainWindow::sLaunchHandler_;
std::mutex MainWindow::sLaunchHandlerMutex_;
std::atomic<int> MainWindow::sLaunchPromptCount_{0};
std::atomic<int> MainWindow::sLaunchExecutedCount_{0};

namespace {

// GitHub #61 (SEC-63) -- per-document /Launch prompt cap.
//
// PDFE deliberately has no trusted-source allowlist: every /Launch action
// prompts, every time (see EveryLaunchClickReprompts_NoTrustListShortcut).
// That is the right default, and it is also the attack surface -- a hostile
// PDF can carry an unbounded number of /Launch link annotations, and walking a
// user through hundreds of identical modal prompts is a workable way to get
// one "Open" click. The prompt is a security control, and a control the user
// is being drowned in is not one.
//
// So the number of times a SINGLE DOCUMENT may ask is bounded. Past the cap
// the link is refused outright, without asking, for as long as that document
// is open.
//
// The cap is per document rather than per session because the threat is one
// document's link set; reopening a document the user actually trusts should
// not inherit a previous document's exhaustion.
//
// Chosen as 5: comfortably more than any legitimate document has been observed
// to need, and few enough that mass-prompting stops being useful. It is not
// derived from a standard -- there isn't one -- and it is stated here rather
// than left as a bare literal.
constexpr int kMaxLaunchPromptsPerDocument = 5;

std::atomic<int>  gLaunchPromptsThisDocument{0};
std::atomic<bool> gLaunchCapNoticeShown{false};

/// Called wherever `document_` is replaced. NOT keyed on the document POINTER:
/// a freed Document can be reallocated at the same address, and a stale
/// pointer-identity check would then silently carry one document's exhausted
/// cap into its successor. Resetting on assignment has no such failure mode.
void resetPerDocumentLaunchPrompts()
{
    gLaunchPromptsThisDocument.store(0, std::memory_order_release);
    gLaunchCapNoticeShown.store(false, std::memory_order_release);
}

}  // namespace

// GitHub #295 — records who sees a Ctrl+Z, and in what state. See the comment
// on MainWindow::testKeyTrace_ for why this records instead of answering a
// query.
//
// Installed on qApp, NOT on MainWindow: a window-level filter is offered the
// event only after Qt has routed it to the focused widget's chain, which is
// precisely the case hypothesis 1 describes. An application filter sees it
// first, for every target.
//
// It NEVER consumes the event and never changes dispatch — it only observes, so
// installing it cannot itself alter the behaviour being measured.
class ShortcutTraceFilter : public QObject {
public:
    explicit ShortcutTraceFilter(MainWindow* window)
        : QObject(window), window_(window) {}

    bool eventFilter(QObject* target, QEvent* event) override
    {
        const QEvent::Type t = event->type();
        if (t == QEvent::ShortcutOverride || t == QEvent::KeyPress
            || t == QEvent::KeyRelease || t == QEvent::Shortcut) {
            // QEvent::Shortcut is not a QKeyEvent, so it is recorded by type
            // only. It is included because its ABSENCE next to a present
            // KeyPress is the whole question: the key arrived and no shortcut
            // matched.
            if (t == QEvent::Shortcut) {
                if (window_->testKeyTraceAll_) {
                    window_->recordCtrlZTrace("Shortcut",
                                              qobject_cast<QWidget*>(target));
                }
                return QObject::eventFilter(target, event);
            }
            auto* ke = static_cast<QKeyEvent*>(event);
            // Ctrl+Z, excluding Ctrl+Shift+Z which is redo on this platform.
            const bool isCtrlZ =
                ke->key() == Qt::Key_Z
                && ke->modifiers().testFlag(Qt::ControlModifier)
                && !ke->modifiers().testFlag(Qt::ShiftModifier);
            // GitHub #295, second round. The first run of the probe recorded
            // NOTHING -- not even on a pristine document, where neither of the
            // issue's hypotheses applies. "No Ctrl+Z arrived" and "no key of
            // any kind arrived" are different findings with different owners
            // (the product versus the harness or the session), and a
            // Ctrl+Z-only filter cannot tell them apart: both read as silence.
            // So the trace can be widened to every key event on request.
            if (!isCtrlZ && !window_->testKeyTraceAll_) {
                return QObject::eventFilter(target, event);
            }
            const QString kind =
                QStringLiteral("%1 key=0x%2 mods=0x%3")
                    .arg(t == QEvent::ShortcutOverride
                             ? QStringLiteral("ShortcutOverride")
                             : (t == QEvent::KeyPress
                                    ? QStringLiteral("KeyPress")
                                    : QStringLiteral("KeyRelease")))
                    .arg(ke->key(), 0, 16)
                    .arg(static_cast<unsigned>(ke->modifiers()), 0, 16);
            window_->recordCtrlZTrace(kind.toUtf8().constData(),
                                      qobject_cast<QWidget*>(target));
        }
        return QObject::eventFilter(target, event);
    }

private:
    MainWindow* window_;
};

void MainWindow::setLaunchHandlerForTesting(LaunchHandler handler)
{
    std::lock_guard<std::mutex> lock(sLaunchHandlerMutex_);
    sLaunchHandler_ = std::move(handler);
}

int MainWindow::launchPromptCountForTesting()
{
    return sLaunchPromptCount_.load(std::memory_order_acquire);
}

int MainWindow::launchExecutedCountForTesting()
{
    return sLaunchExecutedCount_.load(std::memory_order_acquire);
}

void MainWindow::resetLaunchCountersForTesting()
{
    sLaunchPromptCount_.store(0, std::memory_order_release);
    sLaunchExecutedCount_.store(0, std::memory_order_release);
    // GitHub #61: the per-document cap must reset here too. Without this a
    // test that exhausts the cap would silently disarm the /Launch prompt for
    // every LATER test in the same binary -- cross-test contamination that
    // would look like an unrelated failure somewhere else entirely.
    resetPerDocumentLaunchPrompts();
}

// Meta-audit F13 (SEC-49): /URI external-action gate state. See MainWindow.h.
MainWindow::UriHandler MainWindow::sUriHandler_;
std::mutex MainWindow::sUriHandlerMutex_;
std::atomic<int> MainWindow::sUriDispatchedCount_{0};
std::atomic<int> MainWindow::sUriRefusedCount_{0};

// WIN-87 site #4 publish-window seam. See MainWindow.h.
std::function<void()> MainWindow::sRedactionRewriteWindowHook_;
std::mutex MainWindow::sRedactionRewriteWindowHookMutex_;

void MainWindow::setRedactionRewriteWindowHookForTesting(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(sRedactionRewriteWindowHookMutex_);
    sRedactionRewriteWindowHook_ = std::move(hook);
}

// WIN-87 site #5 save-window seam. See MainWindow.h.
std::function<void()> MainWindow::sSaveCloseWindowHook_;
std::mutex MainWindow::sSaveCloseWindowHookMutex_;

void MainWindow::setSaveCloseWindowHookForTesting(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(sSaveCloseWindowHookMutex_);
    sSaveCloseWindowHook_ = std::move(hook);
}

void MainWindow::fireSaveCloseWindowHookForTesting()
{
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(sSaveCloseWindowHookMutex_);
        hook = sSaveCloseWindowHook_;
    }
    if (hook) hook();
}

// WIN-87 site #5 staging-publish seam. See MainWindow.h.
std::function<void(const std::filesystem::path&)>
    MainWindow::sAlignStagingWindowHook_;
std::mutex MainWindow::sAlignStagingWindowHookMutex_;

void MainWindow::setAlignStagingWindowHookForTesting(
    std::function<void(const std::filesystem::path&)> hook)
{
    std::lock_guard<std::mutex> lock(sAlignStagingWindowHookMutex_);
    sAlignStagingWindowHook_ = std::move(hook);
}

void MainWindow::fireAlignStagingWindowHookForTesting(
    const std::filesystem::path& stagingPath)
{
    std::function<void(const std::filesystem::path&)> hook;
    {
        std::lock_guard<std::mutex> lock(sAlignStagingWindowHookMutex_);
        hook = sAlignStagingWindowHook_;
    }
    if (hook) hook(stagingPath);
}

// WIN-87 site #4 output-staging seam. See MainWindow.h.
std::function<void(const std::filesystem::path&)>
    MainWindow::sRedactionOutputStagingHook_;
std::mutex MainWindow::sRedactionOutputStagingHookMutex_;

void MainWindow::setRedactionOutputStagingWindowHookForTesting(
    std::function<void(const std::filesystem::path&)> hook)
{
    std::lock_guard<std::mutex> lock(sRedactionOutputStagingHookMutex_);
    sRedactionOutputStagingHook_ = std::move(hook);
}

void MainWindow::fireRedactionOutputStagingHookForTesting(
    const std::filesystem::path& stagingPath)
{
    std::function<void(const std::filesystem::path&)> hook;
    {
        std::lock_guard<std::mutex> lock(sRedactionOutputStagingHookMutex_);
        hook = sRedactionOutputStagingHook_;
    }
    if (hook) hook(stagingPath);
}

void MainWindow::setUriHandlerForTesting(UriHandler handler)
{
    std::lock_guard<std::mutex> lock(sUriHandlerMutex_);
    sUriHandler_ = std::move(handler);
}

int MainWindow::uriDispatchedCountForTesting()
{
    return sUriDispatchedCount_.load(std::memory_order_acquire);
}

int MainWindow::uriRefusedCountForTesting()
{
    return sUriRefusedCount_.load(std::memory_order_acquire);
}

void MainWindow::resetUriCountersForTesting()
{
    sUriDispatchedCount_.store(0, std::memory_order_release);
    sUriRefusedCount_.store(0, std::memory_order_release);
}

bool MainWindow::externalActionsAllowed() const
{
    // Fail CLOSED: no settings object means no way to prove the user opted in.
    if (!appSettings_) return false;
    return appSettings_->externalActionsAllowed();
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName("mainWindow");
    setWindowTitle("PDFE");
    // Drag-drop file open (UI-DD-01). dragEnterEvent + dropEvent
    // route .pdf drops through openFile().
    setAcceptDrops(true);
    // Explicit per-window icon. main.cpp also calls
    // QApplication::setWindowIcon(); we duplicate it here so that
    // a) the title-bar icon is set before any Win32 native window-
    // frame creation that follows show(), and b) tests that
    // construct MainWindow directly (without a fully-configured
    // QApplication) still get the branded icon.
    setWindowIcon(pdfeAppIcon());
    resize(1280, 800);

    // Create app settings
    appSettings_ = std::make_unique<pdfe::app::AppSettings>();

    // #50: apply the persisted JPEG2000-rendering preference to the MuPDF decode
    // gate at startup, BEFORE any PDF is opened. Default OFF — a malformed JPX can
    // heap-overflow the bundled OpenJPEG (potential RCE) and fz_try/fz_catch cannot
    // trap it, so JPX bytes never reach openjpeg unless the user has opted in.
    pdfe::core::setJpxDecodingEnabled(appSettings_->jpxRenderingEnabled());

    // SEC-56 (#56): same treatment for JBIG2 — apply the persisted preference to
    // the MuPDF decode gate at startup, BEFORE any PDF is opened. Default OFF:
    // jbig2dec's decoders have a long memory-safety CVE history and fz_try/
    // fz_catch cannot trap heap corruption, so JBIG2 bytes never reach jbig2dec
    // unless the user has opted in.
    pdfe::core::setJbig2DecodingEnabled(appSettings_->jbig2RenderingEnabled());

    // GitHub #127: opt-in, notify-only version check. Returns immediately
    // unless the user has enabled it in Preferences (default OFF), so a stock
    // install issues no request. It never downloads or executes anything --
    // installing stays a human act via Help > Check for Updates, which is what
    // keeps UC-1..UC-8 (docs/code-signing.md) out of scope. See UC-9.
    maybeCheckForUpdateOnStartup();

    // Pipeline F phase-5: theme manager owns the persisted Light/Dark/System
    // choice and applies it on construction (default: Theme::System).
    themeManager_ = std::make_unique<pdfe::theme::ThemeManager>(
        appSettings_.get(), this);
    themeManager_->applyPersistedTheme();

    // Pipeline E phase-4: image-stamp library (persists across sessions
    // via QSettings).
    imageStampLibrary_ =
        std::make_unique<pdfe::annotations::ImageStampLibrary>();
    imageStampLibrary_->loadFromSettings();

    // Create MuPDF context (shared for app lifetime)
    muPdfContext_ = std::make_shared<pdfe::core::MuPdfContext>();

    // F24 — install the certification (DocMDP) save guard. This is the
    // production consumer for DocMdpEnforcer: from here on, every
    // Document::save / saveAs / saveIncremental in this process consults
    // /Perms /DocMDP before it writes. Installed at context creation so no
    // document can be opened, edited and saved before the guard exists.
    pdfe::signatures::installDocMdpSaveGuard(muPdfContext_);

    // Phase-9 alignment pipeline — queues per-page alignment requests
    // from TextEditToolBar and flushes them at save time.
    alignmentPipeline_ = std::make_unique<pdfe::reflow::AlignmentPipeline>(
        muPdfContext_);

    // Create central canvas
    canvas_ = new PdfCanvas(this);
    canvas_->setObjectName("pdfCanvas");
    canvas_->setFocusPolicy(Qt::StrongFocus);
    canvas_->setAccessibleName(tr("PDF Canvas"));
    canvas_->setAccessibleDescription(tr("PDF document viewer and editor"));

    // GitHub #303 — hand keyboard focus back when a UIA write to a hidden test
    // pipe steals it. Installed HERE because the guard needs the canvas as its
    // fallback target, and this is the first point at which the canvas exists.
    //
    // Unconditional on purpose: #182 records that `PDFE_TESTING` is never set
    // OFF by any build path in this repository, so gating on it would be a
    // comment rather than a control. The guard is inert by construction where
    // no test pipes exist -- `isTestPipe` cannot match a widget that is not
    // there -- so there is nothing to gate.
    testPipeFocusGuard_ = new pdfe::ui::TestPipeFocusGuard(canvas_, this);
    testPipeFocusGuard_->attach();

    // GitHub #60 (SEC-62): the central widget is now a thin vertical container
    // holding the active-content advisory banner ABOVE the canvas, rather than
    // the canvas itself. The container carries no margins or spacing, so with
    // the banner hidden — its state for every document that has no active
    // content — the canvas occupies exactly the pixels it used to.
    centralArea_ = new QWidget(this);
    centralArea_->setObjectName("pdfCentralArea");
    centralArea_->setAccessibleName(tr("Document Area"));
    auto* centralLayout = new QVBoxLayout(centralArea_);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    createProhibitedFeatureBanner();
    centralLayout->addWidget(prohibitedFeatureBanner_, 0);
    centralLayout->addWidget(canvas_, 1);
    setCentralWidget(centralArea_);

    // Pipeline b3f19d72 phase-11: hidden invocation pipe for Python test seam.
    // Pipeline b3d7f942 phase-1 (GAP-15): gated by PDFE_TEST_PIPES env var so
    // the UIA-accessible set pipe is absent in production builds.
    // Python pywinauto tests locate this widget by auto_id "testToolActivatePipe"
    // and write a QAction objectName via set_edit_text(); textChanged triggers
    // activateToolByObjectName, bypassing flaky UIA toolbar-button discovery.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testToolActivatePipe_ = new QLineEdit(this);
        testToolActivatePipe_->setObjectName("testToolActivatePipe");
        testToolActivatePipe_->setAccessibleName("Test Tool Activate Pipe");
        testToolActivatePipe_->resize(0, 0);           // zero-size: invisible to users
        testToolActivatePipe_->setFocusPolicy(Qt::NoFocus);
        testToolActivatePipe_->setMaximumSize(0, 0);
        connect(testToolActivatePipe_, &QLineEdit::textChanged,
                this, [this](const QString& actionName) {
            if (!actionName.isEmpty()) {
                activateToolByObjectName(actionName);
                // Clear immediately so the pipe is ready for the next call.
                testToolActivatePipe_->blockSignals(true);
                testToolActivatePipe_->clear();
                testToolActivatePipe_->blockSignals(false);
            }
        });

        // GitHub #271 / #294 — the acknowledgement half. See the member's
        // declaration for why the trigger's return value cannot serve as one.
        testToolActivateResultPipe_ = new QLineEdit(this);
        testToolActivateResultPipe_->setObjectName("testToolActivateResultPipe");
        testToolActivateResultPipe_->setAccessibleName("Test Tool Activate Result Pipe");
        testToolActivateResultPipe_->resize(0, 0);
        testToolActivateResultPipe_->setFocusPolicy(Qt::NoFocus);
        testToolActivateResultPipe_->setMaximumSize(0, 0);
        testToolActivateResultPipe_->setReadOnly(true);

        // GitHub #271, second half. Measured 2026-10-09: the 1000-annotation
        // stress test's undo loop fires Ctrl+Z 1010 times through real
        // SendInput and removed ZERO of 1000 annotations, while triggering
        // `undoAction` through the pipe removed one immediately. So the
        // keystroke is being swallowed -- GitHub #295, because which widget
        // eats it is a product question -- and the loop could not tell,
        // since it counted keystrokes SENT and nothing else. That is the same
        // defect as #294 one loop further down the same test.
        //
        // "<count>" in, "<performed>:<remaining-enabled>" out. Written as ONE
        // pipe call rather than N, because a UIA round trip costs ~270 ms at
        // 1000 annotations and 1010 of them would be 273 s -- the very cost
        // this issue exists to avoid. The reply is the number of undos that
        // ACTUALLY happened, so the caller asserts on an outcome instead of on
        // its own loop index.
        testUndoRepeatPipe_ = new QLineEdit(this);
        testUndoRepeatPipe_->setObjectName("testUndoRepeatPipe");
        testUndoRepeatPipe_->setAccessibleName("Test Undo Repeat Pipe");
        testUndoRepeatPipe_->resize(0, 0);
        testUndoRepeatPipe_->setFocusPolicy(Qt::NoFocus);
        testUndoRepeatPipe_->setMaximumSize(0, 0);
        connect(testUndoRepeatPipe_, &QLineEdit::textChanged,
                this, [this](const QString& spec) {
            if (spec.isEmpty())
                return;
            testUndoRepeatPipe_->blockSignals(true);
            testUndoRepeatPipe_->clear();
            testUndoRepeatPipe_->blockSignals(false);
            // DEFER, for the same reason activateToolByObjectName does.
            //
            // Running the batch inline blocks this handler, and therefore the
            // UIA ValuePattern::SetValue call that wrote the pipe, for as long
            // as the batch takes. Measured 2026-10-09: 1010 undos took 64.79 s
            // and the write came back with no readable reply, so the caller
            // got "unknown" rather than a count. The test correctly refused to
            // read that as an empty undo stack -- but it is still a dead end.
            //
            // Queued, the write returns at once and the caller polls the
            // result pipe, which it already does on a 300 s budget. The batch
            // itself calls processEvents between operations so the accessibility
            // provider keeps answering those polls while it works.
            QTimer::singleShot(0, this, [this, spec]() {
                runTestUndoRedoRepeat(spec);
            });
        });

        testUndoRepeatResultPipe_ = new QLineEdit(this);
        testUndoRepeatResultPipe_->setObjectName("testUndoRepeatResultPipe");
        testUndoRepeatResultPipe_->setAccessibleName("Test Undo Repeat Result Pipe");
        testUndoRepeatResultPipe_->resize(0, 0);
        testUndoRepeatResultPipe_->setFocusPolicy(Qt::NoFocus);
        testUndoRepeatResultPipe_->setMaximumSize(0, 0);
        testUndoRepeatResultPipe_->setReadOnly(true);
    }

    // Pipeline b3f19d72 phase-12: hidden click-by-pdf-coord invocation pipe.
    // Pipeline b3d7f942 phase-1 (GAP-15): gated by PDFE_TEST_PIPES env var.
    // Python pywinauto tests write "pageIndex,pdfX,pdfY" (e.g. "0,280.0,700.0")
    // via UIA set_edit_text(); textChanged parses the tuple and calls
    // canvas_->clickByPdfCoord(), routing through the normal FormTool
    // hit-test pipeline just as a real user click would.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testClickByPdfCoordPipe_ = new QLineEdit(this);
        testClickByPdfCoordPipe_->setObjectName("testClickByPdfCoordPipe");
        testClickByPdfCoordPipe_->setAccessibleName("Test Click By PDF Coord Pipe");
        testClickByPdfCoordPipe_->resize(0, 0);
        testClickByPdfCoordPipe_->setFocusPolicy(Qt::NoFocus);
        testClickByPdfCoordPipe_->setMaximumSize(0, 0);
        connect(testClickByPdfCoordPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (!payload.isEmpty()) {
                // Parse "pageIndex,pdfX,pdfY".
                const QStringList parts = payload.split(',');
                if (parts.size() == 3) {
                    bool ok1 = false, ok2 = false, ok3 = false;
                    const int   pageIndex = parts[0].trimmed().toInt(&ok1);
                    const qreal pdfX      = parts[1].trimmed().toDouble(&ok2);
                    const qreal pdfY      = parts[2].trimmed().toDouble(&ok3);
                    if (ok1 && ok2 && ok3 && canvas_) {
                        // Pipeline b3f19d72 phase-12: the pipe is used for
                        // test-automation field-selection (interaction mode),
                        // not field creation.  FormTool starts in creation mode
                        // by default (creationFieldType_ == Text), which makes
                        // onMousePress start drawing a new field instead of
                        // hit-testing existing ones.  Force interaction mode
                        // before dispatching the synthetic click.
                        if (auto* ft = dynamic_cast<pdfe::forms::FormTool*>(
                                canvas_->currentTool())) {
                            ft->setCreationFieldType(
                                pdfe::forms::FormFieldType::Unknown);
                        }
                        canvas_->clickByPdfCoord(pageIndex, pdfX, pdfY);
                    }
                }
                // Clear immediately so the pipe is ready for the next call.
                testClickByPdfCoordPipe_->blockSignals(true);
                testClickByPdfCoordPipe_->clear();
                testClickByPdfCoordPipe_->blockSignals(false);
            }
        });
    }

    // Pipeline 7-loop: double-click-by-PDF-coord pipe.  Mirrors the
    // single-click pipe but dispatches PdfCanvas::doubleClickByPdfCoord so
    // a UI test can deterministically trigger TextEditTool word-selection
    // at a PDF coordinate, independent of canvas letterbox margins (the
    // root cause of the phase-7 text-edit no-op-edit failures: screen-
    // fraction double-clicks landed in the gray page margin, never on a
    // span).  Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testDoubleClickByPdfCoordPipe_ = new QLineEdit(this);
        testDoubleClickByPdfCoordPipe_->setObjectName(
            "testDoubleClickByPdfCoordPipe");
        testDoubleClickByPdfCoordPipe_->setAccessibleName(
            "Test Double Click By PDF Coord Pipe");
        testDoubleClickByPdfCoordPipe_->resize(0, 0);
        testDoubleClickByPdfCoordPipe_->setFocusPolicy(Qt::NoFocus);
        testDoubleClickByPdfCoordPipe_->setMaximumSize(0, 0);
        connect(testDoubleClickByPdfCoordPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (!payload.isEmpty()) {
                const QStringList parts = payload.split(',');
                if (parts.size() == 3) {
                    bool ok1 = false, ok2 = false, ok3 = false;
                    const int   pageIndex = parts[0].trimmed().toInt(&ok1);
                    const qreal pdfX      = parts[1].trimmed().toDouble(&ok2);
                    const qreal pdfY      = parts[2].trimmed().toDouble(&ok3);
                    if (ok1 && ok2 && ok3 && canvas_) {
                        canvas_->doubleClickByPdfCoord(pageIndex, pdfX, pdfY);
                    }
                }
                testDoubleClickByPdfCoordPipe_->blockSignals(true);
                testDoubleClickByPdfCoordPipe_->clear();
                testDoubleClickByPdfCoordPipe_->blockSignals(false);
            }
        });
    }

    // Pipeline 7-loop: type-into-text-edit pipe.  Delivers text (or a single
    // Qt::Key via "@KEY:<int>") to the active text-edit tool through the
    // real keyPressEvent path, bypassing OS keyboard delivery to the
    // focusless custom canvas (the second half of the phase-7 text-edit
    // failure: even with a span selected, Win32 typewrite never reached
    // PdfCanvas::keyPressEvent).  Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testTextEditTypePipe_ = new QLineEdit(this);
        testTextEditTypePipe_->setObjectName("testTextEditTypePipe");
        testTextEditTypePipe_->setAccessibleName("Test TextEdit Type Pipe");
        testTextEditTypePipe_->resize(0, 0);
        testTextEditTypePipe_->setFocusPolicy(Qt::NoFocus);
        testTextEditTypePipe_->setMaximumSize(0, 0);
        connect(testTextEditTypePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (!payload.isEmpty() && canvas_) {
                if (payload.startsWith(QLatin1String("@KEY:"))) {
                    bool ok = false;
                    const int qtKey = payload.mid(5).toInt(&ok);
                    if (ok) {
                        canvas_->sendKeyToTextEdit(qtKey);
                    }
                } else {
                    canvas_->typeIntoTextEdit(payload);
                }
            }
            if (testTextEditTypePipe_) {
                testTextEditTypePipe_->blockSignals(true);
                testTextEditTypePipe_->clear();
                testTextEditTypePipe_->blockSignals(false);
            }
        });
    }

    // Pipeline b3f19d72 phase-15: AltText readback pipe pair.
    // Trigger pipe: Python writes any non-empty string to invoke
    // getCurrentAnnotationAltText(); result appears in result pipe.
    // Result pipe: Python reads via UIA ValuePattern (set_value / iface_value).
    // Pipeline 3e7c1a05 phase-1 (security): gated by PDFE_TEST_PIPES env var so
    // neither the SET trigger pipe nor the READ result pipe is present in
    // production builds.  launch.py already sets PDFE_TEST_PIPES=1 for all
    // PDFESession-launched test processes, so the 3 alt-text tests are unaffected.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testAltTextReadbackTriggerPipe_ = new QLineEdit(this);
        testAltTextReadbackTriggerPipe_->setObjectName(
            "testAltTextReadbackTriggerPipe");
        testAltTextReadbackTriggerPipe_->setAccessibleName(
            "Test AltText Readback Trigger");
        testAltTextReadbackTriggerPipe_->resize(0, 0);
        testAltTextReadbackTriggerPipe_->setFocusPolicy(Qt::NoFocus);
        testAltTextReadbackTriggerPipe_->setMaximumSize(0, 0);

        testAltTextReadbackResultPipe_ = new QLineEdit(this);
        testAltTextReadbackResultPipe_->setObjectName(
            "testAltTextReadbackResultPipe");
        testAltTextReadbackResultPipe_->setAccessibleName(
            "Test AltText Readback Result");
        testAltTextReadbackResultPipe_->resize(0, 0);
        testAltTextReadbackResultPipe_->setFocusPolicy(Qt::NoFocus);
        testAltTextReadbackResultPipe_->setMaximumSize(0, 0);
        testAltTextReadbackResultPipe_->setReadOnly(true);

        connect(testAltTextReadbackTriggerPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                // Invoke the Q_INVOKABLE, store result in result pipe.
                QString altText = getCurrentAnnotationAltText();
                testAltTextReadbackResultPipe_->blockSignals(true);
                testAltTextReadbackResultPipe_->setText(altText);
                testAltTextReadbackResultPipe_->blockSignals(false);
                // Clear trigger so the pipe is ready for the next call.
                testAltTextReadbackTriggerPipe_->blockSignals(true);
                testAltTextReadbackTriggerPipe_->clear();
                testAltTextReadbackTriggerPipe_->blockSignals(false);
            }
        });
    }

    // Pipeline 7-loop: hidden two-pipe readback seam for the comment-panel
    // row inventory.  Mirrors the alt-text readback pipe.  Lets Python UI
    // tests read the comment-tree row list from the model instead of
    // enumerating the QTreeWidget via UIA descendants(), which Qt6's UIA
    // bridge on Windows Server 2019 does not reliably surface (viewport-only
    // rows, often zero).  Gated by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testCommentInventoryTriggerPipe_ = new QLineEdit(this);
        testCommentInventoryTriggerPipe_->setObjectName(
            "testCommentInventoryTriggerPipe");
        testCommentInventoryTriggerPipe_->setAccessibleName(
            "Test Comment Inventory Trigger");
        testCommentInventoryTriggerPipe_->resize(0, 0);
        testCommentInventoryTriggerPipe_->setFocusPolicy(Qt::NoFocus);
        testCommentInventoryTriggerPipe_->setMaximumSize(0, 0);

        testCommentInventoryResultPipe_ = new QLineEdit(this);
        testCommentInventoryResultPipe_->setObjectName(
            "testCommentInventoryResultPipe");
        testCommentInventoryResultPipe_->setAccessibleName(
            "Test Comment Inventory Result");
        testCommentInventoryResultPipe_->resize(0, 0);
        testCommentInventoryResultPipe_->setFocusPolicy(Qt::NoFocus);
        testCommentInventoryResultPipe_->setMaximumSize(0, 0);
        testCommentInventoryResultPipe_->setReadOnly(true);

        connect(testCommentInventoryTriggerPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                // The trigger text IS the query.  "*" is the sentinel for
                // "total row count" (empty query); any other value is a
                // substring token (e.g. "[Highlight]").  Using a non-empty
                // sentinel keeps the handler's "ignore empty" guard intact.
                const QString query =
                    (trigger == QLatin1String("*")) ? QString() : trigger;
                const QString result = commentPanelRowCount(query);
                testCommentInventoryResultPipe_->blockSignals(true);
                testCommentInventoryResultPipe_->setText(result);
                testCommentInventoryResultPipe_->blockSignals(false);
                testCommentInventoryTriggerPipe_->blockSignals(true);
                testCommentInventoryTriggerPipe_->clear();
                testCommentInventoryTriggerPipe_->blockSignals(false);
            }
        });
    }

    // Pipeline 7-loop: hidden two-pipe readback seam for the text-edit
    // selection diagnostic.  Mirrors the alt-text readback pipe.  Lets
    // Python UI tests confirm a double-click placed a cursor on a real
    // vector-text span before a typed edit.  Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testTextEditDiagTriggerPipe_ = new QLineEdit(this);
        testTextEditDiagTriggerPipe_->setObjectName(
            "testTextEditDiagTriggerPipe");
        testTextEditDiagTriggerPipe_->setAccessibleName(
            "Test TextEdit Diag Trigger");
        testTextEditDiagTriggerPipe_->resize(0, 0);
        testTextEditDiagTriggerPipe_->setFocusPolicy(Qt::NoFocus);
        testTextEditDiagTriggerPipe_->setMaximumSize(0, 0);

        testTextEditDiagResultPipe_ = new QLineEdit(this);
        testTextEditDiagResultPipe_->setObjectName(
            "testTextEditDiagResultPipe");
        testTextEditDiagResultPipe_->setAccessibleName(
            "Test TextEdit Diag Result");
        testTextEditDiagResultPipe_->resize(0, 0);
        testTextEditDiagResultPipe_->setFocusPolicy(Qt::NoFocus);
        testTextEditDiagResultPipe_->setMaximumSize(0, 0);
        testTextEditDiagResultPipe_->setReadOnly(true);

        connect(testTextEditDiagTriggerPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                const QString result = textEditSelectionDiag();
                testTextEditDiagResultPipe_->blockSignals(true);
                testTextEditDiagResultPipe_->setText(result);
                testTextEditDiagResultPipe_->blockSignals(false);
                testTextEditDiagTriggerPipe_->blockSignals(true);
                testTextEditDiagTriggerPipe_->clear();
                testTextEditDiagTriggerPipe_->blockSignals(false);
            }
        });

        // Pipeline 9a4e2f71 phase-2: write-only seam to drive a font-size
        // change through the toolbar deterministically (the UIA spinbox
        // set-value gesture no-ops in a non-interactive CI runner session —
        // CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2). Python
        // writes a float (points); we forward it to the toolbar's test seam,
        // which fires fontSizeChanged → TextEditTool::applyFontSize. The
        // applied value is read back via textEditSelectionDiag's ";tbsz=".
        testToolbarFontSizePipe_ = new QLineEdit(this);
        testToolbarFontSizePipe_->setObjectName("testToolbarFontSizePipe");
        testToolbarFontSizePipe_->setAccessibleName("Test Toolbar Font Size");
        testToolbarFontSizePipe_->resize(0, 0);
        testToolbarFontSizePipe_->setFocusPolicy(Qt::NoFocus);
        testToolbarFontSizePipe_->setMaximumSize(0, 0);
        connect(testToolbarFontSizePipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (trigger.isEmpty()) {
                return;
            }
            bool ok = false;
            const float sizePt = trigger.toFloat(&ok);
            if (ok && textEditToolBar_) {
                textEditToolBar_->setFontSizeForTesting(sizePt);
            }
            testToolbarFontSizePipe_->blockSignals(true);
            testToolbarFontSizePipe_->clear();
            testToolbarFontSizePipe_->blockSignals(false);
        });

        // Pipeline 9a4e2f71 phase-2: deterministic readback of the
        // TextEditToolBar visibility — ground truth for "is the toolbar shown"
        // that replaces flaky UIA enumeration of its QFontComboBox child
        // (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #1).
        testToolbarVisibleQueryPipe_ = new QLineEdit(this);
        testToolbarVisibleQueryPipe_->setObjectName("testToolbarVisibleQueryPipe");
        testToolbarVisibleQueryPipe_->setAccessibleName("Test Toolbar Visible Query");
        testToolbarVisibleQueryPipe_->resize(0, 0);
        testToolbarVisibleQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testToolbarVisibleQueryPipe_->setMaximumSize(0, 0);
        testToolbarVisibleResultPipe_ = new QLineEdit(this);
        testToolbarVisibleResultPipe_->setObjectName("testToolbarVisibleResultPipe");
        testToolbarVisibleResultPipe_->setAccessibleName("Test Toolbar Visible Result");
        testToolbarVisibleResultPipe_->resize(0, 0);
        testToolbarVisibleResultPipe_->setFocusPolicy(Qt::NoFocus);
        testToolbarVisibleResultPipe_->setMaximumSize(0, 0);
        testToolbarVisibleResultPipe_->setReadOnly(true);
        connect(testToolbarVisibleQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (trigger.isEmpty()) {
                return;
            }
            const QString result =
                (textEditToolBar_ && textEditToolBar_->isVisible())
                    ? QStringLiteral("1") : QStringLiteral("0");
            testToolbarVisibleResultPipe_->blockSignals(true);
            testToolbarVisibleResultPipe_->setText(result);
            testToolbarVisibleResultPipe_->blockSignals(false);
            testToolbarVisibleQueryPipe_->blockSignals(true);
            testToolbarVisibleQueryPipe_->clear();
            testToolbarVisibleQueryPipe_->blockSignals(false);
        });
    }

    // GitHub #85 / #86 (WIN-54 / WIN-55): dock-arrangement readback seam.
    //
    // The persistence tests relaunch pdfe.exe against the same
    // PDFE_SETTINGS_PATH INI and must compare the dock arrangement the
    // second process restored against the one the first process saved.
    // Enumerating QDockWidgets through UIA cannot answer that: a dock that
    // is tabbed BEHIND another is a live, mapped child window, so its UIA
    // pane is present and "visible" either way — exactly the distinction
    // (visible vs raised) the issue asks the test to assert.
    //
    // Writing anything to testDockStateQueryPipe publishes, into
    // testDockStateResultPipe, one record per QDockWidget:
    //
    //     objectName:shown:area:raised[;...]
    //
    //   shown  1 when the dock is not hidden (QDockWidget::isHidden() false)
    //   area   the Qt::DockWidgetArea integer (1 left, 2 right, 4 top,
    //          8 bottom, 0 when floating/undocked)
    //   raised 1 when the dock is the CURRENT tab of its tab group — i.e.
    //          actually painting — which is what QDockWidget's own
    //          visibilityChanged() reports and what "visible/raised" means
    //          for a tabified dock.
    //
    // Records are emitted sorted by objectName so the string compares
    // directly across two processes. Gated by PDFE_TEST_PIPES; the widgets
    // do not exist in a production build. Deliberately uses local pointers
    // captured by the lambda rather than new MainWindow members, so this
    // seam costs no header change.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        auto* dockStateResult = new QLineEdit(this);
        dockStateResult->setObjectName("testDockStateResultPipe");
        dockStateResult->setAccessibleName("Test Dock State Result");
        dockStateResult->resize(0, 0);
        dockStateResult->setFocusPolicy(Qt::NoFocus);
        dockStateResult->setMaximumSize(0, 0);
        dockStateResult->setReadOnly(true);

        auto* dockStateQuery = new QLineEdit(this);
        dockStateQuery->setObjectName("testDockStateQueryPipe");
        dockStateQuery->setAccessibleName("Test Dock State Query");
        dockStateQuery->resize(0, 0);
        dockStateQuery->setFocusPolicy(Qt::NoFocus);
        dockStateQuery->setMaximumSize(0, 0);

        connect(dockStateQuery, &QLineEdit::textChanged, this,
                [this, dockStateQuery, dockStateResult](const QString& trigger) {
            if (trigger.isEmpty()) {
                return;
            }
            QStringList records;
            const QList<QDockWidget*> docks = findChildren<QDockWidget*>();
            for (QDockWidget* dock : docks) {
                if (!dock || dock->objectName().isEmpty()) continue;
                const int area = dock->isFloating()
                    ? 0 : static_cast<int>(dockWidgetArea(dock));
                // "Raised" == currently painting: a tab that is not the
                // current one in its group has an empty visible region even
                // though it is not hidden.
                const bool raised = !dock->isHidden()
                                    && !dock->visibleRegion().isEmpty();
                records << QStringLiteral("%1:%2:%3:%4")
                               .arg(dock->objectName())
                               .arg(dock->isHidden() ? 0 : 1)
                               .arg(area)
                               .arg(raised ? 1 : 0);
            }
            records.sort();
            dockStateResult->blockSignals(true);
            dockStateResult->setText(records.join(QLatin1Char(';')));
            dockStateResult->blockSignals(false);
            dockStateQuery->blockSignals(true);
            dockStateQuery->clear();
            dockStateQuery->blockSignals(false);
        });

        // Companion setter: "objectName:visible[:area]".
        //
        //   objectName  a QDockWidget objectName
        //   visible     1 show, 0 hide
        //   area        optional Qt::DockWidgetArea integer to re-dock into
        //
        // The persistence tests need to ESTABLISH a distinctive arrangement
        // in the first process before closing it. The View-menu toggle
        // actions cannot do that job over testToolActivatePipe:
        // activateToolByObjectName() returns early for an action that is
        // already checked, so it can only ever turn a panel ON, never off,
        // and it cannot re-dock anything. Dragging a dock header between
        // areas is not automatable at all. This setter drives QDockWidget
        // directly; the behaviour under test is still the product's own
        // saveState()/restoreState() round trip through the INI.
        auto* dockSet = new QLineEdit(this);
        dockSet->setObjectName("testDockSetPipe");
        dockSet->setAccessibleName("Test Dock Set Pipe");
        dockSet->resize(0, 0);
        dockSet->setFocusPolicy(Qt::NoFocus);
        dockSet->setMaximumSize(0, 0);
        connect(dockSet, &QLineEdit::textChanged, this,
                [this, dockSet](const QString& request) {
            if (request.isEmpty()) {
                return;
            }
            const QStringList parts = request.split(QLatin1Char(':'));
            if (parts.size() >= 2) {
                QDockWidget* dock = findChild<QDockWidget*>(parts.at(0));
                if (dock) {
                    if (parts.size() >= 3) {
                        bool ok = false;
                        const int area = parts.at(2).toInt(&ok);
                        if (ok && area != 0) {
                            addDockWidget(
                                static_cast<Qt::DockWidgetArea>(area), dock);
                        }
                    }
                    dock->setVisible(parts.at(1) != QLatin1String("0"));
                    if (parts.at(1) != QLatin1String("0")) {
                        dock->raise();
                    }
                }
            }
            dockSet->blockSignals(true);
            dockSet->clear();
            dockSet->blockSignals(false);
        });
    }

    // Pipeline e7d3c2a1 phase-2: render-oracle active-tool readback seam.
    // Mirrors the text-edit diag pipe.  Lets render-verification tests assert
    // the per-region tool routing decision (defect #4: page-wide
    // hasTextContent() routes the WHOLE page to span- vs region-tool).  Gated
    // by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testActiveToolQueryPipe_ = new QLineEdit(this);
        testActiveToolQueryPipe_->setObjectName("testActiveToolQueryPipe");
        testActiveToolQueryPipe_->setAccessibleName("Test Active Tool Query");
        testActiveToolQueryPipe_->resize(0, 0);
        testActiveToolQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testActiveToolQueryPipe_->setMaximumSize(0, 0);

        testActiveToolResultPipe_ = new QLineEdit(this);
        testActiveToolResultPipe_->setObjectName("testActiveToolResultPipe");
        testActiveToolResultPipe_->setAccessibleName("Test Active Tool Result");
        testActiveToolResultPipe_->resize(0, 0);
        testActiveToolResultPipe_->setFocusPolicy(Qt::NoFocus);
        testActiveToolResultPipe_->setMaximumSize(0, 0);
        testActiveToolResultPipe_->setReadOnly(true);

        connect(testActiveToolQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                const QString result = activeToolDiag();
                testActiveToolResultPipe_->blockSignals(true);
                testActiveToolResultPipe_->setText(result);
                testActiveToolResultPipe_->blockSignals(false);
                testActiveToolQueryPipe_->blockSignals(true);
                testActiveToolQueryPipe_->clear();
                testActiveToolQueryPipe_->blockSignals(false);
            }
        });
    }

    // GitHub #295 — Ctrl+Z shortcut-dispatch trace. Write "dump" to read the
    // records accumulated so far (';'-separated, oldest first, "empty" when
    // none), or "clear" to start a fresh observation window.
    //
    // The filter goes on qApp rather than on this window; see ShortcutTraceFilter.
    // Gated by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testKeyTraceQueryPipe_ = new QLineEdit(this);
        testKeyTraceQueryPipe_->setObjectName("testKeyTraceQueryPipe");
        testKeyTraceQueryPipe_->setAccessibleName("Test Key Trace Query");
        testKeyTraceQueryPipe_->resize(0, 0);
        testKeyTraceQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testKeyTraceQueryPipe_->setMaximumSize(0, 0);

        testKeyTraceResultPipe_ = new QLineEdit(this);
        testKeyTraceResultPipe_->setObjectName("testKeyTraceResultPipe");
        testKeyTraceResultPipe_->setAccessibleName("Test Key Trace Result");
        testKeyTraceResultPipe_->resize(0, 0);
        testKeyTraceResultPipe_->setFocusPolicy(Qt::NoFocus);
        testKeyTraceResultPipe_->setMaximumSize(0, 0);
        testKeyTraceResultPipe_->setReadOnly(true);

        connect(testKeyTraceQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (trigger.isEmpty()) return;
            QString result;
            if (trigger == QLatin1String("clear")) {
                testKeyTrace_.clear();
                result = QStringLiteral("cleared");
            } else if (trigger == QLatin1String("all")) {
                testKeyTraceAll_ = true;
                testKeyTrace_.clear();
                result = QStringLiteral("recording all key events");
            } else if (trigger == QLatin1String("ctrlz")) {
                testKeyTraceAll_ = false;
                testKeyTrace_.clear();
                result = QStringLiteral("recording ctrl+z only");
            } else {
                // "empty" rather than "" so a test can tell "no Ctrl+Z was
                // ever seen" from "the pipe did not answer" — the two look
                // identical through an empty result field, and that ambiguity
                // is what made #295 hard to pin down in the first place.
                result = testKeyTrace_.isEmpty()
                             ? QStringLiteral("empty")
                             : testKeyTrace_.join(QLatin1Char(';'));
            }
            testKeyTraceResultPipe_->blockSignals(true);
            testKeyTraceResultPipe_->setText(result);
            testKeyTraceResultPipe_->blockSignals(false);
            testKeyTraceQueryPipe_->blockSignals(true);
            testKeyTraceQueryPipe_->clear();
            testKeyTraceQueryPipe_->blockSignals(false);
        });

        testKeyTraceFilter_ = new ShortcutTraceFilter(this);
        if (qApp) qApp->installEventFilter(testKeyTraceFilter_);
    }

    // Zoom-set seam. Write a percentage (e.g. "25") to testSetZoomPipe.
    //
    // Exists because PDFE FITS THE PAGE TO THE VIEWPORT: a 180x240pt page and
    // a 612x792pt page both render at ~58-60% of the canvas width, so page
    // size cannot be used to control how much of the canvas a page covers.
    // That matters for testing canvas geometry -- a helper that asks the app
    // where the page is can only be shown to beat a canvas-fraction guess on
    // a page that does NOT fill the canvas, and zoom is the only lever that
    // produces one. Gated by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSetZoomPipe_ = new QLineEdit(this);
        testSetZoomPipe_->setObjectName("testSetZoomPipe");
        testSetZoomPipe_->setAccessibleName("Test Set Zoom Pipe");
        testSetZoomPipe_->resize(0, 0);
        testSetZoomPipe_->setFocusPolicy(Qt::NoFocus);
        testSetZoomPipe_->setMaximumSize(0, 0);
        connect(testSetZoomPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            bool ok = false;
            const double pct = payload.trimmed().toDouble(&ok);
            if (ok && canvas_ && pct >= 5.0 && pct <= 1600.0) {
                canvas_->setZoom(static_cast<float>(pct / 100.0));
            }
            testSetZoomPipe_->blockSignals(true);
            testSetZoomPipe_->clear();
            testSetZoomPipe_->blockSignals(false);
        });
    }

    // View-state readback seam. Write any token to testViewStateQueryPipe;
    // the handler answers "page=<n>,scrollY=<n>,annots=<n>,selected=<n>" on
    // testViewStateResultPipe.
    //
    // Exists because a large family of canvas tests asserted only
    // `session.proc.poll() is None` -- that the process had not crashed. Such
    // a test passes whether or not the gesture did anything at all, including
    // when the click lands off the page entirely. These four values are the
    // observable state those gestures are supposed to change:
    //
    //   page      scrolling / page navigation moved the view
    //   scrollY   a drag-scroll actually scrolled
    //   annots    a click placed (or did not place) an annotation
    //   selected  a click selected, and a click-away deselected
    //
    // Gated by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testViewStateQueryPipe_ = new QLineEdit(this);
        testViewStateQueryPipe_->setObjectName("testViewStateQueryPipe");
        testViewStateQueryPipe_->setAccessibleName("Test View State Query");
        testViewStateQueryPipe_->resize(0, 0);
        testViewStateQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testViewStateQueryPipe_->setMaximumSize(0, 0);

        testViewStateResultPipe_ = new QLineEdit(this);
        testViewStateResultPipe_->setObjectName("testViewStateResultPipe");
        testViewStateResultPipe_->setAccessibleName("Test View State Result");
        testViewStateResultPipe_->resize(0, 0);
        testViewStateResultPipe_->setFocusPolicy(Qt::NoFocus);
        testViewStateResultPipe_->setMaximumSize(0, 0);
        testViewStateResultPipe_->setReadOnly(true);

        connect(testViewStateQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (trigger.isEmpty()) return;
            int page = -1, scrollY = -1, annots = -1, selected = -1;
            if (canvas_) {
                page = canvas_->currentPage();
                scrollY = canvas_->scrollOffsetY();
                selected = currentSelectedAnnotationIndex();
                if (document_ && annotationManager_ && page >= 0
                    && page < document_->pageCount()) {
                    if (auto pg = document_->page(page)) {
                        annots = static_cast<int>(
                            annotationManager_->getAnnotations(pg.get()).size());
                    }
                }
            }
            // scrollX: a horizontal-pan test cannot assert it panned without
            // it. selCount: a rubber-band test cannot assert it SELECTED
            // anything without it. rect: a move/resize test cannot assert the
            // annotation actually moved or changed size without it.
            int scrollX = -1, selCount = -1;
            QString rect = QStringLiteral("none");
            if (canvas_) {
                scrollX = canvas_->scrollOffsetX();
                selCount = canvas_->selectedAnnotationCount();
                if (document_ && annotationManager_ && selected >= 0
                    && page >= 0 && page < document_->pageCount()) {
                    if (auto pg = document_->page(page)) {
                        const auto all =
                            annotationManager_->getAnnotations(pg.get());
                        if (selected < static_cast<int>(all.size())) {
                            const QRectF r = all[selected].rect;
                            rect = QStringLiteral("%1 %2 %3 %4")
                                       .arg(qRound(r.x())).arg(qRound(r.y()))
                                       .arg(qRound(r.width()))
                                       .arg(qRound(r.height()));
                        }
                    }
                }
            }
            // GitHub #79 / WIN-42. The keyboard-shortcut catalog asserted
            // `alive_safe` — "the process is still running" — for the rotate
            // rows. That passes for a Rotate action that does nothing at all.
            // viewRot is the very value the
            // QInvokableSeams.PdfCanvas_ViewRotation_ReflectsRotateViewCW
            // gtest reads (PdfCanvas::viewRotation(), Q_INVOKABLE); pageRot
            // is its per-page counterpart for Rotate Page CW/CCW, which
            // changes the page's /Rotate rather than the view transform.
            // Surfaced here rather than through a new pipe pair so the live
            // UI reads the SAME seam the unit test pins.
            int viewRot = -1, pageRot = -1;
            if (canvas_) {
                viewRot = canvas_->viewRotation();
                if (document_ && page >= 0 && page < document_->pageCount()) {
                    if (auto pg = document_->page(page)) {
                        pageRot = pg->rotation();
                    }
                }
            }
            const QString result = QStringLiteral(
                "page=%1,scrollY=%2,annots=%3,selected=%4,"
                "scrollX=%5,selCount=%6,viewRot=%7,pageRot=%8,rect=%9")
                .arg(page).arg(scrollY).arg(annots).arg(selected)
                .arg(scrollX).arg(selCount).arg(viewRot).arg(pageRot)
                .arg(rect);
            testViewStateResultPipe_->blockSignals(true);
            testViewStateResultPipe_->setText(result);
            testViewStateResultPipe_->blockSignals(false);
            testViewStateQueryPipe_->blockSignals(true);
            testViewStateQueryPipe_->clear();
            testViewStateQueryPipe_->blockSignals(false);
        });
    }

    // Page-rect readback seam. Write a page index to testPageRectQueryPipe;
    // the handler answers "x,y,w,h" in CANVAS-WIDGET coordinates on
    // testPageRectResultPipe, or "none".
    //
    // Exists because a UI test that wants to drag a region on a page has to
    // know where that page actually is, and every way of guessing it was
    // wrong: a screenshot disagreed with the UIA canvas rect by ~145px (DPI),
    // and a fraction-of-the-canvas guess put the press at PDF x = -30.7 --
    // off the left edge of the page. The app already computes this exactly,
    // in getPageScreenRect(); this just lets a test ask instead of guess.
    // Gated by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testPageRectQueryPipe_ = new QLineEdit(this);
        testPageRectQueryPipe_->setObjectName("testPageRectQueryPipe");
        testPageRectQueryPipe_->setAccessibleName("Test Page Rect Query");
        testPageRectQueryPipe_->resize(0, 0);
        testPageRectQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testPageRectQueryPipe_->setMaximumSize(0, 0);

        testPageRectResultPipe_ = new QLineEdit(this);
        testPageRectResultPipe_->setObjectName("testPageRectResultPipe");
        testPageRectResultPipe_->setAccessibleName("Test Page Rect Result");
        testPageRectResultPipe_->resize(0, 0);
        testPageRectResultPipe_->setFocusPolicy(Qt::NoFocus);
        testPageRectResultPipe_->setMaximumSize(0, 0);
        testPageRectResultPipe_->setReadOnly(true);

        connect(testPageRectQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            bool ok = false;
            const int idx = payload.trimmed().toInt(&ok);
            QString result = QStringLiteral("none");
            if (ok && canvas_) {
                QPoint origin;
                int w = 0, h = 0;
                if (canvas_->getPageScreenRect(idx, origin, w, h)) {
                    result = QStringLiteral("%1,%2,%3,%4")
                                 .arg(origin.x()).arg(origin.y()).arg(w).arg(h);
                }
            }
            testPageRectResultPipe_->blockSignals(true);
            testPageRectResultPipe_->setText(result);
            testPageRectResultPipe_->blockSignals(false);
            testPageRectQueryPipe_->blockSignals(true);
            testPageRectQueryPipe_->clear();
            testPageRectQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline e7d3c2a1 phase-3: OCR-text readback seam.  Write any non-empty
    // token to testOcrTextQueryPipe; the handler runs currentPageOcrText()
    // (which OCRs the current page via ocrPipeline_) and writes the recognized
    // plain text to testOcrTextResultPipe.  Used by the multilingual #3 render
    // test to assert the Arabic block OCRs to Arabic-script characters.  Gated
    // by PDFE_TEST_PIPES; absent in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testOcrTextQueryPipe_ = new QLineEdit(this);
        testOcrTextQueryPipe_->setObjectName("testOcrTextQueryPipe");
        testOcrTextQueryPipe_->setAccessibleName("Test OCR Text Query");
        testOcrTextQueryPipe_->resize(0, 0);
        testOcrTextQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testOcrTextQueryPipe_->setMaximumSize(0, 0);

        testOcrTextResultPipe_ = new QLineEdit(this);
        testOcrTextResultPipe_->setObjectName("testOcrTextResultPipe");
        testOcrTextResultPipe_->setAccessibleName("Test OCR Text Result");
        testOcrTextResultPipe_->resize(0, 0);
        testOcrTextResultPipe_->setFocusPolicy(Qt::NoFocus);
        testOcrTextResultPipe_->setMaximumSize(0, 0);
        testOcrTextResultPipe_->setReadOnly(true);

        connect(testOcrTextQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                const QString result = currentPageOcrText();
                testOcrTextResultPipe_->blockSignals(true);
                testOcrTextResultPipe_->setText(
                    result.isEmpty() ? QStringLiteral("__null__") : result);
                testOcrTextResultPipe_->blockSignals(false);
                testOcrTextQueryPipe_->blockSignals(true);
                testOcrTextQueryPipe_->clear();
                testOcrTextQueryPipe_->blockSignals(false);
            }
        });
    }

    // Pipeline f2a8c3e1 phase-2: annotation property pipe seams.
    // These hidden QLineEdit widgets let Python pywinauto tests set annotation
    // selection and apply property changes directly to the model, bypassing:
    //   • QColorDialog UIA dance (race: dialog not in UIA tree until next event tick)
    //   • UIA ValuePattern on QLineEdit/QTextEdit (race: textChanged is queued)
    //   • SelectTool canvas click hit-test (non-deterministic for text-markup annots
    //     because the hit-box is narrow and canvas scaling depends on render state)
    //
    // All pipes are gated by PDFE_TEST_ANNOT_PIPES=1; inert in production.
    //
    // testAnnotSelectPipe_: write "pageIdx,annotIdx" to set selectedAnnotation*
    //   and populate annotationProperties_ from the live annotationManager_.
    //   This is the deterministic substitute for canvas-click annotation selection.
    // testAnnotColorPipe_: write "R,G,B" to set color on the selected annotation.
    // testAnnotAuthorPipe_: write author string to set author on selected annotation.
    // testAnnotContentsPipe_: write contents string to set contents on selected annotation.
    //
    // Each handler reads selectedAnnotationPageIndex_ / selectedAnnotationIndex_,
    // reads fresh data from annotationManager_ (no panel cache dependency), then
    // calls onAnnotationPropertiesChanged() — the same code path the UI takes.
    if (qgetenv("PDFE_TEST_ANNOT_PIPES") == "1") {
        // testAnnotSelectPipe_: write "pageIdx,annotIdx" to select an annotation
        // by index directly, bypassing the flaky SelectTool canvas-click hit-test.
        // After selection, annotationProperties_ is populated and the dock is raised.
        testAnnotSelectPipe_ = new QLineEdit(this);
        testAnnotSelectPipe_->setObjectName("testAnnotSelectPipe");
        testAnnotSelectPipe_->setAccessibleName("Test Annot Select Pipe");
        testAnnotSelectPipe_->resize(0, 0);
        testAnnotSelectPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotSelectPipe_->setMaximumSize(0, 0);
        connect(testAnnotSelectPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 2) {
                bool ok1 = false, ok2 = false;
                const int pi = parts[0].trimmed().toInt(&ok1);
                const int ai = parts[1].trimmed().toInt(&ok2);
                if (ok1 && ok2 && pi >= 0 && ai >= 0 && document_ && annotationManager_) {
                    auto page = document_->page(pi);
                    if (page) {
                        auto data = annotationManager_->getAnnotation(page.get(), ai);
                        data.pageIndex  = pi;
                        data.annotIndex = ai;
                        selectedAnnotationPageIndex_ = pi;
                        selectedAnnotationIndex_ = ai;
                        // c82a1f37 P4: also drive the canvas SelectTool's own
                        // selection state so resize/rotate handles go live for
                        // a subsequent testCanvasDragPipe drag (the handle seam
                        // reads SelectTool, not this panel-level selection).
                        if (canvas_) {
                            canvas_->selectAnnotationInSelectTool(pi, ai);
                        }
                        if (annotationProperties_) {
                            annotationProperties_->setAnnotation(data);
                        }
                        if (annotationPropertiesDock_) {
                            annotationPropertiesDock_->show();
                            annotationPropertiesDock_->raise();
                        }
                        if (openAltTextDialogAction_) {
                            openAltTextDialogAction_->setEnabled(true);
                        }
                    }
                }
            }
            testAnnotSelectPipe_->blockSignals(true);
            testAnnotSelectPipe_->clear();
            testAnnotSelectPipe_->blockSignals(false);
        });

        testAnnotColorPipe_ = new QLineEdit(this);
        testAnnotColorPipe_->setObjectName("testAnnotColorPipe");
        testAnnotColorPipe_->setAccessibleName("Test Annot Color Pipe");
        testAnnotColorPipe_->resize(0, 0);
        testAnnotColorPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotColorPipe_->setMaximumSize(0, 0);
        connect(testAnnotColorPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            // Parse "R,G,B"
            const QStringList parts = payload.split(',');
            if (parts.size() == 3) {
                bool ok1 = false, ok2 = false, ok3 = false;
                const int r = parts[0].trimmed().toInt(&ok1);
                const int g = parts[1].trimmed().toInt(&ok2);
                const int b = parts[2].trimmed().toInt(&ok3);
                if (ok1 && ok2 && ok3 &&
                    document_ && annotationManager_ &&
                    selectedAnnotationPageIndex_ >= 0 &&
                    selectedAnnotationIndex_ >= 0)
                {
                    // Read fresh annotation data directly from the model so we
                    // don't depend on the panel's cached state being up-to-date.
                    auto page = document_->page(selectedAnnotationPageIndex_);
                    if (page) {
                        pdfe::annotations::AnnotationData data =
                            annotationManager_->getAnnotation(
                                page.get(), selectedAnnotationIndex_);
                        data.pageIndex  = selectedAnnotationPageIndex_;
                        data.annotIndex = selectedAnnotationIndex_;
                        data.color = QColor(r, g, b);
                        if (annotationProperties_) {
                            annotationProperties_->setAnnotation(data);
                        }
                        onAnnotationPropertiesChanged(data);
                    }
                }
            }
            testAnnotColorPipe_->blockSignals(true);
            testAnnotColorPipe_->clear();
            testAnnotColorPipe_->blockSignals(false);
        });

        testAnnotAuthorPipe_ = new QLineEdit(this);
        testAnnotAuthorPipe_->setObjectName("testAnnotAuthorPipe");
        testAnnotAuthorPipe_->setAccessibleName("Test Annot Author Pipe");
        testAnnotAuthorPipe_->resize(0, 0);
        testAnnotAuthorPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotAuthorPipe_->setMaximumSize(0, 0);
        connect(testAnnotAuthorPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            if (document_ && annotationManager_ &&
                selectedAnnotationPageIndex_ >= 0 &&
                selectedAnnotationIndex_ >= 0)
            {
                auto page = document_->page(selectedAnnotationPageIndex_);
                if (page) {
                    pdfe::annotations::AnnotationData data =
                        annotationManager_->getAnnotation(
                            page.get(), selectedAnnotationIndex_);
                    data.pageIndex  = selectedAnnotationPageIndex_;
                    data.annotIndex = selectedAnnotationIndex_;
                    data.author = payload.toStdString();
                    if (annotationProperties_) {
                        annotationProperties_->setAnnotation(data);
                    }
                    onAnnotationPropertiesChanged(data);
                }
            }
            testAnnotAuthorPipe_->blockSignals(true);
            testAnnotAuthorPipe_->clear();
            testAnnotAuthorPipe_->blockSignals(false);
        });

        testAnnotContentsPipe_ = new QLineEdit(this);
        testAnnotContentsPipe_->setObjectName("testAnnotContentsPipe");
        testAnnotContentsPipe_->setAccessibleName("Test Annot Contents Pipe");
        testAnnotContentsPipe_->resize(0, 0);
        testAnnotContentsPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotContentsPipe_->setMaximumSize(0, 0);
        connect(testAnnotContentsPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            if (document_ && annotationManager_ &&
                selectedAnnotationPageIndex_ >= 0 &&
                selectedAnnotationIndex_ >= 0)
            {
                auto page = document_->page(selectedAnnotationPageIndex_);
                if (page) {
                    pdfe::annotations::AnnotationData data =
                        annotationManager_->getAnnotation(
                            page.get(), selectedAnnotationIndex_);
                    data.pageIndex  = selectedAnnotationPageIndex_;
                    data.annotIndex = selectedAnnotationIndex_;
                    data.contents = payload.toStdString();
                    if (annotationProperties_) {
                        annotationProperties_->setAnnotation(data);
                    }
                    onAnnotationPropertiesChanged(data);
                }
            }
            testAnnotContentsPipe_->blockSignals(true);
            testAnnotContentsPipe_->clear();
            testAnnotContentsPipe_->blockSignals(false);
        });

        // Pipeline c4a9e271 phase-6: deterministic annotation-DELETE seam.
        // Writing any non-empty token deletes the currently-selected annotation
        // via deleteSelectedAnnotation() — the same Q_INVOKABLE the GoogleTest
        // suite uses. This replaces the unreliable synthetic-click + Delete-key
        // flow (Qt6 spontaneity barrier; see P-GAP-75) for the native-UI
        // batch-2 delete scenario, which drives PDFE purely through pipes and
        // cannot synthesise a spontaneous mouse selection for the SelectTool.
        // Gated by PDFE_TEST_ANNOT_PIPES; inert in production.
        testAnnotDeletePipe_ = new QLineEdit(this);
        testAnnotDeletePipe_->setObjectName("testAnnotDeletePipe");
        testAnnotDeletePipe_->setAccessibleName("Test Annot Delete Pipe");
        testAnnotDeletePipe_->resize(0, 0);
        testAnnotDeletePipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotDeletePipe_->setMaximumSize(0, 0);
        connect(testAnnotDeletePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            deleteSelectedAnnotation();
            testAnnotDeletePipe_->blockSignals(true);
            testAnnotDeletePipe_->clear();
            testAnnotDeletePipe_->blockSignals(false);
        });

        // c82a1f37 test-hardening: deterministic annotation CREATE seam. Python
        // writes "type,page,x,y,w,h" (type ∈ Text/FreeText/Highlight/Underline/
        // StrikeOut/Square/Circle/Line/Ink/Stamp; coords in fitz points). Invokes
        // createAnnotationForTest() — the same CreateAnnotationCommand path a tool
        // create takes — so an annotation is committed + the comment panel
        // refreshed deterministically. Replaces the synthetic-click create that
        // does NOT reliably commit (P4 finding: sendEvent creates don't commit;
        // real-mouse is timing-fragile → the CI flake in
        // test_sticky_note_appears_in_comment_panel). Gated by
        // PDFE_TEST_ANNOT_PIPES; inert in production.
        testAnnotCreatePipe_ = new QLineEdit(this);
        testAnnotCreatePipe_->setObjectName("testAnnotCreatePipe");
        testAnnotCreatePipe_->setAccessibleName("Test Annot Create Pipe");
        testAnnotCreatePipe_->resize(0, 0);
        testAnnotCreatePipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotCreatePipe_->setMaximumSize(0, 0);
        connect(testAnnotCreatePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (qgetenv("PDFE_TEST_ANNOT_PIPES") != "1") return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 6) {
                static const QHash<QString, pdfe::annotations::AnnotationType> typeMap = {
                    {"Text",      pdfe::annotations::AnnotationType::Text},
                    {"FreeText",  pdfe::annotations::AnnotationType::FreeText},
                    {"Highlight", pdfe::annotations::AnnotationType::Highlight},
                    {"Underline", pdfe::annotations::AnnotationType::Underline},
                    {"StrikeOut", pdfe::annotations::AnnotationType::StrikeOut},
                    {"Square",    pdfe::annotations::AnnotationType::Square},
                    {"Circle",    pdfe::annotations::AnnotationType::Circle},
                    {"Line",      pdfe::annotations::AnnotationType::Line},
                    {"Ink",       pdfe::annotations::AnnotationType::Ink},
                    {"Stamp",     pdfe::annotations::AnnotationType::Stamp},
                };
                const auto it = typeMap.constFind(parts[0].trimmed());
                bool okp = false, okx = false, oky = false, okw = false, okh = false;
                const int page = parts[1].trimmed().toInt(&okp);
                const double x = parts[2].trimmed().toDouble(&okx);
                const double y = parts[3].trimmed().toDouble(&oky);
                const double w = parts[4].trimmed().toDouble(&okw);
                const double h = parts[5].trimmed().toDouble(&okh);
                if (it != typeMap.constEnd() && okp && okx && oky && okw && okh
                    && page >= 0) {
                    createAnnotationForTest(page, it.value(), QRectF(x, y, w, h));
                }
            }
            testAnnotCreatePipe_->blockSignals(true);
            testAnnotCreatePipe_->clear();
            testAnnotCreatePipe_->blockSignals(false);
        });
    }

    // Pipeline c91e7f3a phase-3: TextEditToolBar fontComboBox readback + set pipes.
    // Pipeline b3d7f942 phase-1 (GAP-15): gated by PDFE_TEST_PIPES env var so
    // these UIA-accessible set pipes are absent in production builds.
    // Qt6's UIA bridge does not reliably expose QFontComboBox contents when the
    // toolbar is mounted via addToolBar() on Windows Server 2019.  These three
    // pipes bridge the gap: query→result for enumeration, and a setter pipe for
    // programmatic font selection.
    //
    // NOTE: textEditToolBar_ is created in createToolBar() (called below).  The
    // pipes are created here (MainWindow ctor body) but the handlers access
    // textEditToolBar_ at call-time — it is always initialised before any test
    // can write to these pipes.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testFontFamiliesResultPipe_ = new QLineEdit(this);
        testFontFamiliesResultPipe_->setObjectName("testFontFamiliesResultPipe");
        testFontFamiliesResultPipe_->setAccessibleName("Test Font Families Result");
        testFontFamiliesResultPipe_->resize(0, 0);
        testFontFamiliesResultPipe_->setFocusPolicy(Qt::NoFocus);
        testFontFamiliesResultPipe_->setMaximumSize(0, 0);
        testFontFamiliesResultPipe_->setReadOnly(true);

        testFontFamiliesQueryPipe_ = new QLineEdit(this);
        testFontFamiliesQueryPipe_->setObjectName("testFontFamiliesQueryPipe");
        testFontFamiliesQueryPipe_->setAccessibleName("Test Font Families Query");
        testFontFamiliesQueryPipe_->resize(0, 0);
        testFontFamiliesQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testFontFamiliesQueryPipe_->setMaximumSize(0, 0);
        connect(testFontFamiliesQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (trigger.isEmpty()) return;
            QStringList entries;
            if (textEditToolBar_) {
                // Walk the toolbar's children looking for the QFontComboBox.
                auto* combo = textEditToolBar_->findChild<QFontComboBox*>(
                    "fontComboBox");
                if (!combo) {
                    // Fall back to the first QFontComboBox child.
                    combo = textEditToolBar_->findChild<QFontComboBox*>();
                }
                if (combo) {
                    // First line: current selection.
                    entries.append(combo->currentText());
                    // Remaining lines: all items in order.
                    for (int i = 0; i < combo->count(); ++i) {
                        entries.append(combo->itemText(i));
                    }
                }
            }
            // Use '|' as separator — QLineEdit strips '\n' from text.
            // Python splits on '|' to recover the list.
            testFontFamiliesResultPipe_->blockSignals(true);
            testFontFamiliesResultPipe_->setText(entries.join('|'));
            testFontFamiliesResultPipe_->blockSignals(false);
            // Clear trigger.
            testFontFamiliesQueryPipe_->blockSignals(true);
            testFontFamiliesQueryPipe_->clear();
            testFontFamiliesQueryPipe_->blockSignals(false);
        });

        testSetFontFamilyPipe_ = new QLineEdit(this);
        testSetFontFamilyPipe_->setObjectName("testSetFontFamilyPipe");
        testSetFontFamilyPipe_->setAccessibleName("Test Set Font Family Pipe");
        testSetFontFamilyPipe_->resize(0, 0);
        testSetFontFamilyPipe_->setFocusPolicy(Qt::NoFocus);
        testSetFontFamilyPipe_->setMaximumSize(0, 0);
        connect(testSetFontFamilyPipe_, &QLineEdit::textChanged,
                this, [this](const QString& family) {
            if (family.isEmpty()) return;
            if (textEditToolBar_) {
                auto* combo = textEditToolBar_->findChild<QFontComboBox*>(
                    "fontComboBox");
                if (!combo) {
                    combo = textEditToolBar_->findChild<QFontComboBox*>();
                }
                if (combo) {
                    combo->setCurrentText(family);
                    // Emit the signal so the font change propagates exactly as
                    // if the user had selected the font from the dropdown.
                    emit textEditToolBar_->fontFamilyChanged(combo->currentText());
                }
            }
            testSetFontFamilyPipe_->blockSignals(true);
            testSetFontFamilyPipe_->clear();
            testSetFontFamilyPipe_->blockSignals(false);
        });

        // Pipeline c94e2f17 phase-4.5 (GAP-61): context-menu QAction
        // discovery query/result pipes. Qt6's UIA bridge does not expose
        // synchronous QMenu::exec popups, so right-click→enumerate is
        // structurally broken. These pipes give the test framework a
        // direct query path that bypasses UIA entirely.
        //
        // Protocol: write a panel objectName (e.g. "pagePanel") to
        // testContextActionsQueryPipe; the handler walks that panel's
        // QAction children via findChildren<QAction*>() and writes a
        // '|'-separated list of their objectNames to
        // testContextActionsResultPipe.
        testContextActionsResultPipe_ = new QLineEdit(this);
        testContextActionsResultPipe_->setObjectName("testContextActionsResultPipe");
        testContextActionsResultPipe_->setAccessibleName("Test Context Actions Result");
        testContextActionsResultPipe_->resize(0, 0);
        testContextActionsResultPipe_->setFocusPolicy(Qt::NoFocus);
        testContextActionsResultPipe_->setMaximumSize(0, 0);
        testContextActionsResultPipe_->setReadOnly(true);

        testContextActionsQueryPipe_ = new QLineEdit(this);
        testContextActionsQueryPipe_->setObjectName("testContextActionsQueryPipe");
        testContextActionsQueryPipe_->setAccessibleName("Test Context Actions Query");
        testContextActionsQueryPipe_->resize(0, 0);
        testContextActionsQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testContextActionsQueryPipe_->setMaximumSize(0, 0);
        connect(testContextActionsQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& panelName) {
            if (panelName.isEmpty()) return;
            QStringList names;
            // Find the named panel widget under MainWindow. Panel
            // objectNames: pagePanel, bookmarkPanel, commentPanel.
            QWidget* panel = this->findChild<QWidget*>(panelName);
            if (panel) {
                // Walk the panel's QAction children (member actions
                // promoted by GAP-58 fix, registered via addAction()
                // by GAP-61 mitigation). Excludes anonymous (no
                // objectName) actions to keep the result clean.
                for (QAction* a : panel->findChildren<QAction*>()) {
                    QString n = a->objectName();
                    if (!n.isEmpty()) {
                        names.append(n);
                    }
                }
            }
            testContextActionsResultPipe_->blockSignals(true);
            testContextActionsResultPipe_->setText(names.join('|'));
            testContextActionsResultPipe_->blockSignals(false);
            // Clear trigger.
            testContextActionsQueryPipe_->blockSignals(true);
            testContextActionsQueryPipe_->clear();
            testContextActionsQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline c91e7f3a phase-5: form field value set pipe.
    // Pipeline c91e7f3a phase-7: form field value read-back pipe pair.
    // Pipeline b3d7f942 phase-1 (GAP-15): both gated by PDFE_TEST_PIPES env var
    // so these UIA-accessible set/query pipes are absent in production builds.
    // Python tests write a desired value string; the handler calls
    // formPropertiesPanel_->setTextValueForTesting(value), which directly
    // fires fieldValueChanged → onFormFieldValueChanged creates a
    // SetFieldValueCommand — bypassing the UIA ValuePattern → editingFinished
    // race that leaves the field uncommitted.
    //
    // NOTE: formPropertiesPanel_ is created in createDockPanels() (below).
    // The pipe is created here; the handler accesses formPropertiesPanel_ at
    // call-time — always initialised before any test can write to the pipe.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testFormFieldValueSetPipe_ = new QLineEdit(this);
        testFormFieldValueSetPipe_->setObjectName("testFormFieldValueSetPipe");
        testFormFieldValueSetPipe_->setAccessibleName("Test Form Field Value Set Pipe");
        testFormFieldValueSetPipe_->resize(0, 0);
        testFormFieldValueSetPipe_->setFocusPolicy(Qt::NoFocus);
        testFormFieldValueSetPipe_->setMaximumSize(0, 0);
        connect(testFormFieldValueSetPipe_, &QLineEdit::textChanged,
                this, [this](const QString& value) {
            if (value.isEmpty()) return;
            if (formPropertiesPanel_) {
                formPropertiesPanel_->setTextValueForTesting(value);
            }
            testFormFieldValueSetPipe_->blockSignals(true);
            testFormFieldValueSetPipe_->clear();
            testFormFieldValueSetPipe_->blockSignals(false);
        });

        // Pipeline c91e7f3a phase-7: form field value read-back pipe pair.
        // Python writes any non-empty token to the query pipe; the handler reads
        // the currently displayed text from FormFieldPropertiesPanel (whichever
        // editor is on top of the stacked widget) and writes it to the result pipe.
        // This eliminates the UIA window.descendants()/FindAll traversal in
        // _form_field_text_value(), which can deadlock when PDFE's main thread is
        // busy painting the panel after click_by_pdf_coord dispatch.
        testFormFieldValueQueryPipe_ = new QLineEdit(this);
        testFormFieldValueQueryPipe_->setObjectName("testFormFieldValueQueryPipe");
        testFormFieldValueQueryPipe_->setAccessibleName("Test Form Field Value Query Pipe");
        testFormFieldValueQueryPipe_->resize(0, 0);
        testFormFieldValueQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testFormFieldValueQueryPipe_->setMaximumSize(0, 0);

        testFormFieldValueResultPipe_ = new QLineEdit(this);
        testFormFieldValueResultPipe_->setObjectName("testFormFieldValueResultPipe");
        testFormFieldValueResultPipe_->setAccessibleName("Test Form Field Value Result Pipe");
        testFormFieldValueResultPipe_->resize(0, 0);
        testFormFieldValueResultPipe_->setFocusPolicy(Qt::NoFocus);
        testFormFieldValueResultPipe_->setMaximumSize(0, 0);

        connect(testFormFieldValueQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString result;
            if (formPropertiesPanel_) {
                result = formPropertiesPanel_->currentTextValue();
            }
            // Write result (may be empty string if no field selected).
            testFormFieldValueResultPipe_->blockSignals(true);
            testFormFieldValueResultPipe_->setText(result.isEmpty() ? QString("__empty__") : result);
            testFormFieldValueResultPipe_->blockSignals(false);
            // Clear query pipe so it can be reused.
            testFormFieldValueQueryPipe_->blockSignals(true);
            testFormFieldValueQueryPipe_->clear();
            testFormFieldValueQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline b3d7f942 phase-3 (GAP-5): movePageProgrammatic invocation pipe.
    // Python tests write "fromIndex,toIndex" to trigger
    // PagePanel::movePageProgrammatic(), which emits pageMoved(from, to) and
    // routes through the existing MovePageCommand pipeline — identical path to
    // a drag-drop reorder.  Gated by PDFE_TEST_PIPES; inert in production.
    //
    // NOTE: pagePanel_ is created in createDockPanels() (called below).
    // The pipe is wired here; the handler reads pagePanel_ at call-time —
    // it is always non-null before any test can write to the pipe.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testMovePagePipe_ = new QLineEdit(this);
        testMovePagePipe_->setObjectName("testMovePagePipe");
        testMovePagePipe_->setAccessibleName("Test Move Page Pipe");
        testMovePagePipe_->resize(0, 0);
        testMovePagePipe_->setFocusPolicy(Qt::NoFocus);
        testMovePagePipe_->setMaximumSize(0, 0);
        connect(testMovePagePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 2 && pagePanel_) {
                bool ok1 = false, ok2 = false;
                const int from = parts[0].trimmed().toInt(&ok1);
                const int to   = parts[1].trimmed().toInt(&ok2);
                if (ok1 && ok2) {
                    pagePanel_->movePageProgrammatic(from, to);
                }
            }
            testMovePagePipe_->blockSignals(true);
            testMovePagePipe_->clear();
            testMovePagePipe_->blockSignals(false);
        });
    }

    // GitHub #100: free-space simulation pipe. Writing a decimal byte count
    // installs a SaveSpacePrecheck probe that reports that figure as the
    // volume's available bytes; writing "off" removes it.
    //
    // This does NOT fake the verdict. It substitutes for the one OS reading
    // (`std::filesystem::space().available`, i.e. GetDiskFreeSpaceExW's
    // lpFreeBytesAvailableToCaller) and nothing else: the estimate, the
    // comparison, the handler dispatch and the QMessageBox installed in
    // main.cpp all run exactly as they would on a genuinely full volume.
    // That is the point — #100 was closed once on a pre-check whose warning
    // path no test could reach, and reopened for it.
    //
    // Gated by PDFE_TEST_PIPES; inert in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSimulateFreeSpacePipe_ = new QLineEdit(this);
        testSimulateFreeSpacePipe_->setObjectName("testSimulateFreeSpacePipe");
        testSimulateFreeSpacePipe_->setAccessibleName("Test Simulate Free Space Pipe");
        testSimulateFreeSpacePipe_->resize(0, 0);
        testSimulateFreeSpacePipe_->setFocusPolicy(Qt::NoFocus);
        testSimulateFreeSpacePipe_->setMaximumSize(0, 0);
        connect(testSimulateFreeSpacePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QString trimmed = payload.trimmed();
            if (trimmed.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0) {
                pdfe::core::setFreeSpaceProbeForTesting({});
            } else {
                bool ok = false;
                const qulonglong bytes = trimmed.toULongLong(&ok);
                if (ok) {
                    const std::uintmax_t simulated =
                        static_cast<std::uintmax_t>(bytes);
                    pdfe::core::setFreeSpaceProbeForTesting(
                        [simulated](const std::filesystem::path&,
                                    std::uintmax_t& availableOut) {
                            availableOut = simulated;
                            return true;
                        });
                }
            }
            testSimulateFreeSpacePipe_->blockSignals(true);
            testSimulateFreeSpacePipe_->clear();
            testSimulateFreeSpacePipe_->blockSignals(false);
        });
    }

    // Pipeline b5e3a9f2 phase-6 (P-GAP-73): programmatic page-panel
    // multi-select. Python tests write a comma-separated list of page
    // indices (e.g. "0,2,3"); the handler calls
    // pagePanel_->selectPagesProgrammatic({0,2,3}) which sets the
    // QListWidget's selection model directly, bypassing the pyautogui
    // Ctrl/Shift+click route that is flaky against Qt6 ExtendedSelection.
    // Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSelectPagesPipe_ = new QLineEdit(this);
        testSelectPagesPipe_->setObjectName("testSelectPagesPipe");
        testSelectPagesPipe_->setAccessibleName("Test Select Pages Pipe");
        testSelectPagesPipe_->resize(0, 0);
        testSelectPagesPipe_->setFocusPolicy(Qt::NoFocus);
        testSelectPagesPipe_->setMaximumSize(0, 0);
        connect(testSelectPagesPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            if (pagePanel_) {
                QList<int> indices;
                for (const QString& part : payload.split(',')) {
                    bool ok = false;
                    const int idx = part.trimmed().toInt(&ok);
                    if (ok) indices.append(idx);
                }
                if (!indices.isEmpty()) {
                    pagePanel_->selectPagesProgrammatic(indices);
                }
            }
            testSelectPagesPipe_->blockSignals(true);
            testSelectPagesPipe_->clear();
            testSelectPagesPipe_->blockSignals(false);
        });
    }

    // Pipeline f7c2a4e8 phase-3 (B1): hidden pipe for PagePanel::
    // dragItemProgrammatic. Python tests write "fromIdx,toIdx"; the
    // handler invokes the Q_INVOKABLE drag-style entry point on
    // PagePanel. Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testDragPipe_ = new QLineEdit(this);
        testDragPipe_->setObjectName("testDragPipe");
        testDragPipe_->setAccessibleName("Test Drag Pipe");
        testDragPipe_->resize(0, 0);
        testDragPipe_->setFocusPolicy(Qt::NoFocus);
        testDragPipe_->setMaximumSize(0, 0);
        connect(testDragPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 2 && pagePanel_) {
                bool ok1 = false, ok2 = false;
                const int from = parts[0].trimmed().toInt(&ok1);
                const int to   = parts[1].trimmed().toInt(&ok2);
                if (ok1 && ok2) {
                    pagePanel_->dragItemProgrammatic(from, to);
                }
            }
            testDragPipe_->blockSignals(true);
            testDragPipe_->clear();
            testDragPipe_->blockSignals(false);
        });
    }

    // Hidden pipe to navigate the canvas to a specific page
    // deterministically. Python tests write a single 0-based page index
    // (e.g. "1"); the handler calls canvas_->goToPage(n). The page-move
    // menu actions (onMovePageUp/onMovePageDown) read
    // canvas_->currentPage() as the move source; the {PGDN} type_keys
    // route the tests previously used is foreground-focus-dependent and
    // does NOT reliably advance the canvas on the Server-2019 CI session,
    // so the move was a no-op (from <= 0 → early return, no command, no
    // dirty marker). This seam sets currentPage() deterministically.
    // Gated by PDFE_TEST_PIPES; inert in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testGoToPagePipe_ = new QLineEdit(this);
        testGoToPagePipe_->setObjectName("testGoToPagePipe");
        testGoToPagePipe_->setAccessibleName("Test Go To Page Pipe");
        testGoToPagePipe_->resize(0, 0);
        testGoToPagePipe_->setFocusPolicy(Qt::NoFocus);
        testGoToPagePipe_->setMaximumSize(0, 0);
        connect(testGoToPagePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            bool ok = false;
            const int idx = payload.trimmed().toInt(&ok);
            if (ok && canvas_ && document_
                && idx >= 0 && idx < document_->pageCount()) {
                canvas_->goToPage(idx);
            }
            testGoToPagePipe_->blockSignals(true);
            testGoToPagePipe_->clear();
            testGoToPagePipe_->blockSignals(false);
        });
    }

    // Pipeline f7c2a4e8 phase-3 (B2): hidden pipe for PdfCanvas::
    // dragByPdfCoord. Python tests write "pageIndex,fromX,fromY,toX,toY"
    // in fitz coords. Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testCanvasDragPipe_ = new QLineEdit(this);
        testCanvasDragPipe_->setObjectName("testCanvasDragPipe");
        testCanvasDragPipe_->setAccessibleName("Test Canvas Drag Pipe");
        testCanvasDragPipe_->resize(0, 0);
        testCanvasDragPipe_->setFocusPolicy(Qt::NoFocus);
        testCanvasDragPipe_->setMaximumSize(0, 0);
        connect(testCanvasDragPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 5 && canvas_) {
                bool ok[5] = {false, false, false, false, false};
                const int pageIdx = parts[0].trimmed().toInt(&ok[0]);
                const qreal fx = parts[1].trimmed().toDouble(&ok[1]);
                const qreal fy = parts[2].trimmed().toDouble(&ok[2]);
                const qreal tx = parts[3].trimmed().toDouble(&ok[3]);
                const qreal ty = parts[4].trimmed().toDouble(&ok[4]);
                if (ok[0] && ok[1] && ok[2] && ok[3] && ok[4]) {
                    canvas_->dragByPdfCoord(pageIdx, fx, fy, tx, ty);
                }
            }
            testCanvasDragPipe_->blockSignals(true);
            testCanvasDragPipe_->clear();
            testCanvasDragPipe_->blockSignals(false);
        });
    }

    // c82a1f37 P4: hidden query/result pipe pair returning the selected
    // annotation's resize/rotate handle centroids in fitz coords. A UI test
    // writes any non-empty token to the trigger pipe, then reads
    // testAnnotHandlesResultPipe ("Name:x,y;...") so it can drag EXACTLY onto
    // a handle via testCanvasDragPipe (the tiny pixel target was the
    // CI-LIMITATION-CANVAS-SYNTHETIC-DRAG blocker). Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testAnnotHandlesTriggerPipe_ = new QLineEdit(this);
        testAnnotHandlesTriggerPipe_->setObjectName("testAnnotHandlesTriggerPipe");
        testAnnotHandlesTriggerPipe_->setAccessibleName("Test Annot Handles Trigger");
        testAnnotHandlesTriggerPipe_->resize(0, 0);
        testAnnotHandlesTriggerPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotHandlesTriggerPipe_->setMaximumSize(0, 0);

        testAnnotHandlesResultPipe_ = new QLineEdit(this);
        testAnnotHandlesResultPipe_->setObjectName("testAnnotHandlesResultPipe");
        testAnnotHandlesResultPipe_->setAccessibleName("Test Annot Handles Result");
        testAnnotHandlesResultPipe_->resize(0, 0);
        testAnnotHandlesResultPipe_->setFocusPolicy(Qt::NoFocus);
        testAnnotHandlesResultPipe_->setMaximumSize(0, 0);
        testAnnotHandlesResultPipe_->setReadOnly(true);

        connect(testAnnotHandlesTriggerPipe_, &QLineEdit::textChanged,
                this, [this](const QString& trigger) {
            if (!trigger.isEmpty()) {
                const QString result = canvas_
                    ? canvas_->selectedAnnotHandleCentroids() : QString();
                testAnnotHandlesResultPipe_->blockSignals(true);
                testAnnotHandlesResultPipe_->setText(result);
                testAnnotHandlesResultPipe_->blockSignals(false);
                testAnnotHandlesTriggerPipe_->blockSignals(true);
                testAnnotHandlesTriggerPipe_->clear();
                testAnnotHandlesTriggerPipe_->blockSignals(false);
            }
        });

        // c82a1f37 P5: deterministic form-field CREATE seam. Python writes
        // "type,page,x,y,w,h" (type ∈ Text/Checkbox/RadioButton/ComboBox/
        // ListBox/PushButton/Signature; coords in fitz points). Invokes the
        // exact same createFormField() path a real drag-release would, so a
        // field of the requested type is committed via the command system —
        // removing the non-deterministic real-mouse placement (P4 found a
        // different 2-3 of 4 types committing per run). Does NOT require the
        // form tool to be active (only document_/formManager_). Gated by
        // PDFE_TEST_PIPES; inert in production.
        testFieldCreatePipe_ = new QLineEdit(this);
        testFieldCreatePipe_->setObjectName("testFieldCreatePipe");
        testFieldCreatePipe_->setAccessibleName("Test Field Create Pipe");
        testFieldCreatePipe_->resize(0, 0);
        testFieldCreatePipe_->setFocusPolicy(Qt::NoFocus);
        testFieldCreatePipe_->setMaximumSize(0, 0);
        connect(testFieldCreatePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 6) {
                static const QHash<QString, pdfe::forms::FormFieldType> typeMap = {
                    {"Text",        pdfe::forms::FormFieldType::Text},
                    {"Checkbox",    pdfe::forms::FormFieldType::Checkbox},
                    {"RadioButton", pdfe::forms::FormFieldType::RadioButton},
                    {"ComboBox",    pdfe::forms::FormFieldType::ComboBox},
                    {"ListBox",     pdfe::forms::FormFieldType::ListBox},
                    {"PushButton",  pdfe::forms::FormFieldType::PushButton},
                    {"Signature",   pdfe::forms::FormFieldType::Signature},
                };
                const auto it = typeMap.constFind(parts[0].trimmed());
                bool okp = false, okx = false, oky = false, okw = false, okh = false;
                const int page = parts[1].trimmed().toInt(&okp);
                const double x = parts[2].trimmed().toDouble(&okx);
                const double y = parts[3].trimmed().toDouble(&oky);
                const double w = parts[4].trimmed().toDouble(&okw);
                const double h = parts[5].trimmed().toDouble(&okh);
                if (it != typeMap.constEnd() && okp && okx && oky && okw && okh
                    && page >= 0) {
                    createFormField(page, it.value(), QRectF(x, y, w, h));
                }
            }
            testFieldCreatePipe_->blockSignals(true);
            testFieldCreatePipe_->clear();
            testFieldCreatePipe_->blockSignals(false);
        });
    }

    // Pipeline f7c2a4e8 phase-3 (B2): hidden pipe for PdfCanvas::
    // tripleClickByPdfCoord. Python tests write "pageIndex,pdfX,pdfY".
    // Gated by PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testTripleClickPipe_ = new QLineEdit(this);
        testTripleClickPipe_->setObjectName("testTripleClickPipe");
        testTripleClickPipe_->setAccessibleName("Test Triple Click Pipe");
        testTripleClickPipe_->resize(0, 0);
        testTripleClickPipe_->setFocusPolicy(Qt::NoFocus);
        testTripleClickPipe_->setMaximumSize(0, 0);
        connect(testTripleClickPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 3 && canvas_) {
                bool ok1 = false, ok2 = false, ok3 = false;
                const int pageIdx = parts[0].trimmed().toInt(&ok1);
                const qreal x = parts[1].trimmed().toDouble(&ok2);
                const qreal y = parts[2].trimmed().toDouble(&ok3);
                if (ok1 && ok2 && ok3) {
                    canvas_->tripleClickByPdfCoord(pageIdx, x, y);
                }
            }
            testTripleClickPipe_->blockSignals(true);
            testTripleClickPipe_->clear();
            testTripleClickPipe_->blockSignals(false);
        });
    }

    // Pipeline f7c2a4e8 phase-3 (B3): hidden pipe for PdfCanvas::
    // scrollByPixels. Python tests write a single integer pixel-delta
    // (positive = scroll up, negative = scroll down). Gated by
    // PDFE_TEST_PIPES.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testScrollPipe_ = new QLineEdit(this);
        testScrollPipe_->setObjectName("testScrollPipe");
        testScrollPipe_->setAccessibleName("Test Scroll Pipe");
        testScrollPipe_->resize(0, 0);
        testScrollPipe_->setFocusPolicy(Qt::NoFocus);
        testScrollPipe_->setMaximumSize(0, 0);
        connect(testScrollPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            bool ok = false;
            const int delta = payload.trimmed().toInt(&ok);
            if (ok && canvas_) {
                canvas_->scrollByPixels(delta);
            }
            testScrollPipe_->blockSignals(true);
            testScrollPipe_->clear();
            testScrollPipe_->blockSignals(false);
        });
    }

    // GitHub #220: Extract Pages invocation pipe + result readback.
    //
    // The F2b matrix row was an UNCONDITIONAL @pytest.mark.skip, and
    // coverage_resolver.py treats an unconditional skip as NOT resolved
    // coverage -- so the row was a real hole, not a tolerated environmental
    // one. The karen f7c2a4e8 review was right to refuse a liveness-check
    // oracle ("the process did not crash" passes for an Extract that does
    // nothing), and right that a truthful oracle needs the extracted FILE.
    // This pipe supplies both halves: a deterministic trigger, and a result
    // channel that distinguishes a failed extraction from a pipe that never
    // ran. Payload "0,2;C:/tmp/out.pdf"; result "ok|<n>" or "err|<reason>".
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testExtractResultPipe_ = new QLineEdit(this);
        testExtractResultPipe_->setObjectName("testExtractResultPipe");
        testExtractResultPipe_->setAccessibleName("Test Extract Result Pipe");
        testExtractResultPipe_->resize(0, 0);
        testExtractResultPipe_->setFocusPolicy(Qt::NoFocus);
        testExtractResultPipe_->setMaximumSize(0, 0);
        testExtractResultPipe_->setReadOnly(true);

        testExtractPipe_ = new QLineEdit(this);
        testExtractPipe_->setObjectName("testExtractPipe");
        testExtractPipe_->setAccessibleName("Test Extract Pipe");
        testExtractPipe_->resize(0, 0);
        testExtractPipe_->setFocusPolicy(Qt::NoFocus);
        testExtractPipe_->setMaximumSize(0, 0);
        connect(testExtractPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            auto report = [this](const QString& text) {
                if (testExtractResultPipe_) {
                    testExtractResultPipe_->setText(text);
                }
                testExtractPipe_->blockSignals(true);
                testExtractPipe_->clear();
                testExtractPipe_->blockSignals(false);
            };
            if (!document_) {
                report(QStringLiteral("err|no-document"));
                return;
            }
            const int sep = payload.indexOf(QLatin1Char(';'));
            if (sep <= 0 || sep == payload.size() - 1) {
                report(QStringLiteral("err|malformed-payload"));
                return;
            }
            const QString indexPart = payload.left(sep);
            const QString outPath = payload.mid(sep + 1);
            // Same defence-in-depth the save-as pipe applies: refuse what
            // the production path would also refuse, so a test cannot use the
            // seam to reach somewhere the product never would.
            if (outPath.contains(QChar(u'\0'))
                || QFileInfo(outPath).fileName().contains(QLatin1Char(':'))) {
                report(QStringLiteral("err|rejected-path"));
                return;
            }
            std::vector<int> indices;
            const QStringList parts = indexPart.split(QLatin1Char(','),
                                                      Qt::SkipEmptyParts);
            for (const QString& p : parts) {
                bool ok = false;
                const int idx = p.trimmed().toInt(&ok);
                if (!ok || idx < 0 || idx >= document_->pageCount()) {
                    report(QStringLiteral("err|bad-index|") + p.trimmed());
                    return;
                }
                indices.push_back(idx);
            }
            if (indices.empty()) {
                report(QStringLiteral("err|no-indices"));
                return;
            }
            try {
                pdfe::core::extractPagesToFile(
                    muPdfContext_, document_.get(), indices,
                    qStringToPath(outPath));
                report(QStringLiteral("ok|%1").arg(
                    static_cast<int>(indices.size())));
            } catch (const std::exception& e) {
                report(QStringLiteral("err|") +
                       QString::fromUtf8(e.what()).left(120));
            } catch (...) {
                report(QStringLiteral("err|unknown-exception"));
            }
        });
    }

    // GitHub #219: TRE span-activation pipe + result readback.
    //
    // The xfail reason localised the break precisely: OCR is fine
    // (TEXT_EDIT_READY|9 is correct), it is the click -> spanActivated hop
    // that does not complete under a synthetic click. TextEditTool already
    // exposes selectSpanForTesting(), which calls the PRODUCTION selectSpan()
    // and therefore emits the real spanActivated -- the same signal
    // MainWindow connects to build the atlas. Nothing reached it from Python.
    //
    // Payload: a bare span index ("3"), or a PDF-space point ("280.0,700.0")
    // whose containing span is resolved here. Reporting the RESOLVED index
    // back matters: a coordinate that lands between spans is a test-authoring
    // error, and "err|no-span-at-point" says so instead of timing out later
    // on an atlas that was never asked for.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSpanActivateResultPipe_ = new QLineEdit(this);
        testSpanActivateResultPipe_->setObjectName("testSpanActivateResultPipe");
        testSpanActivateResultPipe_->setAccessibleName("Test Span Activate Result Pipe");
        testSpanActivateResultPipe_->resize(0, 0);
        testSpanActivateResultPipe_->setFocusPolicy(Qt::NoFocus);
        testSpanActivateResultPipe_->setMaximumSize(0, 0);
        testSpanActivateResultPipe_->setReadOnly(true);

        testSpanActivatePipe_ = new QLineEdit(this);
        testSpanActivatePipe_->setObjectName("testSpanActivatePipe");
        testSpanActivatePipe_->setAccessibleName("Test Span Activate Pipe");
        testSpanActivatePipe_->resize(0, 0);
        testSpanActivatePipe_->setFocusPolicy(Qt::NoFocus);
        testSpanActivatePipe_->setMaximumSize(0, 0);
        connect(testSpanActivatePipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            auto report = [this](const QString& text) {
                if (testSpanActivateResultPipe_) {
                    testSpanActivateResultPipe_->setText(text);
                }
                testSpanActivatePipe_->blockSignals(true);
                testSpanActivatePipe_->clear();
                testSpanActivatePipe_->blockSignals(false);
            };
            pdfe::textedit::TextEditTool* tool =
                canvas_ ? canvas_->textEditTool() : nullptr;
            if (!tool) {
                report(QStringLiteral("err|no-text-edit-tool"));
                return;
            }
            const int spanCount = tool->spanCountForTesting();
            if (spanCount <= 0) {
                report(QStringLiteral("err|no-spans"));
                return;
            }
            int target = -1;
            if (payload.contains(QLatin1Char(','))) {
                const QStringList parts = payload.split(QLatin1Char(','));
                bool okX = false, okY = false;
                const qreal px = parts.value(0).trimmed().toDouble(&okX);
                const qreal py = parts.value(1).trimmed().toDouble(&okY);
                if (parts.size() != 2 || !okX || !okY) {
                    report(QStringLiteral("err|malformed-point"));
                    return;
                }
                for (int i = 0; i < spanCount; ++i) {
                    if (tool->spanBboxForTesting(i).contains(QPointF(px, py))) {
                        target = i;
                        break;
                    }
                }
                if (target < 0) {
                    report(QStringLiteral("err|no-span-at-point"));
                    return;
                }
            } else {
                bool ok = false;
                target = payload.trimmed().toInt(&ok);
                if (!ok || target < 0 || target >= spanCount) {
                    report(QStringLiteral("err|bad-span-index"));
                    return;
                }
            }
            if (!tool->selectSpanForTesting(target)) {
                report(QStringLiteral("err|select-span-refused"));
                return;
            }
            report(QStringLiteral("ok|%1").arg(target));
        });
    }

    // GitHub #222: PagePanel selection readback.
    //
    // testSelectPagesPipe could SET a selection; nothing could READ one. So the
    // two SendInput gesture tests fell back to "the process is still running",
    // which is satisfied by a Ctrl+click that selected nothing. Answers the
    // selected indices ("0,2") or the literal "none" -- "none" rather than an
    // empty string because an empty result pipe is indistinguishable from a
    // pipe that never answered, and those are different failures.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSelectedPagesResultPipe_ = new QLineEdit(this);
        testSelectedPagesResultPipe_->setObjectName("testSelectedPagesResultPipe");
        testSelectedPagesResultPipe_->setAccessibleName("Test Selected Pages Result");
        testSelectedPagesResultPipe_->resize(0, 0);
        testSelectedPagesResultPipe_->setFocusPolicy(Qt::NoFocus);
        testSelectedPagesResultPipe_->setMaximumSize(0, 0);
        testSelectedPagesResultPipe_->setReadOnly(true);

        testSelectedPagesQueryPipe_ = new QLineEdit(this);
        testSelectedPagesQueryPipe_->setObjectName("testSelectedPagesQueryPipe");
        testSelectedPagesQueryPipe_->setAccessibleName("Test Selected Pages Query");
        testSelectedPagesQueryPipe_->resize(0, 0);
        testSelectedPagesQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testSelectedPagesQueryPipe_->setMaximumSize(0, 0);
        connect(testSelectedPagesQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString answer = QStringLiteral("none");
            if (pagePanel_) {
                QStringList parts;
                for (int idx : pagePanel_->selectedPageIndices()) {
                    parts << QString::number(idx);
                }
                if (!parts.isEmpty()) {
                    answer = parts.join(QLatin1Char(','));
                }
            }
            testSelectedPagesResultPipe_->blockSignals(true);
            testSelectedPagesResultPipe_->setText(answer);
            testSelectedPagesResultPipe_->blockSignals(false);
            testSelectedPagesQueryPipe_->blockSignals(true);
            testSelectedPagesQueryPipe_->clear();
            testSelectedPagesQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline b3d7f942 phase-3 (GAP-6/GAP-7): addRedactionMarkAtPdfRect
    // invocation pipe.  Python tests write "pageIndex,x,y,w,h" (all in
    // MuPDF/fitz-space: origin top-left, Y-down, points) to trigger
    // PdfCanvas::addRedactionMarkAtPdfRect(), which emits
    // redactionMarkRequested — connected in setupCanvasConnections() to the
    // CreateRedactionCommand pipeline, giving per-mark undo behaviour
    // identical to a user drag.  Gated by PDFE_TEST_PIPES; inert in
    // production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testAddRedactionMarkPipe_ = new QLineEdit(this);
        testAddRedactionMarkPipe_->setObjectName("testAddRedactionMarkPipe");
        testAddRedactionMarkPipe_->setAccessibleName("Test Add Redaction Mark Pipe");
        testAddRedactionMarkPipe_->resize(0, 0);
        testAddRedactionMarkPipe_->setFocusPolicy(Qt::NoFocus);
        testAddRedactionMarkPipe_->setMaximumSize(0, 0);
        connect(testAddRedactionMarkPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const QStringList parts = payload.split(',');
            if (parts.size() == 5 && canvas_) {
                bool ok1 = false, ok2 = false, ok3 = false, ok4 = false, ok5 = false;
                const int    pageIndex = parts[0].trimmed().toInt(&ok1);
                const qreal  x = parts[1].trimmed().toDouble(&ok2);
                const qreal  y = parts[2].trimmed().toDouble(&ok3);
                const qreal  w = parts[3].trimmed().toDouble(&ok4);
                const qreal  h = parts[4].trimmed().toDouble(&ok5);
                if (ok1 && ok2 && ok3 && ok4 && ok5 && w > 0.0 && h > 0.0) {
                    canvas_->addRedactionMarkAtPdfRect(pageIndex, QRectF(x, y, w, h));
                }
            }
            testAddRedactionMarkPipe_->blockSignals(true);
            testAddRedactionMarkPipe_->clear();
            testAddRedactionMarkPipe_->blockSignals(false);
        });
    }

    // Pipeline b3d7f942 phase-3 (GAP-7): redaction mark count query+result pipe.
    // Python tests write any non-empty token to the query pipe; the handler
    // reads redactionPanel_->topLevelItemCount() (the panel's internal count,
    // bypassing the UIA QTreeWidget cell-inflation issue where each row
    // exposes 4 DataItem children for 4 columns) and writes the integer as a
    // decimal string to the result pipe.  Gated by PDFE_TEST_PIPES.
    //
    // NOTE: redactionPanel_ is created in createDockPanels() (called below).
    // The pipe is wired here; handlers read redactionPanel_ at call-time.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testRedactionMarkCountResultPipe_ = new QLineEdit(this);
        testRedactionMarkCountResultPipe_->setObjectName(
            "testRedactionMarkCountResultPipe");
        testRedactionMarkCountResultPipe_->setAccessibleName(
            "Test Redaction Mark Count Result");
        testRedactionMarkCountResultPipe_->resize(0, 0);
        testRedactionMarkCountResultPipe_->setFocusPolicy(Qt::NoFocus);
        testRedactionMarkCountResultPipe_->setMaximumSize(0, 0);
        testRedactionMarkCountResultPipe_->setReadOnly(true);

        testRedactionMarkCountQueryPipe_ = new QLineEdit(this);
        testRedactionMarkCountQueryPipe_->setObjectName(
            "testRedactionMarkCountQueryPipe");
        testRedactionMarkCountQueryPipe_->setAccessibleName(
            "Test Redaction Mark Count Query");
        testRedactionMarkCountQueryPipe_->resize(0, 0);
        testRedactionMarkCountQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testRedactionMarkCountQueryPipe_->setMaximumSize(0, 0);
        connect(testRedactionMarkCountQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            int count = 0;
            if (redactionPanel_) {
                count = redactionPanel_->topLevelItemCount();
            }
            testRedactionMarkCountResultPipe_->blockSignals(true);
            testRedactionMarkCountResultPipe_->setText(QString::number(count));
            testRedactionMarkCountResultPipe_->blockSignals(false);
            testRedactionMarkCountQueryPipe_->blockSignals(true);
            testRedactionMarkCountQueryPipe_->clear();
            testRedactionMarkCountQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline a7c4f891 phase-3 (E2 / P12-INFRA-1 sub-issue 5):
    // hidden query+result pipe pair for reading printAction_->
    // isEnabled(). The QAction is added only to fileMenu, so the
    // UIA tree only exposes it when the File menu is open. Tests
    // need a foreground-independent way to read the enabled state
    // (which reflects permission gating on encrypted docs). Pattern
    // mirrors testCanvasRotationQueryPipe.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testPrintActionEnabledResultPipe_ = new QLineEdit(this);
        testPrintActionEnabledResultPipe_->setObjectName(
            "testPrintActionEnabledResultPipe");
        testPrintActionEnabledResultPipe_->setAccessibleName(
            "Test Print Action Enabled Result");
        testPrintActionEnabledResultPipe_->resize(0, 0);
        testPrintActionEnabledResultPipe_->setFocusPolicy(Qt::NoFocus);
        testPrintActionEnabledResultPipe_->setMaximumSize(0, 0);
        testPrintActionEnabledResultPipe_->setReadOnly(true);

        testPrintActionEnabledQueryPipe_ = new QLineEdit(this);
        testPrintActionEnabledQueryPipe_->setObjectName(
            "testPrintActionEnabledQueryPipe");
        testPrintActionEnabledQueryPipe_->setAccessibleName(
            "Test Print Action Enabled Query");
        testPrintActionEnabledQueryPipe_->resize(0, 0);
        testPrintActionEnabledQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testPrintActionEnabledQueryPipe_->setMaximumSize(0, 0);
        connect(testPrintActionEnabledQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            // Result: "1" if enabled, "0" if disabled, "-1" if printAction_
            // is null (PDFE not fully initialized).
            QString result = "-1";
            if (printAction_) {
                result = printAction_->isEnabled() ? "1" : "0";
            }
            testPrintActionEnabledResultPipe_->blockSignals(true);
            testPrintActionEnabledResultPipe_->setText(result);
            testPrintActionEnabledResultPipe_->blockSignals(false);
            testPrintActionEnabledQueryPipe_->blockSignals(true);
            testPrintActionEnabledQueryPipe_->clear();
            testPrintActionEnabledQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline a7c4f891 phase-5 (D1 / P-GAP-74): atlas readiness pipe.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testAtlasReadyResultPipe_ = new QLineEdit(this);
        testAtlasReadyResultPipe_->setObjectName(
            "testAtlasReadyResultPipe");
        testAtlasReadyResultPipe_->setAccessibleName(
            "Test Atlas Ready Result");
        testAtlasReadyResultPipe_->resize(0, 0);
        testAtlasReadyResultPipe_->setFocusPolicy(Qt::NoFocus);
        testAtlasReadyResultPipe_->setMaximumSize(0, 0);
        testAtlasReadyResultPipe_->setReadOnly(true);

        testAtlasReadyQueryPipe_ = new QLineEdit(this);
        testAtlasReadyQueryPipe_->setObjectName(
            "testAtlasReadyQueryPipe");
        testAtlasReadyQueryPipe_->setAccessibleName(
            "Test Atlas Ready Query");
        testAtlasReadyQueryPipe_->resize(0, 0);
        testAtlasReadyQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testAtlasReadyQueryPipe_->setMaximumSize(0, 0);
        connect(testAtlasReadyQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString result = "0";
            if (scanCroppedAtlas_ && freeTypeStyledAtlas_) {
                result = "1";
            }
            testAtlasReadyResultPipe_->blockSignals(true);
            testAtlasReadyResultPipe_->setText(result);
            testAtlasReadyResultPipe_->blockSignals(false);
            testAtlasReadyQueryPipe_->blockSignals(true);
            testAtlasReadyQueryPipe_->clear();
            testAtlasReadyQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline phase-7 (loop-until-clean): zoom-combo readback pipe.
    // The zoom QComboBox lives in a QToolBar; the Qt6 UIA bridge on the
    // Windows-Server-2019 CI rig does not reliably expose a combo-in-toolbar
    // by AutomationId, so test_zoom_combo_uia_exposed cannot locate it via
    // UIA even though MainWindowIntegrationTest.ZoomComboExists confirms it.
    // This pipe returns the live zoomCombo_->currentText() (a genuine
    // readback) so the test asserts the widget exists and is wired.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testZoomComboResultPipe_ = new QLineEdit(this);
        testZoomComboResultPipe_->setObjectName("testZoomComboResultPipe");
        testZoomComboResultPipe_->setAccessibleName("Test Zoom Combo Result");
        testZoomComboResultPipe_->resize(0, 0);
        testZoomComboResultPipe_->setFocusPolicy(Qt::NoFocus);
        testZoomComboResultPipe_->setMaximumSize(0, 0);
        testZoomComboResultPipe_->setReadOnly(true);

        testZoomComboQueryPipe_ = new QLineEdit(this);
        testZoomComboQueryPipe_->setObjectName("testZoomComboQueryPipe");
        testZoomComboQueryPipe_->setAccessibleName("Test Zoom Combo Query");
        testZoomComboQueryPipe_->resize(0, 0);
        testZoomComboQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testZoomComboQueryPipe_->setMaximumSize(0, 0);
        connect(testZoomComboQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            const QString result =
                zoomCombo_ ? zoomCombo_->currentText() : QStringLiteral("__null__");
            testZoomComboResultPipe_->blockSignals(true);
            testZoomComboResultPipe_->setText(result);
            testZoomComboResultPipe_->blockSignals(false);
            testZoomComboQueryPipe_->blockSignals(true);
            testZoomComboQueryPipe_->clear();
            testZoomComboQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline phase-7 (loop-until-clean): page-count readback pipe.
    // The page-operations tests read the count from the status-bar pageLabel
    // via UIA, but Qt does not surface QLabel::text() through UIA and the
    // accessibleName mirror is flaky to discover immediately after launch,
    // so the read can return 0 while the document is in fact loaded. This
    // pipe returns the authoritative document_->pageCount().
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testPageCountResultPipe_ = new QLineEdit(this);
        testPageCountResultPipe_->setObjectName("testPageCountResultPipe");
        testPageCountResultPipe_->setAccessibleName("Test Page Count Result");
        testPageCountResultPipe_->resize(0, 0);
        testPageCountResultPipe_->setFocusPolicy(Qt::NoFocus);
        testPageCountResultPipe_->setMaximumSize(0, 0);
        testPageCountResultPipe_->setReadOnly(true);

        testPageCountQueryPipe_ = new QLineEdit(this);
        testPageCountQueryPipe_->setObjectName("testPageCountQueryPipe");
        testPageCountQueryPipe_->setAccessibleName("Test Page Count Query");
        testPageCountQueryPipe_->resize(0, 0);
        testPageCountQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testPageCountQueryPipe_->setMaximumSize(0, 0);
        connect(testPageCountQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            const int count = document_ ? document_->pageCount() : 0;
            testPageCountResultPipe_->blockSignals(true);
            testPageCountResultPipe_->setText(QString::number(count));
            testPageCountResultPipe_->blockSignals(false);
            testPageCountQueryPipe_->blockSignals(true);
            testPageCountQueryPipe_->clear();
            testPageCountQueryPipe_->blockSignals(false);
        });
    }

    // GitHub #136 (CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT): canvas
    // rectangle readback, in MainWindow CLIENT-AREA DEVICE PIXELS — the same
    // space GetClientRect/PrintWindow use, so the answer can be handed
    // straight to capture_hwnd(hwnd, region=(x, y, w, h)).
    //
    // The visual-regression helper used to crop a FRACTION of the captured
    // window (x = 15% of width, y = 20% of height, 70% x 65%). That is the
    // "never guess canvas coordinates" rule broken for a capture region: the
    // moment the dock widths differ from whatever they were when the baseline
    // was taken, the fractional box slides off the canvas and the SSIM
    // compare is measuring dock furniture. Measured symptom: 45% of pixels
    // differing, in the very session the baselines were captured in.
    //
    // devicePixelRatioF() is applied here rather than in Python because Qt
    // widget geometry is in logical pixels and the capture is in device
    // pixels; the two spaces disagreed by ~145 px in an earlier incident.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testCanvasRectResultPipe_ = new QLineEdit(this);
        testCanvasRectResultPipe_->setObjectName("testCanvasRectResultPipe");
        testCanvasRectResultPipe_->setAccessibleName("Test Canvas Rect Result");
        testCanvasRectResultPipe_->resize(0, 0);
        testCanvasRectResultPipe_->setFocusPolicy(Qt::NoFocus);
        testCanvasRectResultPipe_->setMaximumSize(0, 0);
        testCanvasRectResultPipe_->setReadOnly(true);

        testCanvasRectQueryPipe_ = new QLineEdit(this);
        testCanvasRectQueryPipe_->setObjectName("testCanvasRectQueryPipe");
        testCanvasRectQueryPipe_->setAccessibleName("Test Canvas Rect Query");
        testCanvasRectQueryPipe_->resize(0, 0);
        testCanvasRectQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testCanvasRectQueryPipe_->setMaximumSize(0, 0);
        // PUBLISHED, NOT POLLED-BY-WRITE. The result pipe is kept current by
        // the app, so a test only has to READ it.
        //
        // This matters more than it looks. Writing to a query pipe goes
        // through UIA set_edit_text, which moves Win32 focus to the hidden
        // QLineEdit — the FOCUS SIDE EFFECT documented on view_state() in
        // tests/helpers/canvas_geometry.py. A screenshot helper runs
        // immediately before keyboard navigation all over the suite, and
        // stealing focus there breaks the very next key. Measured while
        // building this: routing _capture_canvas through a query WRITE made
        // test_apply_redactions_across_multiple_pages skip with "Did not
        // return to page 1 after PGUP" — the PGUP went to the pipe. UIA
        // set_focus() on the canvas did NOT undo it; only not stealing focus
        // in the first place does.
        //
        // A 250 ms timer rather than a resizeEvent override because the
        // canvas also moves when a DOCK is resized, which never reaches
        // MainWindow::resizeEvent. setText is suppressed unless the value
        // actually changed, so the UIA tree sees no churn.
        const auto publishCanvasRect = [this]() {
            if (!testCanvasRectResultPipe_) return;
            QString result = QStringLiteral("none");
            if (canvas_ && canvas_->width() > 0 && canvas_->height() > 0) {
                const QPoint tl = canvas_->mapTo(this, QPoint(0, 0));
                const qreal dpr = devicePixelRatioF();
                result = QStringLiteral("%1,%2,%3,%4")
                             .arg(qRound(tl.x() * dpr))
                             .arg(qRound(tl.y() * dpr))
                             .arg(qRound(canvas_->width() * dpr))
                             .arg(qRound(canvas_->height() * dpr));
            }
            if (testCanvasRectResultPipe_->text() == result) return;
            testCanvasRectResultPipe_->blockSignals(true);
            testCanvasRectResultPipe_->setText(result);
            testCanvasRectResultPipe_->blockSignals(false);
        };

        auto* canvasRectTimer = new QTimer(this);
        canvasRectTimer->setInterval(250);
        connect(canvasRectTimer, &QTimer::timeout, this, publishCanvasRect);
        canvasRectTimer->start();
        QTimer::singleShot(0, this, publishCanvasRect);

        // The query pipe remains for a caller that wants a synchronous
        // refresh and can afford the focus move.
        connect(testCanvasRectQueryPipe_, &QLineEdit::textChanged,
                this, [this, publishCanvasRect](const QString& token) {
            if (token.isEmpty()) return;
            publishCanvasRect();
            testCanvasRectQueryPipe_->blockSignals(true);
            testCanvasRectQueryPipe_->clear();
            testCanvasRectQueryPipe_->blockSignals(false);
        });
    }

    // GitHub #136 (CI-LIMITATION-PAGEPANEL-POPULATE-RACE): PagePanel
    // readiness pipe. Answers "<panelItems>,<docPages>".
    //
    // The page-operations tests used to wait for the PagePanel by polling
    // the UIA tree for at least N ListItems, where N was a hand-picked
    // floor (3 for the 3-page fixture, 50 for the 100-page one). A floor
    // is satisfied by a panel that is still filling, and by a UIA tree
    // that has only realised some of the rows — which is precisely the
    // documented failure ("fixture has only 0 pages", "have 0 items",
    // multi-select lost before the rotate). This pipe answers with the
    // real numbers, so a test can wait for the exact terminal state
    // instead of a threshold, and can tell "not ready yet" apart from
    // "the UIA tree is under-reporting".
    //
    // NOT a longer sleep: the predicate is an equality on two live counts,
    // so it goes true the instant the panel finishes and never goes true
    // for a partially-populated panel.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testPagePanelReadyResultPipe_ = new QLineEdit(this);
        testPagePanelReadyResultPipe_->setObjectName("testPagePanelReadyResultPipe");
        testPagePanelReadyResultPipe_->setAccessibleName("Test Page Panel Ready Result");
        testPagePanelReadyResultPipe_->resize(0, 0);
        testPagePanelReadyResultPipe_->setFocusPolicy(Qt::NoFocus);
        testPagePanelReadyResultPipe_->setMaximumSize(0, 0);
        testPagePanelReadyResultPipe_->setReadOnly(true);

        testPagePanelReadyQueryPipe_ = new QLineEdit(this);
        testPagePanelReadyQueryPipe_->setObjectName("testPagePanelReadyQueryPipe");
        testPagePanelReadyQueryPipe_->setAccessibleName("Test Page Panel Ready Query");
        testPagePanelReadyQueryPipe_->resize(0, 0);
        testPagePanelReadyQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testPagePanelReadyQueryPipe_->setMaximumSize(0, 0);
        connect(testPagePanelReadyQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            const int items = pagePanel_ ? pagePanel_->panelItemCount() : -1;
            const int pages = document_ ? document_->pageCount() : 0;
            testPagePanelReadyResultPipe_->blockSignals(true);
            testPagePanelReadyResultPipe_->setText(
                QString("%1,%2").arg(items).arg(pages));
            testPagePanelReadyResultPipe_->blockSignals(false);
            testPagePanelReadyQueryPipe_->blockSignals(true);
            testPagePanelReadyQueryPipe_->clear();
            testPagePanelReadyQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline f3c91a2e phase-7 (GAP-A14): three query/result pipe pairs
    // that give the LTV pywinauto tests a genuine PASS path on the
    // Windows-Server-2019 CI session, where UIA cannot read the
    // SignaturePanel label text nor enumerate modal Qt dialogs. Each
    // handler writes the asserted state into its result pipe on any
    // non-empty query token. Gated by PDFE_TEST_PIPES; nullptr in
    // production. See test_ui_ltv.py.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        // (1) LTV status label readback.
        testLtvStatusResultPipe_ = new QLineEdit(this);
        testLtvStatusResultPipe_->setObjectName("testLtvStatusResultPipe");
        testLtvStatusResultPipe_->setAccessibleName("Test LTV Status Result");
        testLtvStatusResultPipe_->resize(0, 0);
        testLtvStatusResultPipe_->setFocusPolicy(Qt::NoFocus);
        testLtvStatusResultPipe_->setMaximumSize(0, 0);
        testLtvStatusResultPipe_->setReadOnly(true);

        testLtvStatusQueryPipe_ = new QLineEdit(this);
        testLtvStatusQueryPipe_->setObjectName("testLtvStatusQueryPipe");
        testLtvStatusQueryPipe_->setAccessibleName("Test LTV Status Query");
        testLtvStatusQueryPipe_->resize(0, 0);
        testLtvStatusQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testLtvStatusQueryPipe_->setMaximumSize(0, 0);
        connect(testLtvStatusQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString text = signaturePanel_ ? signaturePanel_->ltvStatusText()
                                           : QString();
            testLtvStatusResultPipe_->blockSignals(true);
            testLtvStatusResultPipe_->setText(
                text.isEmpty() ? QStringLiteral("__null__") : text);
            testLtvStatusResultPipe_->blockSignals(false);
            testLtvStatusQueryPipe_->blockSignals(true);
            testLtvStatusQueryPipe_->clear();
            testLtvStatusQueryPipe_->blockSignals(false);
        });

        // (2) ValidationRequestDialog introspection readback.
        testValidationDialogResultPipe_ = new QLineEdit(this);
        testValidationDialogResultPipe_->setObjectName(
            "testValidationDialogResultPipe");
        testValidationDialogResultPipe_->setAccessibleName(
            "Test Validation Dialog Result");
        testValidationDialogResultPipe_->resize(0, 0);
        testValidationDialogResultPipe_->setFocusPolicy(Qt::NoFocus);
        testValidationDialogResultPipe_->setMaximumSize(0, 0);
        testValidationDialogResultPipe_->setReadOnly(true);

        testValidationDialogQueryPipe_ = new QLineEdit(this);
        testValidationDialogQueryPipe_->setObjectName(
            "testValidationDialogQueryPipe");
        testValidationDialogQueryPipe_->setAccessibleName(
            "Test Validation Dialog Query");
        testValidationDialogQueryPipe_->resize(0, 0);
        testValidationDialogQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testValidationDialogQueryPipe_->setMaximumSize(0, 0);
        connect(testValidationDialogQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString rec =
                pdfe::ui::ValidationRequestDialog::lastTestIntrospection();
            testValidationDialogResultPipe_->blockSignals(true);
            testValidationDialogResultPipe_->setText(
                rec.isEmpty() ? QStringLiteral("__null__") : rec);
            testValidationDialogResultPipe_->blockSignals(false);
            testValidationDialogQueryPipe_->blockSignals(true);
            testValidationDialogQueryPipe_->clear();
            testValidationDialogQueryPipe_->blockSignals(false);
        });

        // (3) TrustAnchorDialog introspection readback.
        testTrustAnchorDialogResultPipe_ = new QLineEdit(this);
        testTrustAnchorDialogResultPipe_->setObjectName(
            "testTrustAnchorDialogResultPipe");
        testTrustAnchorDialogResultPipe_->setAccessibleName(
            "Test Trust Anchor Dialog Result");
        testTrustAnchorDialogResultPipe_->resize(0, 0);
        testTrustAnchorDialogResultPipe_->setFocusPolicy(Qt::NoFocus);
        testTrustAnchorDialogResultPipe_->setMaximumSize(0, 0);
        testTrustAnchorDialogResultPipe_->setReadOnly(true);

        testTrustAnchorDialogQueryPipe_ = new QLineEdit(this);
        testTrustAnchorDialogQueryPipe_->setObjectName(
            "testTrustAnchorDialogQueryPipe");
        testTrustAnchorDialogQueryPipe_->setAccessibleName(
            "Test Trust Anchor Dialog Query");
        testTrustAnchorDialogQueryPipe_->resize(0, 0);
        testTrustAnchorDialogQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testTrustAnchorDialogQueryPipe_->setMaximumSize(0, 0);
        connect(testTrustAnchorDialogQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            QString rec =
                pdfe::ui::TrustAnchorDialog::lastTestIntrospection();
            testTrustAnchorDialogResultPipe_->blockSignals(true);
            testTrustAnchorDialogResultPipe_->setText(
                rec.isEmpty() ? QStringLiteral("__null__") : rec);
            testTrustAnchorDialogResultPipe_->blockSignals(false);
            testTrustAnchorDialogQueryPipe_->blockSignals(true);
            testTrustAnchorDialogQueryPipe_->clear();
            testTrustAnchorDialogQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline f3c91a2e phase-7 (GAP-A14): generic dialog-introspection
    // readback. Python writes a dialog objectName to the query pipe; the
    // handler returns that dialog's recorded child-widget inventory from
    // the TestDialogSeam.h registry. Covers every dialog instrumented with
    // pdfe::ui::testseam::registerDialogIntrospection(). Gated by
    // PDFE_TEST_PIPES; nullptr in production.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testDialogIntrospectionResultPipe_ = new QLineEdit(this);
        testDialogIntrospectionResultPipe_->setObjectName(
            "testDialogIntrospectionResultPipe");
        testDialogIntrospectionResultPipe_->setAccessibleName(
            "Test Dialog Introspection Result");
        testDialogIntrospectionResultPipe_->resize(0, 0);
        testDialogIntrospectionResultPipe_->setFocusPolicy(Qt::NoFocus);
        testDialogIntrospectionResultPipe_->setMaximumSize(0, 0);
        testDialogIntrospectionResultPipe_->setReadOnly(true);

        testDialogIntrospectionQueryPipe_ = new QLineEdit(this);
        testDialogIntrospectionQueryPipe_->setObjectName(
            "testDialogIntrospectionQueryPipe");
        testDialogIntrospectionQueryPipe_->setAccessibleName(
            "Test Dialog Introspection Query");
        testDialogIntrospectionQueryPipe_->resize(0, 0);
        testDialogIntrospectionQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testDialogIntrospectionQueryPipe_->setMaximumSize(0, 0);
        connect(testDialogIntrospectionQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& dialogName) {
            if (dialogName.isEmpty()) return;
            QString rec = pdfe::ui::testseam::dialogIntrospection(dialogName);
            testDialogIntrospectionResultPipe_->blockSignals(true);
            testDialogIntrospectionResultPipe_->setText(
                rec.isEmpty() ? QStringLiteral("__null__") : rec);
            testDialogIntrospectionResultPipe_->blockSignals(false);
            testDialogIntrospectionQueryPipe_->blockSignals(true);
            testDialogIntrospectionQueryPipe_->clear();
            testDialogIntrospectionQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline a7c4f891 phase-2 (D4 / P-GAP-82): hidden pipe for
    // BookmarkPanel::insertBookmark. Python tests write
    // "pageIndex,title" — handler parses and calls
    // bookmarkPanel_->insertBookmark(pageIndex, title). Gated by
    // PDFE_TEST_PIPES. Provides a genuine PASS path for P-GAP-82
    // independent of QMenu UIA reachability (which remains
    // P-GAP-88 family).
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testInsertBookmarkPipe_ = new QLineEdit(this);
        testInsertBookmarkPipe_->setObjectName("testInsertBookmarkPipe");
        testInsertBookmarkPipe_->setAccessibleName("Test Insert Bookmark Pipe");
        testInsertBookmarkPipe_->resize(0, 0);
        testInsertBookmarkPipe_->setFocusPolicy(Qt::NoFocus);
        testInsertBookmarkPipe_->setMaximumSize(0, 0);
        connect(testInsertBookmarkPipe_, &QLineEdit::textChanged,
                this, [this](const QString& payload) {
            if (payload.isEmpty()) return;
            const int comma = payload.indexOf(',');
            if (comma < 1) {
                testInsertBookmarkPipe_->blockSignals(true);
                testInsertBookmarkPipe_->clear();
                testInsertBookmarkPipe_->blockSignals(false);
                return;
            }
            bool ok = false;
            const int pageIndex = payload.left(comma).trimmed().toInt(&ok);
            const QString title = payload.mid(comma + 1);
            if (ok && bookmarkPanel_) {
                bookmarkPanel_->insertBookmark(pageIndex, title);
            }
            testInsertBookmarkPipe_->blockSignals(true);
            testInsertBookmarkPipe_->clear();
            testInsertBookmarkPipe_->blockSignals(false);
        });
    }

    // Pipeline a7c4f891 phase-2 (D3 / P-GAP-84): hidden query+result
    // pipe pair for reading PdfCanvas::viewRotation(). Python tests
    // write any non-empty token to the query pipe; handler reads
    // canvas_->viewRotation() and writes the integer (as a string —
    // 0/90/180/270) to the result pipe. Unblocks the rotation
    // accessor gap documented in P-GAP-84 (test_ui_view_rotate.py
    // + test_view_rotate_cw_90).
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testCanvasRotationResultPipe_ = new QLineEdit(this);
        testCanvasRotationResultPipe_->setObjectName(
            "testCanvasRotationResultPipe");
        testCanvasRotationResultPipe_->setAccessibleName(
            "Test Canvas Rotation Result");
        testCanvasRotationResultPipe_->resize(0, 0);
        testCanvasRotationResultPipe_->setFocusPolicy(Qt::NoFocus);
        testCanvasRotationResultPipe_->setMaximumSize(0, 0);
        testCanvasRotationResultPipe_->setReadOnly(true);

        testCanvasRotationQueryPipe_ = new QLineEdit(this);
        testCanvasRotationQueryPipe_->setObjectName(
            "testCanvasRotationQueryPipe");
        testCanvasRotationQueryPipe_->setAccessibleName(
            "Test Canvas Rotation Query");
        testCanvasRotationQueryPipe_->resize(0, 0);
        testCanvasRotationQueryPipe_->setFocusPolicy(Qt::NoFocus);
        testCanvasRotationQueryPipe_->setMaximumSize(0, 0);
        connect(testCanvasRotationQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& token) {
            if (token.isEmpty()) return;
            int rotation = -1;
            if (canvas_) {
                rotation = canvas_->viewRotation();
            }
            testCanvasRotationResultPipe_->blockSignals(true);
            testCanvasRotationResultPipe_->setText(QString::number(rotation));
            testCanvasRotationResultPipe_->blockSignals(false);
            testCanvasRotationQueryPipe_->blockSignals(true);
            testCanvasRotationQueryPipe_->clear();
            testCanvasRotationQueryPipe_->blockSignals(false);
        });
    }

    // Pipeline b5e3a9f2 phase-4 (GAP-51): hidden pipe for invoking
    // Document::saveAs() programmatically. Python tests write an
    // absolute target path; the handler calls document_->saveAs(path)
    // directly, bypassing the QFileDialog + native Windows
    // overwrite-confirmation modal that integration tests can't
    // reliably drive. GAP-51 was not a product bug — Document::saveAs()
    // overwrites correctly (proven by SaveTest.SaveAs_OverwritesExistingFile).
    // The bug was the integration test driver.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testSaveAsPipe_ = new QLineEdit(this);
        testSaveAsPipe_->setObjectName("testSaveAsPipe");
        testSaveAsPipe_->setAccessibleName("Test Save As Pipe");
        testSaveAsPipe_->resize(0, 0);
        testSaveAsPipe_->setFocusPolicy(Qt::NoFocus);
        testSaveAsPipe_->setMaximumSize(0, 0);
        connect(testSaveAsPipe_, &QLineEdit::textChanged,
                this, [this](const QString& path) {
            if (path.isEmpty()) return;
            if (!document_ || !document_->isPdf()) {
                // Clear so the next call can fire; no error path
                // because the pipe is best-effort for tests.
                testSaveAsPipe_->blockSignals(true);
                testSaveAsPipe_->clear();
                testSaveAsPipe_->blockSignals(false);
                return;
            }
            // Pipeline f7c2a4e8 phase-2 (A5): defense-in-depth — the pipe
            // refuses inputs that Document::saveAs() would also reject.
            // Listed here explicitly so the pipe's safety contract is
            // discoverable without tracing into core/Document.cpp.
            if (path.contains(QChar(u'\0'))
                || QFileInfo(path).fileName().contains(QLatin1Char(':'))) {
                testSaveAsPipe_->blockSignals(true);
                testSaveAsPipe_->clear();
                testSaveAsPipe_->blockSignals(false);
                return;
            }
            try {
                std::filesystem::path target = qStringToPath(path);
                if (bookmarkPanel_) bookmarkPanel_->commitToDocument(document_.get());
                // Route through the production drain/rebind path exactly like
                // onSaveAsFile (non-flush branch): drop the active tool's page
                // holder, drain all Page holders so the write is unlocked, run
                // Document::saveAs, then re-resolve and rebind the undo commands
                // — and refresh caches WITHOUT dropping the rebound pages
                // (preserveCommandPages=true). Using the raw document_->saveAs
                // here bypassed the drain, so the journey-op99 UAF never
                // surfaced on the test path; this makes the pipe exercise the
                // real save so a save failure is observable.
                toolPage_.reset();
                pdfe::core::saveAsWithCommandPageRebind(
                    *document_, *undoManager_, commandPages_, target);
                onDocumentStructureChanged(/*preserveCommandPages=*/true);
                currentFileName_ = path;
                // Same as onSaveAsFile: a real path replaces any placeholder
                // display name (e.g. "Untitled" from File > New).
                documentDisplayName_.clear();
                undoManager_->setClean();
                updateWindowTitle();
            } catch (...) {
                // Swallow — tests check file state on disk, not exceptions.
            }
            testSaveAsPipe_->blockSignals(true);
            testSaveAsPipe_->clear();
            testSaveAsPipe_->blockSignals(false);
        });
    }

    // Pipeline f3c91a2e phase-7 (GAP-A14 / SKIP=FAIL): two write-only pipes
    // that convert the file_dialog_flows suite from skip-on-native-modal to
    // genuine end-to-end passes.
    //
    //  * testFileDialogPath — a test pushes the absolute path it wants the
    //    NEXT QFileDialog::getSaveFileName/getOpenFileName to return. The
    //    call sites consult pdfe::ui::testseam::takeFileDialogPath() first,
    //    so no native #32770 shell modal is shown.
    //  * testDialogAction — a test enqueues an action script for a modal
    //    dialog BEFORE opening it (while this window is still non-modal so
    //    the pipe receives the UIA value event). Format:
    //      "<dialogObjectName>|<verb1>|<verb2>|..."
    //    e.g. "ComplianceDialog|combo:LevelCombo=PDF/A-2b|click:ConvertButton"
    //    registerDialogIntrospection() replays it on the dialog's own loop.
    //
    // Both are children of MainWindow, written while it is non-modal, and are
    // inert unless PDFE_TEST_PIPES is set.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        auto* fileDialogPipe = new QLineEdit(this);
        fileDialogPipe->setObjectName("testFileDialogPathPipe");
        fileDialogPipe->setAccessibleName("Test File Dialog Path Pipe");
        fileDialogPipe->resize(0, 0);
        fileDialogPipe->setFocusPolicy(Qt::NoFocus);
        fileDialogPipe->setMaximumSize(0, 0);
        connect(fileDialogPipe, &QLineEdit::textChanged, this,
                [fileDialogPipe](const QString& path) {
            if (path.isEmpty()) return;
            pdfe::ui::testseam::enqueueFileDialogPath(path);
            fileDialogPipe->blockSignals(true);
            fileDialogPipe->clear();
            fileDialogPipe->blockSignals(false);
        });

        auto* dialogActionPipe = new QLineEdit(this);
        dialogActionPipe->setObjectName("testDialogActionPipe");
        dialogActionPipe->setAccessibleName("Test Dialog Action Pipe");
        dialogActionPipe->resize(0, 0);
        dialogActionPipe->setFocusPolicy(Qt::NoFocus);
        dialogActionPipe->setMaximumSize(0, 0);
        connect(dialogActionPipe, &QLineEdit::textChanged, this,
                [dialogActionPipe](const QString& spec) {
            if (spec.isEmpty()) return;
            const QStringList parts = spec.split(QLatin1Char('|'));
            if (parts.size() >= 2) {
                const QString key = parts.first();
                const QStringList script = parts.mid(1);
                pdfe::ui::testseam::enqueueDialogActions(key, script);
            }
            dialogActionPipe->blockSignals(true);
            dialogActionPipe->clear();
            dialogActionPipe->blockSignals(false);
        });

        // Attachment-panel driver (GAP-A14 / SKIP=FAIL): the attachments
        // dock's QTreeWidget cannot be enumerated reliably over UIA on the CI
        // session. This query/result pair exposes the panel's test seams.
        // Commands written to the query pipe (result read back from the
        // result pipe via UIA ValuePattern):
        //   "names"        -> "name1|name2|..." (empty string if none)
        //   "select:NAME"  -> "1" if selected, else "0"
        //   "add"          -> runs the real Add handler -> "ok"
        //   "extract"      -> runs the real Extract handler -> "ok"
        // For add/extract, push the file/dir path via testFileDialogPathPipe
        // first; the handler consumes it instead of showing a native modal.
        auto* attachResultPipe = new QLineEdit(this);
        attachResultPipe->setObjectName("testAttachmentResultPipe");
        attachResultPipe->setAccessibleName("Test Attachment Result Pipe");
        attachResultPipe->resize(0, 0);
        attachResultPipe->setFocusPolicy(Qt::NoFocus);
        attachResultPipe->setMaximumSize(0, 0);

        auto* attachQueryPipe = new QLineEdit(this);
        attachQueryPipe->setObjectName("testAttachmentQueryPipe");
        attachQueryPipe->setAccessibleName("Test Attachment Query Pipe");
        attachQueryPipe->resize(0, 0);
        attachQueryPipe->setFocusPolicy(Qt::NoFocus);
        attachQueryPipe->setMaximumSize(0, 0);
        connect(attachQueryPipe, &QLineEdit::textChanged, this,
                [this, attachQueryPipe, attachResultPipe](const QString& cmd) {
            if (cmd.isEmpty()) return;
            QString result = QStringLiteral("err:no-panel");
            if (attachmentPanel_) {
                if (cmd == QLatin1String("names")) {
                    QStringList ns;
                    for (const QString& n :
                         attachmentPanel_->attachmentNamesForTesting()) {
                        ns << n;
                    }
                    result = ns.join(QLatin1Char('|'));
                } else if (cmd.startsWith(QLatin1String("select:"))) {
                    const QString name = cmd.mid(7);
                    result = attachmentPanel_->selectAttachmentForTesting(name)
                                 ? QStringLiteral("1")
                                 : QStringLiteral("0");
                } else if (cmd == QLatin1String("add")) {
                    attachmentPanel_->triggerAddForTesting();
                    result = QStringLiteral("ok");
                } else if (cmd == QLatin1String("extract")) {
                    attachmentPanel_->triggerExtractForTesting();
                    result = QStringLiteral("ok");
                } else {
                    result = QStringLiteral("err:unknown-cmd");
                }
            }
            attachResultPipe->blockSignals(true);
            attachResultPipe->setText(result);
            attachResultPipe->blockSignals(false);
            attachQueryPipe->blockSignals(true);
            attachQueryPipe->clear();
            attachQueryPipe->blockSignals(false);
        });
    }

    // Create UI
    createActions();
    createMenus();
    createToolBar();
    createAnnotationToolBar();
    // Pipeline E phase-10: replay any saved toolbar customization on top
    // of the just-built factory toolbars.
    loadSavedToolbarLayout();
    createStatusBar();
    // Create undo manager BEFORE createDockPanels so panels that need it
    // (LayerPanel) receive it at construction time
    undoManager_ = new pdfe::commands::UndoManager(this);

    createDockPanels();

    // Set accessible names on dock panels
    setupAccessibleNames();

    // Set up panel focus shortcuts (Alt+1-4)
    setupPanelShortcuts();

    // Connect undo manager signals
    connect(undoManager_, &pdfe::commands::UndoManager::undoStateChanged, this, [this]() {
        const bool canUndo = undoManager_->canUndo();
        const bool canRedo = undoManager_->canRedo();
        const bool undoEnabledChanged = (undoAction_->isEnabled() != canUndo);
        const bool redoEnabledChanged = (redoAction_->isEnabled() != canRedo);

        undoAction_->setEnabled(canUndo);
        redoAction_->setEnabled(canRedo);

        if (canUndo) {
            undoAction_->setText(tr("&Undo %1").arg(undoManager_->undoDescription()));
        } else {
            undoAction_->setText(tr("&Undo"));
        }

        if (canRedo) {
            redoAction_->setText(tr("&Redo %1").arg(undoManager_->redoDescription()));
        } else {
            redoAction_->setText(tr("&Redo"));
        }

        // Pipeline b3f19d72 phase-3 (P7-BUG-1): explicitly push UIA
        // state-change notifications so external automation drivers
        // (pywinauto, WinAppDriver, FlaUI) observe the QAction's
        // enabled-state transition without waiting for the Qt6 UIA
        // bridge's lazy cache refresh.  QAction::setEnabled() alone
        // updates the QAction's internal flag and the QToolButton/
        // QMenu proxies visually react, but the platform-accessibility
        // bus does not always re-publish the IsEnabled property in
        // time for automated tests polling at ~5 Hz.
        //
        // Three notifications are pushed:
        //   1. The QAction itself (when found via Edit menu).
        //   2. Every QToolButton on every QToolBar that this QAction
        //      is associatedActions() on (when found via toolbar).
        //   3. The MainWindow itself (cascades a re-enumeration).
        auto pushStateChange = [](QObject* target) {
            if (!target) return;
            QAccessible::State changed;
            changed.disabled = true;
            QAccessibleStateChangeEvent ev(target, changed);
            QAccessible::updateAccessibility(&ev);
        };
        auto pushForActionAndProxies = [&](QAction* action, bool enabledChanged) {
            if (!enabledChanged || !action) return;
            pushStateChange(action);
            // Walk all toolbars and find the QToolButton wrapping this
            // QAction.  The toolbar proxies are what pywinauto's UIA
            // backend typically resolves "auto_id=undoAction" to when
            // the toolbar is visible (Qt6's UIA bridge stamps the
            // QToolButton with the underlying QAction's objectName).
            for (QToolBar* tb : findChildren<QToolBar*>()) {
                if (QWidget* w = tb->widgetForAction(action)) {
                    pushStateChange(w);
                    // Also force a repaint so Qt6's UIA bridge re-runs
                    // its property cache against the current QWidget
                    // state.  Empirically the IsEnabled property does
                    // not always propagate to UIAutomationCore until a
                    // paint/show cycle re-validates the widget.
                    w->update();
                }
            }
        };
        pushForActionAndProxies(undoAction_, undoEnabledChanged);
        pushForActionAndProxies(redoAction_, redoEnabledChanged);
    });

    // Connect canvas signals
    connect(canvas_, &PdfCanvas::pageChanged, this, &MainWindow::onPageChanged);
    connect(canvas_, &PdfCanvas::zoomChanged, this, &MainWindow::onZoomChanged);
    connect(canvas_, &PdfCanvas::annotationCreated, this, &MainWindow::onAnnotationCreatedByTool);
    connect(canvas_, &PdfCanvas::canvasClicked, this,
        [this](int pageIndex, QPointF pdfPoint) {
            followLinkAt(pageIndex, pdfPoint);
        });
    connect(canvas_, &PdfCanvas::zoomChanged, this, [this](float) {
        repositionStickyNotePopup();
    });
    connect(canvas_, &PdfCanvas::scrolled, this, [this]() {
        repositionStickyNotePopup();
    });

    // Pipeline a7c2e541 phase-17 test-automation hook: PdfCanvas's
    // Q_INVOKABLE addRedactionMarkAtPdfRect emits this signal. Route
    // it through the same CreateRedactionCommand path as a user drag
    // so per-mark undo behaviour is preserved (closes P11-BUG-2 /
    // P11-INFRA-1 / P15-INFRA-2 test-side debt).
    connect(canvas_, &PdfCanvas::redactionMarkRequested, this,
        [this](int pageIndex, QRectF pdfRect) {
            if (!document_ || !redactionManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto* pagePtr = page.get();
            commandPages_.push_back(std::move(page));

            auto cmd = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
                redactionManager_.get(), pagePtr, pdfRect);
            undoManager_->execute(std::move(cmd));

            // Refresh redaction panel and enable apply action
            auto marks = redactionManager_->getAllRedactionMarks(document_.get());
            if (redactionPanel_) redactionPanel_->setMarks(marks);
            if (applyRedactionsAction_) applyRedactionsAction_->setEnabled(!marks.empty());

            if (canvas_) canvas_->refreshPage(pageIndex);
            updateWindowTitle();
        });

    // Defect #4 (per-region tool routing): on a mixed-content page the text
    // tool is the page default, but a click on a scanned image block with no
    // live text span under it belongs to the region/TRE tool. PdfCanvas emits
    // regionToolRequested for that click; switch tools here so the routing is
    // per-location, not the page-wide hasTextContent() decision.
    connect(canvas_, &PdfCanvas::regionToolRequested, this,
        [this](int pageIndex, QPointF /*pdfPoint*/) {
            if (!document_) return;
            if (pageIndex < 0 || pageIndex >= document_->pageCount()) return;
            initTreComponents();
            if (treToolAction_) {
                onAnnotationToolSelected(treToolAction_);
            }
        });

    // Default canvas right-click context menu (UI-CTX-01). Tool-specific
    // right-click flows (text-edit / annotation tools) handle their own
    // contextMenuEvent before this signal fires.
    connect(canvas_, &PdfCanvas::canvasContextMenuRequested, this,
            [this](QPoint screenPos) {
        QMenu menu(this);
        if (zoomInAction_)    menu.addAction(zoomInAction_);
        if (zoomOutAction_)   menu.addAction(zoomOutAction_);
        if (actualSizeAction_) menu.addAction(actualSizeAction_);
        if (fitWidthAction_)   menu.addAction(fitWidthAction_);
        if (fitPageAction_)    menu.addAction(fitPageAction_);
        menu.addSeparator();
        if (previousViewAction_) menu.addAction(previousViewAction_);
        if (nextViewAction_)     menu.addAction(nextViewAction_);
        // Pipeline F phase-5: surface Alt Text editor for accessibility.
        // Only show when at least one annotation exists on the current
        // page so the dialog has a target.
        if (altTextAction_ && document_ && annotationManager_ && canvas_) {
            int pageIdx = canvas_->currentPage();
            auto page = document_->page(pageIdx);
            const int annotCount = page
                ? static_cast<int>(
                    annotationManager_->getAnnotations(page.get()).size())
                : 0;
            if (annotCount > 0) {
                menu.addSeparator();
                menu.addAction(altTextAction_);
            }
        }
        if (menu.actions().isEmpty()) return;
        menu.exec(screenPos);
    });

    // Restore window state from settings.
    //
    // GitHub #136: under PDFE_TEST_DETERMINISTIC_LAYOUT this restore is
    // SKIPPED and a fixed dock layout applied instead. The persisted layout is
    // per-user-profile state, so it is the reason the visual-regression
    // baselines never matched: the dock widths moved, the canvas rectangle
    // moved with them, and the screenshot compare was measuring furniture.
    // See testDeterministicLayoutEnabled() in TestDialogSeam.h for the
    // measurement. Statically dead under -DPDFE_TESTING=OFF.
    if (pdfe::ui::testseam::testDeterministicLayoutEnabled()) {
        applyDeterministicTestLayout();
    } else {
        QByteArray savedGeometry = appSettings_->windowGeometry();
        if (!savedGeometry.isEmpty()) {
            restoreGeometry(savedGeometry);
        }
        QByteArray savedState = appSettings_->windowState();
        if (!savedState.isEmpty()) {
            restoreState(savedState);
        }
    }

    // bugfix-3e91a7c4 #3: enforce Pages as the default left-dock tab AFTER
    // restoreState() (which may reapply a stale Layers-raised / Pages-hidden
    // layout). Must come last so the restore can't override it.
    ensurePagesDockDefault();

    // Update UI state
    updateWindowTitle();

    // R3-04: after the window is up, check for any session that did not exit
    // cleanly (an autosave recovery sidecar left behind) and offer to recover.
    // Deferred via singleShot so the prompt appears after the event loop starts
    // rather than mid-construction.
    recoveryTrace("constructor reached the recovery scheduling point");
    QTimer::singleShot(0, this, [this]() {
        recoveryTrace("deferred recovery check firing");
        checkForRecoverableSessions();
    });

    // F10 / WIN-68 + WIN-70: subscribe to the session manager. On Windows Qt
    // raises commitDataRequest from its WM_QUERYENDSESSION handling, which is
    // the ONLY notice we get before a logoff/shutdown/restart terminates us —
    // no QCloseEvent is delivered, so the unsaved-changes prompt in
    // closeEvent() would never run. DirectConnection because the session
    // manager gives us a synchronous window in which to answer; a queued slot
    // would run after the decision has already been made.
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::commitDataRequest, this,
                [this](QSessionManager& manager) {
                    if (handleSessionEnd(manager.allowsInteraction())
                        == SessionEndDecision::Cancel) {
                        manager.cancel();
                    }
                },
                Qt::DirectConnection);
    }

    // GitHub #213 / WIN-71 — react to a machine suspend.
    //
    // A suspend is a session end that intends to come back, and it shares the
    // same hazard: the process can be frozen and then never resumed (battery
    // dies, the VM is discarded, hibernate image is dropped), in which case
    // unsaved work is gone exactly as it would be on a `shutdown /f`. So the
    // reaction is deliberately the SAME first step handleSessionEnd() takes —
    // publish a recovery snapshot — reached through the same AutosaveManager.
    //
    // It is ONLY that first step. There is no prompt: Windows allows an
    // application roughly two seconds on PBT_APMSUSPEND and does not wait for
    // a dialog, and there is no user present to answer one at the moment a lid
    // closes. There is deliberately NO resume-side action at all; see
    // docs/power-event-handling-scope.md.
    pdfe::app::PowerEventCoordinator::instance().setSuspendHook([this]() {
        if (autosaveManager_) {
            autosaveManager_->autosaveNow();
        }
    });
}

MainWindow::SessionEndDecision MainWindow::handleSessionEnd(bool allowsInteraction)
{
    if (!hasUnsavedChanges()) {
        return SessionEndDecision::Proceed;
    }

    // STEP 1 — make the work recoverable BEFORE anything that can block.
    // The session manager may terminate us at any point from here on and is
    // under no obligation to wait for a dialog (`shutdown /f`, a policy-driven
    // logoff, a UPS-triggered shutdown all forbid interaction entirely). The
    // snapshot — not the prompt — is what makes the guarantee "unsaved changes
    // are never lost SILENTLY": whatever happens next, the next launch's
    // recovery scan surfaces this session.
    if (autosaveManager_) {
        autosaveManager_->autosaveNow();
    }

    // STEP 2 — if we are allowed to talk to the user, offer the normal
    // save / discard / cancel choice.
    if (!allowsInteraction) {
        return SessionEndDecision::Proceed;
    }

    switch (promptSaveOnClose()) {
    case CloseAction::Cancel:
        return SessionEndDecision::Cancel;
    case CloseAction::Save:
        onSaveFile();
        // A failed save must not let the session end quietly — abort the
        // shutdown so the user can deal with it (same rule as closeEvent).
        return hasUnsavedChanges() ? SessionEndDecision::Cancel
                                   : SessionEndDecision::Proceed;
    case CloseAction::Discard:
        break;
    }
    return SessionEndDecision::Proceed;
}

bool MainWindow::hasRecoverySnapshotForTesting() const
{
    return autosaveManager_ && autosaveManager_->hasRecovery();
}

void MainWindow::clearRecoveryForTesting()
{
    if (autosaveManager_) {
        autosaveManager_->clearRecovery();
    }
}

MainWindow::~MainWindow()
{
    // GitHub #213 / WIN-71 — drop the suspend hook FIRST. The coordinator is
    // process-wide and the hook captures `this`; a suspend arriving after this
    // window is gone would otherwise call into freed memory. Unconditional
    // rather than "only if we installed it": the coordinator holds exactly one
    // hook, so in the test binary (where MainWindows come and go) the last
    // window to be destroyed must leave it empty regardless of ordering.
    pdfe::app::PowerEventCoordinator::instance().setSuspendHook({});

    // F24 — release this session's claim on the certification save guard. The
    // guard is a property of a RUNNING PDFE session (it is installed in the
    // constructor); leaving it installed after the window is gone would impose
    // a UI policy on an embedder — or, in the test binary, on every later test
    // in the shard. installDocMdpSaveGuard/uninstall are reference-counted, so
    // a second live MainWindow keeps it installed.
    pdfe::signatures::uninstallDocMdpSaveGuard();

    // Tear down the text-edit tool before any owned member (document_,
    // muPdfContext_) is destroyed. TextEditTool holds raw document_/context_
    // pointers and its ~TextEditTool runs cancelEdit(); because canvas_ is a
    // child QWidget it is otherwise destroyed later (in ~QObject) — after
    // document_ has already been freed — so cancelEdit() would dereference a
    // dangling document/context (0xC0000005). closeEvent() already does this
    // for the normal window-close path; doing it here too makes direct
    // destruction (no closeEvent, e.g. programmatic teardown) equally safe.
    // Idempotent: clearing an already-cleared tool is a no-op.
    if (canvas_) {
        canvas_->clearTextEditTool();
    }

    // Pipeline E phase-9: break the document JS-event callback link
    // before any member is torn down. Field destruction order would
    // otherwise let document_ outlive jsEngine_ and the willClose
    // callback (lambda capturing jsEngine_.get()) would deref a
    // dangling pointer. closeDocument() fires willClose explicitly
    // when used; this path covers MainWindow destruction without
    // an explicit close.
    if (document_) {
        document_->setDocumentEventCallback(nullptr);
    }

    // Clear the undo/redo stacks while the members the queued commands point
    // into (imageManager_, document_, commandPages_) are STILL ALIVE. Commands
    // hold raw pointers into those members and their destructors call back into
    // them — e.g. InsertImageCommand::~InsertImageCommand() runs
    // imageManager_->releaseRef(savedContentsRef_). Because undoManager_ is a
    // QObject CHILD of this window, it is otherwise deleted in ~QObject (base
    // class) AFTER every MainWindow member destructor has already run, so those
    // callbacks would dereference freed memory (0xC0000005). closeDocument()
    // does this for the normal close/reopen path; doing it here makes DIRECT
    // destruction (no closeEvent — e.g. programmatic teardown, or paste-then-
    // reset in a test) equally safe. Idempotent: clearing empty stacks is a
    // no-op.
    if (undoManager_) {
        undoManager_->clear();
    }

    // Persist the image-stamp library so user-imported stamps survive
    // between sessions. (Pipeline E phase-4)
    if (imageStampLibrary_) {
        imageStampLibrary_->saveToSettings();
    }
}

// WIN-21: is this URL something we could actually open?
//
// `isLocalFile()` is false for every payload Explorer produces when the item
// being dragged is not a file on disk — a shell VIRTUAL FILE out of a
// compressed folder, an FTP location, a library view. Those carry
// CFSTR_FILEDESCRIPTOR/CFSTR_FILECONTENTS instead of a uri-list, so the
// URL list is either empty or non-local, and both must fall through.
//
// The remote check is LEXICAL-plus-drive-resolution, not a filesystem probe:
// touching a UNC path to find out whether it exists is the very NetBIOS/LLMNR
// broadcast the gate exists to prevent, so a remote target is refused before
// anything stats it.
static bool droppedUrlNamesACandidatePdf(const QUrl& url, QString& pathOut)
{
    if (!url.isValid() || url.isEmpty()) return false;
    if (!url.isLocalFile()) return false;
    const QString path = url.toLocalFile();
    if (path.isEmpty()) return false;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) return false;
    if (pdfe::core::path_security::is_unc_or_remote(path.toStdWString())) return false;
    pathOut = path;
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    // Accept the drag only when the payload includes at least one local,
    // non-remote file URL with a .pdf suffix. Other drops (text, image data,
    // shell virtual files) fall through unhandled. (UI-DD-01, WIN-21)
    if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
        return;
    }
    QString path;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (droppedUrlNamesACandidatePdf(url, path)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    // Open one .pdf from the drop. Multi-file drops open a single document;
    // the rest are ignored — Acrobat behaves the same. (UI-DD-01)
    if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
        return;
    }

    // WIN-21: prefer the first candidate that IS THERE, rather than the first
    // one whose name ends in .pdf.
    //
    // A drag out of a compressed folder can yield several entries of which
    // only some have been extracted to a scratch path by the time the drop
    // arrives, and the shell does not order them helpfully. The old loop
    // committed to the first .pdf-suffixed URL and returned, so one
    // unextracted entry ahead of a readable one turned the whole drop into a
    // "could not be found" dialog — with the file the user could see in the
    // window right there in the payload, unopened.
    //
    // The fallback matters as much as the preference: if NOTHING in the drop
    // exists, we still hand the first candidate to openFile so the user gets
    // the specific "no file exists at this location" report. Falling through
    // silently would make a drop that PDFE understood perfectly well look
    // like one it ignored.
    // GitHub #241 — a drop of several PDFs used to open exactly one and discard
    // the rest, the same singular-value defect the command line had. Collect
    // every candidate now, open the first that EXISTS in this window (unchanged
    // behaviour, including the fallback below), and offer the remainder to the
    // window manager so each gets a window of its own.
    QString firstCandidate;
    QString path;
    QStringList existing;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!droppedUrlNamesACandidatePdf(url, path)) continue;
        if (firstCandidate.isEmpty()) firstCandidate = path;
        if (QFileInfo(path).isFile()) {
            existing.append(path);
        }
    }

    if (!existing.isEmpty()) {
        // Route through the window manager when there IS one, and route ALL of
        // the files through it — including the first.
        //
        // Opening the first here directly and only emitting for 2..N was wrong,
        // and wrong in the one way that matters: the manager never learned this
        // window had taken that document, so a drop containing the same file
        // twice opened a SECOND editor on it. That is the concurrent-save
        // data-loss window R3-02 exists to close, reintroduced by the fix for
        // #241 through a door #241 did not come in by. Caught by
        // MultiDocumentOpenTest.ADropContainingADuplicateOpensItOnce.
        //
        // With a manager, openPath() puts the first file in this window anyway
        // when this is the only empty window — the ordinary drop-onto-an-empty-
        // editor case — and gives the rest their own.
        // An explicit flag rather than isSignalConnected().
        //
        // Why, honestly: isSignalConnected() was used first, and it appeared to
        // report NO receiver with the manager's connection in place — which would
        // mean a managed window silently taking the single-window fallback. That
        // reading was an artefact of STALE OBJECT FILES: adding a member to this
        // class changed its layout, and translation units that had not been
        // recompiled read the flag at the wrong offset. After a clean rebuild the
        // symptom vanished, so the introspection was probably innocent.
        //
        // The flag stays anyway, on its own merits: a caller that owns windows
        // and routes drops should SAY so rather than leave the window to infer it
        // from the shape of its own connections. The inference bought nothing and
        // failed silently — its fallback looks exactly like the defect this fixes.
        if (dropRoutingDelegated_) {
            for (const QString& candidate : existing) {
                emit openPathRequested(candidate);
            }
        } else {
            // No manager listening: a bare MainWindow, which is every widget
            // test and any embedding that wants the old single-window
            // behaviour. Open the first and discard the rest, exactly as before
            // — see UiClipboardDragDropTest.MultiUrlDropOpensExactlyTheFirstPdf.
            openFile(existing.first());
        }
        event->acceptProposedAction();
        return;
    }

    // Nothing in the drop exists. Hand the first candidate to openFile anyway
    // so the user gets the specific "no file exists at this location" report
    // rather than a drop that looks ignored.
    if (!firstCandidate.isEmpty()) {
        openFile(firstCandidate);
        event->acceptProposedAction();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // UI-KB-07 — prompt to save unsaved changes before exit.
    // Skip the prompt when there is no document or no pending edits.
    if (hasUnsavedChanges()) {
        switch (promptSaveOnClose()) {
        case CloseAction::Cancel:
            event->ignore();
            return;
        case CloseAction::Save:
            onSaveFile();
            // If save failed (file still dirty), abort the close so the
            // user doesn't lose the unsaved work.
            if (hasUnsavedChanges()) {
                event->ignore();
                return;
            }
            break;
        case CloseAction::Discard:
            // Fall through to normal close path.
            break;
        }
    }

    // Save window state
    if (appSettings_) {
        appSettings_->setWindowGeometry(saveGeometry());
        appSettings_->setWindowState(saveState());

        // Save panel visibility
        if (pagePanelDock_) {
            appSettings_->setPagePanelVisible(pagePanelDock_->isVisible());
        }
        if (propertiesPanelDock_) {
            appSettings_->setPropertiesPanelVisible(propertiesPanelDock_->isVisible());
        }
        if (searchPanelDock_) {
            appSettings_->setSearchPanelVisible(searchPanelDock_->isVisible());
        }
        if (bookmarkPanelDock_) {
            appSettings_->setBookmarkPanelVisible(bookmarkPanelDock_->isVisible());
        }
        appSettings_->sync();
    }

    // Destroy text edit tool before window destruction to avoid dangling pointers.
    // clearTextEditTool() calls deactivate() → commitEdit() which accesses
    // model/document — at destruction time, these may be partially destroyed.
    if (canvas_) {
        canvas_->clearTextEditTool();
    }

    QMainWindow::closeEvent(event);
}

bool MainWindow::hasUnsavedChanges() const
{
    return document_ != nullptr && undoManager_
        && !undoManager_->isClean();
}

MainWindow::CloseAction MainWindow::promptSaveOnClose()
{
    // Pipeline b3f19d72 phase-11 (test infrastructure): when set,
    // skip the unsaved-changes prompt and treat close as "Discard".
    // 100% inert in production — the env var is only set by
    // tests/ui_harness/core/launch.py::_qt_env_for_determinism().
    // The intent is to prevent modal dialog blocking that wedges UIA
    // tests mid-run and orphans PDFE.exe processes.
    if (qgetenv("PDFE_TEST_SKIP_UNSAVED_PROMPT") == "1") {
        return CloseAction::Discard;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unsaved changes"));
    box.setText(tr("The document has unsaved changes."));
    box.setInformativeText(tr("Do you want to save them before exiting?"));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard
                           | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);

    switch (box.exec()) {
    case QMessageBox::Save:    return CloseAction::Save;
    case QMessageBox::Discard: return CloseAction::Discard;
    default:                   return CloseAction::Cancel;
    }
}

// Diagnostic seam for the autosave-recovery startup path (GitHub #285 cluster G).
//
// Why a FILE and not qInfo(): the app's stderr stops reaching a parent harness
// once QApplication is up (only the pre-QApplication ProcessMitigations lines
// arrive), and LogHandler's file sink is off by default -- so a qInfo() here is
// invisible to a test, which is precisely why "the prompt never appears" stayed
// undecidable between "seam set", "scan empty" and "never called". Writing
// straight to a path the harness chooses bypasses the whole logging stack.
//
// Inert unless PDFE_RECOVERY_TRACE names a file, so it costs nothing in
// production and cannot itself change startup behaviour.
void MainWindow::recoveryTrace(const char* what)
{
    const QByteArray dest = qgetenv("PDFE_RECOVERY_TRACE");
    if (dest.isEmpty()) return;
    QFile f(QString::fromLocal8Bit(dest));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&f) << what << "\n";
}

// GitHub #295. One record per observed Ctrl+Z. Records the state that
// distinguishes the issue's two hypotheses, and records it at DELIVERY time
// because both halves of it move on immediately afterwards:
//
//   * focus/target  -- hypothesis 1, a focused child widget consuming Ctrl+Z
//                      for its own undo (a QLineEdit does exactly that, and
//                      beats a Qt::WindowShortcut-context action)
//   * undoEnabled   -- hypothesis 2, undoAction_ transiently disabled. It
//                      starts setEnabled(false) and is enabled from the undo
//                      stack's state, and a disabled QAction's shortcut does
//                      not fire while the pipe path would still find it
//                      enabled a moment later.
//
// canUndo is recorded alongside undoEnabled deliberately: if the stack can undo
// while the action is disabled, the two have drifted apart, which is a different
// defect from either hypothesis and would otherwise look like hypothesis 2.
void MainWindow::recordCtrlZTrace(const char* eventKind, QWidget* target)
{
    const auto describe = [](const QWidget* w) -> QString {
        if (!w) return QStringLiteral("none");
        const QString name = w->objectName().isEmpty()
                                 ? QStringLiteral("(unnamed)")
                                 : w->objectName();
        return name + QLatin1Char('[')
               + QString::fromLatin1(w->metaObject()->className())
               + QLatin1Char(']');
    };

    const QString record =
        QStringLiteral("event=") + QString::fromLatin1(eventKind)
        + QStringLiteral(" target=") + describe(target)
        + QStringLiteral(" focus=") + describe(QApplication::focusWidget())
        + QStringLiteral(" undoEnabled=")
        + ((undoAction_ && undoAction_->isEnabled()) ? QStringLiteral("1")
                                                     : QStringLiteral("0"))
        + QStringLiteral(" canUndo=")
        + ((undoManager_ && undoManager_->canUndo()) ? QStringLiteral("1")
                                                     : QStringLiteral("0"))
        + QStringLiteral(" undoDepth=")
        + QString::number(undoManager_ ? undoManager_->undoStackSize() : -1);

    testKeyTrace_.append(record);
    while (testKeyTrace_.size() > kTestKeyTraceMax) {
        testKeyTrace_.removeFirst();
    }
}

void MainWindow::checkForRecoverableSessions()
{
    recoveryTrace("checkForRecoverableSessions entered");
    // Inert under the UI-test determinism harness (which sets this env var):
    // a modal recovery prompt at startup would wedge automated UIA runs and
    // orphan PDFE.exe. Same seam used by promptSaveOnClose().
    //
    // The seam's VALUE is logged, not just its effect. GitHub #285 cluster G
    // spent a long time undecidable because "no prompt appeared" is produced
    // identically by the seam being set, the scan finding nothing, and the
    // function never running — and nothing distinguished them.
    recoveryTrace(qgetenv("PDFE_TEST_SKIP_UNSAVED_PROMPT") == "1"
                      ? "seam PDFE_TEST_SKIP_UNSAVED_PROMPT=1 -> suppressed"
                      : "seam absent -> will scan");
    if (qgetenv("PDFE_TEST_SKIP_UNSAVED_PROMPT") == "1") {
        // Beyond suppressing the prompt, proactively DISCARD any stale recovery
        // sidecars. A UI test that dirtied a document and was then force-killed
        // by PDFESession (per-test relaunch) never runs the clean-close
        // clearRecovery(), so its sidecar survives. Left in place it would fire
        // this recovery prompt on the NEXT launch via a path that does not set
        // this seam (e.g. the smoke / open-robustness launchers), wedging their
        // automated teardown. Clearing them here keeps every subsequent launch
        // on a clean slate.
        pdfe::core::AutosaveManager::discardAllRecoverable();
        return;
    }

    std::vector<pdfe::core::RecoveryRecord> records;
    try {
        records = pdfe::core::AutosaveManager::scanForRecoverable();
    } catch (...) {
        // Recovery is best-effort and must never block startup -- but a SILENT
        // swallow here is why GitHub #285 cluster G cost a day: the prompt
        // simply did not appear, and nothing anywhere said whether the scan had
        // thrown, found nothing, or was never reached. Say which.
        recoveryTrace("scanForRecoverable() THREW; no prompt offered");
        qWarning() << "[recovery] scanForRecoverable() THREW; no prompt offered";
        return;
    }
    recoveryTrace(QByteArray("scanForRecoverable() -> "
                             + QByteArray::number(qulonglong(records.size()))
                             + " record(s)").constData());
    if (records.empty()) {
        return;
    }

    // Offer to recover the most recent session (records are unordered; a single
    // consolidated prompt keeps the UX simple). Discarding drops all sidecars.
    const pdfe::core::RecoveryRecord& rec = records.front();
    const QString origName =
        rec.originalPath.empty()
            ? tr("an untitled document")
            : QFileInfo(QString::fromStdU16String(rec.originalPath.u16string()))
                  .fileName();

    QMessageBox box(this);
    box.setObjectName("recoveryPromptDialog");
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Recover unsaved changes"));
    box.setText(tr("PDFE found unsaved changes from a previous session "
                   "that did not close normally."));
    box.setInformativeText(
        tr("Recover the autosaved version of %1?").arg(origName));
    QPushButton* recoverBtn =
        box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    recoverBtn->setObjectName("recoveryRecoverButton");
    recoverBtn->setAccessibleName(tr("Recover unsaved changes"));
    QPushButton* discardBtn =
        box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    discardBtn->setObjectName("recoveryDiscardButton");
    discardBtn->setAccessibleName(tr("Discard recovered changes"));
    box.setDefaultButton(recoverBtn);
    box.exec();

    if (box.clickedButton() == recoverBtn) {
        // Open the recovery copy as an ordinary document. It carries the
        // unsaved edits; the user can then Save As back over the original.
        openFile(QString::fromStdU16String(rec.recoveryFilePath.u16string()));
        statusBar_->showMessage(
            tr("Recovered unsaved changes — use Save As to keep them"), 8000);
    } else {
        // Discard: remove all recovered sidecars + copies.
        for (const auto& r : records) {
            pdfe::core::AutosaveManager::discardRecovery(r);
        }
    }
}

void MainWindow::buildTextEditContextMenu(QMenu& menu,
                                           pdfe::textedit::TextEditTool* tool)
{
    const bool hasSel  = tool && tool->hasTextSelection();
    const bool hasSpan = tool && tool->hasActiveSpan();

    // Cut / Copy / Paste / Select All — UI-CTX-02. Action handlers
    // are wired here so triggering the action (via menu.exec OR via
    // QAction::trigger() in tests) dispatches directly to the tool —
    // no separate post-exec dispatch lambda is needed.
    auto* cutAction = menu.addAction(tr("Cut"));
    cutAction->setObjectName("textEditCutAction");
    cutAction->setShortcut(QKeySequence::Cut);
    cutAction->setEnabled(hasSel);
    if (tool) {
        connect(cutAction, &QAction::triggered,
                tool, [tool]() { tool->performCut(); });
    }

    auto* copyAction = menu.addAction(tr("Copy"));
    copyAction->setObjectName("textEditCopyAction");
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(hasSel);
    if (tool) {
        connect(copyAction, &QAction::triggered,
                tool, [tool]() { tool->performCopy(); });
    }

    auto* pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setObjectName("textEditPasteAction");
    pasteAction->setShortcut(QKeySequence::Paste);
    pasteAction->setEnabled(hasSpan);
    if (tool) {
        connect(pasteAction, &QAction::triggered,
                tool, [tool]() { tool->performPaste(); });
    }

    auto* selectAllAction = menu.addAction(tr("Select All"));
    selectAllAction->setObjectName("textEditSelectAllAction");
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    selectAllAction->setEnabled(hasSpan);
    if (tool) {
        connect(selectAllAction, &QAction::triggered,
                tool, [tool]() { tool->performSelectAll(); });
    }

    menu.addSeparator();

    // Bold / Italic — UI-CTX-11 (formatting on selection)
    auto* boldAction = menu.addAction(tr("Bold"));
    boldAction->setObjectName("textEditBoldAction");
    boldAction->setShortcut(QKeySequence::Bold);
    boldAction->setCheckable(true);
    boldAction->setEnabled(hasSpan);
    if (tool) {
        connect(boldAction, &QAction::triggered,
                tool, [tool]() { tool->applyBold(true); });
    }

    auto* italicAction = menu.addAction(tr("Italic"));
    italicAction->setObjectName("textEditItalicAction");
    italicAction->setShortcut(QKeySequence::Italic);
    italicAction->setCheckable(true);
    italicAction->setEnabled(hasSpan);
    if (tool) {
        connect(italicAction, &QAction::triggered,
                tool, [tool]() { tool->applyItalic(true); });
    }

    menu.addSeparator();

    // Find... — UI-CTX-11. Trigger forwards to the global findAction_
    // so the standard Find UI surfaces (Ctrl+F dialog, etc.).
    if (findAction_) {
        auto* findAction = menu.addAction(findAction_->text());
        findAction->setObjectName("textEditFindAction");
        findAction->setShortcut(findAction_->shortcut());
        connect(findAction, &QAction::triggered,
                this, [this]() {
                    if (findAction_) findAction_->trigger();
                });
    }

    // Atlas Mode submenu (preserved when atlas data is available).
    if (pendingAtlas_.ready) {
        menu.addSeparator();
        auto* modeMenu = menu.addMenu(tr("Atlas Mode"));
        modeMenu->setObjectName("textEditAtlasModeMenu");

        auto* scanAction = modeMenu->addAction(tr("Scan Glyphs"));
        scanAction->setObjectName("textEditAtlasScanAction");
        scanAction->setCheckable(true);
        scanAction->setChecked(currentAtlasMode_ == pdfe::tre::AtlasMode::ScanCropped);
        connect(scanAction, &QAction::triggered, this,
                [this]() { applyAtlasMode(pdfe::tre::AtlasMode::ScanCropped); });

        auto* fontAction = modeMenu->addAction(tr("Font Matched"));
        fontAction->setObjectName("textEditAtlasFontAction");
        fontAction->setCheckable(true);
        fontAction->setChecked(currentAtlasMode_ == pdfe::tre::AtlasMode::FreeTypeStyled);
        connect(fontAction, &QAction::triggered, this,
                [this]() { applyAtlasMode(pdfe::tre::AtlasMode::FreeTypeStyled); });

        auto* modeGroup = new QActionGroup(&menu);
        modeGroup->addAction(scanAction);
        modeGroup->addAction(fontAction);

        auto* compareAction = menu.addAction(tr("Compare Atlas Modes..."));
        compareAction->setObjectName("textEditAtlasCompareAction");
        connect(compareAction, &QAction::triggered, this,
                [this]() { showAtlasComparison(); });
    }
}

void MainWindow::createActions()
{
    // File menu actions
    newFileAction_ = new QAction(tr("&New"), this);
    newFileAction_->setObjectName("newFileAction");
    newFileAction_->setShortcut(QKeySequence::New);
    newFileAction_->setStatusTip(tr("Create a blank PDF document"));
    connect(newFileAction_, &QAction::triggered, this, &MainWindow::onNewFile);

    openAction_ = new QAction(tr("&Open..."), this);
    openAction_->setObjectName("openAction");
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setStatusTip(tr("Open a PDF file"));
    connect(openAction_, &QAction::triggered, this, &MainWindow::onOpenFile);

    saveAction_ = new QAction(tr("&Save"), this);
    saveAction_->setObjectName("saveAction");
    saveAction_->setShortcut(QKeySequence::Save);
    saveAction_->setStatusTip(tr("Save the current PDF"));
    saveAction_->setEnabled(false);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::onSaveFile);

    saveAsAction_ = new QAction(tr("Save &As..."), this);
    saveAsAction_->setObjectName("saveAsAction");
    saveAsAction_->setShortcut(QKeySequence::SaveAs);
    saveAsAction_->setStatusTip(tr("Save the PDF with a new name"));
    saveAsAction_->setEnabled(false);
    connect(saveAsAction_, &QAction::triggered, this, &MainWindow::onSaveAsFile);

    exportPlainTextAction_ = new QAction(tr("Export Plain &Text..."), this);
    exportPlainTextAction_->setObjectName("exportPlainTextAction");
    exportPlainTextAction_->setStatusTip(
        tr("Save the document's extracted text as a .txt file"));
    exportPlainTextAction_->setEnabled(false);
    connect(exportPlainTextAction_, &QAction::triggered,
            this, &MainWindow::onExportPlainText);

    createPdfFromImagesAction_ = new QAction(tr("Create PDF from &Images..."), this);
    createPdfFromImagesAction_->setObjectName("createPdfFromImagesAction");
    createPdfFromImagesAction_->setStatusTip(
        tr("Build a new PDF from one or more image files"));
    connect(createPdfFromImagesAction_, &QAction::triggered,
            this, &MainWindow::onCreatePdfFromImages);

    // Pipeline E phase-11: Combine Files...
    combineFilesAction_ = new QAction(tr("Combine &Files..."), this);
    combineFilesAction_->setObjectName("combineFilesAction");
    combineFilesAction_->setStatusTip(
        tr("Merge multiple PDF and image files into a single PDF"));
    connect(combineFilesAction_, &QAction::triggered,
            this, &MainWindow::onCombineFiles);

    previousViewAction_ = new QAction(tr("Pre&vious View"), this);
    previousViewAction_->setObjectName("previousViewAction");
    previousViewAction_->setShortcut(QKeySequence(tr("Alt+Left")));
    previousViewAction_->setStatusTip(tr("Navigate back through view history"));
    previousViewAction_->setEnabled(false);
    connect(previousViewAction_, &QAction::triggered,
            this, &MainWindow::onPreviousView);

    nextViewAction_ = new QAction(tr("Ne&xt View"), this);
    nextViewAction_->setObjectName("nextViewAction");
    nextViewAction_->setShortcut(QKeySequence(tr("Alt+Right")));
    nextViewAction_->setStatusTip(tr("Navigate forward through view history"));
    nextViewAction_->setEnabled(false);
    connect(nextViewAction_, &QAction::triggered,
            this, &MainWindow::onNextView);

    closeAction_ = new QAction(tr("&Close"), this);
    closeAction_->setObjectName("closeAction");
    closeAction_->setShortcut(QKeySequence(tr("Ctrl+W")));
    closeAction_->setStatusTip(tr("Close the current document"));
    closeAction_->setEnabled(false);
    connect(closeAction_, &QAction::triggered, this, &MainWindow::onCloseFile);

    printAction_ = new QAction(tr("&Print..."), this);
    printAction_->setObjectName("printAction");
    // Pipeline a7c4f891 phase-3 (E2 / P12-INFRA-1): QAction in Qt6
    // is a QObject (not QWidget) and has no setAccessibleName().
    // UIA reads QAction text() as the Name property. The mnemonic
    // '&' is stripped automatically. The pipe seam below
    // (testPrintActionEnabledQueryPipe) is the actual fix for the
    // UIA-tree reachability gap — accessibleName would only help
    // if the action were reachable in the first place, which it's
    // not because it's menu-only (no toolbar entry).
    printAction_->setShortcut(QKeySequence::Print);
    printAction_->setStatusTip(tr("Print the document"));
    printAction_->setEnabled(false);
    connect(printAction_, &QAction::triggered, this, &MainWindow::onPrint);

    printPreviewAction_ = new QAction(tr("Print Pre&view..."), this);
    printPreviewAction_->setObjectName("printPreviewAction");
    printPreviewAction_->setStatusTip(tr("Preview the document before printing"));
    printPreviewAction_->setEnabled(false);
    connect(printPreviewAction_, &QAction::triggered, this, &MainWindow::onPrintPreview);

    exitAction_ = new QAction(tr("E&xit"), this);
    exitAction_->setObjectName("exitAction");
    exitAction_->setShortcut(QKeySequence(tr("Ctrl+Q")));
    exitAction_->setStatusTip(tr("Exit the application"));
    connect(exitAction_, &QAction::triggered, this, &MainWindow::onExit);

    // Edit menu actions
    undoAction_ = new QAction(tr("&Undo"), this);
    undoAction_->setObjectName("undoAction");
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setStatusTip(tr("Undo the last action"));
    undoAction_->setEnabled(false);
    connect(undoAction_, &QAction::triggered, this, &MainWindow::onUndo);

    redoAction_ = new QAction(tr("&Redo"), this);
    redoAction_->setObjectName("redoAction");
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setStatusTip(tr("Redo the last undone action"));
    redoAction_->setEnabled(false);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::onRedo);

    copyAction_ = new QAction(tr("&Copy"), this);
    copyAction_->setObjectName("copyAction");
    copyAction_->setShortcut(QKeySequence::Copy);
    copyAction_->setStatusTip(tr("Copy selected text to clipboard"));
    copyAction_->setEnabled(false);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::onCopy);

    cutAction_ = new QAction(tr("Cu&t"), this);
    cutAction_->setObjectName("cutAction");
    cutAction_->setShortcut(QKeySequence::Cut);
    cutAction_->setStatusTip(tr("Cut selected text to clipboard"));
    cutAction_->setEnabled(false);
    connect(cutAction_, &QAction::triggered, this, &MainWindow::onCut);

    pasteAction_ = new QAction(tr("&Paste"), this);
    pasteAction_->setObjectName("pasteAction");
    pasteAction_->setShortcut(QKeySequence::Paste);
    pasteAction_->setStatusTip(tr("Paste text from clipboard"));
    pasteAction_->setEnabled(false);
    connect(pasteAction_, &QAction::triggered, this, &MainWindow::onPaste);

    selectAllAction_ = new QAction(tr("Select &All"), this);
    selectAllAction_->setObjectName("selectAllAction");
    selectAllAction_->setShortcut(QKeySequence::SelectAll);
    selectAllAction_->setStatusTip(tr("Select all text in the document"));
    selectAllAction_->setEnabled(false);
    connect(selectAllAction_, &QAction::triggered, this, &MainWindow::onSelectAll);

    findAction_ = new QAction(tr("&Find..."), this);
    findAction_->setObjectName("findAction");
    findAction_->setShortcut(QKeySequence::Find);
    findAction_->setStatusTip(tr("Find text in the document"));
    connect(findAction_, &QAction::triggered, this, &MainWindow::onFind);

    replaceAction_ = new QAction(tr("&Replace..."), this);
    replaceAction_->setObjectName("replaceAction");
    replaceAction_->setShortcut(QKeySequence(tr("Ctrl+H")));
    replaceAction_->setStatusTip(tr("Find and replace text in the document"));
    replaceAction_->setEnabled(false);
    connect(replaceAction_, &QAction::triggered, this, [this]() {
        if (searchPanelDock_) {
            searchPanelDock_->show();
            searchPanelDock_->raise();
        }
        if (searchPanel_) {
            searchPanel_->focusSearchBox();
        }
    });

    preferencesAction_ = new QAction(tr("Pr&eferences..."), this);
    preferencesAction_->setObjectName("preferencesAction");
    preferencesAction_->setStatusTip(tr("Open application preferences"));
    connect(preferencesAction_, &QAction::triggered, this, [this]() {
        PreferencesDialog dlg(appSettings_.get(), this);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        dlg.exec();
    });

    // Pipeline F phase-10: Trust Anchors dialog. Edit > Trust Anchors...
    // (also surfaced from Security menu for discoverability).
    trustAnchorAction_ = new QAction(tr("Trust &Anchors..."), this);
    trustAnchorAction_->setObjectName("trustAnchorAction");
    trustAnchorAction_->setStatusTip(
        tr("Manage trusted root certificates for offline (LTV) signature validation"));
    connect(trustAnchorAction_, &QAction::triggered, this, [this]() {
        pdfe::ui::TrustAnchorDialog dlg(this);
        dlg.exec();
    });

    // View menu actions
    zoomInAction_ = new QAction(tr("Zoom &In"), this);
    zoomInAction_->setObjectName("zoomInAction");
    zoomInAction_->setShortcut(QKeySequence::ZoomIn);
    zoomInAction_->setStatusTip(tr("Zoom in"));
    zoomInAction_->setEnabled(false);
    connect(zoomInAction_, &QAction::triggered, this, &MainWindow::onZoomIn);

    zoomOutAction_ = new QAction(tr("Zoom &Out"), this);
    zoomOutAction_->setObjectName("zoomOutAction");
    zoomOutAction_->setShortcut(QKeySequence::ZoomOut);
    zoomOutAction_->setStatusTip(tr("Zoom out"));
    zoomOutAction_->setEnabled(false);
    connect(zoomOutAction_, &QAction::triggered, this, &MainWindow::onZoomOut);

    fitWidthAction_ = new QAction(tr("Fit &Width"), this);
    fitWidthAction_->setObjectName("fitWidthAction");
    fitWidthAction_->setStatusTip(tr("Fit page width to window"));
    fitWidthAction_->setEnabled(false);
    connect(fitWidthAction_, &QAction::triggered, this, &MainWindow::onFitWidth);

    fitPageAction_ = new QAction(tr("Fit &Page"), this);
    fitPageAction_->setObjectName("fitPageAction");
    fitPageAction_->setStatusTip(tr("Fit entire page to window"));
    fitPageAction_->setEnabled(false);
    connect(fitPageAction_, &QAction::triggered, this, &MainWindow::onFitPage);

    actualSizeAction_ = new QAction(tr("&Actual Size"), this);
    actualSizeAction_->setObjectName("actualSizeAction");
    actualSizeAction_->setShortcut(QKeySequence(tr("Ctrl+1")));
    actualSizeAction_->setStatusTip(tr("Zoom to 100% (1:1)"));
    actualSizeAction_->setEnabled(false);
    connect(actualSizeAction_, &QAction::triggered, this, &MainWindow::onActualSize);

    fullScreenAction_ = new QAction(tr("&Full Screen"), this);
    fullScreenAction_->setObjectName("fullScreenAction");
    fullScreenAction_->setShortcut(QKeySequence(tr("F11")));
    fullScreenAction_->setStatusTip(tr("Toggle full-screen mode"));
    fullScreenAction_->setCheckable(true);
    connect(fullScreenAction_, &QAction::triggered, this, &MainWindow::onToggleFullScreen);

    togglePagePanelAction_ = new QAction(tr("&Page Panel"), this);
    togglePagePanelAction_->setObjectName("togglePagePanelAction");
    togglePagePanelAction_->setCheckable(true);
    togglePagePanelAction_->setChecked(true);
    connect(togglePagePanelAction_, &QAction::triggered, this, &MainWindow::onTogglePagePanel);

    togglePropertiesPanelAction_ = new QAction(tr("P&roperties Panel"), this);
    togglePropertiesPanelAction_->setObjectName("togglePropertiesPanelAction");
    togglePropertiesPanelAction_->setCheckable(true);
    togglePropertiesPanelAction_->setChecked(true);
    connect(togglePropertiesPanelAction_, &QAction::triggered, this, &MainWindow::onTogglePropertiesPanel);

    toggleSearchPanelAction_ = new QAction(tr("&Search Panel"), this);
    toggleSearchPanelAction_->setObjectName("toggleSearchPanelAction");
    toggleSearchPanelAction_->setCheckable(true);
    toggleSearchPanelAction_->setChecked(true);
    connect(toggleSearchPanelAction_, &QAction::triggered, this, &MainWindow::onToggleSearchPanel);

    toggleBookmarkPanelAction_ = new QAction(tr("&Bookmark Panel"), this);
    toggleBookmarkPanelAction_->setObjectName("toggleBookmarkPanelAction");
    toggleBookmarkPanelAction_->setCheckable(true);
    toggleBookmarkPanelAction_->setChecked(true);
    connect(toggleBookmarkPanelAction_, &QAction::triggered, this, &MainWindow::onToggleBookmarkPanel);

    toggleLayerPanelAction_ = new QAction(tr("&Layer Panel"), this);
    toggleLayerPanelAction_->setObjectName("actionToggleLayerPanel");
    toggleLayerPanelAction_->setCheckable(true);
    toggleLayerPanelAction_->setChecked(false);
    connect(toggleLayerPanelAction_, &QAction::triggered, this, [this](bool checked) {
        if (layerPanelDock_) layerPanelDock_->setVisible(checked);
    });

    // Page manipulation actions
    rotatePageCWAction_ = new QAction(tr("Rotate &Clockwise"), this);
    rotatePageCWAction_->setObjectName("rotatePageCWAction");
    rotatePageCWAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+Right")));
    rotatePageCWAction_->setStatusTip(tr("Rotate selected pages clockwise 90°"));
    rotatePageCWAction_->setEnabled(false);
    connect(rotatePageCWAction_, &QAction::triggered, this, &MainWindow::onRotatePageCW);

    rotatePageCCWAction_ = new QAction(tr("Rotate C&ounter-Clockwise"), this);
    rotatePageCCWAction_->setObjectName("rotatePageCCWAction");
    rotatePageCCWAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+Left")));
    rotatePageCCWAction_->setStatusTip(tr("Rotate selected pages counter-clockwise 90°"));
    rotatePageCCWAction_->setEnabled(false);
    connect(rotatePageCCWAction_, &QAction::triggered, this, &MainWindow::onRotatePageCCW);

    rotatePage180Action_ = new QAction(tr("Rotate &180°"), this);
    rotatePage180Action_->setObjectName("rotatePage180Action");
    rotatePage180Action_->setStatusTip(tr("Rotate selected pages 180°"));
    rotatePage180Action_->setEnabled(false);
    connect(rotatePage180Action_, &QAction::triggered, this, &MainWindow::onRotatePage180);

    deletePageAction_ = new QAction(tr("&Delete Selected Pages"), this);
    deletePageAction_->setObjectName("deletePageAction");
    deletePageAction_->setShortcut(QKeySequence::Delete);
    deletePageAction_->setStatusTip(tr("Delete selected pages from document"));
    deletePageAction_->setEnabled(false);
    connect(deletePageAction_, &QAction::triggered, this, &MainWindow::onDeletePages);

    insertBlankPageAction_ = new QAction(tr("Insert &Blank Page"), this);
    insertBlankPageAction_->setObjectName("insertBlankPageAction");
    insertBlankPageAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+N")));
    insertBlankPageAction_->setStatusTip(tr("Insert a blank page after current page"));
    insertBlankPageAction_->setEnabled(false);
    connect(insertBlankPageAction_, &QAction::triggered, this, &MainWindow::onInsertBlankPage);

    insertPagesFromFileAction_ = new QAction(tr("Insert Pages from &File..."), this);
    insertPagesFromFileAction_->setObjectName("insertPagesFromFileAction");
    insertPagesFromFileAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+I")));
    insertPagesFromFileAction_->setStatusTip(tr("Insert pages from another PDF"));
    insertPagesFromFileAction_->setEnabled(false);
    connect(insertPagesFromFileAction_, &QAction::triggered, this, &MainWindow::onInsertPagesFromFile);

    extractPagesAction_ = new QAction(tr("&Extract Pages..."), this);
    extractPagesAction_->setObjectName("extractPagesAction");
    extractPagesAction_->setStatusTip(tr("Extract selected pages to a new PDF"));
    extractPagesAction_->setEnabled(false);
    connect(extractPagesAction_, &QAction::triggered, this, &MainWindow::onExtractPages);

    movePageUpAction_ = new QAction(tr("Move Page &Up"), this);
    movePageUpAction_->setObjectName("movePageUpAction");
    movePageUpAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+Up")));
    movePageUpAction_->setStatusTip(tr("Move the current page one position earlier"));
    movePageUpAction_->setEnabled(false);
    connect(movePageUpAction_, &QAction::triggered, this, &MainWindow::onMovePageUp);

    movePageDownAction_ = new QAction(tr("Move Page &Down"), this);
    movePageDownAction_->setObjectName("movePageDownAction");
    movePageDownAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+Down")));
    movePageDownAction_->setStatusTip(tr("Move the current page one position later"));
    movePageDownAction_->setEnabled(false);
    connect(movePageDownAction_, &QAction::triggered, this, &MainWindow::onMovePageDown);

    // Text edit action
    textEditToolAction_ = new QAction(tr("Edit Te&xt"), this);
    textEditToolAction_->setObjectName("textEditToolAction");
    textEditToolAction_->setCheckable(true);
    textEditToolAction_->setStatusTip(tr("Edit text content (auto-detects native vs scanned pages)"));
    connect(textEditToolAction_, &QAction::triggered, this, &MainWindow::onTextEditToolSelected);

    // TRE (Scanned Text Edit) action -- internal only, not shown in toolbar
    treToolAction_ = new QAction(tr("&Scanned Text Edit"), this);
    treToolAction_->setObjectName("treToolAction");
    treToolAction_->setCheckable(true);
    treToolAction_->setStatusTip(tr("Edit text on scanned/image-only pages"));

    // Form actions
    formToolAction_ = new QAction(tr("Form &Tool"), this);
    formToolAction_->setObjectName("formToolAction");
    formToolAction_->setCheckable(true);
    formToolAction_->setStatusTip(tr("Interact with form fields"));
    connect(formToolAction_, &QAction::triggered, this, &MainWindow::onFormToolSelected);

    createFormFieldAction_ = new QAction(tr("&Create Field"), this);
    createFormFieldAction_->setObjectName("createFormFieldAction");
    createFormFieldAction_->setStatusTip(
        tr("Activate the form tool and show the field creation toolbar"));
    createFormFieldAction_->setEnabled(false);
    connect(createFormFieldAction_, &QAction::triggered, this, &MainWindow::onCreateFormField);

    formFieldPropertiesAction_ = new QAction(tr("Field &Properties"), this);
    formFieldPropertiesAction_->setObjectName("formFieldPropertiesAction");
    formFieldPropertiesAction_->setStatusTip(
        tr("Show the form field properties panel"));
    formFieldPropertiesAction_->setEnabled(false);
    connect(formFieldPropertiesAction_, &QAction::triggered, this, &MainWindow::onFormFieldProperties);

    exportFormDataAction_ = new QAction(tr("&Export Form Data..."), this);
    exportFormDataAction_->setObjectName("exportFormDataAction");
    exportFormDataAction_->setStatusTip(tr("Export form data to XFDF file"));
    exportFormDataAction_->setEnabled(false);
    connect(exportFormDataAction_, &QAction::triggered, this, &MainWindow::onExportFormData);

    importFormDataAction_ = new QAction(tr("&Import Form Data..."), this);
    importFormDataAction_->setObjectName("importFormDataAction");
    importFormDataAction_->setStatusTip(tr("Import form data from XFDF file"));
    importFormDataAction_->setEnabled(false);
    connect(importFormDataAction_, &QAction::triggered, this, &MainWindow::onImportFormData);

    resetFormAction_ = new QAction(tr("&Reset Form"), this);
    resetFormAction_->setObjectName("resetFormAction");
    resetFormAction_->setStatusTip(tr("Reset all form fields to default values"));
    resetFormAction_->setEnabled(false);
    connect(resetFormAction_, &QAction::triggered, this, &MainWindow::onResetForm);

    flattenFormAction_ = new QAction(tr("&Flatten Form"), this);
    flattenFormAction_->setObjectName("flattenFormAction");
    flattenFormAction_->setStatusTip(tr("Flatten form fields into static content (irreversible)"));
    flattenFormAction_->setEnabled(false);
    connect(flattenFormAction_, &QAction::triggered, this, &MainWindow::onFlattenForm);

    tabOrderAction_ = new QAction(tr("Tab &Order..."), this);
    tabOrderAction_->setObjectName("tabOrderAction");
    tabOrderAction_->setStatusTip(tr("Edit form field tab order"));
    tabOrderAction_->setEnabled(false);
    connect(tabOrderAction_, &QAction::triggered, this, [this]() {
        if (!document_ || !formManager_) return;
        int pageIdx = canvas_->currentPage();
        auto page = document_->page(pageIdx);
        if (!page) return;

        TabOrderDialog dlg(formManager_.get(), page.get(), this);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        if (dlg.exec() == QDialog::Accepted) {
            auto newOrder = dlg.newTabOrder();
            formManager_->setTabOrder(page.get(), newOrder);
            canvas_->update();
        }
    });

    // Pipeline F phase-5: Calculation Order — form-wide /AcroForm /CO
    // editor. Operates on field names at document level (parallels
    // Tab Order which is page-local).
    // Pipeline F phase-5 NOTE: mnemonic 'u' (Calc&ulation Order) avoids
    // collision with 'C' on Create Field — Forms menu has both, and
    // Qt's mnemonic cycler would otherwise need a second keypress to
    // disambiguate, breaking keyboard-driven UI tests.
    calculationOrderAction_ = new QAction(tr("Calc&ulation Order..."), this);
    calculationOrderAction_->setObjectName("calculationOrderAction");
    calculationOrderAction_->setStatusTip(
        tr("Edit form-wide field calculation order"));
    calculationOrderAction_->setEnabled(false);
    connect(calculationOrderAction_, &QAction::triggered, this, [this]() {
        if (!document_ || !formManager_) return;
        CalculationOrderDialog dlg(formManager_.get(), document_.get(), this);
        dlg.setObjectName("calculationOrderDialog");
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        dlg.exec();
        // CalculationOrderDialog applies its own changes via Apply
        // button; no caller-side post-processing needed.
    });

    // Pipeline F phase-5: Alt Text editor for the first annotation on
    // the current page (full per-annotation selection wiring is
    // future scope — this satisfies the AC's dialog-open path without
    // pretending to do per-annotation right-click yet).
    // Pipeline F phase-5 NOTE: mnemonic 'x' (Alt Te&xt) avoids
    // collisions in the Tools menu — &Alt would clash with "OCR
    // &All Pages", and 'A' collides with the canvas-context menu's
    // "&Actual Size" entry too.
    altTextAction_ = new QAction(tr("Alt Te&xt..."), this);
    altTextAction_->setObjectName("altTextAction");
    altTextAction_->setStatusTip(
        tr("Edit alternate text and actual text for the selected annotation"));
    // Keyboard shortcut also makes the action reliably triggerable
    // from accessibility automation that has trouble synthesizing
    // mouse events on the canvas's Custom-control surface.
    altTextAction_->setShortcut(QKeySequence("Ctrl+Alt+T"));
    altTextAction_->setShortcutContext(Qt::ApplicationShortcut);
    altTextAction_->setEnabled(false);

    // Pipeline H phase-4: selection-aware AutomationId path for the
    // Alt Text dialog. Distinct from altTextAction_ — this one is
    // enabled ONLY when exactly one annotation is selected (via
    // SelectTool's annotationSelected callback), and its triggered
    // slot opens AltTextDialog for that specific annotation index.
    // QAction inherits QObject (not QWidget) so it has no
    // setAccessibleName(); the objectName below maps directly to
    // UIA AutomationId and is what the pywinauto/WinAppDriver
    // ``find_element("accessibility id", ...)`` lookup matches.
    openAltTextDialogAction_ = new QAction(tr("Alt Text (Selected Annotation)..."), this);
    openAltTextDialogAction_->setObjectName("openAltTextDialogAction");
    openAltTextDialogAction_->setStatusTip(
        tr("Edit alternate text and actual text for the currently-selected "
           "annotation (enabled when exactly one annotation is selected)"));
    openAltTextDialogAction_->setEnabled(false);
    connect(openAltTextDialogAction_, &QAction::triggered, this, [this]() {
        if (!document_ || !annotationManager_) return;
        if (selectedAnnotationPageIndex_ < 0 ||
            selectedAnnotationIndex_ < 0) {
            return;
        }
        auto page = document_->page(selectedAnnotationPageIndex_);
        if (!page) return;
        auto* dlg = new AltTextDialog(
            annotationManager_.get(), page.get(),
            selectedAnnotationIndex_, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(true);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    connect(altTextAction_, &QAction::triggered, this, [this]() {
        if (!document_ || !annotationManager_ || !canvas_) return;
        int pageIdx = canvas_->currentPage();
        auto page = document_->page(pageIdx);
        if (!page) return;
        const int annotCount = static_cast<int>(
            annotationManager_->getAnnotations(page.get()).size());
        if (annotCount <= 0) {
            QMessageBox::information(this, tr("Alt Text"),
                tr("No annotation on this page to edit alt text for."));
            return;
        }
        // GAP-27: use the annotation currently selected via SelectTool
        // (selectedAnnotationIndex_) when it is valid for this page;
        // fall back to annotation 0 only when no selection exists.
        // Pipeline c91e7f3a phase-5: update selection indices so that
        // getCurrentAnnotationAltText() (and the readback pipe seam) can
        // return the correct value after the dialog accepts.
        const int annotIndex =
            (selectedAnnotationPageIndex_ == pageIdx &&
             selectedAnnotationIndex_ >= 0 &&
             selectedAnnotationIndex_ < annotCount)
            ? selectedAnnotationIndex_
            : 0;
        selectedAnnotationPageIndex_ = pageIdx;
        selectedAnnotationIndex_ = annotIndex;
        if (openAltTextDialogAction_) {
            openAltTextDialogAction_->setEnabled(true);
        }
        // Keep the Page alive for the duration of the dialog by moving it
        // into commandPages_.  The dialog is non-blocking (show() returns
        // immediately), so 'page' would otherwise be destroyed when this
        // lambda returns — leaving AltTextDialog::page_ dangling before
        // onSave() can call setAltText().
        commandPages_.push_back(std::move(page));
        auto* pagePtr = commandPages_.back().get();
        // Heap-allocate so the dialog survives any subsequent mouse
        // events that arrive after the menu closes (otherwise a
        // queued mouse-up from the right-click can immediately
        // dismiss a stack-allocated dialog before it ever paints).
        auto* dlg = new AltTextDialog(
            annotationManager_.get(), pagePtr, annotIndex, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(true);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });

    submitFormAction_ = new QAction(tr("&Submit Form..."), this);
    submitFormAction_->setObjectName("submitFormAction");
    submitFormAction_->setStatusTip(tr("Submit form data to a URL"));
    submitFormAction_->setEnabled(false);
    connect(submitFormAction_, &QAction::triggered, this, [this]() {
        QDialog dlg(this);
        dlg.setObjectName("submitFormDialog");
        dlg.setWindowTitle(tr("Submit Form"));
        auto* layout = new QVBoxLayout(&dlg);
        auto* submitUrlField = new QLineEdit(&dlg);
        submitUrlField->setObjectName("submitUrlField");
        submitUrlField->setAccessibleName(tr("Form Submit URL"));
        submitUrlField->setPlaceholderText(tr("Submit URL..."));
        layout->addWidget(submitUrlField);
        auto* submitButton = new QPushButton(tr("Submit"), &dlg);
        submitButton->setObjectName("submitButton");
        submitButton->setAccessibleName(tr("Submit Form"));
        layout->addWidget(submitButton);
        connect(submitButton, &QPushButton::clicked, &dlg, &QDialog::accept);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }

        // GitHub #18 (PDF-48). Until this block, the dialog's Accept button
        // was the whole feature: it closed the dialog and nothing happened.
        // The required-field enforcement in FormSubmitter was correct, tested
        // by seven GoogleTests, and unreachable — an incomplete form was
        // refused in a unit test and "submitted" fine here.
        const QUrl target(submitUrlField->text().trimmed(),
                          QUrl::StrictMode);
        QString outcome = submitCurrentFormTo(target, /*hostConfirmed=*/false);

        if (outcome.startsWith(QLatin1String("confirm-host:"))) {
            // The host prompt is raised HERE rather than inside
            // submitCurrentFormTo so the decision function stays free of
            // modal I/O — that is what lets a test drive the real decision
            // without a dialog, and what makes an unconfirmed pass incapable
            // of opening a connection.
            const QString host = outcome.mid(
                QString(QLatin1String("confirm-host:")).size());
            const auto answer = QMessageBox::question(
                this, tr("Send Form Data?"),
                tr("This form wants to send its data to “%1”, which you have "
                   "not sent form data to before.\n\nSend it?").arg(host),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                statusBar()->showMessage(
                    tr("Form submission cancelled."), 5000);
                return;
            }
            confirmedSubmitHosts_.insert(host);
            outcome = submitCurrentFormTo(target, /*hostConfirmed=*/true);
        }

        if (outcome.startsWith(QLatin1String("reject-missing-required:"))) {
            const QString names = outcome.mid(
                QString(QLatin1String("reject-missing-required:")).size());
            // Name the fields. "This form is incomplete" with no further
            // information is a dead end on a 40-field form, which is why
            // FormSubmitter::missingRequiredFields returns names, not a bool.
            QMessageBox box(QMessageBox::Warning,
                            tr("Form Is Incomplete"),
                            tr("This form was not sent: a required field has "
                               "no value."),
                            QMessageBox::Ok, this);
            box.setObjectName("formSubmitIncompleteDialog");
            box.setAccessibleName(tr("Form Is Incomplete"));
            box.setInformativeText(tr("Required field(s): %1").arg(names));
            pdfe::ui::testseam::registerDialogIntrospection(&box);
            box.exec();
            statusBar()->showMessage(
                tr("Form not sent — required field(s) empty: %1").arg(names),
                8000);
            return;
        }
        if (outcome.startsWith(QLatin1String("submitted:"))) {
            statusBar()->showMessage(tr("Form data sent (HTTP %1).")
                .arg(outcome.mid(QString(QLatin1String("submitted:")).size())),
                5000);
            return;
        }
        // Every remaining token is a refusal or a network failure. Each is
        // reported with its OWN reason: a blocked destination must not be
        // reported as an incomplete form, or a security refusal reads to the
        // user as a validation nag.
        QString reason;
        if (outcome == QLatin1String("reject-external-actions-disabled")) {
            reason = tr("Sending form data is disabled. Enable it under "
                        "Preferences ▸ Security (external actions).");
        } else if (outcome == QLatin1String("reject-no-document")) {
            reason = tr("No document is open.");
        } else if (outcome == QLatin1String("reject-invalid-url")) {
            reason = tr("“%1” is not a valid http or https address.")
                         .arg(submitUrlField->text().trimmed());
        } else if (outcome == QLatin1String("reject-blocked-destination")) {
            reason = tr("That address is on this machine or inside a private "
                        "network. Form data is never sent there.");
        } else if (outcome.startsWith(QLatin1String("submit-failed:"))) {
            reason = outcome.mid(
                QString(QLatin1String("submit-failed:")).size());
        } else {
            reason = outcome;
        }
        QMessageBox box(QMessageBox::Warning, tr("Form Not Sent"), reason,
                        QMessageBox::Ok, this);
        box.setObjectName("formSubmitRefusedDialog");
        box.setAccessibleName(tr("Form Not Sent"));
        pdfe::ui::testseam::registerDialogIntrospection(&box);
        box.exec();
        statusBar()->showMessage(tr("Form not sent — %1").arg(reason), 8000);
    });

    // Security (Redaction) actions
    redactToolAction_ = new QAction(tr("&Redaction Tool"), this);
    redactToolAction_->setObjectName("redactToolAction");
    redactToolAction_->setCheckable(true);
    redactToolAction_->setStatusTip(tr("Mark areas for redaction"));
    connect(redactToolAction_, &QAction::triggered, this, &MainWindow::onRedactToolSelected);

    markRedactionAction_ = new QAction(tr("Mark &Redaction"), this);
    markRedactionAction_->setObjectName("markRedactionAction");
    markRedactionAction_->setStatusTip(tr("Mark selected area for redaction"));
    connect(markRedactionAction_, &QAction::triggered, this, [this]() {
        if (redactToolAction_) {
            redactToolAction_->setChecked(true);
            onRedactToolSelected();
        }
    });

    applyRedactionsAction_ = new QAction(tr("&Apply Redactions..."), this);
    applyRedactionsAction_->setObjectName("applyRedactionsAction");
    applyRedactionsAction_->setStatusTip(tr("Permanently remove content under redaction marks"));
    applyRedactionsAction_->setEnabled(false);
    connect(applyRedactionsAction_, &QAction::triggered, this, &MainWindow::onApplyRedactions);

    // Security (Encryption) actions
    saveWithEncryptionAction_ = new QAction(tr("Save with &Encryption..."), this);
    saveWithEncryptionAction_->setObjectName("saveWithEncryptionAction");
    saveWithEncryptionAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+E")));
    saveWithEncryptionAction_->setStatusTip(tr("Save the document with password protection"));
    saveWithEncryptionAction_->setEnabled(false);
    connect(saveWithEncryptionAction_, &QAction::triggered, this, &MainWindow::onSaveWithEncryption);

    documentPermissionsAction_ = new QAction(tr("Document &Permissions..."), this);
    documentPermissionsAction_->setObjectName("documentPermissionsAction");
    documentPermissionsAction_->setStatusTip(tr("View encryption and permission details"));
    documentPermissionsAction_->setEnabled(false);
    connect(documentPermissionsAction_, &QAction::triggered, this, &MainWindow::onDocumentPermissions);

    removePasswordAction_ = new QAction(tr("&Remove Password..."), this);
    removePasswordAction_->setObjectName("removePasswordAction");
    removePasswordAction_->setStatusTip(
        tr("Save an unencrypted copy of the document"));
    removePasswordAction_->setEnabled(false);
    connect(removePasswordAction_, &QAction::triggered, this, &MainWindow::onRemovePassword);

    // Security (Signatures) actions
    signDocumentAction_ = new QAction(tr("&Sign Document..."), this);
    signDocumentAction_->setObjectName("signDocumentAction");
    // Ctrl+Alt+S, not Ctrl+Shift+S — the latter collides with
    // QKeySequence::SaveAs (saveAsAction_) on Windows. (UI-KB-04)
    signDocumentAction_->setShortcut(QKeySequence(tr("Ctrl+Alt+S")));
    signDocumentAction_->setStatusTip(tr("Sign the document with a digital certificate"));
    signDocumentAction_->setEnabled(false);
    connect(signDocumentAction_, &QAction::triggered, this, &MainWindow::onSignDocument);

    verifySignaturesAction_ = new QAction(tr("&Verify Signatures"), this);
    verifySignaturesAction_->setObjectName("verifySignaturesAction");
    verifySignaturesAction_->setShortcut(QKeySequence(tr("Ctrl+Shift+V")));
    verifySignaturesAction_->setStatusTip(tr("Verify all digital signatures in the document"));
    verifySignaturesAction_->setEnabled(false);
    connect(verifySignaturesAction_, &QAction::triggered, this, &MainWindow::onVerifySignatures);

    // Pipeline F phase-9. EXTERNAL NETWORK FETCH — disabled by default,
    // enabled only after verifyAllSignatures() reports >= 1 valid signature.
    // The action wraps the user-confirmed validation flow; it never
    // auto-fires.
    requestValidationAction_ = new QAction(tr("&Request Validation Data..."), this);
    requestValidationAction_->setObjectName("requestValidationAction");
    requestValidationAction_->setStatusTip(
        tr("Contact OCSP/CRL servers to fetch revocation data and "
           "embed it for long-term validation (LTV)"));
    requestValidationAction_->setEnabled(false);
    connect(requestValidationAction_, &QAction::triggered,
            this, &MainWindow::onRequestValidation);

    // Help menu actions
    aboutAction_ = new QAction(tr("&About PDFE"), this);
    aboutAction_->setObjectName("aboutAction");
    aboutAction_->setStatusTip(tr("Show the About dialog"));
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::onAbout);

    documentationAction_ = new QAction(tr("&Documentation"), this);
    documentationAction_->setObjectName("documentationAction");
    documentationAction_->setShortcut(QKeySequence::HelpContents);
    documentationAction_->setStatusTip(tr("Open the PDFE documentation"));
    connect(documentationAction_, &QAction::triggered, this, &MainWindow::onDocumentation);

    reportIssueAction_ = new QAction(tr("&Report Issue..."), this);
    reportIssueAction_->setObjectName("reportIssueAction");
    reportIssueAction_->setStatusTip(tr("Open the PDFE issue tracker"));
    connect(reportIssueAction_, &QAction::triggered, this, &MainWindow::onReportIssue);

    checkUpdatesAction_ = new QAction(tr("Check for &Updates..."), this);
    checkUpdatesAction_->setObjectName("checkUpdatesAction");
    checkUpdatesAction_->setStatusTip(tr("Open the PDFE releases page"));
    connect(checkUpdatesAction_, &QAction::triggered, this, &MainWindow::onCheckUpdates);

    // Image actions
    imageToolAction_ = new QAction(tr("Image Tool"), this);
    imageToolAction_->setObjectName("imageToolAction");
    imageToolAction_->setCheckable(true);
    imageToolAction_->setEnabled(false);

    insertImageAction_ = new QAction(tr("Insert Image..."), this);
    insertImageAction_->setObjectName("insertImageAction");
    insertImageAction_->setEnabled(false);
    connect(insertImageAction_, &QAction::triggered, this, &MainWindow::onInsertImage);

    stripExifAction_ = new QAction(tr("Strip Image Metadata"), this);
    stripExifAction_->setObjectName("stripExifAction");
    stripExifAction_->setEnabled(false);
    connect(stripExifAction_, &QAction::triggered, this, &MainWindow::onStripExif);

    imageRemoveAction_ = new QAction(tr("&Remove Image"), this);
    imageRemoveAction_->setObjectName("imageRemoveAction");
    imageRemoveAction_->setStatusTip(tr("Remove the selected image from the page"));
    imageRemoveAction_->setEnabled(false);
    connect(imageRemoveAction_, &QAction::triggered, this, &MainWindow::onImageRemove);

    imageRotateAction_ = new QAction(tr("Rotate Image..."), this);
    imageRotateAction_->setObjectName("imageRotateAction");
    imageRotateAction_->setStatusTip(tr("Rotate the selected image"));
    imageRotateAction_->setEnabled(false);
    connect(imageRotateAction_, &QAction::triggered, this, &MainWindow::onImageRotate90);

    imageCropAction_ = new QAction(tr("Crop Image..."), this);
    imageCropAction_->setObjectName("imageCropAction");
    imageCropAction_->setStatusTip(tr("Crop the selected image"));
    imageCropAction_->setEnabled(false);
    connect(imageCropAction_, &QAction::triggered, this, &MainWindow::onImageCrop);

    imageResizeAction_ = new QAction(tr("Resi&ze Image..."), this);
    imageResizeAction_->setObjectName("imageResizeAction");
    imageResizeAction_->setStatusTip(tr("Resize the selected image"));
    imageResizeAction_->setEnabled(false);
    connect(imageResizeAction_, &QAction::triggered, this, &MainWindow::onImageResize);

    // Metadata
    documentMetadataAction_ = new QAction(tr("Document Properties..."), this);
    documentMetadataAction_->setObjectName("documentMetadataAction");
    documentMetadataAction_->setEnabled(false);
    connect(documentMetadataAction_, &QAction::triggered, this, &MainWindow::onDocumentMetadata);

    // Compliance
    complianceAction_ = new QAction(tr("Compliance..."), this);
    complianceAction_->setObjectName("complianceAction");
    complianceAction_->setEnabled(false);
    connect(complianceAction_, &QAction::triggered, this, &MainWindow::onCompliance);

    // Preflight — dedicated report dialog on top of ComplianceManager.
    preflightAction_ = new QAction(tr("Preflight..."), this);
    preflightAction_->setObjectName("preflightAction");
    preflightAction_->setEnabled(false);
    connect(preflightAction_, &QAction::triggered, this, [this]() {
        if (!document_) return;
        pdfe::ui::PreflightDialog dlg(document_.get(), muPdfContext_, this);
        dlg.exec();
    });

    // Tools menu actions
    ocrAction_ = new QAction(tr("&OCR Current Page"), this);
    ocrAction_->setObjectName("ocrAction");
    ocrAction_->setStatusTip(tr("Run OCR on the current page"));
    ocrAction_->setEnabled(false);
    connect(ocrAction_, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("OCR"), tr("OCR processing..."));
    });

    // Phase 7d: OCR All Pages — iterates every page with a cancellable
    // progress dialog and records the work in an UndoManager command
    // so users can revert (invalidates OCR cache for processed pages).
    ocrAllPagesAction_ = new QAction(tr("OCR &All Pages..."), this);
    ocrAllPagesAction_->setObjectName("actionOcrAllPages");
    ocrAllPagesAction_->setToolTip(tr("OCR All Pages"));
    ocrAllPagesAction_->setStatusTip(tr("Run OCR on every page with progress"));
    ocrAllPagesAction_->setEnabled(false);
    connect(ocrAllPagesAction_, &QAction::triggered, this, [this]() {
        if (!document_ || !ocrPipeline_) return;
        const int total = document_->pageCount();
        if (total <= 0) return;
        std::vector<int> pages;
        pages.reserve(total);
        for (int i = 0; i < total; ++i) pages.push_back(i);

        QProgressDialog dlg(tr("Running OCR on all pages..."), tr("Cancel"),
                            0, total, this);
        dlg.setObjectName("ocrAllProgressDialog");
        dlg.setAccessibleName(tr("OCR All Pages Progress"));
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setMinimumDuration(0);
        dlg.setAutoClose(false);
        dlg.setAutoReset(false);
        // Replace the default cancel button with one that carries a
        // stable objectName + accessibleName so UIA tests can locate
        // it deterministically. Pipeline G phase-7 AC #2.
        auto* cancelBtn = new QPushButton(tr("Cancel"), &dlg);
        cancelBtn->setObjectName("ocrAllProgressCancelButton");
        cancelBtn->setAccessibleName(tr("Cancel"));
        dlg.setCancelButton(cancelBtn);

        auto cmd = std::make_unique<pdfe::tre::OcrAllPagesCommand>(
            std::move(pages),
            [this](int pageIdx) { ocrPipeline_->ocrPage(pageIdx); },
            [this](int pageIdx) { ocrPipeline_->invalidateCache(pageIdx); },
            [&dlg](int done, int totalPages) -> bool {
                dlg.setValue(done);
                QCoreApplication::processEvents();
                return !dlg.wasCanceled();
            });

        if (undoManager_) {
            undoManager_->execute(std::move(cmd));
        } else {
            // No undo manager yet — execute in-place.
            cmd->execute();
        }
        dlg.close();
    });

    treAction_ = new QAction(tr("Text &Reconstruction..."), this);
    treAction_->setObjectName("treAction");
    treAction_->setStatusTip(tr("Run text reconstruction on current page"));
    treAction_->setEnabled(false);
    connect(treAction_, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("Text Reconstruction"),
                                 tr("Text reconstruction is available via the Edit Text tool on scanned pages."));
    });

    accessibilityAction_ = new QAction(tr("&Accessibility Check..."), this);
    accessibilityAction_->setObjectName("accessibilityAction");
    accessibilityAction_->setStatusTip(tr("Check document accessibility"));
    accessibilityAction_->setEnabled(false);
    connect(accessibilityAction_, &QAction::triggered, this, [this]() {
        QDialog dlg(this);
        dlg.setObjectName("accessibilityDialog");
        dlg.setWindowTitle(tr("Accessibility Check"));
        auto* layout = new QVBoxLayout(&dlg);
        auto* accessibilityReport = new QTextEdit(&dlg);
        accessibilityReport->setObjectName("accessibilityReport");
        accessibilityReport->setAccessibleName(
            tr("Accessibility check report"));
        accessibilityReport->setReadOnly(true);
        accessibilityReport->setPlaceholderText(tr("Accessibility report will appear here..."));
        layout->addWidget(accessibilityReport);
        auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
        buttonBox->setObjectName("accessibilityReportButtonBox");
        layout->addWidget(buttonBox);
        connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        dlg.exec();
    });

    batchAction_ = new QAction(tr("&Batch Process..."), this);
    batchAction_->setObjectName("batchAction");
    batchAction_->setStatusTip(tr("Process multiple files in batch"));
    connect(batchAction_, &QAction::triggered, this, [this]() {
        BatchDialog dlg(muPdfContext_, this);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        dlg.exec();
    });

    compareAction_ = new QAction(tr("&Compare Documents..."), this);
    compareAction_->setObjectName("compareAction");
    compareAction_->setStatusTip(tr("Compare two PDF documents"));
    connect(compareAction_, &QAction::triggered, this, [this]() {
        CompareDialog dlg(muPdfContext_, this);
        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
        dlg.exec();
    });


    // View rotation and theme actions
    // QAction inherits QObject (not QWidget) so it has no setAccessibleName().
    // The accessible name for assistive tech is QAction::text(); UIA also
    // surfaces toolTip() as AccessibleDescription. We set both explicitly.
    rotateViewAction_ = new QAction(tr("Rotate View Clockwise"), this);
    rotateViewAction_->setObjectName("actionRotateViewCW");
    rotateViewAction_->setToolTip(tr("Rotate View Clockwise"));
    rotateViewAction_->setStatusTip(tr("Rotate the viewport 90° clockwise (display only)"));
    rotateViewAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(rotateViewAction_, &QAction::triggered, this, [this]() {
        if (canvas_) {
            canvas_->rotateViewCW();
            statusBar()->showMessage(
                tr("View rotation: %1\u00B0").arg(canvas_->viewRotation()), 2000);
        }
    });

    rotateViewCCWAction_ = new QAction(tr("Rotate View Counter-Clockwise"), this);
    rotateViewCCWAction_->setObjectName("actionRotateViewCCW");
    rotateViewCCWAction_->setToolTip(tr("Rotate View Counter-Clockwise"));
    rotateViewCCWAction_->setStatusTip(tr("Rotate the viewport 90° counter-clockwise (display only)"));
    connect(rotateViewCCWAction_, &QAction::triggered, this, [this]() {
        if (canvas_) {
            canvas_->rotateViewCCW();
            statusBar()->showMessage(
                tr("View rotation: %1\u00B0").arg(canvas_->viewRotation()), 2000);
        }
    });

    // Pipeline F phase-6: dedicated 180° view rotation action so the
    // common "flip the page upside down" gesture is one click instead
    // of two CW presses. Adds View > Rotate > 180 (matrix row
    // view_rotate_180).
    rotateView180Action_ = new QAction(tr("Rotate View 180°"), this);
    rotateView180Action_->setObjectName("actionRotateView180");
    rotateView180Action_->setToolTip(tr("Rotate View 180°"));
    rotateView180Action_->setStatusTip(tr("Rotate the viewport 180° (display only)"));
    connect(rotateView180Action_, &QAction::triggered, this, [this]() {
        if (canvas_) {
            canvas_->rotateViewCW();
            canvas_->rotateViewCW();
            statusBar()->showMessage(
                tr("View rotation: %1°").arg(canvas_->viewRotation()), 2000);
        }
    });

    resetViewRotationAction_ = new QAction(tr("Reset View Rotation"), this);
    resetViewRotationAction_->setObjectName("actionResetViewRotation");
    resetViewRotationAction_->setToolTip(tr("Reset View Rotation"));
    resetViewRotationAction_->setStatusTip(tr("Reset the viewport rotation to 0°"));
    connect(resetViewRotationAction_, &QAction::triggered, this, [this]() {
        if (canvas_) {
            canvas_->resetViewRotation();
            statusBar()->showMessage(tr("View rotation reset"), 2000);
        }
    });

    themeToggleAction_ = new QAction(tr("Toggle &Dark Theme"), this);
    themeToggleAction_->setObjectName("themeToggleAction");
    themeToggleAction_->setCheckable(true);
    themeToggleAction_->setStatusTip(tr("Toggle dark/light application theme"));
    connect(themeToggleAction_, &QAction::triggered, this, [this](bool checked) {
        // Legacy single-toggle path retained for any keyboard shortcut
        // or scripted caller still wired to it. Forwards through the
        // ThemeManager so persistence stays consistent.
        if (themeManager_) {
            themeManager_->setTheme(checked ? pdfe::theme::Theme::Dark
                                            : pdfe::theme::Theme::Light);
        }
    });
    // GitHub #217: this action's checked state is what decides which way the
    // toggle goes, and it used to start UNCHECKED no matter what the persisted
    // theme was — it is not in themeGroup and nothing synced it. Launching
    // already in Dark therefore made "Toggle Dark Theme" a NO-OP: it set the
    // theme it was already on. That is a real toggle defect, and it is also
    // what produced the evidence behind #217's "the canvas never repaints" —
    // the probe drove this action, changed nothing, and read the resulting
    // pixel-identical capture as the canvas ignoring the theme. Seeded below
    // from the effective theme and kept in step by syncCheckedFromTheme().

    // Pipeline F phase-5: View > Theme submenu (Light / Dark / System)
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);

    themeLightAction_ = new QAction(tr("&Light"), this);
    themeLightAction_->setObjectName("themeLightAction");
    themeLightAction_->setCheckable(true);
    themeLightAction_->setStatusTip(tr("Switch to the Light theme"));
    themeGroup->addAction(themeLightAction_);
    connect(themeLightAction_, &QAction::triggered, this, [this]() {
        if (themeManager_) themeManager_->setTheme(pdfe::theme::Theme::Light);
    });

    themeDarkAction_ = new QAction(tr("&Dark"), this);
    themeDarkAction_->setObjectName("themeDarkAction");
    themeDarkAction_->setCheckable(true);
    themeDarkAction_->setStatusTip(tr("Switch to the Dark theme"));
    themeGroup->addAction(themeDarkAction_);
    connect(themeDarkAction_, &QAction::triggered, this, [this]() {
        if (themeManager_) themeManager_->setTheme(pdfe::theme::Theme::Dark);
    });

    themeSystemAction_ = new QAction(tr("&System"), this);
    themeSystemAction_->setObjectName("themeSystemAction");
    themeSystemAction_->setCheckable(true);
    themeSystemAction_->setStatusTip(tr("Follow the system Light/Dark setting"));
    themeGroup->addAction(themeSystemAction_);
    connect(themeSystemAction_, &QAction::triggered, this, [this]() {
        if (themeManager_) themeManager_->setTheme(pdfe::theme::Theme::System);
    });

    // Reflect the current persisted theme in the checked-state of the
    // submenu. Connect themeChanged for live updates on programmatic
    // setTheme() calls (e.g. the legacy toggle action above).
    auto syncCheckedFromTheme = [this]() {
        if (!themeManager_) return;
        const auto t = themeManager_->theme();
        if (themeLightAction_)  themeLightAction_->setChecked(t == pdfe::theme::Theme::Light);
        if (themeDarkAction_)   themeDarkAction_->setChecked(t == pdfe::theme::Theme::Dark);
        if (themeSystemAction_) themeSystemAction_->setChecked(t == pdfe::theme::Theme::System);
        // GitHub #217: keep the legacy single toggle in step too. Theme::System
        // is resolved to what the user is ACTUALLY looking at, because that is
        // what the toggle has to invert to be a toggle at all.
        if (themeToggleAction_) {
            bool dark = (t == pdfe::theme::Theme::Dark);
            if (t == pdfe::theme::Theme::System) {
                dark = QGuiApplication::styleHints()->colorScheme()
                       == Qt::ColorScheme::Dark;
            }
            themeToggleAction_->setChecked(dark);
        }
    };
    if (themeManager_) {
        connect(themeManager_.get(), &pdfe::theme::ThemeManager::themeChanged,
                this, [syncCheckedFromTheme](pdfe::theme::Theme) {
                    syncCheckedFromTheme();
                });
    }
    syncCheckedFromTheme();

    // Toggle actions
    toggleStickyNotesAction_ = new QAction(tr("Show Sticky &Notes"), this);
    toggleStickyNotesAction_->setObjectName("toggleStickyNotesAction");
    toggleStickyNotesAction_->setCheckable(true);
    toggleStickyNotesAction_->setChecked(true);
    toggleStickyNotesAction_->setStatusTip(tr("Toggle sticky note visibility"));
    connect(toggleStickyNotesAction_, &QAction::toggled,
            this, &MainWindow::onToggleStickyNotes);

    toggleCopyPreventionAction_ = new QAction(tr("Prevent &Copy"), this);
    toggleCopyPreventionAction_->setObjectName("toggleCopyPreventionAction");
    toggleCopyPreventionAction_->setCheckable(true);
    toggleCopyPreventionAction_->setChecked(false);
    toggleCopyPreventionAction_->setStatusTip(tr("Prevent copying text from this document"));
    connect(toggleCopyPreventionAction_, &QAction::toggled,
            this, &MainWindow::onToggleCopyPrevention);

    togglePrintPreventionAction_ = new QAction(tr("Prevent &Print"), this);
    togglePrintPreventionAction_->setObjectName("togglePrintPreventionAction");
    togglePrintPreventionAction_->setCheckable(true);
    togglePrintPreventionAction_->setChecked(false);
    togglePrintPreventionAction_->setStatusTip(tr("Prevent printing this document"));
    connect(togglePrintPreventionAction_, &QAction::toggled,
            this, &MainWindow::onTogglePrintPrevention);

    toggleOcrBoxesAction_ = new QAction(tr("Show OCR &Boxes"), this);
    toggleOcrBoxesAction_->setObjectName("toggleOcrBoxesAction");
    toggleOcrBoxesAction_->setCheckable(true);
    toggleOcrBoxesAction_->setChecked(false);
    toggleOcrBoxesAction_->setStatusTip(tr("Show per-character OCR bounding boxes on scanned pages"));
    connect(toggleOcrBoxesAction_, &QAction::toggled,
            this, &MainWindow::onToggleOcrBoxes);
}

void MainWindow::createMenus()
{
    // File menu
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName("fileMenu");
    fileMenu->addAction(newFileAction_);
    fileMenu->addAction(openAction_);
    fileMenu->addAction(saveAction_);
    fileMenu->addAction(saveAsAction_);
    fileMenu->addAction(closeAction_);
    fileMenu->addSeparator();
    recentFilesMenu_ = new RecentFilesMenu(tr("Recent &Files"), appSettings_.get(), this);
    fileMenu->addMenu(recentFilesMenu_);
    connect(recentFilesMenu_, &RecentFilesMenu::fileTriggered, this, &MainWindow::openFile);
    fileMenu->addSeparator();
    fileMenu->addAction(createPdfFromImagesAction_);
    fileMenu->addAction(combineFilesAction_);
    fileMenu->addSeparator();

    // Pipeline E phase-4: File > Stamps submenu — import an image as a
    // custom stamp and manage the library. The submenu rebuilds itself
    // on aboutToShow so newly-imported stamps appear without having to
    // recreate the menu structure.
    {
        QMenu* stampsMenu = fileMenu->addMenu(tr("&Stamps"));
        stampsMenu->setObjectName("stampsMenu");
        connect(stampsMenu, &QMenu::aboutToShow, this, [this, stampsMenu]() {
            stampsMenu->clear();
            auto* importAct = stampsMenu->addAction(
                tr("&Import Image As Stamp..."));
            importAct->setObjectName("importImageStampAction");
            importAct->setStatusTip(
                tr("Import a PNG/JPG/BMP image as a reusable stamp"));
            connect(importAct, &QAction::triggered, this,
                    &MainWindow::onImportImageStamp);

            if (!imageStampLibrary_) return;
            const QStringList names = imageStampLibrary_->stampNames();
            if (names.isEmpty()) return;

            stampsMenu->addSeparator();
            QMenu* removeMenu = stampsMenu->addMenu(tr("&Remove Stamp"));
            removeMenu->setObjectName("removeStampMenu");
            for (const QString& name : names) {
                QAction* a = removeMenu->addAction(name);
                connect(a, &QAction::triggered, this, [this, name]() {
                    if (!imageStampLibrary_) return;
                    if (imageStampLibrary_->removeStamp(name)) {
                        imageStampLibrary_->saveToSettings();
                    }
                });
            }
        });
    }

    fileMenu->addSeparator();
    fileMenu->addAction(exportPlainTextAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(printAction_);
    fileMenu->addAction(printPreviewAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(documentMetadataAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction_);

    // Edit menu
    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName("editMenu");
    editMenu->addAction(undoAction_);
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    editMenu->addAction(copyAction_);
    editMenu->addAction(cutAction_);
    editMenu->addAction(pasteAction_);
    editMenu->addSeparator();
    editMenu->addAction(selectAllAction_);
    editMenu->addSeparator();
    editMenu->addAction(findAction_);
    editMenu->addAction(replaceAction_);
    editMenu->addSeparator();
    // Pipeline H phase-4: selection-aware Alt Text path. Enabled iff
    // exactly one annotation is selected via SelectTool.
    if (openAltTextDialogAction_) {
        editMenu->addAction(openAltTextDialogAction_);
        editMenu->addSeparator();
    }
    editMenu->addAction(trustAnchorAction_);
    editMenu->addAction(preferencesAction_);

    // Pages menu
    QMenu* pagesMenu = menuBar()->addMenu(tr("&Pages"));
    pagesMenu->setObjectName("pagesMenu");
    pagesMenu->addAction(rotatePageCWAction_);
    pagesMenu->addAction(rotatePageCCWAction_);
    pagesMenu->addAction(rotatePage180Action_);
    pagesMenu->addSeparator();
    pagesMenu->addAction(insertBlankPageAction_);
    pagesMenu->addAction(insertPagesFromFileAction_);

    // Replace Page — local-scope action to avoid growing MainWindow member layout
    {
        auto* replaceAct = new QAction(tr("&Replace Page..."), this);
        replaceAct->setObjectName("replacePageAction");
        replaceAct->setStatusTip(tr("Replace current page with a page from another PDF"));
        connect(replaceAct, &QAction::triggered, this, [this]() {
            if (!document_ || !canvas_) return;
            int targetIdx = canvas_->currentPage();

            QString srcPath = QFileDialog::getOpenFileName(
                this, tr("Select Source PDF"), QString(), tr("PDF Files (*.pdf)"));
            if (srcPath.isEmpty()) return;

            auto srcDoc = pdfe::core::Document::open(muPdfContext_, qStringToPath(srcPath));
            if (!srcDoc) {
                // Named the file nowhere at all, which for a user who just
                // picked one of several PDFs is as unhelpful as naming the
                // wrong one. Basename, like every other dialog body.
                QMessageBox::warning(this, tr("Replace Page"),
                    tr("Failed to open source PDF: %1")
                        .arg(QFileInfo(srcPath).fileName()));
                return;
            }
            int srcPageCount = srcDoc->pageCount();
            if (srcPageCount == 0) {
                QMessageBox::warning(this, tr("Replace Page"),
                    tr("Source PDF has no pages."));
                return;
            }

            bool ok;
            int srcIdx = QInputDialog::getInt(this, tr("Replace Page"),
                tr("Source page number (1-%1):").arg(srcPageCount),
                1, 1, srcPageCount, 1, &ok);
            if (!ok) return;
            srcIdx -= 1;  // Convert 1-based UI to 0-based

            auto cmd = std::make_unique<pdfe::commands::ReplacePageCommand>(
                document_.get(), targetIdx, std::shared_ptr<pdfe::core::Document>(srcDoc.release()),
                srcIdx);
            undoManager_->execute(std::move(cmd));
            onDocumentStructureChanged();
            canvas_->refreshAllPages();
        });
        pagesMenu->addAction(replaceAct);
    }
    pagesMenu->addSeparator();
    pagesMenu->addAction(deletePageAction_);
    pagesMenu->addSeparator();
    pagesMenu->addAction(movePageUpAction_);
    pagesMenu->addAction(movePageDownAction_);
    pagesMenu->addSeparator();
    pagesMenu->addAction(extractPagesAction_);

    // Pipeline E phase-6 (Bucket 3c): Page Labels editor.
    {
        auto* labelsAct = new QAction(tr("Page &Labels..."), this);
        labelsAct->setObjectName("pageLabelsAction");
        labelsAct->setStatusTip(tr(
            "Edit per-page label ranges (Roman, decimal, alpha, prefix)"));
        connect(labelsAct, &QAction::triggered, this, [this]() {
            if (!document_) return;
            pdfe::ui::PageLabelsDialog dlg(document_.get(), this);
            // Route the edit through the undo stack so it is undoable and
            // triggers autosave (autosave-coverage Phase-2, PageLabels gap).
            dlg.setUndoManager(undoManager_);
            dlg.exec();
        });
        pagesMenu->addSeparator();
        pagesMenu->addAction(labelsAct);
    }

    // Forms menu
    QMenu* formsMenu = menuBar()->addMenu(tr("F&orms"));
    formsMenu->setObjectName("formsMenu");
    formsMenu->addAction(createFormFieldAction_);
    formsMenu->addAction(formFieldPropertiesAction_);
    formsMenu->addSeparator();
    formsMenu->addAction(exportFormDataAction_);
    formsMenu->addAction(importFormDataAction_);
    formsMenu->addSeparator();
    formsMenu->addAction(resetFormAction_);
    formsMenu->addAction(flattenFormAction_);
    formsMenu->addSeparator();
    formsMenu->addAction(tabOrderAction_);
    formsMenu->addAction(calculationOrderAction_);
    formsMenu->addAction(submitFormAction_);

    // Security menu
    QMenu* securityMenu = menuBar()->addMenu(tr("&Security"));
    securityMenu->setObjectName("securityMenu");
    securityMenu->addAction(redactToolAction_);
    securityMenu->addAction(markRedactionAction_);
    securityMenu->addSeparator();
    securityMenu->addAction(applyRedactionsAction_);
    securityMenu->addSeparator();
    securityMenu->addAction(saveWithEncryptionAction_);
    securityMenu->addAction(documentPermissionsAction_);
    securityMenu->addAction(removePasswordAction_);
    securityMenu->addSeparator();
    securityMenu->addAction(signDocumentAction_);
    securityMenu->addAction(verifySignaturesAction_);
    securityMenu->addAction(requestValidationAction_);
    securityMenu->addSeparator();
    securityMenu->addAction(toggleCopyPreventionAction_);
    securityMenu->addAction(togglePrintPreventionAction_);

    // Image menu
    QMenu* imageMenu = menuBar()->addMenu(tr("&Image"));
    imageMenu->setObjectName("imageMenu");
    imageMenu->addAction(insertImageAction_);
    imageMenu->addAction(stripExifAction_);
    imageMenu->addSeparator();
    imageMenu->addAction(imageRemoveAction_);
    imageMenu->addAction(imageRotateAction_);
    imageMenu->addAction(imageCropAction_);
    imageMenu->addAction(imageResizeAction_);

    // Tools menu
    QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName("toolsMenu");
    toolsMenu->addAction(complianceAction_);
    toolsMenu->addAction(preflightAction_);
    toolsMenu->addSeparator();
    toolsMenu->addAction(ocrAction_);
    toolsMenu->addAction(ocrAllPagesAction_);
    toolsMenu->addAction(treAction_);
    toolsMenu->addSeparator();
    toolsMenu->addAction(accessibilityAction_);
    toolsMenu->addAction(batchAction_);
    toolsMenu->addAction(compareAction_);
    // Pipeline F phase-5: Alt Text editor on Tools menu so it's
    // discoverable from keyboard + menu, not only from the canvas
    // right-click context menu.
    if (altTextAction_) toolsMenu->addAction(altTextAction_);

    // Pipeline E phase-7 (Bucket 3e): Convert to Grayscale — recolor
    // every page to DeviceGray via MuPDF's pdf_recolor_page. This
    // edits the document in-place; the user must save afterwards to
    // persist the change.
    {
        auto* grayAct = new QAction(tr("Convert to &Grayscale"), this);
        grayAct->setObjectName("convertToGrayscaleAction");
        grayAct->setStatusTip(tr(
            "Recolor all pages, images, and text to DeviceGray"));
        connect(grayAct, &QAction::triggered, this, [this]() {
            if (!document_) return;
            const int rc = QMessageBox::question(this,
                tr("Convert to Grayscale"),
                tr("Convert every page to grayscale? This recolors all "
                   "images, text, and vector content to DeviceGray. "
                   "Save the file afterwards to persist the change."),
                QMessageBox::Ok | QMessageBox::Cancel);
            if (rc != QMessageBox::Ok) return;
            auto result = pdfe::optimize::GrayscaleConverter::
                convertDocumentToGrayscale(document_.get());
            if (result.ok) {
                if (canvas_) canvas_->refreshAllPages();
                statusBar_->showMessage(tr(
                    "Converted %1 page(s) to grayscale")
                        .arg(result.pagesConverted), 5000);
            } else {
                QMessageBox::warning(this, tr("Convert to Grayscale"),
                    tr("Conversion finished with %1 page(s) failed: %2")
                        .arg(result.pagesFailed)
                        .arg(QString::fromStdString(result.errorMessage)));
            }
        });
        toolsMenu->addAction(grayAct);
    }

    // Pipeline E phase-6 (Bucket 3d): Crop tool — drag a rectangle on
    // the page to set a new /CropBox; undo restores the previous box.
    {
        auto* cropAct = new QAction(tr("&Crop Page..."), this);
        cropAct->setObjectName("cropToolAction");
        cropAct->setStatusTip(tr(
            "Drag a rectangle on the current page to set its CropBox"));
        connect(cropAct, &QAction::triggered, this, [this]() {
            if (!document_ || !canvas_) return;
            auto tool = std::make_unique<pdfe::annotations::CropTool>();
            tool->setActivePage(canvas_->currentPage());
            tool->cropApplied = [this](int pageIndex, QRectF cropRect) {
                if (!document_ || !undoManager_) return;
                auto cmd = std::make_unique<pdfe::commands::CropPageCommand>(
                    document_.get(), pageIndex, cropRect);
                undoManager_->execute(std::move(cmd));
                onDocumentStructureChanged();
                if (canvas_) canvas_->refreshAllPages();
                if (canvas_) canvas_->clearCurrentTool();
            };
            canvas_->setCurrentTool(std::move(tool));
        });
        toolsMenu->addAction(cropAct);
    }

    // Watermark (per-document) — local-scope action to avoid adding member
    {
        auto* watermarkAct = new QAction(tr("Add &Watermark..."), this);
        watermarkAct->setObjectName("watermarkAction");
        watermarkAct->setStatusTip(tr("Add a text watermark to this document"));
        connect(watermarkAct, &QAction::triggered, this, [this]() {
            if (!document_) return;
            WatermarkDialog dlg(muPdfContext_, document_.get(), this);
            dlg.exec();
        });
        toolsMenu->addAction(watermarkAct);
    }

    // Headers and Footers
    {
        auto* hfAct = new QAction(tr("Add &Headers and Footers..."), this);
        hfAct->setObjectName("headerFooterAction");
        hfAct->setToolTip(tr("Add Headers and Footers"));
        hfAct->setStatusTip(tr("Add page headers and footers to this document"));
        connect(hfAct, &QAction::triggered, this, [this]() {
            if (!document_) return;
            HeaderFooterDialog dlg(muPdfContext_, document_.get(), this);
            pdfe::ui::testseam::registerDialogIntrospection(&dlg);
            dlg.exec();
        });
        toolsMenu->addAction(hfAct);
    }

    // Phase 5a: Measurement tool submenu (distance/perimeter/area).
    {
        auto* measureMenu = toolsMenu->addMenu(tr("&Measure"));
        measureMenu->setObjectName("measureMenu");
        auto mkAction = [this, measureMenu](const QString& text,
                                            const QString& objName,
                                            pdfe::measure::MeasureMode mode) {
            auto* act = new QAction(text, this);
            act->setObjectName(objName);
            act->setStatusTip(text);
            connect(act, &QAction::triggered, this, [this, mode]() {
                if (!document_ || !annotationManager_ || !canvas_) return;
                auto page = document_->page(canvas_->currentPage());
                if (!page) return;
                auto tool = std::make_unique<pdfe::measure::MeasureTool>(
                    annotationManager_.get(), mode);
                tool->setPage(page.get());
                // GitHub #21 / PDF-57: hand the tool the scale the DOCUMENT
                // declares in its page /VP. Before this, PDFE measured an
                // architectural drawing at 1:1 against default user space no
                // matter what the drawing said its scale was.
                tool->setPageMeasures(
                    pdfe::measure::MeasureDictReader(muPdfContext_)
                        .readViewports(page.get()));
                tool->setColor(currentAnnotColor_);
                toolPage_ = std::move(page);
                tool->annotationCreated = [this](int idx, pdfe::annotations::AnnotationData d) {
                    onAnnotationCreatedByTool(idx, d);
                };
                canvas_->setCurrentTool(std::move(tool));
            });
            measureMenu->addAction(act);
            return act;
        };
        mkAction(tr("&Distance"),  "measureDistanceAction",  pdfe::measure::MeasureMode::Distance);
        mkAction(tr("&Perimeter"), "measurePerimeterAction", pdfe::measure::MeasureMode::Perimeter);
        mkAction(tr("&Area"),      "measureAreaAction",      pdfe::measure::MeasureMode::Area);
    }

    // Phase 5b: Optimize PDF
    {
        auto* optAct = new QAction(tr("&Optimize PDF..."), this);
        optAct->setObjectName("optimizePdfAction");
        optAct->setToolTip(tr("Optimize PDF"));
        optAct->setStatusTip(tr("Reduce file size: compress, subset fonts, remove unused objects"));
        connect(optAct, &QAction::triggered, this, [this]() {
            if (!document_) return;
            OptimizeDialog dlg(muPdfContext_, document_.get(), this);
            dlg.exec();
        });
        toolsMenu->addAction(optAct);
    }

    // View menu
    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName("viewMenu");
    viewMenu->addAction(zoomInAction_);
    viewMenu->addAction(zoomOutAction_);
    viewMenu->addAction(fitWidthAction_);
    viewMenu->addAction(fitPageAction_);
    viewMenu->addAction(actualSizeAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(previousViewAction_);
    viewMenu->addAction(nextViewAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(fullScreenAction_);
    // Pipeline F phase-6: group rotate-view items under a "Rotate"
    // submenu so the matrix-listed path "View > Rotate > 180"
    // navigates to a real menu structure.
    QMenu* rotateMenu = viewMenu->addMenu(tr("&Rotate"));
    rotateMenu->setObjectName("rotateMenu");
    rotateMenu->addAction(rotateViewAction_);
    rotateMenu->addAction(rotateViewCCWAction_);
    rotateMenu->addAction(rotateView180Action_);
    rotateMenu->addAction(resetViewRotationAction_);

    // Pipeline F phase-5: View > Theme submenu (Light / Dark / System)
    QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
    themeMenu->setObjectName("themeMenu");
    themeMenu->addAction(themeLightAction_);
    themeMenu->addAction(themeDarkAction_);
    themeMenu->addAction(themeSystemAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(togglePagePanelAction_);
    viewMenu->addAction(togglePropertiesPanelAction_);
    viewMenu->addAction(toggleSearchPanelAction_);
    viewMenu->addAction(toggleBookmarkPanelAction_);
    viewMenu->addAction(toggleLayerPanelAction_);
    {
        QAction* a = new QAction(tr("&Attachments Panel"), this);
        a->setObjectName("toggleAttachmentPanelAction");
        a->setCheckable(true);
        a->setChecked(false);
        connect(a, &QAction::triggered, this, [this, a](bool checked) {
            if (attachmentPanelDock_) {
                attachmentPanelDock_->setVisible(checked);
            }
        });
        // Sync action state when the dock is shown/hidden by other
        // means (e.g. auto-show on document open). The dock is
        // constructed later in this function; we connect after
        // construction below.
        toggleAttachmentPanelAction_ = a;
        viewMenu->addAction(a);
    }
    viewMenu->addSeparator();
    viewMenu->addAction(toggleStickyNotesAction_);
    viewMenu->addAction(toggleOcrBoxesAction_);
    viewMenu->addSeparator();
    // Pipeline E phase-10: Customize Toolbars...
    customizeToolbarsAction_ = new QAction(tr("&Customize Toolbars..."), this);
    customizeToolbarsAction_->setObjectName("customizeToolbarsAction");
    customizeToolbarsAction_->setStatusTip(tr("Reorder, add, or remove toolbar buttons"));
    connect(customizeToolbarsAction_, &QAction::triggered,
            this, &MainWindow::onCustomizeToolbars);
    viewMenu->addAction(customizeToolbarsAction_);

    // Help menu
    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName("helpMenu");
    helpMenu->addAction(documentationAction_);
    helpMenu->addAction(reportIssueAction_);
    helpMenu->addAction(checkUpdatesAction_);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction_);
}

// bugfix-3e91a7c4 #2: load a toolbar action icon from the action_icons.qrc
// resource (src/ui/icons/action_<name>.svg). Kept local to MainWindow.cpp so
// the icon wiring lives next to the toolbar construction that uses it.
static QIcon actionIcon(const char* name)
{
    return QIcon(QStringLiteral(":/icons/actions/%1.svg").arg(QLatin1String(name)));
}

void MainWindow::applyDeterministicTestLayout()
{
    // GitHub #136. Two things happen here, and the FIRST is the one that
    // matters: the caller has already skipped restoreGeometry()/restoreState(),
    // so the layout is now whatever createDockPanels() built rather than
    // whatever this user profile last dragged the splitters to. That alone
    // removes the variance. Pinning the widths below makes the result explicit
    // and immune to Qt sizing the docks from their content's size hint (which
    // depends on font metrics and on how long the open document's name is).
    QList<QDockWidget*> leftDocks;
    QList<QDockWidget*> rightDocks;
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        if (!dock || dock->isFloating()) {
            continue;
        }
        switch (dockWidgetArea(dock)) {
        case Qt::LeftDockWidgetArea:  leftDocks.append(dock);  break;
        case Qt::RightDockWidgetArea: rightDocks.append(dock); break;
        default: break;
        }
    }

    const auto pin = [this](const QList<QDockWidget*>& docks, int width) {
        if (docks.isEmpty()) {
            return;
        }
        QList<int> widths;
        widths.reserve(docks.size());
        for (int i = 0; i < docks.size(); ++i) {
            widths.append(width);
        }
        resizeDocks(docks, widths, Qt::Horizontal);
    };

    pin(leftDocks, pdfe::ui::testseam::kTestDeterministicLeftDockWidth);
    pin(rightDocks, pdfe::ui::testseam::kTestDeterministicRightDockWidth);

    // resizeDocks() is a layout request, and during construction the main
    // window has not been laid out yet, so the first call can be a no-op.
    // Re-apply once the event loop turns. Tests do not depend on the timing:
    // they READ the canvas rectangle from testCanvasRectQueryPipe at capture
    // time and assert the captured region matches the baseline's dimensions,
    // so a layout that failed to take is a loud failure, never a silent drift.
    QTimer::singleShot(0, this, [this, leftDocks, rightDocks, pin]() {
        pin(leftDocks, pdfe::ui::testseam::kTestDeterministicLeftDockWidth);
        pin(rightDocks, pdfe::ui::testseam::kTestDeterministicRightDockWidth);
    });
}

void MainWindow::ensurePagesDockDefault()
{
    // bugfix-3e91a7c4 #3: Pages is the primary navigation panel. A stale saved
    // windowState (restored just above in the constructor) — or a persisted
    // Panels/pagePanel=false — can leave the left dock showing only Layers, or
    // Pages hidden entirely. Re-assert the intended default: Pages visible and
    // raised as the active left-dock tab, with Layers/Bookmarks hidden until
    // their content exists (openFile re-shows them when the document has
    // layers/bookmarks). This runs on every launch so the default is stable.
    if (!pagePanelDock_) return;
    if (layerPanelDock_)    layerPanelDock_->hide();
    if (bookmarkPanelDock_) bookmarkPanelDock_->hide();
    pagePanelDock_->show();
    pagePanelDock_->raise();
    if (togglePagePanelAction_) togglePagePanelAction_->setChecked(true);
}

void MainWindow::createToolBar()
{
    // bugfix-3e91a7c4 #2: register the action_icons.qrc resource (static-lib
    // .qrc — see the global-scope forward declaration above). Idempotent.
    ::qInitResources_action_icons();

    QToolBar* toolBar = addToolBar(tr("Main Toolbar"));
    toolBar->setObjectName("mainToolBar");
    toolBar->setMovable(false);
    // bugfix-3e91a7c4 #1/#2: show icon + label per button so the toolbar reads
    // as distinct buttons rather than a "series of words".
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    openAction_->setIcon(actionIcon("open"));
    saveAction_->setIcon(actionIcon("save"));
    undoAction_->setIcon(actionIcon("undo"));
    redoAction_->setIcon(actionIcon("redo"));
    zoomInAction_->setIcon(actionIcon("zoom_in"));
    zoomOutAction_->setIcon(actionIcon("zoom_out"));
    fitWidthAction_->setIcon(actionIcon("fit_width"));
    fitPageAction_->setIcon(actionIcon("fit_page"));
    // printPreviewAction_ is not on the default toolbar, but it IS in
    // buildToolbarActionRegistry(), so a user can add it via Customize
    // Toolbars -- at which point it rendered as bare text. The artwork existed
    // and was already aliased in action_icons.qrc; only this line was missing.
    // Eleven other registry actions are still icon-less for the opposite
    // reason (no artwork exists); see the backlog row rather than assuming
    // this line completed the set.
    printPreviewAction_->setIcon(actionIcon("print_preview"));

    toolBar->addAction(openAction_);
    toolBar->addAction(saveAction_);
    toolBar->addSeparator();
    toolBar->addAction(undoAction_);
    toolBar->addAction(redoAction_);
    toolBar->addSeparator();

    toolBar->addAction(zoomInAction_);
    toolBar->addAction(zoomOutAction_);

    // Zoom combo box
    zoomCombo_ = new QComboBox(this);
    zoomCombo_->setObjectName("zoomCombo");
    zoomCombo_->setAccessibleName(tr("Zoom Level"));
    zoomCombo_->setEditable(true);
    zoomCombo_->setMinimumWidth(100);
    zoomCombo_->addItems({
        "25%", "50%", "75%", "100%",
        "125%", "150%", "200%", "400%"
    });
    zoomCombo_->setCurrentText("100%");
    zoomCombo_->setEnabled(false);
    connect(zoomCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onZoomComboChanged);
    connect(zoomCombo_, &QComboBox::editTextChanged,
            this, &MainWindow::onZoomComboEdited);
    toolBar->addWidget(zoomCombo_);

    toolBar->addAction(fitWidthAction_);
    toolBar->addAction(fitPageAction_);
}

void MainWindow::createAnnotationToolBar()
{
    annotationToolBar_ = addToolBar(tr("Annotation Toolbar"));
    annotationToolBar_->setObjectName("annotationToolBar");
    annotationToolBar_->setMovable(false);
    // bugfix-3e91a7c4 #1/#2: icon + label per tool button.
    annotationToolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Text edit toolbar (hidden by default, shown when text edit tool is active)
    addToolBarBreak();
    textEditToolBar_ = new pdfe::ui::TextEditToolBar(this);
    addToolBar(textEditToolBar_);
    textEditToolBar_->setVisible(false);

    // Form field toolbar (hidden by default, shown when form tool is active)
    formToolBar_ = addToolBar(tr("Form Fields"));
    formToolBar_->setObjectName("FormToolBar");
    formToolBar_->setVisible(false);
    // bugfix-3e91a7c4 #1: label buttons and group the field types with
    // separators so the form toolbar reads as grouped buttons, not a run of words.
    formToolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    formTextFieldAction_ = formToolBar_->addAction(tr("Text Field"));
    formTextFieldAction_->setObjectName("formTextFieldAction");
    formTextFieldAction_->setCheckable(true);
    formTextFieldAction_->setStatusTip(tr("Create text field"));

    // Group 1 | Group 2: separate the free-text field from the choice fields.
    formToolBar_->addSeparator();

    formCheckboxAction_ = formToolBar_->addAction(tr("Checkbox"));
    formCheckboxAction_->setObjectName("formCheckboxAction");
    formCheckboxAction_->setCheckable(true);
    formCheckboxAction_->setStatusTip(tr("Create checkbox"));

    formRadioButtonAction_ = formToolBar_->addAction(tr("Radio Button"));
    formRadioButtonAction_->setObjectName("formRadioButtonAction");
    formRadioButtonAction_->setCheckable(true);
    formRadioButtonAction_->setStatusTip(tr("Create radio button"));

    formComboBoxAction_ = formToolBar_->addAction(tr("Combo Box"));
    formComboBoxAction_->setObjectName("formComboBoxAction");
    formComboBoxAction_->setCheckable(true);
    formComboBoxAction_->setStatusTip(tr("Create combo box"));

    formListBoxAction_ = formToolBar_->addAction(tr("List Box"));
    formListBoxAction_->setObjectName("formListBoxAction");
    formListBoxAction_->setCheckable(true);
    formListBoxAction_->setStatusTip(tr("Create list box"));

    // Group 2 | Group 3: separate the choice fields from the button/signature group.
    formToolBar_->addSeparator();

    formPushButtonAction_ = formToolBar_->addAction(tr("Push Button"));
    formPushButtonAction_->setObjectName("formPushButtonAction");
    formPushButtonAction_->setCheckable(true);
    formPushButtonAction_->setStatusTip(tr("Create push button"));

    formSignatureAction_ = formToolBar_->addAction(tr("Signature"));
    formSignatureAction_->setObjectName("formSignatureAction");
    formSignatureAction_->setCheckable(true);
    formSignatureAction_->setStatusTip(tr("Create signature field"));

    // GitHub #253 — Delete Field.
    //
    // `DeleteFormFieldCommand` was compiled in and unit-tested with NO WAY TO
    // REACH IT: six FormCommands siblings exist, four are referenced from here,
    // and Delete was not. The product could create every field type above and
    // never remove one. That is test-gap class C-BELOW -- a correct, tested
    // component is indistinguishable from a working one when nothing calls it.
    //
    // NOT CHECKABLE, unlike every action above it: those are creation MODES that
    // stay armed, this is a one-shot act on the current selection.
    //
    // LOCAL SCOPE and a trigger-time lookup of the target, rather than a new
    // MainWindow member caching the selection -- see
    // FormFieldPropertiesPanel::currentWidgetIndex() for why. The panel is the
    // thing that knows what is selected, so there is no second copy to go stale.
    //
    // No confirmation prompt: the deletion goes through the undo stack, so
    // Ctrl+Z restores the field and its value. A modal confirm on an undoable
    // action trains people to dismiss confirms.
    {
        auto* deleteFieldAct = formToolBar_->addAction(tr("Delete Field"));
        deleteFieldAct->setObjectName("formDeleteFieldAction");
        deleteFieldAct->setStatusTip(tr("Delete the selected form field"));
        connect(deleteFieldAct, &QAction::triggered, this, [this]() {
            if (!document_ || !formManager_ || !canvas_ || !undoManager_
                || !formPropertiesPanel_) {
                return;
            }
            const int widgetIndex = formPropertiesPanel_->currentWidgetIndex();
            if (widgetIndex < 0) {
                // Nothing selected. Say so rather than failing silently -- a
                // toolbar button that does nothing without explanation reads as
                // broken.
                if (statusBar_) {
                    statusBar_->showMessage(
                        tr("Select a form field first, then Delete Field."),
                        3000);
                }
                return;
            }
            const int pageIndex = canvas_->currentPage();
            auto page = document_->page(pageIndex);
            if (!page) return;

            // Same ownership handoff the other form commands use: the command
            // holds a raw Page*, so the shared_ptr has to outlive it.
            commandPages_.push_back(std::move(page));
            auto* pagePtr = commandPages_.back().get();

            auto cmd = std::make_unique<pdfe::forms::DeleteFormFieldCommand>(
                formManager_.get(), pagePtr, widgetIndex);
            undoManager_->execute(std::move(cmd));

            // The field that panel was describing no longer exists.
            formPropertiesPanel_->clear();
            canvas_->refreshPage(pageIndex);
            updateWindowTitle();
            if (statusBar_) {
                statusBar_->showMessage(tr("Form field deleted (Ctrl+Z to undo)."),
                                        3000);
            }
        });
    }

    // P-GAP-72 (pipeline b5e3a9f2 phase-3): apply the GAP-57 deferred-
    // naming pattern to FormToolBar. The previous synchronous call to
    // formToolBar_->widgetForAction(action) returned nullptr because Qt
    // hadn't instantiated the QToolButton children yet (QToolBar
    // ActionAdded events are queued, not synchronous). The deferred pass
    // runs on the next event-loop tick after the buttons exist, sets
    // accessibleName = action->text() so pywinauto matches by Name, and
    // sets objectName = action->objectName() + "Button" mirroring the
    // annotationToolBar_ convention.
    QTimer::singleShot(0, this, [this]() {
        for (QToolButton* btn : formToolBar_->findChildren<QToolButton*>()) {
            if (QAction* act = btn->defaultAction()) {
                QString name = act->text().remove(QLatin1Char('&'));
                btn->setAccessibleName(name);
                btn->setAccessibleDescription(act->statusTip());
                btn->setObjectName(act->objectName() + "Button");
            }
        }
    });

    // GitHub #243 — connect the field-type actions ONCE, here, and resolve the
    // live FormTool at trigger time.
    //
    // These seven connections used to be made inside onAnnotationToolSelected(),
    // capturing a raw `FormTool*` that had just been allocated there. Two things
    // went wrong with that, and the second one crashed:
    //
    //   1. The receiver context is `this` (MainWindow), so every connection
    //      outlived the tool it captured. Nothing in this file ever calls
    //      disconnect() — activating the form tool N times left N live
    //      connections per action, N-1 of them holding freed pointers.
    //   2. The FormTool is destroyed on EVERY tool change
    //      (PdfCanvas::clearCurrentTool() resets the unique_ptr), and
    //      onFlattenForm() clears it while leaving formToolAction_ checked. So
    //      triggering e.g. formSignatureAction_ afterwards called
    //      setCreationFieldType() on released heap — reading manager_, page_,
    //      undoManager_ and a std::function out of a 0xDD-filled block.
    //
    // It was intermittent because the replacement FormTool is allocated
    // immediately after the old one is freed and is the same size, so the
    // allocator usually hands back the very block just released and the stale
    // pointer happens to alias the live object. Whether it does depends on heap
    // state, which is why a heap-perturbing test run ahead of it changed the
    // outcome.
    //
    // Resolving through canvas_->currentTool() has neither problem: one
    // connection per action for the window's lifetime, and the pointer is
    // whatever is alive NOW — or nothing, which is a no-op rather than a crash.
    // This is the pattern already used correctly elsewhere in this file.
    auto connectFieldTypeAction = [this](QAction* action,
                                         pdfe::forms::FormFieldType type) {
        connect(action, &QAction::triggered, this, [this, type]() {
            if (!canvas_) return;
            if (auto* formTool = dynamic_cast<pdfe::forms::FormTool*>(
                    canvas_->currentTool())) {
                formTool->setCreationFieldType(type);
            }
        });
    };
    connectFieldTypeAction(formTextFieldAction_, pdfe::forms::FormFieldType::Text);
    connectFieldTypeAction(formCheckboxAction_, pdfe::forms::FormFieldType::Checkbox);
    connectFieldTypeAction(formRadioButtonAction_, pdfe::forms::FormFieldType::RadioButton);
    connectFieldTypeAction(formComboBoxAction_, pdfe::forms::FormFieldType::ComboBox);
    connectFieldTypeAction(formListBoxAction_, pdfe::forms::FormFieldType::ListBox);
    connectFieldTypeAction(formPushButtonAction_, pdfe::forms::FormFieldType::PushButton);
    connectFieldTypeAction(formSignatureAction_, pdfe::forms::FormFieldType::Signature);

    // Make form field actions checkable and exclusive
    auto* formTypeGroup = new QActionGroup(this);
    formTypeGroup->setExclusive(true);
    for (auto* action : {formTextFieldAction_, formCheckboxAction_,
                          formRadioButtonAction_,
                          formComboBoxAction_, formListBoxAction_,
                          formPushButtonAction_, formSignatureAction_}) {
        formTypeGroup->addAction(action);
    }
    formTextFieldAction_->setChecked(true);

    // Create action group (exclusive, checkable)
    annotToolGroup_ = new QActionGroup(this);
    annotToolGroup_->setExclusive(true);

    // Select tool (default)
    selectToolAction_ = new QAction(tr("Select"), this);
    selectToolAction_->setObjectName("selectToolAction");
    selectToolAction_->setIcon(actionIcon("select"));
    selectToolAction_->setCheckable(true);
    selectToolAction_->setChecked(true);
    selectToolAction_->setStatusTip(tr("Select and interact with annotations"));
    annotToolGroup_->addAction(selectToolAction_);
    annotationToolBar_->addAction(selectToolAction_);

    annotationToolBar_->addSeparator();

    // Sticky note tool
    stickyNoteToolAction_ = new QAction(tr("Sticky Note"), this);
    stickyNoteToolAction_->setObjectName("stickyNoteToolAction");
    stickyNoteToolAction_->setIcon(actionIcon("sticky_note"));
    stickyNoteToolAction_->setCheckable(true);
    stickyNoteToolAction_->setStatusTip(tr("Add sticky note annotation"));
    annotToolGroup_->addAction(stickyNoteToolAction_);
    annotationToolBar_->addAction(stickyNoteToolAction_);

    // Highlight tool
    highlightToolAction_ = new QAction(tr("Highlight"), this);
    highlightToolAction_->setObjectName("highlightToolAction");
    highlightToolAction_->setIcon(actionIcon("highlight"));
    highlightToolAction_->setCheckable(true);
    highlightToolAction_->setStatusTip(tr("Highlight text"));
    annotToolGroup_->addAction(highlightToolAction_);
    annotationToolBar_->addAction(highlightToolAction_);

    // Underline tool
    underlineToolAction_ = new QAction(tr("Underline"), this);
    underlineToolAction_->setObjectName("underlineToolAction");
    underlineToolAction_->setIcon(actionIcon("underline"));
    underlineToolAction_->setCheckable(true);
    underlineToolAction_->setStatusTip(tr("Underline text"));
    annotToolGroup_->addAction(underlineToolAction_);
    annotationToolBar_->addAction(underlineToolAction_);

    // StrikeOut tool
    strikeOutToolAction_ = new QAction(tr("Strike Out"), this);
    strikeOutToolAction_->setObjectName("strikeOutToolAction");
    strikeOutToolAction_->setIcon(actionIcon("strikeout"));
    strikeOutToolAction_->setCheckable(true);
    strikeOutToolAction_->setStatusTip(tr("Strike out text"));
    annotToolGroup_->addAction(strikeOutToolAction_);
    annotationToolBar_->addAction(strikeOutToolAction_);

    annotationToolBar_->addSeparator();

    // Form tool
    formToolAction_->setIcon(actionIcon("form"));
    annotToolGroup_->addAction(formToolAction_);
    annotationToolBar_->addAction(formToolAction_);

    // Image tool
    imageToolAction_->setIcon(actionIcon("image"));
    annotToolGroup_->addAction(imageToolAction_);
    annotationToolBar_->addAction(imageToolAction_);

    // Text edit tool (auto-routes between native and scanned modes)
    textEditToolAction_->setIcon(actionIcon("edit_text"));
    annotToolGroup_->addAction(textEditToolAction_);
    annotationToolBar_->addAction(textEditToolAction_);

    // Redaction tool
    redactToolAction_->setIcon(actionIcon("redaction"));
    annotToolGroup_->addAction(redactToolAction_);
    annotationToolBar_->addAction(redactToolAction_);

    annotationToolBar_->addSeparator();

    // Ink tool
    inkToolAction_ = new QAction(tr("Ink"), this);
    inkToolAction_->setObjectName("inkToolAction");
    inkToolAction_->setIcon(actionIcon("ink"));
    inkToolAction_->setCheckable(true);
    inkToolAction_->setStatusTip(tr("Draw freehand"));
    annotToolGroup_->addAction(inkToolAction_);
    annotationToolBar_->addAction(inkToolAction_);

    // FreeText tool
    freeTextToolAction_ = new QAction(tr("Text Box"), this);
    freeTextToolAction_->setObjectName("freeTextToolAction");
    freeTextToolAction_->setIcon(actionIcon("text_box"));
    freeTextToolAction_->setCheckable(true);
    freeTextToolAction_->setStatusTip(tr("Add text box"));
    annotToolGroup_->addAction(freeTextToolAction_);
    annotationToolBar_->addAction(freeTextToolAction_);

    annotationToolBar_->addSeparator();

    // Rectangle tool
    rectangleToolAction_ = new QAction(tr("Rectangle"), this);
    rectangleToolAction_->setObjectName("rectangleToolAction");
    rectangleToolAction_->setIcon(actionIcon("rectangle"));
    rectangleToolAction_->setCheckable(true);
    rectangleToolAction_->setStatusTip(tr("Draw rectangle"));
    annotToolGroup_->addAction(rectangleToolAction_);
    annotationToolBar_->addAction(rectangleToolAction_);

    // Circle tool
    circleToolAction_ = new QAction(tr("Circle"), this);
    circleToolAction_->setObjectName("circleToolAction");
    circleToolAction_->setIcon(actionIcon("circle"));
    circleToolAction_->setCheckable(true);
    circleToolAction_->setStatusTip(tr("Draw circle"));
    annotToolGroup_->addAction(circleToolAction_);
    annotationToolBar_->addAction(circleToolAction_);

    // Line tool
    lineToolAction_ = new QAction(tr("Line"), this);
    lineToolAction_->setObjectName("lineToolAction");
    lineToolAction_->setIcon(actionIcon("line"));
    lineToolAction_->setCheckable(true);
    lineToolAction_->setStatusTip(tr("Draw line"));
    annotToolGroup_->addAction(lineToolAction_);
    annotationToolBar_->addAction(lineToolAction_);

    // Stamp tool — local-scope action to avoid growing MainWindow member layout.
    // Uses currentAnnotColor_ for stamp color; dispatches to StampTool via
    // PdfCanvas::setCurrentTool when activated.
    {
        auto* stampAct = new QAction(tr("Stamp"), this);
        stampAct->setObjectName("stampToolAction");
        stampAct->setIcon(actionIcon("stamp"));
        stampAct->setCheckable(true);
        stampAct->setStatusTip(tr("Place a stamp (Draft/Approved/Confidential)"));
        annotToolGroup_->addAction(stampAct);
        annotationToolBar_->addAction(stampAct);
        connect(stampAct, &QAction::triggered, this, [this, stampAct](bool checked) {
            if (!checked) { if (canvas_) canvas_->clearCurrentTool(); return; }
            if (!document_ || !annotationManager_ || !canvas_) return;
            auto page = document_->page(canvas_->currentPage());
            if (!page) return;

            // Phase 4f: offer built-in dynamic stamps + a free-text option.
            // Pipeline E phase-4: also offer any image stamps the user
            // has imported into the library.
            QStringList choices = {
                tr("Draft (dynamic: date)"),
                tr("Approved (dynamic: author + date)"),
                tr("Reviewed (dynamic: author + date)"),
                tr("Custom (plain text)..."),
            };
            const int firstImageIdx = choices.size();
            QStringList imageStampNames;
            if (imageStampLibrary_) {
                imageStampNames = imageStampLibrary_->stampNames();
                for (const QString& n : imageStampNames) {
                    choices.push_back(tr("Image: %1").arg(n));
                }
            }
            bool ok = false;
            const QString pick = QInputDialog::getItem(this, tr("Stamp Type"),
                tr("Choose a stamp:"), choices, 0, false, &ok);
            if (!ok) { stampAct->setChecked(false); return; }

            QString stampName;
            QImage stampImage;
            using DT = pdfe::annotations::StampTool::DynamicType;
            const int pickIdx = choices.indexOf(pick);
            if (pickIdx == 0) {
                stampName = pdfe::annotations::StampTool::builtInTemplate(DT::Draft);
            } else if (pickIdx == 1) {
                stampName = pdfe::annotations::StampTool::builtInTemplate(DT::Approved);
            } else if (pickIdx == 2) {
                stampName = pdfe::annotations::StampTool::builtInTemplate(DT::Reviewed);
            } else if (pickIdx == 3) {
                stampName = QInputDialog::getText(this, tr("Stamp Text"),
                    tr("Stamp label (supports {date} and {author} tokens):"),
                    QLineEdit::Normal, tr("Draft"), &ok);
                if (!ok || stampName.isEmpty()) {
                    stampAct->setChecked(false); return;
                }
            } else if (pickIdx >= firstImageIdx && imageStampLibrary_) {
                const int imgIdx = pickIdx - firstImageIdx;
                if (imgIdx < imageStampNames.size()) {
                    stampName = imageStampNames[imgIdx];
                    stampImage = imageStampLibrary_->stamp(stampName);
                }
                if (stampImage.isNull()) {
                    stampAct->setChecked(false); return;
                }
            }

            auto tool = std::make_unique<pdfe::annotations::StampTool>(
                annotationManager_.get());
            tool->setPage(page.get());
            tool->setColor(currentAnnotColor_);
            tool->setStampName(stampName);
            tool->setAuthor(appSettings_ ? appSettings_->author() : QString());
            if (!stampImage.isNull()) {
                tool->setImageStamp(stampName, stampImage);
            }
            // Keep page alive for the active tool (pattern from other tools)
            toolPage_ = std::move(page);
            tool->annotationCreated = [this](int pageIdx, pdfe::annotations::AnnotationData d) {
                onAnnotationCreatedByTool(pageIdx, d);
            };
            canvas_->setCurrentTool(std::move(tool));
        });
    }

    // GitHub #253 — the Link annotation tool, the eleventh of eleven.
    //
    // Ten of PDFE's eleven annotation tools were reachable from main(); this one
    // alone was not. It was compiled in and unit-tested, so it looked no
    // different from a tool that worked — which is test-gap class C-BELOW
    // exactly: "a feature that is correct and UNREACHABLE looks identical to one
    // that works". `check_engine_wiring.py` is the standing control, and
    // LinkTool's entry is removed from its baseline by this change, so the guard
    // now FAILS if main() ever stops reaching it again.
    //
    // LOCAL SCOPE, deliberately, following the Stamp and FileAttachment tools
    // above and below. A new QAction* member on MainWindow would change the
    // class layout, and a stale .obj surviving a relink after a layout change is
    // a failure this project has already paid for. Nothing here needs to be a
    // member: the action is owned by `this`, the tool group and toolbar hold it,
    // and the lambda resolves everything it needs at trigger time.
    {
        auto* linkAct = new QAction(tr("Link"), this);
        linkAct->setObjectName("linkToolAction");
        linkAct->setIcon(actionIcon("link"));
        linkAct->setCheckable(true);
        linkAct->setStatusTip(tr("Draw a link region and attach a URL, "
                                 "mailto: or #page=N destination"));
        annotToolGroup_->addAction(linkAct);
        annotationToolBar_->addAction(linkAct);
        connect(linkAct, &QAction::triggered, this, [this](bool checked) {
            if (!checked) { if (canvas_) canvas_->clearCurrentTool(); return; }
            if (!document_ || !annotationManager_ || !canvas_) return;
            auto page = document_->page(canvas_->currentPage());
            if (!page) return;

            auto tool = std::make_unique<pdfe::annotations::LinkTool>(
                annotationManager_.get());
            tool->setPage(page.get());
            // Keep the page alive for as long as the tool holds a raw pointer to
            // it -- the same ownership handoff every other tool here performs.
            toolPage_ = std::move(page);
            tool->annotationCreated = [this](int pageIdx,
                                             pdfe::annotations::AnnotationData d) {
                onAnnotationCreatedByTool(pageIdx, d);
            };
            canvas_->setCurrentTool(std::move(tool));
        });
    }

    // Pipeline E phase-5: /FileAttachment annotation tool. Local
    // scope to avoid growing MainWindow member layout. On activation
    // prompts for the source file and icon style, then drops the
    // selected file's bytes onto the page as a /FileAttachment.
    {
        auto* attachAct = new QAction(tr("Attach File"), this);
        attachAct->setObjectName("fileAttachmentToolAction");
        attachAct->setIcon(actionIcon("attach_file"));
        attachAct->setCheckable(true);
        attachAct->setStatusTip(tr(
            "Attach a file as an on-page icon (PDF /FileAttachment)"));
        annotToolGroup_->addAction(attachAct);
        annotationToolBar_->addAction(attachAct);
        connect(attachAct, &QAction::triggered, this,
                [this, attachAct](bool checked) {
            if (!checked) { if (canvas_) canvas_->clearCurrentTool(); return; }
            if (!document_ || !annotationManager_ || !canvas_) return;
            auto page = document_->page(canvas_->currentPage());
            if (!page) return;

            const QString filePath = QFileDialog::getOpenFileName(
                this, tr("Attach File"), QString(),
                tr("All Files (*)"));
            if (filePath.isEmpty()) {
                attachAct->setChecked(false); return;
            }

            const QStringList iconChoices = {
                QStringLiteral("PushPin"),
                QStringLiteral("PaperClip"),
                QStringLiteral("Graph"),
                QStringLiteral("Tag"),
            };
            bool ok = false;
            const QString icon = QInputDialog::getItem(this,
                tr("Attachment Icon"),
                tr("Choose an icon style:"),
                iconChoices, 0, false, &ok);
            if (!ok) { attachAct->setChecked(false); return; }

            auto tool = std::make_unique<pdfe::annotations::FileAttachmentTool>(
                annotationManager_.get());
            tool->setPage(page.get());
            tool->setColor(currentAnnotColor_);
            tool->setSourceFile(filePath.toStdString());
            tool->setIconName(icon.toStdString());
            toolPage_ = std::move(page);
            tool->annotationCreated =
                [this](int pageIdx, pdfe::annotations::AnnotationData d) {
                onAnnotationCreatedByTool(pageIdx, d);
            };
            canvas_->setCurrentTool(std::move(tool));
        });
    }

    annotationToolBar_->addSeparator();

    // Color picker button
    colorPickerButton_ = new QPushButton(this);
    colorPickerButton_->setObjectName("annotColorPickerButton");
    colorPickerButton_->setAccessibleName(tr("Annotation Color Picker"));
    colorPickerButton_->setFixedSize(32, 32);
    colorPickerButton_->setStyleSheet(
        QString("QPushButton { background-color: rgba(%1, %2, %3, 255); }")
            .arg(currentAnnotColor_.red())
            .arg(currentAnnotColor_.green())
            .arg(currentAnnotColor_.blue())
    );
    colorPickerButton_->setStatusTip(tr("Choose annotation color"));
    annotationToolBar_->addWidget(colorPickerButton_);

    // GAP-57 fix: set accessible names + objectName on toolbar buttons for
    // UI automation (pywinauto). Must be deferred to the next event-loop
    // cycle because QToolBar::addAction() only queues ActionAdded events
    // — the QToolButton children are not instantiated until that event is
    // processed. Walking findChildren<QToolButton*>() synchronously here
    // finds ZERO buttons and silently sets no names, causing all 97
    // toolbar-button-reachability ui_automation tests to fail with
    // "*ToolActionButton not reachable".
    auto applyToolBarNames = [](QToolBar* tb) {
        for (QToolButton* btn : tb->findChildren<QToolButton*>()) {
            if (QAction* act = btn->defaultAction()) {
                QString name = act->text().remove(QLatin1Char('&'));
                btn->setAccessibleName(name);
                btn->setAccessibleDescription(act->statusTip());
                btn->setObjectName(act->objectName() + "Button");
            }
        }
    };
    QTimer::singleShot(0, this, [this, applyToolBarNames]() {
        applyToolBarNames(annotationToolBar_);
    });

    // Connect signals
    connect(annotToolGroup_, &QActionGroup::triggered,
            this, &MainWindow::onAnnotationToolSelected);
    connect(colorPickerButton_, &QPushButton::clicked,
            this, &MainWindow::onAnnotationColorPicker);
}

void MainWindow::createStatusBar()
{
    statusBar_ = new StatusBar(this);
    statusBar_->setObjectName("statusBar");
    setStatusBar(statusBar_);
}

void MainWindow::createDockPanels()
{
    // Page panel (left)
    pagePanel_ = new PagePanel(this);
    pagePanel_->setObjectName("pagePanel");
    connect(pagePanel_, &PagePanel::pageSelected, this, &MainWindow::onPageSelected);
    connect(pagePanel_, &PagePanel::pageMoved, this, &MainWindow::onPageMoved);
    connect(pagePanel_, &PagePanel::pagesDeleteRequested, this, [this](QList<int> indices) {
        onDeletePages();
    });
    connect(pagePanel_, &PagePanel::rotatePagesCW, this, [this](QList<int>) {
        onRotatePageCW();
    });
    connect(pagePanel_, &PagePanel::rotatePagesCCW, this, [this](QList<int>) {
        onRotatePageCCW();
    });
    connect(pagePanel_, &PagePanel::rotatePages180, this, [this](QList<int>) {
        onRotatePage180();
    });
    connect(pagePanel_, &PagePanel::extractPagesRequested, this, [this](QList<int>) {
        onExtractPages();
    });
    connect(pagePanel_, &PagePanel::pageInsertRequested, this, [this](int) {
        onInsertBlankPage();
    });

    pagePanelDock_ = new QDockWidget(tr("Pages"), this);
    pagePanelDock_->setObjectName("pagePanelDock");
    pagePanelDock_->setWidget(pagePanel_);
    pagePanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, pagePanelDock_);
    connect(pagePanelDock_, &QDockWidget::visibilityChanged,
            togglePagePanelAction_, &QAction::setChecked);

    // Bookmark panel (left, tabbed with pages)
    bookmarkPanel_ = new BookmarkPanel(this);
    bookmarkPanel_->setObjectName("bookmarkPanel");
    bookmarkPanel_->setContext(muPdfContext_);
    bookmarkPanel_->setUndoManager(undoManager_);
    connect(bookmarkPanel_, &BookmarkPanel::bookmarkActivated,
            this, &MainWindow::onBookmarkActivated);

    bookmarkPanelDock_ = new QDockWidget(tr("Bookmarks"), this);
    bookmarkPanelDock_->setObjectName("bookmarkPanelDock");
    bookmarkPanelDock_->setWidget(bookmarkPanel_);
    bookmarkPanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, bookmarkPanelDock_);
    tabifyDockWidget(pagePanelDock_, bookmarkPanelDock_);
    // Hide by default — only shown when PDF has bookmarks (see openFile)
    bookmarkPanelDock_->hide();
    toggleBookmarkPanelAction_->setChecked(false);
    connect(bookmarkPanelDock_, &QDockWidget::visibilityChanged,
            toggleBookmarkPanelAction_, &QAction::setChecked);

    // Layer panel (left, tabbed with pages/bookmarks)
    layerPanel_ = new LayerPanel(this);
    layerPanel_->setObjectName("layerPanel");
    layerPanel_->setContext(muPdfContext_);
    layerPanel_->setUndoManager(undoManager_);
    connect(layerPanel_, &LayerPanel::layerVisibilityChanged, this, [this]() {
        if (canvas_) canvas_->refreshAllPages();
    });

    layerPanelDock_ = new QDockWidget(tr("Layers"), this);
    layerPanelDock_->setObjectName("layerPanelDock");
    layerPanelDock_->setWidget(layerPanel_);
    layerPanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, layerPanelDock_);
    tabifyDockWidget(pagePanelDock_, layerPanelDock_);
    layerPanelDock_->hide();
    connect(layerPanelDock_, &QDockWidget::visibilityChanged,
            toggleLayerPanelAction_, &QAction::setChecked);

    // Search panel (right sidebar, first tab)
    searchPanel_ = new SearchPanel(this);
    searchPanel_->setObjectName("searchPanel");
    connect(searchPanel_, &SearchPanel::searchResultSelected,
            this, &MainWindow::onSearchResultSelected);
    connect(searchPanel_, &SearchPanel::clearSearchHighlights,
            canvas_, &PdfCanvas::clearSearchHighlights);
    connect(searchPanel_, &SearchPanel::replaceRequested,
            this, &MainWindow::onReplace);
    connect(searchPanel_, &SearchPanel::replaceAllRequested,
            this, &MainWindow::onReplaceAll);

    searchPanelDock_ = new QDockWidget(tr("Search"), this);
    searchPanelDock_->setObjectName("searchPanelDock");
    searchPanelDock_->setWidget(searchPanel_);
    searchPanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, searchPanelDock_);
    connect(searchPanelDock_, &QDockWidget::visibilityChanged,
            toggleSearchPanelAction_, &QAction::setChecked);

    // Properties panel (right, tabbed after Search)
    propertiesPanel_ = new PropertiesPanel(this);
    propertiesPanel_->setObjectName("propertiesPanel");

    propertiesPanelDock_ = new QDockWidget(tr("Properties"), this);
    propertiesPanelDock_->setObjectName("propertiesPanelDock");
    propertiesPanelDock_->setWidget(propertiesPanel_);
    propertiesPanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, propertiesPanelDock_);
    tabifyDockWidget(searchPanelDock_, propertiesPanelDock_);
    connect(propertiesPanelDock_, &QDockWidget::visibilityChanged,
            togglePropertiesPanelAction_, &QAction::setChecked);

    // Defaults log panel (right, will be tabified last after Form Properties)
    defaultsLogPanel_ = new DefaultsLogPanel(this);
    defaultsLogPanel_->setObjectName("defaultsLogPanel");
    setDefaultsLogPanel(defaultsLogPanel_); // Register global accessor

    defaultsLogDock_ = new QDockWidget(tr("Defaults Log"), this);
    defaultsLogDock_->setObjectName("defaultsLogDock");
    defaultsLogDock_->setWidget(defaultsLogPanel_);
    defaultsLogDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, defaultsLogDock_);

    // Comment panel (right)
    commentPanel_ = new CommentPanel(this);
    commentPanel_->setObjectName("commentPanel");
    connect(commentPanel_, &CommentPanel::navigateToAnnotation,
            this, &MainWindow::onCommentPanelNavigate);
    connect(commentPanel_, &CommentPanel::deleteAnnotation,
            this, &MainWindow::onCommentPanelDelete);

    commentPanelDock_ = new QDockWidget(tr("Comments"), this);
    commentPanelDock_->setObjectName("commentPanelDock");
    commentPanelDock_->setWidget(commentPanel_);
    commentPanelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, commentPanelDock_);

    // Annotation properties (right, tabbed with Properties)
    annotationProperties_ = new AnnotationPropertiesWidget(this);
    annotationProperties_->setObjectName("annotationPropertiesWidget");
    connect(annotationProperties_, &AnnotationPropertiesWidget::propertiesChanged,
            this, &MainWindow::onAnnotationPropertiesChanged);

    annotationPropertiesDock_ = new QDockWidget(tr("Annotation Properties"), this);
    annotationPropertiesDock_->setObjectName("annotationPropertiesDock");
    annotationPropertiesDock_->setWidget(annotationProperties_);
    annotationPropertiesDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, annotationPropertiesDock_);
    tabifyDockWidget(propertiesPanelDock_, annotationPropertiesDock_);

    // Form field properties (right, tabbed after Annotation Properties)
    formPropertiesPanel_ = new FormFieldPropertiesPanel(this);
    formPropertiesPanel_->setObjectName("formFieldPropertiesPanel");
    connect(formPropertiesPanel_, &FormFieldPropertiesPanel::fieldValueChanged,
            this, &MainWindow::onFormFieldValueChanged);

    formPropertiesDock_ = new QDockWidget(tr("Form Properties"), this);
    formPropertiesDock_->setObjectName("formPropertiesDock");
    formPropertiesDock_->setWidget(formPropertiesPanel_);
    formPropertiesDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, formPropertiesDock_);
    tabifyDockWidget(annotationPropertiesDock_, formPropertiesDock_);

    // Now tabify Defaults Log as the last tab, after Form Properties
    tabifyDockWidget(formPropertiesDock_, defaultsLogDock_);

    // Make Search the default active tab on startup
    searchPanelDock_->raise();

    // Redaction panel
    redactionPanelDock_ = new QDockWidget(tr("Redactions"), this);
    redactionPanelDock_->setObjectName("redactionPanelDock");
    redactionPanel_ = new RedactionPanel(this);
    redactionPanelDock_->setWidget(redactionPanel_);
    addDockWidget(Qt::RightDockWidgetArea, redactionPanelDock_);
    redactionPanelDock_->hide();

    connect(redactionPanel_, &RedactionPanel::applyAllRequested,
            this, &MainWindow::onApplyRedactions);
    connect(redactionPanel_, &RedactionPanel::deleteMarkRequested,
            this, &MainWindow::onDeleteRedactionMark);
    connect(redactionPanel_, &RedactionPanel::markSelected,
            this, &MainWindow::onRedactionMarkSelected);

    // GitHub #253 — editing a pending redaction mark.
    //
    // `ModifyRedactionCommand` was compiled in, unit-tested, and unreachable
    // from main(). The issue reads as though an undo step were missing next to a
    // working operation; in fact there was NO modify operation -- this panel
    // could apply and delete and nothing else, so there was nothing for the
    // command to undo. The Properties... button it responds to is new.
    //
    // A LAMBDA rather than a new MainWindow slot, so MainWindow.h is untouched:
    // every other per-tool action in this file does the same, and a header
    // change here means every translation unit that includes it must recompile
    // (and a stale .obj surviving a relink is a failure this project has
    // already paid for).
    //
    // ONE COMMAND PER CHANGED PROPERTY, because ModifyRedactionCommand's
    // Property enum is per-property. Changing both colour and text therefore
    // pushes two commands -- the class provides mergeWith()/mergeId() and the
    // undo manager coalesces them where it can, so this is not two user-visible
    // undo steps by construction. Issuing a single command with a composite
    // "both" property would mean inventing an enumerator the command does not
    // have.
    connect(redactionPanel_, &RedactionPanel::modifyMarkRequested, this,
            [this](int pageIndex, int annotIndex) {
        if (!document_ || !redactionManager_ || !undoManager_ || !canvas_) {
            return;
        }
        auto page = document_->page(pageIndex);
        if (!page) return;

        // Read the CURRENT state as the "old" side of the command. Taken from
        // the manager rather than from the panel's tree text, which is a
        // formatted, truncated rendering (overlay text over 30 characters is
        // elided with "...") and would write that ellipsis into the document.
        const auto marks = redactionManager_->getAllRedactionMarks(document_.get());
        const pdfe::redaction::RedactionData* found = nullptr;
        for (const auto& m : marks) {
            if (m.pageIndex == pageIndex && m.annotIndex == annotIndex) {
                found = &m;
                break;
            }
        }
        if (!found) {
            if (statusBar_) {
                statusBar_->showMessage(
                    tr("That redaction mark no longer exists."), 3000);
            }
            return;
        }
        const pdfe::redaction::RedactionData oldData = *found;

        // Overlay text first.
        bool ok = false;
        const QString newText = QInputDialog::getText(
            this, tr("Redaction Mark Properties"),
            tr("Overlay text (shown in place of the redacted content; "
               "leave empty for none):"),
            QLineEdit::Normal,
            QString::fromStdString(oldData.overlayText), &ok);
        if (!ok) return;  // Cancelled: change nothing, not even the colour.

        const QColor newColor = QColorDialog::getColor(
            oldData.fillColor, this, tr("Redaction Fill Colour"));
        // An invalid QColor means the colour dialog was cancelled. Keep the
        // text edit the user already confirmed rather than discarding it.
        const QColor effectiveColor =
            newColor.isValid() ? newColor : oldData.fillColor;

        commandPages_.push_back(std::move(page));
        auto* pagePtr = commandPages_.back().get();

        using MRC = pdfe::redaction::ModifyRedactionCommand;
        int issued = 0;

        if (newText.toStdString() != oldData.overlayText) {
            auto newData = oldData;
            newData.overlayText = newText.toStdString();
            undoManager_->execute(std::make_unique<MRC>(
                redactionManager_.get(), pagePtr, annotIndex,
                MRC::Property::OverlayText, oldData, newData));
            ++issued;
        }
        if (effectiveColor != oldData.fillColor) {
            auto newData = oldData;
            newData.fillColor = effectiveColor;
            undoManager_->execute(std::make_unique<MRC>(
                redactionManager_.get(), pagePtr, annotIndex,
                MRC::Property::FillColor, oldData, newData));
            ++issued;
        }

        if (issued == 0) {
            if (statusBar_) {
                statusBar_->showMessage(tr("No changes to the redaction mark."),
                                        2000);
            }
            return;
        }

        // Re-read rather than mutating the panel's copy: the manager is the
        // source of truth and the command has just changed it.
        redactionPanel_->setMarks(
            redactionManager_->getAllRedactionMarks(document_.get()));
        canvas_->refreshPage(pageIndex);
        updateWindowTitle();
        if (statusBar_) {
            statusBar_->showMessage(
                tr("Redaction mark updated (Ctrl+Z to undo)."), 3000);
        }
    });

    // Signature panel
    signaturePanelDock_ = new QDockWidget(tr("Signatures"), this);
    signaturePanelDock_->setObjectName("signaturePanelDock");
    signaturePanel_ = new SignaturePanel(this);
    signaturePanelDock_->setWidget(signaturePanel_);
    addDockWidget(Qt::RightDockWidgetArea, signaturePanelDock_);
    signaturePanelDock_->hide();

    connect(signaturePanel_, &SignaturePanel::signRequested,
            this, [this](int pageIndex, int widgetIndex) {
                Q_UNUSED(pageIndex);
                Q_UNUSED(widgetIndex);
                onSignDocument();
            });
    connect(signaturePanel_, &SignaturePanel::verifyAllRequested,
            this, &MainWindow::onVerifySignatures);

    // Attachment panel — lists /EmbeddedFiles attachments. Hidden by
    // default; auto-shown on document open if the doc has attachments,
    // or revealed via View menu toggle.
    attachmentPanelDock_ = new QDockWidget(tr("Attachments"), this);
    attachmentPanelDock_->setObjectName("attachmentPanelDock");
    attachmentPanel_ = new AttachmentPanel(this);
    attachmentPanel_->setObjectName("attachmentPanel");
    attachmentPanelDock_->setWidget(attachmentPanel_);
    attachmentPanelDock_->setAllowedAreas(
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, attachmentPanelDock_);
    attachmentPanelDock_->hide();
    connect(attachmentPanel_, &AttachmentPanel::attachmentsChanged,
            this, [this]() {
                // AttachmentManager mutates /EmbeddedFiles directly on
                // the underlying pdf_obj, so the document is dirty as
                // soon as the panel reports a change. Refresh the
                // window title so the leading '*' appears.
                updateWindowTitle();
            });
    // Keep the View menu toggle action in sync with the dock's
    // visibility (auto-show on document-open + manual toggle both
    // route through visibilityChanged).
    if (toggleAttachmentPanelAction_) {
        connect(attachmentPanelDock_, &QDockWidget::visibilityChanged,
                toggleAttachmentPanelAction_, &QAction::setChecked);
    }

    // Image properties panel
    imagePropertiesDock_ = new QDockWidget(tr("Image Properties"), this);
    imagePropertiesDock_->setObjectName("imagePropertiesDock");
    imagePropertiesPanel_ = new pdfe::image::ImagePropertiesPanel(imagePropertiesDock_);
    // P-GAP-92 / b5e3a9f2 phase-5: expose the panel widget itself by
    // objectName so test_canvas_phase3[canvas_p4_image_select_click] can
    // find it via find_by_suffix("imagePropertiesPanel"). Without this,
    // the inner widget's automation_id is empty and the test fails with
    // "imagePropertiesPanel not found after ImageTool click".
    imagePropertiesPanel_->setObjectName("imagePropertiesPanel");
    imagePropertiesDock_->setWidget(imagePropertiesPanel_);
    addDockWidget(Qt::RightDockWidgetArea, imagePropertiesDock_);
    imagePropertiesDock_->hide();

    // Sticky note popup.
    // Pipeline b3f19d72 phase-6 (P8-BUG-1): parented to MainWindow (not
    // canvas_) and constructed with Qt::Tool | Qt::FramelessWindowHint
    // inside StickyNotePopup so the popup is a top-level frameless tool
    // window — discoverable in the UIA tree by pywinauto + screen
    // readers, where a child of PdfCanvas would be hidden behind that
    // widget's QAccessible::Canvas role.
    stickyNotePopup_ = new StickyNotePopup(this);
    stickyNotePopup_->setObjectName("stickyNotePopup");
    stickyNotePopup_->hide();

    connect(stickyNotePopup_, &StickyNotePopup::contentsChanged,
            this, &MainWindow::onStickyNotePopupContentsChanged);
    connect(stickyNotePopup_, &StickyNotePopup::popupClosed,
            this, &MainWindow::onStickyNotePopupClosed);
}

void MainWindow::setupAccessibleNames()
{
    // Dock panels
    pagePanelDock_->setAccessibleName(tr("Pages Panel"));
    pagePanelDock_->setAccessibleDescription(tr("Thumbnail page navigation panel"));
    bookmarkPanelDock_->setAccessibleName(tr("Bookmarks Panel"));
    bookmarkPanelDock_->setAccessibleDescription(tr("Document bookmarks navigation panel"));
    propertiesPanelDock_->setAccessibleName(tr("Properties Panel"));
    propertiesPanelDock_->setAccessibleDescription(tr("Document and page properties panel"));
    searchPanelDock_->setAccessibleName(tr("Search Panel"));
    searchPanelDock_->setAccessibleDescription(tr("Text search panel"));
    commentPanelDock_->setAccessibleName(tr("Comments Panel"));
    commentPanelDock_->setAccessibleDescription(tr("Annotation comments panel"));
    annotationPropertiesDock_->setAccessibleName(tr("Annotation Properties Panel"));
    formPropertiesDock_->setAccessibleName(tr("Form Properties Panel"));
    redactionPanelDock_->setAccessibleName(tr("Redactions Panel"));
    signaturePanelDock_->setAccessibleName(tr("Signatures Panel"));
    imagePropertiesDock_->setAccessibleName(tr("Image Properties Panel"));
    defaultsLogDock_->setAccessibleName(tr("Defaults Log Panel"));
    defaultsLogDock_->setAccessibleDescription(tr("Log of fallback default values used due to errors"));
    if (attachmentPanelDock_) {
        attachmentPanelDock_->setAccessibleName(tr("Attachments Panel"));
        attachmentPanelDock_->setAccessibleDescription(
            tr("Embedded files attached to the document"));
    }

    // Status bar
    if (statusBar_) {
        statusBar_->setAccessibleName(tr("Status Bar"));
    }
}

void MainWindow::setupPanelShortcuts()
{
    // Alt+1: Focus PDF Canvas
    auto* focusCanvas = new QShortcut(QKeySequence(tr("Alt+1")), this);
    connect(focusCanvas, &QShortcut::activated, this, [this]() {
        canvas_->setFocus();
    });

    // Alt+2: Focus/Show Pages Panel
    auto* focusPages = new QShortcut(QKeySequence(tr("Alt+2")), this);
    connect(focusPages, &QShortcut::activated, this, [this]() {
        pagePanelDock_->show();
        pagePanelDock_->raise();
        pagePanel_->setFocus();
    });

    // Alt+3: Focus/Show Search Panel
    auto* focusSearch = new QShortcut(QKeySequence(tr("Alt+3")), this);
    connect(focusSearch, &QShortcut::activated, this, [this]() {
        searchPanelDock_->show();
        searchPanelDock_->raise();
        searchPanel_->setFocus();
    });

    // Alt+4: Focus/Show Properties Panel
    auto* focusProperties = new QShortcut(QKeySequence(tr("Alt+4")), this);
    connect(focusProperties, &QShortcut::activated, this, [this]() {
        propertiesPanelDock_->show();
        propertiesPanelDock_->raise();
        propertiesPanel_->setFocus();
    });
}

void MainWindow::openFile(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return;
    }

    const QString fileName = filePath;

    // Name to put in DIALOG BODIES. House style (see onSaveAsFile) is the
    // basename, not the whole path; the status bar and logs keep the full
    // path. A trailing-separator or otherwise nameless path falls back to
    // the full string so the user is never shown an empty pair of quotes.
    const QString displayName = [&]() -> QString {
        if (!pendingDisplayName_.isEmpty())
            return pendingDisplayName_;
        const QString base = QFileInfo(fileName).fileName();
        return base.isEmpty() ? fileName : base;
    }();
    // Consumed exactly once, whichever way this open ends, so a failed
    // File > New cannot leave "Untitled" attached to the next real open.
    pendingDisplayName_.clear();

    // Report an open failure consistently: modal dialog (basename in the
    // body, full path in the informative text where it helps) plus a
    // full-path status-bar line.
    auto reportOpenFailure = [this](const QString& title, const QString& text,
                                    const QString& informative,
                                    const QString& statusMessage) {
        QMessageBox box(QMessageBox::Critical, title, text, QMessageBox::Ok, this);
        box.setObjectName("openFileErrorDialog");
        box.setAccessibleName(title);
        if (!informative.isEmpty())
            box.setInformativeText(informative);
        // GitHub #91: an unattended test cannot read this box any other way.
        // UIA cannot enumerate a modal exec() on this platform, and a
        // QMessageBox's body is a QLabel with no HWND of its own, so a Win32
        // child walk cannot see it either — only the CAPTION is reachable from
        // outside, and asserting on a caption would not prove the reason was
        // stated. registerDialogIntrospection() parents the dialog-child query
        // pipes to the box, which is the one channel that survives modality;
        // the label Qt names "qt_msgbox_label" then answers a "text:" query.
        // Inert in production: the whole seam early-returns unless
        // PDFE_TEST_PIPES is set.
        pdfe::ui::testseam::registerDialogIntrospection(&box);
        box.exec();
        statusBar()->showMessage(statusMessage, 5000);
    };

    // ---- Refuse UNC / network locations BEFORE any filesystem probe. ----
    // GitHub #91 (WIN-60). Document::open() already refuses these, but the UI
    // never gets that far in a usable state:
    //
    //   * the very next statement is QFileInfo::exists(), and probing an
    //     UNREACHABLE UNC blocks the GUI thread for as long as the SMB
    //     redirector takes to give up (measured ~1 s for a bad host name, and
    //     unbounded for a host that answers but does not serve the share).
    //     Document.cpp:1017 documents the same hazard for canonicalization;
    //     exists() is the identical trap one layer up.
    //   * a REACHABLE UNC gets past exists() and is refused further down by
    //     the engine — but worded for a DOCUMENT ("malformed or crafted to
    //     exhaust resources"), which is both wrong and alarming when the only
    //     thing wrong with the file is where it lives.
    //
    // So the UI leg carries its own gate and NAMES the reason. The predicate
    // is the one the drag-drop filter already uses
    // (droppedUrlNamesACandidatePdf, ~line 3080), so a UNC typed into
    // File > Open and a UNC dropped from Explorer are refused identically.
    // GitHub #240 — a USER may open a network file; a DOCUMENT may not.
    //
    // The flat refusal above was the right call while there was nothing to
    // distinguish the two, and the reasoning that produced it still holds for
    // document-requested paths: that is the SMB / NetBIOS egress vector, and
    // `Document::open` still refuses it (OpenOrigin::DocumentRequested is the
    // default, so any caller that does not state otherwise keeps the refusal).
    //
    // openFile() is by definition the user's own request — File > Open, a drop
    // from Explorer, a recent-files entry, or a path on the command line. So it
    // no longer refuses outright; it checks the share responds first, off the
    // GUI thread, because the reason the old code refused BEFORE probing was
    // that probing an unreachable UNC blocks on the SMB redirector for as long
    // as it likes. That property is preserved: the wait here is bounded and
    // happens on a worker.
    const bool isNetworkPath =
        pdfe::core::path_security::is_unc_or_remote(fileName.toStdWString());
    if (isNetworkPath) {
        const NetworkOpenDecision decision = decideNetworkOpen(fileName);
        if (decision == NetworkOpenDecision::Unreachable) {
            reportOpenFailure(
                tr("Cannot Open Document"),
                tr("“%1” is on a network location that did not respond.")
                    .arg(displayName),
                tr("PDFE waited for the network location to answer and it did "
                   "not, so this file was not opened:\n%1\n\nIf the share is "
                   "slow rather than unavailable, try again.")
                    .arg(QDir::toNativeSeparators(fileName)),
                tr("Network location did not respond: %1").arg(fileName));
            return;
        }
        if (decision == NetworkOpenDecision::Refused) {
            reportOpenFailure(
                tr("Cannot Open Document"),
                tr("“%1” is on a network (UNC) path.").arg(displayName),
                tr("PDFE is configured to open local files only, so this UNC / "
                   "network location was not opened:\n%1")
                    .arg(QDir::toNativeSeparators(fileName)),
                tr("Refused UNC / network path: %1").arg(fileName));
            return;
        }
        // Reachable — fall through and open it like any other file.
    }

    // ---- Triage the path BEFORE the crash-isolation gate. ---------------
    // isDocumentOpenSafe() cannot distinguish a HOSTILE document from one
    // that simply is not there. The preflight child calls fz_open_document()
    // on whatever path it is handed (src/app/pdfe_preflight.cpp); a missing
    // file, a directory and a text file all make it exit non-zero, and the
    // parent reads exit-code != 0 as "unsafe" (src/core/PreflightOpen.cpp).
    // Every mistyped or bogus path was therefore accused of being "malformed
    // or crafted to exhaust resources". The user-reported instance was a
    // launcher handing the app the bare argument "pdfe" — nothing in this
    // repo does it, but src/main.cpp forwards positionalArguments().first()
    // to openFile() unvalidated, and QFileInfo("pdfe").fileName() is "pdfe",
    // so the dialog named the PROGRAM and called it a hostile document.
    //
    // This is a UX classifier, NOT a security boundary, and it is written so
    // it cannot become one: it only ever refuses EARLIER, never admits. Every
    // path that survives it still goes through the preflight gate below, so a
    // file that appears, is replaced, or is swapped for a directory between
    // the two checks is still vetted by the sandbox exactly as before.
    const QFileInfo pathInfo(fileName);
    if (!pathInfo.exists()) {
        reportOpenFailure(
            tr("Cannot Open Document"),
            tr("“%1” could not be found.").arg(displayName),
            tr("No file exists at this location:\n%1")
                .arg(QDir::toNativeSeparators(pathInfo.absoluteFilePath())),
            tr("File not found: %1").arg(fileName));
        return;
    }
    if (pathInfo.isDir()) {
        reportOpenFailure(
            tr("Cannot Open Document"),
            tr("“%1” is a folder, not a PDF document.").arg(displayName),
            tr("Choose a PDF file inside it instead:\n%1")
                .arg(QDir::toNativeSeparators(pathInfo.absoluteFilePath())),
            tr("Not a document (folder): %1").arg(fileName));
        return;
    }
    if (!pathInfo.isFile()) {
        reportOpenFailure(
            tr("Cannot Open Document"),
            tr("“%1” is not a regular file, so it cannot be opened as a "
               "PDF document.").arg(displayName),
            tr("Location:\n%1")
                .arg(QDir::toNativeSeparators(pathInfo.absoluteFilePath())),
            tr("Not a regular file: %1").arg(fileName));
        return;
    }

    try {
        // Close existing document
        closeDocument();

        // Out-of-process crash-isolation pre-flight. A hostile PDF can drive
        // MuPDF's parser/repair into unbounded (effectively cyclic) recursion
        // that exhausts the stack — an unrecoverable crash that cannot be
        // trapped in-process (fz_try/fz_catch is setjmp-based; a stack
        // overflow also corrupts the allocator). We first attempt the open in
        // a disposable child process (pdfe_preflight); if it cannot open the
        // document cleanly (crash / timeout / parser error), we refuse it here
        // instead of crashing the editor. Found by the fuzz_document_open
        // harness; see src/core/PreflightOpen and src/app/pdfe_preflight.cpp.
        // GitHub #240 — preflight a LOCAL COPY when the document is on a share.
        //
        // The preflight child is created with a LOW-INTEGRITY token
        // (src/core/PreflightOpen.cpp), and a low-integrity process cannot
        // authenticate to SMB. MEASURED on this host: the same PDF clears the
        // preflight locally and is refused through its UNC spelling
        // (tests/test_preflight_unc_reachability.cpp pins that, and reddens if
        // the isolation ever changes). So handing the child a UNC path would
        // refuse every network document — with the hostile-document wording,
        // which is precisely the misleading message GitHub #91 introduced the
        // UI-level refusal to avoid.
        //
        // Staging a local copy and vetting THAT keeps both controls: the child
        // stays low-integrity, and a hostile document still detonates in the
        // sandbox rather than in the editor. Raising the child's integrity for
        // network paths would have been simpler and would have weakened the
        // isolation for exactly the inputs least under our control.
        //
        // Residual, stated rather than hidden: the copy and the original are
        // vetted and opened separately, so a share that changes the file in
        // between is not covered. That window is the cost of not weakening the
        // sandbox, and it is far narrower than the status quo of refusing the
        // file outright.
        const QString preflightTarget =
            isNetworkPath ? stageLocalPreflightCopy(fileName) : fileName;
        if (isNetworkPath && preflightTarget.isEmpty()) {
            reportOpenFailure(
                tr("Cannot Open Document"),
                tr("“%1” could not be read from the network location.")
                    .arg(displayName),
                tr("PDFE copies a network document locally before checking it, "
                   "and that copy could not be made:\n%1")
                    .arg(QDir::toNativeSeparators(fileName)),
                tr("Could not stage network document for preflight: %1")
                    .arg(fileName));
            return;
        }
        if (!pdfe::core::isDocumentOpenSafe(qStringToPath(preflightTarget))) {
            // The refusal has ALREADY been decided by the sandbox above and
            // is not revisited here; all that follows is how it is WORDED.
            // A file with no PDF header at all (someone picked a .txt, or a
            // .pdf that is really something else) is an ordinary mistake,
            // and telling that user their document is "crafted to exhaust
            // resources" is both alarming and wrong. An unreadable file
            // keeps the strict wording — refusing to guess is the safe way
            // to be wrong here.
            bool hasPdfHeader = true;
            {
                QFile probe(fileName);
                if (probe.open(QIODevice::ReadOnly)) {
                    // ISO 32000-1 puts "%PDF-" at the start of the file;
                    // lenient readers (MuPDF's repair included) scan a
                    // leading window for it, so we look at the same 1 KiB.
                    hasPdfHeader = probe.read(1024).contains("%PDF-");
                }
            }
            if (!hasPdfHeader) {
                reportOpenFailure(
                    tr("Cannot Open Document"),
                    tr("“%1” is not a PDF document.").arg(displayName),
                    tr("The file does not begin with a PDF header, so there "
                       "is nothing here for PDFE to open."),
                    tr("Not a PDF document: %1").arg(fileName));
            } else {
                reportOpenFailure(
                    tr("Cannot Open Document"),
                    tr("“%1” could not be opened safely. It appears to be "
                       "malformed or crafted to exhaust resources, and has "
                       "been refused to protect the application.")
                        .arg(displayName),
                    QString(),
                    tr("Refused unsafe document: %1").arg(fileName));
            }
            return;
        }

        // Try opening without password first; if it needs one, prompt up to 3 times
        std::string password;
        bool opened = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            try {
                // GitHub #240 — UserInitiated. openFile() is reached only from
                // File > Open, a drop, a recent-files entry, or argv, so a
                // person named this path. The engine's default is
                // DocumentRequested, which still refuses network locations, and
                // every other Document::open call site in the tree keeps that
                // default untouched.
                document_ = pdfe::core::Document::open(
                    muPdfContext_, qStringToPath(fileName), password,
                    pdfe::core::Document::OpenOrigin::UserInitiated);
                // GitHub #61: a newly opened document gets a fresh /Launch
                // prompt budget.
                //
                // This is the ONLY site that resets it. The two other
                // `document_ = Document::open(...)` sites in this file are
                // re-opens of the SAME file immediately after signing it, and
                // they deliberately do NOT reset: otherwise a hostile document
                // that had exhausted its budget could regain one by inducing a
                // sign-and-reopen, which is a bypass rather than a new
                // document.
                resetPerDocumentLaunchPrompts();
                opened = true;
                break;
            } catch (const std::exception& ex) {
                std::string msg = ex.what();
                bool needsPwd = (msg.find("Password required") != std::string::npos ||
                                 msg.find("Invalid password") != std::string::npos);
                if (!needsPwd || attempt >= 3) {
                    throw;
                }
                PasswordDialog pwdDialog(this);
                pwdDialog.setWindowTitle(attempt == 0
                    ? tr("Password Required")
                    : tr("Invalid Password - Try Again"));
                if (pwdDialog.exec() != QDialog::Accepted) {
                    return; // User cancelled
                }
                password = pwdDialog.password().toStdString();
            }
        }

        if (!opened || !document_) {
            // Dialog body names the file the way every other dialog does —
            // by basename; the full path goes to the status bar.
            reportOpenFailure(tr("Error"),
                tr("Failed to open PDF file: %1").arg(displayName),
                QString(),
                tr("Failed to open PDF file: %1").arg(fileName));
            return;
        }

        // Create rendering infrastructure
        tileCache_ = std::make_unique<pdfe::rendering::TileCache>();
        tileRenderer_ = std::make_unique<pdfe::rendering::TileRenderer>(document_.get());

        // Set up canvas
        canvas_->setDocument(document_.get(), tileCache_.get(), tileRenderer_.get());

        // Create annotation manager
        annotationManager_ = std::make_unique<pdfe::annotations::AnnotationManager>(muPdfContext_);

        // GitHub #135: let the canvas follow a /Link even while the Select
        // tool owns the press. The canvasClicked signal below still covers
        // the no-tool case; this covers the case that is actually normal.
        canvas_->linkFollowRequested = [this](int pageIndex,
                                              QPointF pdfPoint) -> bool {
            return followLinkAt(pageIndex, pdfPoint);
        };

        // Wire annotation tooltip for hover preview
        canvas_->tooltipRequested = [this](int pageIndex, QPointF pdfPoint) -> QString {
            // Annotation tooltips
            if (document_ && annotationManager_) {
                auto page = document_->page(pageIndex);
                if (page) {
                    auto hitIndex = annotationManager_->getAnnotationAt(page.get(), pdfPoint);
                    if (hitIndex.has_value()) {
                        auto data = annotationManager_->getAnnotation(page.get(), hitIndex.value());
                        if (data.type == pdfe::annotations::AnnotationType::Text ||
                            data.type == pdfe::annotations::AnnotationType::FreeText) {
                            QString text = QString::fromStdString(data.contents);
                            if (text.length() > 100) {
                                text = text.left(100) + QStringLiteral("...");
                            }
                            return text;
                        }
                    }
                }
            }

            // OCR box tooltips (when overlay is active)
            if (ocrBoxesEnabled_ && canvas_) {
                const auto* lines = canvas_->ocrBoxDataForPage(pageIndex);
                if (lines) {
                    float ocrDpi = ocrPipeline_ ? static_cast<float>(ocrPipeline_->config().ocrDpi) : 300.0f;
                    float pdfToOcr = ocrDpi / 72.0f;
                    float ocrX = static_cast<float>(pdfPoint.x()) * pdfToOcr;
                    float ocrY = static_cast<float>(pdfPoint.y()) * pdfToOcr;

                    for (const auto& line : *lines) {
                        for (const auto& word : line.words) {
                            for (const auto& ch : word.chars) {
                                if (ch.codepoint == 0) continue;
                                if (ocrX >= ch.bbox.x0 && ocrX <= ch.bbox.x1 &&
                                    ocrY >= ch.bbox.y0 && ocrY <= ch.bbox.y1) {
                                    QString tip;
                                    tip += QString("Char: '%1' (U+%2)\n")
                                        .arg(QString::fromUcs4(&ch.codepoint, 1))
                                        .arg(static_cast<uint>(ch.codepoint), 4, 16, QChar('0'));
                                    tip += QString("Confidence: %1%\n").arg(ch.confidence);
                                    tip += QString("Bbox: %1,%2 → %3,%4\n")
                                        .arg(ch.bbox.x0).arg(ch.bbox.y0)
                                        .arg(ch.bbox.x1).arg(ch.bbox.y1);
                                    // Word-level info
                                    if (word.fontSizePx > 0)
                                        tip += QString("Font size: %1px\n").arg(word.fontSizePx, 0, 'f', 1);
                                    tip += QString("Word: \"%1\" (conf %2%)\n")
                                        .arg(QString::fromStdString(word.text)).arg(word.confidence);
                                    // Line-level metrics
                                    if (line.xSizePx > 0)
                                        tip += QString("x-height: %1px\n").arg(line.xSizePx, 0, 'f', 1);
                                    if (line.ascendersPx > 0)
                                        tip += QString("Ascenders: %1px\n").arg(line.ascendersPx, 0, 'f', 1);
                                    if (line.descendersPx > 0)
                                        tip += QString("Descenders: %1px\n").arg(line.descendersPx, 0, 'f', 1);
                                    return tip.trimmed();
                                }
                            }
                        }
                    }
                }
            }
            return {};
        };

        // Create form support
        formManager_ = std::make_unique<pdfe::forms::FormManager>(muPdfContext_);
        jsEngine_ = std::make_unique<pdfe::forms::JavaScriptEngine>(muPdfContext_);
        formDataIO_ = std::make_unique<pdfe::forms::FormDataIO>(muPdfContext_);

        // Create redaction support
        redactionManager_ = std::make_unique<pdfe::redaction::RedactionManager>(muPdfContext_);

        // Create encryption support
        encryptionManager_ = std::make_unique<pdfe::encryption::EncryptionManager>(muPdfContext_);

        // Create signature support
        signatureManager_ = std::make_unique<pdfe::signatures::SignatureManager>(muPdfContext_);

        // Image support
        imageManager_ = std::make_unique<pdfe::image::ImageManager>(muPdfContext_);
        imageContentEditor_ = std::make_unique<pdfe::image::ImageContentEditor>(muPdfContext_);
        exifStripper_ = std::make_unique<pdfe::image::ExifStripper>(muPdfContext_);

        // Metadata
        metadataManager_ = std::make_unique<pdfe::metadata::MetadataManager>(muPdfContext_);

        // Enable JavaScript for form calculations -- gated by
        // AppSettings::javaScriptEnabled() (default OFF). Phase-3
        // corpus runs measured concrete network-resolution leakage
        // from MuPDF's /AA, /OpenAction, and AcroForm /CO handlers
        // when JS was unconditionally enabled at document load. The
        // user-opt-in default closes that auto-fire path.
        // Settings -> Security -> "Allow JavaScript in PDF forms"
        // is the intended UX to flip this on for trusted documents.
        //
        // Pipeline b3f19d72 phase-12 test seam: PDFE_ENABLE_JS=1
        // forces JavaScript on for automated testing without touching
        // the persistent QSettings store. Mirrors PDFE_USE_QT_FILE_DIALOG.
        const bool jsEnabledBySetting =
            appSettings_ && appSettings_->javaScriptEnabled();
        const bool jsEnabledByEnv =
            qgetenv("PDFE_ENABLE_JS") == "1";
        if (document_->pageCount() > 0 && (jsEnabledBySetting || jsEnabledByEnv)) {
            auto firstPage = document_->page(0);
            jsEngine_->enable(firstPage.get());
        }

        // Pipeline E phase-9: wire document-level JS event callback so
        // Document::save/saveAs/destructor fire willSave/didSave/willClose
        // through JavaScriptEngine. Capture document_ raw pointer (not
        // this) — the callback fires in destructor where 'this' may
        // already be in a teardown path.
        if (jsEngine_ && document_->pageCount() > 0) {
            pdfe::core::Document* docPtr = document_.get();
            pdfe::forms::JavaScriptEngine* jsPtr = jsEngine_.get();
            document_->setDocumentEventCallback(
                [docPtr, jsPtr](const std::string& name) {
                    if (!docPtr || !jsPtr) return;
                    if (docPtr->pageCount() <= 0) return;
                    auto pg = docPtr->page(0);
                    if (!pg) return;
                    jsPtr->fireDocumentEvent(name, pg.get());
                }
            );
        }

        // Install the tool that the toolbar already SAYS is active.
        //
        // selectToolAction_ is setChecked(true) at construction, so the Select
        // tool looks active from the moment a document opens -- but the tool
        // OBJECT was only ever constructed inside onAnnotationToolSelected(),
        // which nothing called until the user either created an annotation or
        // switched to another tool and back. Until then canvas_->currentTool()
        // was null, so clicking an existing annotation did not select it and a
        // drag panned the view instead of rubber-band selecting.
        //
        // Found by a UI test asserting selection state instead of asserting
        // only that the process had not crashed; the checked state and the
        // real state had simply never been compared.
        if (selectToolAction_ && selectToolAction_->isChecked()) {
            onAnnotationToolSelected(selectToolAction_);
        }

        // Set up panels
        pagePanel_->setDocument(document_.get());
        searchPanel_->setDocument(document_.get());
        bookmarkPanel_->setDocument(document_.get());
        if (layerPanel_) layerPanel_->setDocument(document_.get());
        commentPanel_->setDocument(document_.get(), annotationManager_.get());
        if (attachmentPanel_) {
            attachmentPanel_->setDocument(muPdfContext_, document_.get());
            // Auto-show the panel if the document has any /EmbeddedFiles
            // entries; otherwise leave hidden so the side bar stays
            // uncluttered. The View menu toggle (created below) lets
            // the user reveal it manually for empty docs.
            if (attachmentPanelDock_) {
                auto* tree = attachmentPanel_->findChild<QTreeWidget*>(
                    "attachmentTree");
                if (tree && tree->topLevelItemCount() > 0) {
                    attachmentPanelDock_->show();
                }
            }
        }
        if (document_->pageCount() > 0) {
            auto pageInfo = document_->pageInfo(0);
            propertiesPanel_->setPageInfo(pageInfo);
        }

        // Show/hide Bookmarks tab based on whether the PDF has bookmarks
        {
            bool hasBookmarks = false;
            try {
                auto bookmarks = document_->extractBookmarks();
                hasBookmarks = !bookmarks.empty();
            } catch (...) {
                // Bookmark extraction failed — hide the tab
            }
            if (hasBookmarks) {
                bookmarkPanelDock_->show();
                toggleBookmarkPanelAction_->setChecked(true);
            } else {
                bookmarkPanelDock_->hide();
                toggleBookmarkPanelAction_->setChecked(false);
            }
        }

        // Pipeline G phase-6 / deferred Phase 8d: surface the
        // document-wide form-field count in the StatusBar's
        // formFieldCount label so screen readers + UIA pick up the
        // count without the user having to enumerate fields manually.
        // FormManager::getFormFields is per-page; sum across pages.
        if (statusBar_ && formManager_ && document_) {
            int totalFields = 0;
            try {
                for (int p = 0; p < document_->pageCount(); ++p) {
                    auto pg = document_->page(p);
                    if (!pg) continue;
                    auto fields = formManager_->getFormFields(pg.get());
                    totalFields += static_cast<int>(fields.size());
                }
            } catch (...) {
                totalFields = 0;
            }
            statusBar_->setFormFieldCount(totalFields);
        }

        // Select first page in PagePanel and trigger Fit Page
        if (document_->pageCount() > 0) {
            pagePanel_->setCurrentPage(0);
            // Use QTimer::singleShot so fitPage runs after layout is settled
            QTimer::singleShot(0, this, [this]() {
                canvas_->fitPage();
            });
        }

        // Clear the defaults log for the new document
        if (defaultsLogPanel_) {
            defaultsLogPanel_->clearLog();
        }

        // Update UI
        currentFileName_ = fileName;
        // File > New routes through openFile() on a temp file called
        // pdfe_untitled_<epoch_ms>.pdf. That machine-generated name is not
        // something to show a user, so onNewFile() supplies a display name
        // and it wins here, in the title bar and in the status bar until a
        // Save As gives the document a real one.
        documentDisplayName_ = displayName;
        updateWindowTitle();
        statusBar_->setFileName(displayName);
        statusBar_->setPageInfo(1, document_->pageCount());
        statusBar_->setZoom(100.0f);

        // Track in recent files
        if (recentFilesMenu_) {
            recentFilesMenu_->addFile(fileName);
        }

        // Remember last open directory
        if (appSettings_) {
            appSettings_->setLastOpenDirectory(QFileInfo(fileName).absolutePath());
        }

        // Enable actions
        saveAction_->setEnabled(true);
        saveAsAction_->setEnabled(true);
        if (exportPlainTextAction_) exportPlainTextAction_->setEnabled(true);
        closeAction_->setEnabled(true);
        copyAction_->setEnabled(true);
        cutAction_->setEnabled(true);
        pasteAction_->setEnabled(true);
        selectAllAction_->setEnabled(true);
        zoomInAction_->setEnabled(true);
        zoomOutAction_->setEnabled(true);
        fitWidthAction_->setEnabled(true);
        fitPageAction_->setEnabled(true);
        actualSizeAction_->setEnabled(true);
        zoomCombo_->setEnabled(true);
        printAction_->setEnabled(true);
        printPreviewAction_->setEnabled(true);
        removePasswordAction_->setEnabled(true);

        // Enable form actions
        createFormFieldAction_->setEnabled(true);
        formFieldPropertiesAction_->setEnabled(true);
        exportFormDataAction_->setEnabled(true);
        importFormDataAction_->setEnabled(true);
        resetFormAction_->setEnabled(true);
        flattenFormAction_->setEnabled(true);

        // Enable page manipulation actions
        rotatePageCWAction_->setEnabled(true);
        rotatePageCCWAction_->setEnabled(true);
        rotatePage180Action_->setEnabled(true);
        deletePageAction_->setEnabled(true);
        insertBlankPageAction_->setEnabled(true);
        insertPagesFromFileAction_->setEnabled(true);
        extractPagesAction_->setEnabled(true);
        movePageUpAction_->setEnabled(true);
        movePageDownAction_->setEnabled(true);

        // Enable encryption actions
        saveWithEncryptionAction_->setEnabled(true);
        documentPermissionsAction_->setEnabled(true);

        // Enable signature actions
        signDocumentAction_->setEnabled(true);
        verifySignaturesAction_->setEnabled(true);

        // Enable image actions
        imageToolAction_->setEnabled(true);
        insertImageAction_->setEnabled(true);
        stripExifAction_->setEnabled(true);
        documentMetadataAction_->setEnabled(true);
        complianceAction_->setEnabled(true);
        if (preflightAction_) preflightAction_->setEnabled(true);

        // Enable new Phase-6 actions
        replaceAction_->setEnabled(true);
        tabOrderAction_->setEnabled(true);
        if (calculationOrderAction_) calculationOrderAction_->setEnabled(true);
        if (altTextAction_) altTextAction_->setEnabled(true);
        submitFormAction_->setEnabled(true);
        ocrAction_->setEnabled(true);
        ocrAllPagesAction_->setEnabled(true);
        treAction_->setEnabled(true);
        accessibilityAction_->setEnabled(true);

        // List signatures and update panel
        if (signatureManager_) {
            auto sigs = signatureManager_->listSignatures(document_.get());
            signaturePanel_->setSignatures(sigs);
            if (!sigs.empty()) {
                signaturePanelDock_->show();
            }
        }

        // Test seam: PDFE_TEST_AUTO_VERIFY_SIGNATURES=1 triggers
        // verifyAllSignatures() immediately after document load so that
        // requestValidationAction is enabled for signed-PDF UI tests.
        // This env var is NEVER set in production; it is inert when unset.
        if (!qEnvironmentVariable("PDFE_TEST_AUTO_VERIFY_SIGNATURES").isEmpty()
            && signatureManager_ && document_)
        {
            try {
                auto summary = signatureManager_->verifyAllSignatures(document_.get());
                signaturePanel_->setSignatures(summary.signatures);
                if (!summary.signatures.empty()) {
                    signaturePanelDock_->show();
                }
                if (requestValidationAction_) {
                    // Respects PDFE_TEST_VALIDATION_REQUEST_UNLOCKED via the
                    // same logic in onVerifySignatures().
                    bool unlock = !qEnvironmentVariable(
                        "PDFE_TEST_VALIDATION_REQUEST_UNLOCKED").isEmpty();
                    int threshold = unlock ? summary.signedCount : summary.validCount;
                    requestValidationAction_->setEnabled(threshold > 0);
                }
            } catch (...) {
                // Silently ignore errors in auto-verify seam (test-only path).
            }
        }

        // R3-04: start periodic autosave / crash-recovery for this document.
        // Headless engine (Document + UndoManager only) so recovery is fully
        // testable without a UI. It writes a recovery sidecar to the per-user
        // recovery dir whenever the document is dirty; a clean save or close
        // clears it (see onSaveFile / onSaveFileAs / closeDocument).
        autosaveManager_ = std::make_unique<pdfe::core::AutosaveManager>(
            document_.get(), undoManager_, qStringToPath(fileName));
        // Bookmarks live in BookmarkManager until committed to the document
        // (they push undo commands but do not mutate the PDF until save). Flush
        // them into the live document just before each recovery snapshot so the
        // recovery copy carries them — mirrors the pre-save flush at
        // onSaveFile / onSaveFileAs (autosave-coverage Phase-2, Bookmarks gap).
        autosaveManager_->setPreSnapshotFlush([this]() {
            if (bookmarkPanel_) bookmarkPanel_->commitToDocument(document_.get());
        });
        // autosave-coverage Phase-5 (UI-E2E test seam): allow the periodic
        // autosave interval to be shortened via PDFE_AUTOSAVE_INTERVAL_MS so a
        // live-autosave UI test need not wait the full 30 s default. 100% inert
        // in production — the env var is set ONLY by the UI-test harness; when
        // absent, start()'s 30 s default applies unchanged. A sane floor guards
        // against a pathological busy-timer.
        int autosaveIntervalMs = 30000;
        if (qEnvironmentVariableIsSet("PDFE_AUTOSAVE_INTERVAL_MS")) {
            bool ok = false;
            const int v =
                qEnvironmentVariable("PDFE_AUTOSAVE_INTERVAL_MS").toInt(&ok);
            if (ok && v >= 250) {
                autosaveIntervalMs = v;
            }
        }
        autosaveManager_->start(autosaveIntervalMs);

        // GitHub #60 (SEC-62) — run ProhibitedFeatureScanner on the OPEN path
        // and surface the result in the advisory banner. This is the wiring
        // the issue is about: the scanner was only ever reachable from the
        // PDF/A compliance dialog, so a document carrying JavaScript or
        // external actions opened in complete silence even though PDFE had
        // already decided not to run it. Non-blocking by construction — the
        // banner is shown, the document opens normally.
        updateProhibitedFeatureBanner();

        // PDF-05 / GitHub #3 — REPORT a PDF 2.0 unencrypted wrapper.
        //
        // The detection is worthless without this half. A wrapper document is
        // an unencrypted PDF whose page content is a COVER SHEET; the real
        // document is an encrypted payload attachment. It opens cleanly, it
        // renders, and nothing else in this function would say a word about
        // it — so the user reads a page that is not the document and has no
        // way to know. That silence is the exact failure mode the construct
        // was invented to prevent, which is why issue #3 says "detected AND
        // reported".
        //
        // Advisory, not a refusal: the wrapper is a legitimate PDF 2.0 shape
        // and the cover sheet may well be worth reading. The user is told what
        // they are looking at and left in charge. The message text comes from
        // the probe (core::UnencryptedWrapperInfo::summary) rather than being
        // assembled here, so what the user is told is covered by the same
        // tests as the detection.
        {
            const auto wrapper = pdfe::core::probeUnencryptedWrapper(
                muPdfContext_, qStringToPath(fileName));
            if (wrapper.isWrapper) {
                const QString body = QString::fromStdString(wrapper.summary);
                QMessageBox box(QMessageBox::Warning,
                                tr("Encrypted Payload Not Shown"), body,
                                QMessageBox::Ok, this);
                box.setObjectName("unencryptedWrapperAdvisoryDialog");
                box.setAccessibleName(tr("Encrypted Payload Not Shown"));
                box.setInformativeText(
                    tr("“%1” opened, but what is displayed is a wrapper's "
                       "cover page, not its protected contents.")
                        .arg(displayName));
                // GitHub #179. The advisory had an objectName and an
                // accessibleName but was never registered with the dialog
                // seam, so the ONE thing no test could observe was whether
                // openFile actually raises it — which is precisely the half
                // #179 says is untested. UIA cannot enumerate a Qt app-modal
                // exec(); this registry can, and it also gives the harness
                // the auto-dismiss path so exec() returns.
                pdfe::ui::testseam::registerDialogIntrospection(&box);
                box.exec();
                statusBar()->showMessage(
                    tr("Unencrypted wrapper: displayed content is a cover "
                       "sheet, not the payload — %1").arg(fileName),
                    10000);
            }
        }

        // Show encryption status in status bar
        if (encryptionManager_ && encryptionManager_->isEncrypted(document_.get())) {
            statusBar_->setEncrypted(true);
            // Seed toggle states from document permissions
            auto perms = encryptionManager_->getPermissions(document_.get());
            if (!perms.copy && toggleCopyPreventionAction_) {
                toggleCopyPreventionAction_->setChecked(true);
            }
            if (!perms.print && togglePrintPreventionAction_) {
                togglePrintPreventionAction_->setChecked(true);
            }
        }

    } catch (const std::exception& e) {
        closeDocument();
        reportOpenFailure(tr("Error"),
            tr("Failed to open PDF file: %1").arg(displayName),
            QString::fromUtf8(e.what()),
            tr("Failed to open PDF file: %1").arg(fileName));
    }
}

void MainWindow::onNewFile()
{
    // Create a blank 1-page Letter-size PDF in the temp dir via the
    // Document::createBlank factory, then route through the existing
    // openFile path so the canvas/panel/status-bar wiring runs
    // unchanged.
    QString tmpPath = QDir::temp().absoluteFilePath(
        QString("pdfe_untitled_%1.pdf")
            .arg(QDateTime::currentMSecsSinceEpoch()));
    // The temp name is an implementation detail of routing New through
    // openFile(); the user must never see "pdfe_untitled_1754689123456.pdf"
    // in the title bar, the status bar or an error dialog.
    pendingDisplayName_ = tr("Untitled");

    try {
        auto blankDoc = pdfe::core::Document::createBlank(
            muPdfContext_, std::filesystem::path(tmpPath.toStdString()));
        // blankDoc is opened but not owned by MainWindow yet — drop
        // it so openFile() can open the path fresh (keeps the state
        // machine in one place).
        blankDoc.reset();
    } catch (const std::exception& e) {
        pendingDisplayName_.clear(); // openFile() never ran to consume it
        QMessageBox::critical(this, tr("New File"),
            tr("Failed to create blank PDF: %1").arg(e.what()));
        return;
    }

    openFile(tmpPath);
}

void MainWindow::onOpenFile()
{
    // SKIP=FAIL (GAP-A14): honour a test-injected path before the native open
    // modal, exactly as onSaveAsFile does for the save side. This is what lets
    // GitHub #91's dialog-leg coverage run on a machine with no reachable SMB
    // share: the native picker refuses a non-existent UNC itself and never
    // hands it to us, so without this seam the ONLY reachable form of the test
    // would be one that needs a live share. Inert in production — the queue is
    // fillable only through the PDFE_TEST_PIPES-gated pipes.
    QString fileName = pdfe::ui::testseam::takeFileDialogPath();
    if (fileName.isEmpty()) {
        QString lastDir = appSettings_ ? appSettings_->lastOpenDirectory()
                                       : QString();
        fileName = QFileDialog::getOpenFileName(
            this,
            tr("Open PDF File"),
            lastDir,
            tr("PDF Files (*.pdf);;All Files (*)")
        );
    }
    openFile(fileName);
}

void MainWindow::onSaveFile()
{
    if (!document_) {
        return;
    }

    if (!document_->isPdf()) {
        QMessageBox::warning(this, tr("Save"),
            tr("Only PDF documents can be saved."));
        return;
    }

    // F10 / WIN-69: ask Windows not to force-close us inside the
    // temp-write → atomic-rename window. Scoped to the whole save (RAII),
    // released on every exit path including the throwing one.
    ShutdownBlocker shutdownBlock(this, tr("PDFE is saving your document."));

    try {
        std::filesystem::path filePath = qStringToPath(currentFileName_);
        // Flush in-memory bookmark edits into /Outlines before save.
        if (bookmarkPanel_) bookmarkPanel_->commitToDocument(document_.get());

        // Drop the active editing tool's page holder before the full-rewrite
        // save. A live Page keeps the source fz_document handle open
        // (fz_keep_document), so on Windows the in-place remove()+rename() of
        // the original fails, save() throws after reopening the pre-edit file,
        // and the edit is silently lost. Tools are recreatable and toolPage_ is
        // reset post-save regardless. See tests/docs/journey-soak-phase3-findings.md.
        toolPage_.reset();

        const bool needFlush = alignmentPipeline_
            && alignmentPipeline_->pendingPageCount() > 0;
        if (needFlush) {
            // Rewriting the file in place requires closing all handles
            // first. Reuse the existing closeDocument/openFile pair —
            // it's the only path that cleans up every downstream raw
            // pointer (pagePanel_, canvas_, form/annotation managers).
            // closeDocument() clears the undo stack, so no rebind is needed
            // here — just drop the remaining Page holders so the rewrite is
            // not locked, then save.
            commandPages_.clear();
            document_->save(filePath);
            QString reopenPath = currentFileName_;
            // PIN BEFORE THE RELEASE. closeDocument() drops every handle on the
            // just-saved document; taking the pin after it leaves the file
            // unheld at its own predictable name across the gap.
            std::error_code alignSrcEc;
            std::FILE* alignSource =
                pdfe::core::openDenyWriteReadStream(filePath, alignSrcEc);
            closeDocument();
            // THE WINDOW, marked deterministically. Every handle on the
            // just-saved document has been released; the flush has not read it
            // yet. Null in production.
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, alignSource);
            openFile(reopenPath);
        } else {
            // Ordinary save: drain commandPages_ and rebind the undo commands
            // to freshly-resolved pages around the full-rewrite save, so the
            // save is not locked AND undo stays valid. Then refresh the stale
            // caches WITHOUT dropping the rebound pages.
            pdfe::core::saveWithCommandPageRebind(
                *document_, *undoManager_, commandPages_, filePath);
            onDocumentStructureChanged(/*preserveCommandPages=*/true);
        }

        undoManager_->setClean();
        // R3-04: the document is now clean on disk — discard any autosave
        // recovery sidecar so a clean save leaves nothing to recover.
        if (autosaveManager_) {
            autosaveManager_->clearRecovery();
        }
        updateWindowTitle();
        statusBar_->showMessage(tr("File saved"), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Save Error"),
            tr("Failed to save file: %1").arg(e.what()));
    }
}

void MainWindow::onSaveAsFile()
{
    if (!document_) {
        return;
    }

    if (!document_->isPdf()) {
        QMessageBox::warning(this, tr("Save As"),
            tr("Only PDF documents can be saved."));
        return;
    }

    // SKIP=FAIL (GAP-A14): honour a test-injected path before the native
    // save dialog. Inert in production (queue only fillable via
    // PDFE_TEST_PIPES).
    QString fileName = pdfe::ui::testseam::takeFileDialogPath();
    if (fileName.isEmpty()) {
        fileName = QFileDialog::getSaveFileName(
            this,
            tr("Save PDF File"),
            currentFileName_,
            tr("PDF Files (*.pdf);;All Files (*)")
        );
    }

    if (fileName.isEmpty()) {
        return;
    }

    // GitHub #240 — the save target obeys the same network policy as the open.
    //
    // Checked HERE, once, before the two save branches below (the flush path
    // calling document_->saveAs and the undo-preserving
    // saveAsWithCommandPageRebind path). Putting it in front of the branch is
    // the point: the issue reported Save As behaving inconsistently between its
    // two branches, and a policy applied inside each branch is a policy that can
    // disagree with itself the next time a third branch appears.
    //
    // There was NO guard on any save target before this — not here and not in
    // Document::save/saveAs — so "PDFE opens local files only" was an open-side
    // rule while Save As to a UNC quietly worked. That asymmetry is the defect;
    // the fix is symmetry, not a new prohibition. A user permitted to open from a
    // share is permitted to save to one, and a user who has turned that off is
    // refused in both directions.
    if (pdfe::core::path_security::is_unc_or_remote(fileName.toStdWString()) &&
        (!appSettings_ || !appSettings_->allowUserNetworkPaths())) {
        // A constructed QMessageBox, not the static QMessageBox::warning().
        //
        // The static one cannot be registered with the dialog-introspection
        // seam, so it is invisible to the UI suite AND unanswerable by the
        // auto-dismisser — a headless test that reached it HUNG rather than
        // failed (measured while writing test_save_as_network_policy.cpp). The
        // open-failure path in this file already builds its box for exactly this
        // reason; this now matches it.
        QMessageBox box(QMessageBox::Warning, tr("Cannot Save Document"),
                        tr("“%1” is on a network (UNC) path, and saving to "
                           "network locations is turned off.")
                            .arg(QDir::toNativeSeparators(fileName)),
                        QMessageBox::Ok, this);
        box.setObjectName(QStringLiteral("saveNetworkRefusedDialog"));
        box.setAccessibleName(tr("Cannot Save Document"));
        box.setInformativeText(
            tr("Enable “Allow opening and saving files on network locations” in "
               "Preferences → Security, or choose a local folder."));
        pdfe::ui::testseam::registerDialogIntrospection(&box);
        box.exec();
        if (statusBar_) {
            statusBar_->showMessage(
                tr("Save refused: network locations are turned off"), 5000);
        }
        return;
    }

    // F10 / WIN-69: same shutdown block as onSaveFile — Save As now also has a
    // temp-write → atomic-rename window whenever it OVERWRITES an existing file.
    ShutdownBlocker shutdownBlock(this, tr("PDFE is saving your document."));

    try {
        std::filesystem::path filePath = qStringToPath(fileName);
        // Flush in-memory bookmark edits into /Outlines before save.
        if (bookmarkPanel_) bookmarkPanel_->commitToDocument(document_.get());

        // Drop the active editing tool's page holder before the write (mirrors
        // onSaveFile). saveAs writes a NEW file so there is no in-place lock,
        // but it still drops+reopens the page tree, so held pages must be
        // re-resolved and the undo commands rebound or a post-Save-As undo
        // would dangle.
        toolPage_.reset();

        const bool needFlush = alignmentPipeline_
            && alignmentPipeline_->pendingPageCount() > 0;
        if (needFlush) {
            // closeDocument() clears the undo stack -> no rebind needed.
            commandPages_.clear();
            document_->saveAs(filePath);
            // PIN BEFORE THE RELEASE — see the other caller and the header.
            std::error_code alignSrcEc;
            std::FILE* alignSource =
                pdfe::core::openDenyWriteReadStream(filePath, alignSrcEc);
            closeDocument();
            // Same window as onSaveFile — see there. Null in production.
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, alignSource);
            openFile(fileName);
        } else {
            // Save As, preserving undo: drain + rebind around saveAs, then
            // refresh stale caches WITHOUT dropping the rebound pages.
            pdfe::core::saveAsWithCommandPageRebind(
                *document_, *undoManager_, commandPages_, filePath);
            onDocumentStructureChanged(/*preserveCommandPages=*/true);
        }

        // F24 — the certification guard allows a Save As that breaks a
        // certification (refusing it would strand the user's work) but it does
        // not let it pass silently. Surface what it recorded.
        if (document_ && !document_->lastSaveGuardWarning().empty()) {
            QMessageBox::warning(this, tr("Certification Not Satisfied"),
                QString::fromStdString(document_->lastSaveGuardWarning()));
        }

        currentFileName_ = fileName;
        // The document now has a real path of its own, so drop any
        // placeholder display name ("Untitled") a File > New put on it.
        documentDisplayName_.clear();
        undoManager_->setClean();
        // R3-04: Save As gave the document a (possibly new) path and a clean
        // state — point the autosave engine at the new path and discard any
        // stale recovery sidecar.
        if (autosaveManager_) {
            autosaveManager_->setSourcePath(qStringToPath(fileName));
            autosaveManager_->clearRecovery();
        }
        updateWindowTitle();
        statusBar_->setFileName(QFileInfo(fileName).fileName());
        statusBar_->showMessage(tr("File saved as %1").arg(QFileInfo(fileName).fileName()), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Save Error"),
            tr("Failed to save file: %1").arg(e.what()));
    }
}

void MainWindow::onCloseFile()
{
    closeDocument();
}

void MainWindow::onCreatePdfFromImages()
{
    auto out = pdfe::ui::ImageToPdfWizard::runInteractive(this);
    if (out.empty()) return;
    openFile(QString::fromStdString(out.string()));
}

void MainWindow::onPreviousView()
{
    if (!canvas_ || !navHistory_.canGoBack()) return;
    navHistory_.setReplaying(true);
    int page = navHistory_.goBack();
    if (page >= 0) canvas_->goToPage(page);
    navHistory_.setReplaying(false);
    if (previousViewAction_) previousViewAction_->setEnabled(navHistory_.canGoBack());
    if (nextViewAction_) nextViewAction_->setEnabled(navHistory_.canGoForward());
}

void MainWindow::onNextView()
{
    if (!canvas_ || !navHistory_.canGoForward()) return;
    navHistory_.setReplaying(true);
    int page = navHistory_.goForward();
    if (page >= 0) canvas_->goToPage(page);
    navHistory_.setReplaying(false);
    if (previousViewAction_) previousViewAction_->setEnabled(navHistory_.canGoBack());
    if (nextViewAction_) nextViewAction_->setEnabled(navHistory_.canGoForward());
}

void MainWindow::onExportPlainText()
{
    if (!document_) return;

    // R2-10: exporting the full document text is a content-extraction operation,
    // gated on the same copy/extract permission as Copy / Select All. Refuse
    // BEFORE the save-file dialog so a copy-denied document cannot be dumped to
    // a .txt file. The batch PlainTextExporter path is gated independently in
    // the exporter itself.
    if (documentDeniesCopy()) {
        if (statusBar_) {
            statusBar_->showMessage(
                tr("Text extraction is not permitted for this document"), 3000);
        }
        return;
    }

    QString suggested = QFileInfo(currentFileName_).completeBaseName();
    if (suggested.isEmpty()) suggested = "document";
    suggested += ".txt";
    QString dir = QFileInfo(currentFileName_).absolutePath();

    QString savePath = QFileDialog::getSaveFileName(
        this, tr("Export Plain Text"),
        dir.isEmpty() ? suggested : dir + "/" + suggested,
        tr("Text Files (*.txt);;All Files (*)"));
    if (savePath.isEmpty()) return;

    const bool ok = pdfe::exports::PlainTextExporter::write(
        document_.get(), savePath.toStdString());
    if (ok) {
        statusBar_->showMessage(
            tr("Exported text to %1").arg(QFileInfo(savePath).fileName()),
            3000);
    } else {
        QMessageBox::warning(this, tr("Export Plain Text"),
            tr("Could not write file:\n%1").arg(savePath));
    }
}

bool MainWindow::followLinkAt(int pageIndex, QPointF pdfPoint)
{
    if (!document_ || !annotationManager_) return false;
    if (pageIndex < 0 || pageIndex >= document_->pageCount()) return false;

    auto page = document_->page(pageIndex);
    if (!page) return false;
    auto annots = annotationManager_->getAnnotations(page.get());
    int linkIdx = -1;
    for (const auto& a : annots) {
        if (a.type != pdfe::annotations::AnnotationType::Link) continue;
        if (a.rect.contains(pdfPoint)) { linkIdx = a.annotIndex; break; }
    }
    if (linkIdx < 0) return false;

    pdfe::annotations::LinkAction act =
        annotationManager_->getLinkAction(page.get(), linkIdx);

    switch (act.type) {
    case pdfe::annotations::LinkActionType::Uri: {
        if (act.uri.empty()) return false;
        // Refuse UNC and file-share / kernel-resolver schemes (smb,
        // cifs, nfs, webdav, ftp, etc.) inside link URIs. Browser-safe
        // schemes (http, https, mailto, tel, ...) pass through to
        // QDesktopServices::openUrl so the user's registered handler
        // can sandbox them. See is_unsafe_link_target() docstring.
        if (pdfe::core::path_security::is_unsafe_link_target(
                std::string_view{act.uri})) {
            // GitHub #240 — the same-share opt-in, and this is the ONLY place it
            // applies.
            //
            // A document on \\finance\reports linking to
            // \\finance\reports\appendix.pdf is the case users asked for. The
            // setting is OFF by default, and even ON it is scoped to the
            // document's own share: a link to \\dc01\sysvol or \\attacker\share
            // stays refused, which is what makes this narrower than "allow
            // network access".
            //
            // Deliberately NOT extended to /Launch. That executes a program
            // rather than reading a resource, and "access other resources on the
            // same share" is not a licence to run one. /Launch keeps its
            // unconditional UNC refusal.
            //
            // The current document's own path is the referrer. When there is no
            // document, or it is local, same_network_share() answers false and
            // the refusal stands.
            const bool sameShareOptIn =
                appSettings_ && appSettings_->allowDocumentSameShareResources();
            // The document's own path, not currentFileName_ — that one is a
            // DISPLAY name (File > New sets it to "Untitled"), and comparing a
            // share against a display name would be a silent no-op.
            const std::wstring docPath =
                document_ ? document_->filePath().wstring() : std::wstring();
            if (sameShareOptIn && !docPath.empty() &&
                pdfe::core::path_security::same_network_share(
                    std::wstring_view{docPath},
                    QString::fromStdString(act.uri).toStdWString())) {
                // Permitted: fall through to the external-actions gate below,
                // which still applies. The opt-in widens WHICH targets are
                // eligible; it does not bypass the other controls.
            } else {
            QMessageBox::warning(
                this, tr("Refusing UNC / file-share link"),
                tr("PDFE refused to follow this link because it points "
                   "at a UNC path or a file-share scheme (smb, nfs, "
                   "webdav, ftp, ...):\n\n%1\n\n"
                   "Open the target manually in the appropriate program "
                   "if you trust the source.")
                    .arg(QString::fromStdString(act.uri)));
            return true;  // handled (refused)
            }
        }
        // Meta-audit F13 (SEC-49): the external-actions gate. Before this
        // phase `Security/externalActionsAllowed` was declared, persisted and
        // exposed as a Preferences → Security checkbox but read NOWHERE in
        // src/ — a live control gating nothing. It now gates /URI dispatch.
        // Default is OFF, so an attacker-controlled PDF cannot cause an
        // outbound request (or a handler-protocol invocation) on a click until
        // the user has explicitly opted in.
        const QString uriText = QString::fromStdString(act.uri);
        if (!externalActionsAllowed()) {
            sUriRefusedCount_.fetch_add(1, std::memory_order_acq_rel);
            if (statusBar_) {
                statusBar_->showMessage(
                    tr("Link blocked: external actions are disabled "
                       "(Preferences → Security)"), 5000);
            }
            return true;  // handled (refused)
        }
        sUriDispatchedCount_.fetch_add(1, std::memory_order_acq_rel);
        UriHandler uriSnapshot;
        {
            std::lock_guard<std::mutex> lock(sUriHandlerMutex_);
            uriSnapshot = sUriHandler_;
        }
        if (uriSnapshot) {
            uriSnapshot(uriText);   // test seam: no real shell dispatch
        } else {
            QDesktopServices::openUrl(QUrl(uriText));
        }
        return true;
    }
    case pdfe::annotations::LinkActionType::GoTo: {
        if (act.pageIndex < 0 || act.pageIndex >= document_->pageCount())
            return false;
        canvas_->goToPage(act.pageIndex);
        return true;
    }
    case pdfe::annotations::LinkActionType::Named: {
        const QString n = QString::fromStdString(act.namedAction);
        if (n == "NextPage" && canvas_) {
            canvas_->goToPage(canvas_->currentPage() + 1);
        } else if (n == "PrevPage" && canvas_) {
            canvas_->goToPage(canvas_->currentPage() - 1);
        } else if (n == "FirstPage" && canvas_) {
            canvas_->goToPage(0);
        } else if (n == "LastPage" && canvas_) {
            canvas_->goToPage(document_->pageCount() - 1);
        } else if (n == "Print") {
            onPrint();
        } else if (n == "ZoomIn" && canvas_) {
            canvas_->zoomIn();
        } else if (n == "ZoomOut" && canvas_) {
            canvas_->zoomOut();
        } else {
            return false;  // unknown named action
        }
        return true;
    }
    case pdfe::annotations::LinkActionType::Launch: {
        if (act.launchPath.empty()) return false;
        const QString path = QString::fromStdString(act.launchPath);
        // Refuse outright if the launch path is a UNC. The user-prompt
        // gate below isn't enough because QUrl::fromLocalFile on a UNC
        // already triggers the resolver inside Qt during URL parsing.
        if (pdfe::core::path_security::is_unc_or_remote(std::string_view{act.launchPath})) {
            QMessageBox::warning(
                this, tr("Refusing UNC launch"),
                tr("PDFE refused to follow this /Launch action because "
                   "it points at a UNC / network path:\n\n%1\n\n"
                   "If this is a file you genuinely trust, copy it to "
                   "local storage first and open it manually.").arg(path));
            return true;  // handled (refused)
        }
        // Security gate: PDFE never auto-executes /Launch — prompt the
        // user every time. The PDF might be malicious and point at an
        // executable on the local network or temp directory. This is
        // intentional security policy and matches Acrobat Pro's
        // "Trust Manager" default which prompts on every launch unless
        // the user has explicitly added the source to a trusted list
        // (PDFE has no such list — every launch always prompts).
        sLaunchPromptCount_.fetch_add(1, std::memory_order_acq_rel);

        // GitHub #61 (SEC-63): per-document prompt cap.
        //
        // NOTE the ordering. sLaunchPromptCount_ counts "a /Launch action
        // reached this gate" and is incremented ABOVE, before the cap, so it
        // keeps its existing meaning: the concurrency stress test asserts it
        // is >= the number of followLinkAt calls, and that contract is
        // unchanged. What the cap governs is whether the USER IS ASKED.
        //
        // Past the cap the action is refused without prompting and without
        // consulting the launch handler, so a capped call can neither prompt
        // nor execute.
        if (gLaunchPromptsThisDocument.fetch_add(1, std::memory_order_acq_rel)
                >= kMaxLaunchPromptsPerDocument) {
            // Tell the user ONCE per document why the prompts stopped. Silence
            // would be worse than the spam: a link that does nothing, with no
            // explanation, reads as a broken application rather than a refused
            // action. Suppressed entirely when the test seam is installed, so
            // no modal dialog can wedge a headless run.
            bool alreadyShown = gLaunchCapNoticeShown.exchange(
                true, std::memory_order_acq_rel);
            LaunchHandler capHandlerSnapshot;
            {
                std::lock_guard<std::mutex> lock(sLaunchHandlerMutex_);
                capHandlerSnapshot = sLaunchHandler_;
            }
            if (!alreadyShown && !capHandlerSnapshot) {
                QMessageBox::warning(
                    this, tr("Too many launch requests"),
                    tr("This document has asked to open external files %1 "
                       "times. PDFE will not ask again for this document.\n\n"
                       "A document that repeatedly asks to launch external "
                       "files is behaving the way a malicious one does. If you "
                       "trust it, close and reopen it.")
                        .arg(kMaxLaunchPromptsPerDocument));
            }
            return true;  // handled (refused)
        }

        // Pipeline F phase-1: copy the handler under the mutex so a
        // concurrent setLaunchHandlerForTesting() can't race us into
        // calling a moved-from std::function. Both branches below use
        // the local snapshot, not the static member.
        LaunchHandler handlerSnapshot;
        {
            std::lock_guard<std::mutex> lock(sLaunchHandlerMutex_);
            handlerSnapshot = sLaunchHandler_;
        }

        bool accepted = false;
        if (handlerSnapshot) {
            // Pipeline E phase-13 test seam: bypass real prompt + shell
            // dispatch. Tests use this to assert prompt-was-reached and
            // decline-blocks-launch without spawning modal dialogs.
            accepted = handlerSnapshot(path);
        } else {
            auto choice = QMessageBox::warning(
                this, tr("Launch external file?"),
                tr("This link wants to open an external file:\n\n%1\n\n"
                   "Launch actions can open executables. Only continue if "
                   "you trust the source of this PDF.").arg(path),
                QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
            accepted = (choice == QMessageBox::Open);
        }
        if (!accepted) return true;  // user cancelled; link was still "handled"
        sLaunchExecutedCount_.fetch_add(1, std::memory_order_acq_rel);
        if (!handlerSnapshot) {
            // Only call into the real shell when the test seam is NOT
            // installed — otherwise the test handler already simulated
            // the launch (or its absence).
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
        return true;
    }
    }
    return false;
}

void MainWindow::queueAlignmentForTest(int pageIndex, Qt::Alignment qtAlign,
                                       const QRectF& pdfBbox,
                                       const std::string& utf8Text,
                                       float fontSizePt)
{
    if (!alignmentPipeline_) return;
    pdfe::reflow::Alignment a = pdfe::reflow::Alignment::Left;
    if      (qtAlign.testFlag(Qt::AlignHCenter)) a = pdfe::reflow::Alignment::Center;
    else if (qtAlign.testFlag(Qt::AlignRight))   a = pdfe::reflow::Alignment::Right;
    else if (qtAlign.testFlag(Qt::AlignJustify)) a = pdfe::reflow::Alignment::Justify;
    auto params = pdfe::reflow::BlockParamsBuilder::build(
        utf8Text, pdfBbox, fontSizePt, a);
    alignmentPipeline_->queueAlignment(pageIndex, params);
}

// ─── Pipeline b3f19d72 phase-1 test-automation hook ──────────────────────

QString MainWindow::getCurrentAnnotationAltText()
{
    // Returns the /Alt value of the annotation currently tracked by
    // SelectTool. Mirrors the gating used by openAltTextDialogAction_
    // (selectedAnnotationPageIndex_ / selectedAnnotationIndex_ both
    // >= 0 mean exactly one annotation is selected).
    if (!document_ || !annotationManager_) return QString();
    if (selectedAnnotationPageIndex_ < 0 || selectedAnnotationIndex_ < 0)
        return QString();

    auto page = document_->page(selectedAnnotationPageIndex_);
    if (!page) return QString();

    try {
        std::string altText = annotationManager_->getAltText(
            page.get(), selectedAnnotationIndex_);
        return QString::fromStdString(altText);
    } catch (...) {
        // getAltText() throws on invalid page or annot-index out of
        // range; treat that the same as "no alt text available".
        return QString();
    }
}

void MainWindow::setSelectedAnnotationForTesting(int pageIndex, int annotIndex)
{
    selectedAnnotationPageIndex_ = pageIndex;
    selectedAnnotationIndex_ = annotIndex;
    if (openAltTextDialogAction_) {
        openAltTextDialogAction_->setEnabled(
            pageIndex >= 0 && annotIndex >= 0);
    }
}

// GitHub #253 / p97 phase-9. Read-back seams for the choice-routing test; see
// the header for why they are this narrow. Both go through the LIVE
// formManager_ and the LIVE current page, because the point is to observe what
// onFormFieldValueChanged actually committed.
int MainWindow::firstListBoxForTesting(int minOptions) const
{
    if (!document_ || !formManager_ || !canvas_) return -1;
    auto page = document_->page(canvas_->currentPage());
    if (!page) return -1;
    for (const auto& f : formManager_->getFormFields(page.get())) {
        if (f.type == pdfe::forms::FormFieldType::ListBox &&
            static_cast<int>(f.options.size()) >= minOptions) {
            return f.widgetIndex;
        }
    }
    return -1;
}

std::vector<int> MainWindow::committedSelectionForTesting(int widgetIndex) const
{
    if (!document_ || !formManager_ || !canvas_) return {};
    auto page = document_->page(canvas_->currentPage());
    if (!page) return {};
    return formManager_->getFormField(page.get(), widgetIndex).selectedIndices;
}

bool MainWindow::setAnnotationAltTextForTesting(int pageIndex, int annotIndex,
                                                const QString& altText)
{
    if (!document_ || !annotationManager_) return false;
    if (pageIndex < 0 || pageIndex >= document_->pageCount()) return false;
    if (annotIndex < 0) return false;

    auto page = document_->page(pageIndex);
    if (!page) return false;

    try {
        annotationManager_->setAltText(
            page.get(), annotIndex, altText.toStdString());
        return true;
    } catch (...) {
        return false;
    }
}

void MainWindow::queueAlignmentForActiveSpan(Qt::Alignment qtAlign)
{
    if (!alignmentPipeline_ || !canvas_) return;
    auto* tool = canvas_->textEditTool();
    if (!tool || !tool->isEditing()) return;
    const pdfe::textedit::TextSpan* span = tool->activeSpan();
    if (!span || span->boundingBox.isEmpty()) return;

    pdfe::reflow::Alignment a = pdfe::reflow::Alignment::Left;
    if      (qtAlign.testFlag(Qt::AlignHCenter)) a = pdfe::reflow::Alignment::Center;
    else if (qtAlign.testFlag(Qt::AlignRight))   a = pdfe::reflow::Alignment::Right;
    else if (qtAlign.testFlag(Qt::AlignJustify)) a = pdfe::reflow::Alignment::Justify;

    // Build UTF-8 text from the span's UTF-32 content.
    std::string utf8;
    utf8.reserve(span->text.size() * 2);
    for (char32_t c : span->text) {
        if (c < 0x80) {
            utf8.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (c >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (c >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (c >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }

    auto params = pdfe::reflow::BlockParamsBuilder::build(
        utf8, span->boundingBox, span->fontSize, a);
    alignmentPipeline_->queueAlignment(tool->currentPageIndex(), params);
}

bool MainWindow::flushAlignmentPipelineIfPending(
    const std::filesystem::path& savedPath, std::FILE* preTakenSource)
{
    // Ownership of the caller's pin passes here on EVERY path, including the
    // early returns below.
    struct SourceGuard {
        std::FILE* fp;
        ~SourceGuard() { if (fp) std::fclose(fp); }
        std::FILE* release() { auto* p = fp; fp = nullptr; return p; }
    } preTaken{preTakenSource};

    if (!alignmentPipeline_ || alignmentPipeline_->pendingPageCount() == 0) {
        return true;
    }
    // Precondition: caller has already closed the document so Windows
    // will allow us to rewrite `savedPath` in place. onSaveFile/onSaveAsFile
    // satisfy this via closeDocument() before invoking; after this
    // returns they call openFile() to restore state.
    //
    // WIN-87 site #5, phase-57 — THE WIDEST WINDOW OF THE SEVEN, AND THERE IS
    // NO LONGER A ".align.tmp".
    //
    // The precondition in the paragraph above is exactly what made this site
    // worse than the redaction pair: the product DELIBERATELY releases every
    // handle on the user's just-saved document across this call, so both the
    // file being read and the staging file being written sat unheld at fully
    // predictable names for the whole flush. Poisoning ".align.tmp" published
    // attacker bytes over a document the user believed they had just written.
    //
    // F10 / WIN-85 already hardened this site once — the comment that used to
    // sit below recorded it — and that fix was CORRECT ABOUT A DIFFERENT
    // HAZARD. It replaced remove(savedPath) + rename(tmp, savedPath), which
    // destroyed the user's document if the rename then failed, with the shared
    // atomic helper. Data loss: closed, and still closed. Substitution: left
    // wide open at all seven sites at once, because the shared helper it routed
    // to takes PATHS. "Already hardened" was the reason nobody looked again.
    //
    // WHAT IT IS NOW. The name is resolved exactly twice, both times up front,
    // and never again:
    //   * the INPUT through a deny-write read handle taken BY THE CALLER BEFORE
    //     IT RELEASED THE DOCUMENT. That last clause is the whole property and
    //     it used to be false: the pin was taken here, i.e. AFTER the caller's
    //     closeDocument(), so the file was unheld under its own predictable
    //     name across the gap. This comment claimed "nothing can substitute the
    //     document being aligned" while that gap was open — a correct sentence
    //     about this function standing in for a claim about the sequence it
    //     belongs to. Caught by the phase-4 karen gate;
    //   * the OUTPUT into a StagingFile behind an exclusive handle at an
    //     unpredictable name, published by renaming THROUGH that handle.
    // The read handle is closed before the publish — a handle held by this
    // process blocks a replace-existing rename (DCA 0021's closing note) —
    // which is why the two are scoped separately below.
    try {
        // USE THE CALLER'S PIN. Re-opening by name here would be a SECOND
        // resolution of the name, taken after closeDocument() has already
        // released the document — which is precisely the window this site was
        // named the worst of the seven for. If the caller could not take it,
        // refuse rather than silently opening the name ourselves: a fallback
        // that reopens by name is the defect wearing a safety net.
        std::FILE* sourceStream = preTaken.release();
        if (!sourceStream) {
            return false;
        }
        std::error_code stageEc;
        auto staged = pdfe::core::StagingFile::createBeside(savedPath, stageEc);
        if (!staged) {
            std::fclose(sourceStream);
            return false;
        }
        std::FILE* stagedStream = staged->openDuplicateStream(stageEc);
        if (!stagedStream) {
            std::fclose(sourceStream);
            return false;
        }

        // Both streams are consumed (and closed) by the flush, whatever it
        // does — including on its throwing path.
        alignmentPipeline_->flushThroughHandles(sourceStream, savedPath,
                                                stagedStream, staged->path());

        {
            std::error_code flushEc;
            staged->flushToStableStorage(flushEc);  // best-effort
        }

        // THE PUBLISH WINDOW, marked deterministically. The staging artifact is
        // fully written and has not yet been renamed over the user's document —
        // the exact instant an attacker must hit to have PDFE publish their
        // bytes. In this build the StagingFile still holds it exclusively, so a
        // substitution attempted here fails; in the pre-phase-57 build it is an
        // ordinary file at ".align.tmp" and the substitution lands. Null in
        // production.
        fireAlignStagingWindowHookForTesting(staged->path());

        std::error_code ec;
        if (!staged->commitOverWithRetry(savedPath, ec)) {
            // Nothing was ever removed first, so savedPath is untouched; the
            // StagingFile destructor removes the unpublished staging file.
            return false;
        }
        alignmentPipeline_->clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void MainWindow::onExit()
{
    close();
}

void MainWindow::onUndo()
{
    if (undoManager_) {
        // Cluster #5 fix: only run onDocumentStructureChanged() when the
        // page count actually changed. That helper clears commandPages_
        // (the vector that keeps Page wrappers alive for stack-resident
        // commands), which is fatal for annotation/text-edit commands
        // whose page_ pointers must remain valid across multiple undo
        // steps. Without this guard the test flow "create annotation +
        // type N chars + undo Modify + undo Create" hard-crashes inside
        // CreateAnnotationCommand::undo() because the Page wrapper it
        // points to was destroyed by the structural refresh fired by
        // the preceding Modify undo. The structural-refresh code path
        // is still needed for actual page operations (insert/delete/
        // move/import) which DO invalidate page wrappers, hence the
        // pageCount-delta guard.
        const int pageCountBefore = document_ ? document_->pageCount() : 0;
        undoManager_->undo();
        const int pageCountAfter = document_ ? document_->pageCount() : 0;

        // Restore the original XObject for scanned page text edits.
        // EditTextCommand only restores the content stream; the XObject
        // modification from refreshImageForScannedPage() must be reverted here
        // using the saved pdf_obj* reference (lossless, no JPEG recompression).
        if (imageEditPos_ >= 0 &&
            imageEditStack_[imageEditPos_].undoDepth > undoManager_->undoStackSize()) {
            auto& entry = imageEditStack_[imageEditPos_];
            if (entry.savedXObjectRef && pdfContentWriter_ && document_) {
                auto page = document_->page(entry.pageIndex);
                if (page) {
                    pdfContentWriter_->restoreImageXObjectRef(
                        page.get(), document_.get(), entry.savedXObjectRef, entry.xobjectName);
                    document_->markPageDirty(entry.pageIndex);
                    // Invalidate ImageReplacer cache so next edit re-reads the restored XObject
                    if (imageReplacer_)
                        imageReplacer_->invalidateOriginalCache(entry.pageIndex);
                }
            }
            imageEditPos_--;
        }

        if (canvas_) canvas_->refreshAllPages();
        // Refresh the PagePanel dock thumbnails and other structure-
        // derived UI. Without this, undo of Insert/Delete/Move page
        // commands leaves the PagePanel showing a stale page list
        // even though the document model is correctly reverted.
        // Discovered in phase-7 (docs/undo-panel-refresh-bug.md).
        if (pageCountBefore != pageCountAfter) {
            onDocumentStructureChanged();
        } else if (commentPanel_) {
            // Non-structural undo (annotations, text edits): keep
            // commandPages_ intact, but still refresh the comment
            // panel so undo of CreateAnnotation / ModifyAnnotation is
            // reflected in the tree.
            commentPanel_->refresh();
        }
        // Pipeline b3d7f942 phase-3 (GAP-7): refresh the redaction panel
        // after undo so CreateRedactionCommand::undo() (which calls
        // manager_->deleteRedactionMark()) is reflected in the panel UI.
        // Without this, the panel retains stale mark rows after undo.
        if (redactionPanel_ && redactionManager_ && document_) {
            auto marks = redactionManager_->getAllRedactionMarks(document_.get());
            redactionPanel_->setMarks(marks);
            if (applyRedactionsAction_)
                applyRedactionsAction_->setEnabled(!marks.empty());
        }
        updateWindowTitle();
    }
}

void MainWindow::onRedo()
{
    if (undoManager_) {
        // Cluster #5 fix: see onUndo above for the rationale. Same
        // page-count guard around onDocumentStructureChanged() to
        // avoid invalidating commandPages_ on non-structural redos.
        const int pageCountBefore = document_ ? document_->pageCount() : 0;
        undoManager_->redo();
        const int pageCountAfter = document_ ? document_->pageCount() : 0;

        // Re-apply the image edit for scanned page text edits.
        // EditTextCommand::redo() only restores the content stream text;
        // re-run refreshImageForScannedPage() to regenerate the XObject
        // at proper resolution (no cached low-res images involved).
        int nextPos = imageEditPos_ + 1;
        if (nextPos < static_cast<int>(imageEditStack_.size()) &&
            imageEditStack_[nextPos].undoDepth <= undoManager_->undoStackSize()) {
            auto& entry = imageEditStack_[nextPos];
            if (!entry.editedText.empty() && document_) {
                refreshImageForScannedPage(entry.pageIndex, entry.editedText,
                                            entry.spanBbox, entry.formattingRuns,
                                            entry.fontName, entry.fontSize);
                document_->markPageDirty(entry.pageIndex);
            }
            imageEditPos_ = nextPos;
        }

        if (canvas_) canvas_->refreshAllPages();
        // Same panel-refresh gap as onUndo, see above.
        if (pageCountBefore != pageCountAfter) {
            onDocumentStructureChanged();
        } else if (commentPanel_) {
            commentPanel_->refresh();
        }
        // Pipeline b3d7f942 phase-3 (GAP-7): refresh the redaction panel
        // after redo so CreateRedactionCommand::redo() (restoreRedactionMark)
        // is reflected in the panel UI.
        if (redactionPanel_ && redactionManager_ && document_) {
            auto marks = redactionManager_->getAllRedactionMarks(document_.get());
            redactionPanel_->setMarks(marks);
            if (applyRedactionsAction_)
                applyRedactionsAction_->setEnabled(!marks.empty());
        }
        updateWindowTitle();
    }
}

void MainWindow::onFind()
{
    if (searchPanelDock_) {
        searchPanelDock_->show();
        searchPanelDock_->raise();
    }
    if (searchPanel_) {
        searchPanel_->focusSearchBox();
    }
}

void MainWindow::onReplace(const QString& find, const QString& replace)
{
    if (!document_ || find.isEmpty()) return;

    std::u32string findU32 = find.toStdU32String();
    std::u32string replaceU32 = replace.toStdU32String();

    const auto* result = searchPanel_->currentSearchResult();
    int idx = searchPanel_->currentResultIndex();
    const bool haveSelectedHit =
        (result && idx >= 0 && idx < result->totalMatches());

    // Pipeline b3f19d72 phase-3 (P7-BUG-3): if no search has been run yet
    // (or no result is selected), fall back to a first-match scan across
    // all pages.  This mirrors the user-mental-model where typing a find
    // term and clicking Replace finds-and-replaces the first occurrence,
    // even if Search wasn't explicitly clicked first.  When a hit IS
    // selected, the previous behaviour (quad-overlap match against that
    // specific hit) is preserved so users who navigate results manually
    // get the expected per-occurrence semantics.
    bool replaced = false;
    int replacedPageIndex = -1;
    if (haveSelectedHit) {
        const auto& hit = result->hits[idx];
        auto page = document_->page(hit.pageIndex);
        if (page) {
            auto blocks = page->extractText();
            pdfe::textedit::TextEditModel model(blocks);
            for (int s = 0; s < model.spanCount(); ++s) {
                const auto& span = model.span(s);
                auto pos = span.text.find(findU32);
                if (pos == std::u32string::npos) continue;

                QRectF spanBox = span.boundingBox;
                bool overlaps = false;
                for (const auto& quad : hit.quads) {
                    if (spanBox.intersects(quad)) { overlaps = true; break; }
                }
                if (!overlaps) continue;

                std::u32string newText = span.text;
                newText.replace(pos, findU32.size(), replaceU32);

                auto cmd = std::make_unique<pdfe::textedit::EditTextCommand>(
                    document_.get(), muPdfContext_, hit.pageIndex, s,
                    span, span.text, newText);
                undoManager_->execute(std::move(cmd));
                replaced = true;
                replacedPageIndex = hit.pageIndex;
                break;
            }
        }
    } else {
        // No selected hit: scan all pages for the first span containing
        // the find string and replace it.
        for (int pageIdx = 0; pageIdx < document_->pageCount() && !replaced;
             ++pageIdx)
        {
            auto page = document_->page(pageIdx);
            if (!page) continue;
            auto blocks = page->extractText();
            pdfe::textedit::TextEditModel model(blocks);
            for (int s = 0; s < model.spanCount(); ++s) {
                const auto& span = model.span(s);
                auto pos = span.text.find(findU32);
                if (pos == std::u32string::npos) continue;

                std::u32string newText = span.text;
                newText.replace(pos, findU32.size(), replaceU32);

                auto cmd = std::make_unique<pdfe::textedit::EditTextCommand>(
                    document_.get(), muPdfContext_, pageIdx, s,
                    span, span.text, newText);
                undoManager_->execute(std::move(cmd));
                replaced = true;
                replacedPageIndex = pageIdx;
                break;
            }
        }
    }

    if (replaced) {
        searchPanel_->setReplaceStatus(tr("Replaced 1 match"));
        if (canvas_) canvas_->refreshAllPages();
        updateWindowTitle();
        // Advance to next match when navigating a search result list.
        if (haveSelectedHit) {
            int nextIdx = idx + 1;
            if (result && nextIdx < result->totalMatches()) {
                searchPanel_->selectResult(nextIdx);
            }
        }
        (void)replacedPageIndex;  // available for future per-page refresh
    } else {
        searchPanel_->setReplaceStatus(tr("Match not found"));
    }
}

void MainWindow::onReplaceAll(const QString& find, const QString& replace)
{
    if (!document_ || find.isEmpty()) return;

    std::u32string findU32 = find.toStdU32String();
    std::u32string replaceU32 = replace.toStdU32String();
    int totalReplaced = 0;

    auto composite = std::make_unique<pdfe::commands::CompositeCommand>("Replace All");

    for (int pageIdx = 0; pageIdx < document_->pageCount(); ++pageIdx) {
        auto page = document_->page(pageIdx);
        if (!page) continue;

        auto blocks = page->extractText();
        pdfe::textedit::TextEditModel model(blocks);

        for (int s = 0; s < model.spanCount(); ++s) {
            const auto& span = model.span(s);
            std::u32string newText = span.text;
            size_t searchFrom = 0;
            bool modified = false;

            while (true) {
                auto pos = newText.find(findU32, searchFrom);
                if (pos == std::u32string::npos) break;
                newText.replace(pos, findU32.size(), replaceU32);
                searchFrom = pos + replaceU32.size();
                totalReplaced++;
                modified = true;
            }

            if (modified) {
                composite->addCommand(std::make_unique<pdfe::textedit::EditTextCommand>(
                    document_.get(), muPdfContext_, pageIdx, s,
                    span, span.text, newText));
            }
        }
    }

    if (!composite->empty()) {
        undoManager_->execute(std::move(composite));
    }

    if (canvas_) canvas_->refreshAllPages();
    updateWindowTitle();

    // bugfix-3e91a7c4 #4/#5 test seam. Post the geometry fix, every matched span's
    // edit actually lands in /Contents, so this model-match count equals the number
    // of visibly-updated occurrences (guarded by test_smalltext_edit_render.cpp).
    lastReplaceAllCountForTesting_ = totalReplaced;

    if (totalReplaced == 0) {
        searchPanel_->setReplaceStatus(tr("No matches found"));
    } else {
        searchPanel_->setReplaceStatus(
            tr("Replaced %1 match(es)").arg(totalReplaced));
    }
}

void MainWindow::onPrint()
{
    if (!document_) {
        return;
    }

    // R2-09: enforce the live print permission at the operation boundary, BEFORE
    // the dialog / dry-run seam. onPrint is reached from the menu, the toolbar,
    // AND the Named /Print link action (followLinkAt) — the link path bypasses
    // the disable-able printAction_, so gating only the QAction is not enough.
    // Refusing here closes every entry point.
    if (documentDeniesPrint()) {
        if (statusBar_) {
            statusBar_->showMessage(
                tr("Printing is not permitted for this document"), 3000);
        }
        return;
    }

    // Pipeline b3f19d72 phase-8: test-only dry-run seam.
    // When PDFE_TEST_DRYRUN_PRINT=1 the PrintDialog still opens so
    // keyboard-navigation tests (P13-INFRA-4) can Tab/Escape through it,
    // but the native QPrinter + PrintEngine path is never reached.
    // consumeLastPrintDryRunFired() is set here (before the dialog) so
    // even a rejected dialog (Escape) lets the shortcut test confirm the
    // slot was entered.
    const bool dryRun = (qgetenv("PDFE_TEST_DRYRUN_PRINT") == "1");
    if (dryRun) {
        lastPrintDryRunFired_ = true;
        qInfo() << "PDFE_TEST_DRYRUN_PRINT: onPrint invoked, dialog will"
                   " open but native print is suppressed";
    }

    PrintDialog dialog(document_.get(), canvas_->currentPage(), this);
    pdfe::ui::testseam::registerDialogIntrospection(&dialog);
    if (dialog.exec() == QDialog::Accepted) {
        if (dryRun) {
            // Native print suppressed — fire JS events for completeness
            // but skip QPrinter / PrintEngine entirely.
            if (jsEngine_ && document_->pageCount() > 0) {
                auto firstPage = document_->page(0);
                if (firstPage) {
                    jsEngine_->fireDocumentEvent("willPrint", firstPage.get());
                    jsEngine_->fireDocumentEvent("didPrint", firstPage.get());
                }
            }
            return;
        }

        pdfe::core::PrintOptions options = dialog.options();

        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::NativeFormat);

        // Pipeline E phase-9: fire document-level willPrint before printing.
        if (jsEngine_ && document_->pageCount() > 0) {
            auto firstPage = document_->page(0);
            if (firstPage) {
                jsEngine_->fireDocumentEvent("willPrint", firstPage.get());
            }
        }

        pdfe::core::PrintEngine engine(document_.get());
        const bool ok = engine.print(&printer, options);
        if (!ok) {
            // GitHub #78 / WIN-28: show the engine's specific, actionable
            // sentence (e.g. "no print devices are installed ... check that the
            // Windows Print Spooler service is running") rather than the old
            // blanket "Failed to print the document.", which told a user with a
            // stopped spooler nothing they could act on. The generic line
            // remains only as a fallback for a failure the engine did not
            // classify.
            QString detail = engine.lastErrorMessage();
            if (detail.isEmpty()) {
                detail = tr("Failed to print the document.");
            }
            QMessageBox::warning(this, tr("Print Error"), detail);
        }

        if (jsEngine_ && document_->pageCount() > 0) {
            auto firstPage = document_->page(0);
            if (firstPage) {
                jsEngine_->fireDocumentEvent("didPrint", firstPage.get());
            }
        }
    }
}

bool MainWindow::consumeLastPrintDryRunFired()
{
    const bool fired = lastPrintDryRunFired_;
    lastPrintDryRunFired_ = false;
    return fired;
}

bool MainWindow::isPrintEnabled() const
{
    return printAction_ && printAction_->isEnabled();
}

bool MainWindow::deleteSelectedAnnotation()
{
    if (!document_ || !annotationManager_ || !undoManager_) return false;
    int pageIdx = canvas_ ? canvas_->currentPage() : 0;
    if (selectedAnnotationIndex_ < 0) return false;
    onCommentPanelDelete(pageIdx, selectedAnnotationIndex_);
    return true;
}

QString MainWindow::commentPanelRowCount(const QString& query) const
{
    if (!commentPanel_) {
        return QStringLiteral("0");
    }
    const std::vector<QString> rows = commentPanel_->rowInventoryForTesting();
    int count = 0;
    if (query.isEmpty()) {
        count = static_cast<int>(rows.size());
    } else {
        for (const QString& r : rows) {
            if (r.contains(query)) {
                ++count;
            }
        }
    }
    return QString::number(count);
}

QString MainWindow::textEditSelectionDiag() const
{
    pdfe::textedit::TextEditTool* tool =
        canvas_ ? canvas_->textEditTool() : nullptr;
    if (!tool) {
        return QStringLiteral("0,-1,-1,-1");
    }
    QString out = QStringLiteral("%1,%2,%3,%4")
        .arg(tool->spanCountForTesting())
        .arg(tool->activeSpanIndexForTesting())
        .arg(tool->selectionStartForTesting())
        .arg(tool->selectionEndForTesting());
    // Append span bboxes (PDF coords) as ";i:x,y,w,h" for diagnostics.
    const int n = tool->spanCountForTesting();
    for (int i = 0; i < n && i < 16; ++i) {
        const QRectF b = tool->spanBboxForTesting(i);
        out += QStringLiteral(";%1:%2,%3,%4,%5")
            .arg(i)
            .arg(b.x(), 0, 'f', 1).arg(b.y(), 0, 'f', 1)
            .arg(b.width(), 0, 'f', 1).arg(b.height(), 0, 'f', 1);
    }
    // Active overlay font-size override (pipeline c4a9e271 Phase 2): the per-line
    // size the clicked line resolved to via FontMatcher::clickedLineSizePt(), as
    // it reached TextEditTool::setOverlayFontSize().  Emitted as ";ovsz=<pt>"
    // among the ';'-delimited fields (after the span bboxes, before the final
    // '|text') so span_selected()'s head parse and the text-last contract both
    // stay intact.  Lets the native-UI per-line-size test read back the exact
    // overlay size deterministically — a heading line and a body line in the
    // same scanned region must report different sizes, proving the wiring is live.
    out += QStringLiteral(";ovsz=%1")
               .arg(tool->overlayFontSizePtOverride(), 0, 'f', 2);
    // Toolbar font-size readback (pipeline 9a4e2f71 phase-2): the current
    // fontSizeSpinBox value, so the native-UI test can confirm a font-size
    // change driven via testToolbarFontSizePipe actually landed on the
    // toolbar control (which fires fontSizeChanged → applyFontSize) BEFORE it
    // asserts the canvas changed. Emitted as ";tbsz=<pt>" among the
    // ';'-delimited fields (before the final '|text'), so span_selected()'s
    // head parse and the text-last contract stay intact.
    if (textEditToolBar_) {
        out += QStringLiteral(";tbsz=%1")
                   .arg(textEditToolBar_->currentFontSizeForTesting(), 0, 'f', 2);
    }
    // Applied selection-run font size (pipeline 9a4e2f71 phase-2): the size
    // applyFontSize() wrote to the SELECTED run, read at the selection start
    // (the spinbox/;tbsz= resets to the cursor-position run just past the
    // selection, so it is not a reliable "was it applied" signal). Emitted as
    // ";runsz=<pt>" so the native-UI font-size test can confirm the change
    // landed before asserting the canvas re-rendered.
    out += QStringLiteral(";runsz=%1")
               .arg(tool->selectionRunFontSizeForTesting(), 0, 'f', 2);
    // Append the active span's text after a '|' delimiter so tests can
    // verify a typed edit actually mutated the span text.  Newlines are
    // stripped (single-line QLineEdit pipe); the field is always last.
    const int ai = tool->activeSpanIndexForTesting();
    if (ai >= 0) {
        QString t = tool->spanTextForTesting(ai);
        t.replace(QChar('\n'), QChar(' ')).replace(QChar('\r'), QChar(' '));
        out += QStringLiteral("|") + t;
    }
    return out;
}

QString MainWindow::activeToolDiag() const
{
    if (!canvas_) {
        return QStringLiteral("none");
    }
    // Region (TRE) tool takes priority — it is the scanned-region editor the
    // #4 routing fix is supposed to activate on a scanned block. It lives in
    // currentTool() (the AnnotationTool dispatch slot), unlike the per-span
    // TextEditTool which has its own dispatch path.
    if (dynamic_cast<pdfe::tre::TreRegionTool*>(canvas_->currentTool())) {
        return QStringLiteral("treToolAction");
    }
    // Per-span vector text tool: present whenever TextEditTool is installed.
    if (canvas_->textEditTool()) {
        return QStringLiteral("textEditToolAction");
    }
    // Any other annotation tool — report the checked action's objectName.
    if (annotToolGroup_) {
        if (QAction* a = annotToolGroup_->checkedAction()) {
            return a->objectName();
        }
    }
    return QStringLiteral("none");
}

// ---------------------------------------------------------------------------
// GitHub #18 (PDF-48) — the shipped form-submit path.
//
// WHY THIS FUNCTION EXISTS AT ALL. FormSubmitter has enforced the AcroForm
// Required bit since commit 972a63d2, with seven GoogleTests over it. None of
// that reached a user: pdfe_formsubmit was linked into nothing, and the
// "Submit Form…" action opened a dialog whose Accept button closed the dialog.
// The library refused an incomplete form; the product sent it. This is the
// missing half.
// ---------------------------------------------------------------------------
QString MainWindow::submitCurrentFormTo(const QUrl& url, bool hostConfirmed)
{
    using pdfe::formsubmit::FormSubmitter;
    using pdfe::formsubmit::HostAllowlist;
    using pdfe::formsubmit::SubmitDecision;
    using pdfe::formsubmit::SubmitFormat;

    // CONSENT GATE (SEC-49). AppSettings::externalActionsAllowed defaults to
    // OFF, and tests/test_action_egress_inertness.cpp named this gate as a
    // precondition of ever wiring this module up. It is checked FIRST, before
    // any field is read, so that a document cannot use the submit path to
    // observe anything while the gate is closed.
    if (!appSettings_ || !appSettings_->externalActionsAllowed()) {
        return QStringLiteral("reject-external-actions-disabled");
    }
    if (!document_ || !formManager_) {
        return QStringLiteral("reject-no-document");
    }

    // Collect the document's fields through the LIVE FormManager — the same
    // parse that sets forms::FormFieldData::isRequired from PDF_FIELD_IS_
    // REQUIRED — and convert with toSubmitFields(), which is what carries that
    // flag across the module boundary. Reading the fields from anywhere else
    // would make the rule one that no real document's data reaches.
    std::vector<pdfe::formsubmit::FormField> fields;
    try {
        for (int p = 0; p < document_->pageCount(); ++p) {
            auto pg = document_->page(p);
            if (!pg) continue;
            const auto pageFields = formManager_->getFormFields(pg.get());
            const auto wire = pdfe::formsubmit::toSubmitFields(pageFields);
            fields.insert(fields.end(), wire.begin(), wire.end());
        }
    } catch (const std::exception& e) {
        return QStringLiteral("submit-failed:") + QString::fromUtf8(e.what());
    }

    HostAllowlist allowlist;
    for (const QString& h : confirmedSubmitHosts_) {
        allowlist.allow(h.toStdString());
    }

    const SubmitDecision decision =
        FormSubmitter::shouldSubmit(url, allowlist, fields);
    switch (decision) {
        case SubmitDecision::RejectInvalidUrl:
            return QStringLiteral("reject-invalid-url");
        case SubmitDecision::RejectBlockedDestination:
            return QStringLiteral("reject-blocked-destination");
        case SubmitDecision::RejectMissingRequiredField: {
            QStringList names;
            for (const auto& n : FormSubmitter::missingRequiredFields(fields)) {
                names << QString::fromStdString(n);
            }
            return QStringLiteral("reject-missing-required:")
                   + names.join(QLatin1Char(','));
        }
        case SubmitDecision::ConfirmNewHost:
            if (!hostConfirmed) {
                return QStringLiteral("confirm-host:") + url.host();
            }
            break;
        case SubmitDecision::Allow:
            break;
    }

    // Only here does anything leave the machine. submitForm() re-enforces the
    // scheme, destination and required-field rules itself, independently of
    // the decision above, so this call site cannot opt out of them.
    FormSubmitter submitter(this);
    const auto result = submitter.submitForm(url, fields,
                                             SubmitFormat::UrlEncoded);
    if (!result.success) {
        return QStringLiteral("submit-failed:") + result.errorMessage;
    }
    return QStringLiteral("submitted:") + QString::number(result.httpStatus);
}

QString MainWindow::submitCurrentFormDiag(const QString& urlText)
{
    if (!pdfe::ui::testseam::testPipesEnabled()) {
        return QStringLiteral("seam-disabled");
    }
    return submitCurrentFormTo(QUrl(urlText.trimmed(), QUrl::StrictMode),
                               /*hostConfirmed=*/false);
}

QString MainWindow::documentEncryptionDiag() const
{
    // GitHub #179 — /EncryptMetadata, surfaced somewhere a test can read it.
    if (!pdfe::ui::testseam::testPipesEnabled()) {
        return QStringLiteral("seam-disabled");
    }
    if (!document_ || !encryptionManager_) {
        return QStringLiteral("no-document");
    }
    try {
        const auto info = encryptionManager_->getEncryptionInfo(document_.get());
        return QStringLiteral("encrypted=%1;metadataEncrypted=%2;method=%3")
            .arg(info.isEncrypted ? 1 : 0)
            .arg(info.metadataEncrypted ? 1 : 0)
            .arg(QString::fromStdString(info.method));
    } catch (const std::exception& e) {
        return QStringLiteral("error:") + QString::fromUtf8(e.what());
    }
}

QString MainWindow::currentPageOcrText()
{
    if (!document_ || !canvas_) {
        return QString();
    }
    // The OCR pipeline is lazily constructed per-document by
    // initTreComponents() (the text-edit path calls it on first edit). The
    // test seam may run before any edit, so trigger the same lazy init here.
    if (!ocrPipeline_) {
        initTreComponents();
    }
    if (!ocrPipeline_) {
        return QString();
    }
    const int pageIdx = canvas_->currentPage();
    if (pageIdx < 0 || pageIdx >= document_->pageCount()) {
        return QString();
    }
    // Defect #3: OCR with a PER-REGION script-aware language instead of the
    // doc-wide English baked at open. A whole-page OSD only sees the majority
    // script (a Latin body hides an Arabic block); detectPageLanguages runs OSD
    // per region and returns the union — e.g. "eng+ara" — so BOTH the Latin and
    // the Arabic blocks on a multilingual page are recognized in one pass and
    // the Arabic block yields Arabic-script codepoints rather than garbled Latin.
    std::string hocr;
    try {
        const std::string base = ocrPipeline_->config().language;  // doc-wide pref
        const std::string lang = ocrPipeline_->detectPageLanguages(pageIdx, base);
        hocr = ocrPipeline_->ocrPage(pageIdx, {}, lang);
    } catch (...) {
        return QString();
    }
    QString text = QString::fromStdString(hocr);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    text.remove(tagRe);
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    text.replace(wsRe, QStringLiteral(" "));
    return text.trimmed();
}

int MainWindow::currentSelectedAnnotationIndex() const
{
    return selectedAnnotationIndex_;
}

// ─── Pipeline b3f19d72 phase-11 test-automation seam ─────────────────────

void MainWindow::publishToolActivateAck(const QString& actionObjectName,
                                       const QString& stateOverride)
{
    // GitHub #271 / #294. Absent in production builds: the result pipe is only
    // constructed when PDFE_TEST_PIPES is set, so this is a null check rather
    // than a policy decision.
    if (!testToolActivateResultPipe_)
        return;

    // "armed" vs "triggered" is reported, not collapsed. A caller re-arming a
    // single-shot tool needs to know a tool OBJECT exists; a caller invoking a
    // menu action only needs to know the action fired. Collapsing them would
    // hand the first caller a success it cannot rely on — which is how #294
    // stayed invisible.
    const bool armed = canvas_ && canvas_->currentTool() != nullptr;

    // The sequence number must advance on EVERY ack, including repeats of the
    // same action, or a caller polling for a change cannot distinguish a fresh
    // acknowledgement from the previous one still sitting in the pipe.
    ++testToolActivateSeq_;

    const QString ack = QString::number(testToolActivateSeq_) + ":"
                        + actionObjectName + ":"
                        + (stateOverride.isEmpty()
                               ? QString(armed ? "armed" : "triggered")
                               : stateOverride);

    // setText on a read-only QLineEdit is fine -- readOnly blocks the USER, not
    // the program. Signals are not blocked because nothing is connected to this
    // pipe; it is write-only from the application's side and read-only from the
    // test's.
    testToolActivateResultPipe_->setText(ack);
}

void MainWindow::publishUndoRepeatResult(const QString& payload)
{
    // GitHub #271. Every reply carries a MONOTONIC SEQUENCE NUMBER, for the
    // same reason the arming acknowledgement does -- and I learned it twice.
    //
    // The caller polls this pipe for its reply. Without a sequence number it
    // can only ask "has the value changed?", and two consecutive batches
    // legitimately produce the SAME string: undoing 1000 operations replies
    // "1000:exhausted", and redoing 1000 replies "1000:exhausted". Identical.
    // So the redo reply was indistinguishable from the undo reply still
    // sitting in the pipe, the caller polled until timeout and returned
    // "unknown", and the test reported that the seam gave no reply.
    //
    // Measured 2026-10-09: the redo had in fact executed -- the window title
    // went from "[doc.pdf]" to "[doc.pdf]*" within a second, and no modal
    // appeared. I spent two runs diagnosing a crash and a hang in the
    // application before finding the defect in the reader.
    //
    // This is the identical defect class the arming acknowledgement exists to
    // prevent, reintroduced one pipe over. Hence: sequence number, not value
    // comparison.
    if (!testUndoRepeatResultPipe_)
        return;
    ++testUndoRepeatSeq_;
    testUndoRepeatResultPipe_->setText(
        QString::number(testUndoRepeatSeq_) + ":" + payload);
}

void MainWindow::runTestUndoRedoRepeat(const QString& spec)
{
    // spec: "<count>" undo, "redo:<count>" redo, or "depth:<n>" to set the
    // undo stack's depth limit.
    QString body = spec;

    // GitHub #271. The depth limit, because the stress test's oracle depends
    // on it and the default makes that oracle UNSATISFIABLE.
    //
    // Measured 2026-10-09: with 1000 annotations created, a request for 1010
    // undos performed exactly 500. UndoManager::maxDepth_ defaults to 500 and
    // UndoManager.h documents it as "configurable depth limit (default 500)",
    // so 500 is CORRECT behaviour -- the oldest 500 commands had already been
    // dropped off the stack and can never be undone.
    //
    // The test's assertion (undo everything, expect 0 annotations) was
    // therefore impossible by design, and had been since it was written. It
    // was simply never reachable: the create loop produced ONE annotation
    // until GitHub #271's acknowledged re-arm fixed it.
    //
    // setMaxDepth is public, documented, supported API. Raising it for a
    // 1000-annotation stress run is configuring the product, not weakening the
    // test -- the oracle stays "undo everything, expect 0".
    if (body.startsWith(QStringLiteral("depth:"))) {
        bool depthOk = false;
        const int depth = body.mid(6).toInt(&depthOk);
        if (!depthOk || depth <= 0) {
            if (testUndoRepeatResultPipe_)
                publishUndoRepeatResult(QStringLiteral("-1:bad-depth"));
            return;
        }
        if (!undoManager_) {
            if (testUndoRepeatResultPipe_)
                publishUndoRepeatResult(
                    QStringLiteral("-1:no-undo-manager"));
            return;
        }
        undoManager_->setMaxDepth(depth);
        if (testUndoRepeatResultPipe_)
            publishUndoRepeatResult(
                QString::number(depth) + ":depth-set");
        return;
    }

    const bool isRedo = body.startsWith(QStringLiteral("redo:"));
    if (isRedo)
        body = body.mid(5);

    bool ok = false;
    const int requested = body.toInt(&ok);
    if (!ok || requested <= 0) {
        if (testUndoRepeatResultPipe_)
            publishUndoRepeatResult(QStringLiteral("-1:bad-spec"));
        return;
    }

    // REENTRANCY GUARD. processEvents() below lets queued work run, including
    // another write to this pipe and any deferred trigger -- so without this a
    // second batch can start inside the first, both walking the same undo stack.
    //
    // Added while chasing what looked like a mid-redo crash. It was not one --
    // the reply was being lost by the reader, see publishUndoRepeatResult --
    // so this guard fixed nothing that was actually broken. It stays because a
    // batch that can re-enter itself has no defined behaviour to reason about,
    // and processEvents genuinely does admit a second write. Recorded honestly:
    // it is a correct precaution, not the fix for a measured defect.
    if (testUndoRepeatBusy_) {
        if (testUndoRepeatResultPipe_)
            publishUndoRepeatResult(QStringLiteral("-1:busy"));
        return;
    }
    testUndoRepeatBusy_ = true;
    struct BusyReset {
        bool* flag;
        ~BusyReset() { *flag = false; }
    } busyReset{&testUndoRepeatBusy_};

    QAction* act = isRedo ? redoAction_ : undoAction_;
    int performed = 0;
    for (int i = 0; i < requested; ++i) {
        // STOP when the action goes disabled. That is the real terminator --
        // the stack is empty -- and it is why the reply carries `performed`
        // rather than `requested`: a caller asking for 1010 on a 1000-deep
        // stack must be able to see that 1000 happened and the rest had
        // nothing to do, which is a success, not a shortfall.
        if (!act || !act->isEnabled())
            break;
        if (isRedo)
            onRedo();
        else
            onUndo();
        ++performed;
        // Let the document and the actions' enabled state settle. Undo here
        // mutates the page and re-evaluates canUndo/canRedo through signals.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (testUndoRepeatResultPipe_) {
        publishUndoRepeatResult(
            QString::number(performed) + ":"
            + ((act && act->isEnabled()) ? "more-available" : "exhausted"));
    }
}

bool MainWindow::activateToolByObjectName(const QString& actionObjectName)
{
    // Iterate all QAction children to find the one whose objectName matches.
    // This covers toolbar actions, menu actions, and any QAction parented to
    // this MainWindow, without requiring a static lookup table.
    for (QAction* action : findChildren<QAction*>()) {
        if (action->objectName() == actionObjectName) {
            if (!action->isEnabled()) {
                // GitHub #271: acknowledge the REFUSAL, distinctly.
                //
                // Without this the caller learns only that no acknowledgement
                // arrived, which it cannot tell apart from the action not
                // existing, the pipe being absent, or the application being
                // wedged -- and it pays the full wait to learn nothing. A
                // disabled tool action is a perfectly ordinary state (no
                // document open, a signature field focused), so the loop that
                // hits it should be told that, immediately, rather than timing
                // out a thousand times.
                publishToolActivateAck(actionObjectName, QStringLiteral("disabled"));
                return false;
            }
            // Pipeline c91e7f3a phase-7: guard for checkable exclusive actions
            // (tool actions in annotToolGroup_).  If the action is already
            // checked, calling trigger() would uncheck it — the opposite of
            // what the test seam intends.  Return true immediately so that
            // repeated calls from _activate_form_tool in the test loop are
            // safe no-ops.
            if (action->isCheckable() && action->isChecked()) {
                // GitHub #271: acknowledge anyway. This path is a no-op for the
                // ACTION, but a caller waiting for an acknowledgement would
                // otherwise wait forever on the one case that needs no work —
                // a hang on the happy path, which is worse than the race.
                publishToolActivateAck(actionObjectName);
                return true;
            }
            // Pipeline 3e9b1d7c phase-2 (P-GAP-102): defer the trigger
            // via QTimer::singleShot(0, ...). Many MainWindow actions
            // open modal QDialog::exec() event loops which would block
            // the pipe's textChanged handler synchronously, causing the
            // UIA set_edit_text() Win32 RPC to time out. Queuing the
            // trigger lets the pipe handler return immediately, then
            // the action fires on the next event loop tick.
            QTimer::singleShot(0, this, [action, this, actionObjectName]() {
                action->trigger();
                // Pipeline c91e7f3a phase-7: after triggering a tool
                // action via the test seam, give the canvas Qt
                // keyboard focus. The pipe QLineEdit has NoFocus
                // policy, but the UIA set_edit_text() call moves
                // Win32 OS focus to the pipe HWND. Without setFocus()
                // here, subsequent PostMessageW Tab/key messages go
                // to the pipe or previously-focused widget instead of
                // PdfCanvas, breaking Tab-navigation in form-tool tests.
                if (canvas_) {
                    canvas_->setFocus(Qt::OtherFocusReason);
                }
                // GitHub #271 / #294. THIS is the acknowledgement, and it is
                // here rather than at the return below precisely because the
                // trigger is deferred: at the return the tool is queued, not
                // armed. A caller that treated the write as completion was
                // racing this lambda.
                publishToolActivateAck(actionObjectName);
            });
            return true;
        }
    }
    return false;
}

void MainWindow::onPrintPreview()
{
    if (!document_) {
        return;
    }

    pdfe::core::PrintOptions options;
    options.range = pdfe::core::PrintRange::AllPages;
    options.fromPage = 1;
    options.toPage = document_->pageCount();

    PrintPreviewDialog dialog(document_.get(), options, this);
    dialog.exec();
}

void MainWindow::onZoomIn()
{
    if (canvas_) {
        canvas_->zoomIn();
    }
}

void MainWindow::onZoomOut()
{
    if (canvas_) {
        canvas_->zoomOut();
    }
}

void MainWindow::onFitWidth()
{
    if (canvas_) {
        canvas_->fitWidth();
    }
}

void MainWindow::onFitPage()
{
    if (canvas_) {
        canvas_->fitPage();
    }
}

void MainWindow::onActualSize()
{
    if (canvas_) {
        canvas_->setZoom(1.0f);
    }
}

void MainWindow::onToggleFullScreen()
{
    if (isFullScreen()) {
        showNormal();
        if (fullScreenAction_) fullScreenAction_->setChecked(false);
    } else {
        showFullScreen();
        if (fullScreenAction_) fullScreenAction_->setChecked(true);
    }
}

// ─── GitHub #216: a View-menu panel toggle must RAISE a tabified dock ──────
//
// The four View > *Panel* toggles all had the same one-line body:
//
//     dock->setVisible(!dock->isVisible());
//
// which is wrong for a dock that is TABIFIED BEHIND a sibling — the default
// arrangement, where Search shares the right-hand dock area with Properties.
// A dock in that state is NOT hidden: it is a live, mapped child that simply
// is not the current tab, so isVisible() returns true and the toggle HIDES it.
// The user clicks "View > Search Panel" while Search is sitting behind
// Properties, the panel they were asking for does not appear, and the menu
// item reads as inert.
//
// MainWindow::onFind (Ctrl+F) already had the right shape — show() then
// raise() — which is why Ctrl+F worked while the menu item did not. These
// helpers give the menu toggles that same shape, and keep the checkable
// action's checked state in step with what actually happened (the slots
// ignored the `checked` argument Qt had already flipped, so the tick mark
// could disagree with the dock).
//
// All four toggles share the shape, so all four are fixed. Only Search is
// named in #216 because that is where the default layout tabifies; the others
// are one user dock-drag away from the identical symptom.
namespace {

// True when `dock` is actually PRESENTED to the user right now.
//
// `visibleRegion()` is the only runtime signal Qt offers for "is the current
// tab" — MainWindow's own testDockStateQueryPipe seam reports exactly this as
// its `raised` field. Before the main window is mapped NOTHING has a visible
// region, so fall back to "not hidden" there; otherwise an unmapped window
// would read every shown dock as tabbed-behind and could never close one.
bool dockIsPresented(const QMainWindow* window, const QDockWidget* dock)
{
    if (!dock || dock->isHidden()) {
        return false;
    }
    if (!window || !window->isVisible()) {
        return true;
    }
    return !dock->visibleRegion().isEmpty();
}

// Hide a presented dock; otherwise show it and raise it to the front of its
// tab group. `action` is the checkable View-menu QAction whose tick must end
// up agreeing with the dock. (Panel-specific follow-up, such as focusing the
// search box, belongs in the caller — only Search wants it.)
void togglePanelDock(QMainWindow* window, QDockWidget* dock, QAction* action)
{
    if (!dock) {
        return;
    }
    if (dockIsPresented(window, dock)) {
        dock->setVisible(false);
        if (action) action->setChecked(false);
        return;
    }
    dock->setVisible(true);
    dock->raise();                 // become the CURRENT tab — the #216 fix
    if (action) action->setChecked(true);
}

} // namespace

void MainWindow::onTogglePagePanel()
{
    togglePanelDock(this, pagePanelDock_, togglePagePanelAction_);
}

void MainWindow::onTogglePropertiesPanel()
{
    togglePanelDock(this, propertiesPanelDock_, togglePropertiesPanelAction_);
}

void MainWindow::onToggleSearchPanel()
{
    togglePanelDock(this, searchPanelDock_, toggleSearchPanelAction_);
    // Same courtesy onFind() extends: a raised search panel with the caret
    // already in the box is what the user asked for.
    if (searchPanel_ && searchPanelDock_ && !searchPanelDock_->isHidden()) {
        searchPanel_->focusSearchBox();
    }
}

void MainWindow::onToggleBookmarkPanel()
{
    togglePanelDock(this, bookmarkPanelDock_, toggleBookmarkPanelAction_);
}

void MainWindow::onAbout()
{
    AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::onDocumentation()
{
    QDesktopServices::openUrl(
        QUrl("https://github.com/ipswitch8/PDFE#documentation"));
}

void MainWindow::onReportIssue()
{
    QDesktopServices::openUrl(
        QUrl("https://github.com/ipswitch8/PDFE/issues/new"));
}

void MainWindow::maybeCheckForUpdateOnStartup()
{
    // Test isolation, following the PDFE_TEST_SKIP_UNSAVED_PROMPT convention
    // already set by tests/test_main.cpp. Without this, a developer who ticks
    // the Preferences box makes EVERY MainWindow-constructing test in the suite
    // issue a live HTTPS request -- AppSettings with no PDFE_SETTINGS_PATH
    // reads the real user store. That would be non-deterministic egress from a
    // test run, and test_action_egress_inertness.cpp exists precisely to assert
    // the absence of such things.
    if (qEnvironmentVariableIsSet("PDFE_TEST_NO_UPDATE_CHECK")) {
        return;
    }

    // OFF BY DEFAULT. A stock install reaches this line and returns without
    // making any request at all -- that is the entire point of the opt-in.
    if (!appSettings_ || !appSettings_->updateCheckEnabled()) {
        return;
    }

    if (!updateCheckNam_) {
        updateCheckNam_ = std::make_unique<QNetworkAccessManager>(this);
    }

    pdfe::app::checkForUpdateAsync(
        *updateCheckNam_,
        [this](const pdfe::app::UpdateCheckResult& r) {
            // NOTIFY ONLY. Nothing here downloads, writes or launches
            // anything; the user is told, and installing stays an explicit
            // act performed through the browser via Help > Check for Updates.
            // See UC-9 in docs/code-signing.md.
            if (!r.checked || !r.updateAvailable) {
                return;     // silent: a failed or negative check must not nag
            }
            // ASCII only in this literal on purpose: a non-ASCII byte here
            // depends on the compiler agreeing with the file's encoding, which
            // is the same class of defect as GitHub #130 one layer down.
            statusBar()->showMessage(
                tr("PDFE %1 is available (you have %2) - see Help > Check for Updates")
                    .arg(r.latestVersion,
                         QString::fromStdString(pdfe::app::currentVersion())),
                15000);
        });
}

void MainWindow::onCheckUpdates()
{
    QDesktopServices::openUrl(
        QUrl("https://github.com/ipswitch8/PDFE/releases"));
}

void MainWindow::onPageChanged(int pageIndex)
{
    if (!document_) {
        return;
    }

    // Record the visit into back/forward history. NavigationHistory
    // ignores recordings made while it's replaying a back/forward
    // navigation, so this call is safe to make unconditionally.
    navHistory_.recordVisit(pageIndex);
    if (previousViewAction_) previousViewAction_->setEnabled(navHistory_.canGoBack());
    if (nextViewAction_) nextViewAction_->setEnabled(navHistory_.canGoForward());

    // Close sticky note popup on page change
    if (stickyNotePopup_ && stickyNotePopup_->isPopupVisible()) {
        stickyNotePopup_->closePopup();
    }

    // Invalidate deferred atlas — HOCR data is page-specific
    pendingAtlas_ = PendingAtlasState{};

    // Update status bar
    statusBar_->setPageInfo(pageIndex + 1, document_->pageCount());

    // Update page panel
    pagePanel_->setCurrentPage(pageIndex);

    // Update properties panel
    if (pageIndex >= 0 && pageIndex < document_->pageCount()) {
        auto pageInfo = document_->pageInfo(pageIndex);
        propertiesPanel_->setPageInfo(pageInfo);
    }

    // Update text edit tool page
    if (canvas_ && canvas_->textEditTool()) {
        canvas_->textEditTool()->setPage(pageIndex);
    }

    // Auto-switch text edit mode when navigating pages
    if (textEditAutoRouting_ && textEditToolAction_->isChecked()) {
        activateTextEditForPage(pageIndex);
    }

    // Load OCR box data for new page (if overlay is active)
    loadOcrBoxesForPage(pageIndex);
}

void MainWindow::onZoomChanged(float zoom)
{
    statusBar_->setZoom(zoom * 100.0f);
    updateZoomCombo(zoom);
}

void MainWindow::onPageSelected(int pageIndex)
{
    if (canvas_) {
        canvas_->goToPage(pageIndex);
    }
}

void MainWindow::onSearchResultSelected(int pageIndex, QVector<QRectF> quads)
{
    if (canvas_) {
        canvas_->setSearchHighlights(pageIndex, quads);
        canvas_->goToPage(pageIndex);
    }
}

void MainWindow::onBookmarkActivated(int pageIndex, float x, float y, float zoom)
{
    // For now, just navigate to the page. Ignore x, y, zoom parameters.
    if (canvas_) {
        canvas_->goToPage(pageIndex);
    }
}

void MainWindow::onZoomComboChanged(int index)
{
    if (!canvas_ || index < 0) {
        return;
    }

    QString text = zoomCombo_->itemText(index);
    text.remove('%');
    bool ok;
    float zoomPercent = text.toFloat(&ok);
    if (ok && zoomPercent > 0) {
        canvas_->setZoom(zoomPercent / 100.0f);
    }
}

void MainWindow::onZoomComboEdited(const QString& text)
{
    if (!canvas_) {
        return;
    }

    QString cleanText = text;
    cleanText.remove('%').remove(' ');
    bool ok;
    float zoomPercent = cleanText.toFloat(&ok);
    if (ok && zoomPercent > 0) {
        canvas_->setZoom(zoomPercent / 100.0f);
    }
}

void MainWindow::updateWindowTitle()
{
    if (document_) {
        QString fileName = documentDisplayName_.isEmpty()
                               ? QFileInfo(currentFileName_).fileName()
                               : documentDisplayName_;
        bool dirty = undoManager_ && !undoManager_->isClean();
        if (dirty) {
            setWindowTitle(tr("PDFE - [%1]*").arg(fileName));
        } else {
            setWindowTitle(tr("PDFE - [%1]").arg(fileName));
        }
    } else {
        setWindowTitle(tr("PDFE"));
    }
}

void MainWindow::updateZoomCombo(float zoom)
{
    if (!zoomCombo_) {
        return;
    }

    // Block signals while updating
    QSignalBlocker blocker(zoomCombo_);

    QString zoomText = QString::number(static_cast<int>(zoom * 100)) + "%";
    zoomCombo_->setCurrentText(zoomText);
}

// ---------------------------------------------------------------------------
// GitHub #60 (SEC-62) — active-content advisory banner.
//
// ProhibitedFeatureScanner existed and was well tested, but its only callers
// were PdfAValidator, PdfAConverter and PdfUAConverter — all of them on the
// explicit "check this document for PDF/A compliance" path the user has to go
// looking for. Nothing ran it when a document was OPENED, which is the moment
// the answer matters: a PDF is an attacker-controlled file format and PDFE
// already default-DENIES JavaScript and /URI actions (AppSettings::
// javaScriptEnabled / externalActionsAllowed, both false). The user was never
// told either that the document contained such content or that it had been
// held back — AppSettings.h has referred to "a per-document banner" as the
// intended UX for opting back in since those gates were added.
//
// A QFrame, NOT a QDialog, and that is the load-bearing decision. A Qt
// app-modal QDialog::exec() cannot be enumerated by UIA at all (measured: a
// dialog open for four minutes while a 240 s poll reported nothing), so a
// modal advisory is unobservable to the UI-automation layer and — worse for a
// document that merely CONTAINS JavaScript rather than one that is dangerous —
// it interrupts every open. The banner is in the widget tree, so it carries a
// stable objectName and accessibleName and any layer can read it.
// ---------------------------------------------------------------------------
void MainWindow::createProhibitedFeatureBanner()
{
    prohibitedFeatureBanner_ = new QFrame(centralArea_);
    prohibitedFeatureBanner_->setObjectName("prohibitedFeatureBanner");
    prohibitedFeatureBanner_->setAccessibleName(tr("Active Content Warning"));
    prohibitedFeatureBanner_->setAccessibleDescription(
        tr("Warns that the open document contains active content, such as "
           "JavaScript or external actions, which PDFE does not run unless "
           "you allow it."));
    prohibitedFeatureBanner_->setFrameShape(QFrame::StyledPanel);
    prohibitedFeatureBanner_->setAutoFillBackground(true);
    prohibitedFeatureBanner_->setStyleSheet(
        QStringLiteral("QFrame#prohibitedFeatureBanner {"
                       " background-color: #fff4ce;"
                       " border: 1px solid #d8bd6a; }"
                       "QFrame#prohibitedFeatureBanner QLabel {"
                       " color: #4a3c00; }"));

    auto* layout = new QHBoxLayout(prohibitedFeatureBanner_);
    layout->setContentsMargins(8, 4, 4, 4);
    layout->setSpacing(8);

    prohibitedFeatureBannerText_ = new QLabel(prohibitedFeatureBanner_);
    prohibitedFeatureBannerText_->setObjectName("prohibitedFeatureBannerText");
    prohibitedFeatureBannerText_->setAccessibleName(
        tr("Active Content Warning Text"));
    // PlainText, NOT the QLabel default of Qt::AutoText. Nothing document-
    // derived is interpolated into this string today (only fixed category
    // names), but the label lives on the open path of an attacker-controlled
    // file format and AutoText renders rich text; pinning the format here
    // means a later edit that DOES interpolate a title cannot become a markup
    // injection by accident.
    prohibitedFeatureBannerText_->setTextFormat(Qt::PlainText);
    prohibitedFeatureBannerText_->setWordWrap(true);
    layout->addWidget(prohibitedFeatureBannerText_, 1);

    prohibitedFeatureBannerDismiss_ = new QToolButton(prohibitedFeatureBanner_);
    prohibitedFeatureBannerDismiss_->setObjectName(
        "prohibitedFeatureBannerDismissButton");
    prohibitedFeatureBannerDismiss_->setText(tr("Dismiss"));
    prohibitedFeatureBannerDismiss_->setAccessibleName(
        tr("Dismiss Active Content Warning"));
    prohibitedFeatureBannerDismiss_->setAccessibleDescription(
        tr("Hides the active content warning for this document."));
    prohibitedFeatureBannerDismiss_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(prohibitedFeatureBannerDismiss_, &QToolButton::clicked, this,
            [this]() {
                if (prohibitedFeatureBanner_)
                    prohibitedFeatureBanner_->hide();
            });
    layout->addWidget(prohibitedFeatureBannerDismiss_, 0);

    prohibitedFeatureBanner_->hide();
}

void MainWindow::updateProhibitedFeatureBanner()
{
    if (!prohibitedFeatureBanner_ || !prohibitedFeatureBannerText_)
        return;

    QStringList features;
    if (document_ && muPdfContext_) {
        try {
            pdfe::compliance::ProhibitedFeatureScanner scanner(muPdfContext_);
            // Only the two ACTIVE-CONTENT categories, deliberately.
            //
            // scanAll() also reports transparency and object streams, but both
            // are PDF/A *formatting* rules with no security meaning, and object
            // streams are present in very nearly every PDF written this
            // century — surfacing them would raise the banner on essentially
            // every document, which trains the user to dismiss it reflexively
            // and is strictly worse than no banner at all. Encryption is
            // likewise omitted: it is not active content and openFile already
            // reports it through StatusBar::setEncrypted.
            if (scanner.hasJavaScript(document_.get()))
                features << tr("JavaScript");
            if (scanner.hasExternalReferences(document_.get()))
                features << tr("external actions (Launch, GoToR, Sound or "
                               "Movie)");
        } catch (const std::exception&) {
            // A scan failure must not block the open, and must not be reported
            // as "clean" by leaving a stale banner up either — features stays
            // empty and the banner is hidden below.
            features.clear();
        }
    }

    if (features.isEmpty()) {
        prohibitedFeatureBannerText_->clear();
        prohibitedFeatureBanner_->hide();
        return;
    }

    prohibitedFeatureBannerText_->setText(
        tr("This document contains active content: %1. PDFE does not run it "
           "unless you allow it in Preferences → Security.")
            .arg(features.join(tr(", "))));
    prohibitedFeatureBanner_->show();
}

void MainWindow::closeDocument()
{
    // Close sticky note popup
    if (stickyNotePopup_) {
        stickyNotePopup_->closePopup();
    }

    // Pipeline H phase-4: clear annotation selection state so
    // openAltTextDialogAction can't fire on a stale index after the
    // page geometry has been torn down.
    selectedAnnotationPageIndex_ = -1;
    selectedAnnotationIndex_ = -1;
    if (openAltTextDialogAction_) {
        openAltTextDialogAction_->setEnabled(false);
    }

    // Clear UI
    canvas_->clearDocument();
    pagePanel_->clearDocument();
    searchPanel_->clearDocument();
    bookmarkPanel_->clearDocument();
    if (layerPanel_) layerPanel_->clearDocument();
    commentPanel_->clearDocument();
    if (attachmentPanel_) attachmentPanel_->clearDocument();
    if (attachmentPanelDock_) attachmentPanelDock_->hide();
    propertiesPanel_->clearPageInfo();
    annotationProperties_->clear();
    statusBar_->clearInfo();

    // GitHub #60 (SEC-62): the advisory describes the document that was open,
    // so it must not outlive it. openFile() calls closeDocument() first, so
    // this is also what guarantees a clean document cannot inherit the previous
    // document's banner.
    if (prohibitedFeatureBanner_) {
        prohibitedFeatureBanner_->hide();
        if (prohibitedFeatureBannerText_)
            prohibitedFeatureBannerText_->clear();
    }

    // Hide bookmarks tab (will be shown again if next PDF has bookmarks)
    if (bookmarkPanelDock_) {
        bookmarkPanelDock_->hide();
        toggleBookmarkPanelAction_->setChecked(false);
    }

    // Clear defaults log for consistency with other panels
    if (defaultsLogPanel_) {
        defaultsLogPanel_->clearLog();
    }

    // R3-04: tear down the autosave engine. Capture the dirty state BEFORE the
    // undo stack is cleared below. A CLEAN document leaves nothing to recover,
    // so remove its sidecar. A DIRTY document being closed/replaced without a
    // save (openFile closes the prior doc without prompting) intentionally
    // LEAVES its recovery sidecar so the unsaved work can still be recovered on
    // a later launch — the recovery feature's whole purpose.
    if (autosaveManager_) {
        autosaveManager_->stop();
        const bool cleanNow = !undoManager_ || undoManager_->isClean();
        if (cleanNow) {
            autosaveManager_->clearRecovery();
        }
        autosaveManager_.reset();
    }

    // Clear undo history
    if (undoManager_) {
        undoManager_->clear();
    }

    // Clear form support
    formPropertiesPanel_->clear();
    formDataIO_.reset();

    // Pipeline E phase-9: fire "willClose" while both document_ and
    // jsEngine_ are still alive, then break the callback link before
    // tearing down the engine — otherwise the document destructor at
    // line 3608 would invoke a lambda that captures a now-dangling
    // jsEngine_ raw pointer and crash.
    if (document_ && jsEngine_ && document_->pageCount() > 0) {
        auto firstPage = document_->page(0);
        if (firstPage) {
            jsEngine_->fireDocumentEvent("willClose", firstPage.get());
        }
    }
    if (document_) {
        document_->setDocumentEventCallback(nullptr);
    }

    jsEngine_.reset();
    formManager_.reset();

    // Clear annotation manager and tool
    annotationManager_.reset();
    toolPage_.reset();
    commandPages_.clear();
    if (canvas_) {
        canvas_->clearTextEditTool();
        canvas_->clearCurrentTool();
    }
    textEditToolBar_->setVisible(false);
    textEditToolBar_->clearFontInfo();
    formToolBar_->setVisible(false);

    // Clear OCR box overlay data
    if (canvas_) {
        canvas_->clearOcrBoxData();
        canvas_->setOcrBoxesVisible(false);
    }
    ocrBoxesEnabled_ = false;
    if (toggleOcrBoxesAction_) {
        QSignalBlocker blocker(toggleOcrBoxesAction_);
        toggleOcrBoxesAction_->setChecked(false);
    }

    // Destroy TRE infrastructure and scanned page edit tracking
    currentEnrichedAtlas_.reset();
    scanCroppedAtlas_.reset();
    freeTypeStyledAtlas_.reset();
    currentAtlasMode_ = pdfe::tre::AtlasMode::ScanCropped;
    atlasChoiceMade_ = false;
    fillMissingPersisted_ = false;
    pendingAtlas_ = PendingAtlasState{};
    textEditToolBar_->showAtlasControls(false);
    releaseImageEditStack();
    imageEditPos_ = -1;
    nextRegionId_ = 1;
    destroyTreComponents();

    // Clear redaction support
    redactionManager_.reset();
    if (redactionPanel_) redactionPanel_->clear();

    // Clear encryption support
    encryptionManager_.reset();

    // Clear signature support
    signatureManager_.reset();
    if (signaturePanel_) signaturePanel_->clear();

    // Clear image support
    imageTool_.reset();
    imageManager_.reset();
    imageContentEditor_.reset();
    exifStripper_.reset();
    if (imagePropertiesPanel_) imagePropertiesPanel_->clearInfo();

    // Clear metadata support
    metadataManager_.reset();

    // Reset toggle states
    if (toggleStickyNotesAction_) toggleStickyNotesAction_->setChecked(true);
    if (toggleCopyPreventionAction_) toggleCopyPreventionAction_->setChecked(false);
    if (togglePrintPreventionAction_) togglePrintPreventionAction_->setChecked(false);
    copyPreventionActive_ = false;
    printPreventionActive_ = false;
    if (canvas_) canvas_->setCopyPrevented(false);

    // Destroy rendering infrastructure
    tileRenderer_.reset();
    tileCache_.reset();
    document_.reset();

    // Update UI
    currentFileName_.clear();
    documentDisplayName_.clear();
    updateWindowTitle();

    // Reset navigation history so the new doc starts fresh.
    navHistory_.clear();
    if (previousViewAction_) previousViewAction_->setEnabled(false);
    if (nextViewAction_) nextViewAction_->setEnabled(false);

    // Disable actions
    saveAction_->setEnabled(false);
    saveAsAction_->setEnabled(false);
    if (exportPlainTextAction_) exportPlainTextAction_->setEnabled(false);
    closeAction_->setEnabled(false);
    copyAction_->setEnabled(false);
    cutAction_->setEnabled(false);
    pasteAction_->setEnabled(false);
    selectAllAction_->setEnabled(false);
    imageRemoveAction_->setEnabled(false);
    imageRotateAction_->setEnabled(false);
    imageCropAction_->setEnabled(false);
    imageResizeAction_->setEnabled(false);
    zoomInAction_->setEnabled(false);
    zoomOutAction_->setEnabled(false);
    fitWidthAction_->setEnabled(false);
    fitPageAction_->setEnabled(false);
    actualSizeAction_->setEnabled(false);
    zoomCombo_->setEnabled(false);
    printAction_->setEnabled(false);
    printPreviewAction_->setEnabled(false);
    removePasswordAction_->setEnabled(false);

    // Disable form actions
    createFormFieldAction_->setEnabled(false);
    formFieldPropertiesAction_->setEnabled(false);
    exportFormDataAction_->setEnabled(false);
    importFormDataAction_->setEnabled(false);
    resetFormAction_->setEnabled(false);
    flattenFormAction_->setEnabled(false);

    // Disable page manipulation actions
    rotatePageCWAction_->setEnabled(false);
    rotatePageCCWAction_->setEnabled(false);
    rotatePage180Action_->setEnabled(false);
    deletePageAction_->setEnabled(false);
    insertBlankPageAction_->setEnabled(false);
    insertPagesFromFileAction_->setEnabled(false);
    extractPagesAction_->setEnabled(false);
    movePageUpAction_->setEnabled(false);
    movePageDownAction_->setEnabled(false);

    // Disable encryption actions
    saveWithEncryptionAction_->setEnabled(false);
    documentPermissionsAction_->setEnabled(false);

    // Disable signature actions
    signDocumentAction_->setEnabled(false);
    verifySignaturesAction_->setEnabled(false);
    if (requestValidationAction_) {
        requestValidationAction_->setEnabled(false);
    }

    // Disable image actions
    imageToolAction_->setEnabled(false);
    insertImageAction_->setEnabled(false);
    stripExifAction_->setEnabled(false);
    documentMetadataAction_->setEnabled(false);
    complianceAction_->setEnabled(false);
    if (preflightAction_) preflightAction_->setEnabled(false);
}

void MainWindow::onAnnotationToolSelected(QAction* action)
{
    if (!document_ || !annotationManager_ || !canvas_) {
        return;
    }

    // Guard: skip the duplicate call from QActionGroup::triggered signal.
    // onTextEditToolSelected sets skipGroupTextEditCall_ before calling
    // activateTextEditForPage, which already handled the routing. The group
    // signal fires this method a second time with textEditToolAction_ —
    // consume the flag and return BEFORE clearing the current tool.
    if (action == textEditToolAction_ && skipGroupTextEditCall_) {
        skipGroupTextEditCall_ = false;
        return;
    }

    // Clear existing tool first
    canvas_->clearCurrentTool();
    canvas_->clearImageTool();

    // Create appropriate tool based on action
    std::unique_ptr<pdfe::annotations::AnnotationTool> tool;

    if (action == selectToolAction_) {
        // Select tool -- hit-test, select, and move annotations
        canvas_->clearTextEditTool();
        textEditToolBar_->setVisible(false);
        textEditToolBar_->clearFontInfo();
        formToolBar_->setVisible(false);
        auto selectTool = std::make_unique<pdfe::annotations::SelectTool>(
            annotationManager_.get());
        selectTool->annotationSelected = [this](int pageIndex, int annotIndex) {
            // Show properties in comment panel
            if (!document_ || !annotationManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto data = annotationManager_->getAnnotation(page.get(), annotIndex);
            if (annotationProperties_) {
                annotationProperties_->setAnnotation(data);
            }
            // Pipeline H phase-4: update selection state for the
            // openAltTextDialogAction; enabling it grants UIA-driven
            // tests an AutomationId path to the Alt Text dialog
            // without a second canvas pixel-click.
            selectedAnnotationPageIndex_ = pageIndex;
            selectedAnnotationIndex_ = annotIndex;
            if (openAltTextDialogAction_) {
                openAltTextDialogAction_->setEnabled(true);
            }
            // Raise the annotation properties dock so the slider / author /
            // contents fields are in front and reachable by UIA-driven tests.
            if (annotationPropertiesDock_) {
                annotationPropertiesDock_->show();
                annotationPropertiesDock_->raise();
            }
        };
        // R3-12: a real on-canvas selection (single annotation, marquee union,
        // or empty-marquee region) sets the copy-selection so onCopy() renders
        // JUST that region to the clipboard as an image (CF_DIB/CF_BITMAP)
        // instead of copying the whole-page text.
        selectTool->regionSelected = [this](int pageIndex, QRectF pdfRect) {
            setCopySelectionRect(pageIndex, pdfRect);
        };
        selectTool->annotationMoved = [this](int pageIndex, int annotIndex, QRectF oldRect, QRectF newRect) {
            if (!document_ || !annotationManager_ || !undoManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto* pagePtr = page.get();
            commandPages_.push_back(std::move(page));

            auto oldData = annotationManager_->getAnnotation(pagePtr, annotIndex);
            auto newData = oldData;
            newData.rect = newRect;
            oldData.rect = oldRect;

            auto cmd = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
                annotationManager_.get(), pagePtr, annotIndex,
                pdfe::annotations::ModifyAnnotationCommand::Property::Rect,
                oldData, newData);
            undoManager_->execute(std::move(cmd));

            if (canvas_) canvas_->refreshPage(pageIndex);
            updateWindowTitle();
        };
        selectTool->annotationDeleted = [this](int pageIndex, int annotIndex) {
            onCommentPanelDelete(pageIndex, annotIndex);
        };
        selectTool->annotationPasted = [this](int pageIndex, pdfe::annotations::AnnotationData pasteData) {
            if (!document_ || !annotationManager_ || !undoManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto* pagePtr = page.get();
            commandPages_.push_back(std::move(page));

            auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                annotationManager_.get(), pagePtr, pasteData.type, pasteData.rect);
            auto* createCmd = cmd.get();
            undoManager_->execute(std::move(cmd));

            // GitHub #170, third instance of the same defect: paste on a page
            // that carries link annotations addressed the /Annots-only link
            // tail of getAnnotations() and threw out of the event handler.
            // See onAnnotationCreatedByTool for the full mechanism.
            const int newIndex = createCmd->createdAnnotation().annotIndex;
            if (newIndex >= 0) {
                if (pasteData.color != QColor(255, 255, 0)) {
                    annotationManager_->setColor(pagePtr, newIndex, pasteData.color);
                }
                if (!pasteData.contents.empty()) {
                    annotationManager_->setContents(pagePtr, newIndex, pasteData.contents);
                }
                if (std::abs(pasteData.opacity - 1.0f) > 0.001f) {
                    annotationManager_->setOpacity(pagePtr, newIndex, pasteData.opacity);
                }
                if (!pasteData.quadPoints.empty()) {
                    annotationManager_->setQuadPoints(pagePtr, newIndex, pasteData.quadPoints);
                }
                if (!pasteData.inkStrokes.empty()) {
                    annotationManager_->setInkStrokes(pagePtr, newIndex, pasteData.inkStrokes);
                }
            }

            if (commentPanel_) commentPanel_->refresh();
            if (canvas_) canvas_->refreshPage(pageIndex);
            updateWindowTitle();
        };
        selectTool->selectionCleared = [this]() {
            if (annotationProperties_) {
                annotationProperties_->clear();
            }
            if (stickyNotePopup_ && stickyNotePopup_->isPopupVisible()) {
                stickyNotePopup_->closePopup();
            }
            // Pipeline H phase-4 AC#2: enabled iff exactly one
            // annotation is selected — the deselection branch must
            // also disable the action and clear the tracked indices,
            // otherwise the invariant is violated when the user
            // selects then deselects.
            selectedAnnotationPageIndex_ = -1;
            selectedAnnotationIndex_ = -1;
            if (openAltTextDialogAction_) {
                openAltTextDialogAction_->setEnabled(false);
            }
            // R3-12: deselect drops the copy-selection back to whole-page.
            setCopySelectionRect(-1, QRectF());
        };
        selectTool->stickyNoteDoubleClicked = [this](int pageIndex, int annotIndex, pdfe::annotations::AnnotationData data) {
            onStickyNoteDoubleClicked(pageIndex, annotIndex, data);
        };
        selectTool->annotationPreviewRequested = [this](int pageIndex, int annotIndex, pdfe::annotations::AnnotationData data) {
            onStickyNoteDoubleClicked(pageIndex, annotIndex, data);
            // Single-click opens in read-only mode
            if (stickyNotePopup_) {
                stickyNotePopup_->setReadOnly(true);
            }
        };
        selectTool->annotationRotated = [this](int pageIndex, int annotIndex, double degrees) {
            if (!document_ || !annotationManager_ || !undoManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto* pagePtr = page.get();
            commandPages_.push_back(std::move(page));

            auto oldData = annotationManager_->getAnnotation(pagePtr, annotIndex);
            auto newData = oldData;
            // Compute rotated bounding box: expand rect to accommodate rotation.
            // This math was always correct and is kept as-is -- a PDF /Rect is
            // axis-aligned by definition, so it MUST widen to the bounds of the
            // rotated shape. What was missing is the line below it: the angle
            // itself was never stored, so nothing downstream could draw the
            // text rotated and the user saw the outline turn while the text
            // stayed upright. Field-reported 2026-08-28; see
            // docs/audit/field-report-2026-08-28-scanned-manual.md (2).
            double rad = degrees * M_PI / 180.0;
            double cosA = std::abs(std::cos(rad));
            double sinA = std::abs(std::sin(rad));
            double cx = oldData.rect.center().x();
            double cy = oldData.rect.center().y();
            double hw = oldData.rect.width() / 2.0;
            double hh = oldData.rect.height() / 2.0;
            double newHw = hw * cosA + hh * sinA;
            double newHh = hw * sinA + hh * cosA;
            newData.rect = QRectF(cx - newHw, cy - newHh, newHw * 2.0, newHh * 2.0);
            newData.rotationDegrees = degrees;

            auto cmd = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
                annotationManager_.get(), pagePtr, annotIndex,
                pdfe::annotations::ModifyAnnotationCommand::Property::Rotation,
                oldData, newData);
            undoManager_->execute(std::move(cmd));

            if (canvas_) canvas_->refreshPage(pageIndex);
            updateWindowTitle();
        };
        selectTool->multipleAnnotationsSelected = [this](int /*pageIndex*/, std::vector<int> indices) {
            if (annotationProperties_) {
                annotationProperties_->clear();
            }
            // Update status bar with selection count
            if (statusBar()) {
                statusBar()->showMessage(
                    tr("%1 annotations selected").arg(static_cast<int>(indices.size())), 3000);
            }
        };
        tool = std::move(selectTool);
    } else if (action == stickyNoteToolAction_) {
        tool = std::make_unique<pdfe::annotations::StickyNoteTool>(annotationManager_.get());
    } else if (action == highlightToolAction_) {
        auto textSelector = std::make_shared<pdfe::annotations::TextSelector>(muPdfContext_);
        tool = std::make_unique<pdfe::annotations::TextMarkupTool>(
            annotationManager_.get(),
            textSelector,
            pdfe::annotations::AnnotationType::Highlight,
            QColor(255, 255, 0)); // Yellow
    } else if (action == underlineToolAction_) {
        auto textSelector = std::make_shared<pdfe::annotations::TextSelector>(muPdfContext_);
        tool = std::make_unique<pdfe::annotations::TextMarkupTool>(
            annotationManager_.get(),
            textSelector,
            pdfe::annotations::AnnotationType::Underline,
            QColor(0, 0, 255)); // Blue
    } else if (action == strikeOutToolAction_) {
        auto textSelector = std::make_shared<pdfe::annotations::TextSelector>(muPdfContext_);
        tool = std::make_unique<pdfe::annotations::TextMarkupTool>(
            annotationManager_.get(),
            textSelector,
            pdfe::annotations::AnnotationType::StrikeOut,
            QColor(255, 0, 0)); // Red
    } else if (action == treToolAction_) {
        // Scanned text edit tool -- TRE region selection
        initTreComponents();
        auto treTool = std::make_unique<pdfe::tre::TreRegionTool>();
        treTool->regionSelected = [this](int pageIndex, QRectF pdfRect) {
            onTreRegionSelected(pageIndex, pdfRect);
        };
        canvas_->clearTextEditTool();
        textEditToolBar_->setVisible(false);
        textEditToolBar_->clearFontInfo();
        formToolBar_->setVisible(false);
        tool = std::move(treTool);
    } else if (action == textEditToolAction_) {
        // Text edit tool uses a separate dispatch path in PdfCanvas.
        //
        // Console-session retry (2026-05-06): preserve currentAtlasMode_
        // and atlasChoiceMade_ across edit-tool activations within the
        // same document. The user's mode preference (Scan Glyphs vs
        // Font Matched) and "have they chosen yet" should persist when
        // the user enters/exits edit mode multiple times. Both are
        // reset on document close (line ~4052) where they belong.
        // Atlases themselves (scanCroppedAtlas_, freeTypeStyledAtlas_)
        // and pendingAtlas_ ARE reset because the page content may
        // have changed and they need to be rebuilt from fresh OCR.
        pendingAtlas_ = PendingAtlasState{};  // Clear stale atlas data from previous page/tool
        scanCroppedAtlas_.reset();
        freeTypeStyledAtlas_.reset();
        // Restore the atlas-controls visibility on the toolbar to match
        // the persisted state: if the user already chose, the toolbar
        // combo should be visible (will be re-shown after rebuild).
        textEditToolBar_->showAtlasControls(false);
        qDebug() << "[TextEdit] Activating text edit tool, page=" << canvas_->currentPage();
        writeUiStatus("TEXT_EDIT_ACTIVATING");
        auto textTool = std::make_unique<pdfe::textedit::TextEditTool>(
            document_.get(), muPdfContext_);

        textTool->textEdited = [this](int pageIndex, int spanIndex,
                                       std::u32string oldText, std::u32string newText,
                                       pdfe::textedit::TextSpan spanInfo) {
            qDebug() << "[TextEdit] textEdited callback: page=" << pageIndex
                     << "span=" << spanIndex
                     << "fontName=" << QString::fromStdString(spanInfo.fontName)
                     << "fontSize=" << spanInfo.fontSize
                     << "bbox=" << spanInfo.boundingBox
                     << "formattingRuns=" << spanInfo.formattingRuns.size()
                     << "textChanged=" << (oldText != newText);
            // Create an undo command for the text edit
            auto cmd = std::make_unique<pdfe::textedit::EditTextCommand>(
                document_.get(),
                muPdfContext_,
                pageIndex,
                spanIndex,
                spanInfo,
                oldText,
                newText
            );
            undoManager_->execute(std::move(cmd));
            updateWindowTitle();

            // Update the visible bitmap layer only when the edited SPAN is an
            // OCR invisible-text layer (glyphless font over a scan) — the same
            // per-span predicate that EditTextCommand::applyText uses to gate
            // the Tr=3 path (defect #1). Unifying on spanInfo.isOcrInvisibleLayer()
            // (instead of the page-wide Page::isImageDominated() verdict this
            // used before) means the two classifiers cannot disagree on a single
            // span: a real vector heading on an image-heavy page is edited as
            // visible text and is NOT routed through the ImageReplacer bitmap path.
            auto page = document_->page(pageIndex);
            bool ocrInvisible = spanInfo.isOcrInvisibleLayer();
            qDebug() << "[TextEdit] textEdited: ocrInvisibleLayer=" << ocrInvisible
                     << "fontName=" << QString::fromStdString(spanInfo.fontName);
            if (ocrInvisible) {
                int depth = undoManager_->undoStackSize();
                bool isMerge = (imageEditPos_ >= 0 &&
                                imageEditStack_[imageEditPos_].undoDepth == depth);

                // Save the XObject reference before modification (for lossless undo).
                // Skip for merges — keep the original pre-edit ref.
                void* xobjRef = nullptr;
                std::string xobjName;
                if (!isMerge && pdfContentWriter_) {
                    auto imageInfo = pdfContentWriter_->findPageImageXObject(
                        page.get(), document_.get());
                    if (imageInfo.has_value()) {
                        xobjName = imageInfo->xobjectName;
                    }
                    xobjRef = pdfContentWriter_->saveImageXObjectRef(
                        page.get(), document_.get());
                }

                // Run ImageReplacer to update visible XObject.
                // This modifies the page's resource dictionary (XObject),
                // invalidating MuPDF's cached page structures.
                page.reset();  // Release before modification
                refreshImageForScannedPage(pageIndex, newText, spanInfo.boundingBox,
                                            spanInfo.formattingRuns,
                                            spanInfo.fontName, spanInfo.fontSize);

                if (isMerge) {
                    // Merged edit: keep original XObject ref, update redo params
                    imageEditStack_[imageEditPos_].spanBbox = spanInfo.boundingBox;
                    imageEditStack_[imageEditPos_].editedText = newText;
                    imageEditStack_[imageEditPos_].formattingRuns = spanInfo.formattingRuns;
                    imageEditStack_[imageEditPos_].fontName = spanInfo.fontName;
                    imageEditStack_[imageEditPos_].fontSize = spanInfo.fontSize;
                } else {
                    // New edit: truncate any redo entries (release their refs) and push
                    for (int i = imageEditPos_ + 1;
                         i < static_cast<int>(imageEditStack_.size()); ++i) {
                        if (imageEditStack_[i].savedXObjectRef && pdfContentWriter_) {
                            pdfContentWriter_->releaseImageXObjectRef(
                                imageEditStack_[i].savedXObjectRef);
                        }
                    }
                    imageEditStack_.resize(imageEditPos_ + 1);

                    ImageEditEntry entry;
                    entry.undoDepth = depth;
                    entry.pageIndex = pageIndex;
                    entry.spanBbox = spanInfo.boundingBox;
                    entry.savedXObjectRef = xobjRef;
                    entry.xobjectName = xobjName;
                    entry.editedText = newText;
                    entry.formattingRuns = spanInfo.formattingRuns;
                    entry.fontName = spanInfo.fontName;
                    entry.fontSize = spanInfo.fontSize;
                    imageEditStack_.push_back(std::move(entry));
                    imageEditPos_ = static_cast<int>(imageEditStack_.size()) - 1;

                    // Enforce memory bound (release refs from evicted entries)
                    while (static_cast<int>(imageEditStack_.size()) > kMaxImageEdits) {
                        if (imageEditStack_[0].savedXObjectRef && pdfContentWriter_) {
                            pdfContentWriter_->releaseImageXObjectRef(
                                imageEditStack_[0].savedXObjectRef);
                        }
                        imageEditStack_.erase(imageEditStack_.begin());
                        imageEditPos_--;
                    }
                }
            } else {
                // GitHub #218: a visible-text span is a legitimate outcome, not
                // a pending one. Publishing it terminally is what stops a
                // waiter blocking its full timeout and then reporting a harness
                // problem for a product decision it could have read directly.
                writeRecompositeStatus("RECOMPOSITE_SKIPPED", pageIndex,
                                       QStringLiteral("span-not-ocr-invisible"));
            }

            if (canvas_) {
                canvas_->refreshPage(pageIndex);
            }
        };

        // UI-TE-UNDO-07: route formatting changes through ApplyFormattingCommand
        // so Ctrl+Z reverses bold/italic/font-family/font-size/underline/
        // strikethrough/text-color toggles. Without this, applyFormattingField
        // mutated the model directly with no undo record.
        textTool->formattingApplied = [this](
            pdfe::textedit::TextEditModel* model,
            int spanIndex,
            std::vector<pdfe::textedit::FormattingRun> oldRuns,
            std::vector<pdfe::textedit::FormattingRun> newRuns,
            std::string description) {
            if (!model) return;
            auto cmd = std::make_unique<pdfe::textedit::ApplyFormattingCommand>(
                model, spanIndex,
                std::move(oldRuns), std::move(newRuns),
                std::move(description));
            undoManager_->execute(std::move(cmd));
            updateWindowTitle();
        };

        // Connect toolbar font info signal
        connect(textTool.get(), &pdfe::textedit::TextEditTool::activeSpanChanged,
                textEditToolBar_, &pdfe::ui::TextEditToolBar::setFontInfo);
        connect(textTool.get(), &pdfe::textedit::TextEditTool::editingStateChanged,
                this, [this](bool editing) {
            if (!editing) {
                textEditToolBar_->clearFontInfo();
            }
            // Suspend the page-level Delete-key shortcut while text editing
            // is active so the Delete key forward-deletes characters at the
            // cursor instead of triggering "Delete Selected Pages". The
            // QAction's shortcut fires window-wide before keyPressEvent
            // reaches the canvas, so accepting the event in the canvas
            // isn't enough — we have to disable the action itself. Save
            // the previous enabled state and restore it when editing ends
            // so the user's selection-driven enable from PagePanel is
            // preserved across edits.
            // Backspace doesn't conflict because no QAction binds it.
            if (deletePageAction_) {
                if (editing) {
                    deletePageActionWasEnabled_ = deletePageAction_->isEnabled();
                    deletePageAction_->setEnabled(false);
                } else {
                    deletePageAction_->setEnabled(deletePageActionWasEnabled_);
                }
            }
        });

        // Toolbar → TextEditTool formatting connections
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::fontFamilyChanged,
                textTool.get(), &pdfe::textedit::TextEditTool::applyFontFamily);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::fontSizeChanged,
                textTool.get(), &pdfe::textedit::TextEditTool::applyFontSize);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::boldToggled,
                textTool.get(), &pdfe::textedit::TextEditTool::applyBold);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::italicToggled,
                textTool.get(), &pdfe::textedit::TextEditTool::applyItalic);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::underlineToggled,
                textTool.get(), &pdfe::textedit::TextEditTool::applyUnderline);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::strikethroughToggled,
                textTool.get(), &pdfe::textedit::TextEditTool::applyStrikethrough);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::textColorChanged,
                textTool.get(), &pdfe::textedit::TextEditTool::applyTextColor);
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::alignmentChanged,
                textTool.get(), &pdfe::textedit::TextEditTool::applyAlignment);
        // Also route the change into AlignmentPipeline so the chosen
        // alignment survives the next save. TextEditTool's own slot
        // only handles live repaint; persistence goes through the
        // pipeline and flushes at save time.
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::alignmentChanged,
                this, &MainWindow::queueAlignmentForActiveSpan);

        // TextEditTool → Toolbar state sync
        connect(textTool.get(), &pdfe::textedit::TextEditTool::selectionFormattingChanged,
                textEditToolBar_, &pdfe::ui::TextEditToolBar::setSelectionFormatting);

        // Atlas mode toolbar connections
        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::atlasModeChanged,
                this, [this](int mode) {
            auto newMode = (mode == 0) ? pdfe::tre::AtlasMode::ScanCropped
                                       : pdfe::tre::AtlasMode::FreeTypeStyled;
            if (newMode == currentAtlasMode_)
                return;
            applyAtlasMode(newMode);
            qDebug() << "[TextEdit] Atlas mode changed to:"
                     << (mode == 0 ? "ScanCropped" : "FreeTypeStyled");
        });

        connect(textEditToolBar_, &pdfe::ui::TextEditToolBar::compareAtlasRequested,
                this, [this]() { showAtlasComparison(); });

        // Context menu on right-click during text edit
        // (UI-CTX-02 + UI-CTX-11). buildTextEditContextMenu() wires
        // each action's triggered signal directly to the appropriate
        // tool/MainWindow slot, so menu.exec() returns the chosen
        // QAction with its handler already invoked — no post-exec
        // dispatch needed. Tests can also call action->trigger()
        // directly to exercise the same wiring.
        connect(textTool.get(), &pdfe::textedit::TextEditTool::contextMenuRequested,
                this, [this, rawTool = textTool.get()](QPoint screenPos) {
            QMenu menu(this);
            buildTextEditContextMenu(menu, rawTool);
            menu.exec(screenPos);
        });

        // Per-line atlas build: on every span click, determine which HOCR line
        // the span belongs to. If it's a different line than the current atlas
        // reference, rebuild the atlas with the new line as reference. This ensures
        // each line gets glyphs harvested from its own font/size context.
        connect(textTool.get(), &pdfe::textedit::TextEditTool::spanActivated,
                this, [this, rawTool = textTool.get()](QRectF pdfBbox) {
            if (pendingAtlas_.allLines.empty())
                return;

            writeUiStatus("SPAN_CLICKED",
                          QString("%1,%2,%3,%4")
                              .arg(pdfBbox.left(), 0, 'f', 1)
                              .arg(pdfBbox.top(), 0, 'f', 1)
                              .arg(pdfBbox.width(), 0, 'f', 1)
                              .arg(pdfBbox.height(), 0, 'f', 1));

            // Map clicked span's PDF bbox to the nearest HOCR line
            // by finding the line with maximum vertical overlap.
            double scale = pendingAtlas_.scanDpi / 72.0;
            double py0 = pdfBbox.top() * scale;
            double py1 = pdfBbox.bottom() * scale;

            int bestIdx = 0;
            double bestOverlap = -1.0;
            for (int i = 0; i < static_cast<int>(pendingAtlas_.allLines.size()); ++i) {
                double oy0 = std::max(py0, pendingAtlas_.allLines[i].bbox.y0);
                double oy1 = std::min(py1, pendingAtlas_.allLines[i].bbox.y1);
                double overlap = std::max(0.0, oy1 - oy0);
                if (overlap > bestOverlap) {
                    bestOverlap = overlap;
                    bestIdx = i;
                }
            }

            // Per-line font size for the CLICKED line (pipeline c4a9e271 Phase 1):
            // a heading line and a body line in the same scanned region get their
            // OWN measured size in the overlay, not the single region average.
            // Falls back to the region fontSizePt when this line has no valid
            // measurement or the vector is empty/out of range. Set unconditionally
            // here so the overlay updates on every click even when CNN ranking is
            // unavailable; the override only affects OCR/image spans (buildOverlayFont
            // ignores it for real embedded fonts).
            const float clickedSizePt = pdfe::tre::FontMatcher::clickedLineSizePt(
                pendingAtlas_.perLineFontSizePt, bestIdx, pendingAtlas_.fontSizePt);
            rawTool->setOverlayFontSize(clickedSizePt);

            // Run CNN visual ranking on the clicked line (once per activation).
            // This defers CNN to click-time so we rank the actual clicked line,
            // not an algorithmically chosen "best" line.
            if (!pendingAtlas_.cnnRanked && !pendingAtlas_.cnnCandidates.empty()
                && fontMatcher_ && bestIdx < static_cast<int>(pendingAtlas_.allLines.size())) {
                const auto& clickedLine = pendingAtlas_.allLines[bestIdx];
                auto rankResult = fontMatcher_->visualRankForLineWithCandidates(
                    clickedLine, pendingAtlas_.pageImage, pendingAtlas_.scanDpi,
                    pendingAtlas_.detectedProperties, pendingAtlas_.cnnCandidates);

                // Store full ranked list for comparison dialog
                pendingAtlas_.visualRankedCandidates = rankResult.rankedCandidates;

                // Crop the clicked line image for the comparison dialog
                {
                    const auto& lb = clickedLine.bbox;
                    QRect lineRect(static_cast<int>(lb.x0), static_cast<int>(lb.y0),
                                   static_cast<int>(lb.width()), static_cast<int>(lb.height()));
                    pendingAtlas_.clickedLineImage =
                        pendingAtlas_.pageImage.copy(lineRect);

                    QString lineText;
                    for (const auto& word : clickedLine.words) {
                        if (!lineText.isEmpty()) lineText += QLatin1Char(' ');
                        lineText += QString::fromStdString(word.text);
                    }
                    pendingAtlas_.clickedLineText = lineText;
                }

                if (rankResult.bestMatch) {
                    const auto& visualResult = *rankResult.bestMatch;
                    qDebug() << "[TextEdit] CNN visual ranking chose:"
                             << QString::fromStdString(visualResult.fontFamily)
                             << "confidence=" << visualResult.confidence;
                    pendingAtlas_.fontFilePath = visualResult.fontFilePath;
                    pendingAtlas_.fontFamily = visualResult.fontFamily;
                    pendingAtlas_.fontFaceIndex = visualResult.fontFaceIndex;
                    // Keep pendingAtlas_.fontSizePt as the region representative /
                    // per-line fallback; the visual ranker selects a font FAMILY,
                    // not a size, so do not let it clobber the region size.
                    // Update the overlay font family to the CNN result; the size
                    // stays the clicked line's per-line size set above.
                    rawTool->setOverlayFontFamily(
                        QString::fromStdString(visualResult.fontFamily));
                    rawTool->setOverlayFontSize(clickedSizePt);
                } else {
                    qDebug() << "[TextEdit] CNN visual ranking returned no result,"
                             << "keeping metric-only font:" << QString::fromStdString(pendingAtlas_.fontFamily);
                }
                pendingAtlas_.cnnRanked = true;
                writeUiStatus("CNN_COMPLETE",
                              QString::fromStdString(pendingAtlas_.fontFamily));
            }

            // Always update the clicked line image/text for the comparison
            // dialog, even if CNN ranking already ran for a previous line.
            if (bestIdx < static_cast<int>(pendingAtlas_.allLines.size())) {
                const auto& clickedLine = pendingAtlas_.allLines[bestIdx];
                const auto& lb = clickedLine.bbox;
                QRect lineRect(static_cast<int>(lb.x0), static_cast<int>(lb.y0),
                               static_cast<int>(lb.width()), static_cast<int>(lb.height()));
                pendingAtlas_.clickedLineImage = pendingAtlas_.pageImage.copy(lineRect);
                QString lineText;
                for (const auto& word : clickedLine.words) {
                    if (!lineText.isEmpty()) lineText += QLatin1Char(' ');
                    lineText += QString::fromStdString(word.text);
                }
                pendingAtlas_.clickedLineText = lineText;
            }

            // Skip rebuild if the atlas was already built for this HOCR line
            if (bestIdx == pendingAtlas_.currentRefLine) {
                qDebug() << "[TextEdit] spanActivated: same ref line" << bestIdx
                         << ", keeping existing atlas";
                return;
            }

            qDebug() << "[TextEdit] spanActivated: pdfBbox=" << pdfBbox
                     << "matched HOCR line" << bestIdx
                     << "(prev=" << pendingAtlas_.currentRefLine << ")"
                     << "overlap=" << bestOverlap;

            // Build the atlas with clicked line as reference
            pdfe::tre::AtlasBuilder::Options opts;
            opts.presynthText = pendingAtlas_.pageText;
            opts.fontFilePath = pendingAtlas_.fontFilePath;
            opts.fontFaceIndex = pendingAtlas_.fontFaceIndex;
            opts.clickedLineIdx = bestIdx;

            auto renderPageFn = [this](int idx) -> QImage {
                return ocrPipeline_->renderPage(idx);
            };
            int totalPages = document_->pageCount();

            try {
                auto buildResult = pdfe::tre::AtlasBuilder::build(
                    pendingAtlas_.pageImage, pendingAtlas_.allLines,
                    pendingAtlas_.textColor, pendingAtlas_.bgColor,
                    ocrPipeline_.get(), renderPageFn,
                    pendingAtlas_.pageIdx, totalPages, opts);

                if (buildResult.enrichedAtlas &&
                    buildResult.enrichedAtlas->size() > 0) {
                    // Wire FreeType fallback with scan style transfer
                    pdfe::tre::FontMatch fm;
                    fm.fontFilePath = pendingAtlas_.fontFilePath;
                    fm.fontFamily = pendingAtlas_.fontFamily;
                    fm.fontFaceIndex = pendingAtlas_.fontFaceIndex;
                    // Per-line size of the clicked line drives the atlas/save render,
                    // not the region average (pipeline c4a9e271 Phase 1).
                    fm.sizePt = clickedSizePt;
                    fm.color = pendingAtlas_.textColor;

                    if (bitmapCompositor_) {
                        buildResult.enrichedAtlas->setFreeTypeFallback(
                            bitmapCompositor_.get(), fm, pendingAtlas_.scanDpi);
                    }

                    // GitHub #266 — record the DPI these glyphs were cropped
                    // from. pendingAtlas_.pageImage is a PAGE-WIDE render at
                    // the OCR DPI, so every length in the atlas is in those
                    // pixels. A consumer compositing a region at its own native
                    // DPI (which ImageReplacer has done since defect #2 was
                    // fixed) must rescale, and without this it cannot tell that
                    // it needs to.
                    buildResult.enrichedAtlas->setSourceDpi(pendingAtlas_.scanDpi);

                    // Cache as scan-cropped atlas
                    scanCroppedAtlas_ = buildResult.enrichedAtlas;

                    // Build FreeType-styled atlas (all glyphs from FreeType)
                    if (bitmapCompositor_) {
                        auto ftAtlas = std::make_shared<pdfe::tre::EnrichedAtlas>();
                        ftAtlas->build(pdfe::tre::GlyphAtlas{}, pendingAtlas_.pageText);
                        // setFreeTypeFallback attempts internal calibration from ftAtlas->atlas_
                        // (empty), which is a no-op. calibrateStyleFrom below provides the
                        // real calibration from scan atlas glyphs.
                        ftAtlas->setFreeTypeFallback(
                            bitmapCompositor_.get(), fm, pendingAtlas_.scanDpi);
                        // Copy reference metrics from scan atlas so FreeType renders at correct size
                        ftAtlas->setReferenceMetrics(
                            scanCroppedAtlas_->referenceLineHeight(),
                            scanCroppedAtlas_->referenceBaselineFromBottom());
                        // Calibrate ScanStyleTransfer from real scan atlas
                        ftAtlas->calibrateStyleFrom(scanCroppedAtlas_->atlas());
                        // Pre-populate FreeType atlas with all page text characters
                        // (not just scan atlas codepoints, which only covers the clicked line)
                        std::set<char32_t> allChars;
                        for (char32_t cp : scanCroppedAtlas_->codepoints())
                            allChars.insert(cp);
                        for (char32_t cp : pendingAtlas_.pageText) {
                            if (cp != U' ' && cp != U'\t' && cp != U'\n' && cp != U'\r')
                                allChars.insert(cp);
                        }
                        for (char32_t cp : allChars)
                            (void)ftAtlas->glyph(cp);
                        // GitHub #266 — same pixel space as the scan atlas: its
                        // reference metrics are copied from it above, and its
                        // FreeType glyphs are rendered at the same scanDpi. An
                        // atlas whose glyphs are correct but whose recorded DPI
                        // is missing would be rescaled by 1.0 and silently keep
                        // the defect for whichever atlas the user selects.
                        ftAtlas->setSourceDpi(pendingAtlas_.scanDpi);
                        freeTypeStyledAtlas_ = ftAtlas;
                    }

                    // Show comparison dialog on first span click
                    //
                    // P7-BUG-2 fix (pipeline b3f19d72 phase-4): the modal
                    // AtlasComparisonDialog blocks the synchronous
                    // spanActivated → AtlasBuilder::build chain triggered
                    // by the user's double-click.  Until the user clicks
                    // "Use Scan" or "Use Font Matched", every subsequent
                    // keystroke is routed to the modal dialog (which
                    // ignores most keys, including ordinary letters),
                    // never reaches TextEditTool::onKeyPress, and the
                    // textEdited callback never fires — so on a
                    // scanned-with-OCR fixture, the test types 'Z' but
                    // the ImageReplacer pipeline never runs and the
                    // canvas is unchanged.  Provide a test-only seam
                    // (PDFE_SKIP_ATLAS_DIALOG=1) that auto-accepts the
                    // current/default atlas mode without opening the
                    // dialog.  Production users continue to see the
                    // dialog on first span click as before.  Mirrors
                    // the PDFE_USE_QT_FILE_DIALOG seam in main.cpp.
                    const bool skipAtlasDialog =
                        qgetenv("PDFE_SKIP_ATLAS_DIALOG") == "1";
                    if (!atlasChoiceMade_ && scanCroppedAtlas_ && freeTypeStyledAtlas_ &&
                        !skipAtlasDialog) {
                        AtlasComparisonDialog dlg(
                            scanCroppedAtlas_, freeTypeStyledAtlas_,
                            pendingAtlas_.clickedLineImage,
                            pendingAtlas_.clickedLineText,
                            pendingAtlas_.visualRankedCandidates,
                            bitmapCompositor_.get(),
                            clickedSizePt,
                            pendingAtlas_.scanDpi,
                            currentAtlasMode_, fillMissingPersisted_, this);
                        dlg.setWindowTitle(
                            tr("Atlas Comparison — Font: %1")
                                .arg(QString::fromStdString(pendingAtlas_.fontFamily)));
                        pdfe::ui::testseam::registerDialogIntrospection(&dlg);
                        if (dlg.exec() == QDialog::Accepted) {
                            currentAtlasMode_ = dlg.selectedMode();
                        }
                        // Persist the fillMissing toggle regardless
                        // of accept/reject so the user's UI choice
                        // survives close+reopen within the session.
                        fillMissingPersisted_ = dlg.fillMissingChecked();
                        atlasChoiceMade_ = true;
                        textEditToolBar_->showAtlasControls(true);
                    } else if (!atlasChoiceMade_ && skipAtlasDialog) {
                        // Test-mode bypass: lock in current default mode
                        // and surface the atlas-mode toolbar so subsequent
                        // mode toggles still work without the dialog.
                        atlasChoiceMade_ = true;
                        textEditToolBar_->showAtlasControls(true);
                        qDebug() << "[TextEdit] AtlasComparisonDialog bypassed"
                                 << "(PDFE_SKIP_ATLAS_DIALOG=1); using mode="
                                 << (currentAtlasMode_ == pdfe::tre::AtlasMode::ScanCropped
                                         ? "ScanCropped" : "FreeTypeStyled");
                    } else if (atlasChoiceMade_) {
                        // Re-entry into edit mode within the same
                        // document — modal already shown earlier; just
                        // restore the atlas-mode toolbar visibility so
                        // the persisted currentAtlasMode_ is editable.
                        textEditToolBar_->showAtlasControls(true);
                    }

                    // Apply selected atlas (uses applyAtlasMode for repaint)
                    applyAtlasMode(currentAtlasMode_);
                    pendingAtlas_.currentRefLine = bestIdx;

                    qDebug() << "[TextEdit] EnrichedAtlas built:"
                             << "atlas=" << buildResult.atlasGlyphCount
                             << "synth=" << buildResult.synthesizedGlyphCount
                             << "donors=" << buildResult.samePageDonorLines
                             << "rejected=" << buildResult.samePageRejectedLines
                             << "crossPage=" << buildResult.crossPageDonorLines
                             << "refLine=" << bestIdx
                             << "mode=" << (currentAtlasMode_ == pdfe::tre::AtlasMode::ScanCropped
                                            ? "ScanCropped" : "FreeTypeStyled");
                    writeUiStatus("ATLAS_READY",
                                  QString("%1|%2")
                                      .arg(bestIdx)
                                      .arg(QString::fromStdString(pendingAtlas_.fontFamily)));
                }
            } catch (const std::exception& e) {
                qDebug() << "[TextEdit] Atlas build failed:" << e.what();
            }
        });

        // For image-dominated pages, use FontMatcher to determine the
        // visually correct overlay font. The PDF font name on OCR pages is
        // meaningless (e.g. "GlyphLessFont", "Liberation Serif") — the actual
        // visual font on the scanned image is what matters.
        {
            int pageIdx = canvas_->currentPage();
            auto pg = document_->page(pageIdx);
            const auto pageClass = pdfe::fonts::VectorSubsetDetector::classify(
                pg.get(), document_.get(), muPdfContext_.get());
            const bool isImgDom =
                (pageClass == pdfe::fonts::PageFontClass::ImageDominated);
            qDebug() << "[TextEdit] Tool activation: page=" << pageIdx
                     << "isImageDominated=" << isImgDom
                     << "pageClass="
                     << (pageClass == pdfe::fonts::PageFontClass::ImageDominated
                             ? "ImageDominated"
                             : pageClass == pdfe::fonts::PageFontClass::VectorSubsetted
                                   ? "VectorSubsetted"
                                   : "VectorComplete");
            if (isImgDom) {
                bool fontOverrideSet = false;
                initTreComponents();
                QString failReason;
                if (!fontMatcher_ || !ocrPipeline_) {
                    failReason = tr("TRE components failed to initialize");
                } else {
                    try {
                        std::string hocrXml = ocrPipeline_->ocrPage(pageIdx);
                        if (hocrXml.empty()) {
                            failReason = tr("OCR returned empty result");
                        } else {
                            auto hocrDoc = pdfe::tre::HocrParser::parse(hocrXml);
                            if (hocrDoc.blocks.empty()) {
                                failReason = tr("OCR found no text blocks");
                            } else {
                                // Build RegionTextData from the full page OCR
                                pdfe::tre::RegionTextData regionData;
                                std::string plainText;
                                for (const auto& block : hocrDoc.blocks) {
                                    for (const auto& para : block.paragraphs) {
                                        for (const auto& line : para.lines) {
                                            regionData.lines.push_back(line);
                                            for (const auto& word : line.words) {
                                                if (!plainText.empty()) plainText += ' ';
                                                plainText += word.text;
                                            }
                                        }
                                    }
                                }
                                regionData.plainText = plainText;
                                // Compute pixelBbox as union of all line bboxes
                                if (!regionData.lines.empty()) {
                                    auto& pb = regionData.pixelBbox;
                                    pb = regionData.lines[0].bbox;
                                    for (size_t li = 1; li < regionData.lines.size(); ++li) {
                                        const auto& lb = regionData.lines[li].bbox;
                                        pb.x0 = std::min(pb.x0, lb.x0);
                                        pb.y0 = std::min(pb.y0, lb.y0);
                                        pb.x1 = std::max(pb.x1, lb.x1);
                                        pb.y1 = std::max(pb.y1, lb.y1);
                                    }
                                }
                                // Render page image at OCR DPI so pixel coordinates
                                // match HOCR bounding boxes from Tesseract.
                                // pg->render() defaults to 72 DPI (scale=1.0), but
                                // Tesseract runs at ocrDpi (300), so all HOCR bboxes
                                // are in 300 DPI space. Using the OCR pipeline's
                                // renderPage() ensures the page image matches.
                                int scanDpi = ocrPipeline_->config().ocrDpi;
                                QImage pageImage = ocrPipeline_->renderPage(pageIdx);
                                auto fontMatch = fontMatcher_->matchRegion(
                                    regionData, pageImage, scanDpi);

                                // Collect CNN candidates now (fast) for deferred
                                // visual ranking when the user clicks a span.
                                auto cnnCandidates = fontMatcher_->collectCandidates(
                                    fontMatch.detectedProperties,
                                    pdfe::tre::utf8ToU32(regionData.plainText));
                                qDebug() << "[TextEdit] FontMatcher result:"
                                         << "family=" << QString::fromStdString(fontMatch.fontFamily)
                                         << "sizePt=" << fontMatch.sizePt
                                         << "fontFilePath=" << QString::fromStdString(fontMatch.fontFilePath)
                                         << "lines=" << regionData.lines.size()
                                         << "cnnCandidates=" << cnnCandidates.size();
                                if (!fontMatch.fontFamily.empty()) {
                                    textTool->setOverlayFontFamily(
                                        QString::fromStdString(fontMatch.fontFamily));
                                    textTool->setOverlayFontSize(fontMatch.sizePt);
                                    fontOverrideSet = true;

                                    // Detect per-word styles and wire to overlay
                                    auto styledLines = regionData.lines;
                                    pdfe::tre::FontMatcher::detectWordStyles(
                                        pageImage, styledLines);
                                    std::vector<pdfe::textedit::TextEditTool::OverlayWordStyle> wordStyles;
                                    int charOffset = 0;
                                    for (const auto& ln : styledLines) {
                                        for (const auto& w : ln.words) {
                                            // Use UTF-32 codepoint count, not UTF-8 byte count
                                            int wLen = static_cast<int>(pdfe::tre::utf8ToU32(w.text).size());
                                            if (w.style.isBold || w.style.isItalic ||
                                                w.style.isUnderlined || w.style.isStrikethrough) {
                                                pdfe::textedit::TextEditTool::OverlayWordStyle ows;
                                                ows.charStart = charOffset;
                                                ows.charEnd = charOffset + wLen;
                                                ows.style = w.style;
                                                wordStyles.push_back(ows);
                                            }
                                            charOffset += wLen + 1; // +1 for space
                                        }
                                    }
                                    if (!wordStyles.empty()) {
                                        textTool->setOverlayWordStyles(
                                            std::move(wordStyles));
                                    }

                                    // Defer atlas building until the user clicks
                                    // a span. Store setup data so the atlas can be
                                    // built with the clicked line as reference.
                                    try {
                                        std::string charHocr = ocrPipeline_->ocrPageWithCharBoxes(pageIdx);
                                        if (!charHocr.empty()) {
                                            auto charDoc = pdfe::tre::HocrParser::parse(charHocr);
                                            std::vector<pdfe::tre::HocrLine> allLines;
                                            for (const auto& blk : charDoc.blocks)
                                                for (const auto& par : blk.paragraphs)
                                                    for (const auto& ln : par.lines)
                                                        allLines.push_back(ln);

                                            pdfe::tre::RgbColor bgColor{255, 255, 255};
                                            if (bgColorDetector_ && !allLines.empty()) {
                                                pdfe::tre::PixelRect fullPage;
                                                fullPage.x0 = 0; fullPage.y0 = 0;
                                                fullPage.x1 = pageImage.width();
                                                fullPage.y1 = pageImage.height();
                                                auto bgInfo = bgColorDetector_->detect(
                                                    pageImage, fullPage, allLines);
                                                bgColor = bgInfo.dominantColor;
                                            }

                                            // Collect page text for pre-synthesis
                                            std::u32string pageText;
                                            for (const auto& ln : allLines)
                                                for (const auto& w : ln.words)
                                                    for (const auto& ch : w.chars)
                                                        if (ch.codepoint != 0)
                                                            pageText += ch.codepoint;

                                            // Per-line font size aligned to allLines (the
                                            // lines a span click resolves against), using the
                                            // SAME shared formula FontMatcher applies to the
                                            // word-level region lines. detectedProperties'
                                            // perLineFontSizePt is NOT index-compatible here
                                            // (allLines is a separate char-box OCR pass), so we
                                            // recompute over allLines to keep bestIdx valid.
                                            const pdfe::fonts::FontScript lineScript =
                                                pdfe::tre::FontMatcher::detectScript(pageText);
                                            std::vector<float> perLineSizes;
                                            perLineSizes.reserve(allLines.size());
                                            for (const auto& ln : allLines) {
                                                const auto [lnPt, lnValid] =
                                                    pdfe::tre::FontMatcher::lineFontSizePt(
                                                        ln, lineScript, scanDpi);
                                                perLineSizes.push_back(lnValid ? lnPt : 0.0f);
                                            }

                                            // Pass HOCR line bboxes in PDF coords now
                                            // (needed for overlay Y positioning even
                                            // before atlas is built).
                                            float pixToPdf = 72.0f / static_cast<float>(scanDpi);
                                            std::vector<QRectF> lineBboxesPdf;
                                            lineBboxesPdf.reserve(allLines.size());
                                            for (const auto& ln : allLines) {
                                                lineBboxesPdf.emplace_back(
                                                    ln.bbox.x0 * pixToPdf,
                                                    ln.bbox.y0 * pixToPdf,
                                                    (ln.bbox.x1 - ln.bbox.x0) * pixToPdf,
                                                    (ln.bbox.y1 - ln.bbox.y0) * pixToPdf);
                                            }
                                            textTool->setOcrLineBboxesPdf(std::move(lineBboxesPdf));

                                            // Store state for deferred atlas build
                                            pendingAtlas_.allLines = std::move(allLines);
                                            pendingAtlas_.pageImage = pageImage;
                                            pendingAtlas_.bgColor = bgColor;
                                            pendingAtlas_.pageText = std::move(pageText);
                                            pendingAtlas_.fontFilePath = fontMatch.fontFilePath;
                                            pendingAtlas_.fontFamily = fontMatch.fontFamily;
                                            pendingAtlas_.fontFaceIndex = fontMatch.fontFaceIndex;
                                            pendingAtlas_.fontSizePt = fontMatch.sizePt;
                                            pendingAtlas_.textColor = fontMatch.color;
                                            pendingAtlas_.pageIdx = pageIdx;
                                            pendingAtlas_.scanDpi = scanDpi;
                                            pendingAtlas_.cnnCandidates = std::move(cnnCandidates);
                                            pendingAtlas_.detectedProperties = fontMatch.detectedProperties;
                                            pendingAtlas_.perLineFontSizePt = std::move(perLineSizes);
                                            pendingAtlas_.cnnRanked = false;
                                            pendingAtlas_.ready = true;

                                            qDebug() << "[TextEdit] Atlas build deferred:"
                                                     << pendingAtlas_.allLines.size() << "lines,"
                                                     << "waiting for span click";
                                        }
                                    } catch (const std::exception& e) {
                                        qDebug() << "[TextEdit] Atlas setup failed:" << e.what();
                                    }
                                } else {
                                    failReason = tr("FontMatcher returned empty family");
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        failReason = tr("OCR/FontMatcher exception: %1")
                            .arg(QString::fromUtf8(e.what()));
                        qWarning("FontMatcher OCR failed for page %d: %s",
                                 pageIdx, e.what());
                    } catch (...) {
                        failReason = tr("Unknown OCR/FontMatcher exception");
                        qWarning("FontMatcher OCR failed for page %d (unknown error)",
                                 pageIdx);
                    }
                }
                if (!fontOverrideSet) {
                    qDebug() << "[TextEdit] FontMatcher FAILED, using Arial 12pt fallback."
                             << "Reason:" << failReason;
                    textTool->setOverlayFontFamily(QStringLiteral("Arial"));
                    textTool->setOverlayFontSize(12.0f);  // Reasonable default for OCR pages
                    logDefaultUsed(
                        tr("Image page font"),
                        tr("FontMatcher detection"),
                        QStringLiteral("Arial 12pt"),
                        tr("page %1: %2").arg(pageIdx).arg(failReason));
                }
            }
        }

        qDebug() << "[TextEdit] Tool setup complete. Installing on canvas.";
        writeUiStatus("TEXT_EDIT_READY", QString::number(pendingAtlas_.allLines.size()));
        textEditToolBar_->setVisible(true);
        formToolBar_->setVisible(false);
        canvas_->setTextEditTool(std::move(textTool));
        return; // Don't go through the normal annotation tool path
    } else if (action == imageToolAction_) {
        // Image tool -- dispatch to onImageToolSelected
        onImageToolSelected();
        return;
    } else if (action == formToolAction_) {
        auto formTool = std::make_unique<pdfe::forms::FormTool>(
            formManager_.get(), jsEngine_.get(), undoManager_);

        // Keep a raw pointer for lambda captures before moving
        auto* formToolRaw = formTool.get();

        formTool->fieldSelected = [this](int widgetIndex) {
            onFormFieldSelected(widgetIndex);
        };
        formTool->fieldDeselected = [this]() {
            onFormFieldDeselected();
        };
        formTool->fieldValueChanged = [this](int pageIndex, pdfe::forms::FormFieldData data) {
            formPropertiesPanel_->setFormField(data);
            updateWindowTitle();
            if (canvas_) {
                canvas_->refreshPage(pageIndex);
            }
        };
        formTool->fieldCreated = [this](int pageIndex, pdfe::forms::FormFieldType type, QRectF rect) {
            createFormField(pageIndex, type, rect);
        };

        // Show the form toolbar. The field-type actions are connected ONCE in
        // setupToolBars() and resolve the live tool through canvas_ at trigger
        // time — see the GitHub #243 note there. Connecting them here, per
        // activation, is what left freed FormTools wired to live QActions.
        formToolBar_->setVisible(true);

        // Default to text field creation. This one is safe to call directly:
        // formToolRaw is the tool we allocated three lines ago and it is still
        // alive right here. It is a call, not a stored pointer.
        formToolRaw->setCreationFieldType(pdfe::forms::FormFieldType::Text);

        tool = std::move(formTool);
    } else if (action == inkToolAction_) {
        tool = std::make_unique<pdfe::annotations::InkTool>(annotationManager_.get());
    } else if (action == freeTextToolAction_) {
        tool = std::make_unique<pdfe::annotations::FreeTextTool>(annotationManager_.get());
    } else if (action == rectangleToolAction_) {
        tool = std::make_unique<pdfe::annotations::ShapeTool>(
            annotationManager_.get(),
            pdfe::annotations::AnnotationType::Square,
            currentAnnotColor_);
    } else if (action == circleToolAction_) {
        tool = std::make_unique<pdfe::annotations::ShapeTool>(
            annotationManager_.get(),
            pdfe::annotations::AnnotationType::Circle,
            currentAnnotColor_);
    } else if (action == lineToolAction_) {
        tool = std::make_unique<pdfe::annotations::ShapeTool>(
            annotationManager_.get(),
            pdfe::annotations::AnnotationType::Line,
            currentAnnotColor_);
    } else if (action == redactToolAction_) {
        auto redactTool = std::make_unique<pdfe::redaction::RedactionTool>(
            redactionManager_.get());
        redactTool->redactionCreated = [this](int pageIndex, QRectF rect) {
            if (!document_ || !redactionManager_) return;
            auto page = document_->page(pageIndex);
            if (!page) return;
            auto* pagePtr = page.get();
            commandPages_.push_back(std::move(page));

            auto cmd = std::make_unique<pdfe::redaction::CreateRedactionCommand>(
                redactionManager_.get(), pagePtr, rect);
            undoManager_->execute(std::move(cmd));

            // Refresh redaction panel and enable apply action
            auto marks = redactionManager_->getAllRedactionMarks(document_.get());
            if (redactionPanel_) redactionPanel_->setMarks(marks);
            applyRedactionsAction_->setEnabled(!marks.empty());

            if (canvas_) canvas_->refreshPage(pageIndex);
            updateWindowTitle();
        };
        tool = std::move(redactTool);
    }

    if (tool) {
        // Set the current page for the tool (keep unique_ptr alive)
        if (document_ && canvas_) {
            int currentPageIndex = canvas_->currentPage();
            if (currentPageIndex >= 0 && currentPageIndex < document_->pageCount()) {
                toolPage_ = document_->page(currentPageIndex);
                auto* page = toolPage_.get();

                // Set page for different tool types
                if (auto* stickyTool = dynamic_cast<pdfe::annotations::StickyNoteTool*>(tool.get())) {
                    stickyTool->setPage(page);
                } else if (auto* markupTool = dynamic_cast<pdfe::annotations::TextMarkupTool*>(tool.get())) {
                    markupTool->setPage(page);
                } else if (auto* inkTool = dynamic_cast<pdfe::annotations::InkTool*>(tool.get())) {
                    inkTool->setPage(page);
                } else if (auto* shapeTool = dynamic_cast<pdfe::annotations::ShapeTool*>(tool.get())) {
                    shapeTool->setPage(page);
                } else if (auto* textTool = dynamic_cast<pdfe::annotations::FreeTextTool*>(tool.get())) {
                    textTool->setPage(page);
                } else if (auto* formTool = dynamic_cast<pdfe::forms::FormTool*>(tool.get())) {
                    formTool->setPage(page);
                } else if (auto* redactTool = dynamic_cast<pdfe::redaction::RedactionTool*>(tool.get())) {
                    redactTool->setPage(page);
                } else if (auto* selectTool = dynamic_cast<pdfe::annotations::SelectTool*>(tool.get())) {
                    selectTool->setPage(page);
                }
            }
        }

        // Don't override color for text markup tools (they use their own defaults:
        // Highlight=yellow, Underline=blue, StrikeOut=red) or FreeText tools
        // (FreeText uses black text on white background set in FreeTextTool)
        if (!dynamic_cast<pdfe::annotations::TextMarkupTool*>(tool.get()) &&
            !dynamic_cast<pdfe::annotations::FreeTextTool*>(tool.get())) {
            tool->setColor(currentAnnotColor_);
        }
        canvas_->setCurrentTool(std::move(tool));
    }
}

void MainWindow::onImportImageStamp()
{
    if (!imageStampLibrary_) return;

    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Import Image As Stamp"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp);;All Files (*)"));
    if (filePath.isEmpty()) return;

    // Default the stamp name to the file's base name; user can override.
    QString defaultName = QFileInfo(filePath).completeBaseName();
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Stamp Name"),
        tr("Name for this stamp:"), QLineEdit::Normal, defaultName, &ok);
    if (!ok || name.isEmpty()) return;

    if (!imageStampLibrary_->importImage(name, filePath)) {
        QMessageBox::warning(this, tr("Import Image As Stamp"),
            tr("Failed to load image: %1").arg(filePath));
        return;
    }
    imageStampLibrary_->saveToSettings();
    QMessageBox::information(this, tr("Import Image As Stamp"),
        tr("Imported stamp '%1'. Use the Stamp tool to place it.")
            .arg(name));
}

void MainWindow::onAnnotationColorPicker()
{
    QColor newColor = QColorDialog::getColor(currentAnnotColor_, this,
                                              tr("Select Annotation Color"));

    if (newColor.isValid() && newColor != currentAnnotColor_) {
        currentAnnotColor_ = newColor;

        // Update button style
        colorPickerButton_->setStyleSheet(
            QString("QPushButton { background-color: rgba(%1, %2, %3, 255); }")
                .arg(currentAnnotColor_.red())
                .arg(currentAnnotColor_.green())
                .arg(currentAnnotColor_.blue())
        );

        // Update the active tool's color immediately
        if (canvas_) {
            auto* activeTool = canvas_->currentTool();
            if (activeTool) {
                activeTool->setColor(currentAnnotColor_);
            }
        }
    }
}

void MainWindow::onStickyNoteDoubleClicked(int pageIndex, int annotIndex, pdfe::annotations::AnnotationData data)
{
    if (!canvas_ || !stickyNotePopup_ || !document_) return;

    // Compute the icon's screen position
    QPoint pageOrigin;
    int widthPx, heightPx;
    if (!canvas_->getPageScreenRect(pageIndex, pageOrigin, widthPx, heightPx))
        return;

    auto pageInfo = document_->pageInfo(pageIndex);
    float zoom = canvas_->zoom();

    // Convert annotation rect to screen coordinates
    QRectF screenRect(
        pageOrigin.x() + data.rect.left() * zoom,
        pageOrigin.y() + data.rect.top() * zoom,
        data.rect.width() * zoom,
        data.rect.height() * zoom);

    // Anchor popup to bottom-right of the icon.
    // Pipeline b3f19d72 phase-6 (P8-BUG-1): popup is a top-level window
    // so the anchor must be in GLOBAL screen coords. screenRect above is
    // in canvas-local widget coords; map it via the canvas widget.
    QPoint anchorLocal(
        static_cast<int>(screenRect.right()),
        static_cast<int>(screenRect.bottom()));
    QPoint anchor = canvas_->mapToGlobal(anchorLocal);

    stickyNotePopup_->openForAnnotation(data, anchor);

    // Also update the dock panel
    if (annotationProperties_) {
        annotationProperties_->setAnnotation(data);
    }
}

void MainWindow::onStickyNotePopupContentsChanged(pdfe::annotations::AnnotationData data)
{
    // Update the dock panel to stay in sync
    if (annotationProperties_) {
        annotationProperties_->setAnnotation(data);
    }

    // Push through the existing modify command path
    onAnnotationPropertiesChanged(data);
}

void MainWindow::onStickyNotePopupClosed(int pageIndex, int annotIndex)
{
    (void)pageIndex;
    (void)annotIndex;
    // Popup is already hidden; no additional cleanup needed
}

void MainWindow::applyAtlasMode(pdfe::tre::AtlasMode mode)
{
    currentAtlasMode_ = mode;
    atlasChoiceMade_ = true;
    textEditToolBar_->setAtlasMode(
        mode == pdfe::tre::AtlasMode::ScanCropped ? 0 : 1);

    auto activeAtlas = (mode == pdfe::tre::AtlasMode::ScanCropped)
                       ? scanCroppedAtlas_ : freeTypeStyledAtlas_;
    if (activeAtlas) {
        auto* tool = canvas_->textEditTool();
        if (tool) {
            tool->setOverlayAtlas(activeAtlas);
            emit tool->repaintRequested();
        }
        currentEnrichedAtlas_ = activeAtlas;
    }
}

void MainWindow::showAtlasComparison()
{
    if (!scanCroppedAtlas_ && !freeTypeStyledAtlas_)
        return;
    AtlasComparisonDialog dlg(
        scanCroppedAtlas_, freeTypeStyledAtlas_,
        pendingAtlas_.clickedLineImage, pendingAtlas_.clickedLineText,
        pendingAtlas_.visualRankedCandidates,
        bitmapCompositor_.get(),
        pendingAtlas_.fontSizePt,
        pendingAtlas_.scanDpi,
        currentAtlasMode_, fillMissingPersisted_, this);
    pdfe::ui::testseam::registerDialogIntrospection(&dlg);
    if (dlg.exec() == QDialog::Accepted) {
        applyAtlasMode(dlg.selectedMode());
    }
    // Persist the fillMissing toggle regardless of accept/reject so
    // the user's UI choice survives close+reopen within the session.
    fillMissingPersisted_ = dlg.fillMissingChecked();
}

void MainWindow::repositionStickyNotePopup()
{
    if (!stickyNotePopup_ || !stickyNotePopup_->isPopupVisible() || !canvas_ || !document_)
        return;

    auto data = stickyNotePopup_->annotationData();
    QPoint pageOrigin;
    int widthPx, heightPx;
    if (!canvas_->getPageScreenRect(data.pageIndex, pageOrigin, widthPx, heightPx)) {
        stickyNotePopup_->closePopup();
        return;
    }

    float zoom = canvas_->zoom();
    QRectF screenRect(
        pageOrigin.x() + data.rect.left() * zoom,
        pageOrigin.y() + data.rect.top() * zoom,
        data.rect.width() * zoom,
        data.rect.height() * zoom);

    // Pipeline b3f19d72 phase-6 (P8-BUG-1): top-level popup needs the
    // anchor in GLOBAL screen coords.
    QPoint anchorLocal(
        static_cast<int>(screenRect.right()),
        static_cast<int>(screenRect.bottom()));
    QPoint anchor = canvas_->mapToGlobal(anchorLocal);
    stickyNotePopup_->updatePosition(anchor);
}

void MainWindow::onToggleStickyNotes(bool checked)
{
    if (!document_) return;
    auto filter = checked
        ? pdfe::core::AnnotationFilter::ShowAll
        : pdfe::core::AnnotationFilter::HideStickyNotes;
    document_->setAnnotationFilter(filter);
    if (canvas_) canvas_->refreshAllPages();
}

bool MainWindow::documentDeniesPrint() const
{
    if (!document_) return false;
    // Live permission is the mandatory floor; the toggle is additive.
    if (!document_->printAllowed()) return true;
    return printPreventionActive_;
}

bool MainWindow::documentDeniesCopy() const
{
    if (!document_) return false;
    if (!document_->copyAllowed()) return true;
    return copyPreventionActive_;
}

void MainWindow::onToggleCopyPrevention(bool checked)
{
    copyPreventionActive_ = checked;
    if (canvas_) canvas_->setCopyPrevented(checked);
    if (canvas_ && canvas_->textEditTool()) {
        canvas_->textEditTool()->setCopyPrevented(checked);
    }
}

void MainWindow::onTogglePrintPrevention(bool checked)
{
    printPreventionActive_ = checked;
    if (printAction_) printAction_->setEnabled(!checked && document_ != nullptr);
    if (printPreviewAction_) printPreviewAction_->setEnabled(!checked && document_ != nullptr);
}

void MainWindow::onToggleOcrBoxes(bool checked)
{
    ocrBoxesEnabled_ = checked;
    if (canvas_) {
        canvas_->setOcrBoxesVisible(checked);
        if (checked && document_) {
            // Ensure TRE/OCR pipeline is initialized
            initTreComponents();
            if (!ocrPipeline_) {
                statusBar()->showMessage(tr("OCR pipeline unavailable"), 5000);
                toggleOcrBoxesAction_->setChecked(false);
                ocrBoxesEnabled_ = false;
                return;
            }
            loadOcrBoxesForPage(canvas_->currentPage());
        }
    }
}

void MainWindow::loadOcrBoxesForPage(int pageIndex)
{
    if (!ocrBoxesEnabled_ || !ocrPipeline_ || !document_ || !canvas_)
        return;

    // Skip if already loaded (includes non-image pages cached as empty)
    if (canvas_->ocrBoxDataForPage(pageIndex))
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);

    auto page = document_->page(pageIndex);
    if (!page || !page->isImageDominated()) {
        canvas_->setOcrBoxData(pageIndex, {});
        QApplication::restoreOverrideCursor();
        return;
    }

    // Set OCR DPI to match pipeline
    canvas_->setOcrDpi(static_cast<float>(ocrPipeline_->config().ocrDpi));

    // Get char-level HOCR and flatten blocks→paragraphs→lines
    std::string hocr = ocrPipeline_->ocrPageWithCharBoxes(pageIndex);
    auto doc = pdfe::tre::HocrParser::parse(hocr);
    std::vector<pdfe::tre::HocrLine> allLines;
    for (auto& block : doc.blocks) {
        for (auto& para : block.paragraphs) {
            for (auto& line : para.lines) {
                allLines.push_back(std::move(line));
            }
        }
    }
    canvas_->setOcrBoxData(pageIndex, std::move(allLines));
    QApplication::restoreOverrideCursor();
}

void MainWindow::onCommentPanelNavigate(int pageIndex, int annotIndex)
{
    if (!canvas_) {
        return;
    }

    // If an annotation index is available, scroll to the annotation's
    // bounding rect so that same-page navigation produces a visible
    // viewport change (not just a goToPage no-op).
    if (annotIndex >= 0 && document_ && annotationManager_) {
        auto page = document_->page(pageIndex);
        if (page) {
            try {
                auto annotData = annotationManager_->getAnnotation(
                    page.get(), annotIndex);
                if (annotData.rect.isValid()) {
                    canvas_->scrollToPdfRect(pageIndex, annotData.rect);
                    return;
                }
            } catch (...) {
                // Fall through to scrollToPdfRect(page top) on any error.
            }
        }
    }

    // No annotation rect available (page-header row or error): scroll to
    // the top of the requested page.  Use scrollToPdfRect with a zero-origin
    // rect so the viewport centers near the page top, which is still a
    // visible change when the canvas is currently scrolled elsewhere.
    canvas_->scrollToPdfRect(pageIndex, QRectF(0.0, 0.0, 1.0, 1.0));
}

void MainWindow::onCommentPanelDelete(int pageIndex, int annotIndex)
{
    if (!document_ || !annotationManager_ || !undoManager_) {
        return;
    }

    auto page = document_->page(pageIndex);
    if (!page) {
        return;
    }

    // Create and execute delete command (keep page alive for undo/redo)
    auto* pagePtr = page.get();
    commandPages_.push_back(std::move(page));

    auto command = std::make_unique<pdfe::annotations::DeleteAnnotationCommand>(
        annotationManager_.get(), pagePtr, annotIndex);

    undoManager_->execute(std::move(command));

    // Refresh comment panel
    if (commentPanel_) {
        commentPanel_->refresh();
    }

    // Invalidate cached tiles and refresh canvas so the deletion is visible
    if (canvas_) {
        canvas_->refreshPage(pageIndex);
    }

    // Refresh the window title dirty marker. Same latent defect as
    // onAnnotationCreatedByTool (see the updateWindowTitle() note there):
    // undoManager_->execute() above makes the stack non-clean, but nothing
    // else in this slot refreshes the title, so a comment-panel/seam-driven
    // annotation delete left the title showing clean until an unrelated
    // action repainted it. (c82a1f37 P4)
    updateWindowTitle();
}

void MainWindow::createFormField(int pageIndex,
                                 pdfe::forms::FormFieldType type,
                                 QRectF rect)
{
    // c82a1f37 P5: extracted from the FormTool::fieldCreated callback so the
    // exact same creation path (CreateFormFieldCommand + undo + refresh +
    // title) is reachable BOTH from a real drag AND from the deterministic
    // testFieldCreatePipe seam. Field placement by real-mouse drag is
    // non-deterministic under headless automation (P4 found a different 2-3
    // of 4 field types committing per run); the seam removes that flakiness.
    if (!document_ || !formManager_ || !undoManager_) return;
    auto page = document_->page(pageIndex);
    if (!page) return;

    // Generate a unique field name
    static int fieldCounter = 0;
    std::string fieldName = "Field" + std::to_string(++fieldCounter);

    // Field creation runs MuPDF object mutation + appearance generation,
    // which can fz_throw (surfaced as a MuPdfException) — e.g. when the
    // form structure has been disrupted by a prior flatten or after a
    // long mutation sequence. This is invoked from inside the FormTool's Qt
    // mouse-event handler (or the pipe handler), so an escaping exception
    // would unwind through the Qt event loop and std::terminate the process.
    // Guard it like every other MainWindow mutation (surface, don't die).
    try {
        auto cmd = std::make_unique<pdfe::forms::CreateFormFieldCommand>(
            formManager_.get(), page.get(), type, rect, fieldName);
        commandPages_.push_back(std::move(page));
        undoManager_->execute(std::move(cmd));
        // c82a1f37 P5: creating a form field adds a new widget annotation to
        // the page /Annots AND a new entry to the AcroForm /Fields array — a
        // STRUCTURAL addition. The incremental-save path (do_incremental=1)
        // does NOT append the newly-created widget (verified: an incremental
        // Ctrl+S left the saved widget count unchanged, while a full-rewrite
        // saveAs persisted it), and Document::save() reopens the file after
        // writing, so the field was silently lost from the live doc too.
        // Mark the session for a full rewrite — the same guard page ops and
        // rotation use — so the created field actually persists. (The deeper
        // question of why MuPDF's incremental writer drops the new widget is
        // noted for later verification.)
        if (document_) {
            document_->markRequiresFullRewrite();
        }
        if (canvas_) {
            canvas_->refreshPage(pageIndex);
        }
        updateWindowTitle();
    } catch (const std::exception& e) {
        qWarning("Form field creation failed: %s", e.what());
        if (this->statusBar()) {
            this->statusBar()->showMessage(
                tr("Could not create form field: %1")
                    .arg(QString::fromUtf8(e.what())),
                5000);
        }
    } catch (...) {
        qWarning("Form field creation failed (unknown error)");
        if (this->statusBar()) {
            this->statusBar()->showMessage(
                tr("Could not create form field (unknown error)"), 5000);
        }
    }
}

void MainWindow::createAnnotationForTest(int pageIndex,
                                         pdfe::annotations::AnnotationType type,
                                         QRectF rect)
{
    // c82a1f37 test-hardening: run the SAME CreateAnnotationCommand path a tool
    // create takes, driven deterministically by the testAnnotCreatePipe seam.
    // A synthetic canvas click does NOT reliably commit an annotation create
    // (only real-mouse commits, and that is timing-fragile) — the root cause of
    // the flaky test_sticky_note_appears_in_comment_panel (before==after==0). This
    // commits the annotation directly, then refreshes the comment panel + canvas
    // + title so the model-seam oracle (comment-tree row count) grows
    // deterministically. Guarded like every other MainWindow mutation.
    if (!document_ || !annotationManager_ || !undoManager_) return;
    auto page = document_->page(pageIndex);
    if (!page) return;
    try {
        auto cmd = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
            annotationManager_.get(), page.get(), type, rect);
        commandPages_.push_back(std::move(page));
        undoManager_->execute(std::move(cmd));
        if (commentPanel_) {
            commentPanel_->refresh();
        }
        if (canvas_) {
            canvas_->refreshPage(pageIndex);
        }
        updateWindowTitle();
    } catch (const std::exception& e) {
        qWarning("Annotation creation failed: %s", e.what());
    } catch (...) {
        qWarning("Annotation creation failed (unknown error)");
    }
}

void MainWindow::onAnnotationPropertiesChanged(pdfe::annotations::AnnotationData data)
{
    if (!document_ || !annotationManager_ || !undoManager_) {
        return;
    }

    // If the widget's cached data has stale indices (e.g. the widget was
    // populated before the UIA-test click fully propagated the selection),
    // fall back to the authoritative selection tracked by the SelectTool
    // callbacks.  This makes property changes always land on the correct
    // annotation even when AnnotationData.pageIndex was -1 at emit time.
    if (data.pageIndex < 0 && selectedAnnotationPageIndex_ >= 0) {
        data.pageIndex = selectedAnnotationPageIndex_;
    }
    if (data.annotIndex < 0 && selectedAnnotationIndex_ >= 0) {
        data.annotIndex = selectedAnnotationIndex_;
    }

    if (data.pageIndex < 0 || data.annotIndex < 0) {
        return;
    }

    auto page = document_->page(data.pageIndex);
    if (!page) {
        return;
    }

    // Keep page alive for undo/redo
    auto* pagePtr = page.get();
    commandPages_.push_back(std::move(page));

    // Get old annotation data
    auto oldData = annotationManager_->getAnnotation(pagePtr, data.annotIndex);

    // Detect which property changed by comparing old and new data
    using Property = pdfe::annotations::ModifyAnnotationCommand::Property;
    Property changedProperty = Property::Contents; // default

    if (oldData.color != data.color) {
        changedProperty = Property::Color;
    } else if (std::abs(oldData.opacity - data.opacity) > 0.001f) {
        changedProperty = Property::Opacity;
    } else if (oldData.rect != data.rect) {
        changedProperty = Property::Rect;
    } else if (oldData.daFontName != data.daFontName ||
               std::abs(oldData.daFontSize - data.daFontSize) > 0.001f ||
               oldData.daColor != data.daColor) {
        changedProperty = Property::DefaultAppearance;
    } else if (oldData.author != data.author) {
        changedProperty = Property::Author;
    }
    // Contents is the fallback

    auto command = std::make_unique<pdfe::annotations::ModifyAnnotationCommand>(
        annotationManager_.get(), pagePtr, data.annotIndex,
        changedProperty, oldData, data);

    undoManager_->execute(std::move(command));

    // Refresh comment panel
    if (commentPanel_) {
        commentPanel_->refresh();
    }

    // Invalidate cached tiles and refresh canvas so the modification is visible
    if (canvas_) {
        canvas_->refreshPage(data.pageIndex);
    }

    // Refresh the window title dirty marker (same latent defect as
    // onCommentPanelDelete / onAnnotationCreatedByTool). (c82a1f37 P4)
    updateWindowTitle();
}

void MainWindow::onAnnotationCreatedByTool(int pageIndex, pdfe::annotations::AnnotationData data)
{
    if (!document_ || !annotationManager_) {
        return;
    }

    if (pageIndex < 0 || pageIndex >= document_->pageCount()) {
        return;
    }

    // The annotation was already created directly by the tool via AnnotationManager.
    // Delete it, then recreate through the command system for proper undo support.
    auto page = document_->page(pageIndex);
    if (!page) {
        return;
    }

    auto* pagePtr = page.get();

    // Snapshot the fully-configured annotation (with color, quads, etc.)
    //
    // GitHub #269. THIS is the line that has been killing the application.
    // Driving sticky-note creation continuously, PDFE died at roughly the
    // 830th, four runs out of four, with getAnnotation() throwing
    // "Annotation index out of range" from here -- out through Qt's event
    // handler, which Qt's own warning says is not survivable, taking the
    // process and the user's document with it.
    //
    // `data.annotIndex` is the index the TOOL observed on the page IT held;
    // `pagePtr` is a separately resolved Page for the same index. The two
    // views diverge after enough creations and the index is then out of
    // range.
    //
    // WHY THEY DIVERGED IS NOW FIXED, and not here: PdfCanvas binds the tool
    // to the page the event names before dispatching
    // (PdfCanvas::bindToolToPage, GitHub #269). Before that, MainWindow bound
    // a tool ONCE at activation to whatever page was then current and nothing
    // rebound it, so a click on any other page acted on the remembered one.
    // It never reproduced through AnnotationManager alone, which survives 900
    // create-and-read cycles (tests/test_annotation_readback_at_scale.cpp) --
    // the divergence needed the canvas, which is where it now cannot happen.
    // Verified: a run that logged 105 of the warnings below logs none.
    //
    // This guard stays as defence in depth, and the reason is the BOUNDARY
    // rather than the bug: an exception must not cross a Qt event handler,
    // whatever produces one. A damaged page, a revoked object or any future
    // MuPDF error would be just as fatal here, and "that one does not throw in
    // practice" is the argument that was available for this very line right up
    // until it took the process down. A tool-created
    // annotation that cannot be snapshotted cannot be handed to the command
    // system either, so the slot returns: the annotation the tool made stays
    // on the page exactly as the tool made it, without undo support, and the
    // user is told. That is a worse outcome than success and a far better one
    // than losing the document.
    pdfe::annotations::AnnotationData snapshot;
    try {
        snapshot = annotationManager_->getAnnotation(pagePtr, data.annotIndex);
    } catch (const std::exception& e) {
        qWarning() << "[MainWindow] annotation" << data.annotIndex << "on page"
                   << pageIndex << "could not be snapshotted; it stays as the "
                      "tool created it and is not undoable:" << e.what();
        statusBar()->showMessage(
            tr("This annotation could not be registered for undo."), 5000);
        return;
    } catch (...) {
        qWarning() << "[MainWindow] annotation" << data.annotIndex << "on page"
                   << pageIndex << "could not be snapshotted (non-standard "
                      "exception); it stays as the tool created it and is not "
                      "undoable";
        statusBar()->showMessage(
            tr("This annotation could not be registered for undo."), 5000);
        return;
    }

    // Retained only now. `commandPages_` exists so a Command holding a raw
    // Page* outlives this slot; the failure path above constructs no command,
    // so parking a Page there bought nothing and never released it -- 105 of
    // them in the run that measured GitHub #269.
    commandPages_.push_back(std::move(page));

    // Delete the tool-created annotation to return to clean state
    annotationManager_->deleteAnnotation(pagePtr, data.annotIndex);

    // Recreate through the command system (this calls createAnnotation internally)
    auto command = std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
        annotationManager_.get(), pagePtr, snapshot.type, snapshot.rect);
    auto* createCommand = command.get();
    undoManager_->execute(std::move(command));

    // GitHub #170. The index of the annotation just created comes from the
    // command's own identity-derived index — createAnnotation walks
    // pdf_first_annot and matches the pdf_annot POINTER it just made — and
    // NOT from getAnnotations().size() - 1.
    //
    // getAnnotations() deliberately returns MORE entries than the index-based
    // accessors can address. After the pdf_first_annot pass it walks /Annots
    // and appends the /Link entries MuPDF's iterator refuses to surface
    // (pdf_load_annots skips /Link and /Popup on load), numbering them from
    // the iterator count upward. Every index-based accessor —
    // getAnnotation / setColor / setContents / setInkStrokes / setQuadPoints —
    // resolves through getNthAnnot, which walks that same iterator and so
    // cannot reach the appended tail at all.
    //
    // So on ANY page carrying link annotations, size() - 1 pointed into the
    // link-only tail, and the first setColor() below threw "Annotation index
    // out of range" straight out of PdfCanvas::mousePressEvent. Measured on
    // tests/fixtures/test_link_types.pdf (4 links, 0 iterator-visible
    // annotations): the new sticky note is index 0 while size() - 1 is 4.
    // Independent of where the user pressed, which is why #170 reproduced at
    // a point carrying no link.
    //
    // createCommand stays alive across execute(): CreateAnnotationCommand
    // does not override mergeId() (so it is never merged and dropped), and
    // enforceMaxDepth() discards from the FRONT of the undo stack.
    const int newIndex = createCommand->createdAnnotation().annotIndex;
    if (newIndex >= 0) {
        // For FreeText: set DA BEFORE contents so MuPDF can render text
        if (!snapshot.daFontName.empty()) {
            annotationManager_->setDefaultAppearance(pagePtr, newIndex,
                snapshot.daFontName, snapshot.daFontSize, snapshot.daColor);
        }
        annotationManager_->setColor(pagePtr, newIndex, snapshot.color);
        if (!snapshot.contents.empty()) {
            annotationManager_->setContents(pagePtr, newIndex, snapshot.contents);
        }
        if (!snapshot.quadPoints.empty()) {
            annotationManager_->setQuadPoints(pagePtr, newIndex, snapshot.quadPoints);
        }
        if (!snapshot.inkStrokes.empty()) {
            annotationManager_->setInkStrokes(pagePtr, newIndex, snapshot.inkStrokes);
        }
        // Restore line endpoints (for Line annotations)
        if (snapshot.type == pdfe::annotations::AnnotationType::Line &&
            (!snapshot.lineStart.isNull() || !snapshot.lineEnd.isNull())) {
            annotationManager_->setLineEndpoints(pagePtr, newIndex,
                snapshot.lineStart, snapshot.lineEnd);
        }
        if (std::abs(snapshot.opacity - 1.0f) > 0.001f) {
            annotationManager_->setOpacity(pagePtr, newIndex, snapshot.opacity);
        }
    }

    // Refresh comment panel
    if (commentPanel_) {
        commentPanel_->refresh();
    }

    // Invalidate cached tiles and refresh canvas so the new annotation is visible
    if (canvas_) {
        canvas_->refreshPage(pageIndex);
    }

    // Auto-switch to SelectTool and show annotation properties after
    // every annotation creation. Previously restricted to StickyNote
    // and FreeText — extended to all types so the Properties Panel
    // populates immediately, enabling configure operations (set color,
    // set opacity, set contents) without a manual re-select click.
    {
        // Switch to SelectTool
        selectToolAction_->setChecked(true);
        onAnnotationToolSelected(selectToolAction_);

        // Select the new annotation and show properties panel.
        // Same index, same reason as above (GitHub #170): a list POSITION in
        // getAnnotations() is not an addressable annotation index.
        if (newIndex >= 0) {
            auto newPage = document_->page(pageIndex);
            if (newPage) {
                // GitHub #269. This is NOT the read-back that killed the
                // application -- the snapshot at the top of this slot is, and
                // it is guarded there with the measurements. This one has
                // never been observed to throw: the run that logged 105
                // warnings from the snapshot site logged none from here.
                //
                // It is guarded anyway, and the reason is not symmetry. The
                // obligation at a slot boundary is "nothing crosses it",
                // which is a property of the boundary and not of any
                // particular call's observed behaviour. An unguarded throw
                // here would be just as fatal, and the argument for leaving
                // it -- "that one does not throw in practice" -- is the
                // argument that was available for the other site too, right
                // up until it took the process down.
                //
                // Everything below is a CONVENIENCE -- populate a panel,
                // open an editing popup -- and none of it is worth the
                // document. The annotation itself is already created and
                // already on the undo stack before this runs, so degrading
                // here loses a panel refresh, not work.
                //
                // Catching by `...` rather than a specific type is deliberate:
                // MuPdfContext::execute translates fz errors, and the
                // obligation is "nothing crosses the slot boundary", which a
                // typed catch does not discharge.
                pdfe::annotations::AnnotationData newData;
                bool haveData = false;
                try {
                    newData = annotationManager_->getAnnotation(newPage.get(),
                                                                newIndex);
                    haveData = true;
                } catch (const std::exception& e) {
                    qWarning() << "[MainWindow] could not read back annotation"
                               << newIndex << "on page" << pageIndex
                               << "-- properties panel not populated:"
                               << e.what();
                } catch (...) {
                    qWarning() << "[MainWindow] could not read back annotation"
                               << newIndex << "on page" << pageIndex
                               << "-- properties panel not populated "
                                  "(non-standard exception)";
                }

                if (haveData && annotationProperties_) {
                    annotationProperties_->setAnnotation(newData);
                    annotationPropertiesDock_->show();
                    annotationPropertiesDock_->raise();
                    annotationProperties_->focusContents();
                }

                // Auto-open inline StickyNotePopup for immediate editing
                // (only for StickyNote/FreeText — other types don't have
                // an inline editing popup)
                if (haveData &&
                    (snapshot.type == pdfe::annotations::AnnotationType::Text ||
                     snapshot.type == pdfe::annotations::AnnotationType::FreeText)) {
                    onStickyNoteDoubleClicked(pageIndex, newIndex, newData);
                }
            }
        }
    }

    // Refresh the window title dirty marker. Without this call, the
    // undoManager_->execute() above pushes a command onto the stack
    // but the title stays clean because nothing else in this slot
    // calls updateWindowTitle(). Discovered in phase-7/9 when the
    // title dirty marker was being used as a post-annotation oracle.
    updateWindowTitle();
}

// ============================================================================
// Page manipulation slots
// ============================================================================

void MainWindow::onRotatePageCW()
{
    if (!document_ || !undoManager_) {
        return;
    }

    QList<int> indices = pagePanel_->selectedPageIndices();
    if (indices.isEmpty()) {
        indices.append(canvas_->currentPage());
    }

    for (int idx : indices) {
        auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
            document_.get(), idx, 90);
        undoManager_->execute(std::move(cmd));
    }

    onDocumentStructureChanged();
}

void MainWindow::onRotatePageCCW()
{
    if (!document_ || !undoManager_) {
        return;
    }

    QList<int> indices = pagePanel_->selectedPageIndices();
    if (indices.isEmpty()) {
        indices.append(canvas_->currentPage());
    }

    for (int idx : indices) {
        auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
            document_.get(), idx, -90);
        undoManager_->execute(std::move(cmd));
    }

    onDocumentStructureChanged();
}

void MainWindow::onRotatePage180()
{
    if (!document_ || !undoManager_) {
        return;
    }

    QList<int> indices = pagePanel_->selectedPageIndices();
    if (indices.isEmpty()) {
        indices.append(canvas_->currentPage());
    }

    for (int idx : indices) {
        auto cmd = std::make_unique<pdfe::commands::RotatePageCommand>(
            document_.get(), idx, 180);
        undoManager_->execute(std::move(cmd));
    }

    onDocumentStructureChanged();
}

void MainWindow::onDeletePages()
{
    if (!document_ || !undoManager_) {
        return;
    }

    QList<int> indices = pagePanel_->selectedPageIndices();
    if (indices.isEmpty()) {
        indices.append(canvas_->currentPage());
    }

    // Refuse to delete all pages
    if (indices.size() >= document_->pageCount()) {
        QMessageBox::warning(this, tr("Delete Pages"),
            tr("Cannot delete all pages. At least one page must remain."));
        return;
    }

    // Confirm deletion
    auto result = QMessageBox::question(this, tr("Delete Pages"),
        tr("Delete %n page(s)?", "", indices.size()),
        QMessageBox::Yes | QMessageBox::No);

    if (result != QMessageBox::Yes) {
        return;
    }

    // Use DeletePageRangeCommand for multiple pages
    std::vector<int> stdIndices(indices.begin(), indices.end());
    auto cmd = std::make_unique<pdfe::commands::DeletePageRangeCommand>(
        document_.get(), std::move(stdIndices));
    undoManager_->execute(std::move(cmd));

    onDocumentStructureChanged();
}

void MainWindow::onInsertBlankPage()
{
    if (!document_ || !undoManager_) {
        return;
    }

    int insertAt = canvas_->currentPage() + 1;

    // Use the current page's size as default, or Letter if no pages
    float widthPt = 612.0f;
    float heightPt = 792.0f;
    if (document_->pageCount() > 0) {
        int refPage = canvas_->currentPage();
        if (refPage >= 0 && refPage < document_->pageCount()) {
            auto info = document_->pageInfo(refPage);
            widthPt = info.widthPt;
            heightPt = info.heightPt;
        }
    }

    auto cmd = std::make_unique<pdfe::commands::InsertBlankPageCommand>(
        document_.get(), insertAt, widthPt, heightPt);
    undoManager_->execute(std::move(cmd));

    onDocumentStructureChanged();

    // Navigate to the new page
    canvas_->goToPage(insertAt);
}

void MainWindow::onInsertPagesFromFile()
{
    if (!document_ || !undoManager_) {
        return;
    }

    // SKIP=FAIL (GAP-A14): honour a test-injected path before the native
    // open dialog, which cannot be driven via UIA on the Server-2019 CI
    // session. Inert in production (queue only fillable via PDFE_TEST_PIPES).
    QString fileName = pdfe::ui::testseam::takeFileDialogPath();
    if (fileName.isEmpty()) {
        fileName = QFileDialog::getOpenFileName(
            this,
            tr("Insert Pages from File"),
            QString(),
            tr("PDF Files (*.pdf);;All Files (*)")
        );
    }

    if (fileName.isEmpty()) {
        return;
    }

    try {
        std::filesystem::path sourcePath = qStringToPath(fileName);

        // Open source to get page count
        auto sourceDoc = pdfe::core::Document::open(muPdfContext_, sourcePath);
        if (!sourceDoc || sourceDoc->pageCount() == 0) {
            QMessageBox::warning(this, tr("Insert Pages"),
                tr("Could not open or empty source file."));
            return;
        }

        int insertAt = canvas_->currentPage() + 1;
        int sourceStart = 0;
        int sourceEnd = sourceDoc->pageCount() - 1;

        auto cmd = std::make_unique<pdfe::commands::ImportPagesCommand>(
            document_.get(), insertAt, sourcePath, muPdfContext_,
            sourceStart, sourceEnd);
        undoManager_->execute(std::move(cmd));

        onDocumentStructureChanged();

        statusBar_->showMessage(
            tr("Imported %1 page(s) from %2")
                .arg(sourceDoc->pageCount())
                .arg(QFileInfo(fileName).fileName()),
            3000);

    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Import Error"),
            tr("Failed to import pages: %1").arg(e.what()));
    }
}

void MainWindow::onExtractPages()
{
    if (!document_) {
        return;
    }

    QList<int> indices = pagePanel_->selectedPageIndices();
    if (indices.isEmpty()) {
        indices.append(canvas_->currentPage());
    }

    // SKIP=FAIL (GAP-A14): honour a test-injected path before the native
    // save dialog, which cannot be driven via UIA on the Server-2019 CI
    // session. Inert in production (queue only fillable via PDFE_TEST_PIPES).
    QString fileName = pdfe::ui::testseam::takeFileDialogPath();
    if (fileName.isEmpty()) {
        fileName = QFileDialog::getSaveFileName(
            this,
            tr("Extract Pages to File"),
            QString(),
            tr("PDF Files (*.pdf);;All Files (*)")
        );
    }

    if (fileName.isEmpty()) {
        return;
    }

    try {
        std::vector<int> stdIndices(indices.begin(), indices.end());
        std::filesystem::path outputPath = qStringToPath(fileName);

        pdfe::core::extractPagesToFile(muPdfContext_, document_.get(),
                                        stdIndices, outputPath);

        statusBar_->showMessage(
            tr("Extracted %1 page(s) to %2")
                .arg(indices.size())
                .arg(QFileInfo(fileName).fileName()),
            3000);

    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Extract Error"),
            tr("Failed to extract pages: %1").arg(e.what()));
    }
}

void MainWindow::onMovePageUp()
{
    if (!document_ || !canvas_ || !undoManager_) return;
    int from = canvas_->currentPage();
    if (from <= 0) return;  // already at top
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document_.get(), from, from - 1);
    undoManager_->execute(std::move(cmd));
    onDocumentStructureChanged();
    canvas_->goToPage(from - 1);
}

void MainWindow::onMovePageDown()
{
    if (!document_ || !canvas_ || !undoManager_) return;
    int from = canvas_->currentPage();
    if (from < 0 || from >= document_->pageCount() - 1) return;  // at end
    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document_.get(), from, from + 1);
    undoManager_->execute(std::move(cmd));
    onDocumentStructureChanged();
    canvas_->goToPage(from + 1);
}

void MainWindow::onPageMoved(int fromIndex, int toIndex)
{
    if (!document_ || !undoManager_) {
        return;
    }

    if (fromIndex == toIndex) {
        return;
    }

    auto cmd = std::make_unique<pdfe::commands::MovePageCommand>(
        document_.get(), fromIndex, toIndex);
    undoManager_->execute(std::move(cmd));

    onDocumentStructureChanged();

    // Navigate to the moved page's new location
    int actualTo = toIndex;
    if (fromIndex < toIndex) {
        actualTo = toIndex - 1;
    }
    canvas_->goToPage(actualTo);
}

void MainWindow::onDocumentStructureChanged(bool preserveCommandPages)
{
    if (!document_) {
        return;
    }

    // Release stale page pointers. The ordinary File>Save path passes
    // preserveCommandPages=true: saveWithCommandPageRebind() already re-resolved
    // commandPages_ to live post-save pages and rebound the undo commands to
    // them, so clearing here would re-dangle undo. toolPage_ is always dropped
    // (it is not rebound; the active tool is recreatable).
    toolPage_.reset();
    // The full-rewrite Save that reached here recreated the document's Page
    // objects, so the ACTIVE annotation tool's own cached raw Page*
    // (SelectTool::page_, bound via selectByIndex when an annotation is selected)
    // is now dangling — distinct from toolPage_ above. Release it before the
    // canvas repaints below, or onPaintOverlay → getAnnotation(page_) →
    // Page::fzPage derefs the freed page (0xC0000005). #47 / DCA 0008.
    if (canvas_) {
        canvas_->releaseActiveToolPageCache();
    }
    if (!preserveCommandPages) {
        // Journey-op99 UAF fix: a structure change (insert/rotate/delete page)
        // reaches here with preserveCommandPages=false while the undo stack is
        // still live and its commands cache raw Page* into commandPages_.
        // Clearing commandPages_ destroys those Page wrappers; if we do NOT
        // first null out the commands' cached page_, every stack-resident
        // command is left holding a DANGLING (non-null, freed) Page*. The next
        // full-rewrite Save then reads cmd->pageIndex() (SaveWithRebind.h
        // step 1) and dereferences the freed page (0xC0000005 / 0xFEEEFEEE).
        // Null-rebind every command FIRST so page_ is nullptr (a defined
        // "page gone" state each command treats as a safe no-op), THEN free.
        if (undoManager_) {
            undoManager_->forEachCommand(
                [](pdfe::commands::Command* c) { c->rebindPage(nullptr); });
        }
        commandPages_.clear();
    }

    // Refresh cached page count
    document_->refreshPageCount();

    // Invalidate rendering caches
    if (tileCache_) {
        tileCache_->invalidateAll();
    }
    if (tileRenderer_) {
        tileRenderer_->cancelAll();
    }

    // Refresh page panel thumbnails + notify accessibility bridge
    if (pagePanel_) {
        pagePanel_->refreshPages();
        QAccessibleEvent ev(pagePanel_, QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&ev);
    }

    // Clamp current page to valid range
    int maxPage = document_->pageCount() - 1;
    int currentPage = canvas_->currentPage();
    if (currentPage > maxPage) {
        currentPage = maxPage;
    }
    if (currentPage < 0) {
        currentPage = 0;
    }

    // Refresh canvas: re-request tiles for the current view.
    // Note: goToPage() has an early-return when page index is unchanged,
    // so we must explicitly call refreshAllPages() to replenish the cache.
    if (canvas_) {
        canvas_->refreshAllPages();
        canvas_->goToPage(currentPage);
    }

    // Update status bar
    statusBar_->setPageInfo(currentPage + 1, document_->pageCount());

    // Update window title for dirty indicator
    updateWindowTitle();
}

// --- Form slots ---

void MainWindow::onTextEditToolSelected()
{
    if (!document_ || !canvas_) {
        return;
    }

    if (!textEditToolAction_->isChecked()) {
        textEditAutoRouting_ = false;
        return;
    }

    // Set flag to prevent the QActionGroup's triggered signal from
    // overwriting the routing decision. The group signal fires AFTER
    // this slot returns, calling onAnnotationToolSelected(textEditToolAction_)
    // a second time. Without this guard, OCR pages get routed to TRE by
    // activateTextEditForPage, then overwritten with TextEditTool by the group.
    skipGroupTextEditCall_ = true;

    int pageIndex = canvas_->currentPage();
    activateTextEditForPage(pageIndex);
}

void MainWindow::activateTextEditForPage(int pageIndex)
{
    if (!document_ || !canvas_) return;
    if (pageIndex < 0 || pageIndex >= document_->pageCount()) return;

    auto page = document_->page(pageIndex);
    if (!page) return;

    bool hasNativeText = page->hasTextContent();

    // Routing:
    //  - hasNativeText → textEditToolAction_ (TextEditTool, span-click flow).
    //    This handles native text + already-OCR'd scans (whose hidden text
    //    layer makes hasTextContent() true).
    //  - else → treToolAction_ (TreRegionTool, drag-a-region flow). Pure
    //    scans without an embedded text layer go here; users drag a
    //    region to OCR. TextEditTool's span-click flow doesn't work on
    //    those pages because page->extractText() returns no spans.
    //
    // (A 2026-05-06 attempt to route pure-scans to textEditToolAction_
    // ran into this exact constraint: even with the OCR pipeline
    // running and pendingAtlas_.allLines populated, TextEditTool
    // had no spans for the user to click, so spanActivated never
    // fired and the AtlasComparisonDialog never opened. Pre-OCR'd
    // fixtures route correctly to textEditToolAction_ via the
    // hasNativeText branch.)
    if (hasNativeText) {
        onAnnotationToolSelected(textEditToolAction_);
    } else {
        initTreComponents();
        onAnnotationToolSelected(treToolAction_);
    }

    textEditAutoRouting_ = true;
}

void MainWindow::onFormToolSelected()
{
    if (!document_ || !formManager_ || !canvas_) {
        return;
    }

    // The form tool action is part of the annotation tool group, so
    // onAnnotationToolSelected handles the actual tool creation.
    // This slot just ensures the action is checked.
    if (formToolAction_->isChecked()) {
        onAnnotationToolSelected(formToolAction_);
    }
}

void MainWindow::onCreateFormField()
{
    // Activating the Form Tool reveals the field-creation toolbar
    // (Text Field / Checkbox / Combo Box / List Box). Users pick the
    // variant they want and drag on the canvas to place it. The menu
    // action provides discoverability for the same workflow.
    if (!formToolAction_) return;
    formToolAction_->setChecked(true);
    onFormToolSelected();
    if (formToolBar_) {
        formToolBar_->setVisible(true);
    }
}

void MainWindow::onFormFieldProperties()
{
    // Field properties live in the Properties Panel dock's form-field
    // page. Show the dock; actual field props populate when the user
    // selects a field.
    if (propertiesPanelDock_) {
        propertiesPanelDock_->setVisible(true);
        propertiesPanelDock_->raise();
    }
}

void MainWindow::onExportFormData()
{
    if (!document_ || !formManager_ || !formDataIO_) {
        return;
    }

    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Export Form Data"),
        QString(),
        tr("XFDF Files (*.xfdf);;All Files (*)")
    );

    if (fileName.isEmpty()) {
        return;
    }

    try {
        std::string xfdf = formDataIO_->exportToXfdf(document_.get(), formManager_.get());

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(this, tr("Error"),
                tr("Could not open file for writing: %1").arg(fileName));
            return;
        }
        file.write(QByteArray::fromStdString(xfdf));
        file.close();

        statusBar_->showMessage(tr("Form data exported to %1").arg(fileName), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to export form data: %1").arg(e.what()));
    }
}

void MainWindow::onImportFormData()
{
    if (!document_ || !formManager_ || !formDataIO_) {
        return;
    }

    // F15 / PDF-40: FDF is now importable too, so it is offered here and
    // dispatched on the file's own header rather than on its extension.
    QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Import Form Data"),
        QString(),
        tr("Form Data (*.xfdf *.fdf);;XFDF Files (*.xfdf);;"
           "FDF Files (*.fdf);;All Files (*)")
    );

    if (fileName.isEmpty()) {
        return;
    }

    try {
        QFile file(fileName);
        // Binary read: an FDF is PDF syntax, not text, and CRLF translation
        // would corrupt it.
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, tr("Error"),
                tr("Could not open file for reading: %1").arg(fileName));
            return;
        }
        std::string data = file.readAll().toStdString();
        file.close();

        if (data.rfind("%FDF-", 0) == 0) {
            formDataIO_->importFromFdf(document_.get(), formManager_.get(), data);
        } else {
            formDataIO_->importFromXfdf(document_.get(), formManager_.get(), data);
        }

        canvas_->refreshAllPages();
        updateWindowTitle();
        statusBar_->showMessage(tr("Form data imported from %1").arg(fileName), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to import form data: %1").arg(e.what()));
    }
}

void MainWindow::onResetForm()
{
    if (!document_ || !formManager_) {
        return;
    }

    // GAP-A14: under the UI test seam the modal Yes/No confirmation cannot
    // be answered via UIA on the Server-2019 CI session (it blocks the event
    // loop in exec()), so a test that wants to exercise the *reset* behavior
    // pre-authorizes the affirmative path with PDFE_TEST_AUTO_CONFIRM=1. This
    // is gated by both PDFE_TEST_PIPES and PDFE_TEST_AUTO_CONFIRM and is inert
    // in production (neither is ever set in a shipped build).
    const bool autoConfirm =
        pdfe::ui::testseam::testPipesEnabled() &&
        qEnvironmentVariableIsSet("PDFE_TEST_AUTO_CONFIRM");
    if (!autoConfirm) {
        QMessageBox::StandardButton reply = QMessageBox::question(this,
            tr("Reset Form"),
            tr("Reset all form fields to their default values?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    try {
        int currentPageIndex = canvas_ ? canvas_->currentPage() : 0;
        auto page = document_->page(currentPageIndex);
        commandPages_.push_back(std::move(page));
        auto* pagePtr = commandPages_.back().get();

        // Pipeline c91e7f3a phase-5: cancel any in-progress FormTool edit
        // before resetting, so that a pending editBuffer_ does not
        // overwrite the reset values when the user next clicks a field.
        if (auto* ft = dynamic_cast<pdfe::forms::FormTool*>(canvas_->currentTool())) {
            ft->cancelCurrentEdit();
        }

        auto cmd = std::make_unique<pdfe::forms::ResetFormCommand>(
            formManager_.get(), pagePtr);
        undoManager_->execute(std::move(cmd));

        canvas_->refreshPage(currentPageIndex);
        formPropertiesPanel_->clear();
        updateWindowTitle();
        statusBar_->showMessage(tr("Form fields reset"), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to reset form: %1").arg(e.what()));
    }
}

bool MainWindow::confirmFlattenForm()
{
    // GAP-A14, extended to flatten (GitHub #243). The modal Yes/No cannot be
    // answered via UIA on the Server-2019 session — QMessageBox::exec() blocks
    // the event loop — and it blocks a headless GoogleTest outright, which is
    // why the UI flatten path had no unit coverage at all before now. Same
    // gating as onResetForm: both PDFE_TEST_PIPES and PDFE_TEST_AUTO_CONFIRM
    // must be set, so this is inert in a shipped build where neither exists.
    if (pdfe::ui::testseam::testPipesEnabled() &&
        qEnvironmentVariableIsSet("PDFE_TEST_AUTO_CONFIRM")) {
        return true;
    }

    // Virtual so a widget test can answer it without a real dialog, matching
    // the promptSaveOnClose() pattern already used for the close prompt.
    return QMessageBox::warning(this,
               tr("Flatten Form"),
               tr("This will permanently convert form fields into static content.\n"
                  "This action cannot be undone.\n\n"
                  "Continue?"),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) == QMessageBox::Yes;
}

void MainWindow::onFlattenForm()
{
    if (!document_ || !formManager_) {
        return;
    }

    if (!confirmFlattenForm()) {
        return;
    }

    try {
        int currentPageIndex = canvas_ ? canvas_->currentPage() : 0;
        auto page = document_->page(currentPageIndex);

        formManager_->flattenForm(page.get());

        // Clear undo stack since flatten is irreversible
        undoManager_->clear();

        // GitHub #243 — return to the select tool rather than just dropping the
        // current one. A bare clearCurrentTool() here destroyed the FormTool but
        // left formToolAction_ CHECKED, so the window claimed to be in form mode
        // with no form tool behind it. Two consequences: the field-type actions
        // fired at a tool that no longer existed (the crash this issue is
        // about), and activateToolByObjectName() — which early-returns for an
        // already-checked checkable action — could not re-enter form mode
        // afterwards, so a UI test that tried to was a silent no-op.
        //
        // There is nothing left to edit as a form once the fields are flattened
        // into page content, so Select is the honest destination.
        selectToolAction_->setChecked(true);
        onAnnotationToolSelected(selectToolAction_);
        canvas_->refreshPage(currentPageIndex);
        formPropertiesPanel_->clear();
        updateWindowTitle();
        statusBar_->showMessage(tr("Form fields flattened"), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to flatten form: %1").arg(e.what()));
    }
}

void MainWindow::onFormFieldSelected(int widgetIndex)
{
    if (!document_ || !formManager_ || !canvas_) {
        return;
    }

    int currentPageIndex = canvas_->currentPage();
    auto page = document_->page(currentPageIndex);
    if (!page) return;

    auto fieldData = formManager_->getFormField(page.get(), widgetIndex);
    formPropertiesPanel_->setFormField(fieldData);

    // Raise the form properties dock
    if (formPropertiesDock_) {
        formPropertiesDock_->raise();
    }
}

void MainWindow::onFormFieldDeselected()
{
    formPropertiesPanel_->clear();
}

void MainWindow::onFormFieldValueChanged(pdfe::forms::FormFieldData data)
{
    if (!document_ || !formManager_ || !canvas_) {
        return;
    }

    // The panel edited a value; commit it via command for undo support
    int currentPageIndex = canvas_->currentPage();
    auto page = document_->page(currentPageIndex);
    if (!page) return;

    commandPages_.push_back(std::move(page));
    auto* pagePtr = commandPages_.back().get();

    // GitHub #253 -- and this one was a REAL DATA LOSS, not just an unreachable
    // class.
    //
    // `SetChoiceSelectionCommand` takes a std::vector<int> of selected indices
    // and was unreachable from main(). Every choice edit went through
    // `SetFieldValueCommand`, which takes a single value STRING. For a
    // multi-select list box that is lossy by construction:
    // FormFieldPropertiesPanel::onListSelectionChanged() collects every selected
    // row into `selectedIndices` and then sets `value` to the text of the FIRST
    // one, so committing `data.value` silently discarded every selection after
    // the first. The whole vector was carried in the data and then dropped here.
    //
    // Routing choice fields through the command that models a selection fixes
    // that, and makes the undo step restore the full prior selection rather than
    // one item of it.
    //
    // WHICH FIELDS: a list box always (an EMPTY selection is a meaningful edit --
    // the user deselected everything -- and `value` is stale in that case), and a
    // non-editable combo box (exactly one selection, so the two routes agree, but
    // the selection is the authoritative thing). An EDITABLE combo is left on the
    // value path deliberately: its value can be free text that is not in the
    // options list at all, and there is no index to express it with.
    const bool isListBox = (data.type == pdfe::forms::FormFieldType::ListBox);
    const bool isPlainCombo =
        (data.type == pdfe::forms::FormFieldType::ComboBox && !data.isEditable);

    if (isListBox || isPlainCombo) {
        // The old selection comes from the FormManager rather than from the
        // panel: the panel has already been updated to the new state by the time
        // this signal arrives, so asking it would record new-as-old and make
        // undo a no-op.
        const auto before = formManager_->getFormField(pagePtr, data.widgetIndex);
        auto cmd = std::make_unique<pdfe::forms::SetChoiceSelectionCommand>(
            formManager_.get(), pagePtr, data.widgetIndex,
            before.selectedIndices, data.selectedIndices);
        undoManager_->execute(std::move(cmd));
    } else {
        auto cmd = std::make_unique<pdfe::forms::SetFieldValueCommand>(
            formManager_.get(), pagePtr, data.widgetIndex, data.value);
        undoManager_->execute(std::move(cmd));
    }

    // Pipeline c91e7f3a phase-5: sync the FormTool's editBuffer_ so that a
    // subsequent commitEdit() (triggered when the user clicks another field)
    // does not overwrite the just-committed value with the stale buffer.
    if (auto* ft = dynamic_cast<pdfe::forms::FormTool*>(canvas_->currentTool())) {
        ft->syncEditBuffer(data.value);
    }

    canvas_->refreshPage(currentPageIndex);
    updateWindowTitle();
}

// ---------------------------------------------------------------------------
// TRE (Scanned Text Edit) methods
// ---------------------------------------------------------------------------

QString MainWindow::resolveTessdataPath() const
{
    QString appDir = QCoreApplication::applicationDirPath();

    // 1. App-relative: <exe_dir>/tessdata/ (post-build deployed, or installed)
    QString path1 = appDir + "/tessdata";
    if (QDir(path1).exists("eng.traineddata")) return path1;

    // 2. Parent-relative: <exe_dir>/../tessdata/ (single-config dev builds)
    QString path2 = appDir + "/../tessdata";
    if (QDir(path2).exists("eng.traineddata")) return path2;

    // 3. Multi-config MSVC dev builds: exe at build/vs2022/bin/<Config>/
    //    Source tree at build/vs2022/../../../../tests/fixtures/tessdata/
    QString path3 = appDir + "/../../../../tests/fixtures/tessdata";
    if (QDir(path3).exists("eng.traineddata")) return path3;

    // 4. Single-config dev builds: exe at build/bin/
    //    Source tree at build/../../tests/fixtures/tessdata/
    QString path4 = appDir + "/../../tests/fixtures/tessdata";
    if (QDir(path4).exists("eng.traineddata")) return path4;

    // 5. System/Tesseract default (empty → nullptr → TESSDATA_PREFIX env var)
    return QString();
}

QString MainWindow::resolveModelPath() const
{
    QString appDir = QCoreApplication::applicationDirPath();

    // 1. App-relative: <exe_dir>/models/ (post-build deployed, or installed)
    QString path1 = appDir + "/models/font_embedder.onnx";
    if (QFile::exists(path1)) return path1;

    // 2. Multi-config MSVC dev builds: exe at build/vs2022/bin/<Config>/
    QString path2 = appDir + "/../../../../models/font_embedder.onnx";
    if (QFile::exists(path2)) return path2;

    // 3. Single-config dev builds: exe at build/bin/
    QString path3 = appDir + "/../../models/font_embedder.onnx";
    if (QFile::exists(path3)) return path3;

    return QString();
}

void MainWindow::initTreComponents()
{
    // Lazy init -- only create once per document
    if (regionReconstructor_ || !document_) {
        return;
    }

    // Font infrastructure
    embeddedFontAnalyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(muPdfContext_.get());
    fontAnalysisCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    substituteFontSelector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        embeddedFontAnalyzer_.get(), fontAnalysisCache_.get());
    pdfFontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(muPdfContext_.get());

    // TRE pipeline with resolved tessdata path
    pdfe::tre::OcrConfig ocrConfig;
    QString tessdataPath = resolveTessdataPath();
    if (!tessdataPath.isEmpty()) {
        ocrConfig.tessdataPath = tessdataPath.toStdString();
    }
    // Honor user-configured OCR language from preferences.
    if (appSettings_) {
        const QString lang = appSettings_->ocrLanguage();
        if (!lang.isEmpty()) ocrConfig.language = lang.toStdString();
    }
    ocrPipeline_ = std::make_unique<pdfe::tre::OcrPipeline>(
        muPdfContext_.get(), document_.get(), ocrConfig);
    coordMapper_ = std::make_unique<pdfe::tre::CoordMapper>(
        muPdfContext_.get(), document_.get());
    bgColorDetector_ = std::make_unique<pdfe::tre::BgColorDetector>();
    fontMatcher_ = std::make_unique<pdfe::tre::FontMatcher>(
        substituteFontSelector_.get(), embeddedFontAnalyzer_.get());

    // CNN font re-ranker (optional -- gracefully absent if model file missing)
    std::string modelPath = resolveModelPath().toStdString();
    cnnFontClassifier_ = std::make_unique<pdfe::tre::CNNFontClassifier>(modelPath);
    if (cnnFontClassifier_->isAvailable()) {
        fontMatcher_->setCNNFontClassifier(cnnFontClassifier_.get());
        qDebug() << "[TRE] CNN font re-ranker loaded";
    } else {
        qDebug() << "[TRE] CNN font model not found -- using NCC fallback";
    }

    qualityAssessor_ = std::make_unique<pdfe::tre::QualityAssessor>();
    imageMasker_ = std::make_unique<pdfe::tre::ImageMasker>();
    bitmapCompositor_ = std::make_unique<pdfe::tre::BitmapCompositor>();
    fontMatcher_->setBitmapCompositor(bitmapCompositor_.get());
    pdfContentWriter_ = std::make_unique<pdfe::tre::PdfContentWriter>(
        muPdfContext_.get(), pdfFontEmbedder_.get());
    regionReconstructor_ = std::make_unique<pdfe::tre::RegionReconstructor>(
        ocrPipeline_.get(), coordMapper_.get(), bgColorDetector_.get(),
        fontMatcher_.get(), qualityAssessor_.get(), imageMasker_.get(),
        pdfContentWriter_.get());
    imageReplacer_ = std::make_unique<pdfe::tre::ImageReplacer>(
        ocrPipeline_.get(), coordMapper_.get(), bgColorDetector_.get(),
        fontMatcher_.get(), imageMasker_.get(), bitmapCompositor_.get(),
        pdfContentWriter_.get());
}

void MainWindow::destroyTreComponents()
{
    imageReplacer_.reset();
    regionReconstructor_.reset();
    pdfContentWriter_.reset();
    bitmapCompositor_.reset();
    imageMasker_.reset();
    qualityAssessor_.reset();
    if (fontMatcher_)
        fontMatcher_->setCNNFontClassifier(nullptr);
    cnnFontClassifier_.reset();
    fontMatcher_.reset();
    bgColorDetector_.reset();
    coordMapper_.reset();
    ocrPipeline_.reset();
    pdfFontEmbedder_.reset();
    substituteFontSelector_.reset();
    fontAnalysisCache_.reset();
    embeddedFontAnalyzer_.reset();
}

void MainWindow::releaseImageEditStack() {
    for (auto& entry : imageEditStack_) {
        if (entry.savedXObjectRef && pdfContentWriter_) {
            pdfContentWriter_->releaseImageXObjectRef(entry.savedXObjectRef);
            entry.savedXObjectRef = nullptr;
        }
    }
    imageEditStack_.clear();
}

void MainWindow::refreshImageForScannedPage(int pageIndex,
                                              const std::u32string& text,
                                              const QRectF& spanBbox,
                                              const std::vector<pdfe::textedit::FormattingRun>& fmtRuns,
                                              const std::string& spanFontName,
                                              float spanFontSize)
{
    initTreComponents();
    // GitHub #218: every exit path publishes a terminal recomposite status.
    // A waiter that only ever sees the SUCCESS status cannot distinguish "the
    // recomposite has not happened yet" from "the recomposite bailed out", and
    // would block for its whole timeout on the second case -- reporting a
    // harness timeout for a product decision. The early returns are results.
    if (!imageReplacer_ || !document_) {
        writeRecompositeStatus("RECOMPOSITE_FAILED", pageIndex,
                               QStringLiteral("no-image-replacer-or-document"));
        return;
    }

    auto page = document_->page(pageIndex);
    if (!page) {
        writeRecompositeStatus("RECOMPOSITE_FAILED", pageIndex,
                               QStringLiteral("page-unavailable"));
        return;
    }

    // Convert span bbox to PdfRect and text to UTF-8
    pdfe::tre::PdfRect selectionPdf{
        spanBbox.left(), spanBbox.top(), spanBbox.right(), spanBbox.bottom()};
    QString qText = QString::fromUcs4(
        text.data(), static_cast<qsizetype>(text.size()));
    std::string utf8Text = qText.toUtf8().toStdString();

    // Convert FormattingRun → FontRun for multi-font bitmap rendering
    qDebug() << "[TextEdit] refreshImageForScannedPage: page=" << pageIndex
             << "text=" << qText.left(40)
             << "bbox=" << spanBbox
             << "fmtRuns=" << fmtRuns.size();
    std::vector<pdfe::tre::FontRun> fontRuns;
    for (const auto& fr : fmtRuns) {
        qDebug() << "  [TextEdit] fmtRun:"
                 << "chars=" << fr.charStart << "-" << fr.charEnd
                 << "family=" << QString::fromStdString(fr.fontFamily)
                 << "fontSize=" << fr.fontSize
                 << "fontFilePath=" << (fr.fontFilePath.empty() ? "EMPTY" : QString::fromStdString(fr.fontFilePath))
                 << "bold=" << fr.bold << "italic=" << fr.italic;
        if (fr.fontFilePath.empty()) {
            qDebug() << "    [TextEdit] SKIPPING run (no fontFilePath)";
            continue;
        }
        pdfe::tre::FontRun run;
        run.charStart = fr.charStart;
        run.charEnd = fr.charEnd;
        run.fontFilePath = fr.fontFilePath;
        run.fontSizePt = fr.fontSize;
        run.style.isBold = fr.bold;
        run.style.isItalic = fr.italic;
        run.style.isUnderlined = fr.underline;
        run.style.isStrikethrough = fr.strikethrough;
        fontRuns.push_back(std::move(run));
    }
    qDebug() << "[TextEdit] fontRuns after filtering:" << fontRuns.size();

    // When the span has a real embedded font (subset prefix like "REFXWK+Liberation Serif"),
    // skip OCR-based font matching entirely. Instead, use the known font name to look up
    // the correct system font via the known-pair table. This avoids FontMatcher picking
    // wrong fonts (e.g., "Segoe Script" or "Palatino Linotype") based on noisy OCR analysis.
    bool hasRealFont = false;
    std::string strippedFontName;
    if (spanFontSize > 4.0f && spanFontName.size() >= 8 && spanFontName[6] == '+') {
        bool hasPrefix = true;
        for (int i = 0; i < 6; ++i) {
            if (spanFontName[i] < 'A' || spanFontName[i] > 'Z') {
                hasPrefix = false;
                break;
            }
        }
        if (hasPrefix) {
            hasRealFont = true;
            strippedFontName = spanFontName.substr(7); // After "XXXXXX+"
            qDebug() << "[TextEdit] Span has real embedded font"
                     << QString::fromStdString(spanFontName)
                     << "stripped=" << QString::fromStdString(strippedFontName)
                     << "fontSize=" << spanFontSize;
        }
    }

    // Determine if glyph atlas should be used: when the user has not changed
    // any font characteristics, extract character images from the original scan
    // for pixel-perfect visual consistency.
    bool useGlyphAtlas = fmtRuns.empty() && fontRuns.empty();

    try {
        // Build font match override when span has a real embedded font.
        // This skips OCR-based FontMatcher (which picks wrong fonts like "Segoe Script"
        // from noisy OCR analysis) and uses the known-pair table to find the correct
        // system font matching the embedded font name.
        std::unique_ptr<pdfe::tre::FontMatch> fontMatchOverride;
        if (hasRealFont) {
            std::string fontFilePath = pdfe::fonts::SubstituteFontSelector::lookupKnownPairPath(strippedFontName);

            // Fallback: serif/sans-serif heuristic from font name
            if (fontFilePath.empty()) {
                std::string lowerName = strippedFontName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                bool nameHasSerif = (lowerName.find("serif") != std::string::npos &&
                                     lowerName.find("sans") == std::string::npos);
                // FontResolver picks platform-appropriate path (bundled
                // fonts first for security; fontconfig/DirectWrite query
                // second; metric-compatible substitution chain third).
                fontFilePath = pdfe::fonts::FontResolver::resolveFontPath(
                    nameHasSerif ? "Times New Roman" : "Arial");
            }

            fontMatchOverride = std::make_unique<pdfe::tre::FontMatch>();
            fontMatchOverride->fontFilePath = fontFilePath;
            fontMatchOverride->fontFamily = strippedFontName;
            fontMatchOverride->sizePt = spanFontSize;
            fontMatchOverride->confidence = 0.95f;

            qDebug() << "[TextEdit] Real font known-pair lookup:"
                     << "stripped=" << QString::fromStdString(strippedFontName)
                     << "fontFilePath=" << QString::fromStdString(fontFilePath)
                     << "sizePt=" << spanFontSize;
        }

        // Always route through replace() which has OCR + glyph atlas support.
        // When fontMatchOverride is set, replace() skips FontMatcher and uses our
        // known-pair font. The enriched atlas (from overlay) provides WYSIWYG:
        // the same atlas+synthesis used for overlay is used for the saved bitmap.
        auto result = imageReplacer_->replace(
            page.get(), document_.get(), pageIndex,
            selectionPdf, utf8Text,
            pdfe::tre::RegionId{nextRegionId_++},
            false,  // snapToLines=false: only modify span area
            fontRuns,
            0.0f,   // no size override (handled by fontMatchOverride or FontMatcher)
            useGlyphAtlas,
            fontMatchOverride.get(),
            currentEnrichedAtlas_);
        if (result.success) {
            document_->markPageDirty(pageIndex);
            // GitHub #218: published AFTER markPageDirty so a waiter that
            // observes the status is guaranteed the page is already flagged
            // for re-render -- the ordering is the whole point of the seam.
            writeRecompositeStatus("RECOMPOSITE_DONE", pageIndex,
                                   QStringLiteral("ok"));
        } else {
            qWarning("Image replacement failed for page %d: %s",
                     pageIndex, result.errorMessage.c_str());
            writeRecompositeStatus(
                "RECOMPOSITE_FAILED", pageIndex,
                QString::fromStdString(result.errorMessage).left(120));
        }
    } catch (const std::exception& e) {
        qWarning("Image replacement for scanned page edit failed: %s", e.what());
        writeRecompositeStatus("RECOMPOSITE_FAILED", pageIndex,
                               QString::fromUtf8(e.what()).left(120));
    }
}

void MainWindow::onTreRegionSelected(int pageIndex, QRectF pdfRect)
{
    if (!document_ || !regionReconstructor_ || !imageReplacer_ || !pdfContentWriter_) {
        return;
    }

    // Convert QRectF to PdfRect
    pdfe::tre::PdfRect selectionPdf{
        pdfRect.left(), pdfRect.top(), pdfRect.right(), pdfRect.bottom()};

    try {
    // Step 1: Analyze the region (OCR + quality assessment, no PDF modification)
    auto analysisResult = regionReconstructor_->analyze(pageIndex, selectionPdf);

    if (!analysisResult.success) {
        QMessageBox::warning(this, tr("Text Recognition"),
            tr("Could not recognize text in the selected region.\n%1")
                .arg(QString::fromStdString(analysisResult.errorMessage)));
        return;
    }

    // Step 2: Show the edit dialog with OCR results
    pdfe::tre::TreEditDialog dialog(analysisResult, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Step 3: Get user choices
    std::string editedText = dialog.editedText().toStdString();
    pdfe::tre::Approach approach = dialog.selectedApproach();

    // Step 4: Create and execute the appropriate command
    pdfe::tre::RegionId regionId{static_cast<uint32_t>(pageIndex * 1000 + 1)};

    std::unique_ptr<pdfe::commands::Command> cmd;

    if (approach == pdfe::tre::Approach::B_HybridReconstruction) {
        cmd = std::make_unique<pdfe::tre::ReconstructRegionCommand>(
            document_.get(), muPdfContext_,
            regionReconstructor_.get(), imageReplacer_.get(),
            pdfContentWriter_.get(),
            pageIndex, selectionPdf, editedText, regionId);
    } else {
        cmd = std::make_unique<pdfe::tre::ImageReplaceCommand>(
            document_.get(), muPdfContext_,
            imageReplacer_.get(), pdfContentWriter_.get(),
            pageIndex, selectionPdf, editedText, regionId);
    }

    undoManager_->execute(std::move(cmd));

    // Step 5: Refresh the page display
    if (canvas_) {
        canvas_->refreshPage(pageIndex);
    }
    updateWindowTitle();

    } catch (const std::exception& ex) {
        QMessageBox::critical(this, tr("Text Recognition Error"),
            tr("An error occurred during text recognition:\n%1")
                .arg(QString::fromStdString(ex.what())));
    }
}

// ---------------------------------------------------------------------------
// Redaction methods
// ---------------------------------------------------------------------------

void MainWindow::onRedactToolSelected()
{
    if (!document_) return;

    if (redactToolAction_->isChecked()) {
        onAnnotationToolSelected(redactToolAction_);
        redactionPanelDock_->show();
    } else {
        onAnnotationToolSelected(selectToolAction_);
    }
}

void MainWindow::onMarkRedaction()
{
    // Placeholder - actual marking done through tool interaction
}

// ===========================================================================
// WIN-87 site #4, phase-57 — the QPDF single-revision rewrite that publishes
// the REDACTED document.
//
// EXTRACTED FROM onApplyRedactions() so it can be driven by a hostile-
// filesystem test: the caller above is behind a modal confirmation, and a
// window that only opens inside a QMessageBox cannot be attacked by a test.
// Nothing about the sequencing changed in the extraction; the closeDocument()
// that used to sit inline still runs here, in the same place.
//
// WHAT IT PUBLISHES AND WHY THAT RAISES THE STAKES. The artifact renamed over
// the user's file at the end of this is the redacted document — the one whose
// entire purpose is that certain content has been removed. Silent adoption of
// attacker bytes here does not merely corrupt a file; it can serve content the
// user believes they destroyed, under a filename they trust as redacted.
//
// TWO WINDOWS, AND THE SECOND ONE IS NOT ON THE STAGING FILE.
//
//   1. The OUTPUT. This wrote ".redact_out" by path and then renamed it over
//      the document with the path-based helper — the WIN-87 shape. It now
//      writes into a StagingFile behind an exclusive handle at an
//      unpredictable name and publishes by renaming THROUGH that handle.
//
//   2. The INPUT, which no acceptance criterion for this phase names, and
//      which the staging migration does not touch. The sequence was:
//
//          closeDocument();            // deliberately releases ALL locks
//          ---- window ----
//          qpdf.open(filePath);        // re-resolves the SOURCE by name
//
//      An attacker who renames a valid decoy PDF over `filePath` in that
//      interval has QPDF read attacker bytes and republish them over the
//      user's document as the redacted result. No staging file is involved in
//      the attack at all, which is exactly why a migration that only hardens
//      what PDFE WRITES leaves it open.
//
//      THE CLOSE IS NOT OPTIONAL AND IS NOT THE BUG. It exists because Windows
//      will not let the rename land while this process holds the file (see the
//      P11-BUG-1 note in onApplyRedactions, and DCA 0021's closing note: a
//      handle held by THIS process blocks a replace-existing rename even under
//      FileRenameInfoEx's POSIX semantics). So the fix is not to keep the
//      document open — it is to resolve the name ONCE, before the close, into
//      a deny-write read handle, read through that handle, and close it before
//      publishing. The document's own locks are released exactly as before;
//      what is gone is the second resolution of the name.
//
// FAILS CLOSED. Every step reports through an exception, the caller's existing
// catch reopens the original, and on any failure the user's file is untouched.
// In particular a failure to acquire the deny-write reader is a refusal, never
// a fallback to opening by name.
// ===========================================================================
void MainWindow::rewriteRedactedDocumentThroughQpdf(
    const std::filesystem::path& filePath)
{
    // (2) Resolve the SOURCE name once, while the document still holds the
    // file and its bytes are still the ones we just wrote. From here no other
    // process can obtain write or delete access to it.
    std::error_code srcEc;
    std::FILE* sourceStream =
        pdfe::core::openDenyWriteReadStream(filePath, srcEc);
    if (!sourceStream) {
        throw std::runtime_error(
            "Could not take an exclusive read handle on the redacted document "
            "before the rewrite (original left intact): " + srcEc.message());
    }
    struct SourceGuard {
        std::FILE* fp;
        ~SourceGuard() { if (fp) std::fclose(fp); }
        std::FILE* release() { auto* p = fp; fp = nullptr; return p; }
    } source{sourceStream};

    // Close the document to release ITS locks — required for the publish, and
    // now harmless, because the bytes QPDF will read are already pinned by the
    // handle above rather than by whatever is at this name afterwards.
    closeDocument();

    // WIN-87 site #4 test seam — see MainWindow.h for why this is not mere
    // convenience. This is THE window: the document's locks are released and
    // the source has not been read yet. In this (hardened) build the pin above
    // is already held, so an attacker firing here cannot write; in the
    // pre-phase-57 build nothing is held and it wins every time. Without a
    // deterministic point the hostile case was measured PASSING against the
    // vulnerable code.
    {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(sRedactionRewriteWindowHookMutex_);
            hook = sRedactionRewriteWindowHook_;
        }
        if (hook) hook();
    }

    // (1) The output: exclusive handle, unpredictable name, published through
    // the handle. This replaced ".redact_out", which existed only to be
    // renamed and is therefore gone rather than merely hardened.
    std::error_code stageEc;
    auto staged = pdfe::core::StagingFile::createBeside(filePath, stageEc);
    if (!staged) {
        throw std::runtime_error(
            "Could not create the staging file for the redacted rewrite "
            "(original left intact): " + stageEc.message());
    }
    std::FILE* stagedStream = staged->openDuplicateStream(stageEc);
    if (!stagedStream) {
        throw std::runtime_error(
            "Could not open the staging stream for the redacted rewrite "
            "(original left intact): " + stageEc.message());
    }

    // OWNERSHIP OF `stagedStream` PASSES TO QPDF ONLY ONCE saveToStream IS
    // ENTERED. Until then this scope owns it, and a throw from openFromStream
    // (any QPDF parse failure of the document we just wrote) used to leak it.
    // That is not a mere handle leak: the duplicate keeps the StagingFile's
    // FileDispositionInfo unlink PENDING, so the staging file survives on disk
    // for the life of the process, at an unpredictable name, beside the user's
    // document — contradicting the "no residue of any kind" property this phase
    // claims. Found by the non-gate security review.
    struct StagedStreamGuard {
        std::FILE* fp;
        ~StagedStreamGuard() { if (fp) std::fclose(fp); }
        std::FILE* release() { auto* p = fp; fp = nullptr; return p; }
    } stagedGuard{stagedStream};

    // Scoped: QPDF reads lazily, so the source stream must live through the
    // write — and it must be CLOSED before the commit below, because a handle
    // this process holds blocks the replace-existing rename.
    {
        pdfe::core::QpdfWrapper qpdf;
        qpdf.openFromStream(source.release(), filePath.string());
        qpdf.saveToStream(stagedGuard.release(), false);
    }

    {
        std::error_code flushEc;
        staged->flushToStableStorage(flushEc);  // best-effort
    }

    // F10 / WIN-85 closed the DATA-LOSS hazard here: this used to be
    // remove(filePath) + rename(tempOut, filePath), so a rename failure after
    // the remove destroyed the user's redacted document outright. That fix is
    // still in force — nothing is ever removed first and on any failure the
    // original is byte-intact.
    //
    // WIN-87 / phase-57 closes the SUBSTITUTION hazard that fix left behind:
    // the shared helper it routed to takes PATHS, so the staging name was
    // re-resolved at rename time with no handle held across the window.
    // commitOverWithRetry publishes through the exclusive handle and keeps the
    // same 10x/100 ms anti-A/V calibration.
    // THE OUTPUT PUBLISH WINDOW, marked deterministically. The staged rewrite
    // is complete and has not yet been renamed over the user's document — the
    // instant an attacker must hit to have PDFE publish their bytes as the
    // redacted result. Here the StagingFile still holds it exclusively at an
    // unpredictable name; in the pre-phase-57 build it was ".redact_out", an
    // ordinary file at a name derived from the document's own. Null in
    // production.
    fireRedactionOutputStagingHookForTesting(staged->path());

    std::error_code renameEc;
    if (!staged->commitOverWithRetry(filePath, renameEc)) {
        throw std::runtime_error(
            "Failed to replace the original with the rewritten file "
            "(original left intact): " + renameEc.message());
    }
}

bool MainWindow::rewriteRedactedDocumentThroughQpdfForTesting(
    const std::filesystem::path& filePath, QString* errorOut)
{
    try {
        rewriteRedactedDocumentThroughQpdf(filePath);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

void MainWindow::onApplyRedactions()
{
    if (!document_ || !redactionManager_) return;

    int markCount = redactionManager_->pendingMarkCount(document_.get());
    if (markCount == 0) {
        QMessageBox::information(this, tr("No Redactions"),
            tr("There are no redaction marks to apply."));
        return;
    }

    QMessageBox::StandardButton reply = QMessageBox::warning(this,
        tr("Apply Redactions"),
        tr("This will permanently and irreversibly remove content under %1 redaction mark(s).\n\n"
           "This action cannot be undone. The undo history will be cleared.\n\n"
           "Continue?").arg(markCount),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    // Capture the mark geometry BEFORE applying, because applying clears it and
    // Step 4 below needs it to verify. This is the only point at which the
    // regions that were redacted are still knowable.
    const std::vector<pdfe::redaction::RedactionData> marksToVerify =
        redactionManager_->getAllRedactionMarks(document_.get());

    // Step 1: Apply redactions in-memory only (skip QPDF rewrite inside
    // applyAllRedactions — the rewrite fails on Windows because MuPDF holds
    // the temp file locked after document->save()).
    pdfe::redaction::RedactionApplyOptions options;
    options.rewriteDocument = false;
    auto result = redactionManager_->applyAllRedactions(document_.get(), options);

    if (result.success) {
        int currentPage = canvas_ ? canvas_->currentPage() : 0;
        std::filesystem::path filePath = qStringToPath(currentFileName_);

        // ----- P11-BUG-1 fix (pipeline b3f19d72 phase-5) -----------
        // Release every owning Page reference *before*
        // Document::save() so that fz_drop_document inside save() drops
        // the document's refcount to zero and Windows releases the file
        // lock — otherwise the temp+rename step fails with "The process
        // cannot access the file because it is being used by another
        // process", the save throws, and the on-disk file ends up
        // byte-identical to the pre-redact original even though the
        // in-memory canvas correctly shows the redacted page.
        //
        // Pages keep the document alive via fz_keep_document inside
        // fz_load_page (MuPDF source/fitz/document.c:1082).  Any Page
        // still alive when Document::save() runs prevents the rename.
        // Redaction is irreversible (we clear undo below), so dropping
        // these references early is safe.
        //
        // Order matters:
        //   1) clear the active tool (the tool may hold a raw pointer
        //      into toolPage_ that becomes dangling on reset);
        //   2) cancel + drain the tile renderer so no worker thread
        //      still owns an fz_display_list (display lists keep the
        //      document alive via fz_keep_document — see MuPDF
        //      source/fitz/list-device.c);
        //   3) invalidate the tile cache (entries reference
        //      display-list output buffers);
        //   4) drop toolPage_ and commandPages_ (these own Page objects
        //      that each hold one fz_keep_document refcount);
        //   5) clear undoManager_ (commands may capture Pages too).
        if (canvas_) {
            canvas_->clearCurrentTool();
        }
        if (tileRenderer_) {
            tileRenderer_->cancelAll();
            tileRenderer_->waitForDone();
        }
        if (tileCache_) {
            tileCache_->invalidateAll();
        }
        toolPage_.reset();
        // Null-rebind before the free: the undo stack is still live at this
        // exact clear (it is cleared on the next line). Rebinding every
        // command's cached Page* to nullptr first keeps the ordering
        // "release page refs -> free pages -> drop commands" free of any
        // transient dangling Page* (defense-in-depth; mirrors the
        // onDocumentStructureChanged fix).
        if (undoManager_) {
            undoManager_->forEachCommand(
                [](pdfe::commands::Command* c) { c->rebindPage(nullptr); });
        }
        commandPages_.clear();
        if (undoManager_) undoManager_->clear();

        // Step 2: Save the document with redactions applied to the original path.
        // Document::save() writes all in-memory changes (including redacted content
        // streams) to disk, closes the MuPDF handle, and reopens from the saved file.
        try {
            document_->save(filePath);
        } catch (const std::exception& e) {
            QMessageBox::critical(this, tr("Redaction Save Error"),
                tr("Redactions were applied but failed to save: %1").arg(e.what()));
            return;
        }

        // Step 3: QPDF single-revision rewrite to eliminate data ghosts.
        // Close the document first to release file locks, then rewrite, then reopen.
        bool rewriteOk = false;
        QString savedPath = currentFileName_;  // Save before closeDocument() clears it
        try {
            rewriteRedactedDocumentThroughQpdf(filePath);
            rewriteOk = true;

            // Reopen the clean file
            openFile(savedPath);
        } catch (const std::exception&) {
            // QPDF rewrite failed — try to reopen the original (still has redactions
            // applied, just with previous revision data still present)
            if (!document_) {
                openFile(savedPath);
            }
        }

        // Restore page position
        if (canvas_ && document_) {
            if (currentPage >= document_->pageCount())
                currentPage = document_->pageCount() - 1;
            if (currentPage >= 0)
                canvas_->goToPage(currentPage);
        }

        // Clear undo stack since redaction is irreversible
        if (undoManager_) undoManager_->clear();
        updateWindowTitle();

        // Update redaction panel
        if (redactionPanel_) redactionPanel_->clear();
        if (applyRedactionsAction_) applyRedactionsAction_->setEnabled(false);

        // Step 4: VERIFY the redaction actually removed the content. (GitHub #253)
        //
        // Until this was wired, RedactionVerifier was compiled in, unit-tested,
        // and unreachable from main() — one of the eighteen classes #253 found in
        // that state, and the one with a security consequence rather than a
        // feature consequence. Redaction itself ships and works; what did not run
        // was the check that distinguishes a redaction from a black rectangle
        // drawn over still-extractable text.
        //
        // It runs AFTER the QPDF rewrite deliberately: checkSingleRevision looks
        // for previous-revision data, and the rewrite is what removes it, so
        // verifying before it would report a failure the product then fixes.
        //
        // AND IT REPORTS INTO THE SUCCESS DIALOG, not a log. The dialog already
        // says "Successfully applied N redaction(s)" — a verification that fails
        // silently behind that sentence is worse than no verification, because the
        // user has been told the irreversible destructive operation worked.
        QString verifyNote;
        // Severity is carried as a FLAG, not re-derived by searching verifyNote
        // for a tr() fragment. A substring match on translated text is a real
        // defect: the translation of a short fragment need not appear inside the
        // translation of the longer sentence containing it, so under any
        // non-English locale the severity would silently fall back to
        // "information" -- announcing success over a FAILED verification, which
        // is the exact outcome this step exists to prevent.
        bool verifyFailed = false;
        if (!marksToVerify.empty() && document_) {
            try {
                pdfe::redaction::RedactionVerifier verifier(document_->context());
                const auto vr = verifier.verify(filePath, marksToVerify);
                QStringList problems;
                if (!vr.textRemoved)
                    problems << tr("text from a redacted region is still present");
                if (!vr.noOrphanStreams)
                    problems << tr("orphaned content streams remain");
                if (!vr.singleRevision)
                    problems << tr("earlier document revisions are still readable");
                for (const auto& w : vr.warnings)
                    problems << QString::fromStdString(w);

                if (!vr.success || !problems.isEmpty()) {
                    verifyFailed = true;
                    verifyNote = tr("\n\nVERIFICATION FAILED — this file may still "
                                    "contain the redacted content:\n  • %1")
                                     .arg(problems.isEmpty()
                                              ? QString::fromStdString(vr.errorMessage)
                                              : problems.join(QStringLiteral("\n  • ")));
                } else {
                    verifyNote = tr("\n\nVerified: redacted content is not "
                                    "recoverable from the saved file.");
                }
            } catch (const std::exception& e) {
                verifyFailed = true;
                // A verifier that cannot run must say so. Reporting nothing here
                // would read as "verified" to anyone looking at the dialog, which
                // is the failure mode this whole step exists to remove.
                verifyNote = tr("\n\nVerification could not be completed (%1), so "
                                "removal of the redacted content is UNCONFIRMED.")
                                 .arg(QString::fromUtf8(e.what()));
            }
        }

        const QString body =
            tr("Successfully applied %1 redaction(s) on %2 page(s).%3%4")
                .arg(result.marksApplied).arg(result.pagesAffected)
                .arg(rewriteOk ? "" : tr("\n\nNote: Document revision cleanup was skipped."))
                .arg(verifyNote);
        if (verifyFailed) {
            QMessageBox::warning(this, tr("Redactions Applied — Not Verified"), body);
        } else {
            QMessageBox::information(this, tr("Redactions Applied"), body);
        }
    } else {
        QMessageBox::critical(this, tr("Redaction Failed"),
            tr("Failed to apply redactions: %1").arg(QString::fromStdString(result.errorMessage)));
    }
}

void MainWindow::onDeleteRedactionMark(int pageIndex, int annotIndex)
{
    if (!document_ || !redactionManager_) return;

    auto page = document_->page(pageIndex);
    commandPages_.push_back(document_->page(pageIndex));
    auto* pagePtr = commandPages_.back().get();

    auto cmd = std::make_unique<pdfe::redaction::DeleteRedactionCommand>(
        redactionManager_.get(), pagePtr, annotIndex);
    undoManager_->execute(std::move(cmd));

    // Refresh panel
    auto marks = redactionManager_->getAllRedactionMarks(document_.get());
    redactionPanel_->setMarks(marks);
    applyRedactionsAction_->setEnabled(!marks.empty());
}

void MainWindow::onRedactionMarkSelected(int pageIndex, int /*annotIndex*/)
{
    if (!document_ || !canvas_) return;
    canvas_->goToPage(pageIndex);
}

// ---------------------------------------------------------------------------
// Encryption methods
// ---------------------------------------------------------------------------

void MainWindow::onSaveWithEncryption()
{
    if (!document_ || !encryptionManager_) return;

    if (!document_->isPdf()) {
        QMessageBox::warning(this, tr("Save with Encryption"),
            tr("Only PDF documents can be encrypted."));
        return;
    }

    // Show encryption dialog
    EncryptionDialog dialog(this);

    // GitHub #180 — seed "Encrypt metadata" from THIS document's existing
    // /EncryptMetadata rather than from a hardcoded checked box. Re-saving a
    // document whose metadata encryption was OFF used to turn it back ON with
    // nothing said; the state must never flip except because the user changed
    // the box. If the state cannot be read the conservative default (encrypt)
    // stands — an unreadable document is not a licence to expose its metadata.
    try {
        dialog.seedFromExistingEncryption(
            encryptionManager_->getEncryptionInfo(document_.get()));
    } catch (const std::exception&) {
        // Leave the conservative default in place.
    }

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto settings = dialog.settings();

    // Ask for output file
    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Save Encrypted PDF"),
        currentFileName_,
        tr("PDF Files (*.pdf);;All Files (*)")
    );

    if (fileName.isEmpty()) {
        return;
    }

    try {
        std::filesystem::path outputPath = qStringToPath(fileName);

        if (settings.algorithm == pdfe::encryption::EncryptionAlgorithm::None) {
            // Save without encryption (remove encryption if present)
            //
            // F26: this is a full rewrite, so any signature on the source stops
            // verifying in the copy. Warn — never refuse; the user explicitly
            // asked for an unprotected artifact at a named output path.
            pdfe::encryption::RemoveEncryptionReport rmReport;
            encryptionManager_->removeEncryption(document_.get(), outputPath,
                                                 &rmReport);
            if (rmReport.rewriteBreaksSignatures) {
                QMessageBox::warning(this, tr("Signatures Will Not Verify"),
                    QString::fromStdString(rmReport.rewriteIntegrityWarning));
            }
            statusBar_->showMessage(tr("File saved without encryption to %1")
                .arg(QFileInfo(fileName).fileName()), 3000);
        } else {
            // PDF-27: the chosen handler may not be able to express every
            // permission the user asked for (RC4-40/R2 defines only four of
            // the eight bits). Report the loss instead of writing a file that
            // silently permits what the user just denied.
            pdfe::encryption::EncryptionSaveReport encReport;
            encryptionManager_->saveEncrypted(document_.get(), outputPath, settings,
                                              &encReport);
            if (encReport.lossyPermissionMapping) {
                QMessageBox::warning(this, tr("Permissions Not Fully Applied"),
                    QString::fromStdString(encReport.summary));
            }
            // F24 — owner-only protection surfaced at the point of APPLYING it,
            // not only from the Document Permissions dialog nobody opens. The
            // dialog already warns live while the passwords are typed
            // (EncryptionDialog::updateEmptyUserPasswordWarning); this covers
            // the same settings arriving from anywhere else.
            if (encReport.emptyUserPassword) {
                QMessageBox::warning(this, tr("Empty User Password"),
                    QString::fromStdString(encReport.emptyUserPasswordWarning));
            }
            statusBar_->showMessage(tr("File saved with %1 encryption to %2")
                .arg(QString::fromStdString(encReport.handlerWritten))
                .arg(QFileInfo(fileName).fileName()), 3000);
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Encryption Error"),
            tr("Failed to save encrypted file: %1").arg(e.what()));
    }
}

void MainWindow::onDocumentPermissions()
{
    if (!document_ || !encryptionManager_) return;

    try {
        auto info = encryptionManager_->getEncryptionInfo(document_.get());

        QString message;
        if (!info.isEncrypted) {
            message = tr("This document is not encrypted.\n\n"
                         "All operations are permitted.");
        } else {
            message = tr("Encryption: %1\n"
                         "Version: V%2 R%3\n"
                         "Key Length: %4 bits\n"
                         "Metadata Encrypted: %5\n\n"
                         "Permissions:\n"
                         "  Print: %6\n"
                         "  Print High Quality: %7\n"
                         "  Modify: %8\n"
                         "  Copy: %9\n"
                         "  Annotate: %10\n"
                         "  Fill Forms: %11\n"
                         "  Assemble: %12\n"
                         "  Accessibility: %13")
                .arg(QString::fromStdString(info.method))
                .arg(info.version)
                .arg(info.revision)
                .arg(info.keyLength)
                .arg(info.metadataEncrypted ? tr("Yes") : tr("No"))
                .arg(info.permissions.print ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.printHighQuality ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.modify ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.copy ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.annotate ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.formFill ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.assemble ? tr("Allowed") : tr("Denied"))
                .arg(info.permissions.accessibility ? tr("Allowed") : tr("Denied"));

            // Encryption / permission-handler security area
            // (docs/security-policy.md). Two conditions make an "encrypted"
            // document far weaker than its header suggests, and neither is
            // visible from the fields above. Say so, in the same dialog.
            if (info.streamsUnprotected) {
                message += tr("\n\nWARNING: this document's stream crypt filter "
                              "is /Identity, so page content, images and "
                              "embedded files are stored UNENCRYPTED inside the "
                              "encrypted wrapper. The permission flags above are "
                              "the only protection present.");
            }
            if (info.emptyUserPassword) {
                message += tr("\n\nWARNING: this document has an EMPTY user "
                              "password. It opens with full read access without "
                              "any password; the permission flags above are "
                              "advisory and most PDF viewers do not enforce "
                              "them.");
            }
        }

        QMessageBox::information(this, tr("Document Permissions"), message);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to read encryption info: %1").arg(e.what()));
    }
}

void MainWindow::onRemovePassword()
{
    if (!document_ || !encryptionManager_) return;

    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Save Unencrypted Copy"),
        currentFileName_,
        tr("PDF Files (*.pdf);;All Files (*)")
    );
    if (fileName.isEmpty()) {
        return;
    }

    try {
        std::filesystem::path outputPath = qStringToPath(fileName);
        // F26 — same warn-never-refuse disposition as onSaveWithEncryption().
        pdfe::encryption::RemoveEncryptionReport rmReport;
        encryptionManager_->removeEncryption(document_.get(), outputPath,
                                             &rmReport);
        if (rmReport.rewriteBreaksSignatures) {
            QMessageBox::warning(this, tr("Signatures Will Not Verify"),
                QString::fromStdString(rmReport.rewriteIntegrityWarning));
        }
        statusBar_->showMessage(
            tr("Unencrypted copy saved to %1")
                .arg(QFileInfo(fileName).fileName()),
            3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Remove Password Error"),
            tr("Failed to save unencrypted copy: %1").arg(e.what()));
    }
}

// ============================================================================
// Signature methods
// ============================================================================

void MainWindow::onSignDocument()
{
    if (!document_ || !signatureManager_) return;

    // Check if there are unsigned signature fields
    auto sigs = signatureManager_->listSignatures(document_.get());
    bool hasUnsignedField = false;
    int targetPage = -1;
    int targetWidget = -1;
    for (const auto& sig : sigs) {
        if (!sig.isSigned) {
            hasUnsignedField = true;
            targetPage = sig.pageIndex;
            targetWidget = sig.widgetIndex;
            break;
        }
    }

    if (!hasUnsignedField) {
        // Create a new signature field first
        if (document_->pageCount() == 0) return;

        auto page = document_->page(canvas_ ? canvas_->currentPage() : 0);
        auto cmd = std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
            signatureManager_.get(), page.get(), "Signature",
            QRectF(50, 50, 200, 60));
        undoManager_->execute(std::move(cmd));
        commandPages_.push_back(std::move(page));

        // Re-list to get the new field
        sigs = signatureManager_->listSignatures(document_.get());
        for (const auto& sig : sigs) {
            if (!sig.isSigned) {
                targetPage = sig.pageIndex;
                targetWidget = sig.widgetIndex;
                break;
            }
        }
    }

    if (targetPage < 0 || targetWidget < 0) return;

    // Show sign dialog
    SignDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    pdfe::signatures::SigningOptions opts = dialog.signingOptions();

    // P12-INFRA-1 sub-issue 3: validate the PFX credentials BEFORE
    // the Save As dialog opens so a wrong-password attempt surfaces
    // a "Signing Error" QMessageBox immediately rather than failing
    // silently (or after the user has chosen a save path).  We probe
    // by attempting to load + drop the PFX via the Pkcs7OpenSslBridge.
    // muPdfContext_->execute already wraps the lambda in fz_try/
    // fz_catch and translates fz_throw to MuPdfException, so the
    // wrong-password fz_throw from readPfx surfaces as a C++
    // std::exception we can catch here.
    {
        pdfe::signatures::Pkcs7OpenSslBridge probeBridge;
        bool pfxOk = false;
        try {
            muPdfContext_->execute([&](fz_context* ctx) {
                pdf_pkcs7_signer* probe = probeBridge.readPfx(
                    ctx, opts.pfxPath.c_str(),
                    opts.pfxPassword.c_str());
                if (probe) {
                    pdfe::signatures::Pkcs7OpenSslBridge::dropSigner(
                        ctx, probe);
                    pfxOk = true;
                }
            });
        } catch (const std::exception&) {
            // Do NOT log the password or any per-attempt detail
            // (intentionally drop e.what() to avoid leaking PFX path
            // / password into logs).
            pfxOk = false;
        }
        if (!pfxOk) {
            QMessageBox::critical(this, tr("Signing Error"),
                tr("Failed to sign document: PFX could not be loaded "
                   "with the provided credentials (wrong password or "
                   "corrupt PFX)."));
            return;
        }
    }

    try {
        // Need to save document first so signing can do incremental write
        QString savePath = QFileDialog::getSaveFileName(
            this, tr("Save Signed PDF"),
            currentFileName_,
            tr("PDF Files (*.pdf)"));
        if (savePath.isEmpty()) return;

        // SaveAs first to preserve original
        document_->saveAs(qStringToPath(savePath));

        // Close current and reopen from new path
        std::filesystem::path newPath = qStringToPath(savePath);
        document_ = pdfe::core::Document::open(muPdfContext_, newPath);

        auto page = document_->page(targetPage);
        signatureManager_->signField(document_.get(), page.get(), targetWidget, opts);

        // Refresh
        currentFileName_ = savePath;
        // Signing wrote the document to a real path of its own — drop any
        // placeholder display name ("Untitled" from File > New).
        documentDisplayName_.clear();
        updateWindowTitle();

        // Reopen to refresh state after signing
        document_ = pdfe::core::Document::open(muPdfContext_, newPath);
        canvas_->setDocument(document_.get(), tileCache_.get(), tileRenderer_.get());

        // Update signature panel
        auto updatedSigs = signatureManager_->listSignatures(document_.get());
        signaturePanel_->setSignatures(updatedSigs);
        signaturePanelDock_->show();

        statusBar_->showMessage(tr("Document signed successfully"), 5000);

    } catch (const std::exception&) {
        // Intentionally do not surface e.what() — exception messages
        // from the signing stack can include the PFX file path or
        // internal OpenSSL detail. Match the probe-catch design above:
        // user gets a generic message, full diagnostic stays in logs.
        QMessageBox::critical(this, tr("Signing Error"),
            tr("Failed to sign the document. Check that the certificate "
               "and password are correct, then try again."));
    }
}

void MainWindow::onVerifySignatures()
{
    if (!document_ || !signatureManager_) return;

    try {
        auto summary = signatureManager_->verifyAllSignatures(document_.get());

        signaturePanel_->setSignatures(summary.signatures);
        signaturePanelDock_->show();

        // Pipeline F phase-10: run the offline-first LTV validator and
        // surface the per-document validation status. Independent of the
        // per-widget verify above — this looks at /DSS as a whole.
        if (summary.signedCount > 0) {
            auto ltv = pdfe::signatures::LtvValidator::verify(document_.get());
            // PHASE-50 (F52/F53): ltv.message used to be discarded right here.
            // It is the only place the specific rule — a rogue OCSP responder,
            // an expired CRL, an untrusted chain — is stated, and the status
            // enum cannot carry it. Pass it to the panel's detail row.
            signaturePanel_->setLtvStatusText(
                pdfe::signatures::ltvStatusToDisplayString(ltv.status),
                ltv.message);
        } else {
            signaturePanel_->setLtvStatusText(QString(), QString());
        }

        // Pipeline F phase-9: gate the LTV "Request Validation Data..."
        // action on verifyAllSignatures().validCount > 0.
        // Test seam: PDFE_TEST_VALIDATION_REQUEST_UNLOCKED=1 lowers the
        // threshold to signedCount (any signed field) so self-signed test
        // certs on the test rig can exercise the dialog.  NEVER set in
        // production; inert when absent.
        if (requestValidationAction_) {
            bool unlock = !qEnvironmentVariable(
                "PDFE_TEST_VALIDATION_REQUEST_UNLOCKED").isEmpty();
            int threshold = unlock ? summary.signedCount : summary.validCount;
            requestValidationAction_->setEnabled(threshold > 0);
        }

        if (summary.totalCount == 0) {
            QMessageBox::information(this, tr("Verify Signatures"),
                tr("This document has no digital signatures."));
            return;
        }

        QString message = tr("Signatures: %1 total\n"
                             "  Signed: %2\n"
                             "  Unsigned: %3\n"
                             "  Valid: %4\n"
                             "  Invalid: %5")
            .arg(summary.totalCount)
            .arg(summary.signedCount)
            .arg(summary.unsignedCount)
            .arg(summary.validCount)
            .arg(summary.invalidCount);

        QMessageBox::information(this, tr("Signature Verification"), message);

    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Verification Error"),
            tr("Failed to verify signatures: %1").arg(e.what()));
    }
}

// Pipeline F phase-9. EXTERNAL NETWORK FETCH path.
//
// SECURITY MODEL:
//   - This slot is the ONLY entry point into ValidationCoordinator.
//   - The action is disabled at startup and re-enabled only after
//     onVerifySignatures() reports validCount > 0.
//   - ValidationCoordinator shows a modal consent dialog BEFORE any
//     QNetworkAccessManager request is constructed; if the user clicks
//     Cancel no socket is opened.
//   - All network I/O is async (QNetworkReply::finished). The UI thread
//     is never blocked.
void MainWindow::onRequestValidation()
{
    if (!document_) return;

    auto* coord = new pdfe::signatures::ValidationCoordinator(this);

    // F58-g (phase-53 review, finding 5) — WHO IS STILL OPEN WHEN THE FETCH
    // FINALLY LANDS.
    //
    // Everything installed here runs on the far side of an unbounded
    // asynchronous wait. `coord` is parented to `this`, so the `this` captures
    // are safe — but `document_` and `undoManager_` are pinned by nothing. A
    // stalled OCSP/CRL responder plus a File>Close (or a File>Open of something
    // else; neither action is disabled while a fetch is in flight) used to land
    // on `*document_` with document_ already reset, and replacing the document
    // rather than closing it was worse still: the write would have succeeded,
    // against the wrong file.
    //
    // The resolver is the guard. The coordinator asks what is open NOW and
    // checks it is the same file carrying the same signature before anything is
    // written. See ValidationCoordinator::setLiveDocumentResolver() for why the
    // check is on identity rather than on the pointer.
    coord->setLiveDocumentResolver([this]() -> pdfe::core::Document* {
        return document_.get();
    });

    // F58 (phase-52). The /DSS write is staged + atomically renamed, so it
    // releases the fz_document before the rename (Windows will not rename over
    // an open file). A live Page keeps that handle open via fz_keep_document —
    // the same constraint File>Save has always had — so the write must run
    // inside the drain -> write -> rebind sequence, or an active editing tool
    // or a non-empty undo stack would make the rename fail. See
    // src/core/SaveWithRebind.h and journey-soak-phase3-findings.md §4.
    //
    // The fail-closed check below is unreachable while the resolver above is
    // installed. It is here anyway because THIS is the code that actually
    // dereferences: a future edit that drops or reorders the resolver must not
    // silently turn this back into a null dereference. Throwing is the
    // documented way for a scope to report the write as failed.
    coord->setWriteScope([this](const std::function<void()>& write) {
        if (!document_ || !undoManager_) {
            throw pdfe::core::PdfeException(
                "the document this validation request was made against is no "
                "longer open");
        }
        toolPage_.reset();  // recreatable; MainWindow::onSaveFile does the same
        pdfe::core::detail::drainRebindAroundWrite(
            *document_, *undoManager_, commandPages_, write);
    });

    connect(coord, &pdfe::signatures::ValidationCoordinator::completed,
            this, [this, coord]() {
        statusBar_->showMessage(tr("Validation data embedded"), 5000);
        coord->deleteLater();
    });
    connect(coord, &pdfe::signatures::ValidationCoordinator::cancelled,
            this, [this, coord]() {
        statusBar_->showMessage(tr("Validation request cancelled"), 3000);
        coord->deleteLater();
    });
    connect(coord, &pdfe::signatures::ValidationCoordinator::failed,
            this, [coord](const QString& reason) {
        Q_UNUSED(reason);
        coord->deleteLater();
    });

    if (!coord->request(this, document_.get())) {
        // request() returned false synchronously — coord didn't even
        // get to the dialog. failed() should already have fired.
        coord->deleteLater();
    }
}

void MainWindow::onImageToolSelected()
{
    if (!document_ || !imageManager_) return;

    // Create ImageTool if not already created
    if (!imageTool_) {
        imageTool_ = std::make_unique<pdfe::image::ImageTool>(imageManager_.get());

        connect(imageTool_.get(), &pdfe::image::ImageTool::imageSelected,
                this, [this](const pdfe::image::PageImageInfo& info) {
            if (imagePropertiesPanel_) {
                imagePropertiesPanel_->setImageInfo(info);
                imagePropertiesDock_->show();
            }
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::imageDeselected,
                this, [this]() {
            if (imagePropertiesPanel_) {
                imagePropertiesPanel_->clearInfo();
            }
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::removeRequested,
                this, [this](int imageIndex) {
            if (!document_ || !imageManager_) return;
            int pageIdx = canvas_->currentPage();
            auto page = document_->page(pageIdx);
            if (!page) return;

            auto cmd = std::make_unique<pdfe::image::RemoveImageCommand>(
                imageManager_.get(), page.get(), document_.get(), imageIndex);
            commandPages_.push_back(std::move(page));
            undoManager_->execute(std::move(cmd));
            canvas_->refreshPage(pageIdx);
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::resizeRequested,
                this, [this](const std::string& xobjectName, float w, float h, float x, float y) {
            if (!document_ || !imageManager_ || !imageContentEditor_) return;
            int pageIdx = canvas_->currentPage();
            auto page = document_->page(pageIdx);
            if (!page) return;

            auto cmd = std::make_unique<pdfe::image::ResizeImageCommand>(
                imageManager_.get(), imageContentEditor_.get(), page.get(), document_.get(),
                xobjectName, w, h, x, y);
            commandPages_.push_back(std::move(page));
            undoManager_->execute(std::move(cmd));
            canvas_->refreshPage(pageIdx);
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::rotateRequested,
                this, [this](const std::string& xobjectName, float angleDegrees) {
            if (!document_ || !imageManager_ || !imageContentEditor_ || !imageTool_) return;
            int pageIdx = canvas_->currentPage();
            auto page = document_->page(pageIdx);
            if (!page) return;

            const auto& img = imageTool_->selectedImage();
            float cx = img.boundingBox.center().x();
            float cy = img.boundingBox.center().y();
            auto cmd = std::make_unique<pdfe::image::RotateImageCommand>(
                imageManager_.get(), imageContentEditor_.get(), page.get(), document_.get(),
                xobjectName, angleDegrees, cx, cy, img.displayWidth, img.displayHeight);
            commandPages_.push_back(std::move(page));
            undoManager_->execute(std::move(cmd));
            canvas_->refreshPage(pageIdx);
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::cropRequested,
                this, [this](const std::string& xobjectName, QRectF cropRect) {
            if (!document_ || !imageManager_ || !imageContentEditor_ || !imageTool_) return;
            int pageIdx = canvas_->currentPage();
            auto page = document_->page(pageIdx);
            if (!page) return;

            const auto& img = imageTool_->selectedImage();
            auto cmd = std::make_unique<pdfe::image::CropImageCommand>(
                imageManager_.get(), imageContentEditor_.get(), page.get(), document_.get(),
                xobjectName, cropRect, img.displayWidth, img.displayHeight,
                static_cast<float>(img.boundingBox.x()),
                static_cast<float>(img.boundingBox.y()));
            commandPages_.push_back(std::move(page));
            undoManager_->execute(std::move(cmd));
            canvas_->refreshPage(pageIdx);
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::imageSelected,
                this, [this](const pdfe::image::PageImageInfo&) {
            imageRemoveAction_->setEnabled(true);
            imageRotateAction_->setEnabled(true);
            imageCropAction_->setEnabled(true);
            imageResizeAction_->setEnabled(true);
        });

        connect(imageTool_.get(), &pdfe::image::ImageTool::imageDeselected,
                this, [this]() {
            imageRemoveAction_->setEnabled(false);
            imageRotateAction_->setEnabled(false);
            imageCropAction_->setEnabled(false);
            imageResizeAction_->setEnabled(false);
        });
    }

    // Set the current page on the tool
    int pageIdx = canvas_->currentPage();
    auto page = document_->page(pageIdx);
    if (page) {
        toolPage_ = std::move(page);
        imageTool_->setPage(toolPage_.get(), document_.get());
    }

    // Activate image tool dispatch in canvas
    canvas_->clearCurrentTool();
    canvas_->clearTextEditTool();
    canvas_->setImageTool(imageTool_.get());
}

void MainWindow::onInsertImage()
{
    if (!document_ || !imageManager_) return;

    // The file-dialog seam, as the save paths already use. Without it this
    // function opens a real modal picker, so nothing headless can reach the
    // insert at all — which is why the placement defect and the missing
    // post-insert tool activation both went untested.
    QString filePath = pdfe::ui::testseam::takeFileDialogPath();
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getOpenFileName(this, tr("Insert Image"),
            QString(), tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tiff);;All Files (*)"));
    }
    if (filePath.isEmpty()) return;

    int pageIdx = canvas_->currentPage();
    auto page = document_->page(pageIdx);
    if (!page) return;

    // GitHub #242 — ask where it goes instead of deciding for the user.
    //
    // This was `opts.targetRect = QRectF(72, 72, 400, 300); // Default
    // placement` — a literal that ignored the page size, the page orientation
    // and the image's own proportions alike, so every image landed in the same
    // box. The other three options InsertImageOptions already accepted (JPEG
    // quality, EXIF stripping, aspect-ratio locking) were never surfaced; EXIF
    // stripping in particular was a privacy-relevant default applied invisibly.
    const QSizeF pageSize = page->boundingBox().size();
    QSize sourceSize;
    {
        QImageReader reader(filePath);
        sourceSize = reader.size();  // header only; does not decode the image
    }

    pdfe::image::InsertImageOptions opts;
    opts.filePath = filePath.toStdString();

    if (const auto chosen = promptInsertImageOptions(filePath, sourceSize, pageSize)) {
        opts.targetRect = chosen->targetRect;
        opts.jpegQuality = chosen->jpegQuality;
        opts.stripExif = chosen->stripExif;
        opts.maintainAspectRatio = chosen->maintainAspectRatio;
    } else {
        return;  // user cancelled
    }

    auto cmd = std::make_unique<pdfe::image::InsertImageCommand>(
        imageManager_.get(), page.get(), document_.get(), opts);
    commandPages_.push_back(std::move(page));
    undoManager_->execute(std::move(cmd));
    canvas_->refreshPage(pageIdx);

    // Put the user in front of the handles that already exist.
    //
    // ImageTool emits resizeRequested, rotateRequested and cropRequested, and
    // onImageToolSelected() connects all three to real commands — post-insert
    // manipulation has worked the whole time. What was missing is that nothing
    // took the user there, so after inserting they had to know to switch tools
    // and find the image themselves. Rotation in particular was reported as
    // absent when it was merely unreachable from this path.
    if (imageToolAction_ && imageToolAction_->isEnabled()) {
        imageToolAction_->setChecked(true);
        onAnnotationToolSelected(imageToolAction_);
    }
}

MainWindow::NetworkOpenDecision MainWindow::decideNetworkOpen(const QString& fileName)
{
    // GitHub #240 — does this network location answer, and are we allowed to ask?
    //
    // OFF THE GUI THREAD, and that is the entire reason this function exists
    // rather than a bare QFileInfo::exists(). Probing an unreachable UNC blocks
    // on the SMB redirector — measured at roughly a second for a bad hostname
    // and unbounded for a host that answers but does not serve the share. The
    // flat refusal this replaces was chosen partly to avoid exactly that hang,
    // so lifting the refusal without moving the probe would have traded a clear
    // message for a frozen window.
    //
    // The wait is bounded and the worker is abandoned rather than joined: a
    // std::future whose result nobody collects still completes in the
    // background, and the only thing it holds is a stat call. Abandoning it is
    // preferable to waiting on a call that has no timeout of its own.
    if (!appSettings_ || !appSettings_->allowUserNetworkPaths()) {
        return NetworkOpenDecision::Refused;
    }

    const int budgetMs = networkProbeTimeoutMsForTesting_ > 0
                             ? networkProbeTimeoutMsForTesting_
                             : 3000;

    auto answered = std::make_shared<std::atomic<bool>>(false);
    auto reachable = std::make_shared<std::atomic<bool>>(false);
    const std::wstring target = fileName.toStdWString();

    std::thread probe([answered, reachable, target]() {
        // Cheapest question that requires the redirector to actually respond.
        const bool ok = QFileInfo(QString::fromStdWString(target)).isFile();
        reachable->store(ok, std::memory_order_release);
        answered->store(true, std::memory_order_release);
    });
    probe.detach();

    QElapsedTimer timer;
    timer.start();
    while (!answered->load(std::memory_order_acquire) && timer.elapsed() < budgetMs) {
        // Keep the window alive while we wait. This is the difference between a
        // three-second pause and a three-second freeze.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }

    if (!answered->load(std::memory_order_acquire)) return NetworkOpenDecision::Unreachable;
    return reachable->load(std::memory_order_acquire) ? NetworkOpenDecision::Allowed
                                                      : NetworkOpenDecision::Unreachable;
}

QString MainWindow::stageLocalPreflightCopy(const QString& networkPath)
{
    // GitHub #240 — a local copy for the low-integrity preflight child to vet.
    //
    // See the call site for why the child cannot be handed the UNC path itself.
    // The copy lands in the per-user temp directory, which is local by
    // definition on any sane profile, and is removed when the returned
    // QTemporaryFile-backed holder is destroyed — held for the window's lifetime
    // rather than this function's, because the preflight runs after we return.
    auto holder = std::make_unique<QTemporaryFile>(
        QDir::temp().filePath(QStringLiteral("pdfe_preflight_XXXXXX.pdf")));
    holder->setAutoRemove(true);
    if (!holder->open()) return {};
    const QString localPath = holder->fileName();
    holder->close();

    QFile source(networkPath);
    if (!source.open(QIODevice::ReadOnly)) return {};
    QFile dest(localPath);
    if (!dest.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};

    // Streamed rather than read-all: a document on a share can be large, and
    // holding it twice in memory to check it would be its own defect.
    constexpr qint64 kChunk = 1 << 20;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kChunk);
        if (chunk.isEmpty()) break;
        if (dest.write(chunk) != chunk.size()) return {};
    }
    if (!dest.flush()) return {};
    dest.close();
    source.close();

    preflightStagingFiles_.push_back(std::move(holder));
    return localPath;
}

std::optional<MainWindow::InsertImageChoice>
MainWindow::promptInsertImageOptions(const QString& imagePath,
                                     QSize sourceSize,
                                     QSizeF pageSize)
{
    // Virtual so a widget test can answer without a real dialog — the same
    // pattern as promptSaveOnClose() and confirmFlattenForm(). A modal blocks
    // the event loop, so without this the insert path is untestable headlessly.
    InsertImageDialog dialog(imagePath, sourceSize, pageSize, this);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return InsertImageChoice{dialog.targetRect(), dialog.jpegQuality(),
                             dialog.stripExif(), dialog.maintainAspectRatio()};
}

void MainWindow::onStripExif()
{
    if (!document_ || !exifStripper_) return;

    int count = exifStripper_->stripAllImages(document_.get());

    if (count > 0) {
        QMessageBox::information(this, tr("Strip Image Metadata"),
            tr("Stripped metadata from %1 image(s).").arg(count));
        canvas_->refreshAllPages();
    } else {
        QMessageBox::information(this, tr("Strip Image Metadata"),
            tr("No images found to strip metadata from."));
    }
}

void MainWindow::onDocumentMetadata()
{
    if (!document_ || !metadataManager_) return;

    auto metadata = metadataManager_->readMetadata(document_.get());

    MetadataDialog dialog(this);
    dialog.setMetadata(metadata);

    if (dialog.exec() == QDialog::Accepted) {
        auto newMetadata = dialog.metadata();
        auto cmd = std::make_unique<pdfe::metadata::EditMetadataCommand>(
            metadataManager_.get(), document_.get(), newMetadata);
        undoManager_->execute(std::move(cmd));
    }
}

void MainWindow::onCompliance()
{
    if (!document_) return;

    ComplianceDialog dialog(muPdfContext_, document_.get(), this);
    pdfe::ui::testseam::registerDialogIntrospection(&dialog);
    dialog.exec();
}

// --- Edit menu slots ---

void MainWindow::onCopy()
{
    if (!document_ || !canvas_) return;
    // Enforce the document's copy/extract permission: an encrypted PDF whose
    // permission bits deny content extraction must not have its text placed
    // on the clipboard. documentDeniesCopy() combines the LIVE document
    // copy/extract permission (the mandatory floor) with the Prevent-Copy
    // toggle, so an encrypted PDF that denies extraction is refused even if the
    // toggle was cleared (R2-10).
    if (documentDeniesCopy()) {
        statusBar_->showMessage(
            tr("Copy is not permitted for this document"), 3000);
        return;
    }

    // R3-12: when the user has a region selected, copy honours that selection
    // — a rendered image of just the selected region — rather than dumping the
    // whole page's text. Falls through to the text path when no region is set.
    if (copySelectionValid_) {
        if (copySelectionAsImage()) {
            statusBar_->showMessage(
                tr("Selection copied to clipboard as image"), 3000);
        }
        return;
    }

    int pageIdx = canvas_->currentPage();
    auto page = document_->page(pageIdx);
    if (!page) return;

    std::string text = page->extractPlainText();
    if (text.empty()) {
        statusBar_->showMessage(tr("Copy: no text on this page"), 3000);
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(text));
    statusBar_->showMessage(tr("Page text copied to clipboard"), 3000);
}

void MainWindow::setCopySelectionRect(int pageIndex, const QRectF& pdfRect)
{
    if (pageIndex < 0 || pdfRect.isEmpty()) {
        copySelectionValid_ = false;
        copySelectionPage_ = -1;
        copySelectionRect_ = QRectF();
        return;
    }
    copySelectionPage_ = pageIndex;
    copySelectionRect_ = pdfRect;
    copySelectionValid_ = true;
}

bool MainWindow::copySelectionAsImage()
{
    if (!document_ || !copySelectionValid_) return false;
    // Same copy/extract permission floor as text copy (R2-10): an encrypted
    // PDF that denies extraction must not have its rendered pixels exfiltrated
    // either.
    if (documentDeniesCopy()) {
        if (statusBar_) {
            statusBar_->showMessage(
                tr("Copy is not permitted for this document"), 3000);
        }
        return false;
    }

    auto page = document_->page(copySelectionPage_);
    if (!page) return false;

    // Page::render() rasterises the WHOLE page (it ignores its region arg), so
    // render at 2x for crispness then crop to the selection.
    //
    // R3-12 coordinate mapping (fixed):
    //   * Page::render() → fz_new_pixmap_from_page(fz_scale(scale)): the image's
    //     pixel (0,0) is the page-bounds TOP-LEFT corner, i.e. it maps to
    //     (pageBox.left(), pageBox.top()) in the page's fitz coordinate frame.
    //   * copySelectionRect_ is in that SAME fitz page frame — it originates
    //     from pdf_bound_annot / SelectTool (drawn on screen with NO Y-flip:
    //     SelectTool::pdfToScreen maps y = pageOrigin.y() + r.top()*zoom), so
    //     the frame is Y-DOWN with the page top at the smaller y == pageBox.y().
    //   * The bounds origin is subtracted on both axes (image-pixel (0,0) maps to
    //     pageBox.left()/top()). NOTE: MuPDF's fz_bound_page NORMALISES a PDF page
    //     to a 0-origin frame — even a MediaBox like [100 100 700 900] reports
    //     pageBox.x()/y() == 0 — so this subtraction is a no-op TODAY. It is kept
    //     as correct-by-construction defence in case the render/selection frame
    //     ever preserves a non-zero origin; it must never be claimed as "the fix".
    //
    // The ACTUAL defect this fixes is the Y-FLIP: the old code did
    // pxY = (pageH - selY - selH)*scale, mirroring an already-Y-DOWN selection so
    // a top-of-page selection copied the BOTTOM of the page. copySelectionRect_ is
    // Y-DOWN fitz page space (SelectTool::pdfToScreen has no flip), the same frame
    // as the rendered image, so NO flip is correct. The old X term (selX*scale)
    // was numerically fine only because pageBox.x()==0. Neither error changed the
    // crop SIZE, so the size-only R3-12 tests never caught the flip. The
    // vertical-orientation oracle in tests/test_mediabox_origin_crop.cpp does.
    const float scale = 2.0f;
    QImage full = page->render(copySelectionRect_, scale);
    if (full.isNull()) return false;

    const QRectF pageBox = page->boundingBox();
    const double pageW = pageBox.width();
    const double pageH = pageBox.height();
    if (pageW <= 0.0 || pageH <= 0.0) return false;

    const double sx = static_cast<double>(full.width()) / pageW;
    const double sy = static_cast<double>(full.height()) / pageH;

    // Subtract the page-bounds origin so the image left/top edges (which map to
    // pageBox.left()/pageBox.top()) are the crop reference. No Y-flip: the
    // selection is already in the image's Y-DOWN frame.
    const double pxX = (copySelectionRect_.x() - pageBox.x()) * sx;
    const double pxY = (copySelectionRect_.y() - pageBox.y()) * sy;
    const double pxW = copySelectionRect_.width() * sx;
    const double pxH = copySelectionRect_.height() * sy;

    QRect cropPx(static_cast<int>(pxX + 0.5), static_cast<int>(pxY + 0.5),
                 static_cast<int>(pxW + 0.5), static_cast<int>(pxH + 0.5));
    cropPx = cropPx.intersected(full.rect());
    if (cropPx.isEmpty()) return false;

    QImage cropped = full.copy(cropPx);
    if (cropped.isNull()) return false;

    QClipboard* cb = QApplication::clipboard();
    if (!cb) return false;
    // On Windows, QClipboard::setImage exposes the image as CF_DIB / CF_BITMAP
    // to other applications automatically.
    cb->setImage(cropped);
    return true;
}

bool MainWindow::selectAnnotationRegionForTesting(int pageIndex, int annotIndex)
{
    if (!canvas_ || !selectToolAction_) return false;
    // Activate the Select tool through the real action so MainWindow installs
    // the production selectTool->regionSelected wiring and sets the tool page.
    selectToolAction_->trigger();
    // Drive the LIVE tool's selection (currentTool_->selectByIndex) — this fires
    // SelectTool::regionSelected → setCopySelectionRect.
    canvas_->selectAnnotationInSelectTool(pageIndex, annotIndex);
    return true;
}

int MainWindow::pageImageCountForTesting(int pageIndex)
{
    if (!document_ || !imageManager_) return -1;
    auto page = document_->page(pageIndex);
    if (!page) return -1;
    return imageManager_->imageCount(page.get(), document_.get());
}

bool MainWindow::pasteImageFromClipboard()
{
    if (!document_ || !imageManager_ || !canvas_ || !undoManager_) return false;

    QClipboard* cb = QApplication::clipboard();
    if (!cb) return false;
    QImage img = cb->image();
    if (img.isNull()) return false;

    // The InsertImage pipeline consumes a file path; materialise the clipboard
    // image to a temp PNG and feed it through the exact same undoable command
    // as File > Insert Image (incl. EXIF strip). The temp file is intentionally
    // left on disk: InsertImageCommand::redo() re-reads the source file, so
    // deleting it would break redo after an undo.
    //
    // WIN-87 WIDER CLASS (phase-60) — AND THE "LEFT ON DISK" ABOVE IS WHY THIS
    // ONE WAS THE WORST OF THE FAMILY. The name used to be
    // "pdfe_clip_paste_<n>.png" with a counter that RESTARTS AT 0 EVERY
    // PROCESS, so the first paste of every session was always
    // pdfe_clip_paste_0.png. And because redo() re-reads the file BY PATH after
    // every undo/redo, an attacker who lost the first race did not need another
    // one: they could swap the file at leisure and wait for the user to press
    // Ctrl+Z, Ctrl+Y. The image that then entered the document was theirs.
    //
    // Unpredictability alone does not close that, precisely because the path
    // stays live for the whole session. So the file is HELD — opened once,
    // exclusively, sharing READ and nothing else, so there is never a writer,
    // deleter or renamer other than us — and that hold is kept by
    // `pinnedPastedImages_` for the lifetime of this window, which is exactly
    // as long as an undo stack can reach back to it.
    std::error_code pasteEc;
    auto staged = core::PinnedTempFile::reserve(
        std::filesystem::path(QDir::tempPath().toStdWString()),
        "pdfe_clip_paste", ".png", pasteEc);
    if (!staged) return false;
    const QString tmpPath = QString::fromStdWString(staged->path().wstring());

    // WRITTEN THROUGH THE HELD HANDLE, not by name. `staged` keeps the PNG open
    // exclusively for the lifetime of this window, so from the instant it
    // exists there is no moment at which another process can write, delete or
    // rename it -- which is what makes redo()'s by-path re-read safe however
    // long after the paste it happens.
    //
    // An earlier draft closed the handle, let QImage::save open the path by
    // name, and then re-opened and compared the file's identity. The phase-7
    // karen gate rejected it and was right: an attacker who opens the SAME file
    // object and overwrites it in place leaves identity untouched, and that is
    // precisely what the legitimate writer does, so the check could not
    // distinguish them. The window is removed rather than watched.
    std::error_code fdEc;
    const int fd = staged->openWriteFd(fdEc);
    if (fd < 0) return false;
    bool saved = false;
    {
        QFile out;
        if (out.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            saved = img.save(&out, "PNG");
            out.close();
        } else {
            ::_close(fd);
        }
    }
    if (!saved) return false;

    const int pageIdx = canvas_->currentPage();
    auto page = document_->page(pageIdx);
    if (!page) return false;

    pdfe::image::InsertImageOptions opts;
    opts.filePath = tmpPath.toStdString();
    // Place at 1pt-per-pixel near the top-left margin; a real selection-aware
    // placement is wired separately. targetRect is in PDF coordinates.
    opts.targetRect = QRectF(72, 72,
                             static_cast<double>(img.width()),
                             static_cast<double>(img.height()));
    opts.stripExif = true;

    auto cmd = std::make_unique<pdfe::image::InsertImageCommand>(
        imageManager_.get(), page.get(), document_.get(), opts);
    commandPages_.push_back(std::move(page));
    undoManager_->execute(std::move(cmd));
    // Held, not dropped: releasing the pin here would reopen the substitution
    // window for every later redo() re-read.
    pinnedPastedImages_.push_back(std::move(staged));
    canvas_->refreshPage(pageIdx);
    return true;
}

void MainWindow::onCut()
{
    if (canvas_) {
        auto* tool = canvas_->textEditTool();
        if (tool && tool->hasTextSelection()) {
            tool->performCut();
            statusBar_->showMessage(tr("Text cut to clipboard"), 3000);
            return;
        }
    }
    statusBar_->showMessage(tr("Cut: select text in an editable span first"), 3000);
}

void MainWindow::onPaste()
{
    // R3-12: if the clipboard holds an image and no text-edit session is
    // active, paste the image INTO the current page. A live text-edit session
    // keeps priority for text paste so image data never clobbers an in-progress
    // text edit.
    const bool textEditing =
        canvas_ && canvas_->textEditTool() && canvas_->textEditTool()->isEditing();
    if (!textEditing) {
        QClipboard* cb = QApplication::clipboard();
        if (cb && !cb->image().isNull()) {
            if (pasteImageFromClipboard()) {
                statusBar_->showMessage(tr("Image pasted into page"), 3000);
            } else {
                statusBar_->showMessage(tr("Paste: could not insert image"), 3000);
            }
            return;
        }
    }

    if (canvas_) {
        auto* tool = canvas_->textEditTool();
        if (tool) {
            tool->performPaste();
            statusBar_->showMessage(tr("Clipboard text pasted"), 3000);
            return;
        }
    }
    // Fallback: if no text edit tool, paste is a no-op at page level
    statusBar_->showMessage(tr("Paste: activate text editing first"), 3000);
}

void MainWindow::onSelectAll()
{
    if (!document_ || !canvas_) return;

    // Pipeline a3f8e21d phase-2 (bug 5): selectAllAction_ owns the
    // QKeySequence::SelectAll shortcut, so Ctrl+A fires this slot even while a
    // text-edit session is active — shadowing TextEditTool's own Ctrl+A
    // handler. In that state the user means "select all text in the span I am
    // editing", not "copy the whole page to the clipboard". Route to the active
    // tool so the global QAction no longer swallows in-edit Select All. Only
    // while actually editing; otherwise the document-level behaviour below runs.
    if (auto* tet = canvas_->textEditTool(); tet && tet->isEditing()) {
        tet->performSelectAll();
        canvas_->update();
        return;
    }

    // Select All copies the page text to the clipboard, so it is subject to
    // the same copy/extract permission as Copy. Refuse when the document
    // denies content extraction (live permission OR the Prevent-Copy toggle).
    if (documentDeniesCopy()) {
        statusBar_->showMessage(
            tr("Copy is not permitted for this document"), 3000);
        return;
    }
    int pageIdx = canvas_->currentPage();
    auto page = document_->page(pageIdx);
    if (!page) return;

    std::string text = page->extractPlainText();
    if (text.empty()) {
        statusBar_->showMessage(tr("Select All: no text on this page"), 3000);
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(text));
    statusBar_->showMessage(tr("All page text selected and copied to clipboard"), 3000);
}

// --- Image menu slots ---

void MainWindow::onImageRemove()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    emit imageTool_->removeRequested(img.imageIndex);
}

void MainWindow::onImageRotate90()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    emit imageTool_->rotateRequested(img.xobjectName, 90.0f);
}

void MainWindow::onImageRotate180()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    emit imageTool_->rotateRequested(img.xobjectName, 180.0f);
}

void MainWindow::onImageRotate270()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    emit imageTool_->rotateRequested(img.xobjectName, 270.0f);
}

void MainWindow::onImageCrop()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    // Use center 80% as default crop region
    float w = static_cast<float>(img.boundingBox.width()) * 0.8f;
    float h = static_cast<float>(img.boundingBox.height()) * 0.8f;
    float x = static_cast<float>(img.boundingBox.x()) + static_cast<float>(img.boundingBox.width()) * 0.1f;
    float y = static_cast<float>(img.boundingBox.y()) + static_cast<float>(img.boundingBox.height()) * 0.1f;
    emit imageTool_->cropRequested(img.xobjectName, QRectF(x, y, w, h));
}

void MainWindow::onImageResize()
{
    if (!imageTool_ || !imageTool_->hasSelection()) return;
    const auto& img = imageTool_->selectedImage();
    // Use 50% scale as default resize
    float newW = img.displayWidth * 0.5f;
    float newH = img.displayHeight * 0.5f;
    emit imageTool_->resizeRequested(img.xobjectName, newW, newH,
        static_cast<float>(img.boundingBox.x()),
        static_cast<float>(img.boundingBox.y()));
}

// --- Pipeline E phase-10: Customizable toolbars ---

std::vector<QAction*> MainWindow::buildToolbarActionRegistry() const
{
    // Curated set of actions that the user can place on toolbars.
    // The order here drives the "Available actions" list ordering in
    // the dialog (which is then sorted alphabetically by display name).
    std::vector<QAction*> reg;
    auto add = [&](QAction* a) { if (a) reg.push_back(a); };

    // File / save
    add(openAction_); add(saveAction_); add(saveAsAction_);
    add(printAction_); add(printPreviewAction_);
    // Edit
    add(undoAction_); add(redoAction_);
    add(copyAction_); add(cutAction_); add(pasteAction_);
    add(findAction_); add(replaceAction_);
    // View / zoom / rotate
    add(zoomInAction_); add(zoomOutAction_);
    add(fitWidthAction_); add(fitPageAction_); add(actualSizeAction_);
    add(rotateViewAction_); add(rotateViewCCWAction_);
    add(fullScreenAction_);
    // Annotation tools
    add(selectToolAction_); add(stickyNoteToolAction_);
    add(highlightToolAction_); add(underlineToolAction_); add(strikeOutToolAction_);
    add(freeTextToolAction_); add(rectangleToolAction_);
    add(circleToolAction_); add(lineToolAction_);
    add(inkToolAction_);
    add(imageToolAction_); add(textEditToolAction_);
    add(redactToolAction_); add(formToolAction_);

    // GitHub #253 — the Link tool, resolved by objectName rather than held in a
    // member.
    //
    // THIS LINE IS WHAT MAKES THE TOOL ACTUALLY APPEAR. Registering it is not
    // cosmetic: `loadSavedToolbarLayout()` runs right after
    // createAnnotationToolBar() and, when a saved layout exists, rebuilds each
    // toolbar from (saved layout ∩ THIS REGISTRY) via
    // ToolbarLayoutManager::applyLayoutToToolbar. An action that is on the
    // toolbar but absent from the registry is therefore DROPPED on any machine
    // that has ever saved a layout -- which is every machine that has run the
    // app for a while.
    //
    // Measured, which is how this was found: with the tool added to the toolbar
    // but not here, a live UIA dump listed exactly the fourteen registry
    // actions and no `linkToolActionButton`. The static reachability guard was
    // perfectly happy -- `make_unique<LinkTool>` is a real code reference from a
    // reachable file -- which is precisely why the live test in
    // tests/test_ui_issue253_wired_components.py exists alongside it. "Reachable
    // in the call graph" and "reachable by the user" are two claims.
    //
    // findChild rather than a new QAction* member: the action's parent is
    // `this`, so this resolves it, and MainWindow's layout is left alone.
    add(findChild<QAction*>(QStringLiteral("linkToolAction")));

    return reg;
}

void MainWindow::applyToolbarLayoutsList(
    const std::vector<ToolbarLayout>& layouts)
{
    const std::vector<QAction*> registry = buildToolbarActionRegistry();
    for (const auto& layout : layouts) {
        QToolBar* tb = findChild<QToolBar*>(layout.toolbarName);
        if (!tb) continue;
        ToolbarLayoutManager::applyLayoutToToolbar(tb, layout, registry);
    }
}

// GitHub #309 / p97 phase-9. Both ToolbarLayoutManager construction sites go
// through here so the saved-layout store honours the SAME isolation seam as the
// rest of the app.
//
// It did not, and that was a hole in the test seam rather than a cosmetic
// inconsistency: `PDFE_SETTINGS_PATH` moves AppSettings and ThemeManager onto a
// throwaway INI, and the whole point is that a test run cannot see or disturb
// the developer's real configuration. `ToolbarLayoutManager` was constructed
// bare, so it read HKCU regardless -- meaning a UI test on an isolated profile
// still inherited whatever toolbar layout this machine happened to have saved.
//
// MEASURED: that made `tests/test_ui_issue253_wired_components.py` report
// `linkToolActionButton` unreachable on this machine (14 toolbar buttons from
// the saved layout) while the same build showed 17 with the layout cleared. A
// test whose result depends on un-isolated machine state is not measuring the
// product, and the first instinct -- have the test skip when it detects a saved
// layout -- would have added two more WIN-45-shaped reachability skips, which is
// the exact false-green shape `check_reachability_skip_shape.py` exists to
// forbid. The guard caught both. Closing the seam removes the need for either.
static QString toolbarLayoutSettingsPath(const pdfe::app::AppSettings* s)
{
    // Empty string means "use the default location", which is precisely what
    // AppSettings::settingsFilePath() returns when no override is in use, so the
    // two contracts line up and no branch is needed here.
    return s ? s->settingsFilePath() : QString();
}

void MainWindow::loadSavedToolbarLayout()
{
    ToolbarLayoutManager mgr;
    mgr.setSettingsFilePath(toolbarLayoutSettingsPath(appSettings_.get()));
    if (!mgr.hasSavedLayout()) return;
    auto layouts = mgr.loadLayouts();
    if (layouts.empty()) return;

    // GitHub #309. RECONCILE BEFORE APPLYING. A saved layout names only the
    // actions that existed when it was written, and applyLayoutToToolbar()
    // rebuilds each toolbar from exactly that list -- so every action added in
    // a later release was dropped for any user who had ever customised a
    // toolbar. Not overflowed: absent, with no route to it short of Customize
    // or deleting the key.
    //
    // Measured on this machine before the fix: the annotation toolbar carried
    // 14 buttons with a saved layout present and 17 without, and
    // `linkToolActionButton` -- one of four controls p97 had just added -- was
    // absent from the UIA tree at any window width.
    //
    // reconcileWithRegistry appends only actions the layout has never seen
    // (in neither its id list nor its knownActionIds), so deliberately-removed
    // actions stay removed. See its header for what a schema-1 layout, which
    // cannot express that distinction, does instead and why.
    const int appended =
        ToolbarLayoutManager::reconcileWithRegistry(layouts,
                                                    buildToolbarActionRegistry());
    if (appended > 0) {
        qInfo("ToolbarLayoutManager: %d action(s) added since this layout was "
              "saved have been appended to it (GitHub #309)", appended);
    }
    applyToolbarLayoutsList(layouts);
}

void MainWindow::onCombineFiles()
{
    CombineFilesWizard wiz(muPdfContext_, this);
    wiz.exec();
}

void MainWindow::onCustomizeToolbars()
{
    const std::vector<QAction*> registry = buildToolbarActionRegistry();

    // Seed dialog from live toolbar state so unsaved tweaks survive.
    std::vector<ToolbarLayout> initial;
    for (const QString& name : {QStringLiteral("mainToolBar"),
                                 QStringLiteral("annotationToolBar")}) {
        QToolBar* tb = findChild<QToolBar*>(name);
        if (!tb) continue;
        initial.push_back(ToolbarLayoutManager::captureFromToolbar(tb));
    }
    if (initial.empty()) {
        initial = ToolbarLayoutManager::defaultLayouts();
    }

    CustomizeToolbarsDialog dlg(registry, initial, this);
    if (dlg.exec() != QDialog::Accepted) return;

    auto updated = dlg.updatedLayouts();
    applyToolbarLayoutsList(updated);

    // GitHub #309. Record what EXISTED at save time, not just what the user
    // kept. This is the half that makes the distinction possible on the next
    // load: an action missing from `actionIds` but present here was removed on
    // purpose and must stay off, while one in neither list postdates the save
    // and must be shown. Without it every future release's new tools would be
    // invisible to this user, which is the defect #309 reports.
    const std::vector<QAction*> savedRegistry = buildToolbarActionRegistry();
    QStringList knownIds;
    knownIds.reserve(static_cast<qsizetype>(savedRegistry.size()));
    for (QAction* a : savedRegistry) {
        if (a && !a->objectName().isEmpty()) knownIds.push_back(a->objectName());
    }
    for (auto& layout : updated) {
        layout.knownActionIds = knownIds;
    }

    // Same seam as the load path -- and this direction matters more: writing to
    // HKCU from a test run would mutate the developer's real toolbar layout as
    // a side effect of a test.
    ToolbarLayoutManager saver;
    saver.setSettingsFilePath(toolbarLayoutSettingsPath(appSettings_.get()));
    saver.saveLayouts(updated);
}

} // namespace pdfe::ui
