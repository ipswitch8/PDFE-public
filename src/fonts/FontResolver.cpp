#include "FontResolver.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
    #include "core/SystemFontDiscovery.h"
    #include "core/FontMetrics.h"
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    // Linux/macOS: use fc-match via popen() for fontconfig query.
    #include <cstdio>
    #include <memory>
#endif

namespace pdfe::fonts {

namespace {

std::string g_bundledOverride;
std::mutex g_bundledOverrideMu;

std::string toLowerNoSep(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '-' || c == ' ' || c == '_' || c == ',') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

#ifndef _WIN32
/// Run `fc-match -f '%{file}' "<family>:weight=<n>:slant=<n>"` and
/// return the file path. Returns empty on failure.
std::string fcMatch(const std::string& family, bool bold, bool italic)
{
    // Build the pattern with weight/slant requests so fc-match picks
    // the right face. fontconfig weight: 80=normal, 200=bold;
    // slant: 0=roman, 100=italic.
    std::string pattern = family;
    if (bold)   pattern += ":weight=200";
    if (italic) pattern += ":slant=100";

    // Escape single quotes in the family name for the shell.
    std::string escaped;
    escaped.reserve(pattern.size() + 8);
    for (char c : pattern) {
        if (c == '\'') escaped += R"('\'')";
        else escaped.push_back(c);
    }

    std::string cmd = "fc-match -f '%{file}' '" + escaped + "' 2>/dev/null";
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return {};

    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe.get())) {
        result.append(buf);
    }
    // fc-match emits no trailing newline with %{file}, but trim just in case.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    // Sanity: must be an absolute path that exists.
    if (result.empty() || result[0] != '/') return {};
    if (!std::filesystem::exists(result)) return {};
    return result;
}
#endif // !_WIN32

/// Family equivalence chain: when the requested family isn't
/// installed, the resolver walks this list in order.
/// Metric-compatible substitutes have matching glyph widths.
const std::vector<std::string>& chainFor(const std::string& family)
{
    static const std::vector<std::string> empty;
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        // Standard 14 + common Microsoft fonts → metric-compatible
        // Linux equivalents
        {"helvetica",      {"Helvetica", "Arial", "Liberation Sans",
                            "Arimo", "Nimbus Sans", "DejaVu Sans"}},
        {"arial",          {"Arial", "Liberation Sans", "Arimo",
                            "Nimbus Sans", "DejaVu Sans"}},
        {"arialmt",        {"Arial", "Liberation Sans", "Arimo",
                            "Nimbus Sans", "DejaVu Sans"}},
        {"timesroman",     {"Times New Roman", "Liberation Serif",
                            "Tinos", "Nimbus Roman", "DejaVu Serif"}},
        {"times",          {"Times New Roman", "Liberation Serif",
                            "Tinos", "Nimbus Roman", "DejaVu Serif"}},
        {"timesnewroman",  {"Times New Roman", "Liberation Serif",
                            "Tinos", "Nimbus Roman", "DejaVu Serif"}},
        {"courier",        {"Courier New", "Liberation Mono",
                            "Cousine", "Nimbus Mono", "DejaVu Sans Mono"}},
        {"couriernew",     {"Courier New", "Liberation Mono",
                            "Cousine", "Nimbus Mono", "DejaVu Sans Mono"}},
        {"calibri",        {"Calibri", "Carlito"}},
        {"cambria",        {"Cambria", "Caladea"}},
        {"georgia",        {"Georgia", "Liberation Serif", "DejaVu Serif"}},
        {"verdana",        {"Verdana", "Liberation Sans", "DejaVu Sans"}},
        {"tahoma",         {"Tahoma", "Liberation Sans", "DejaVu Sans"}},
        {"trebuchet",      {"Trebuchet MS", "Liberation Sans", "DejaVu Sans"}},
        {"consolas",       {"Consolas", "Liberation Mono", "DejaVu Sans Mono"}},
        {"palatino",       {"Palatino Linotype", "Liberation Serif",
                            "DejaVu Serif"}},
        {"garamond",       {"Garamond", "Liberation Serif", "DejaVu Serif"}},
        {"symbol",         {"Symbol", "DejaVu Sans"}},
        {"zapfdingbats",   {"ZapfDingbats", "Wingdings", "DejaVu Sans"}},
        // OCR invisible fonts → any sans-serif
        {"glyphlessfont",  {"Liberation Sans", "Arial", "DejaVu Sans"}},
        {"invisiblefont",  {"Liberation Sans", "Arial", "DejaVu Sans"}},
        {"hiddenocrfont",  {"Liberation Sans", "Arial", "DejaVu Sans"}},
        {"ocrfont",        {"Liberation Sans", "Arial", "DejaVu Sans"}},
        {"ocra",           {"Liberation Mono", "Courier New", "DejaVu Sans Mono"}},
        {"ocrb",           {"Liberation Mono", "Courier New", "DejaVu Sans Mono"}},
        {"finereaderfont", {"Liberation Sans", "Arial", "DejaVu Sans"}},
        {"adobeblank",     {"Liberation Sans", "Arial", "DejaVu Sans"}},
        // Linux native names — if a doc was authored with these,
        // map them to themselves (or Microsoft equivalents).
        {"liberationsans", {"Liberation Sans", "Arial", "Arimo"}},
        {"liberationserif",{"Liberation Serif", "Times New Roman", "Tinos"}},
        {"liberationmono", {"Liberation Mono", "Courier New", "Cousine"}},
        {"arimo",          {"Arimo", "Arial", "Liberation Sans"}},
        {"tinos",          {"Tinos", "Times New Roman", "Liberation Serif"}},
        {"cousine",        {"Cousine", "Courier New", "Liberation Mono"}},
        {"carlito",        {"Carlito", "Calibri"}},
        {"caladea",        {"Caladea", "Cambria"}},
    };
    auto it = table.find(toLowerNoSep(family));
    return (it != table.end()) ? it->second : empty;
}

/// Build the bundled font file name for a family + bold/italic.
/// Returns names like "LiberationSans-Bold.ttf" that can be
/// looked up under the bundled fonts dir.
std::string bundledNameFor(const std::string& family, bool bold, bool italic)
{
    // Strip whitespace
    std::string fam;
    fam.reserve(family.size());
    for (char c : family) if (c != ' ') fam.push_back(c);
    const char* suffix = bold && italic ? "-BoldItalic"
                       : bold           ? "-Bold"
                       : italic         ? "-Italic"
                                        : "-Regular";
    return fam + suffix + ".ttf";
}

} // anonymous namespace

void FontResolver::setBundledFontsDir(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(g_bundledOverrideMu);
    g_bundledOverride = dir;
}

std::string FontResolver::bundledFontsDir()
{
    {
        std::lock_guard<std::mutex> lock(g_bundledOverrideMu);
        if (!g_bundledOverride.empty()) return g_bundledOverride;
    }

    // Derive from executable path. Layout:
    //   <exe-dir>/pdfe(.exe)
    //   <exe-dir>/fonts/  ← bundled here
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    wchar_t wpath[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    fs::path exePath(wpath);
#else
    fs::path exePath = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
#endif
    fs::path candidate = exePath.parent_path() / "fonts";
    if (fs::is_directory(candidate, ec)) return candidate.string();
    return {};
}

std::string FontResolver::queryOsFontFile(const std::string& family,
                                            bool bold, bool italic)
{
#ifdef _WIN32
    static thread_local pdfe::core::SystemFontDiscovery discovery;
    auto weight = bold ? pdfe::core::FontWeight::Bold
                       : pdfe::core::FontWeight::Normal;
    auto style  = italic ? pdfe::core::FontStyle::Italic
                         : pdfe::core::FontStyle::Normal;
    auto result = discovery.findByFamilyStyle(family, weight, style);
    if (result.has_value() && std::filesystem::exists(result->filePath)) {
        return result->filePath;
    }
    return {};
#else
    return fcMatch(family, bold, italic);
#endif
}

std::vector<std::string> FontResolver::substitutionChain(const std::string& family)
{
    return chainFor(family);
}

std::string FontResolver::resolveFontPath(const std::string& pdfFamilyName,
                                            bool bold, bool italic)
{
    if (pdfFamilyName.empty()) return {};

    // Build the candidate chain: requested family first, then any
    // metric-compatible substitutes.
    std::vector<std::string> candidates;
    candidates.push_back(pdfFamilyName);
    for (const auto& alt : chainFor(pdfFamilyName)) {
        // Avoid duplicates from chains that include the original.
        if (alt != pdfFamilyName) candidates.push_back(alt);
    }

    namespace fs = std::filesystem;
    const std::string bundleDir = bundledFontsDir();

    for (const auto& candidate : candidates) {
        // (1) Bundled fonts first — security: trusted source.
        if (!bundleDir.empty()) {
            fs::path p = fs::path(bundleDir) / bundledNameFor(candidate, bold, italic);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p.string();
        }

        // (2) OS query via fontconfig (Linux/macOS) or DirectWrite (Win).
        std::string osPath = queryOsFontFile(candidate, bold, italic);
        if (!osPath.empty()) return osPath;
    }

    return {};
}

std::string FontResolver::resolveStandard14(const std::string& standard14Name)
{
    if (standard14Name.empty()) return {};
    // Split on '-' suffix: "Helvetica-BoldOblique" → family=Helvetica,
    // bold=true, italic=true.
    auto dash = standard14Name.find('-');
    std::string family = (dash == std::string::npos)
                            ? standard14Name
                            : standard14Name.substr(0, dash);
    std::string suffix = (dash == std::string::npos)
                            ? std::string{}
                            : standard14Name.substr(dash + 1);
    bool bold   = false, italic = false;
    std::string lower = suffix;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (lower.find("bold")    != std::string::npos) bold   = true;
    if (lower.find("italic")  != std::string::npos ||
        lower.find("oblique") != std::string::npos) italic = true;
    return resolveFontPath(family, bold, italic);
}

} // namespace pdfe::fonts
