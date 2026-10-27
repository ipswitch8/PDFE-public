#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include "Error.h"

namespace pdfe::core {

/// Result of a structural integrity check (the "qpdf --check" equivalent).
/// @c ok is true only when a FULL parse of the object graph + xref (+ the
/// linearization tables, when the file is linearized) surfaced no errors or
/// warnings. When false, @c problems lists every issue collected — the caller
/// gets the whole picture rather than only the first thrown error.
struct StructureCheck {
    bool ok = false;
    std::vector<std::string> problems;
};

/// RAII wrapper for QPDF object.
///
/// In the PDFE architecture, MuPDF owns the document model. QPDF is used
/// for specific structural operations:
/// - Incremental saves (preserving digital signatures)
/// - Linearization (web-optimized output)
/// - Page-level merge/split operations
/// - Object graph inspection/repair
class QpdfWrapper {
public:
    QpdfWrapper() = default;
    ~QpdfWrapper() = default;

    // Non-copyable (QPDF is non-copyable)
    QpdfWrapper(const QpdfWrapper&) = delete;
    QpdfWrapper& operator=(const QpdfWrapper&) = delete;

    // Movable
    QpdfWrapper(QpdfWrapper&&) noexcept = default;
    QpdfWrapper& operator=(QpdfWrapper&&) noexcept = default;

    /// Open a PDF file for structural operations.
    /// @param path Path to the PDF file
    /// @param password Optional password for encrypted PDFs
    void open(const std::filesystem::path& path, const std::string& password = "");

    // -----------------------------------------------------------------------
    // The by-HANDLE pair — WIN-87 phase-57.
    // -----------------------------------------------------------------------
    //
    // open()/save() above take PATHS, and every path is re-resolved at the
    // moment it is used. Both redaction rewrite sites used them in sequence —
    // MuPDF (or Document::save) produced a file, QPDF reopened it BY NAME, and
    // QPDF wrote its result to another name that was later renamed over the
    // user's document. An attacker who won either name had attacker bytes
    // published as "the redacted document", with the save reporting success.
    //
    // These two take an already-open stdio stream instead, so the caller can
    // resolve the name ONCE — while it still holds the bytes it produced or
    // while the document still holds the file open — and hand the resulting
    // handle straight through. QPDF supports this natively
    // (QPDF::processFile(desc, FILE*, close_file) and the QPDFWriter FILE*
    // constructor); nothing here is a workaround.

    /// Parse a PDF from an already-open, seekable, binary stream. TAKES
    /// OWNERSHIP: QPDF fcloses `file` when this wrapper is destroyed, so the
    /// caller must not. `description` appears in QPDF's diagnostics in place
    /// of a filename — pass the path it came from, which is what a user needs
    /// to read the error, NOT a name anything re-resolves.
    ///
    /// The stream must stay open for the LIFETIME of this wrapper: QPDF reads
    /// objects lazily, so the bytes are still being pulled through it during
    /// saveToStream().
    void openFromStream(std::FILE* file, const std::string& description,
                        const std::string& password = "");

    /// Full-rewrite save into an already-open, seekable, binary stream — the
    /// by-handle counterpart of save(). TAKES OWNERSHIP of `file` and fcloses
    /// it when the write finishes, which is also what flushes it; when `file`
    /// is a duplicate of a StagingFile handle (the expected caller), that
    /// closes the DUPLICATE only and the staging file's own exclusive handle
    /// is untouched and still spans the window through to publication.
    void saveToStream(std::FILE* file, bool linearize = false);

    /// saveToStream() with the PDF/A structural settings the compliance
    /// converters need — WIN-87 phase-59.
    ///
    /// Encryption is stripped (`setPreserveEncryption(false)`) and object
    /// streams are disabled or generated per @p disableObjectStreams, which is
    /// what PDF/A-1 vs PDF/A-2+ requires. It exists as a named method rather
    /// than flags on saveToStream so the converters cannot drift apart from
    /// each other, and so "what PDF/A needs from QPDF" is stated in one place.
    ///
    /// Linearization is off, as it was before: a linearized PDF/A is legal but
    /// the converters have never produced one, and changing that here would be
    /// an unrelated behaviour change riding along with a security fix.
    void savePdfAToStream(std::FILE* file, bool disableObjectStreams);

    /// Save with full rewrite (non-incremental).
    /// @param path Output path
    /// @param linearize Whether to linearize (web-optimize) the output
    void save(const std::filesystem::path& path, bool linearize = false);

    /// Save incrementally (append changes, preserving digital signatures).
    /// @param path Output path (typically same as input)
    void saveIncremental(const std::filesystem::path& path);

    /// Check if a file is open.
    [[nodiscard]] bool isOpen() const noexcept { return qpdf_ != nullptr; }

    /// Get page count.
    [[nodiscard]] int pageCount() const;

    /// Structural integrity check on the currently-open file (the reusable
    /// "qpdf --check" equivalent that R3-05 / R3-10 verification calls for).
    ///
    /// Forces a full resolution of the object graph and cross-reference table
    /// by walking every page (getAllPages) and every indirect object
    /// (getAllObjects) — either surfaces a broken xref or dangling reference —
    /// and, for linearized files, runs checkLinearization(). QPDF warnings
    /// (e.g. a recovered-but-damaged xref) are collected too, not just hard
    /// errors. Problems are accumulated rather than only thrown, so a single
    /// call reports every issue.
    [[nodiscard]] StructureCheck checkStructure() const;

    /// Convenience: open @p path in a throwaway wrapper and run checkStructure()
    /// on it. A failed open is itself reported as a problem (ok == false).
    [[nodiscard]] static StructureCheck checkFile(
        const std::filesystem::path& path, const std::string& password = "");

    /// Get the underlying QPDF object for advanced operations.
    [[nodiscard]] QPDF& qpdf() { return *qpdf_; }
    [[nodiscard]] const QPDF& qpdf() const { return *qpdf_; }

private:
    std::unique_ptr<QPDF> qpdf_;
    std::filesystem::path currentPath_;
};

} // namespace pdfe::core
