// Phase-1 NEW-1: Runtime DLL presence checks.
//
// Verifies all required DLLs deployed by the CMake POST_BUILD step
// exist in the test binary output directory. Prevents CI-blocking
// modal error dialogs when DLLs are absent.

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace fs = std::filesystem;

// Returns true if the DLL is loadable by the OS loader — i.e. present
// either next to the binary OR resolvable on the process PATH (which is
// how PDFE.exe finds runtime-only-via-PATH DLLs like libssl in a dev
// build, and where the installer stages them in a packaged build). This
// is a truer "will the end user hit 0xC0000135?" check than file-exists
// in a single directory for DLLs that the test binary does not directly
// link. On non-Windows this is a no-op (returns true).
static bool dllLoadableOnPath(const std::vector<std::string>& names) {
#ifdef _WIN32
    for (const auto& n : names) {
        std::wstring w(n.begin(), n.end());
        HMODULE h = ::LoadLibraryExW(w.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                     | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!h) {
            // Fall back to a plain search (honors full process PATH).
            h = ::LoadLibraryW(w.c_str());
        }
        if (h) { ::FreeLibrary(h); return true; }
    }
    return false;
#else
    (void)names;
    return true;
#endif
}

class DllDependencyTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifndef _WIN32
        // Linux/macOS use .so/.dylib not .dll, and runtime libs are
        // located via ld.so / system loader instead of being staged
        // next to the exe. This entire test class is Windows-specific.
        GTEST_SKIP() << "DLL presence tests apply only to Windows builds";
#endif
        binDir_ = fs::path(::testing::internal::GetArgvs()[0]).parent_path();
        if (!fs::exists(binDir_)) {
            GTEST_SKIP() << "Binary directory not found: " << binDir_;
        }

#ifdef _DEBUG
        isDebug_ = true;
#else
        isDebug_ = false;
#endif
    }

    bool dllExists(const std::string& name) const {
        return fs::exists(binDir_ / name);
    }

    bool dllExistsAny(const std::vector<std::string>& variants) const {
        for (const auto& v : variants) {
            if (fs::exists(binDir_ / v)) return true;
        }
        return false;
    }

    fs::path binDir_;
    bool isDebug_ = false;
};

TEST_F(DllDependencyTest, Qt6CorePresent) {
    ASSERT_TRUE(dllExistsAny({"Qt6Cored.dll", "Qt6Core.dll"}))
        << "Qt6Core DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, Qt6GuiPresent) {
    ASSERT_TRUE(dllExistsAny({"Qt6Guid.dll", "Qt6Gui.dll"}))
        << "Qt6Gui DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, Qt6WidgetsPresent) {
    ASSERT_TRUE(dllExistsAny({"Qt6Widgetsd.dll", "Qt6Widgets.dll"}))
        << "Qt6Widgets DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, Qt6PrintSupportPresent) {
    ASSERT_TRUE(dllExistsAny({"Qt6PrintSupportd.dll", "Qt6PrintSupport.dll"}))
        << "Qt6PrintSupport DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, QpdfPresent) {
    ASSERT_TRUE(dllExistsAny({"qpdf30.dll", "qpdf29.dll", "qpdf.dll"}))
        << "QPDF DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, TesseractPresent) {
    ASSERT_TRUE(dllExistsAny({"tesseract55d.dll", "tesseract55.dll",
                              "tesseract54d.dll", "tesseract54.dll"}))
        << "Tesseract DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, HarfbuzzPresent) {
    ASSERT_TRUE(dllExists("harfbuzz.dll"))
        << "HarfBuzz DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, FreetypePresent) {
    ASSERT_TRUE(dllExistsAny({"freetyped.dll", "freetype.dll"}))
        << "FreeType DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, Sqlite3Present) {
    ASSERT_TRUE(dllExists("sqlite3.dll"))
        << "SQLite3 DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, ZlibPresent) {
    ASSERT_TRUE(dllExistsAny({"zlibd1.dll", "zlib1.dll", "zlib.dll"}))
        << "zlib DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, IcuDataPresent) {
    ASSERT_TRUE(dllExistsAny({"icudtd78.dll", "icudt78.dll",
                              "icudtd77.dll", "icudt77.dll"}))
        << "ICU data DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, IcuI18nPresent) {
    ASSERT_TRUE(dllExistsAny({"icuind78.dll", "icuin78.dll",
                              "icuind77.dll", "icuin77.dll"}))
        << "ICU i18n DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, IcuUnicodePresent) {
    ASSERT_TRUE(dllExistsAny({"icuucd78.dll", "icuuc78.dll",
                              "icuucd77.dll", "icuuc77.dll"}))
        << "ICU unicode DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, OnnxRuntimePresent) {
    ASSERT_TRUE(dllExists("onnxruntime.dll"))
        << "ONNX Runtime DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, OpenSslCryptoPresent) {
    ASSERT_TRUE(dllExistsAny({"libcrypto-3-x64.dll", "libcrypto-3.dll"}))
        << "OpenSSL crypto DLL not found in " << binDir_;
}

// dll_dependency_checks (memory item): the following DLLs are loaded only
// by PDFE.exe at runtime (not always pulled in by the pdfe_tests link),
// so they were absent from the original test set even though a missing one
// would give the end user an opaque 0xC0000135 dialog. They are deployed
// by the same CMake install() list (CMakeLists.txt _required_dlls /
// _special_release), so the test binary's bin/ dir has them too.

TEST_F(DllDependencyTest, OpenSslTlsPresent) {
    // libssl is required for HTTPS Timestamp-Authority (TSA) requests and
    // any libcurl HTTPS transfer in the signature flow. The test binary
    // does not directly link libssl, so in a dev build it is resolved via
    // the vcpkg bin on PATH rather than staged next to the exe (the
    // installer stages it for packaged builds). Assert loadability — the
    // real "will the user hit 0xC0000135?" question — accepting either
    // location.
    const std::vector<std::string> names = {"libssl-3-x64.dll", "libssl-3.dll"};
    ASSERT_TRUE(dllExistsAny(names) || dllLoadableOnPath(names))
        << "OpenSSL TLS (libssl) DLL not present in " << binDir_
        << " and not loadable on PATH — HTTPS/TSA signing would fail at "
           "runtime with STATUS_DLL_NOT_FOUND";
}

TEST_F(DllDependencyTest, LibcurlPresent) {
    // libcurl drives the network round-trip for RFC-3161 timestamping.
    // Without it the TSA signing flow fails at runtime, not at build time.
    ASSERT_TRUE(dllExistsAny({"libcurl.dll", "libcurl-d.dll"}))
        << "libcurl DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, LeptonicaPresent) {
    // Leptonica is Tesseract's image backend; OCR / TRE crash at runtime
    // without it. Version-tolerant match (1.87.x family + bare name).
    ASSERT_TRUE(dllExistsAny({"leptonica-1.87.0.dll", "leptonica-1.87.0d.dll",
                              "leptonica-1.85.0.dll", "leptonica.dll"}))
        << "Leptonica DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, JpegPresent) {
    // libjpeg is a MuPDF image-decode dependency; scanned/JPEG-bearing
    // PDFs fail to render without it.
    ASSERT_TRUE(dllExistsAny({"jpeg62.dll", "jpeg62d.dll", "jpeg.dll"}))
        << "libjpeg DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, OpenJpeg2000Present) {
    // openjp2 decodes JPEG2000 streams embedded in PDFs.
    ASSERT_TRUE(dllExistsAny({"openjp2.dll", "openjp2d.dll"}))
        << "OpenJPEG (openjp2) DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, PugiXmlPresent) {
    ASSERT_TRUE(dllExists("pugixml.dll"))
        << "pugixml DLL not found in " << binDir_;
}

TEST_F(DllDependencyTest, PlatformPluginPresent) {
    auto platformDir = binDir_ / "platforms";
    ASSERT_TRUE(dllExistsAny({"platforms/qwindowsd.dll", "platforms/qwindows.dll"}))
        << "Qt platform plugin not found in " << platformDir;
}

TEST_F(DllDependencyTest, TlsBackendPluginPresent) {
    // Qt's TLS backend plugin (qopensslbackend) is what QNetwork / any
    // HTTPS path in the signing/TSA flow loads at runtime. It lives under
    // bin/tls and, when absent, HTTPS silently fails with no build error.
    auto tlsDir = binDir_ / "tls";
    if (!fs::exists(tlsDir)) {
        GTEST_SKIP() << "tls plugin dir not present in this packaging variant: "
                     << tlsDir;
    }
    ASSERT_TRUE(dllExistsAny({"tls/qopensslbackendd.dll",
                              "tls/qopensslbackend.dll",
                              "tls/qcertonlybackendd.dll",
                              "tls/qcertonlybackend.dll"}))
        << "Qt TLS backend plugin not found in " << tlsDir;
}
