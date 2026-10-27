#include "TextBlock.h"

namespace pdfe::core {

std::string TextBlock::text() const {
    std::string result;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            result += '\n';
        }
        result += lines[i].text();
    }

    return result;
}

} // namespace pdfe::core
