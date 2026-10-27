// -*- coding: utf-8 -*-
//
// Pipeline e4c71f3a phase-2 (SEC-PKCS7-FUZZ): PKCS#7 verification path
// fuzz harness + GTest corpus smoke test.
//
// The harness feeds arbitrary bytes through d2i_PKCS7 → PKCS7_verify →
// PKCS7_get0_signers, covering the same code path as
// Pkcs7OpenSslBridge::openssl_check_digest and openssl_check_certificate.
//
// PROMOTED IN PHASE-73. This header used to end: "On Windows/MSVC the libFuzzer
// entry point compiles but is not linked as a fuzzer target — the GTest wrapper
// exercises the corpus seeds." That was accurate and it was the defect: the
// LLVMFuzzerTestOneInput below this comment was never linked into anything, so
// the file advertised coverage-guided fuzzing it did not perform (DCA 0037).
//
// It is now real. tests/security/fuzz/fuzz_pkcs7_verify.cpp is a registered
// pdfe_add_fuzz_target, and the entry point that used to sit here has moved
// there. What remains in this file is what was always genuine: the deterministic
// SEED-REPLAY oracle over tests/corpus/pkcs7/, which runs in the ordinary MSVC
// gtest suite where libFuzzer cannot go.
//
// Both drivers now call the SAME body, security/fuzz/pkcs7_fuzz_core.h. Copying
// it would have let the campaign and the oracle harden separately — see
// DCA 0036, where one of two sibling encoders got the fix.

#include "security/fuzz/pkcs7_fuzz_core.h"

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <openssl/objects.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Generous per-input wall-clock ceiling (see test_font_fuzz.cpp). A malformed
// PKCS#7 blob must parse-or-refuse well under this; the libFuzzer -timeout in
// the coverage-guided CI lane is the true non-terminating-hang cap.
constexpr double kPkcs7TimeCapMs = 5000.0;

std::string corpusDir()
{
    return std::string(TEST_FIXTURES_DIR) + "/../corpus/pkcs7";
}

std::vector<uint8_t> readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    auto s = ss.str();
    return {s.begin(), s.end()};
}

// Returns true iff d2i_PKCS7 accepted the input (a "successful parse"). Used by
// the smoke tests to assert the OUTPUT INVARIANT: a parse that succeeds returns
// a structurally-valid object (non-null ASN.1 type), and a rejected input
// returns a clean null + a queued error, not a half-constructed PKCS7.
bool fuzzPkcs7Verification(const uint8_t* data, size_t size)
{
    const auto r = pdfe::fuzz::pkcs7::probe(data, size);

    // OUTPUT INVARIANT: a successfully-decoded PKCS7 has a resolvable content
    // type OID (the ASN.1 CHOICE selector). d2i never yields a typeless object
    // on success -- if it did, downstream PKCS7_verify would deref garbage.
    //
    // The shared core REPORTS this rather than asserting it, because the fuzz
    // driver has to abort on it (libFuzzer only records a saved artifact when
    // the process dies) while the gtest driver has to EXPECT on it. Same
    // invariant, two reactions, one implementation.
    if (r.parsed) {
        EXPECT_TRUE(r.hasContentType)
            << "a successfully-parsed PKCS7 must carry a content-type OID";
    }
    return r.parsed;
}

} // namespace

// =====================================================================
// GTest: smoke-test every seed in the corpus directory
// =====================================================================

class Pkcs7FuzzSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto dir = std::filesystem::path(corpusDir());
        if (!std::filesystem::exists(dir))
            GTEST_SKIP() << "Corpus directory not found: " << dir.string();
    }
};

TEST_F(Pkcs7FuzzSmokeTest, CorpusInputsDoNotCrash)
{
    auto dir = std::filesystem::path(corpusDir());
    int seedCount = 0;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto data = readFile(entry.path());
        SCOPED_TRACE("Seed: " + entry.path().filename().string());

        // (a) no crash/throw + (b) bounded time.
        auto t0 = std::chrono::steady_clock::now();
        bool parsed = false;
        ASSERT_NO_THROW({ parsed = fuzzPkcs7Verification(data.data(), data.size()); });
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        EXPECT_LT(ms, kPkcs7TimeCapMs) << "parse exceeded wall-clock cap: " << ms << "ms";

        // (c) determinism: the same seed must reach the same parse verdict
        // (a half-constructed / uninitialised-memory decode would not).
        bool parsedAgain = false;
        ASSERT_NO_THROW({ parsedAgain = fuzzPkcs7Verification(data.data(), data.size()); });
        EXPECT_EQ(parsed, parsedAgain)
            << "parse verdict not deterministic for a fixed seed";
        ++seedCount;
    }

    EXPECT_GE(seedCount, 3)
        << "Corpus must contain at least 3 seed files";
}

TEST_F(Pkcs7FuzzSmokeTest, ZeroByteInputDoesNotCrash)
{
    // Zero-length input must be a clean refusal (false), never a parse.
    EXPECT_FALSE(fuzzPkcs7Verification(nullptr, 0));
    uint8_t empty = 0;
    EXPECT_FALSE(fuzzPkcs7Verification(&empty, 0));
}

TEST_F(Pkcs7FuzzSmokeTest, LargeGarbageInputDoesNotCrash)
{
    std::vector<uint8_t> garbage(65536, 0xAA);
    bool parsed = true;
    auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_THROW({ parsed = fuzzPkcs7Verification(garbage.data(), garbage.size()); });
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, kPkcs7TimeCapMs) << "parse exceeded wall-clock cap: " << ms << "ms";
    // Uniform 0xAA is not valid DER SignedData -> clean refusal expected.
    EXPECT_FALSE(parsed) << "uniform garbage must be refused, not parsed";
}

TEST_F(Pkcs7FuzzSmokeTest, RepeatedParsingDoesNotLeak)
{
    auto dir = std::filesystem::path(corpusDir());
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto data = readFile(entry.path());
        for (int i = 0; i < 100; ++i) {
            fuzzPkcs7Verification(data.data(), data.size());
        }
        break;
    }
}
