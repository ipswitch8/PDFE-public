#pragma once

#include "ComplianceTypes.h"
#include <memory>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class FontComplianceChecker {
public:
    explicit FontComplianceChecker(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~FontComplianceChecker();
    FontComplianceChecker(const FontComplianceChecker&) = delete;
    FontComplianceChecker& operator=(const FontComplianceChecker&) = delete;

    std::vector<FontComplianceInfo> checkFonts(
        pdfe::core::Document* doc,
        bool requireUnicode = false
    );

    std::vector<FontComplianceInfo> checkPageFonts(
        pdfe::core::Document* doc,
        int pageIndex,
        bool requireUnicode = false
    );

    bool allFontsCompliant(
        pdfe::core::Document* doc,
        bool requireUnicode = false
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
