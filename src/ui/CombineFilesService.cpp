#include "CombineFilesService.h"

#include "../core/Document.h"
#include "../core/MuPdfContext.h"
#include "../core/TempFileSafety.h"
#include "ImageToPdfWizard.h"

#include <QString>
#include <QStringList>
#include <QFile>
#include <QIODevice>

#include <io.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>

namespace pdfe::ui {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;

CombineFilesService::CombineFilesService(std::shared_ptr<MuPdfContext> context)
    : context_(std::move(context))
{
}

CombineFilesService::~CombineFilesService() = default;

namespace {

std::string lowerExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

} // namespace

CombineFilesService::InputKind
CombineFilesService::classifyInput(const std::filesystem::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".pdf") return InputKind::Pdf;
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".tif" || ext == ".tiff" || ext == ".bmp") {
        return InputKind::Image;
    }
    return InputKind::Unsupported;
}

CombineFilesService::Result
CombineFilesService::combine(
    const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& output,
    ProgressCallback onProgress)
{
    Result r;

    // Basename of a path the USER chose, for the user-facing message. The
    // wizard's own list shows these names, so this is what they can match
    // against; a full temp path is noise they cannot act on.
    auto shownName = [](const std::filesystem::path& p) {
        return QString::fromStdString(p.filename().string());
    };

    if (inputs.size() < 2) {
        r.errorMessage = "Combine Files needs at least 2 input files.";
        r.userMessage = tr("Combine Files needs at least 2 input files.");
        return r;
    }
    for (const auto& p : inputs) {
        if (!std::filesystem::exists(p)) {
            r.errorMessage = "Input does not exist: " + p.string();
            r.userMessage = tr("“%1” could not be found.").arg(shownName(p));
            return r;
        }
        if (classifyInput(p) == InputKind::Unsupported) {
            r.errorMessage = "Unsupported input type: " + p.string();
            r.userMessage =
                tr("“%1” is not a file type Combine Files can use. Choose "
                   "PDF, JPEG, PNG, TIFF or BMP files.").arg(shownName(p));
            return r;
        }
    }

    // Materialize image inputs as 1-page temp PDFs so the rest of the
    // pipeline can graft them uniformly. Each wrapper is an unpredictable,
    // PINNED PinnedTempFile (WIN-87 wider class, phase-60) and is deleted by
    // its destructor when this function returns, on every path including a
    // throw — see the block at its creation for why the old name was not
    // random despite looking it.
    std::vector<std::filesystem::path> pdfPaths;
    pdfPaths.reserve(inputs.size());
    // Parallel to pdfPaths: the input the USER actually chose for that
    // entry. For a PDF the two are the same path; for an image they are
    // NOT — pdfPaths holds the pdfe_combine_img.<hex> wrapper. Every failure
    // below is reported against this vector, never against pdfPaths, so
    // the user is never shown the name of a file PDFE invented.
    std::vector<std::filesystem::path> sourceForPdfPath;
    sourceForPdfPath.reserve(inputs.size());
    // The image wrappers, each OWNED and PINNED for as long as this vector
    // lives. Cleanup is RAII rather than the six hand-written removal loops
    // this function used to carry — one per early return, which is exactly the
    // shape where one gets forgotten. ~PinnedTempFile releases the pin and
    // deletes the file, on every path out of this function including a throw.
    std::vector<std::unique_ptr<core::PinnedTempFile>> pinnedImagePdfs;

    for (const auto& p : inputs) {
        if (classifyInput(p) == InputKind::Pdf) {
            pdfPaths.push_back(p);
            sourceForPdfPath.push_back(p);
            continue;
        }
        // Image: convert to a single-page temp PDF.
        QStringList one;
        one << QString::fromStdString(p.string());
        // WIN-87 wider class (phase-60). This name used to be
        //   "pdfe_combine_img_" + (uintptr_t)&p + "_" + index + ".pdf"
        // and was described in the reverification as "random". It is not. `&p`
        // is the address of a loop reference into a container the caller owns,
        // so it is a STACK/HEAP address — constant across runs under a fixed
        // allocator, leaked by any prior information disclosure, and in any case
        // drawn from a space far smaller than it looks. The index is a counter
        // from 0. An attacker who guesses it can substitute their own PDF
        // between writePdf() and Document::open() below, and every page of it
        // lands in the user's combined output.
        //
        // PinnedTempFile answers all three parts: 128 CSPRNG bits so it cannot
        // be aimed at, CREATE_NEW so nothing was already sitting there, and an
        // exclusive handle HELD for the object's whole life so no second writer
        // can exist to substitute anything.
        std::error_code stagingEc;
        auto staged = core::PinnedTempFile::reserve(
            std::filesystem::temp_directory_path(),
            "pdfe_combine_img", ".pdf", stagingEc);
        if (!staged) {
            r.errorMessage = "Failed to reserve a staging file for: " + p.string();
            r.userMessage =
                tr("“%1” could not be prepared for combining.")
                    .arg(shownName(p));
            return r;
        }
        const std::filesystem::path tempPdf = staged->path();

        // WRITTEN THROUGH THE HELD HANDLE, not by name. `staged` keeps the file
        // open exclusively for its whole life, so there is no instant at which
        // another process could write, delete or rename this path -- and
        // therefore no window for the merge below to read somebody else's PDF.
        // An earlier draft closed the handle, let QPdfWriter open the path by
        // name, and re-checked the file's identity afterwards; the phase-7
        // karen gate correctly rejected that, because an in-place overwrite
        // preserves file identity and is exactly what the legitimate writer
        // does.
        const int fd = staged->openWriteFd(stagingEc);
        bool wrote = false;
        if (fd >= 0) {
            QFile out;
            if (out.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
                wrote = ImageToPdfWizard::writePdf(one, &out);
                out.close();
            } else {
                ::_close(fd);
            }
        }
        if (!wrote) {
            r.errorMessage = "Failed to wrap image as PDF: " + p.string();
            r.userMessage =
                tr("“%1” could not be converted to a PDF page.")
                    .arg(shownName(p));
            return r;
        }
        // Ownership moves to `pinnedImagePdfs`, which holds the file open (and
        // so the substitution guarantee) until the merge below has read it.
        pinnedImagePdfs.push_back(std::move(staged));
        pdfPaths.push_back(tempPdf);
        sourceForPdfPath.push_back(p);
    }

    // Merge: open first PDF as base, graft the rest at the end.
    try {
        auto base = Document::open(context_, pdfPaths[0]);
        if (!base) {
            r.errorMessage = "Failed to open first input: "
                             + sourceForPdfPath[0].string();
            r.userMessage = tr("“%1” could not be opened.")
                                .arg(shownName(sourceForPdfPath[0]));
            return r;
        }
        if (onProgress) onProgress(0, static_cast<int>(inputs.size()));

        for (size_t i = 1; i < pdfPaths.size(); ++i) {
            auto src = Document::open(context_, pdfPaths[i]);
            if (!src) {
                r.errorMessage = "Failed to open input: "
                                 + sourceForPdfPath[i].string();
                r.userMessage = tr("“%1” could not be opened.")
                                    .arg(shownName(sourceForPdfPath[i]));
                return r;
            }
            const int srcCount = src->pageCount();
            if (srcCount > 0) {
                // F6 / PDF-89: a bare graftPages() merge silently dropped every
                // non-first input's annotations, bookmarks, form fields and
                // their VALUES, layers, page labels, attachments and named
                // destinations -- pdf_graft_mapped_page copies none of them.
                base->graftPagesPreservingArtifacts(base->pageCount(), *src,
                                                    0, srcCount - 1);
            }
            if (onProgress) onProgress(static_cast<int>(i),
                                       static_cast<int>(inputs.size()));
        }

        base->saveAs(output);
        r.outputPageCount = base->pageCount();
        r.ok = true;
    } catch (const std::exception& ex) {
        r.errorMessage = std::string("Combine failed: ") + ex.what();
        r.userMessage = tr("Combine failed: %1").arg(QString::fromUtf8(ex.what()));
    } catch (...) {
        r.errorMessage = "Combine failed: unknown error";
        r.userMessage = tr("Combine failed: unknown error.");
    }

    if (onProgress) onProgress(static_cast<int>(inputs.size()),
                               static_cast<int>(inputs.size()));

    // The image wrappers are deleted by ~PinnedTempFile as `pinnedImagePdfs`
    // goes out of scope here.
    return r;
}

} // namespace pdfe::ui
