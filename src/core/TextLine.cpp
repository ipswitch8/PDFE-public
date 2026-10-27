#include "TextLine.h"
#include <QString>

namespace pdfe::core {

std::string TextLine::text() const {
    QString result;
    result.reserve(static_cast<int>(chars.size()));

    for (const auto& ch : chars) {
        if (ch.codepoint <= 0xFFFF) {
            result.append(QChar(static_cast<char16_t>(ch.codepoint)));
        } else {
            // Supplementary plane: encode as surrogate pair
            result.append(QString::fromUcs4(&ch.codepoint, 1));
        }
    }

    return result.toStdString();
}

} // namespace pdfe::core
