#include "ImageStampLibrary.h"

#include <QBuffer>
#include <QByteArray>
#include <QSettings>

namespace pdfe::annotations {

ImageStampLibrary::ImageStampLibrary() = default;

int ImageStampLibrary::indexOf(const QString& name) const
{
    for (int i = 0; i < static_cast<int>(stamps_.size()); ++i) {
        if (stamps_[i].name == name) return i;
    }
    return -1;
}

bool ImageStampLibrary::importImage(const QString& name, const QString& filePath)
{
    if (name.isEmpty()) return false;
    QImage img;
    if (!img.load(filePath)) return false;
    return addStamp(name, img);
}

bool ImageStampLibrary::addStamp(const QString& name, const QImage& image)
{
    if (name.isEmpty() || image.isNull()) return false;
    int idx = indexOf(name);
    if (idx >= 0) {
        stamps_[idx].image = image;
    } else {
        stamps_.push_back(Entry{name, image});
    }
    return true;
}

bool ImageStampLibrary::removeStamp(const QString& name)
{
    int idx = indexOf(name);
    if (idx < 0) return false;
    stamps_.erase(stamps_.begin() + idx);
    return true;
}

QImage ImageStampLibrary::stamp(const QString& name) const
{
    int idx = indexOf(name);
    return idx < 0 ? QImage() : stamps_[idx].image;
}

bool ImageStampLibrary::hasStamp(const QString& name) const
{
    return indexOf(name) >= 0;
}

QStringList ImageStampLibrary::stampNames() const
{
    QStringList out;
    out.reserve(static_cast<int>(stamps_.size()));
    for (const auto& e : stamps_) out.push_back(e.name);
    return out;
}

void ImageStampLibrary::saveToSettings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("pdfe/imageStamps"));
    settings.remove(QString());  // clear previous group contents
    QStringList names;
    for (const auto& e : stamps_) {
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

void ImageStampLibrary::loadFromSettings()
{
    stamps_.clear();
    QSettings settings;
    settings.beginGroup(QStringLiteral("pdfe/imageStamps"));
    QStringList order = settings.value(QStringLiteral("__order")).toStringList();
    for (const QString& key : order) {
        QByteArray b64 = settings.value(key).toByteArray();
        if (b64.isEmpty()) continue;
        QByteArray pngBytes = QByteArray::fromBase64(b64);
        QImage img;
        if (!img.loadFromData(pngBytes, "PNG")) continue;
        QString name = QString::fromUtf8(QByteArray::fromBase64(key.toLatin1()));
        if (name.isEmpty()) continue;
        stamps_.push_back(Entry{name, img});
    }
    settings.endGroup();
}

} // namespace pdfe::annotations
