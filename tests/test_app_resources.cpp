// test_app_resources.cpp
// Unit tests for application resources (icon, resource files)

#include <gtest/gtest.h>
#include <QIcon>
#include <QFile>
#include <QApplication>
#include <filesystem>

namespace {

// Helper to get the project root directory
std::filesystem::path getProjectRoot() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / ".." / "..";
}

} // anonymous namespace

//
// AppIcon Tests
//

TEST(AppIcon, FileExists) {
    // pdfe-red.ico at repo root is the canonical icon source after the
    // SVG theme-aware variants were retired; src/app/pdfe.rc references
    // it as "../../pdfe-red.ico" and pdfe_icons.qrc embeds it.
    std::filesystem::path iconPath = getProjectRoot() / "pdfe-red.ico";

    EXPECT_TRUE(std::filesystem::exists(iconPath))
        << "Application icon file should exist at: " << iconPath.string();
}

TEST(AppIcon, NonZeroSize) {
    std::filesystem::path iconPath = getProjectRoot() / "pdfe-red.ico";

    std::uintmax_t fileSize = 0;
    if (std::filesystem::exists(iconPath)) {
        fileSize = std::filesystem::file_size(iconPath);
    }

    EXPECT_GT(fileSize, 0u)
        << "Application icon file should have non-zero size: " << iconPath.string();
}

//
// Resource Header Tests
//

TEST(ResourceHeader, Exists) {
    // Arrange
    std::filesystem::path resourceHeaderPath = getProjectRoot() / "src" / "app" / "resource.h";

    // Act & Assert
    EXPECT_TRUE(std::filesystem::exists(resourceHeaderPath))
        << "Resource header file should exist at: " << resourceHeaderPath.string();
}
