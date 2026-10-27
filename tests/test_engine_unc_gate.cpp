// -*- coding: utf-8 -*-
//
// Regression tests for the engine-level UNC gate.
//
// Phase-1 of the security hardening pipeline only gated Document::open
// and the MainWindow link dispatcher. Five engines (BatchEngine,
// OptimizeEngine, RedactionVerifier, AlignmentInjector,
// HeaderFooterEngine) call pdf_open_document / fz_open_document
// directly on user-supplied paths and were left as a deferred
// follow-up. The corpus run on 2026-04-26 confirmed the gap was real
// (uri_unc/test34_1.pdf still fails because its inline /F filespec
// goes through pdf_open_document).
//
// These tests assert that each public engine entry point now refuses
// UNC inputs. They do NOT need a real UNC share to exist: a `\\server\share`
// string is rejected on its shape, before anything resolves it.
//
// SEC-50 (phase-61) CORRECTION. This header used to say "the check is purely
// lexical", and that is no longer true of the check — only of these inputs.
// `path_security::is_unc_or_remote` now also RESOLVES a leading drive letter,
// because `net use Z: \\server\share` produces a local-looking string that
// reaches the same redirector, and a lexical gate passed it. The tests below
// are unaffected (they pass UNC-shaped strings, which never reach the
// resolution step), but the sentence was a live claim about the product and
// went stale the moment the product changed. Drive-resolution coverage is in
// `tests/test_path_security.cpp::MappedDrive*`; WIN-62's separate point —
// that no engine test touches a real loopback share — still stands.

#include <gtest/gtest.h>

#include "batch/BatchEngine.h"
#include "core/MuPdfContext.h"
#include "headerfooter/HeaderFooterEngine.h"
#include "optimize/OptimizeEngine.h"
#include "redaction/RedactionVerifier.h"
#include "reflow/AlignmentInjector.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace {

constexpr const char* kUncPath  = "\\\\attacker.example\\share\\evil.pdf";
constexpr const char* kLocalOut = "C:\\Users\\Public\\nonexistent_out.pdf";

std::shared_ptr<pdfe::core::MuPdfContext> makeCtx() {
    return std::make_shared<pdfe::core::MuPdfContext>();
}

} // namespace

TEST(EngineUncGate, OptimizeEngineRefusesUnc) {
    pdfe::optimize::OptimizeEngine engine(makeCtx());
    pdfe::optimize::OptimizeOptions opts;
    auto result = engine.run(kUncPath, kLocalOut, opts);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_NE(result.errorMessage.find("UNC"), std::string::npos)
        << "got: " << result.errorMessage;
}

TEST(EngineUncGate, RedactionVerifierRefusesUnc) {
    pdfe::redaction::RedactionVerifier verifier(makeCtx());
    auto result = verifier.verify(kUncPath, {});
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("UNC"), std::string::npos)
        << "got: " << result.errorMessage;
}

TEST(EngineUncGate, AlignmentInjectorApplyRefusesUnc) {
    pdfe::reflow::AlignmentInjector injector(makeCtx());
    EXPECT_THROW(
        injector.apply(kUncPath, kLocalOut, {}),
        std::runtime_error);
}

TEST(EngineUncGate, AlignmentInjectorRemoveRefusesUnc) {
    pdfe::reflow::AlignmentInjector injector(makeCtx());
    EXPECT_THROW(
        injector.remove(kUncPath, kLocalOut),
        std::runtime_error);
}

TEST(EngineUncGate, HeaderFooterApplyRefusesUnc) {
    pdfe::headerfooter::HeaderFooterEngine engine(makeCtx());
    pdfe::headerfooter::HeaderFooterConfig config;
    EXPECT_THROW(
        engine.apply(kUncPath, kLocalOut, config),
        std::runtime_error);
}

TEST(EngineUncGate, HeaderFooterRemoveRefusesUnc) {
    pdfe::headerfooter::HeaderFooterEngine engine(makeCtx());
    EXPECT_THROW(
        engine.remove(kUncPath, kLocalOut),
        std::runtime_error);
}

TEST(EngineUncGate, BatchEngineWatermarkRefusesUnc) {
    pdfe::batch::BatchEngine engine(makeCtx());
    pdfe::batch::WatermarkConfig config;
    config.text = "TEST";
    EXPECT_THROW(
        engine.applyWatermark(kUncPath, kLocalOut, config),
        std::runtime_error);
}

TEST(EngineUncGate, BatchEngineProcessRefusesUnc) {
    // BatchEngine::process calls processFile per input. A UNC entry
    // should be reported as failed via BatchFileResult, not crash.
    pdfe::batch::BatchEngine engine(makeCtx());
    auto result = engine.process({kUncPath}, {});
    ASSERT_EQ(result.fileResults.size(), 1u);
    const auto& fr = result.fileResults[0];
    EXPECT_FALSE(fr.errorMessage.empty());
    EXPECT_NE(fr.errorMessage.find("UNC"), std::string::npos)
        << "got: " << fr.errorMessage;
}

// ===========================================================================
// GitHub #93 (WIN-62) -- prove the engine refusal is not merely STRING-SHAPED.
//
// This file's own header records the gap: the tests above pass literal
// `\attacker.example\share\evil.pdf` strings, which are rejected on shape
// before anything resolves them. As that header puts it, "WIN-62's separate
// point -- that no engine test touches a real loopback share -- still stands."
//
// A shape-only test cannot distinguish a real gate from a substring match. So
// this one builds a UNC path that ACTUALLY RESOLVES -- the loopback
// administrative share `\\127.0.0.1\C$\...`, which aliases the local volume --
// points a real engine at it, and asserts the refusal still happens.
//
// The helper is duplicated from tests/test_path_roundtrips.cpp rather than
// shared. That is a deliberate, and slightly uncomfortable, choice: hoisting it
// into a shared header would touch a file every path test includes, and this
// phase's scope judgement (no widely-included header) is what keeps its
// evidence a targeted run. Recorded here so the duplication is visible rather
// than accidental -- if a third caller appears, hoist it.
// ===========================================================================
#ifdef _WIN32

namespace {

std::optional<std::filesystem::path>
loopbackAdminShareOfEngine(const std::filesystem::path& local) {
    const std::wstring w = local.wstring();
    if (w.size() < 3 || w[1] != L':') return std::nullopt;   // need "X:\..."
    // NOTE the FOUR backslashes: this must produce the two-backslash UNC
    // prefix `\\127.0.0.1\`. An earlier version of this line had two, giving a
    // single leading backslash -- which is not a UNC shape at all, so the
    // classifier correctly ignored it and the engine correctly reported "input
    // file does not exist". That read exactly like a product defect (a
    // resolvable share not being refused) and was an escaping error in the
    // test. Left recorded because the false finding was one commit away.
    std::wstring unc = L"\\\\127.0.0.1\\";
    unc += w[0];          // drive letter
    unc += L'$';          // admin share suffix
    unc += w.substr(2);   // "\a\b"
    return std::filesystem::path(unc);
}

}  // namespace

TEST(EngineUncGate, OptimizeEngineRefusesARealResolvableLoopbackShare) {
    namespace fs = std::filesystem;

    const fs::path fixture = fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    ASSERT_TRUE(fs::exists(fixture)) << "missing fixture test_single.pdf";

    // Stage a real file somewhere the admin share can reach.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "pdfe_unc_real";
    fs::create_directories(dir, ec);
    const fs::path local = dir / "via_share.pdf";
    fs::copy_file(fixture, local, fs::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(fs::exists(local)) << "could not stage the fixture: " << ec.message();

    const auto uncOpt = loopbackAdminShareOfEngine(local);
    ASSERT_TRUE(uncOpt.has_value())
        << "could not derive a loopback admin-share alias for " << local.string();
    const fs::path unc = *uncOpt;

    // Is the admin share actually enabled on this machine? If it is not, the
    // path does not resolve and this test would be asserting the shape case
    // again -- which the tests above already cover. Establish it explicitly
    // rather than assume, and say so if not.
    const bool shareResolves = fs::exists(unc, ec) && !ec;

    pdfe::optimize::OptimizeEngine engine(makeCtx());
    pdfe::optimize::OptimizeOptions opts;
    const fs::path out = dir / "via_share_out.pdf";
    fs::remove(out, ec);

    const auto r = engine.run(unc, out, opts);

    EXPECT_FALSE(r.success())
        << "OptimizeEngine accepted a UNC input. shareResolves="
        << shareResolves << ", path=" << unc.string();
    EXPECT_NE(r.errorMessage.find("UNC"), std::string::npos)
        << "refused, but not with the UNC refusal -- the rejection may be "
           "incidental (a failed open) rather than the gate. Message: "
        << r.errorMessage;
    EXPECT_FALSE(fs::exists(out))
        << "the engine wrote an output despite refusing the input";

    if (shareResolves) {
        // The strong form: the alias really did point at the staged bytes, so
        // the refusal happened on a path the OS would have opened.
        SUCCEED() << "loopback admin share resolves on this machine; the "
                     "refusal was of a genuinely reachable path";
    } else {
        // Honest downgrade, recorded rather than silently passing as if the
        // strong form had held.
        GTEST_SKIP() << "the loopback administrative share (C$) is disabled on "
                        "this machine, so the UNC path does not resolve and "
                        "this run only re-covers the shape case. The refusal "
                        "assertions above still ran and passed.";
    }
}

#endif  // _WIN32
