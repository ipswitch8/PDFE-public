#pragma once

#include <string>
#include <vector>
#include <variant>

namespace pdfe::batch {

enum class BatchActionType {
    AddWatermark,
    FlattenAnnotations,
    ConvertToPdfA,
    SetPermissions
};

struct WatermarkConfig {
    std::string text = "DRAFT";
    std::string position = "center";
    float opacity = 0.3f;
    float fontSize = 48.0f;
    float rotation = 45.0f;
};

// Phase 4e extensions kept as a parallel struct to avoid breaking the
// variant-backed ActionConfig (which caches WatermarkConfig layout) and
// to dodge a persistent MSVC Debug NSDMI miscompile where the default
// constructor failed to initialize trailing std::string members when
// WatermarkConfig was a variant alternative.
struct WatermarkExtConfig {
    std::string mode = "text";            // "text" or "image"
    std::string imagePath;                // PNG/JPG path for image mode
    std::string rangeMode = "all";        // all / odd / even / explicit
    std::vector<int> explicitPages;
};

struct PdfAConfig {
    int level = 2;    // 1, 2, or 3
    char conformance = 'b';  // 'b', 'u', or 'a'
};

struct PermissionsConfig {
    bool allowPrint = true;
    bool allowCopy = false;
    bool allowEdit = false;
    std::string ownerPassword;
};

using ActionConfig = std::variant<WatermarkConfig, PdfAConfig, PermissionsConfig, std::monostate>;

struct BatchAction {
    BatchActionType type;
    ActionConfig config;
};

struct BatchFileResult {
    std::string filePath;
    bool success = false;
    std::string errorMessage;
    int actionsCompleted = 0;
};

struct BatchResult {
    std::vector<BatchFileResult> fileResults;
    int totalFiles = 0;
    int successCount = 0;
    int errorCount = 0;
    bool cancelled = false;
};

} // namespace pdfe::batch
