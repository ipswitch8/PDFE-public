// -*- coding: utf-8 -*-
//
// windows-fixes phase-5 (SEC-TSA): offline RFC 3161 TimeStampToken verification.
//
// The signing path embeds an RFC 3161 TimeStampToken (id-smime-aa-timeStampToken)
// whose messageImprint is the SHA-256 of the signature value. Until now nothing
// verified that token on the read side. TsaClient::verifyTimestampToken closes
// that gap; these tests HARD-ASSERT the RFC 3161 binding fully offline — they
// build a genuine token with a local TSA (no network) and check:
//   * a token whose imprint matches the timestamped data verifies;
//   * a token whose imprint does NOT match (a forged/mismatched timestamp that
//     attests to different bytes) is rejected on the imprint binding;
//   * a token not signed by the expected TSA fails signature verification;
//   * malformed bytes are not mistaken for a token.

#include <gtest/gtest.h>

#include "signatures/TsaClient.h"
#include "signatures/LtvValidator.h"

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace pdfe::signatures;

namespace {

// Fixed serial callback so TS_RESP_CTX can mint a response without external
// serial-number state.
ASN1_INTEGER* fixedSerialCb(TS_RESP_CTX* /*ctx*/, void* /*data*/)
{
    ASN1_INTEGER* serial = ASN1_INTEGER_new();
    if (serial) ASN1_INTEGER_set(serial, 1);
    return serial;
}

// Build a self-signed TSA certificate bearing the critical id-kp-timeStamping
// extended key usage (RFC 3161 §2.3 requires it for a TSA signer).
bool makeTsaSigner(EVP_PKEY*& outKey, X509*& outCert, const char* cn)
{
    outKey = EVP_EC_gen("P-256");
    if (!outKey) return false;
    outCert = X509_new();
    if (!outCert) { EVP_PKEY_free(outKey); outKey = nullptr; return false; }

    X509_set_version(outCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(outCert), 7);
    X509_gmtime_adj(X509_getm_notBefore(outCert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(outCert), 60 * 60 * 24);
    X509_set_pubkey(outCert, outKey);

    X509_NAME* nm = X509_get_subject_name(outCert);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(outCert, nm);  // self-signed

    // extendedKeyUsage = critical, timeStamping — TS_RESP_CTX refuses to sign
    // without it, and OpenSSL's token verifier requires it too.
    X509_EXTENSION* eku = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_ext_key_usage, "critical,timeStamping");
    if (eku) {
        X509_add_ext(outCert, eku, -1);
        X509_EXTENSION_free(eku);
    }
    X509_EXTENSION* bc = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_basic_constraints, "critical,CA:FALSE");
    if (bc) {
        X509_add_ext(outCert, bc, -1);
        X509_EXTENSION_free(bc);
    }

    if (X509_sign(outCert, outKey, EVP_sha256()) == 0) {
        X509_free(outCert); outCert = nullptr;
        EVP_PKEY_free(outKey); outKey = nullptr;
        return false;
    }
    return true;
}

// Build a CA certificate and a TSA leaf ISSUED BY IT — i.e. the shape every
// real-world public TSA has, and the shape the self-signed makeTsaSigner()
// above structurally cannot produce. Added for the phase-48 review's SEC-4
// finding: TsaClient::verifyTimestampToken built its X509_STORE without
// X509_V_FLAG_PARTIAL_CHAIN, so the path could only ever terminate at a
// self-signed certificate, and the entire existing corpus was self-signed.
bool makeIssuedTsaSigner(EVP_PKEY*& caKey, X509*& caCert,
                         EVP_PKEY*& outKey, X509*& outCert,
                         const char* caCn, const char* leafCn)
{
    caKey = nullptr; caCert = nullptr; outKey = nullptr; outCert = nullptr;

    auto fail = [&]() {
        if (outCert) { X509_free(outCert); outCert = nullptr; }
        if (outKey) { EVP_PKEY_free(outKey); outKey = nullptr; }
        if (caCert) { X509_free(caCert); caCert = nullptr; }
        if (caKey) { EVP_PKEY_free(caKey); caKey = nullptr; }
        ERR_clear_error();
        return false;
    };

    // --- the CA ---
    caKey = EVP_EC_gen("P-256");
    if (!caKey) return fail();
    caCert = X509_new();
    if (!caCert) return fail();
    X509_set_version(caCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(caCert), 11);
    X509_gmtime_adj(X509_getm_notBefore(caCert), -7200);
    X509_gmtime_adj(X509_getm_notAfter(caCert), 60 * 60 * 24);
    X509_set_pubkey(caCert, caKey);
    X509_NAME* caNm = X509_get_subject_name(caCert);
    X509_NAME_add_entry_by_txt(caNm, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(caCn), -1, -1, 0);
    X509_set_issuer_name(caCert, caNm);  // self-signed root of this little PKI
    if (X509_EXTENSION* bc = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE")) {
        X509_add_ext(caCert, bc, -1);
        X509_EXTENSION_free(bc);
    }
    if (X509_EXTENSION* ku = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_key_usage, "critical,keyCertSign,cRLSign")) {
        X509_add_ext(caCert, ku, -1);
        X509_EXTENSION_free(ku);
    }
    if (X509_sign(caCert, caKey, EVP_sha256()) == 0) return fail();

    // --- the TSA leaf, issued by that CA ---
    outKey = EVP_EC_gen("P-256");
    if (!outKey) return fail();
    outCert = X509_new();
    if (!outCert) return fail();
    X509_set_version(outCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(outCert), 12);
    X509_gmtime_adj(X509_getm_notBefore(outCert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(outCert), 60 * 60 * 24);
    X509_set_pubkey(outCert, outKey);
    X509_NAME* nm = X509_get_subject_name(outCert);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(leafCn), -1, -1, 0);
    // NOT self-signed: the issuer is the CA above. This is the whole point.
    X509_set_issuer_name(outCert, X509_get_subject_name(caCert));
    if (X509_EXTENSION* eku = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_ext_key_usage, "critical,timeStamping")) {
        X509_add_ext(outCert, eku, -1);
        X509_EXTENSION_free(eku);
    }
    if (X509_EXTENSION* bc = X509V3_EXT_conf_nid(
            nullptr, nullptr, NID_basic_constraints, "critical,CA:FALSE")) {
        X509_add_ext(outCert, bc, -1);
        X509_EXTENSION_free(bc);
    }
    if (X509_sign(outCert, caKey, EVP_sha256()) == 0) return fail();
    return true;
}

// Mint a genuine RFC 3161 TimeStampToken over @p imprint (a SHA-256 digest)
// using a local TSA — entirely offline. Returns the DER-encoded token.
std::vector<uint8_t> mintTokenForImprint(EVP_PKEY* tsaKey, X509* tsaCert,
                                          const unsigned char* imprint,
                                          size_t imprintLen)
{
    std::vector<uint8_t> tokenDer;

    TS_RESP_CTX* ctx = TS_RESP_CTX_new();
    if (!ctx) return tokenDer;

    ASN1_OBJECT* policy = OBJ_txt2obj("1.2.3.4.1", 1);

    bool ok = policy != nullptr
        && TS_RESP_CTX_set_signer_cert(ctx, tsaCert) == 1
        && TS_RESP_CTX_set_signer_key(ctx, tsaKey) == 1
        && TS_RESP_CTX_set_def_policy(ctx, policy) == 1
        && TS_RESP_CTX_add_md(ctx, EVP_sha256()) == 1;
    if (ok) {
        TS_RESP_CTX_set_serial_cb(ctx, fixedSerialCb, nullptr);

        // Build the request carrying the imprint.
        TS_REQ* req = TS_REQ_new();
        TS_MSG_IMPRINT* mi = TS_MSG_IMPRINT_new();
        X509_ALGOR* algo = X509_ALGOR_new();
        if (req && mi && algo) {
            TS_REQ_set_version(req, 1);
            X509_ALGOR_set0(algo, OBJ_nid2obj(NID_sha256), V_ASN1_NULL, nullptr);
            TS_MSG_IMPRINT_set_algo(mi, algo);
            TS_MSG_IMPRINT_set_msg(mi, const_cast<unsigned char*>(imprint),
                                   static_cast<int>(imprintLen));
            TS_REQ_set_msg_imprint(req, mi);
            TS_REQ_set_cert_req(req, 1);

            unsigned char* reqDer = nullptr;
            int reqLen = i2d_TS_REQ(req, &reqDer);
            if (reqLen > 0 && reqDer) {
                BIO* reqBio = BIO_new_mem_buf(reqDer, reqLen);
                if (reqBio) {
                    TS_RESP* resp = TS_RESP_create_response(ctx, reqBio);
                    if (resp) {
                        PKCS7* token = TS_RESP_get_token(resp);
                        if (token) {
                            unsigned char* td = nullptr;
                            int tl = i2d_PKCS7(token, &td);
                            if (tl > 0 && td) {
                                tokenDer.assign(td, td + tl);
                                OPENSSL_free(td);
                            }
                        }
                        TS_RESP_free(resp);
                    }
                    BIO_free(reqBio);
                }
                OPENSSL_free(reqDer);
            }
        }
        if (req) TS_REQ_free(req);       // frees mi + algo it now owns
        else {
            if (mi) TS_MSG_IMPRINT_free(mi);
            if (algo) X509_ALGOR_free(algo);
        }
    }
    if (policy) ASN1_OBJECT_free(policy);
    TS_RESP_CTX_free(ctx);
    ERR_clear_error();
    return tokenDer;
}

} // namespace

class TsaTimestampVerifyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(makeTsaSigner(tsaKey_, tsaCert_, "PDFE Test TSA"));
        // The message imprint the signing path timestamps = SHA-256 of the
        // (here synthetic) signature value.
        const char* sigValue = "the-signature-value-bytes";
        SHA256(reinterpret_cast<const unsigned char*>(sigValue),
               std::strlen(sigValue), imprint_);
        token_ = mintTokenForImprint(tsaKey_, tsaCert_, imprint_, SHA256_DIGEST_LENGTH);
        ASSERT_FALSE(token_.empty()) << "Failed to mint a local RFC 3161 token";
    }
    void TearDown() override {
        if (tsaKey_) EVP_PKEY_free(tsaKey_);
        if (tsaCert_) X509_free(tsaCert_);
    }
    EVP_PKEY* tsaKey_ = nullptr;
    X509* tsaCert_ = nullptr;
    unsigned char imprint_[SHA256_DIGEST_LENGTH] = {0};
    std::vector<uint8_t> token_;
};

// A genuine token over the correct imprint, signed by the expected TSA,
// verifies on all three properties.
TEST_F(TsaTimestampVerifyTest, GenuineTokenVerifiesImprintAndSignature)
{
    auto v = TsaClient::verifyTimestampToken(
        token_, imprint_, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    EXPECT_TRUE(v.parsed);
    EXPECT_TRUE(v.imprintMatches)
        << "messageImprint must equal the timestamped signature digest";
    EXPECT_TRUE(v.signatureValid)
        << "token signed by the supplied TSA cert must verify";
}

// HARD-ASSERT the binding: a token that timestamps DIFFERENT bytes (imprint
// does not match the signature value) must be rejected. This is the forged /
// mismatched-timestamp case — the token is structurally valid and correctly
// signed, but it does not attest to our signature.
TEST_F(TsaTimestampVerifyTest, MismatchedImprintRejected)
{
    unsigned char wrong[SHA256_DIGEST_LENGTH];
    std::memcpy(wrong, imprint_, SHA256_DIGEST_LENGTH);
    wrong[0] ^= 0xFF;  // now attests to a different digest

    auto v = TsaClient::verifyTimestampToken(
        token_, wrong, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    EXPECT_TRUE(v.parsed) << "still a well-formed token";
    EXPECT_FALSE(v.imprintMatches)
        << "a token whose imprint != expected must NOT be treated as timestamping "
           "our signature (RFC 3161 binding violated)";
}

// A token whose signature is checked against an UNRELATED cert must fail the
// signature property (even though the imprint still binds).
TEST_F(TsaTimestampVerifyTest, WrongTsaCertFailsSignature)
{
    EVP_PKEY* otherKey = nullptr; X509* otherCert = nullptr;
    ASSERT_TRUE(makeTsaSigner(otherKey, otherCert, "Unrelated TSA"));

    auto v = TsaClient::verifyTimestampToken(
        token_, imprint_, SHA256_DIGEST_LENGTH, otherCert,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    EXPECT_TRUE(v.imprintMatches) << "imprint still binds the data";
    EXPECT_FALSE(v.signatureValid)
        << "token was not signed by this cert — signature must not verify";

    EVP_PKEY_free(otherKey);
    X509_free(otherCert);
}

// ─────────────────────────────────────────────────────────────────────
// SEC-4 (phase-48 review): a TSA certificate ISSUED BY A CA — the shape of
// every real-world public TSA — must verify.
//
// TsaClient::verifyTimestampToken builds an X509_STORE holding exactly one
// certificate (the caller-supplied TSA cert) and calls TS_RESP_verify_token.
// Without X509_V_FLAG_PARTIAL_CHAIN, X509_verify_cert() requires the path to
// terminate at a SELF-SIGNED certificate, so a CA-issued TSA cert could never
// terminate one and signatureValid was stuck false — the identical latent bug
// phase-48 fixed one file over in ocspResponseIsAuthorized().
//
// Every other test in this file uses makeTsaSigner(), which is self-signed, so
// the bug was structurally unreachable: a self-signed cert satisfies the
// terminate-at-self-signed requirement for free. That is the same
// fixture-topology monoculture recorded in DCA 0014, in a second file.
//
// RED-ness: drop the X509_V_FLAG_PARTIAL_CHAIN block from TsaClient.cpp and
// this test fails on signatureValid while every other test here stays green.
// WrongTsaCertFailsSignature is the paired negative — it proves the flag did
// not make the check accept everything.
// ─────────────────────────────────────────────────────────────────────
TEST_F(TsaTimestampVerifyTest, IntermediateIssuedTsaCertVerifies)
{
    EVP_PKEY* caKey = nullptr; X509* caCert = nullptr;
    EVP_PKEY* leafKey = nullptr; X509* leafCert = nullptr;
    ASSERT_TRUE(makeIssuedTsaSigner(caKey, caCert, leafKey, leafCert,
                                    "PDFE Test TSA Root", "PDFE Test TSA Leaf"));

    // Precondition, asserted rather than assumed: the TSA cert must NOT be
    // self-signed, or this test degenerates into GenuineTokenVerifies... and
    // proves nothing about PARTIAL_CHAIN.
    ASSERT_NE(X509_NAME_cmp(X509_get_subject_name(leafCert),
                            X509_get_issuer_name(leafCert)), 0)
        << "the TSA leaf must be CA-issued, not self-signed";

    unsigned char imprint[SHA256_DIGEST_LENGTH] = {0};
    const char* sigValue = "issued-tsa-signature-value";
    SHA256(reinterpret_cast<const unsigned char*>(sigValue),
           std::strlen(sigValue), imprint);

    auto token = mintTokenForImprint(leafKey, leafCert, imprint,
                                     SHA256_DIGEST_LENGTH);
    ASSERT_FALSE(token.empty()) << "could not mint a token from a CA-issued TSA";

    auto v = TsaClient::verifyTimestampToken(token, imprint,
                                             SHA256_DIGEST_LENGTH, leafCert,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    EXPECT_TRUE(v.parsed);
    EXPECT_TRUE(v.imprintMatches);
    EXPECT_TRUE(v.signatureValid)
        << "a genuine token signed by a CA-ISSUED TSA certificate must verify. "
           "False here means TsaClient's X509_STORE still requires the path to "
           "terminate at a self-signed cert, i.e. no real public TSA can ever "
           "be authenticated";

    X509_free(leafCert);
    EVP_PKEY_free(leafKey);
    X509_free(caCert);
    EVP_PKEY_free(caKey);
}

// ---------------------------------------------------------------------------
// F61 (follow-up phase-67) — the PARTIAL_CHAIN precondition, now enforced.
//
// verifyTimestampToken() installs the caller's certificate as a TRUST ANCHOR
// (X509_V_FLAG_PARTIAL_CHAIN over a one-certificate store). TsaClient.cpp's
// load-bearing comment says why that is sound, and its entire argument rests on
// a precondition about the CALLER: the certificate is "the TSA I expect this
// token to be signed by, already established by whatever means the caller
// used". That was documented and unenforced.
//
// THE REGISTER DESCRIBES THE HAZARD AS REACHING PARTIAL_CHAIN VIA THE `nullptr`
// THE PRODUCTION CALLER PASSES. It does not, and this file already proved that
// before the fix: NullInputsSafe asserts a null certificate yields
// imprintMatches==true / signatureValid==false, because the whole block sits
// inside `if (tsaCert)`. Guarding null would have reddened that test and broken
// LtvValidator, which passes null precisely to get the imprint check alone.
//
// The unenforced precondition is about a NON-NULL certificate's provenance —
// a caller that lifts the cert out of the token hands the attacker its own
// anchor. These two cases pin both directions of the guard.
// ---------------------------------------------------------------------------

// The exploit shape: a token minted by a key nobody trusts, "verified" against
// the certificate that came with it. Before the guard this reported
// signatureValid; the caller had to have applied the precondition itself.
TEST_F(TsaTimestampVerifyTest, UntrustedProvenanceCertIsRefusedNotVerified)
{
    // A self-consistent forgery: attacker key, attacker cert, attacker token.
    EVP_PKEY* rogueKey = nullptr;
    X509* rogueCert = nullptr;
    ASSERT_TRUE(makeTsaSigner(rogueKey, rogueCert, "PDFE Rogue TSA"));

    unsigned char imprint[SHA256_DIGEST_LENGTH] = {0};
    const char* sigValue = "rogue-signature-value";
    SHA256(reinterpret_cast<const unsigned char*>(sigValue),
           std::strlen(sigValue), imprint);
    auto token = mintTokenForImprint(rogueKey, rogueCert, imprint,
                                     SHA256_DIGEST_LENGTH);
    ASSERT_FALSE(token.empty());

    // Control, and it is the whole point of the pair: with the SAME inputs and
    // the caller asserting it established this certificate, the token verifies.
    // So the refusal below is caused by the provenance and nothing else.
    const auto trusted = TsaClient::verifyTimestampToken(
        token, imprint, SHA256_DIGEST_LENGTH, rogueCert,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    ASSERT_TRUE(trusted.signatureValid)
        << "the control arm must verify, or this test proves nothing about "
           "provenance -- it would just be testing a broken token";

    const auto refused = TsaClient::verifyTimestampToken(
        token, imprint, SHA256_DIGEST_LENGTH, rogueCert,
        TsaClient::TsaCertTrust::NotEstablished);

    EXPECT_FALSE(refused.signatureValid)
        << "a certificate whose trust the caller has NOT established was used "
           "as a PARTIAL_CHAIN trust anchor. A token carrying its own signer "
           "then authenticates itself.";
    EXPECT_TRUE(refused.signatureRefusedUntrustedCert)
        << "the refusal must be reported in its own field. Folding it into "
           "signatureValid==false makes 'refused to look' indistinguishable "
           "from 'no certificate supplied', which is how a caller comes to "
           "read a refusal as a benign skip.";

    X509_free(rogueCert);
    EVP_PKEY_free(rogueKey);
}

// The ZERO value of TsaCertTrust must be the refusing one. Raised by the
// security-audit gate: there is no default argument, so this cannot be reached
// by omission today — but `TsaCertTrust{}` value-initialises to enumerator 0,
// and a future call site that writes it must land on the safe branch.
//
// A static_assert rather than a runtime check: the property is about the type,
// and the moment someone reorders the enumerators this stops compiling rather
// than starting to fail.
static_assert(static_cast<int>(TsaClient::TsaCertTrust::NotEstablished) == 0,
              "TsaCertTrust's zero value must be NotEstablished, so a "
              "value-initialised TsaCertTrust{} refuses rather than trusts");
static_assert(TsaClient::TsaCertTrust{} == TsaClient::TsaCertTrust::NotEstablished,
              "a value-initialised TsaCertTrust must be the refusing one");

// The null path is UNCHANGED, stated as its own case because the register's
// proposed fix would have broken it and the only production caller depends on
// it. A null certificate installs no anchor, so provenance is inert.
TEST_F(TsaTimestampVerifyTest, NullCertStillChecksImprintUnderEitherProvenance)
{
    for (auto trust : {TsaClient::TsaCertTrust::EstablishedByCaller,
                       TsaClient::TsaCertTrust::NotEstablished}) {
        const auto v = TsaClient::verifyTimestampToken(
            token_, imprint_, SHA256_DIGEST_LENGTH, nullptr, trust);
        EXPECT_TRUE(v.parsed);
        EXPECT_TRUE(v.imprintMatches)
            << "the RFC 3161 imprint binding must still be checked with no "
               "certificate -- LtvValidator.cpp reads exactly this and nothing "
               "else";
        EXPECT_FALSE(v.signatureValid);
        EXPECT_FALSE(v.signatureRefusedUntrustedCert)
            << "nothing was refused: with a null certificate no trust anchor is "
               "installed, so there was no precondition to violate.";
    }
}

// Garbage bytes are not a token.
TEST_F(TsaTimestampVerifyTest, MalformedTokenNotParsed)
{
    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11};
    auto v = TsaClient::verifyTimestampToken(
        garbage, imprint_, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    EXPECT_FALSE(v.parsed);
    EXPECT_FALSE(v.imprintMatches);
    EXPECT_FALSE(v.signatureValid);
}

// Null/empty inputs are handled without crashing.
TEST_F(TsaTimestampVerifyTest, NullInputsSafe)
{
    EXPECT_NO_THROW({
        auto v1 = TsaClient::verifyTimestampToken({}, imprint_, SHA256_DIGEST_LENGTH, tsaCert_,
                                                   TsaClient::TsaCertTrust::EstablishedByCaller);
        EXPECT_FALSE(v1.parsed);
        auto v2 = TsaClient::verifyTimestampToken(token_, nullptr, 0, tsaCert_,
                                                  TsaClient::TsaCertTrust::EstablishedByCaller);
        EXPECT_FALSE(v2.imprintMatches);
        auto v3 = TsaClient::verifyTimestampToken(token_, imprint_, SHA256_DIGEST_LENGTH, nullptr,
                                                  TsaClient::TsaCertTrust::EstablishedByCaller);
        EXPECT_TRUE(v3.imprintMatches);       // imprint still checked
        EXPECT_FALSE(v3.signatureValid);      // no cert -> signature unchecked
    });
}

// ---------------------------------------------------------------------------
// Integration: the verifier is reachable from the real verify path via
// LtvValidator::checkEmbeddedTimestamp() (which LtvValidator::verifyWith calls
// to populate LtvVerifyResult). These tests drive that exact extraction path
// with genuine PKCS#7 bytes carrying an embedded token, offline.
// ---------------------------------------------------------------------------
namespace {

// Embed a DER TimeStampToken as the id-smime-aa-timeStampToken unauthenticated
// attribute on the first SignerInfo — mirroring Pkcs7OpenSslBridge's embed.
bool embedTimestampAttr(PKCS7* p7, const std::vector<uint8_t>& tokenDer)
{
    STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
    if (!sis || sk_PKCS7_SIGNER_INFO_num(sis) == 0) return false;
    PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
    if (!si) return false;
    ASN1_OBJECT* oid = OBJ_txt2obj("1.2.840.113549.1.9.16.2.14", 1);
    if (!oid) return false;
    if (!si->unauth_attr) si->unauth_attr = sk_X509_ATTRIBUTE_new_null();
    X509_ATTRIBUTE* attr = X509_ATTRIBUTE_new();
    X509_ATTRIBUTE_set1_object(attr, oid);
    X509_ATTRIBUTE_set1_data(attr, V_ASN1_SEQUENCE, tokenDer.data(),
                             static_cast<int>(tokenDer.size()));
    sk_X509_ATTRIBUTE_push(si->unauth_attr, attr);
    ASN1_OBJECT_free(oid);
    return true;
}

// Produce a real PKCS#7 SignedData over some bytes; yields the PKCS7 (caller
// frees) and the SHA-256 of its signature value (the imprint a timestamp binds).
bool buildSignedPkcs7(unsigned char outImprint[SHA256_DIGEST_LENGTH],
                      PKCS7*& outP7 /* caller frees */)
{
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    if (!makeTsaSigner(key, cert, "PDFE Test Signer")) return false;
    BIO* data = BIO_new_mem_buf("document-content-bytes", -1);
    PKCS7* p7 = data ? PKCS7_sign(cert, key, nullptr, data,
                                  PKCS7_BINARY | PKCS7_NOATTR) : nullptr;
    if (data) BIO_free(data);
    EVP_PKEY_free(key);
    X509_free(cert);
    if (!p7) return false;

    STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
    PKCS7_SIGNER_INFO* si = sis && sk_PKCS7_SIGNER_INFO_num(sis) > 0
        ? sk_PKCS7_SIGNER_INFO_value(sis, 0) : nullptr;
    if (!si || !si->enc_digest || !si->enc_digest->data ||
        si->enc_digest->length <= 0) { PKCS7_free(p7); return false; }
    SHA256(si->enc_digest->data,
           static_cast<size_t>(si->enc_digest->length), outImprint);
    outP7 = p7;
    return true;
}

std::vector<uint8_t> derOf(PKCS7* p7)
{
    unsigned char* der = nullptr;
    int len = i2d_PKCS7(p7, &der);
    std::vector<uint8_t> out;
    if (len > 0 && der) { out.assign(der, der + len); OPENSSL_free(der); }
    return out;
}

} // namespace

using namespace pdfe::signatures;

// A signature whose embedded timestamp binds it: the real verify path reports
// present == true, imprintValid == true.
TEST_F(TsaTimestampVerifyTest, IntegrationGenuineEmbeddedTimestampBinds)
{
    unsigned char imprint[SHA256_DIGEST_LENGTH];
    PKCS7* p7 = nullptr;
    ASSERT_TRUE(buildSignedPkcs7(imprint, p7));
    auto token = mintTokenForImprint(tsaKey_, tsaCert_, imprint, SHA256_DIGEST_LENGTH);
    ASSERT_FALSE(token.empty());
    ASSERT_TRUE(embedTimestampAttr(p7, token));

    auto der = derOf(p7);
    PKCS7_free(p7);
    ASSERT_FALSE(der.empty());

    auto chk = LtvValidator::checkEmbeddedTimestamp(der);
    EXPECT_TRUE(chk.present) << "embedded timestamp attribute must be found";
    EXPECT_TRUE(chk.imprintValid)
        << "a timestamp over SHA-256(signature value) must bind the signature";
}

// A signature carrying a timestamp that attests to DIFFERENT bytes: present ==
// true but imprintValid == false — the real path detects the mismatch.
TEST_F(TsaTimestampVerifyTest, IntegrationForgedEmbeddedTimestampDetected)
{
    unsigned char imprint[SHA256_DIGEST_LENGTH];
    PKCS7* p7 = nullptr;
    ASSERT_TRUE(buildSignedPkcs7(imprint, p7));

    unsigned char wrong[SHA256_DIGEST_LENGTH];
    std::memcpy(wrong, imprint, SHA256_DIGEST_LENGTH);
    wrong[5] ^= 0xAA;  // timestamp binds different bytes than the signature
    auto forged = mintTokenForImprint(tsaKey_, tsaCert_, wrong, SHA256_DIGEST_LENGTH);
    ASSERT_FALSE(forged.empty());
    ASSERT_TRUE(embedTimestampAttr(p7, forged));

    auto der = derOf(p7);
    PKCS7_free(p7);
    ASSERT_FALSE(der.empty());

    auto chk = LtvValidator::checkEmbeddedTimestamp(der);
    EXPECT_TRUE(chk.present) << "the attribute is present";
    EXPECT_FALSE(chk.imprintValid)
        << "a mismatched-imprint timestamp must NOT be reported as binding";
}

// A signature with no timestamp at all: present == false.
TEST_F(TsaTimestampVerifyTest, IntegrationNoTimestampReportsAbsent)
{
    unsigned char imprint[SHA256_DIGEST_LENGTH];
    PKCS7* p7 = nullptr;
    ASSERT_TRUE(buildSignedPkcs7(imprint, p7));
    auto der = derOf(p7);
    PKCS7_free(p7);
    ASSERT_FALSE(der.empty());

    auto chk = LtvValidator::checkEmbeddedTimestamp(der);
    EXPECT_FALSE(chk.present);
    EXPECT_FALSE(chk.imprintValid);
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0006 (follow-up phase 42).
//
// The INSTANCE tests above each pin one property of one artifact:
// MismatchedImprintRejected pins the imprint binding,
// IntegrationForgedEmbeddedTimestampDetected pins the same binding through the
// wired path. Every one of them was written AFTER someone noticed the consumer
// was missing — which is the thing DCA 0006 says is the problem. Its class is
// *"asymmetric feature — producer without a consumer, where the absent consumer
// is also the absent oracle"*: you cannot write a test for a verdict that has
// no API to produce it, so the gap is structurally invisible until a human
// happens to read both halves of the feature.
//
// THE CLASS-LEVEL FORM inverts the question. Instead of asking "does the
// consumer we know about work?", it asks "is there ANY artifact the producer
// emits that nothing observes?" — and answers it mechanically:
//
//   1. TAMPER SYMMETRY. For each property of the embedded artifact, tamper with
//      exactly that property and require an OBSERVABLE at the read surface to
//      change. A property no observable reacts to has no consumer — that is
//      the 0006 defect, detected without knowing in advance which property was
//      forgotten. The genuine article is asserted alongside, so "report
//      everything as broken" cannot pass.
//   2. PRODUCER ENUMERATION. The rows are only as complete as the list of
//      things the producer embeds, so that list is read out of the PRODUCER'S
//      SOURCE rather than remembered. Pkcs7OpenSslBridge embeds exactly one
//      unauthenticated attribute (id-smime-aa-timeStampToken,
//      1.2.840.113549.1.9.16.2.14). If a second embedded artifact is ever
//      added, this goes red and demands its tamper rows — instead of shipping
//      another write-only artifact.
//
// RED-ness: drop the imprint hard-assert from TsaClient::verifyTimestampToken
// and the imprint row fails; drop the TS_RESP_verify_token call and the
// TSA-signature row fails; make checkEmbeddedTimestamp always report present
// and the presence row fails. Each tamper axis fails independently, which is
// what tells you WHICH consumer went missing.
TEST_F(TsaTimestampVerifyTest, EveryEmbeddedArtifactPropertyHasAReactingConsumer)
{
    // ---- 2. PRODUCER ENUMERATION (read from the producer's source) ----
    {
        const std::filesystem::path bridge =
            std::filesystem::path(PDFE_REPO_ROOT) / "src" / "signatures" /
            "Pkcs7OpenSslBridge.cpp";
        ASSERT_TRUE(std::filesystem::exists(bridge))
            << "cannot find " << bridge.string()
            << " — this guard must fail closed, not pass silently";
        std::ifstream in(bridge, std::ios::binary);
        ASSERT_TRUE(in.good());

        // Count the distinct OIDs the signing path turns into embedded
        // attributes. Today: exactly one, the RFC 3161 timestamp token.
        std::set<std::string> embeddedOids;
        std::string line;
        // Custom delimiter: the pattern itself contains a `)"` sequence.
        const std::regex oidRx(R"RX(OBJ_txt2obj\(\s*"([0-9.]+)")RX");
        while (std::getline(in, line)) {
            const auto firstNonSpace = line.find_first_not_of(" \t\r");
            if (firstNonSpace != std::string::npos &&
                line.compare(firstNonSpace, 2, "//") == 0) {
                continue;
            }
            std::smatch m;
            if (std::regex_search(line, m, oidRx)) embeddedOids.insert(m[1].str());
        }

        EXPECT_EQ(embeddedOids.size(), 1u)
            << "Pkcs7OpenSslBridge now embeds " << embeddedOids.size()
            << " attribute OIDs into the signature. Every artifact the signing "
               "path emits needs a read-side consumer AND a tamper row below — "
               "an artifact nothing reads proves nothing, which is DCA 0006.";
        EXPECT_TRUE(embeddedOids.count("1.2.840.113549.1.9.16.2.14") == 1)
            << "the id-smime-aa-timeStampToken OID is no longer embedded by the "
               "signing path — the rows below no longer describe the producer";
    }

    // ---- 1. TAMPER SYMMETRY -------------------------------------------
    // Baseline: a genuine signature carrying a genuine, binding timestamp.
    unsigned char imprint[SHA256_DIGEST_LENGTH];
    PKCS7* genuineP7 = nullptr;
    ASSERT_TRUE(buildSignedPkcs7(imprint, genuineP7));
    auto genuineToken = mintTokenForImprint(tsaKey_, tsaCert_, imprint,
                                             SHA256_DIGEST_LENGTH);
    ASSERT_FALSE(genuineToken.empty());
    ASSERT_TRUE(embedTimestampAttr(genuineP7, genuineToken));
    const auto genuineDer = derOf(genuineP7);
    PKCS7_free(genuineP7);
    ASSERT_FALSE(genuineDer.empty());

    const auto baseline = LtvValidator::checkEmbeddedTimestamp(genuineDer);
    ASSERT_TRUE(baseline.present)
        << "positive control failed: the genuine embedded token was not even "
           "found, so no tamper row below can mean anything";
    ASSERT_TRUE(baseline.imprintValid)
        << "positive control failed: the genuine token does not bind";
    const auto baselineToken = TsaClient::verifyTimestampToken(
        genuineToken, imprint, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
    ASSERT_TRUE(baselineToken.parsed);
    ASSERT_TRUE(baselineToken.imprintMatches);
    ASSERT_TRUE(baselineToken.signatureValid);

    // AXIS A — PRESENCE. Remove the artifact: `present` must react.
    {
        unsigned char imp[SHA256_DIGEST_LENGTH];
        PKCS7* p7 = nullptr;
        ASSERT_TRUE(buildSignedPkcs7(imp, p7));
        const auto der = derOf(p7);
        PKCS7_free(p7);
        ASSERT_FALSE(der.empty());
        const auto chk = LtvValidator::checkEmbeddedTimestamp(der);
        EXPECT_NE(chk.present, baseline.present)
            << "removing the embedded timestamp changed NOTHING observable at "
               "the read surface — nothing consumes its presence (DCA 0006)";
        EXPECT_FALSE(chk.present);
    }

    // AXIS B — IMPRINT BINDING. Same TSA, same structure, attests to different
    // bytes. `imprintValid` must react; this is the property whose consumer was
    // entirely absent before DCA 0006.
    {
        unsigned char imp[SHA256_DIGEST_LENGTH];
        PKCS7* p7 = nullptr;
        ASSERT_TRUE(buildSignedPkcs7(imp, p7));
        unsigned char wrong[SHA256_DIGEST_LENGTH];
        std::memcpy(wrong, imp, SHA256_DIGEST_LENGTH);
        wrong[11] ^= 0x5Au;
        auto forged = mintTokenForImprint(tsaKey_, tsaCert_, wrong,
                                           SHA256_DIGEST_LENGTH);
        ASSERT_FALSE(forged.empty());
        ASSERT_TRUE(embedTimestampAttr(p7, forged));
        const auto der = derOf(p7);
        PKCS7_free(p7);
        ASSERT_FALSE(der.empty());

        const auto chk = LtvValidator::checkEmbeddedTimestamp(der);
        EXPECT_TRUE(chk.present) << "the attribute is still there";
        EXPECT_NE(chk.imprintValid, baseline.imprintValid)
            << "a timestamp attesting to DIFFERENT bytes was reported exactly "
               "like one that binds this signature — nothing consumes the RFC "
               "3161 messageImprint (DCA 0006, the original defect)";
        EXPECT_FALSE(chk.imprintValid);
    }

    // AXIS C — TSA SIGNATURE. Correct imprint, but minted by an unrelated TSA.
    // `signatureValid` must react while `imprintMatches` stays true, so the two
    // properties are shown to be independently observed rather than conflated.
    {
        EVP_PKEY* otherKey = nullptr;
        X509* otherCert = nullptr;
        ASSERT_TRUE(makeTsaSigner(otherKey, otherCert, "Unrelated TSA"));
        auto otherToken = mintTokenForImprint(otherKey, otherCert, imprint,
                                               SHA256_DIGEST_LENGTH);
        const auto v = TsaClient::verifyTimestampToken(
            otherToken, imprint, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
        EVP_PKEY_free(otherKey);
        X509_free(otherCert);
        ASSERT_FALSE(otherToken.empty());

        EXPECT_TRUE(v.parsed);
        EXPECT_TRUE(v.imprintMatches)
            << "the imprint still binds — only the signer differs";
        EXPECT_NE(v.signatureValid, baselineToken.signatureValid)
            << "a token signed by an unrelated TSA verified identically to one "
               "signed by the expected TSA — nothing consumes the token "
               "signature (DCA 0006 class)";
        EXPECT_FALSE(v.signatureValid);
    }

    // AXIS D — STRUCTURE. Corrupt the DER; `parsed` must react rather than the
    // bytes being waved through as "a timestamp we chose not to look at".
    {
        auto corrupt = genuineToken;
        ASSERT_GT(corrupt.size(), 32u);
        for (size_t i = 8; i < 24; ++i) corrupt[i] ^= 0xFFu;
        const auto v = TsaClient::verifyTimestampToken(
            corrupt, imprint, SHA256_DIGEST_LENGTH, tsaCert_,
        TsaClient::TsaCertTrust::EstablishedByCaller);
        EXPECT_NE(v.parsed && v.imprintMatches && v.signatureValid,
                  baselineToken.parsed && baselineToken.imprintMatches &&
                      baselineToken.signatureValid)
            << "structurally corrupt token bytes verified exactly like a "
               "genuine token — nothing consumes the token structure";
    }
}
