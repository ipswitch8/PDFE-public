// Every QDialog subclass must register with the dialog-introspection seam.
//
// WHY THIS TEST EXISTS
// --------------------
// pywinauto's UIA backend cannot enumerate a Qt app-modal dialog while its
// exec() loop runs. The project's answer is
// pdfe::ui::testseam::registerDialogIntrospection(), which records the
// dialog's child inventory into a registry a test can read through a pipe.
//
// That answer was OPT-IN, and nothing failed when it was forgotten. On
// 2026-08-29 an audit found only 7 of 25 QDialog subclasses registered. The
// consequence was not loud: tests that wanted to assert on those dialogs
// quietly stopped asserting instead. From tests/ui_automation/test_ui_alt_text.py
// before that audit:
//
//     # do not assert on the resulting dialog because UIA cannot
//     # reliably enumerate it under non-interactive automation
//
// A limitation whose mitigation must be REMEMBERED gets missed one instance at
// a time. This test converts "remember to call it" into "the build tells you",
// which is the only version that survives new dialogs being added by people
// who never read the audit.
//
// IF THIS TEST FAILS you have added a QDialog subclass without registering it.
// Add, as the LAST statement of its constructor (the seam snapshots the child
// inventory, so anything parented later is invisible):
//
//     pdfe::ui::testseam::registerDialogIntrospection(this);
//
// with #include "ui/TestDialogSeam.h".
//
// It scans SOURCE rather than instantiating dialogs: constructing 25 dialogs
// needs a QApplication, real documents and, for several, a live signing or
// print stack. The property under test -- "the call is present" -- is a
// property of the source, and checking it there costs nothing at runtime.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path repoRoot()
{
    // TEST_FIXTURES_DIR is <repo>/tests/fixtures.
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Header declares a class deriving QDialog or QWizard?
bool declaresDialog(const std::string& body)
{
    return body.find(": public QDialog") != std::string::npos
        || body.find(": public QWizard") != std::string::npos;
}

} // namespace

TEST(DialogIntrospectionRegistry, EveryDialogRegistersWithTheTestSeam)
{
    const auto src = repoRoot() / "src";
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    std::vector<std::string> unregistered;
    std::vector<std::string> checked;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".h") continue;

        const std::string header = readAll(entry.path());
        if (!declaresDialog(header)) continue;

        // The implementation carries the registration call.
        std::filesystem::path impl = entry.path();
        impl.replace_extension(".cpp");
        if (!std::filesystem::exists(impl)) {
            // Header-only dialog: the call would have to live in the header.
            if (header.find("registerDialogIntrospection") == std::string::npos) {
                unregistered.push_back(entry.path().filename().string()
                                       + " (header-only)");
            }
            continue;
        }

        checked.push_back(impl.stem().string());
        if (readAll(impl).find("registerDialogIntrospection") == std::string::npos) {
            unregistered.push_back(impl.stem().string());
        }
    }

    // A scan that found nothing would pass vacuously and hide a broken glob.
    ASSERT_GE(checked.size(), 20u)
        << "only found " << checked.size() << " dialog implementations; the "
           "scan is probably broken rather than the codebase having shrunk";

    std::sort(unregistered.begin(), unregistered.end());
    std::string joined;
    for (const auto& u : unregistered) joined += "\n    " + u;

    EXPECT_TRUE(unregistered.empty())
        << unregistered.size() << " QDialog subclass(es) do not call "
           "registerDialogIntrospection(this):" << joined
        << "\n\nAdd it as the LAST statement of the constructor, with "
           "#include \"ui/TestDialogSeam.h\". Without it, UIA cannot observe "
           "the dialog while its modal exec() runs and tests silently stop "
           "asserting on it. See docs/audit/ui-test-observability-audit.md.";
}
