#pragma once

#include <memory>
#include <QRectF>
#include <QString>

class QPrinter;
class QPainter;

namespace pdfe::core {

class Document;

/// Why the last print attempt failed. `None` after a successful print.
///
/// GitHub #78 / WIN-28: `print()` used to report every failure as a bare
/// `false`, so the only thing the UI could say was "Failed to print the
/// document." — a dead end for a user whose real problem is that the Windows
/// Print Spooler service is stopped (or that no printer is installed at all).
/// Each enumerator below pairs with an actionable sentence from
/// `lastErrorMessage()`.
enum class PrintError {
    None = 0,
    NoDocument,        ///< No document / no printer object was supplied.
    PrintNotPermitted, ///< The document's owner password denies printing.
    InvalidPageRange,  ///< The requested from/to range is empty or inverted.
    NoPrinterAvailable,///< Windows enumerated zero print devices.
    DeviceOpenFailed,  ///< The print device refused to start the job.
    PageEmitFailed,    ///< A page could not be started or rendered.
};

enum class PrintRange {
    AllPages,
    CurrentPage,
    PageRange
};

struct PrintOptions {
    PrintRange range = PrintRange::AllPages;
    int fromPage = 1;     // 1-based page numbers
    int toPage = 1;
    int copies = 1;
    bool fitToPage = true;
    bool centerOnPage = true;
    bool autoRotate = true;
};

class PrintEngine {
public:
    explicit PrintEngine(Document* document);
    ~PrintEngine();

    bool print(QPrinter* printer, const PrintOptions& options);
    bool printPage(QPainter* painter, int pageIndex, const QRectF& targetRect, const PrintOptions& options);

    /// The category of the most recent failure (PrintError::None after success).
    PrintError lastError() const;

    /// A complete, translated, user-facing sentence explaining the most recent
    /// failure and what the user can do about it. Empty after a success.
    /// This is the string the UI shows verbatim; it never contains a raw
    /// error code, HRESULT or enumerator name.
    QString lastErrorMessage() const;

    PrintEngine(const PrintEngine&) = delete;
    PrintEngine& operator=(const PrintEngine&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
