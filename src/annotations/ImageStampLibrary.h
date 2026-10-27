#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

namespace pdfe::annotations {

/// Named library of user-imported image stamps.
///
/// Each entry pairs a unique name with a QImage payload. Stamps are
/// retrievable by name from the StampTool picker, and can be placed on
/// any page; placement writes the image as a Form XObject appearance
/// stream on a PDF /Stamp annotation (see
/// AnnotationManager::setStampImageAppearance).
///
/// The library persists across application sessions via QSettings
/// under the "pdfe/imageStamps" prefix (PNG-encoded base64 payloads).
class ImageStampLibrary {
public:
    ImageStampLibrary();

    /// Import an image file as a named stamp. Accepts any format
    /// QImage::load supports (PNG, JPG, BMP, ...). If `name` already
    /// exists it is replaced.
    /// @return true on success; false if the file could not be loaded.
    bool importImage(const QString& name, const QString& filePath);

    /// Add a stamp directly from a QImage. Useful for tests and for
    /// loading from QSettings. If `name` already exists it is replaced.
    /// @return false if the image is null.
    bool addStamp(const QString& name, const QImage& image);

    /// Remove the stamp with the given name.
    /// @return true if removed, false if not found.
    bool removeStamp(const QString& name);

    /// Look up a stamp by name. Returns a null QImage if not found.
    QImage stamp(const QString& name) const;

    /// True if a stamp with this name exists.
    bool hasStamp(const QString& name) const;

    /// All stamp names in insertion order (most recent imports last).
    QStringList stampNames() const;

    /// Number of stamps in the library.
    int count() const { return static_cast<int>(stamps_.size()); }

    /// Clear all stamps.
    void clear() { stamps_.clear(); }

    /// Persist the library to QSettings under the "pdfe/imageStamps"
    /// prefix. Each entry is stored as a PNG-encoded base64 string.
    void saveToSettings() const;

    /// Replace the in-memory library with the contents of QSettings.
    void loadFromSettings();

private:
    struct Entry {
        QString name;
        QImage image;
    };
    std::vector<Entry> stamps_;

    int indexOf(const QString& name) const;
};

} // namespace pdfe::annotations
