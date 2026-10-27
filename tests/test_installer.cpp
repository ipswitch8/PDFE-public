// test_installer.cpp
// Unit tests for CMake install/CPack configuration

#include <gtest/gtest.h>
#include <QFile>
#include <QTextStream>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Helper to get the project root directory
std::filesystem::path getProjectRoot() {
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

// Helper to read file contents into a string
static std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // anonymous namespace

//
// Root CMakeLists.txt Tests
//

TEST(CMakeLists, HasProjectVersion) {
    // Arrange
    std::filesystem::path cmakePath = getProjectRoot() / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read CMakeLists.txt from: " << cmakePath.string();
    EXPECT_NE(contents.find("project(pdfe VERSION"), std::string::npos)
        << "Root CMakeLists.txt should contain project version declaration";
}

TEST(CMakeLists, HasInstallDirectives) {
    // Arrange - install(TARGETS) is in src/app/CMakeLists.txt; root has install(FILES)/install(DIRECTORY)
    std::filesystem::path appCmakePath = getProjectRoot() / "src" / "app" / "CMakeLists.txt";
    std::string appContents = readFileContents(appCmakePath);

    // Act & Assert
    EXPECT_FALSE(appContents.empty())
        << "Should be able to read app CMakeLists.txt from: " << appCmakePath.string();
    EXPECT_NE(appContents.find("install(TARGETS"), std::string::npos)
        << "App CMakeLists.txt should contain install(TARGETS directive";
}

TEST(CMakeLists, InstallsCnnFontModel) {
    // Regression guard (c82a1f37 follow-up): the font embedder loads
    // <exe_dir>/models/font_embedder.onnx at runtime (src/ui/MainWindow.cpp).
    // src/app/CMakeLists.txt POST_BUILD stages it into the BUILD tree, but the
    // INSTALLER (root CMakeLists install rules) previously had NO rule for it, so
    // an installed app silently lost CNN font re-ranking. Assert the installer
    // now bundles the model at bin/models.
    std::filesystem::path cmakePath = getProjectRoot() / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);
    ASSERT_FALSE(contents.empty());
    EXPECT_NE(contents.find("models/font_embedder.onnx"), std::string::npos)
        << "Installer must bundle the CNN font model "
           "(install(FILES .../models/font_embedder.onnx DESTINATION bin/models))";
}

TEST(CMakeLists, InstallerBundlesDeployedRuntimeAssets) {
    // Regression guard against deploy-vs-install DRIFT: every runtime asset the
    // app's POST_BUILD deploy stages (src/app/CMakeLists.txt) must ALSO appear in
    // the installer's install() rules (root CMakeLists.txt), or an installed app
    // differs from a dev/CI build. Checks the assets added over time that are the
    // most likely to drift.
    std::string install = readFileContents(getProjectRoot() / "CMakeLists.txt");
    ASSERT_FALSE(install.empty());
    EXPECT_NE(install.find("eng.traineddata"), std::string::npos) << "tessdata eng";
    EXPECT_NE(install.find("ara.traineddata"), std::string::npos) << "tessdata ara";
    EXPECT_NE(install.find("osd.traineddata"), std::string::npos) << "tessdata osd";
    EXPECT_NE(install.find("onnxruntime.dll"), std::string::npos) << "ONNX Runtime";
    EXPECT_NE(install.find("libssl-3-x64"), std::string::npos) << "OpenSSL libssl";
    // Qt module DLLs and plugins (including the TLS backends such as
    // qopensslbackend) are now deployed by windeployqt rather than an explicit
    // per-plugin install() line, so assert the windeployqt step is present.
    EXPECT_NE(install.find("windeployqt"), std::string::npos) << "Qt runtime via windeployqt";
    EXPECT_NE(install.find("models/font_embedder.onnx"), std::string::npos) << "CNN model";
}

TEST(CMakeLists, HasCPack) {
    // Arrange
    std::filesystem::path cmakePath = getProjectRoot() / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read CMakeLists.txt from: " << cmakePath.string();
    EXPECT_NE(contents.find("include(CPack)"), std::string::npos)
        << "Root CMakeLists.txt should include CPack module";
}

TEST(CMakeLists, HasCPackNSIS) {
    // Arrange
    std::filesystem::path cmakePath = getProjectRoot() / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read CMakeLists.txt from: " << cmakePath.string();
    EXPECT_NE(contents.find("CPACK_GENERATOR"), std::string::npos)
        << "Root CMakeLists.txt should specify CPACK_GENERATOR";
    EXPECT_NE(contents.find("NSIS"), std::string::npos)
        << "Root CMakeLists.txt should configure NSIS generator";
}

//
// App CMakeLists.txt Tests
//

TEST(AppCMakeLists, HasResourceFile) {
    // Arrange
    std::filesystem::path cmakePath = getProjectRoot() / "src" / "app" / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read app CMakeLists.txt from: " << cmakePath.string();
    EXPECT_NE(contents.find("pdfe.rc"), std::string::npos)
        << "App CMakeLists.txt should reference pdfe.rc resource file";
}

TEST(AppCMakeLists, HasInstall) {
    // Arrange
    std::filesystem::path cmakePath = getProjectRoot() / "src" / "app" / "CMakeLists.txt";
    std::string contents = readFileContents(cmakePath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read app CMakeLists.txt from: " << cmakePath.string();
    EXPECT_NE(contents.find("install("), std::string::npos)
        << "App CMakeLists.txt should contain install directive";
}

//
// Resource File Tests
//

TEST(ResourceFile, HasIcon) {
    // Arrange
    std::filesystem::path rcPath = getProjectRoot() / "src" / "app" / "pdfe.rc";
    std::string contents = readFileContents(rcPath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read pdfe.rc from: " << rcPath.string();
    EXPECT_NE(contents.find("ICON"), std::string::npos)
        << "Resource file should contain ICON declaration";
}

TEST(ResourceFile, HasVersionInfo) {
    // Arrange
    std::filesystem::path rcPath = getProjectRoot() / "src" / "app" / "pdfe.rc";
    std::string contents = readFileContents(rcPath);

    // Act & Assert
    EXPECT_FALSE(contents.empty())
        << "Should be able to read pdfe.rc from: " << rcPath.string();
    EXPECT_NE(contents.find("VS_VERSION_INFO"), std::string::npos)
        << "Resource file should contain VS_VERSION_INFO";
    EXPECT_NE(contents.find("VERSIONINFO"), std::string::npos)
        << "Resource file should contain VERSIONINFO block";
}

//
// License File Tests
//

TEST(LicenseFile, Exists) {
    // Arrange
    std::filesystem::path licensePath = getProjectRoot() / "LICENSE";

    // Act & Assert
    EXPECT_TRUE(std::filesystem::exists(licensePath))
        << "LICENSE file should exist at project root: " << licensePath.string();
}

//
// Installer Residue Tests (R3-14)
//
// Static completeness verification of the NSIS uninstaller: a real
// install->uninstall on the live machine mutates the registry and requires
// admin, so it is not CI-safe. Instead these tests parse the AUTHORED installer
// sources from the source tree and assert install/uninstall SYMMETRY — every
// install-time artifact (file, shortcut, registry write) has a matching
// uninstall directive — plus that the HKCU user-data intent is explicit and the
// config matches it. Deterministic, no admin, no system mutation.
//
// Sources parsed:
//   packaging/file_assoc.nsh           -- install: desktop shortcut + reg writes
//   packaging/file_assoc_uninstall.nsh -- uninstall: matching removals
//   CMakeLists.txt                     -- CPack start-menu shortcut create/delete
//   docs/installer-residue-and-hkcu-intent.md -- HKCU intent of record
//

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimLeft(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::vector<std::string> splitLines(const std::string& contents) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : contents) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// The first double-quoted substring of a line, lowercased. Empty if none.
std::string firstQuoted(const std::string& line) {
    size_t a = line.find('"');
    if (a == std::string::npos) return "";
    size_t b = line.find('"', a + 1);
    if (b == std::string::npos) return "";
    return toLower(line.substr(a + 1, b - a - 1));
}

// A parsed registry directive: root (hkcr/hklm/hkcu/...) + key path, lowercased.
struct RegDir {
    std::string root;
    std::string key;
    std::string raw;  // original line, for diagnostics
};

// Parse WriteRegStr / WriteRegDWORD / DeleteRegKey / DeleteRegValue lines whose
// command (case-insensitive) starts with `verbPrefix` (e.g. "writereg",
// "deleteregkey", "deleteregvalue"). Layout: <VERB> <ROOT> "<KEY>" ...
std::vector<RegDir> parseReg(const std::string& contents, const std::string& verbPrefix) {
    std::vector<RegDir> out;
    for (const std::string& rawLine : splitLines(contents)) {
        std::string line = trimLeft(rawLine);
        if (line.empty() || line[0] == ';') continue;  // NSIS comment
        // token 0 = verb
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string verb = toLower(line.substr(0, sp));
        if (verb.rfind(verbPrefix, 0) != 0) continue;
        // token 1 = root
        std::string rest = trimLeft(line.substr(sp));
        size_t sp2 = rest.find_first_of(" \t");
        if (sp2 == std::string::npos) continue;
        std::string root = toLower(rest.substr(0, sp2));
        std::string key = firstQuoted(rest);
        if (root.empty() || key.empty()) continue;
        out.push_back({root, key, rawLine});
    }
    return out;
}

// True if `full` is `anc` itself or a descendant key of `anc` (registry key
// separator is backslash). Both must already be lowercased.
bool keyCoveredBy(const std::string& full, const std::string& anc) {
    if (full == anc) return true;
    if (full.rfind(anc, 0) != 0) return false;
    return full.size() > anc.size() && full[anc.size()] == '\\';
}

} // anonymous namespace

// Every registry write in the install .nsh must have a matching removal in the
// uninstall .nsh: either a DeleteRegKey of that key or an ancestor of it, or a
// DeleteRegValue of exactly that key. This is the core symmetry oracle — it
// re-fires (RED) if any single uninstall directive is dropped.
TEST(InstallerResidue, EveryRegistryWriteHasMatchingRemoval) {
    std::string install = readFileContents(getProjectRoot() / "packaging" / "file_assoc.nsh");
    std::string uninstall = readFileContents(getProjectRoot() / "packaging" / "file_assoc_uninstall.nsh");
    ASSERT_FALSE(install.empty()) << "file_assoc.nsh must be readable";
    ASSERT_FALSE(uninstall.empty()) << "file_assoc_uninstall.nsh must be readable";

    std::vector<RegDir> writes = parseReg(install, "writereg");
    std::vector<RegDir> delKeys = parseReg(uninstall, "deleteregkey");
    std::vector<RegDir> delVals = parseReg(uninstall, "deleteregvalue");
    ASSERT_FALSE(writes.empty()) << "expected the installer to write registry entries";

    for (const RegDir& w : writes) {
        bool matched = false;
        for (const RegDir& d : delKeys) {
            if (d.root == w.root && keyCoveredBy(w.key, d.key)) { matched = true; break; }
        }
        if (!matched) {
            for (const RegDir& d : delVals) {
                if (d.root == w.root && d.key == w.key) { matched = true; break; }
            }
        }
        EXPECT_TRUE(matched)
            << "Uninstall residue: registry write has no matching DeleteRegKey/"
               "DeleteRegValue in file_assoc_uninstall.nsh -> "
            << w.root << " \"" << w.key << "\"  (from: " << trimLeft(w.raw) << ")";
    }
}

// The desktop shortcut created in file_assoc.nsh must be deleted in the
// uninstall .nsh.
TEST(InstallerResidue, DesktopShortcutRemoved) {
    std::string install = readFileContents(getProjectRoot() / "packaging" / "file_assoc.nsh");
    std::string uninstall = readFileContents(getProjectRoot() / "packaging" / "file_assoc_uninstall.nsh");
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(uninstall.empty());

    // Install creates $DESKTOP\PDFE.lnk ...
    EXPECT_NE(toLower(install).find("createshortcut \"$desktop\\pdfe.lnk\""), std::string::npos)
        << "expected the installer to create a desktop shortcut";
    // ... uninstall must Delete it.
    EXPECT_NE(toLower(uninstall).find("delete \"$desktop\\pdfe.lnk\""), std::string::npos)
        << "Uninstall residue: desktop shortcut $DESKTOP\\PDFE.lnk is not removed "
           "by file_assoc_uninstall.nsh";
}

// The Start-menu shortcut created via CPACK_NSIS_CREATE_ICONS_EXTRA must have a
// matching CPACK_NSIS_DELETE_ICONS_EXTRA removal in the root CMakeLists.txt.
TEST(InstallerResidue, StartMenuShortcutRemoved) {
    std::string cmake = readFileContents(getProjectRoot() / "CMakeLists.txt");
    ASSERT_FALSE(cmake.empty());
    std::string lc = toLower(cmake);

    EXPECT_NE(lc.find("cpack_nsis_create_icons_extra"), std::string::npos)
        << "expected a Start-menu shortcut create directive";
    ASSERT_NE(lc.find("cpack_nsis_delete_icons_extra"), std::string::npos)
        << "Uninstall residue: no CPACK_NSIS_DELETE_ICONS_EXTRA to remove the "
           "Start-menu shortcut";
    // The delete directive must actually reference the same .lnk it created.
    size_t delPos = lc.find("cpack_nsis_delete_icons_extra");
    std::string delTail = lc.substr(delPos, 300);
    EXPECT_NE(delTail.find("pdfe.lnk"), std::string::npos)
        << "Uninstall residue: DELETE_ICONS_EXTRA does not remove PDFE.lnk";
    EXPECT_NE(delTail.find("delete "), std::string::npos)
        << "Uninstall residue: DELETE_ICONS_EXTRA does not issue a Delete";
}

// The Qt runtime is deployed by windeployqt at install time. Its output must
// land INSIDE the install prefix so CPack's file manifest captures it and the
// generated uninstaller removes it -- otherwise the entire Qt runtime is
// uninstall residue. Assert the deploy target is ${CMAKE_INSTALL_PREFIX}/bin.
TEST(InstallerResidue, QtRuntimeDeployedInsidePrefix) {
    std::string cmake = readFileContents(getProjectRoot() / "CMakeLists.txt");
    ASSERT_FALSE(cmake.empty());
    std::string lc = toLower(cmake);
    ASSERT_NE(lc.find("windeployqt"), std::string::npos);
    // windeployqt --dir must be under the CPack-managed install prefix. (The
    // CMake source escapes the quote/variable as \"\${...}\", so match on the
    // escape-stable tail rather than the exact quoted form.)
    EXPECT_NE(lc.find("--dir"), std::string::npos)
        << "windeployqt must be invoked with an explicit --dir";
    EXPECT_NE(lc.find("cmake_install_prefix}/bin"), std::string::npos)
        << "Uninstall residue: windeployqt must deploy into "
           "${CMAKE_INSTALL_PREFIX}/bin so its files are tracked by the CPack "
           "install manifest and removed on uninstall";
}

// HKCU user-data intent must be EXPLICIT and documented. The uninstaller does
// not remove HKCU\Software\PDFE, so the acceptance criterion requires the
// preservation decision to be written down with a rationale.
TEST(InstallerResidue, HkcuIntentDocumented) {
    std::string doc = readFileContents(
        getProjectRoot() / "docs" / "installer-residue-and-hkcu-intent.md");
    ASSERT_FALSE(doc.empty())
        << "Missing HKCU user-data intent note: "
           "docs/installer-residue-and-hkcu-intent.md must document whether "
           "HKCU\\Software\\PDFE is removed or preserved on uninstall, with a "
           "rationale (R3-14 acceptance criterion).";
    std::string lc = toLower(doc);
    EXPECT_NE(lc.find("hkcu\\software\\pdfe"), std::string::npos)
        << "intent note must name the HKCU user-data key";
    // The decision keyword must be present so the intent is unambiguous.
    EXPECT_TRUE(lc.find("preserv") != std::string::npos ||
                lc.find("remove") != std::string::npos)
        << "intent note must state a decision (preserved vs removed)";
    EXPECT_NE(lc.find("rationale"), std::string::npos)
        << "intent note must give a rationale for the decision";
}

// The uninstaller config must MATCH the documented "preserve" intent: it removes
// the machine-wide HKLM\Software\PDFE app registration but must NOT delete the
// per-user HKCU\Software\PDFE settings hive. This guards against a future
// "residue fix" that over-reaches and wipes user data contrary to intent.
TEST(InstallerResidue, HkcuIntentMatchesConfig) {
    std::string uninstall = readFileContents(
        getProjectRoot() / "packaging" / "file_assoc_uninstall.nsh");
    ASSERT_FALSE(uninstall.empty());

    std::vector<RegDir> delKeys = parseReg(uninstall, "deleteregkey");
    std::vector<RegDir> delVals = parseReg(uninstall, "deleteregvalue");

    bool removesHklmPdfe = false;
    bool touchesHkcu = false;
    for (const RegDir& d : delKeys) {
        if (d.root == "hklm" && keyCoveredBy("software\\pdfe", d.key)) removesHklmPdfe = true;
        if (d.root == "hkcu" && keyCoveredBy("software\\pdfe", d.key)) touchesHkcu = true;
    }
    for (const RegDir& d : delVals) {
        if (d.root == "hkcu" && keyCoveredBy("software\\pdfe", d.key)) touchesHkcu = true;
    }

    EXPECT_TRUE(removesHklmPdfe)
        << "Uninstall residue: machine-wide HKLM\\Software\\PDFE app "
           "registration is not removed on uninstall";
    EXPECT_FALSE(touchesHkcu)
        << "Intent mismatch: uninstaller deletes HKCU\\Software\\PDFE, but the "
           "documented intent (docs/installer-residue-and-hkcu-intent.md) is to "
           "PRESERVE per-user data. Update the doc or the uninstaller so they agree.";
}
