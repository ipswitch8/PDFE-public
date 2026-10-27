#pragma once
#include "Command.h"
#include <QRectF>
#include <filesystem>
#include <memory>
#include <vector>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::commands {

/**
 * @brief Command to rotate a page by a specified angle delta.
 *
 * Supports undo by rotating in the opposite direction.
 */
class RotatePageCommand : public Command {
public:
    /**
     * @brief Construct a rotate page command.
     *
     * @param doc The document containing the page
     * @param pageIndex The index of the page to rotate (0-based)
     * @param angleDelta The rotation angle in degrees (typically 90, -90, 180, or 270)
     */
    RotatePageCommand(
        pdfe::core::Document* doc,
        int pageIndex,
        int angleDelta
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int pageIndex_;
    int angleDelta_;
    bool executed_ = false;
};

/**
 * @brief Command to delete a page from a document.
 *
 * Supports undo by retaining the page object and reinserting it.
 */
class DeletePageCommand : public Command {
public:
    /**
     * @brief Construct a delete page command.
     *
     * @param doc The document containing the page
     * @param pageIndex The index of the page to delete (0-based)
     */
    DeletePageCommand(
        pdfe::core::Document* doc,
        int pageIndex
    );

    ~DeletePageCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int pageIndex_;
    void* retainedPageObj_ = nullptr;
    bool executed_ = false;
};

/**
 * @brief Command to insert a blank page into a document.
 *
 * Supports undo by deleting the inserted page.
 */
class InsertBlankPageCommand : public Command {
public:
    /**
     * @brief Construct an insert blank page command.
     *
     * @param doc The document to insert the page into
     * @param atIndex The index at which to insert the page (0-based)
     * @param widthPt The width of the page in points
     * @param heightPt The height of the page in points
     * @param rotation The initial rotation of the page in degrees (default 0)
     */
    InsertBlankPageCommand(
        pdfe::core::Document* doc,
        int atIndex,
        float widthPt,
        float heightPt,
        int rotation = 0
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int atIndex_;
    float widthPt_;
    float heightPt_;
    int rotation_;
    bool executed_ = false;
};

/**
 * @brief Command to move a page from one position to another within a document.
 *
 * Supports undo by moving the page back to its original position.
 */
class MovePageCommand : public Command {
public:
    /**
     * @brief Construct a move page command.
     *
     * @param doc The document containing the page
     * @param fromIndex The current index of the page (0-based)
     * @param toIndex The target index for the page (0-based)
     */
    MovePageCommand(
        pdfe::core::Document* doc,
        int fromIndex,
        int toIndex
    );

    ~MovePageCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int fromIndex_;
    int toIndex_;
    void* retainedPageObj_ = nullptr;  // legacy; unused since the R3-01 reorder routing
    std::vector<int> movePermutation_;  // R3-01: original-index order applied by execute()
    bool executed_ = false;
};

/**
 * @brief Command to import pages from another PDF file.
 *
 * Supports undo by deleting the imported pages.
 */
class ImportPagesCommand : public Command {
public:
    /**
     * @brief Construct an import pages command.
     *
     * @param doc The destination document
     * @param atIndex The index at which to insert the imported pages (0-based)
     * @param sourcePath The path to the source PDF file
     * @param context The MuPDF context for opening the source document
     * @param sourceStart The first page index to import from the source (0-based)
     * @param sourceEnd The last page index to import from the source (0-based, inclusive)
     */
    ImportPagesCommand(
        pdfe::core::Document* doc,
        int atIndex,
        std::filesystem::path sourcePath,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        int sourceStart,
        int sourceEnd
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int atIndex_;
    std::filesystem::path sourcePath_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    int sourceStart_;
    int sourceEnd_;
    int importedCount_ = 0;
    bool executed_ = false;
};

/**
 * @brief Command to delete multiple pages from a document.
 *
 * Supports undo by retaining all page objects and reinserting them.
 */
class DeletePageRangeCommand : public Command {
public:
    /**
     * @brief Construct a delete page range command.
     *
     * @param doc The document containing the pages
     * @param pageIndices The indices of the pages to delete (must be sorted ascending)
     */
    DeletePageRangeCommand(
        pdfe::core::Document* doc,
        std::vector<int> pageIndices
    );

    ~DeletePageRangeCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    std::vector<int> pageIndices_;
    std::vector<void*> retainedPageObjs_;
    bool executed_ = false;
};

/**
 * @brief Command to replace a page with a page from another document.
 *
 * Retains the original page object before replacement; undo restores it by
 * deleting the grafted page and reinserting the original.
 */
class ReplacePageCommand : public Command {
public:
    ReplacePageCommand(
        pdfe::core::Document* doc,
        int pageIndex,
        std::shared_ptr<pdfe::core::Document> sourceDoc,
        int sourcePageIndex
    );

    ~ReplacePageCommand() override;

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int pageIndex_;
    std::shared_ptr<pdfe::core::Document> sourceDoc_;  // Kept alive for undo/redo
    int sourcePageIndex_;
    void* originalPageObj_ = nullptr;  // Retained from original page
    bool executed_ = false;
};

/**
 * @brief Pipeline E phase-6: undoable /CropBox edit.
 *
 * Captures the current /CropBox at construction so undo can restore
 * it byte-for-byte even if the page had no explicit /CropBox before
 * (in which case the current bounding box, falling back to /MediaBox,
 * is treated as the "previous" value).
 */
class CropPageCommand : public Command {
public:
    CropPageCommand(
        pdfe::core::Document* doc,
        int pageIndex,
        QRectF newCropBox
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

private:
    pdfe::core::Document* doc_;
    int pageIndex_;
    QRectF newCropBox_;
    QRectF oldCropBox_;
    bool executed_ = false;
};

} // namespace pdfe::commands
