#pragma once

#include "image/ImageData.h"
#include "core/Command.h"
#include <memory>
#include <string>

namespace pdfe::core {
class Page;
class Document;
}

namespace pdfe::image {

class ImageManager;
class ImageContentEditor;

/// Command to insert an image onto a page (undoable).
class InsertImageCommand : public pdfe::commands::Command {
public:
    InsertImageCommand(
        ImageManager* manager,
        pdfe::core::Page* page,
        pdfe::core::Document* doc,
        const InsertImageOptions& options);
    ~InsertImageCommand() override;

    void execute() override;
    void undo() override;
    // redo() inherits Command default { execute(); }
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

    [[nodiscard]] const ImageOperationResult& result() const { return result_; }

private:
    ImageManager* manager_;
    pdfe::core::Page* page_;
    pdfe::core::Document* doc_;
    InsertImageOptions options_;
    ImageOperationResult result_;
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

/// Command to remove an image from a page (undoable).
class RemoveImageCommand : public pdfe::commands::Command {
public:
    RemoveImageCommand(
        ImageManager* manager,
        pdfe::core::Page* page,
        pdfe::core::Document* doc,
        int imageIndex);
    ~RemoveImageCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    ImageManager* manager_;
    pdfe::core::Page* page_;
    pdfe::core::Document* doc_;
    int imageIndex_;
    PageImageInfo savedInfo_;
    void* savedImageRef_ = nullptr;
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

/// Command to resize an image (undoable).
class ResizeImageCommand : public pdfe::commands::Command {
public:
    ResizeImageCommand(
        ImageManager* manager,
        ImageContentEditor* editor,
        pdfe::core::Page* page,
        pdfe::core::Document* doc,
        const std::string& xobjectName,
        float newWidth, float newHeight,
        float x, float y);
    ~ResizeImageCommand() override;

    void execute() override;
    void undo() override;
    // redo() inherits Command default { execute(); }
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    ImageManager* manager_;
    ImageContentEditor* editor_;
    pdfe::core::Page* page_;
    pdfe::core::Document* doc_;
    std::string xobjectName_;
    float newWidth_, newHeight_, x_, y_;
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

/// Command to rotate an image (undoable).
class RotateImageCommand : public pdfe::commands::Command {
public:
    RotateImageCommand(
        ImageManager* manager,
        ImageContentEditor* editor,
        pdfe::core::Page* page,
        pdfe::core::Document* doc,
        const std::string& xobjectName,
        float angleDegrees,
        float centerX, float centerY,
        float displayWidth, float displayHeight);
    ~RotateImageCommand() override;

    void execute() override;
    void undo() override;
    // redo() inherits Command default { execute(); }
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    ImageManager* manager_;
    ImageContentEditor* editor_;
    pdfe::core::Page* page_;
    pdfe::core::Document* doc_;
    std::string xobjectName_;
    float angleDegrees_, centerX_, centerY_;
    float displayWidth_, displayHeight_;
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

/// Command to crop an image (undoable).
class CropImageCommand : public pdfe::commands::Command {
public:
    CropImageCommand(
        ImageManager* manager,
        ImageContentEditor* editor,
        pdfe::core::Page* page,
        pdfe::core::Document* doc,
        const std::string& xobjectName,
        QRectF cropRect,
        float displayWidth, float displayHeight,
        float x, float y);
    ~CropImageCommand() override;

    void execute() override;
    void undo() override;
    // redo() inherits Command default { execute(); }
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    ImageManager* manager_;
    ImageContentEditor* editor_;
    pdfe::core::Page* page_;
    pdfe::core::Document* doc_;
    std::string xobjectName_;
    QRectF cropRect_;
    float displayWidth_, displayHeight_, x_, y_;
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

} // namespace pdfe::image
