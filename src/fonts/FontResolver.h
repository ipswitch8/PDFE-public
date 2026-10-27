#pragma once
#include <string>
#include <vector>

namespace pdfe::fonts {

/// Cross-platform font file resolver for PDF Standard-14 + common
/// document fonts. Returns absolute paths to TTF/OTF files for use
/// by MuPDF's pdf_add_simple_font / FreeType.
///
/// Lookup priority (security-first):
///   1. Bundled fonts at  <exe-dir>/fonts/  — trusted source,
///      protects against environment-controlled path substitution
///      attacks where a malicious local font could shadow a system
///      font.
///   2. OS-installed fonts queried via standard system mechanisms:
///        - Linux/macOS:  fc-match  (fontconfig)
///        - Windows:      DirectWrite (via SystemFontDiscovery)
///      No hardcoded paths — locations come from the OS so they
///      stay correct across distros, user-configured font dirs,
///      relocated system roots, and XDG_DATA_DIRS.
///   3. Metric-compatible substitution chain: when "Arial" isn't
///      installed, fall back through "Liberation Sans", "Arimo",
///      "DejaVu Sans". Glyph widths match the Microsoft originals
///      so document layout is identical.
///
/// All lookups are case-insensitive on the family name. The caller
/// can request bold/italic variants and the resolver picks the
/// closest matching face.
class FontResolver {
public:
    /// Resolve a PDF font name (e.g. "Helvetica", "Times-Roman",
    /// "Calibri") to an absolute file path. Empty string if no
    /// match found at any priority level — caller must decide
    /// whether to fall back to its own logic.
    static std::string resolveFontPath(const std::string& pdfFamilyName,
                                         bool bold = false,
                                         bool italic = false);

    /// Same as resolveFontPath but for the canonical Standard-14
    /// PDF font names with explicit weight/style suffixes:
    ///   "Helvetica-Bold", "Times-BoldItalic", "Courier-Oblique"
    /// etc. Parses the suffix and dispatches to resolveFontPath.
    static std::string resolveStandard14(const std::string& standard14Name);

    /// Override the bundled fonts directory (default: <exe-dir>/fonts).
    /// Primarily for tests. Empty string disables bundled lookup.
    static void setBundledFontsDir(const std::string& dir);

private:
    static std::string bundledFontsDir();
    static std::string queryOsFontFile(const std::string& family,
                                         bool bold, bool italic);
    static std::vector<std::string> substitutionChain(const std::string& family);
};

} // namespace pdfe::fonts
