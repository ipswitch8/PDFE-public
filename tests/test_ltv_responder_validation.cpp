// -*- coding: utf-8 -*-
//
// Pipeline f8a12c4d phase-3 (SEC-LTV-CERT): OCSP/CRL issuer certificate
// validation edge cases and boundary conditions.

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "signatures/SignatureManager.h"
#include "signatures/TrustAnchorManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QCoreApplication>
#include <QRectF>
#include <QString>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    // PER-PROCESS, not a fixed shared name. The suite runs under a 16-shard
    // parallel runner and every case here writes the same file name into this
    // directory, so a fixed path is shared with every OTHER shard and with any
    // residue a previous run left behind. MEASURED on 2026-08-12: SetUp threw
    // `remove: The process cannot access the file because it is being used by
    // another process` on ltv_resp_test.pdf, and the runner HEALED it on retry
    // — i.e. the failure was absorbed into a heal list rather than reported.
    // Isolation is the fix; a retry is not.
    static const auto d = [] {
        auto p = std::filesystem::temp_directory_path()
               / ("pdfe_ltv_resp_"
                  + std::to_string(
                        static_cast<long long>(QCoreApplication::applicationPid())));
        std::filesystem::create_directories(p);
        return p;
    }();
    std::filesystem::create_directories(d);
    return d;
}

// ── phase-48 (F47 item F1) fixture migration ────────────────────────────
// This suite used to run on tests/fixtures/test_signer.pfx: a SELF-SIGNED
// END-ENTITY certificate that acted as its own CA and issued its own CRLs and
// OCSP responses. That is not a chain any real deployment produces, and it
// blocked RFC 5280 §6.3.3(f) enforcement outright — the fixture carries
// keyUsage `digitalSignature, contentCommitment` and no `cRLSign`, so the rule
// (correctly) rejects every CRL it signs.
//
// The suite now runs on tests/fixtures/crl_chain/ (built by
// tests/fixtures/gen_two_level_ca_signer.py): root CA → intermediate CA
// bearing cRLSign → end-entity signer. The signer is the leaf, and ALL
// revocation evidence — OCSP and CRL alike — is issued by the intermediate,
// which is what actually happens in the field. test_signer.pfx is untouched
// and every other suite still uses it.
//
// Consequence worth stating: before this migration, several rejection tests
// here would have passed for the WRONG reason once the cRLSign rule landed
// (every CRL rejected, so "expired CRL rejected" proved nothing about
// expiry). Running them against an issuer that IS entitled to sign CRLs keeps
// the freshness and structure rules load-bearing.
struct CrlChainMaterial {
    X509* leaf = nullptr;        // the signer (end-entity)
    X509* issuer = nullptr;      // the intermediate CA — issues the evidence
    EVP_PKEY* issuerKey = nullptr;
    X509* root = nullptr;        // the trust anchor
};

std::vector<uint8_t> readFileBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    auto s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

X509* loadDerCertFile(const std::filesystem::path& p)
{
    auto der = readFileBytes(p);
    if (der.empty()) return nullptr;
    const unsigned char* q = der.data();
    X509* c = d2i_X509(nullptr, &q, static_cast<long>(der.size()));
    if (!c) ERR_clear_error();
    return c;
}

bool loadCrlChain(CrlChainMaterial& out)
{
    const auto dir = std::filesystem::path(fixturesDir()) / "crl_chain";
    out.leaf = loadDerCertFile(dir / "leaf_crlsign.der");
    out.issuer = loadDerCertFile(dir / "inter_crlsign.der");
    out.root = loadDerCertFile(dir / "root.der");
    BIO* kb = BIO_new_file((dir / "inter_crlsign_key.pem").string().c_str(), "rb");
    if (kb) {
        out.issuerKey = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        BIO_free(kb);
    }
    if (!out.issuerKey) ERR_clear_error();
    if (out.leaf && out.issuer && out.root && out.issuerKey) return true;
    if (out.leaf) { X509_free(out.leaf); out.leaf = nullptr; }
    if (out.issuer) { X509_free(out.issuer); out.issuer = nullptr; }
    if (out.root) { X509_free(out.root); out.root = nullptr; }
    if (out.issuerKey) { EVP_PKEY_free(out.issuerKey); out.issuerKey = nullptr; }
    return false;
}

// Build an OCSP response ABOUT `target`, issued and signed by `issuerCert` /
// `issuerKey`. The CERTID is (target, issuerCert), which is exactly the id
// evaluateOcspForCert() reconstructs, so a mismatch here would silently
// degrade every OCSP test into "no response found".
std::vector<uint8_t> buildOcspDer(EVP_PKEY* issuerKey, X509* issuerCert,
                                   X509* target, int status,
                                   time_t thisUpd, time_t nextUpd)
{
    std::vector<uint8_t> result;
    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    OCSP_CERTID* cid = OCSP_cert_to_id(EVP_sha1(), target, issuerCert);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, thisUpd);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, nextUpd);
    ASN1_TIME* rev = nullptr;
    if (status == V_OCSP_CERTSTATUS_REVOKED)
        rev = ASN1_TIME_set(nullptr, time(nullptr) - 3600);
    OCSP_basic_add1_status(basic, cid, status, 0, rev, t1, t2);
    OCSP_CERTID_free(cid);
    ASN1_TIME_free(t1); ASN1_TIME_free(t2);
    if (rev) ASN1_TIME_free(rev);
    OCSP_basic_sign(basic, issuerCert, issuerKey, EVP_sha256(), nullptr, 0);
    OCSP_RESPONSE* resp = OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    OCSP_BASICRESP_free(basic);
    unsigned char* der = nullptr;
    int len = i2d_OCSP_RESPONSE(resp, &der);
    OCSP_RESPONSE_free(resp);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

// Like buildOcspDer but decouples the RESPONDER (respCert/respKey — who signs
// the response) from the CERTID subject/issuer (target/issuer — who the
// response is about). This lets a test forge a GOOD status about `target`
// while signing with an unrelated rogue key, exercising responder
// authentication (RFC 6960 §4.2.2.2).
std::vector<uint8_t> buildOcspDerSignedBy(X509* target, X509* issuer,
                                           EVP_PKEY* respKey, X509* respCert,
                                           int status,
                                           time_t thisUpd, time_t nextUpd)
{
    std::vector<uint8_t> result;
    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    OCSP_CERTID* cid = OCSP_cert_to_id(EVP_sha1(), target, issuer);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, thisUpd);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, nextUpd);
    OCSP_basic_add1_status(basic, cid, status, 0, nullptr, t1, t2);
    OCSP_CERTID_free(cid);
    ASN1_TIME_free(t1); ASN1_TIME_free(t2);
    // Sign with the responder's own key/cert; OCSP_basic_sign embeds respCert
    // as the responder certificate in the response.
    OCSP_basic_sign(basic, respCert, respKey, EVP_sha256(), nullptr, 0);
    OCSP_RESPONSE* resp = OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    OCSP_BASICRESP_free(basic);
    unsigned char* der = nullptr;
    int len = i2d_OCSP_RESPONSE(resp, &der);
    OCSP_RESPONSE_free(resp);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

// Generate a fresh, unrelated self-signed cert + key: a "rogue" OCSP
// responder that is NOT the issuer of any real cert and chains to no trusted
// root. Returns false on any OpenSSL failure.
bool makeRogueSigner(EVP_PKEY*& outKey, X509*& outCert)
{
    outKey = EVP_EC_gen("P-256");
    if (!outKey) return false;
    outCert = X509_new();
    if (!outCert) { EVP_PKEY_free(outKey); outKey = nullptr; return false; }
    X509_set_version(outCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(outCert), 0x5A5A);
    X509_gmtime_adj(X509_getm_notBefore(outCert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(outCert), 60 * 60 * 24);
    X509_set_pubkey(outCert, outKey);
    X509_NAME* nm = X509_get_subject_name(outCert);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("Rogue OCSP Responder"), -1, -1, 0);
    X509_set_issuer_name(outCert, nm);  // self-signed
    if (X509_sign(outCert, outKey, EVP_sha256()) == 0) {
        X509_free(outCert); outCert = nullptr;
        EVP_PKEY_free(outKey); outKey = nullptr;
        return false;
    }
    return true;
}

// Write a cert to a temp PEM file so TrustAnchorManager::addTrust can import it.
std::filesystem::path writeCertPem(X509* cert, const char* name)
{
    auto p = tempDir() / name;
    BIO* bio = BIO_new_file(p.string().c_str(), "wb");
    if (!bio) return {};
    PEM_write_bio_X509(bio, cert);
    BIO_free(bio);
    return p;
}

// RAII trust anchor: adds `pem` as a trust anchor for the scope, then removes
// exactly the anchor it added (identified by diffing the anchor list) so other
// tests' trust state is untouched.
struct ScopedTrustAnchor {
    QString fp;
    bool added = false;
    explicit ScopedTrustAnchor(const std::filesystem::path& pem)
    {
        auto& tam = pdfe::signatures::TrustAnchorManager::instance();
        auto before = tam.listTrusts();
        added = tam.addTrust(pem);
        if (!added) return;
        for (const auto& a : tam.listTrusts()) {
            bool seen = false;
            for (const auto& b : before)
                if (b.fingerprintSha256 == a.fingerprintSha256) { seen = true; break; }
            if (!seen) { fp = a.fingerprintSha256; break; }
        }
    }
    ~ScopedTrustAnchor()
    {
        if (added && !fp.isEmpty())
            pdfe::signatures::TrustAnchorManager::instance().removeTrust(fp);
    }
};

std::vector<uint8_t> buildCrlDer(EVP_PKEY* key, X509* cert,
                                  time_t lastUpd, time_t nextUpd)
{
    std::vector<uint8_t> result;
    X509_CRL* crl = X509_CRL_new();
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(cert));
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, lastUpd);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, nextUpd);
    X509_CRL_set1_lastUpdate(crl, t1);
    X509_CRL_set1_nextUpdate(crl, t2);
    ASN1_TIME_free(t1); ASN1_TIME_free(t2);
    X509_CRL_sort(crl);
    X509_CRL_sign(crl, key, EVP_sha256());
    unsigned char* der = nullptr;
    int len = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

// Like buildCrlDer but decouples the CRL's ISSUER NAME from the key that SIGNS
// it, and can list a serial as revoked. That separation is the whole point: it
// lets a test mint a CRL that CLAIMS to come from the real issuer (same DN,
// which is all the pre-phase-42 validator ever checked) while being signed by
// an unrelated rogue key. Mirrors buildOcspDerSignedBy on the CRL side.
std::vector<uint8_t> buildCrlDerAs(X509_NAME* issuerName, EVP_PKEY* signKey,
                                    time_t lastUpd, time_t nextUpd,
                                    const ASN1_INTEGER* revokedSerial)
{
    std::vector<uint8_t> result;
    X509_CRL* crl = X509_CRL_new();
    if (!crl) return result;
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, issuerName);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, lastUpd);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, nextUpd);
    X509_CRL_set1_lastUpdate(crl, t1);
    X509_CRL_set1_nextUpdate(crl, t2);
    ASN1_TIME_free(t1); ASN1_TIME_free(t2);
    if (revokedSerial) {
        X509_REVOKED* rev = X509_REVOKED_new();
        if (rev) {
            X509_REVOKED_set_serialNumber(
                rev, const_cast<ASN1_INTEGER*>(revokedSerial));
            ASN1_TIME* rt = ASN1_TIME_set(nullptr, time(nullptr) - 3600);
            X509_REVOKED_set_revocationDate(rev, rt);
            ASN1_TIME_free(rt);
            // X509_CRL_add0_revoked takes ownership on success.
            if (X509_CRL_add0_revoked(crl, rev) == 0) X509_REVOKED_free(rev);
        }
    }
    X509_CRL_sort(crl);
    X509_CRL_sign(crl, signKey, EVP_sha256());
    unsigned char* der = nullptr;
    int len = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

// A CRL with NO nextUpdate field at all. Pre-phase-42 such a CRL never
// expired, so one issued at any point in history counted as fresh evidence.
std::vector<uint8_t> buildCrlDerAsNoNextUpdate(X509_NAME* issuerName,
                                                EVP_PKEY* signKey,
                                                time_t lastUpd)
{
    std::vector<uint8_t> result;
    X509_CRL* crl = X509_CRL_new();
    if (!crl) return result;
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, issuerName);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, lastUpd);
    X509_CRL_set1_lastUpdate(crl, t1);
    ASN1_TIME_free(t1);
    // Deliberately no X509_CRL_set1_nextUpdate.
    X509_CRL_sort(crl);
    X509_CRL_sign(crl, signKey, EVP_sha256());
    unsigned char* der = nullptr;
    int len = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

// A DELTA CRL: carries the critical deltaCRLIndicator extension naming a base
// CRL number. It lists only what changed since that base, so on its own it
// conceals every revocation the base recorded.
std::vector<uint8_t> buildDeltaCrlDer(X509_NAME* issuerName, EVP_PKEY* signKey,
                                       time_t lastUpd, time_t nextUpd)
{
    std::vector<uint8_t> result;
    X509_CRL* crl = X509_CRL_new();
    if (!crl) return result;
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, issuerName);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, lastUpd);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, nextUpd);
    X509_CRL_set1_lastUpdate(crl, t1);
    X509_CRL_set1_nextUpdate(crl, t2);
    ASN1_TIME_free(t1); ASN1_TIME_free(t2);

    // deltaCRLIndicator = base CRL number (critical per RFC 5280 §5.2.4).
    ASN1_INTEGER* base = ASN1_INTEGER_new();
    if (base) {
        ASN1_INTEGER_set(base, 1);
        X509_CRL_add1_ext_i2d(crl, NID_delta_crl, base, /*crit=*/1, 0);
        ASN1_INTEGER_free(base);
    }

    X509_CRL_sort(crl);
    X509_CRL_sign(crl, signKey, EVP_sha256());
    unsigned char* der = nullptr;
    int len = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl);
    if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
    return result;
}

std::filesystem::path buildSignedFixture(std::shared_ptr<MuPdfContext> ctx)
{
    auto pfx = std::filesystem::path(fixturesDir()) / "crl_chain" / "leaf_crlsign.pfx";
    auto src = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(src)) return {};
    auto out = tempDir() / "ltv_resp_test.pdf";
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);
    { auto doc = Document::open(ctx, src); auto pg = doc->page(0);
      mgr.createSignatureField(pg.get(), "RespSig", QRectF(100,500,200,50));
      doc->saveAs(out); }
    { auto doc = Document::open(ctx, out); auto pg = doc->page(0);
      SigningOptions opts; opts.pfxPath = pfx.string();
      opts.pfxPassword = "test123"; opts.reason = "LTV resp test";
      mgr.signField(doc.get(), pg.get(), 0, opts); }
    return out;
}

} // namespace

class LtvResponderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        // phase-48: this was `if (signedPath_.empty()) GTEST_SKIP() << ...`.
        // Both inputs it depends on — tests/fixtures/test_single.pdf and
        // tests/fixtures/crl_chain/leaf_crlsign.pfx — are COMMITTED, so a
        // failure here means the corpus was lost or corrupted, not that the
        // environment lacks something optional. A skip in that situation
        // silently disables all 14 tests in this suite and reads as green,
        // which is the masking pattern this programme keeps finding (F45 was
        // the same class in reverse). Fail loudly instead.
        signedPath_ = buildSignedFixture(ctx_);
        ASSERT_FALSE(signedPath_.empty())
            << "could not sign tests/fixtures/test_single.pdf with "
               "tests/fixtures/crl_chain/leaf_crlsign.pfx — both are committed, "
               "so this is a lost or corrupt fixture; regenerate with "
               "tests/fixtures/gen_two_level_ca_signer.py";
        ASSERT_TRUE(loadCrlChain(chain_))
            << "tests/fixtures/crl_chain/ is missing or unreadable — "
               "regenerate with tests/fixtures/gen_two_level_ca_signer.py";
        cert_ = chain_.leaf;
        issuerCert_ = chain_.issuer;
        issuerKey_ = chain_.issuerKey;

        // A1-62: install a trust anchor for the whole fixture.
        //
        // Without a trusted chain, verifyWith() short-circuits at
        // `if (!chainValid) return UntrustedChain;` BEFORE the OCSP/CRL
        // freshness verdict can influence anything — which is precisely why
        // the boundary tests below could only manage a non-pinning
        // EXPECT_NE: every one of them returned the same UntrustedChain
        // regardless of what the revocation data said. With the anchor in
        // place the classification becomes:
        //     valid revocation evidence                   -> LtvValidated
        //     stale / expired / unparseable evidence      -> RevocationEvidenceRejected
        //     no evidence supplied at all                 -> NoDssData
        // so each boundary case has ONE correct answer to pin.
        //
        // PHASE-50 (F53) split the middle row out of the last one; before
        // that, present-and-refused and never-supplied were the same value.
        // The assertions below were updated with the code, this note was not,
        // and the phase-50 review caught it — see the fuller correction on the
        // A1-62 REPAIR block further down.
        //
        // phase-48: the anchor is now the ROOT CA, not the signer. Anchoring
        // the signer itself only made sense while the signer was its own
        // issuer; with a real chain the anchor belongs at the top of it.
        anchorPem_ = writeCertPem(chain_.root, "ltv_resp_shared_anchor.pem");
        ASSERT_FALSE(anchorPem_.empty()) << "Could not write trust anchor PEM";
        anchor_ = std::make_unique<ScopedTrustAnchor>(anchorPem_);
        ASSERT_TRUE(anchor_->added) << "Failed to install test trust anchor";
    }
    void TearDown() override {
        anchor_.reset();
        if (chain_.issuerKey) EVP_PKEY_free(chain_.issuerKey);
        if (chain_.leaf) X509_free(chain_.leaf);
        if (chain_.issuer) X509_free(chain_.issuer);
        if (chain_.root) X509_free(chain_.root);
        chain_ = CrlChainMaterial{};
        cert_ = nullptr; issuerCert_ = nullptr; issuerKey_ = nullptr;
    }
    // The /DSS /Certs the document supplies: the signer leaf plus the
    // intermediate that issued it. The intermediate is what makes the chain
    // buildable AND what is entitled to authorize revocation evidence; the
    // root is never embedded (it is the anchor, supplied out of band).
    std::vector<std::vector<uint8_t>> certChain() const {
        std::vector<std::vector<uint8_t>> out;
        for (X509* c : {cert_, issuerCert_}) {
            unsigned char* d = nullptr; int l = i2d_X509(c, &d);
            if (l > 0 && d) { out.emplace_back(d, d + l); OPENSSL_free(d); }
        }
        return out;
    }
    // Verify with the shared trust anchor honoured, so the OCSP/CRL verdict
    // — not chain trust — decides the outcome.
    LtvVerifyResult verifyTrusted(
        pdfe::core::Document* doc,
        const std::vector<std::vector<uint8_t>>& ocsps,
        const std::vector<std::vector<uint8_t>>& crls) const {
        return LtvValidator::verifyWith(doc, certChain(), ocsps, crls,
                                        /*useTrustAnchors=*/true);
    }
    static std::string shown(LtvStatus s) {
        return ltvStatusToDisplayString(s).toStdString();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path signedPath_;
    std::filesystem::path anchorPem_;
    std::unique_ptr<ScopedTrustAnchor> anchor_;
    CrlChainMaterial chain_;
    X509* cert_ = nullptr;         // signer leaf (alias into chain_)
    X509* issuerCert_ = nullptr;   // intermediate CA — issues all evidence
    EVP_PKEY* issuerKey_ = nullptr;
};

// ═════════════════════════════════════════════════════════════════════
// A1-62 REPAIR (meta-audit-2026-07 status register).
//
// WHAT WAS WRONG. This file carried 7 EXPECT_NE assertions against 2
// EXPECT_EQ. `EXPECT_NE(status, InvalidSignature)` is satisfied by EIGHT of
// the nine LtvStatus values (phase-50 added RevocationEvidenceRejected; this
// sentence said "seven of eight" and was not updated with the assertions it
// describes), so the "7-day OCSP boundary" and "CRL expiry
// boundary" tests could not tell accept from reject: every one of them
// returned UntrustedChain (the tests passed useTrustAnchors=false, which
// short-circuits before revocation is classified) and every one of them
// passed. Widening the 7-day window, deleting the freshness check outright,
// or inverting the CRL expiry comparison were all invisible.
//
// THE REPAIR. The signer is installed as a trust anchor (see SetUp), which
// makes the revocation verdict decisive, and every boundary assertion is
// now EXPECT_EQ against the ONE status that boundary must produce:
//     valid, authorized, fresh revocation evidence -> LtvValidated
//     stale / expired / unparseable                -> RevocationEvidenceRejected
//     absent                                       -> NoDssData
//
// CORRECTED BY THE PHASE-50 REVIEW. This mapping used to send all four of
// stale/expired/unparseable/absent to NoDssData, which was accurate until F53
// gave present-and-refused evidence its own status. Each EXPECT_EQ below was
// updated at that time; this explanatory note was not, and a reader trusting
// it would have concluded the assertions were wrong. The mutation list that
// follows is unaffected — every mutation named there is still killed, because
// each one flips its test to LtvValidated, and LtvValidated is not any of the
// three expected values above.
//
// MUTATIONS KILLED (all in LtvValidator.cpp):
//   * raise kOcspMaxAgeSec (7 days -> 30 days) -> OcspJustPast7Day...
//     expects NoDssData, gets LtvValidated.
//   * lower kOcspMaxAgeSec (7 days -> 6 days)  -> OcspAtExact7Day...
//     expects LtvValidated, gets NoDssData.
//   * delete the OCSP_check_validity() call    -> both stale/expired tests
//     flip to LtvValidated.
//   * flip the CRL expiry test (`< 0` -> `> 0`, or drop `expired`)
//     -> CrlJustPastExpiry... flips to LtvValidated.
//   * treat an unparseable OCSP/CRL blob as evidence (drop the `continue`
//     after d2i failure) -> the Malformed* tests flip to LtvValidated.
//   * make hasRevocationEvidence unconditional -> EmptyOcspAndEmptyCrl...
//     flips to LtvValidated.
// ═════════════════════════════════════════════════════════════════════

TEST_F(LtvResponderTest, OcspAtExact7DayBoundaryAccepted)
{
    time_t now = time(nullptr);
    // thisUpdate = 6 days 23 hours ago: INSIDE the 7-day max-age window.
    // nextUpdate = +1 day so the response is not also expired — this case
    // isolates the max-age rule.
    auto ocsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                              now - (7 * 86400 - 3600), now + 86400);
    ASSERT_FALSE(ocsp.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {ocsp}, {});
    EXPECT_EQ(r.status, LtvStatus::LtvValidated)
        << "A GOOD OCSP one hour inside the 7-day max-age window is valid "
           "revocation evidence and must produce full LTV; got "
        << shown(r.status);
}

TEST_F(LtvResponderTest, OcspJustPast7DayBoundaryRejected)
{
    time_t now = time(nullptr);
    // thisUpdate = 7 days + 1 hour ago: one hour PAST the max-age window.
    // nextUpdate is deliberately still in the future so the ONLY reason to
    // reject is the 7-day staleness rule. (The old version also expired the
    // response, so it could not tell which rule fired.)
    auto ocsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                              now - (7 * 86400 + 3600), now + 86400);
    ASSERT_FALSE(ocsp.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {ocsp}, {});
    // PHASE-50 (F53): was NoDssData. A stale response IS evidence that was
    // present and was refused, and it now says so — NoDssData is reserved for
    // evidence that was never supplied. Still fail-closed: not LtvValidated.
    EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
        << "A GOOD OCSP one hour PAST the 7-day max age is stale and must be "
           "discarded as refused evidence; got " << shown(r.status);
    EXPECT_NE(r.status, LtvStatus::LtvValidated);
    EXPECT_TRUE(r.message.contains("freshness"))
        << "the refusing rule must be named; message was: "
        << r.message.toStdString();
}

TEST_F(LtvResponderTest, OcspWithExpiredNextUpdateRejected)
{
    time_t now = time(nullptr);
    // Fresh thisUpdate (1 hour old, well inside max-age) but nextUpdate is
    // already in the past: the responder itself declared this answer stale.
    // Isolates the expiry rule from the max-age rule.
    auto ocsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                              now - 3600, now - 1800);
    ASSERT_FALSE(ocsp.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {ocsp}, {});
    // PHASE-50 (F53): was NoDssData — see OcspJustPast7DayBoundaryRejected.
    EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
        << "An OCSP response whose nextUpdate has passed is expired and must "
           "not count as revocation evidence; got " << shown(r.status);
    EXPECT_NE(r.status, LtvStatus::LtvValidated);
}

TEST_F(LtvResponderTest, CrlAtExactExpiryBoundary)
{
    time_t now = time(nullptr);
    // JUST INSIDE the expiry boundary. A literal nextUpdate == now cannot be
    // asserted without a race (X509_cmp_current_time re-reads the clock, and
    // any elapsed second flips the comparison), so the inside case sits one
    // minute in the future — still a boundary case, but deterministic.
    auto crl = buildCrlDer(issuerKey_, issuerCert_, now - 86400, now + 60);
    ASSERT_FALSE(crl.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {}, {crl});
    EXPECT_EQ(r.status, LtvStatus::LtvValidated)
        << "An unexpired CRL from the signer's issuer that does NOT list the "
           "signer is valid revocation evidence; got " << shown(r.status);
}

TEST_F(LtvResponderTest, CrlJustPastExpiryRejected)
{
    time_t now = time(nullptr);
    // The other side of the same boundary: nextUpdate one minute in the past.
    auto crl = buildCrlDer(issuerKey_, issuerCert_, now - 86400, now - 60);
    ASSERT_FALSE(crl.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {}, {crl});
    // PHASE-50 (F53): was NoDssData. The CRL was there and was refused.
    EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
        << "An EXPIRED CRL must not count as revocation evidence — accepting "
           "it would let a stale list vouch for a since-revoked cert; got "
        << shown(r.status);
    EXPECT_NE(r.status, LtvStatus::LtvValidated);
}

TEST_F(LtvResponderTest, MixedOcspGoodAndCrlEmpty)
{
    time_t now = time(nullptr);
    auto ocsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                              now, now + 86400);
    auto crl = buildCrlDer(issuerKey_, issuerCert_, now, now + 7 * 86400);
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {ocsp}, {crl});
    EXPECT_EQ(r.status, LtvStatus::LtvValidated)
        << "A fresh GOOD OCSP plus a current CRL that does not list the "
           "signer must LTV-validate; got " << shown(r.status);
}

TEST_F(LtvResponderTest, EmptyOcspAndEmptyCrlProducesNoDssData)
{
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {}, {});
    EXPECT_EQ(r.status, LtvStatus::NoDssData)
        << "A trusted chain with NO revocation evidence at all is exactly the "
           "NoDssData case — not LtvValidated, not UntrustedChain; got "
        << shown(r.status);
}

TEST_F(LtvResponderTest, MalformedOcspBlobIgnored)
{
    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF};
    auto doc = Document::open(ctx_, signedPath_);
    LtvVerifyResult r;
    EXPECT_NO_THROW({ r = verifyTrusted(doc.get(), {garbage}, {}); })
        << "Malformed OCSP blob must not crash";
    // PHASE-50 (F53): was NoDssData. Four junk bytes in /DSS are evidence
    // that was supplied and refused, not evidence that was absent.
    EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
        << "An unparseable OCSP blob must be DISCARDED, not counted as "
           "evidence — otherwise four junk bytes would grant full LTV; got "
        << shown(r.status);
    EXPECT_NE(r.status, LtvStatus::LtvValidated);
}

TEST_F(LtvResponderTest, MalformedCrlBlobIgnored)
{
    std::vector<uint8_t> garbage = {0xCA, 0xFE, 0xBA, 0xBE};
    auto doc = Document::open(ctx_, signedPath_);
    LtvVerifyResult r;
    EXPECT_NO_THROW({ r = verifyTrusted(doc.get(), {}, {garbage}); })
        << "Malformed CRL blob must not crash";
    // PHASE-50 (F53): was NoDssData — see MalformedOcspBlobIgnored.
    EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
        << "An unparseable CRL blob must be DISCARDED, not counted as "
           "evidence; got " << shown(r.status);
    EXPECT_NE(r.status, LtvStatus::LtvValidated);
}

// A1-62 addition: the OTHER side of the classification — a REVOKED OCSP must
// beat a fully trusted chain. Without this, a mutation that simply never
// returns Revoked (e.g. treating REVOKED as "no evidence") would leave every
// test above green: they all expect LtvValidated, NoDssData or
// RevocationEvidenceRejected (phase-50 F53 added the third).
TEST_F(LtvResponderTest, RevokedOcspOverridesTrustedChain)
{
    time_t now = time(nullptr);
    auto ocsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_REVOKED,
                              now - 3600, now + 86400);
    ASSERT_FALSE(ocsp.empty());
    auto doc = Document::open(ctx_, signedPath_);
    auto r = verifyTrusted(doc.get(), {ocsp}, {});
    EXPECT_EQ(r.status, LtvStatus::Revoked)
        << "A REVOKED status from an authorized responder is a hard fail "
           "regardless of chain trust; got " << shown(r.status);
}

// SECURITY (RFC 6960 §4.2.2.2): a GOOD OCSP status is only trustworthy when the
// response is signed by an authorized responder — the target's issuer, or a
// delegate the issuer designated. A rogue responder (unrelated self-signed key)
// that forges a GOOD status must NOT be accepted as revocation evidence.
//
// Oracle: trust the signer cert as an anchor so the signer chain validates and
// the OCSP verdict becomes decisive. A LEGIT OCSP (signed by the real signer,
// which is its own issuer here) then reaches LtvValidated; the FORGED OCSP —
// identical GOOD status, same target, but signed by a rogue key — must be
// rejected and fall back to RevocationEvidenceRejected (phase-50 F53; it was
// NoDssData before that, and the forgery is exactly the case F53 exists to
// name). If responder authentication were absent
// (the pre-fix behaviour), BOTH would reach LtvValidated and this test fails.
TEST_F(LtvResponderTest, ForgedGoodOcspFromRogueResponderRejected)
{
    time_t now = time(nullptr);

    // The signer cert is trusted as an anchor by the fixture SetUp, so its
    // (self-issued) chain validates and the OCSP verdict is decisive.
    ASSERT_NE(anchor_, nullptr);
    ASSERT_TRUE(anchor_->added) << "Failed to install test trust anchor";

    // (a) Baseline: a legitimately-signed GOOD OCSP must reach full LTV, proving
    //     the trust setup is correct and an AUTHORIZED responder is accepted.
    {
        auto legit = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                  now - 3600, now + 86400);
        ASSERT_FALSE(legit.empty());
        auto doc = Document::open(ctx_, signedPath_);
        auto r = LtvValidator::verifyWith(doc.get(), certChain(), {legit}, {},
                                          /*useTrustAnchors=*/true);
        EXPECT_EQ(r.status, LtvStatus::LtvValidated)
            << "An authorized GOOD OCSP over a trusted signer must LTV-validate; "
               "got " << ltvStatusToDisplayString(r.status).toStdString();
    }

    // (b) Attack: same target, same GOOD status, but signed by a rogue key whose
    //     cert is neither the issuer nor a designated delegate. Must be rejected.
    {
        EVP_PKEY* rogueKey = nullptr; X509* rogueCert = nullptr;
        ASSERT_TRUE(makeRogueSigner(rogueKey, rogueCert));
        auto forged = buildOcspDerSignedBy(cert_, issuerCert_, rogueKey, rogueCert,
                                           V_OCSP_CERTSTATUS_GOOD,
                                           now - 3600, now + 86400);
        EVP_PKEY_free(rogueKey);
        X509_free(rogueCert);
        ASSERT_FALSE(forged.empty());

        auto doc = Document::open(ctx_, signedPath_);
        auto r = LtvValidator::verifyWith(doc.get(), certChain(), {forged}, {},
                                          /*useTrustAnchors=*/true);
        EXPECT_NE(r.status, LtvStatus::LtvValidated)
            << "A forged GOOD OCSP from a rogue responder must NOT LTV-validate "
               "(revocation bypass)";
        // PHASE-50 (F53). This assertion USED to read NoDssData, and that is
        // precisely the defect F53 names: a document carrying an ACTIVELY
        // FORGED OCSP response rendered identically to one carrying no
        // revocation data at all. The forgery is the single most important
        // thing PDFE learns about this document and it was being discarded.
        EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
            << "a forged OCSP must be reported as REFUSED evidence, not as "
               "absent evidence; got "
            << ltvStatusToDisplayString(r.status).toStdString();
        EXPECT_TRUE(r.message.contains("responder authorisation"))
            << "the message must name the rule that caught the forgery; "
               "message was: " << r.message.toStdString();
        EXPECT_TRUE(r.message.contains("rogue responder"))
            << "message was: " << r.message.toStdString();
    }
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0005 (follow-up phase 42).
//
// The INSTANCE test above (ForgedGoodOcspFromRogueResponderRejected) presents a
// rogue key on exactly ONE of the artifact types the LTV validator consumes:
// the OCSP response. DCA 0005 names the class as *"a security check whose
// absence is masked because no test supplies an adversarial input that the
// missing check would have caught"* — and that class is about the ADVERSARIAL
// INPUT being absent, not about OCSP. Any evidence artifact the validator reads
// out of an attacker-writable /DSS has the same exposure.
//
// THE CLASS-LEVEL FORM enumerates the evidence types as a corpus and presents
// a rogue-signed instance of EACH, with the genuine article alongside as a
// positive control. Without the controls, "reject everything" would pass; with
// them, each row pins the discrimination the check is supposed to make.
//
// WHAT IT CAUGHT (phase-42): the CRL rows were RED. isCertRevokedByCrl() and
// hasValidCrlForCert() authenticated a CRL by comparing its issuer DN to the
// signer's issuer DN — and nothing else. No X509_CRL_verify anywhere. A DN is
// attacker-chosen text, so anyone able to place bytes in /DSS could mint a CRL
// bearing the real issuer's name and have it accepted as revocation evidence
// (→ LtvValidated on a possibly-revoked certificate), or accepted as proof of
// revocation (→ a forged Revoked verdict on a good signature). That is the
// identical defect DCA 0005 fixed for OCSP, surviving in the sibling artifact
// because the fix was scoped to the one artifact the review named. Fixed in
// src/signatures/LtvValidator.cpp (crlIsAuthorized) as part of this phase.
//
// RED-ness: delete the crlIsAuthorized() call from either CRL consumer and the
// corresponding rogue-CRL row flips; delete the ocspResponseIsAuthorized() call
// and the rogue-OCSP row flips. The positive-control rows stay green in every
// case, so a fix that simply rejects all evidence cannot pass this test.
TEST_F(LtvResponderTest, RogueKeyEvidenceRejectedForEveryEvidenceType)
{
    const time_t now = time(nullptr);

    EVP_PKEY* rogueKey = nullptr;
    X509* rogueCert = nullptr;
    ASSERT_TRUE(makeRogueSigner(rogueKey, rogueCert));
    struct RogueGuard {
        EVP_PKEY* k; X509* c;
        ~RogueGuard() { if (k) EVP_PKEY_free(k); if (c) X509_free(c); }
    } rogueGuard{rogueKey, rogueCert};

    X509_NAME* realIssuerName = X509_get_issuer_name(cert_);
    ASSERT_NE(realIssuerName, nullptr);
    const ASN1_INTEGER* signerSerial = X509_get0_serialNumber(cert_);
    ASSERT_NE(signerSerial, nullptr);

    // --- the corpus -----------------------------------------------------
    // Genuine OCSP: GOOD for the signer, signed by the signer's own issuer.
    auto genuineOcsp = buildOcspDer(issuerKey_, issuerCert_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                     now - 3600, now + 86400);
    ASSERT_FALSE(genuineOcsp.empty());
    // Rogue OCSP: identical claim, signed by an unrelated self-signed key.
    auto rogueOcsp = buildOcspDerSignedBy(cert_, issuerCert_, rogueKey, rogueCert,
                                           V_OCSP_CERTSTATUS_GOOD,
                                           now - 3600, now + 86400);
    ASSERT_FALSE(rogueOcsp.empty());

    // Genuine CRL: real issuer DN, signed by the real issuer key, in date,
    // signer not listed.
    auto genuineCrl = buildCrlDerAs(realIssuerName, issuerKey_, now - 3600,
                                     now + 86400, nullptr);
    ASSERT_FALSE(genuineCrl.empty());
    // Rogue CRL #1 — forged "still valid" evidence: the real issuer's DN, in
    // date, signer NOT listed, signed by the rogue key.
    auto rogueCrlEmpty = buildCrlDerAs(realIssuerName, rogueKey, now - 3600,
                                        now + 86400, nullptr);
    ASSERT_FALSE(rogueCrlEmpty.empty());
    // Rogue CRL #2 — forged REVOCATION: same, but listing the signer's serial.
    auto rogueCrlRevoking = buildCrlDerAs(realIssuerName, rogueKey, now - 3600,
                                           now + 86400, signerSerial);
    ASSERT_FALSE(rogueCrlRevoking.empty());

    struct EvidenceRow {
        const char* name;
        std::vector<std::vector<uint8_t>> ocsps;
        std::vector<std::vector<uint8_t>> crls;
        LtvStatus expected;
        const char* why;
    };
    const std::vector<EvidenceRow> corpus = {
        // Positive controls — the authorized article must still be honoured.
        {"OCSP/genuine", {genuineOcsp}, {}, LtvStatus::LtvValidated,
         "a GOOD OCSP signed by the signer's own issuer is authorized evidence; "
         "if this row fails the check is rejecting everything, which is not a fix"},
        {"CRL/genuine", {}, {genuineCrl}, LtvStatus::LtvValidated,
         "an in-date CRL issued and SIGNED by the signer's issuer is authorized "
         "evidence; if this row fails the CRL check rejects legitimate CRLs"},
        // Adversarial rows — one per evidence type.
        {"OCSP/rogue-signed", {rogueOcsp}, {},
         LtvStatus::RevocationEvidenceRejected,
         "a GOOD OCSP signed by a key that is neither the issuer nor an "
         "id-kp-OCSPSigning delegate is not evidence (RFC 6960 §4.2.2.2)"},
        {"CRL/rogue-signed-empty", {}, {rogueCrlEmpty},
         LtvStatus::RevocationEvidenceRejected,
         "a CRL carrying the real issuer's DN but signed by an unrelated key is "
         "not evidence (RFC 5280 §6.3.3). Honouring it lets an attacker who can "
         "write /DSS manufacture 'this certificate is not revoked'"},
        {"CRL/rogue-signed-revoking", {}, {rogueCrlRevoking},
         LtvStatus::RevocationEvidenceRejected,
         "a rogue-signed CRL listing the signer's serial must not produce a "
         "Revoked verdict either — forging a revocation is the mirror-image "
         "attack on the same missing signature check"},
    };

    ASSERT_EQ(corpus.size(), 5u)
        << "the corpus is the test — every evidence artifact the validator "
           "reads out of /DSS needs a genuine row AND a rogue-signed row";

    for (const auto& row : corpus) {
        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr) << row.name;
        auto r = LtvValidator::verifyWith(doc.get(), certChain(), row.ocsps,
                                          row.crls, /*useTrustAnchors=*/true);
        EXPECT_EQ(r.status, row.expected)
            << "[" << row.name << "] expected "
            << ltvStatusToDisplayString(row.expected).toStdString()
            << ", got " << ltvStatusToDisplayString(r.status).toStdString()
            << " — " << row.why;
    }
}

// ═════════════════════════════════════════════════════════════════════
// THE ROW THE CORPUS ABOVE WAS MISSING (phase-42 remediation).
//
// RogueKeyEvidenceRejectedForEveryEvidenceType states in its own header that
// the threat is "anyone able to place bytes in /DSS" — and then never places
// any. It passes `certChain()` (the genuine signer cert) as the embedded-cert
// set, so the validator's issuer-candidate pool contains only honest certs and
// the rogue key can never be found as an authorizing key. **The test's threat
// model was narrower than the one its own comment claimed**, which is this
// programme's signature failure: evidence that could not establish the claim.
//
// The real attacker controls /DSS ENTIRELY — /Certs as well as /CRLs and
// /OCSPs. `DssReader::readDssEmbedded` reads /DSS /Certs verbatim with no trust
// filtering, and the revocation step pooled those certs as authorizing keys
// (for CRLs) and as TRUST ANCHORS (for OCSP_basic_verify). So:
//
//   1. mint a self-signed rogue cert whose SUBJECT DN == the real issuer's DN;
//   2. put it in the embedded-cert set (= /DSS /Certs);
//   3. sign forged revocation evidence with the rogue key.
//
// The first phase-42 CRL fix did not stop this: it verified the CRL signature
// against a key it resolved BY DN, and the attacker supplied that key too.
//
// The forged-REVOCATION case is the worst of the two, and is asserted first:
// revocation is classified BEFORE the chain-trust check, so it needs no valid
// chain at all — ANY signed PDF could be forced to `Revoked`.
//
// Fixed by certIssuedSigner(): a candidate may authorize revocation data only
// if its key actually verifies the SIGNER CERTIFICATE's signature. A DN is
// attacker-chosen; a signature is not.
TEST_F(LtvResponderTest, RogueCertPlantedInDssCannotAuthorizeEvidence)
{
    const time_t now = time(nullptr);

    // The attacker's cert: self-signed, but wearing the real issuer's NAME.
    EVP_PKEY* rogueKey = EVP_EC_gen("P-256");
    ASSERT_NE(rogueKey, nullptr);
    X509* rogueCert = X509_new();
    ASSERT_NE(rogueCert, nullptr);
    struct Guard {
        EVP_PKEY* k; X509* c;
        ~Guard() { if (k) EVP_PKEY_free(k); if (c) X509_free(c); }
    } guard{rogueKey, rogueCert};

    X509_set_version(rogueCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(rogueCert), 0x7A7A);
    X509_gmtime_adj(X509_getm_notBefore(rogueCert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(rogueCert), 60 * 60 * 24);
    X509_set_pubkey(rogueCert, rogueKey);
    // THE ATTACK: impersonate the real issuer's distinguished name exactly.
    X509_NAME* realIssuerName = X509_get_issuer_name(cert_);
    ASSERT_NE(realIssuerName, nullptr);
    ASSERT_EQ(X509_set_subject_name(rogueCert, realIssuerName), 1);
    ASSERT_EQ(X509_set_issuer_name(rogueCert, realIssuerName), 1);
    ASSERT_NE(X509_sign(rogueCert, rogueKey, EVP_sha256()), 0);

    // Sanity: the rogue cert really does carry the issuer's DN, so a DN-keyed
    // lookup WOULD find it. Without this the test could pass for the wrong
    // reason (a DN that never matched in the first place).
    ASSERT_EQ(X509_NAME_cmp(X509_get_subject_name(rogueCert), realIssuerName), 0)
        << "the rogue cert does not actually impersonate the issuer DN — this "
           "test would then prove nothing";

    // The attacker-controlled /DSS /Certs: the genuine signer cert PLUS the
    // planted rogue. Both are read verbatim by DssReader.
    auto dssCerts = certChain();
    {
        unsigned char* d = nullptr;
        const int l = i2d_X509(rogueCert, &d);
        ASSERT_GT(l, 0);
        dssCerts.emplace_back(d, d + l);
        OPENSSL_free(d);
    }

    const ASN1_INTEGER* signerSerial = X509_get0_serialNumber(cert_);
    ASSERT_NE(signerSerial, nullptr);

    // ---- 1. Forged REVOCATION via a rogue-signed CRL (unconditional) ----
    {
        auto forgedCrl = buildCrlDerAs(realIssuerName, rogueKey, now - 3600,
                                        now + 86400, signerSerial);
        ASSERT_FALSE(forgedCrl.empty());

        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto r = LtvValidator::verifyWith(doc.get(), dssCerts, {}, {forgedCrl},
                                          /*useTrustAnchors=*/true);
        EXPECT_NE(r.status, LtvStatus::Revoked)
            << "a CRL signed by a cert the ATTACKER planted in /DSS forced a "
               "Revoked verdict. Revocation is classified before the chain "
               "check, so this needs no valid chain — any signed PDF can be "
               "smeared as revoked. The authorizing key must be a PROVEN "
               "issuer of the signer, not a DN match.";
    }

    // ---- 2. Forged NON-revocation via a rogue-signed CRL ----
    {
        auto forgedCrl = buildCrlDerAs(realIssuerName, rogueKey, now - 3600,
                                        now + 86400, nullptr);
        ASSERT_FALSE(forgedCrl.empty());

        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto r = LtvValidator::verifyWith(doc.get(), dssCerts, {}, {forgedCrl},
                                          /*useTrustAnchors=*/true);
        EXPECT_NE(r.status, LtvStatus::LtvValidated)
            << "a CRL signed by an attacker-planted /DSS cert was accepted as "
               "valid revocation evidence, so a genuinely revoked certificate "
               "reads as LTV-validated (revocation bypass).";
    }

    // ---- 3. Forged GOOD OCSP signed by the planted rogue ----
    // Pre-fix this was worse than the CRL case: the planted cert was added to
    // the X509_STORE handed to OCSP_basic_verify, i.e. promoted to a TRUST
    // ANCHOR by the document that contained it.
    {
        auto forgedOcsp = buildOcspDerSignedBy(cert_, issuerCert_, rogueKey, rogueCert,
                                                V_OCSP_CERTSTATUS_GOOD,
                                                now - 3600, now + 86400);
        ASSERT_FALSE(forgedOcsp.empty());

        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto r = LtvValidator::verifyWith(doc.get(), dssCerts, {forgedOcsp}, {},
                                          /*useTrustAnchors=*/true);
        EXPECT_NE(r.status, LtvStatus::LtvValidated)
            << "a GOOD OCSP signed by an attacker-planted /DSS cert was "
               "honoured — the cert the document supplied was used as the "
               "trust anchor that authenticated it.";
    }

    // ---- 4. POSITIVE CONTROL: planting a rogue must not break honest evidence.
    // The genuine issuer cert is still in /DSS alongside the rogue, so genuine
    // evidence must still validate. Without this row, a "fix" that ignored all
    // revocation evidence whenever /DSS contained anything unexpected would
    // pass rows 1-3.
    {
        auto genuineCrl = buildCrlDerAs(realIssuerName, issuerKey_, now - 3600,
                                         now + 86400, nullptr);
        ASSERT_FALSE(genuineCrl.empty());

        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto r = LtvValidator::verifyWith(doc.get(), dssCerts, {}, {genuineCrl},
                                          /*useTrustAnchors=*/true);
        EXPECT_EQ(r.status, LtvStatus::LtvValidated)
            << "a CRL signed by the REAL issuer stopped validating once a rogue "
               "cert was present in /DSS. The fix must reject the impostor, not "
               "abandon revocation checking; got "
            << ltvStatusToDisplayString(r.status).toStdString();
    }
}

// ═════════════════════════════════════════════════════════════════════
// CRL acceptability rules that did not exist before phase-42's remediation.
// Each row is an independently-forged CRL that the pre-remediation code would
// have accepted as valid revocation evidence. All are signed by the GENUINE
// issuer key, so the signature check cannot be what rejects them — these pin
// the structural and freshness rules specifically.
TEST_F(LtvResponderTest, StructurallyUnacceptableCrlsAreNotEvidence)
{
    const time_t now = time(nullptr);
    X509_NAME* issuerName = X509_get_issuer_name(cert_);
    ASSERT_NE(issuerName, nullptr);

    struct Row {
        const char* name;
        std::vector<uint8_t> crl;
        const char* why;
    };
    std::vector<Row> rows;

    // Positive control first: an ordinary, fresh, genuine CRL.
    rows.push_back({"genuine-fresh",
                    buildCrlDerAs(issuerName, issuerKey_, now - 3600, now + 86400, nullptr),
                    "control — a normal CRL must still be honoured"});

    // FUTURE-DATED: thisUpdate was never checked at all before phase-42.
    rows.push_back({"future-dated",
                    buildCrlDerAs(issuerName, issuerKey_, now + 30 * 86400,
                                   now + 60 * 86400, nullptr),
                    "a CRL whose thisUpdate is a month in the future is not "
                    "evidence about today; thisUpdate was previously unchecked"});

    // NO nextUpdate + ancient: previously 'never expires', valid forever.
    rows.push_back({"no-nextupdate-ancient",
                    buildCrlDerAsNoNextUpdate(issuerName, issuerKey_, now - 400 * 86400),
                    "a CRL with no nextUpdate used to never expire, so one "
                    "issued years ago counted as fresh evidence forever"});

    // DELTA CRL: lists only changes since a base, so alone it hides every
    // revocation in the base while satisfying 'we have a valid CRL'.
    rows.push_back({"delta-crl",
                    buildDeltaCrlDer(issuerName, issuerKey_, now - 3600, now + 86400),
                    "a delta CRL alone satisfied hasValidCrl while listing none "
                    "of the base CRL's revocations"});

    ASSERT_EQ(rows.size(), 4u);
    for (const auto& row : rows) {
        ASSERT_FALSE(row.crl.empty()) << row.name << ": fixture build failed";
        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr) << row.name;
        auto r = LtvValidator::verifyWith(doc.get(), certChain(), {}, {row.crl},
                                          /*useTrustAnchors=*/true);
        const bool isControl = std::string(row.name) == "genuine-fresh";
        if (isControl) {
            EXPECT_EQ(r.status, LtvStatus::LtvValidated)
                << "[" << row.name << "] " << row.why << "; got "
                << ltvStatusToDisplayString(r.status).toStdString();
        } else {
            // PHASE-50 (F53): was NoDssData. Each of these CRLs was PRESENT
            // and was refused by a named structural or freshness rule.
            EXPECT_EQ(r.status, LtvStatus::RevocationEvidenceRejected)
                << "[" << row.name << "] " << row.why << "; got "
                << ltvStatusToDisplayString(r.status).toStdString();
            EXPECT_NE(r.status, LtvStatus::LtvValidated)
                << "[" << row.name << "] must never become evidence";
        }
    }
}
