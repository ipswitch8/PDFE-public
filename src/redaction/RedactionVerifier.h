#pragma once

#include "RedactionData.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::redaction {

/// Verifies that redaction was applied correctly.
///
/// Post-apply verification checks:
/// 1. No redacted text remains in the content streams
/// 2. No orphan content streams reference removed objects
/// 3. Single-revision document (no previous data accessible)
class RedactionVerifier {
public:
    explicit RedactionVerifier(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~RedactionVerifier();

    // Non-copyable
    RedactionVerifier(const RedactionVerifier&) = delete;
    RedactionVerifier& operator=(const RedactionVerifier&) = delete;

    // Movable
    RedactionVerifier(RedactionVerifier&&) noexcept;
    RedactionVerifier& operator=(RedactionVerifier&&) noexcept;

    /// Verify that redaction was applied correctly on a file.
    /// @param filePath Path to the PDF file to verify
    /// @param redactedRegions The regions that were redacted (for text checking)
    RedactionVerifyResult verify(
        const std::filesystem::path& filePath,
        const std::vector<RedactionData>& redactedRegions
    );

    /// Check if any text from the original regions remains in the file.
    bool checkTextRemoved(
        const std::filesystem::path& filePath,
        const std::vector<RedactionData>& redactedRegions
    );

    /// Check if the file has a single revision (no incremental updates).
    bool checkSingleRevision(const std::filesystem::path& filePath);

    /// Check for orphan streams that reference removed objects.
    bool checkNoOrphanStreams(const std::filesystem::path& filePath);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::redaction
