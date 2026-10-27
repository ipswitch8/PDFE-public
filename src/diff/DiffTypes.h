#pragma once

#include <QImage>
#include <QRectF>
#include <string>
#include <vector>

namespace pdfe::diff {

enum class ChangeType {
    Unchanged,
    Inserted,
    Deleted,
    Modified
};

struct TextChange {
    ChangeType type = ChangeType::Unchanged;
    std::string text;
    int lineNumber = -1;
};

struct PageDiffResult {
    int pageIndex = 0;
    double similarityScore = 1.0;  // 0.0 = completely different, 1.0 = identical
    QImage diffImage;              // pixel diff overlay
    std::vector<QRectF> changeRegions;
    std::vector<TextChange> textChanges;
    int insertionCount = 0;
    int deletionCount = 0;
    int modificationCount = 0;
};

struct DiffResult {
    std::vector<PageDiffResult> pages;
    int totalPages = 0;
    int changedPages = 0;
    double overallSimilarity = 1.0;
    std::string errorMessage;
};

} // namespace pdfe::diff
