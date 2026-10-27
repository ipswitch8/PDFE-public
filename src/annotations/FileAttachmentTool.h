#pragma once
#include "AnnotationTool.h"

#include <QString>
#include <filesystem>
#include <string>

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for placing /FileAttachment annotations on a page.
///
/// Click-to-place: pressing the left mouse button drops a small icon
/// at the click position whose payload is the configured source file.
/// The icon style is one of "PaperClip", "PushPin" (default), "Graph"
/// or "Tag" — matching the four /Name entries the PDF spec defines
/// for /FileAttachment annotations.
///
/// The tool routes through AnnotationManager::createFileAttachmentAnnotation
/// which embeds the file's bytes as a PDF embedded file stream and
/// references it via the annotation's /FS entry.
class FileAttachmentTool : public AnnotationTool {
public:
    explicit FileAttachmentTool(AnnotationManager* manager);

    AnnotationType toolType() const override {
        return AnnotationType::FileAttachment;
    }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    /// Set the page context. Must be called before placing.

    /// Set the file whose bytes will be embedded.
    void setSourceFile(const std::filesystem::path& path) {
        sourceFile_ = path;
    }
    std::filesystem::path sourceFile() const { return sourceFile_; }

    /// Set the icon style. Accepts "PaperClip" / "PushPin" / "Graph" /
    /// "Tag"; anything else is treated as PushPin by the manager.
    void setIconName(const std::string& name) { iconName_ = name; }
    std::string iconName() const { return iconName_; }

    /// Optional override for the embedded display name. When empty
    /// the source file's basename is used.
    void setDisplayName(const std::string& name) { displayName_ = name; }

    void setColor(QColor color) override { color_ = color; }

    /// Icon size in PDF points (default 24x24, matches Acrobat).
    static constexpr float ICON_SIZE = 24.0f;

private:
    AnnotationManager* manager_ = nullptr;
    QColor color_{255, 255, 0};   // yellow icon tint, mirrors sticky note default
    std::filesystem::path sourceFile_;
    std::string iconName_ = "PushPin";
    std::string displayName_;
};

} // namespace pdfe::annotations
