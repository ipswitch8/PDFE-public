#pragma once

#include "ComplianceTypes.h"
#include <memory>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class ProhibitedFeatureScanner {
public:
    explicit ProhibitedFeatureScanner(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ProhibitedFeatureScanner();
    ProhibitedFeatureScanner(const ProhibitedFeatureScanner&) = delete;
    ProhibitedFeatureScanner& operator=(const ProhibitedFeatureScanner&) = delete;

    bool hasEncryption(pdfe::core::Document* doc) const;
    bool hasJavaScript(pdfe::core::Document* doc) const;
    bool hasTransparency(pdfe::core::Document* doc) const;
    bool hasObjectStreams(pdfe::core::Document* doc) const;
    bool hasExternalReferences(pdfe::core::Document* doc) const;

    std::vector<ComplianceIssue> scanAll(
        pdfe::core::Document* doc,
        PdfALevel level
    ) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
