// -*- coding: utf-8 -*-
// Meta-audit 2026-07 follow-up, phase F26 — item 2.
//
// WHAT WAS WRONG
// --------------
// F23 (gap PDF-39) built a controlled-anchor seam — CertChainValidator taking
// an explicit TrustAnchorSet — and rewired the product's chain checks through
// it: LtvValidator.cpp:457/467 and Pkcs7OpenSslBridge.cpp:293/297
// (openssl_check_certificate). phase-23-dispositions.md reads as though both of
// Pkcs7OpenSslBridge's trust sites were done. Only one was.
//
// Pkcs7OpenSslBridge::verifyWithChain kept its own inline X509_STORE, seeded
// with X509_STORE_set_default_paths() and nothing else, and set chainTrusted
// from it. The user's explicit trust anchors had no bearing on that verdict at
// all. Nothing consulted it — LtvDssWriter.cpp reads only certChain — so it was
// inert TODAY; the defect was that the next caller to honour chainTrusted would
// inherit an uncontrolled trust decision that bypasses the anchor set entirely.
//
// F26 routes it through CertChainValidator with the anchor set as a call
// parameter (VerifyWithChainOptions).
//
// WHY ROUTING ALONE PROVES NOTHING
// --------------------------------
// A test that only checks "verifyWithChain now calls CertChainValidator" is
// satisfied by a call whose result is discarded, or by one that passes an empty
// anchor set and always verdicts untrusted. So this file asserts the VERDICT is
// DRIVEN BY the anchor set, using the same F23 corpus
// (tests/fixtures/cert_chains/) and the same pinned instant: leafA under interA
// verdicts trusted against rootA and NOT trusted against rootB, with the anchor
// set as the only difference between the two arms.
//
// THE PKCS#7 CONTAINER
// --------------------
// The corpus deliberately commits no private keys (gen_cert_chain_corpus.py
// writes them to TEMP only), so leafA cannot sign anything here. It does not
// need to: verifyWithChain's TRUST path only needs a parseable SignedData whose
// signer-info resolves to leafA. buildSignedDataAround() assembles exactly that
// — real certificates, a real signer-info identifying the leaf by issuer and
// serial, and a placeholder signature value. digestValid is not asserted on
// (no content is supplied and no key exists to have signed one); the trust
// verdict is what this file is about, and it is computed from the certificates.
//
// See docs/audit/meta-audit-2026-07/followup/phase-26-dispositions.md §2.

#include <gtest/gtest.h>

#include "signatures/Pkcs7OpenSslBridge.h"
#include "signatures/TrustAnchorSet.h"
#include "signatures/TrustAnchorManager.h"

#include <openssl/asn1.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pdfe::signatures;

namespace {

// Must match VERIFICATION_INSTANT in tests/fixtures/gen_cert_chain_corpus.py
// and kVerificationTimeUnix in tests/test_cert_chain_trust.cpp:
// 2030-06-15T00:00:00Z.
constexpr std::int64_t kVerificationTimeUnix = 1907712000;

std::vector<uint8_t> readDer(const char* name)
{
    const auto path =
        std::filesystem::path(TEST_FIXTURES_DIR) / "cert_chains" / name;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

X509* parse(const std::vector<uint8_t>& der)
{
    if (der.empty()) return nullptr;
    const unsigned char* p = der.data();
    return d2i_X509(nullptr, &p, static_cast<long>(der.size()));
}

/// Build a DER PKCS#7 SignedData carrying @p leafDer plus @p extraDer, whose
/// single signer-info identifies the leaf by issuer + serial.
///
/// No private key is involved: enc_digest is a placeholder. This is a
/// CERTIFICATE container, which is all the trust path reads.
std::vector<uint8_t> buildSignedDataAround(
    const std::vector<uint8_t>& leafDer,
    const std::vector<std::vector<uint8_t>>& extraDer)
{
    std::vector<uint8_t> out;

    X509* leaf = parse(leafDer);
    if (!leaf) return out;

    PKCS7* p7 = PKCS7_new();
    if (!p7) { X509_free(leaf); return out; }

    if (PKCS7_set_type(p7, NID_pkcs7_signed) != 1
        || PKCS7_content_new(p7, NID_pkcs7_data) != 1) {
        PKCS7_free(p7);
        X509_free(leaf);
        return out;
    }

    PKCS7_add_certificate(p7, leaf);  // takes a reference
    for (const auto& d : extraDer) {
        X509* c = parse(d);
        if (!c) continue;
        PKCS7_add_certificate(p7, c);
        X509_free(c);
    }

    PKCS7_SIGNER_INFO* si = PKCS7_SIGNER_INFO_new();
    if (!si) { PKCS7_free(p7); X509_free(leaf); return out; }

    ASN1_INTEGER_set(si->version, 1);
    X509_NAME_set(&si->issuer_and_serial->issuer, X509_get_issuer_name(leaf));
    ASN1_INTEGER_free(si->issuer_and_serial->serial);
    si->issuer_and_serial->serial =
        ASN1_INTEGER_dup(X509_get_serialNumber(leaf));
    X509_ALGOR_set0(si->digest_alg, OBJ_nid2obj(NID_sha256), V_ASN1_NULL,
                    nullptr);
    X509_ALGOR_set0(si->digest_enc_alg, OBJ_nid2obj(NID_rsaEncryption),
                    V_ASN1_NULL, nullptr);
    static const unsigned char kPlaceholderSignature[8] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF
    };
    ASN1_STRING_set(si->enc_digest, kPlaceholderSignature,
                    static_cast<int>(sizeof(kPlaceholderSignature)));

    if (PKCS7_add_signer(p7, si) != 1) {
        PKCS7_SIGNER_INFO_free(si);
        PKCS7_free(p7);
        X509_free(leaf);
        return out;
    }

    unsigned char* der = nullptr;
    const int len = i2d_PKCS7(p7, &der);
    if (len > 0 && der) {
        out.assign(der, der + len);
    }
    if (der) OPENSSL_free(der);

    PKCS7_free(p7);
    X509_free(leaf);
    return out;
}

class VerifyWithChainTrustTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        rootA_  = readDer("rootA.der");
        interA_ = readDer("interA.der");
        leafA_  = readDer("leafA.der");
        rootB_  = readDer("rootB.der");

        ASSERT_FALSE(rootA_.empty())
            << "missing tests/fixtures/cert_chains/rootA.der — regenerate "
               "with: python tests/fixtures/gen_cert_chain_corpus.py";
        ASSERT_FALSE(interA_.empty()) << "missing interA.der";
        ASSERT_FALSE(leafA_.empty())  << "missing leafA.der";
        ASSERT_FALSE(rootB_.empty())  << "missing rootB.der";

        pkcs7_ = buildSignedDataAround(leafA_, { interA_ });
        ASSERT_FALSE(pkcs7_.empty())
            << "failed to assemble the PKCS#7 container; every assertion "
               "below would otherwise be about an empty input";

        persistedAnchorsAtStart_ =
            TrustAnchorManager::instance().listTrusts().size();
    }

    void TearDown() override
    {
        // Same invariant test_cert_chain_trust.cpp holds: getting a controlled
        // verdict must never require mutating the user's persisted trust list.
        EXPECT_EQ(TrustAnchorManager::instance().listTrusts().size(),
                  persistedAnchorsAtStart_)
            << "a test in this file mutated the persisted trust list; the "
               "anchor set is supposed to be a parameter of the call.";
    }

    static TrustAnchorSet anchorsOf(const std::vector<std::vector<uint8_t>>& ders)
    {
        TrustAnchorSet set;
        for (const auto& d : ders) {
            EXPECT_TRUE(set.addDer(d)) << "anchor rejected by TrustAnchorSet";
        }
        EXPECT_EQ(set.size(), static_cast<int>(ders.size()));
        return set;
    }

    /// The base options every arm starts from: hermetic (OpenSSL default
    /// cert paths OFF; the Windows store is never read either) and
    /// calendar-independent (instant pinned). Arms then set `anchors` and
    /// nothing else, which is what makes the pairing a control.
    static VerifyWithChainOptions hermeticOptions(const TrustAnchorSet& anchors)
    {
        VerifyWithChainOptions opts;
        opts.anchors = &anchors;
        opts.useOpenSslDefaultPaths = false;
        opts.verificationTimeUnix = kVerificationTimeUnix;
        return opts;
    }

    std::vector<uint8_t> rootA_, interA_, leafA_, rootB_;
    std::vector<uint8_t> pkcs7_;
    qsizetype persistedAnchorsAtStart_ = 0;
};

// ===========================================================================
// AC #1 — the verdict is TRUSTED when the chain reaches the supplied anchor.
// ===========================================================================
TEST_F(VerifyWithChainTrustTest, TrustedAnchorYieldsTrustedVerdict)
{
    const TrustAnchorSet anchors = anchorsOf({ rootA_ });
    const auto result = Pkcs7OpenSslBridge::verifyWithChain(
        pkcs7_.data(), pkcs7_.size(), nullptr, 0, hermeticOptions(anchors));

    // The container was understood at all — otherwise "untrusted" below would
    // be about a parse failure rather than about trust.
    ASSERT_FALSE(result.certChain.empty())
        << "no certificates extracted: " << result.statusMessage;

    EXPECT_TRUE(result.chainTrusted)
        << "leafA under interA must verdict TRUSTED against rootA: "
        << result.statusMessage;
    EXPECT_EQ(result.chainOpensslError, X509_V_OK);
    EXPECT_EQ(result.statusMessage, "Trusted");
}

// ===========================================================================
// AC #2 — THE DISCRIMINATING CONTROL.
//
// Byte-identical call except for the anchor set. Same PKCS#7, same instant,
// same (disabled) OpenSSL default paths. So the verdict difference is
// attributable to the
// anchors and to nothing else — and a rewiring that ignored the anchor set
// (e.g. left the default paths on, or kept the old inline X509_STORE) would
// fail one of these two arms.
// ===========================================================================
TEST_F(VerifyWithChainTrustTest, UntrustedAnchorYieldsUntrustedVerdict)
{
    {
        const TrustAnchorSet trusted = anchorsOf({ rootA_ });
        const auto control = Pkcs7OpenSslBridge::verifyWithChain(
            pkcs7_.data(), pkcs7_.size(), nullptr, 0,
            hermeticOptions(trusted));
        ASSERT_TRUE(control.chainTrusted)
            << "control arm did not verdict trusted, so the negative arm "
               "proves nothing: " << control.statusMessage;
    }

    const TrustAnchorSet anchors = anchorsOf({ rootB_ });
    const auto result = Pkcs7OpenSslBridge::verifyWithChain(
        pkcs7_.data(), pkcs7_.size(), nullptr, 0, hermeticOptions(anchors));

    ASSERT_FALSE(result.certChain.empty())
        << "no certificates extracted: " << result.statusMessage;
    EXPECT_FALSE(result.chainTrusted)
        << "rootB does not issue interA; this must NOT verdict trusted";
    // The SPECIFIC reason: the path ran out of issuers. Asserting the code
    // rules out "failed for some other reason that also happens to be a
    // failure" — the same oracle strength test_cert_chain_trust.cpp uses.
    EXPECT_TRUE(result.chainOpensslError == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT
                || result.chainOpensslError
                       == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY)
        << "unexpected X509_V_ERR " << result.chainOpensslError << ": "
        << result.statusMessage;
}

// ===========================================================================
// SECURITY INVARIANT — a certificate carried INSIDE the container is
// path-building material, never a source of trust.
//
// interA is embedded in the PKCS#7 alongside the leaf. If verifyWithChain fed
// the container's certificates into the trust store rather than into
// untrustedDer, an attacker-supplied chain would certify itself. With an EMPTY
// anchor set and no ambient trust source there is no legitimate anchor
// anywhere, so
// the only way this can verdict trusted is that bug.
// ===========================================================================
TEST_F(VerifyWithChainTrustTest, EmbeddedCertificatesCannotAnchorThemselves)
{
    const TrustAnchorSet none;
    ASSERT_TRUE(none.isEmpty());

    const auto result = Pkcs7OpenSslBridge::verifyWithChain(
        pkcs7_.data(), pkcs7_.size(), nullptr, 0, hermeticOptions(none));

    ASSERT_FALSE(result.certChain.empty())
        << "no certificates extracted: " << result.statusMessage;
    EXPECT_FALSE(result.chainTrusted)
        << "the container's own certificates must not be able to terminate "
           "the chain: " << result.statusMessage;
    EXPECT_NE(result.chainOpensslError, X509_V_OK);
}

// ===========================================================================
// The inline store is GONE, not merely bypassed.
//
// verifyWithChain used to call X509_STORE_set_default_paths() unconditionally.
// With useOpenSslDefaultPaths left at its default (true) the production path
// still calls it — deliberate and unchanged — but it must do so THROUGH the
// controlled request, so an explicitly supplied anchor still counts.
//
// GitHub #212 — SAY WHAT THAT FLAG ACTUALLY DOES. This comment used to read
// "the production path still consults the machine store", and the test was
// named ...WithSystemStoreOn. Both were false, and in exactly the way #212 was
// filed about: X509_STORE_set_default_paths() loads OPENSSLDIR/certs plus
// SSL_CERT_FILE / SSL_CERT_DIR — POSIX conventions that never touch the
// Windows certificate store, and which on this vcpkg build resolve to a
// directory that does not exist, so they contribute ZERO anchors.
//
// The field was renamed from useSystemStore three lines below this block in
// the same commit; the prose around it was not. A false comment sitting beside
// a corrected name is worse than one on its own, because the rename makes the
// file look already audited. Caught by the phase-8 claim-check gate.
//
// What the test actually establishes is unchanged and still worth having:
// rootA is in no default path on any platform, so a trusted verdict here can
// only come from the supplied anchor set being honoured while the default-path
// flag is on.
// ===========================================================================
TEST_F(VerifyWithChainTrustTest, SuppliedAnchorIsHonouredWithDefaultPathsOn)
{
    const TrustAnchorSet anchors = anchorsOf({ rootA_ });
    VerifyWithChainOptions opts;
    opts.anchors = &anchors;
    opts.useOpenSslDefaultPaths = true;   // production default
    opts.verificationTimeUnix = kVerificationTimeUnix;

    const auto result = Pkcs7OpenSslBridge::verifyWithChain(
        pkcs7_.data(), pkcs7_.size(), nullptr, 0, opts);

    EXPECT_TRUE(result.chainTrusted)
        << "an explicit anchor must still be honoured when OpenSSL's default "
           "certificate paths are also consulted: " << result.statusMessage;
    EXPECT_EQ(result.chainOpensslError, X509_V_OK);
}

// ===========================================================================
// Malformed input reports MALFORMED, not "untrusted".
//
// Conflating the two is exactly the unfalsifiable-oracle shape F23 removed: a
// typo'd fixture and a genuine trust rejection would otherwise be the same
// observation.
// ===========================================================================
TEST_F(VerifyWithChainTrustTest, GarbageInputIsNotReportedAsATrustDecision)
{
    std::vector<uint8_t> garbage(256, 0x41);
    const TrustAnchorSet anchors = anchorsOf({ rootA_ });
    const auto result = Pkcs7OpenSslBridge::verifyWithChain(
        garbage.data(), garbage.size(), nullptr, 0, hermeticOptions(anchors));

    EXPECT_FALSE(result.chainTrusted);
    EXPECT_TRUE(result.certChain.empty());
    EXPECT_EQ(result.statusMessage, "Failed to parse PKCS#7")
        << "an unparseable container must be reported as such, never as an "
           "untrusted chain";
}

} // namespace
