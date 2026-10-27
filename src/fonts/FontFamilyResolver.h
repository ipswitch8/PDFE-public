#pragma once

#include <string>
#include <memory>

namespace pdfe::fonts {

/// Resolves Qt font family names + style flags to system font file paths.
/// Wraps SystemFontDiscovery with result caching.
class FontFamilyResolver {
public:
    FontFamilyResolver();
    ~FontFamilyResolver();

    /// Resolve font family name + style → system .ttf/.otf path.
    /// Returns empty string if font not found.
    std::string resolve(const std::string& familyName,
                        bool bold = false, bool italic = false);

    // Non-copyable, movable
    FontFamilyResolver(const FontFamilyResolver&) = delete;
    FontFamilyResolver& operator=(const FontFamilyResolver&) = delete;
    FontFamilyResolver(FontFamilyResolver&&) noexcept;
    FontFamilyResolver& operator=(FontFamilyResolver&&) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::fonts
