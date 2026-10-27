// -*- coding: utf-8 -*-
// Phase-8 (R2-15/16/17) smoke test for the write/mutation fuzz harnesses.
//
// This compiles into the NORMAL GoogleTest build — there is no libFuzzer /
// AFL++ / sanitizer toolchain here. It drives each NEW harness's per-input
// core (factored into inline pdfe::fuzz::run_* functions in
// security/fuzz/fuzz_targets.h) over the committed seed corpus plus a few
// crafted inputs, asserting the harness logic is sound (returns cleanly, no
// crash / UAF observable without a sanitizer) and that:
//
//   * R2-16 — the encryption harness now feeds a crypt-SHAPED input that
//     reaches MuPDF's /Standard security handler (structural regression lock),
//     rather than the old raw-bytes-as-whole-dict input that never did.
//   * R2-17 — the benign-throw triage classifier routes a simulated memory
//     fault (and std::bad_alloc, and unknown exception types) to MustCrash,
//     while genuine MuPDF/PDFE parse refusals stay BenignExpected.
//
// RED against the pre-remediation tree: neither the run_* cores, the
// build_encrypted_pdf builder, nor the classify_caught / SimulatedMemoryFault
// triage symbols existed — so this TU did not compile. GREEN once they do.
#include <gtest/gtest.h>

#include "security/fuzz/fuzz_targets.h"   // inline cores + build_encrypted_pdf
#include "core/Error.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <new>
#include <stdexcept>

namespace {

namespace fz = pdfe::fuzz;
namespace stdfs = std::filesystem;

// Root of the committed fuzz seed corpora, derived from TEST_FIXTURES_DIR
// (= tests/fixtures) so no extra CMake wiring is required.
stdfs::path seedsRoot()
{
    return stdfs::path(TEST_FIXTURES_DIR).parent_path() / "security" / "fuzz" / "seeds";
}

std::vector<std::uint8_t> readFile(const stdfs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return bytes;
}

// All committed seeds for one harness subdir.
std::vector<std::vector<std::uint8_t>> loadSeeds(const std::string& harness)
{
    std::vector<std::vector<std::uint8_t>> out;
    stdfs::path dir = seedsRoot() / harness;
    std::error_code ec;
    if (stdfs::exists(dir, ec)) {
        for (const auto& e : stdfs::directory_iterator(dir, ec)) {
            if (e.is_regular_file()) out.push_back(readFile(e.path()));
        }
    }
    return out;
}

// A handful of adversarial crafted inputs every core must survive.
std::vector<std::vector<std::uint8_t>> craftedInputs()
{
    std::vector<std::vector<std::uint8_t>> v;
    v.push_back({});                                             // empty
    v.push_back({0x00});                                        // single NUL
    v.push_back({'%','P','D','F','-','1','.','7','\n'});        // header only
    // truncated xref-ish garbage
    {
        std::string s = "%PDF-1.7\n1 0 obj<< /Type /Catalog >>endobj\ntrailer<<";
        v.emplace_back(s.begin(), s.end());
    }
    // deterministic pseudo-random block
    {
        std::vector<std::uint8_t> b(512);
        std::uint32_t x = 0x9E3779B9u;
        for (auto& c : b) { x = x * 1664525u + 1013904223u; c = std::uint8_t(x >> 24); }
        v.push_back(std::move(b));
    }
    return v;
}

// Drive a core over seeds + crafted inputs; assert it returns without an
// escaping exception (a MustCrash rethrow would surface here as a failure).
template <class Fn>
void driveCore(Fn core, const std::string& harness)
{
    for (auto& s : loadSeeds(harness)) {
        ASSERT_NO_THROW(core(s.data(), s.size())) << "seed input, harness=" << harness;
    }
    for (auto& s : craftedInputs()) {
        ASSERT_NO_THROW(core(s.data(), s.size())) << "crafted input, harness=" << harness;
    }
}

// ---------------------------------------------------------------
// R2-17: benign-throw triage
// ---------------------------------------------------------------

TEST(FuzzTriage, SimulatedMemoryFaultIsNotBenign)
{
    fz::SimulatedMemoryFault mf;
    EXPECT_EQ(fz::classify_caught(mf), fz::ThrowClass::MustCrash);
}

TEST(FuzzTriage, BadAllocIsNotBenign)
{
    std::bad_alloc ba;
    EXPECT_EQ(fz::classify_caught(ba), fz::ThrowClass::MustCrash);
}

TEST(FuzzTriage, UnknownLogicErrorIsNotBenign)
{
    std::logic_error le("bad invariant");
    EXPECT_EQ(fz::classify_caught(le), fz::ThrowClass::MustCrash);
}

TEST(FuzzTriage, MuPdfParseRefusalIsBenign)
{
    pdfe::core::MuPdfException me("cannot parse xref");
    EXPECT_EQ(fz::classify_caught(me), fz::ThrowClass::BenignExpected);
}

TEST(FuzzTriage, RuntimeErrorFromParserIsBenign)
{
    std::runtime_error re("unexpected EOF");
    EXPECT_EQ(fz::classify_caught(re), fz::ThrowClass::BenignExpected);
}

// triage_or_rethrow must rethrow a MustCrash and swallow a benign one.
TEST(FuzzTriage, TriageRethrowsMemoryFaultSwallowsParseError)
{
    bool rethrew = false;
    try {
        try { throw fz::SimulatedMemoryFault{}; }
        catch (const std::exception& e) { fz::triage_or_rethrow(e); }
    } catch (const fz::SimulatedMemoryFault&) { rethrew = true; }
    EXPECT_TRUE(rethrew) << "a simulated memory fault must NOT be swallowed";

    bool swallowed = true;
    try {
        try { throw pdfe::core::MuPdfException("refuse"); }
        catch (const std::exception& e) { fz::triage_or_rethrow(e); }
    } catch (...) { swallowed = false; }
    EXPECT_TRUE(swallowed) << "an expected parse refusal must be swallowed";
}

// ---------------------------------------------------------------
// R2-16: encryption harness reaches the crypt parser
// ---------------------------------------------------------------

TEST(FuzzEncryption, BuilderEmitsCryptShapedInput)
{
    // Cover all four crypt shapes the selector byte chooses (RC4-40/128,
    // AESV2, AESV3) and assert every one is a real /Standard-handler input.
    for (int sel = 0; sel < 4; ++sel) {
        std::vector<std::uint8_t> in(256);
        in[0] = std::uint8_t(sel);
        for (std::size_t i = 1; i < in.size(); ++i) in[i] = std::uint8_t(i * 7 + sel);

        std::string pdf = fz::build_encrypted_pdf(in.data(), in.size());

        // Structural regression lock: the OLD harness produced
        // `2 0 obj << <raw bytes> >>` with none of these markers, so MuPDF
        // never constructed the standard security handler. The new input
        // must carry a well-formed /Filter /Standard dict + /ID trailer.
        EXPECT_NE(pdf.find("/Filter /Standard"), std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/Encrypt 2 0 R"),    std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/ID [<"),            std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/O <"),              std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/U <"),              std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/V "),               std::string::npos) << "sel=" << sel;
        EXPECT_NE(pdf.find("/R "),               std::string::npos) << "sel=" << sel;
    }

    // AESV3 (sel==3) must additionally carry the V5-only crypt material and
    // a crypt-filter block, without which MuPDF's AESV3 path is unreachable.
    std::vector<std::uint8_t> aes(256, 0xAB);
    aes[0] = 3;
    std::string p5 = fz::build_encrypted_pdf(aes.data(), aes.size());
    EXPECT_NE(p5.find("/CF <<"),   std::string::npos);
    EXPECT_NE(p5.find("/AESV3"),   std::string::npos);
    EXPECT_NE(p5.find("/OE <"),    std::string::npos);
    EXPECT_NE(p5.find("/UE <"),    std::string::npos);
    EXPECT_NE(p5.find("/Perms <"), std::string::npos);
}

TEST(FuzzEncryption, DictCoreNoCrash)
{
    driveCore(&fz::run_encryption_dict, "fuzz_encryption_dict");
}

// ---------------------------------------------------------------
// R2-15: write/mutation-surface harness cores are sound
// ---------------------------------------------------------------

TEST(FuzzWriteSurfaces, ContentStreamEditCoreNoCrash)
{
    driveCore(&fz::run_content_stream_edit, "fuzz_content_stream_edit");
}

TEST(FuzzWriteSurfaces, SaveRoundtripCoreNoCrash)
{
    driveCore(&fz::run_save_roundtrip, "fuzz_save_roundtrip");
}

TEST(FuzzWriteSurfaces, MujsExecCoreNoCrash)
{
    driveCore(&fz::run_mujs_exec, "fuzz_mujs_exec");
}

TEST(FuzzWriteSurfaces, RedactionApplyCoreNoCrash)
{
    driveCore(&fz::run_redaction_apply, "fuzz_redaction_apply");
}

} // namespace
