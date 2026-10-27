#pragma once

#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

/// Manages ICC color profiles for PDF/A compliance.
/// Provides functionality to check for and add output intents,
/// specifically sRGB IEC61966-2.1 profiles required for PDF/A-1b.
class IccProfileManager {
public:
    explicit IccProfileManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~IccProfileManager();

    IccProfileManager(const IccProfileManager&) = delete;
    IccProfileManager& operator=(const IccProfileManager&) = delete;

    /// Checks if the document has an output intent defined.
    /// @param doc The document to check
    /// @return true if the document has at least one output intent
    bool hasOutputIntent(pdfe::core::Document* doc) const;

    /// Adds an sRGB IEC61966-2.1 output intent to the document.
    /// This is required for PDF/A-1b compliance when the document
    /// doesn't already have an output intent.
    /// @param doc The document to modify
    void addSrgbOutputIntent(pdfe::core::Document* doc);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
