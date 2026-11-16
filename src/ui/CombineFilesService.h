#pragma once

#include <QCoreApplication>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::ui {

/// Pipeline E phase-11: combine many PDF (and image) files into a
/// single output PDF using MuPDF's pdf_graft_mapped_page mechanism
/// (the same path Document::graftPages uses for ImportPages).
///
/// Input files are processed in order. PDF inputs are grafted page
/// by page; image inputs are first wrapped into a 1-page temp PDF
/// via the existing image-to-pdf path, then grafted.
class CombineFilesService {
    // Not a QObject, but its failure messages are shown to the user, so it
    // needs tr(). Q_DECLARE_TR_FUNCTIONS gives the class a static tr()
    // under its own translation context; lupdate collects the strings.
    Q_DECLARE_TR_FUNCTIONS(CombineFilesService)
public:
    /// Optional progress callback. `currentInputIndex` is 0-based;
    /// `totalInputs` is the size of the input list. Called once per
    /// input file processed (and once at the end with index == total).
    using ProgressCallback =
        std::function<void(int currentInputIndex, int totalInputs)>;

    explicit CombineFilesService(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~CombineFilesService();

    CombineFilesService(const CombineFilesService&) = delete;
    CombineFilesService& operator=(const CombineFilesService&) = delete;

    struct Result {
        bool ok = false;
        int outputPageCount = 0;
        /// Diagnostic detail for logs and tests: English, full paths.
        /// Populated only when ok == false.
        std::string errorMessage;
        /// The same failure phrased for the USER: translated via tr() and
        /// naming the file the user actually picked, by basename. The
        /// wizard shows THIS one. Populated whenever errorMessage is.
        ///
        /// Two fields rather than one because they answer different
        /// questions: an image input is combined by first wrapping it in a
        /// temp PDF (pdfe_combine_img_*), and the failure that follows is
        /// reported against that wrapper. A log wants the wrapper path; a
        /// user wants the name of the picture they chose, and previously
        /// got the wrapper — a filename they had never seen and could not
        /// act on.
        QString userMessage;
    };

    /// Combine the given inputs (PDF and/or image files) into a single
    /// PDF at `output`. Requires at least 2 inputs. The first input
    /// becomes the base document; remaining inputs are appended via
    /// pdf_graft_mapped_page. Returns ok=false with a non-empty
    /// errorMessage on any error; in that case `output` may not exist.
    Result combine(const std::vector<std::filesystem::path>& inputs,
                   const std::filesystem::path& output,
                   ProgressCallback onProgress = {});

    /// Heuristic: is this path a PDF (by file extension) or an image
    /// (jpg/jpeg/png/tif/tiff/bmp). Anything else returns Unsupported.
    enum class InputKind { Pdf, Image, Unsupported };
    static InputKind classifyInput(const std::filesystem::path& path);

    /// Linux port phase-6 test seam. Fires once per image wrapper, with the
    /// wrapper's path, immediately after the wrapper has been SEALED and before
    /// combine() re-reads it by path — the exact window a same-user attacker
    /// would use on POSIX. A test tampers with the file here and asserts the
    /// combine is refused by verifyUnchanged(). Same shape and guarding as
    /// MainWindow::setAlignStagingWindowHookForTesting. Never set in
    /// production; a null hook costs one predictable branch.
    static void setSealedWrapperHookForTesting(
        std::function<void(const std::filesystem::path&)> hook);

private:
    static void fireSealedWrapperHookForTesting(const std::filesystem::path& p);

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace pdfe::ui
