#pragma once

#include <QDialog>
#include <memory>
#include <string>

namespace pdfe::metadata {
struct DocumentMetadata;
}

namespace pdfe::ui {

class MetadataDialog : public QDialog {
    Q_OBJECT
public:
    explicit MetadataDialog(QWidget* parent = nullptr);
    ~MetadataDialog() override;

    /// Set the metadata to display/edit.
    void setMetadata(const pdfe::metadata::DocumentMetadata& metadata);

    /// Get the (possibly edited) metadata.
    pdfe::metadata::DocumentMetadata metadata() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::ui
