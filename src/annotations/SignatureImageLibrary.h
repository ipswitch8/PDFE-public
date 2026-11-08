#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

namespace pdfe::annotations {

/// GitHub #362 — named library of user-saved signature images.
///
/// Today SignDialog::onBrowseImage is one-shot: a browsed signature image
/// is used for a single signing and then forgotten. This library lets a
/// chosen signature graphic be SAVED under a name, listed, and re-selected
/// in a later session, so a recurring signature no longer has to be
/// re-browsed every time.
///
/// Each entry pairs a unique name with a QImage payload. The library
/// persists across application sessions via QSettings under the
/// "pdfe/signatureImages" prefix (PNG-encoded base64 payloads).
///
/// This mirrors pdfe::annotations::ImageStampLibrary exactly, including the
/// bare-`QSettings` persistence convention (see ImageStampLibrary.cpp:68):
/// a default-constructed QSettings follows the ambient
/// QCoreApplication::organizationName()/applicationName() identity, so the
/// hive is NOT pinned in source and tests can redirect it via
/// pdfe_test::SettingsScope (meta-audit follow-up F28/F33).
class SignatureImageLibrary {
public:
    SignatureImageLibrary();

    /// Import an image file as a named signature image. Accepts any format
    /// QImage::load supports (PNG, JPG, BMP, ...). If `name` already exists
    /// it is replaced.
    /// @return true on success; false if the file could not be loaded.
    bool importImage(const QString& name, const QString& filePath);

    /// Add a signature image directly from a QImage. Useful for tests and
    /// for loading from QSettings. If `name` already exists it is replaced.
    /// @return false if the name is empty or the image is null.
    bool addSignatureImage(const QString& name, const QImage& image);

    /// Remove the signature image with the given name.
    /// @return true if removed, false if not found.
    bool removeSignatureImage(const QString& name);

    /// Look up a signature image by name. Returns a null QImage if not found.
    QImage signatureImage(const QString& name) const;

    /// True if a signature image with this name exists.
    bool hasSignatureImage(const QString& name) const;

    /// All signature-image names in insertion order (most recent last).
    QStringList signatureImageNames() const;

    /// Number of signature images in the library.
    int count() const { return static_cast<int>(images_.size()); }

    /// Clear all signature images.
    void clear() { images_.clear(); }

    /// Persist the library to QSettings under the "pdfe/signatureImages"
    /// prefix. Each entry is stored as a PNG-encoded base64 string.
    void saveToSettings() const;

    /// Replace the in-memory library with the contents of QSettings.
    void loadFromSettings();

private:
    struct Entry {
        QString name;
        QImage image;
    };
    std::vector<Entry> images_;

    int indexOf(const QString& name) const;
};

} // namespace pdfe::annotations
