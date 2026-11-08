// GitHub #362 — persisted signature-image library. Mirrors
// ImageStampLibrary.cpp (same QSettings convention and PNG/base64 payload
// encoding) so the "save a signature image once, reuse it across sessions"
// feature shares the storage shape the stamp picker already relies on.
#include "SignatureImageLibrary.h"

#include <QBuffer>
#include <QByteArray>
#include <QSettings>

namespace pdfe::annotations {

SignatureImageLibrary::SignatureImageLibrary() = default;

int SignatureImageLibrary::indexOf(const QString& name) const
{
    for (int i = 0; i < static_cast<int>(images_.size()); ++i) {
        if (images_[i].name == name) return i;
    }
    return -1;
}

bool SignatureImageLibrary::importImage(const QString& name, const QString& filePath)
{
    if (name.isEmpty()) return false;
    QImage img;
    if (!img.load(filePath)) return false;
    return addSignatureImage(name, img);
}

bool SignatureImageLibrary::addSignatureImage(const QString& name, const QImage& image)
{
    if (name.isEmpty() || image.isNull()) return false;
    int idx = indexOf(name);
    if (idx >= 0) {
        images_[idx].image = image;  // overwrite by name; no duplicate entry
    } else {
        images_.push_back(Entry{name, image});
    }
    return true;
}

bool SignatureImageLibrary::removeSignatureImage(const QString& name)
{
    int idx = indexOf(name);
    if (idx < 0) return false;
    images_.erase(images_.begin() + idx);
    return true;
}

QImage SignatureImageLibrary::signatureImage(const QString& name) const
{
    int idx = indexOf(name);
    return idx < 0 ? QImage() : images_[idx].image;
}

bool SignatureImageLibrary::hasSignatureImage(const QString& name) const
{
    return indexOf(name) >= 0;
}

QStringList SignatureImageLibrary::signatureImageNames() const
{
    QStringList out;
    out.reserve(static_cast<int>(images_.size()));
    for (const auto& e : images_) out.push_back(e.name);
    return out;
}

void SignatureImageLibrary::saveToSettings() const
{
    // Bare QSettings — follows the ambient org/app identity, exactly like
    // ImageStampLibrary::saveToSettings (ImageStampLibrary.cpp:68). Do NOT
    // pin an org/app pair here: that would make the hive unreachable to the
    // SettingsScope test seam (meta-audit follow-up F33).
    QSettings settings;
    settings.beginGroup(QStringLiteral("pdfe/signatureImages"));
    settings.remove(QString());  // clear previous group contents
    QStringList names;
    for (const auto& e : images_) {
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        e.image.save(&buf, "PNG");
        buf.close();
        // QSettings keys can contain '/' which acts as a sub-group
        // separator. Encode each entry under a sanitized key.
        QString key = QString::fromLatin1(e.name.toUtf8().toBase64());
        settings.setValue(key, bytes.toBase64());
        names.push_back(key);
    }
    settings.setValue(QStringLiteral("__order"), names);
    settings.endGroup();
}

void SignatureImageLibrary::loadFromSettings()
{
    images_.clear();
    QSettings settings;
    settings.beginGroup(QStringLiteral("pdfe/signatureImages"));
    QStringList order = settings.value(QStringLiteral("__order")).toStringList();
    for (const QString& key : order) {
        QByteArray b64 = settings.value(key).toByteArray();
        if (b64.isEmpty()) continue;
        QByteArray pngBytes = QByteArray::fromBase64(b64);
        QImage img;
        if (!img.loadFromData(pngBytes, "PNG")) continue;
        QString name = QString::fromUtf8(QByteArray::fromBase64(key.toLatin1()));
        if (name.isEmpty()) continue;
        images_.push_back(Entry{name, img});
    }
    settings.endGroup();
}

} // namespace pdfe::annotations
