#include "ImageExporter.h"
#include "../core/Document.h"
#include "../core/Page.h"
#include "../core/AtomicFileReplace.h"

#include <QFile>
#include <QImage>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <system_error>

namespace pdfe::exports {

namespace {

std::string formatExtension(ImageFormat fmt)
{
    switch (fmt) {
        case ImageFormat::Png:  return "png";
        case ImageFormat::Jpeg: return "jpg";
    }
    return "png";
}

const char* qtFormatTag(ImageFormat fmt)
{
    switch (fmt) {
        case ImageFormat::Png:  return "PNG";
        case ImageFormat::Jpeg: return "JPG";
    }
    return "PNG";
}

std::string zeroPad(int value, int width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", width, value);
    return buf;
}

std::string replaceAll(std::string s, const std::string& needle, const std::string& repl)
{
    if (needle.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return s;
}

/// Atomic write through an EXCLUSIVE HANDLE — WIN-87 site #6, phase-58.
///
/// This used to render to "<finalPath>.part" BY NAME, close it, and then hand
/// that name to the shared replace helper. Both steps take PATHS, so between
/// the last write and the rename the staging artifact sat unheld at a name
/// derived from the caller's own output path: an attacker who knows where you
/// are exporting knows the temp's name, and can substitute their own image
/// bytes into it and have PDFE publish them as the user's export.
///
/// F10 / WIN-85 hardened the DATA-LOSS half of this and its comment survives
/// below, because that fix is still in force and still correct: nothing is ever
/// removed first, so on any failure `finalPath` is completely untouched. What
/// WIN-85 could not fix is that the helper it routed to takes paths.
///
/// Now: a StagingFile (CREATE_NEW, exclusive handle, 128-bit CSPRNG name beside
/// the target), rendered into through a DUPLICATE of that still-held handle via
/// QFile::open(FILE*), and published by renaming THROUGH the handle. There is no
/// interval in which the artifact has a guessable name, and none in which it is
/// unheld.
///
/// WINDOWS-ONLY PROPERTY, stated rather than implied: `commitOverWithRetry`'s
/// POSIX arm has no mandatory share mode, so the substitution property does not
/// hold there. The product is Windows-only today.
bool writeImageAtomic(const QImage& img, const std::filesystem::path& finalPath,
                      ImageFormat fmt, int jpegQuality, std::string& err)
{
    std::error_code ec;
    auto staged = pdfe::core::StagingFile::createBeside(finalPath, ec);
    if (!staged) {
        err = "could not create the staging file for " + finalPath.string()
            + " (original left intact): " + ec.message();
        return false;
    }

    // Scoped so the duplicate is CLOSED before the publish: a handle this
    // process still holds on the source blocks a replace-existing rename, and
    // that self-inflicted lock has cost this programme three separate
    // debugging sessions already.
    {
        std::FILE* fp = staged->openDuplicateStream(ec);
        if (!fp) {
            err = "could not open the staging stream for " + finalPath.string()
                + " (original left intact): " + ec.message();
            return false;
        }
        QFile out;
        // DontCloseHandle: `fp` is OURS. QFile must not close a handle whose
        // lifetime the StagingFile contract owns.
        if (!out.open(fp, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) {
            std::fclose(fp);
            err = "could not adopt the staging stream for " + finalPath.string();
            return false;
        }
        const bool saved = img.save(&out, qtFormatTag(fmt),
                                    fmt == ImageFormat::Jpeg ? jpegQuality : -1);
        // Flush THROUGH Qt and then through the CRT, in that order: QFile
        // buffers, and a commit taken over half-written bytes publishes a
        // truncated image that is not a failure anywhere.
        out.flush();
        out.close();
        const bool flushed = (std::fflush(fp) == 0);
        std::fclose(fp);
        if (!saved) {
            err = "QImage::save failed for the staged export of "
                + finalPath.string();
            return false;
        }
        if (!flushed) {
            err = "could not flush the staged export of " + finalPath.string();
            return false;
        }
    }

    {
        std::error_code flushEc;
        staged->flushToStableStorage(flushEc);  // best-effort
    }

    // F10 / WIN-85 (data loss), still in force: the previous fallback here was
    // remove(finalPath) + rename(temp, finalPath) — the exact remove-first
    // anti-pattern R2-04 removed from Document::save(). If the second rename
    // then failed (A/V still holding the temp, target directory ACL, media
    // gone) the user's EXISTING exported image had already been deleted and the
    // replacement never arrived: silent data loss on an overwrite. Nothing is
    // removed first here either; on failure finalPath is completely untouched
    // and the StagingFile's destructor removes the unpublished artifact.
    std::error_code renameEc;
    if (!staged->commitOverWithRetry(finalPath, renameEc)) {
        err = "rename failed: " + renameEc.message();
        return false;
    }
    return true;
}

} // namespace

ImageExporter::ImageExporter(pdfe::core::Document* document)
    : doc_(document)
{
}

ImageExporter::~ImageExporter() = default;

void ImageExporter::cancel()
{
    cancelled_.store(true, std::memory_order_relaxed);
}

std::string ImageExporter::applyPattern(const std::string& pattern,
                                        int pageIndexZeroBased,
                                        int totalPages,
                                        const std::string& docBasename,
                                        const std::string& extension)
{
    const int digits = std::max<int>(1,
        static_cast<int>(std::to_string(std::max(1, totalPages)).size()));

    std::string s = pattern;
    s = replaceAll(s, "{basename}", docBasename);
    s = replaceAll(s, "{page}",  zeroPad(pageIndexZeroBased + 1, digits));
    s = replaceAll(s, "{pageZ}", zeroPad(pageIndexZeroBased, digits));
    s = replaceAll(s, "{ext}",   extension);
    return s;
}

ImageExportResult ImageExporter::exportToFolder(
    const std::filesystem::path& outputDir,
    const ImageExportOptions& opts,
    ProgressCallback progress)
{
    ImageExportResult result;
    // Do NOT reset cancelled_ here — a caller that invokes cancel() before
    // exportToFolder() intends to preempt the upcoming run, and resetting
    // the flag at entry would silently wipe that intent.

    if (!doc_) {
        ImageExportFileResult f;
        f.success = false;
        f.errorMessage = "No document";
        result.fileResults.push_back(f);
        result.errorCount = 1;
        return result;
    }

    // PDF-21 (meta-audit follow-up F2): rasterising every page to PNG/JPEG is
    // content extraction — arguably the most complete form of it, since the
    // result can be OCR'd back to text. Gate on the same copy/extract
    // permission PlainTextExporter uses. Refused before create_directories so
    // the run leaves NOTHING on disk (not even the output folder).
    if (!doc_->copyAllowed()) {
        ImageExportFileResult f;
        f.success = false;
        f.errorMessage = "Content extraction is not permitted for this document";
        result.fileResults.push_back(f);
        result.errorCount = 1;
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        ImageExportFileResult f;
        f.success = false;
        f.errorMessage = "create_directories failed: " + ec.message();
        result.fileResults.push_back(f);
        result.errorCount = 1;
        return result;
    }

    const int total = doc_->pageCount();
    const std::string basename = doc_->filePath().stem().string();
    const std::string ext = formatExtension(opts.format);

    std::vector<int> pagesToExport;
    if (opts.explicitPages.empty()) {
        pagesToExport.reserve(total);
        for (int i = 0; i < total; ++i) pagesToExport.push_back(i);
    } else {
        std::copy_if(opts.explicitPages.begin(), opts.explicitPages.end(),
                     std::back_inserter(pagesToExport),
                     [total](int i) { return i >= 0 && i < total; });
    }
    result.totalRequested = static_cast<int>(pagesToExport.size());

    const float scale = static_cast<float>(opts.dpi) / 72.0f;

    int done = 0;
    for (int pageIdx : pagesToExport) {
        if (cancelled_.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            break;
        }

        ImageExportFileResult file;
        file.pageIndex = pageIdx;

        const std::string name = applyPattern(opts.filenamePattern,
                                              pageIdx, total, basename, ext);
        file.outputPath = outputDir / (name + "." + ext);

        auto page = doc_->page(pageIdx);
        if (!page) {
            file.success = false;
            file.errorMessage = "page() returned null";
            result.errorCount++;
            result.fileResults.push_back(file);
            if (progress) progress(++done, result.totalRequested);
            continue;
        }

        try {
            QImage img = page->render(QRectF(), scale);
            if (img.isNull()) {
                file.success = false;
                file.errorMessage = "render returned null image";
                result.errorCount++;
            } else {
                std::string err;
                if (writeImageAtomic(img, file.outputPath, opts.format,
                                     opts.jpegQuality, err)) {
                    file.success = true;
                    result.successCount++;
                } else {
                    file.success = false;
                    file.errorMessage = err;
                    result.errorCount++;
                }
            }
        } catch (const std::exception& e) {
            file.success = false;
            file.errorMessage = e.what();
            result.errorCount++;
        }
        result.fileResults.push_back(file);

        if (progress) progress(++done, result.totalRequested);
    }

    return result;
}

} // namespace pdfe::exports
