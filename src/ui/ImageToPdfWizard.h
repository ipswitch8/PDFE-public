#pragma once

#include <QStringList>
#include <QIODevice>
#include <filesystem>

class QWidget;

namespace pdfe::ui {

/// Minimal "Create PDF from Images" wizard. Not a multi-step QWizard
/// (Acrobat's is more elaborate — image ordering, page-size options,
/// margins) but covers the core flow: pick N images, pick output PDF
/// path, write one image per page.
///
/// Each input image becomes one page sized to the image's pixel
/// dimensions at 72 DPI. The image is drawn to fill the page
/// exactly — no margins, no borders. Callers that want margins
/// can post-process via the page manipulation APIs.
class ImageToPdfWizard {
public:
    /// Run the full interactive flow: multi-file picker → output path
    /// picker → write PDF. Returns the output path on success, empty
    /// when the user cancelled. Throws std::runtime_error on I/O
    /// failures.
    static std::filesystem::path runInteractive(QWidget* parent);

    /// Programmatic entry point used by tests. Does no UI.
    /// `images` is a list of image file paths; each one that loads
    /// becomes a page in the output PDF. Returns true when at least
    /// one image was successfully written.
    static bool writePdf(const QStringList& images,
                         const std::filesystem::path& outPath);

    /// Same, but writing THROUGH an already-open device rather than opening
    /// `outPath` by name.
    ///
    /// WIN-87 wider class (phase-60): the combine service holds its staging
    /// file open exclusively for the whole of its life, so that nothing can
    /// substitute the wrapper PDF between "written" and "read back into the
    /// merge". A by-name writer cannot be used there — it would have to open a
    /// path the holder has locked. The path-taking overload above is retained
    /// for callers with no such requirement (the interactive wizard writes to
    /// the user's chosen destination). Neither overload calls the other: each
    /// constructs its own QPdfWriter over its own sink and hands it to one
    /// shared private helper, so the page-sizing rules — including the ISO
    /// 32000-1 Annex C.2 clamp from meta-audit F17 — exist in exactly one place.
    static bool writePdf(const QStringList& images, QIODevice* device);

    /// Image extensions Qt's QImageReader handles out of the box,
    /// used to build the file-dialog filter string. Exposed for
    /// testing.
    static QStringList supportedImageExtensions();

    /// Convenience: construct a Qt file-dialog filter from
    /// supportedImageExtensions(). Exposed for testing.
    static QString imageFilterString();
};

} // namespace pdfe::ui
