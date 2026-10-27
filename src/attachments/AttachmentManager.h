#pragma once

#include "AttachmentData.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::attachments {

/// CRUD for /EmbeddedFiles (a.k.a. attachments / file attachments).
///
/// The PDF catalog's /Names /EmbeddedFiles name tree holds filespecs;
/// each filespec points to an embedded file stream. This manager provides:
///   - list: return the current entries
///   - add: embed a file from disk (writes to a name tree entry)
///   - extract: write an embedded file's bytes to disk (byte-exact)
///   - remove: delete a name-tree entry
///
/// The sanitizeFilename() helper is exposed for testing; it strips path
/// separators and traversal sequences so extract() can't be tricked into
/// writing outside the caller's chosen directory.
class AttachmentManager {
public:
    explicit AttachmentManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~AttachmentManager();

    /// Return the current attachment list for the given document. Does
    /// not take ownership of the document.
    std::vector<AttachmentEntry> list(pdfe::core::Document* doc);

    /// Embed `file` into `doc` under the display `name` (also used as the
    /// /UF filename). Returns true on success. Marks the document dirty.
    bool add(pdfe::core::Document* doc,
             const std::filesystem::path& file,
             const std::string& name);

    /// Extract the attachment at the given name-tree key to `outDir`.
    /// The output filename is the filespec's /UF sanitized via
    /// sanitizeFilename() so path traversal can't escape outDir.
    /// Returns the written path on success, or an empty path on failure.
    std::filesystem::path extract(pdfe::core::Document* doc,
                                  const std::string& name,
                                  const std::filesystem::path& outDir);

    /// Remove the attachment with the given name-tree key.
    bool remove(pdfe::core::Document* doc, const std::string& name);

    /// Sanitize a filename for safe use in a filesystem path. Strips path
    /// separators (`/`, `\\`), drive letters, and dot-segments (`..`).
    /// Exposed for testing.
    static std::string sanitizeFilename(const std::string& raw);

    /// Largest attachment, in bytes, that extract() will write to disk.
    /// Default 256 MiB.
    ///
    /// A real API rather than a test hook: `pdf_load_embedded_file_contents`
    /// decompresses before this class sees anything, so the cap does not bound
    /// the decompression — it bounds what gets SPILLED. An attachment a few KB
    /// on the wire can expand to gigabytes, and a caller extracting into a
    /// constrained location (a temp volume, a user quota) has a legitimate
    /// reason to want a tighter bound than the default.
    void setMaxExtractBytes(std::size_t bytes) { maxExtractBytes_ = bytes; }
    std::size_t maxExtractBytes() const { return maxExtractBytes_; }

    /// True when the most recent list()/extract() stopped descending the
    /// /EmbeddedFiles name tree early — because /Kids exceeded the depth cap or
    /// because it referenced a node already on the path (a cycle).
    ///
    /// This exists because a guard whose only effect is "did not crash" is
    /// indistinguishable from a guard that never ran. When it is true the
    /// returned attachment list is a PREFIX of what the document claims, not the
    /// whole of it, and a caller that presents the list as complete is lying to
    /// the user about a document that was built to make it do so. Callers that
    /// do not care may ignore it; the point is that they CAN know.
    ///
    /// Reset at the start of each list()/extract() call.
    bool lastTraversalTruncated() const { return lastTraversalTruncated_; }

    /// True when the most recent extract() found the attachment and then
    /// refused to write it because its decompressed size exceeded
    /// maxExtractBytes().
    ///
    /// extract() reports every failure the same way — an empty path — so
    /// without this a size refusal is indistinguishable from "no such
    /// attachment" or "the stream would not load". Those want different words
    /// in front of a user: one is a missing thing, the other is a policy this
    /// application applied and can relax.
    ///
    /// Reset at the start of each extract() call.
    bool lastExtractRefusedOversize() const { return lastExtractRefusedOversize_; }

    /// Portfolio (a.k.a. "package") detection: a PDF whose /Catalog has
    /// a /Collection dict is intended to be shown as a container of
    /// its /EmbeddedFiles rather than as a single page-body document.
    /// Returns true when /Collection is present.
    bool isPortfolio(pdfe::core::Document* doc);

    /// Sort key stored in the /Collection /View entry: "D" (details
    /// pane, the default in Acrobat), "T" (tile/icon grid), "H"
    /// (hidden). Returns "D" when the document isn't a portfolio or
    /// the /View value is missing/unknown.
    std::string portfolioView(pdfe::core::Document* doc);

private:
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::size_t maxExtractBytes_ = 256ull * 1024 * 1024;
    bool lastTraversalTruncated_ = false;
    bool lastExtractRefusedOversize_ = false;
};

} // namespace pdfe::attachments
