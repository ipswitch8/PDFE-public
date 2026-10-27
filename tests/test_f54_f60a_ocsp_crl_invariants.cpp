// -*- coding: utf-8 -*-
//
// Meta-audit follow-up phase-51 — F54 and F60(a).
//
// Both rows are about an INVARIANT that the code already depended on and that
// nothing enforced. Neither is a behaviour change: every assertion below passes
// against the pre-phase-51 binary as well. What phase-51 adds is the machinery
// that makes the invariant impossible to lose, and what this file adds is the
// control that makes losing it LOUD.
//
//   F54    ocspResponseIsAuthorized() turns on X509_V_FLAG_PARTIAL_CHAIN, which
//          promotes every member of its X509_STORE to trust-anchor status. That
//          is safe only because the caller pre-filtered the pool through
//          certIssuedSigner() — a real signature verification. The requirement
//          was stated in a COMMENT on a plain `const std::vector<X509*>&`, so a
//          second caller, or a refactor that moved the filter, would have
//          re-opened the phase-42 bypass and compiled clean.
//
//   F60(a) crlEntryRevokesCert() walks X509_CRL_get_REVOKED() in ENCODED order
//          because RFC 5280 §5.3.3 certificateIssuer carry-forward is defined
//          over encoded order. X509_CRL_get0_by_serial() sorts that stack IN
//          PLACE, which silently re-attributes every bare entry to whichever
//          entry the sort happens to put in front of it.
//
// ═══════════════════════════════════════════════════════════════════════
// F54 — WHY THE PRIMARY FIX CANNOT BE TESTED AT RUNTIME, AND WHAT IS INSTEAD.
//
// The hardening is a COMPILE-TIME fact: `SignerIssuerPool` has a private
// constructor and one static factory, so the expression
//
//     ocspResponseIsAuthorized(basic, std::vector<X509*>{someDssCert})
//
// no longer names an overload that exists. There is no raw-vector entry point
// left to call, which is the whole point — and a test cannot execute a call that
// does not compile. Asserting on it would need a compile-failure harness this
// project does not have, and a `#if 0` block asserts nothing.
//
// What CARRIES the security claim and IS executable is the factory's admission
// rule, because it is the only thing standing between the document's /DSS and a
// trust anchor. Both tests below drive it:
//
//   * PoolAdmitsOnlyProvenIssuersOfTheSigner  — the factory itself.
//   * RogueDssCertCannotAuthoriseAnOcspResponse — the same rule observed at the
//     point of use, through the real ocspResponseIsAuthorized().
//
// REVERT-AND-OBSERVE (F54). Change SignerIssuerPool::fromCandidates() to push
// every candidate unconditionally (i.e. delete the certIssuedSigner() guard) and
// BOTH tests go red. See the phase-51 report for the verbatim output.
// ═══════════════════════════════════════════════════════════════════════

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "support/CrlChainFixture.h"

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <ctime>
#include <type_traits>
#include <vector>

namespace cc = pdfe_test::crlchain;

// COMPILE-TIME NOTE, standing in for the compile-failure harness this project
// lacks. The two seams below are the ONLY declared entry points to
// SignerIssuerPool from outside LtvValidator.cpp, and both take bytes. If a
// future edit reintroduces a `std::vector<X509*>` route into the OCSP
// authorisation path, it cannot do so through this header without changing one
// of these signatures — which breaks this static_assert and forces the author
// past the F54 rationale above.
static_assert(
    std::is_same_v<
        decltype(&pdfe::signatures::detail::signerIssuerPoolAdmittedIndices),
        std::vector<int> (*)(const std::vector<std::vector<uint8_t>>&,
                             const std::vector<uint8_t>&)>,
    "F54: the SignerIssuerPool seam must take DER bytes, never a raw X509* pool");
static_assert(
    std::is_same_v<
        decltype(&pdfe::signatures::detail::ocspAuthorizedForSigner),
        bool (*)(const std::vector<uint8_t>&,
                 const std::vector<std::vector<uint8_t>>&,
                 const std::vector<uint8_t>&)>,
    "F54: the OCSP authorisation seam must take DER bytes, never a raw X509* pool");

namespace {

// ---------------------------------------------------------------------------
// Small OpenSSL RAII, local to this file. tests/support/CrlChainFixture.h owns
// the shared CRL-minting and corpus-loading helpers and is reused verbatim; the
// certificate minting below is deliberately NOT copied there, because F60(a)
// needs serial numbers chosen so that a sort demonstrably reorders the entries,
// which no other suite wants.
struct SynthCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    SynthCert() = default;
    SynthCert(const SynthCert&) = delete;
    SynthCert& operator=(const SynthCert&) = delete;
    ~SynthCert()
    {
        if (key) EVP_PKEY_free(key);
        if (cert) X509_free(cert);
    }
    std::vector<uint8_t> der() const
    {
        std::vector<uint8_t> out;
        unsigned char* d = nullptr;
        const int l = i2d_X509(cert, &d);
        if (l > 0 && d) { out.assign(d, d + l); OPENSSL_free(d); }
        return out;
    }
};

// Phase-51 review, test hygiene. The F60 test below builds a standalone
// ASN1_INTEGER and X509_NAME and used to free them on the last two lines of the
// body. Every ASSERT_* between creation and those frees is a `return` — so any
// one of them firing leaked both. These two holders make the frees unmissable,
// in the same shape as SynthCert above.
struct OwnedAsn1Integer {
    ASN1_INTEGER* p = ASN1_INTEGER_new();
    OwnedAsn1Integer() = default;
    OwnedAsn1Integer(const OwnedAsn1Integer&) = delete;
    OwnedAsn1Integer& operator=(const OwnedAsn1Integer&) = delete;
    ~OwnedAsn1Integer() { if (p) ASN1_INTEGER_free(p); }
};

struct OwnedX509Name {
    X509_NAME* p = X509_NAME_new();
    OwnedX509Name() = default;
    OwnedX509Name(const OwnedX509Name&) = delete;
    OwnedX509Name& operator=(const OwnedX509Name&) = delete;
    ~OwnedX509Name() { if (p) X509_NAME_free(p); }
};

// Mint a CA certificate named @p cn with serial @p serial. Self-signed when
// @p issuer is null, otherwise really signed by @p issuer.
//
// keyUsage is OMITTED on purpose: X509_get_key_usage() returns UINT32_MAX for an
// absent extension, which RFC 5280 §4.2.1.3 reads as "unconstrained", so these
// CAs may sign CRLs. That is the rule crlIsAuthorized() implements, and it keeps
// the F60(a) fixture about entry ORDER rather than about keyUsage.
bool mintCa(SynthCert& out, const char* cn, const SynthCert* issuer, long serial)
{
    out.key = EVP_EC_gen("P-256");
    if (!out.key) { ERR_clear_error(); return false; }
    out.cert = X509_new();
    if (!out.cert) return false;

    X509_set_version(out.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(out.cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(out.cert), -86400);
    X509_gmtime_adj(X509_getm_notAfter(out.cert), 365 * 86400);
    X509_set_pubkey(out.cert, out.key);

    X509_NAME* subject = X509_get_subject_name(out.cert);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1,
                               -1, 0);
    X509_set_issuer_name(out.cert,
                         issuer ? X509_get_subject_name(issuer->cert) : subject);

    X509_EXTENSION* bc = X509V3_EXT_conf_nid(nullptr, nullptr,
                                             NID_basic_constraints,
                                             "critical,CA:TRUE");
    if (bc) { X509_add_ext(out.cert, bc, -1); X509_EXTENSION_free(bc); }

    if (X509_sign(out.cert, issuer ? issuer->key : out.key, EVP_sha256()) == 0) {
        ERR_clear_error();
        return false;
    }
    return true;
}

// A ROGUE certificate: a fresh self-signed cert that WEARS @p stolenSubject —
// the exact DN of a real CA — while holding a key that never signed anything in
// the real chain. This is the /DSS-planted certificate the phase-42 bypass used,
// and it is the only interesting input to certIssuedSigner(): it passes the
// cheap DN gate and must be refused by the signature check.
bool mintRogueWearing(SynthCert& out, X509_NAME* stolenSubject)
{
    out.key = EVP_EC_gen("P-256");
    if (!out.key) { ERR_clear_error(); return false; }
    out.cert = X509_new();
    if (!out.cert) return false;

    X509_set_version(out.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(out.cert), 0x7F54);
    X509_gmtime_adj(X509_getm_notBefore(out.cert), -86400);
    X509_gmtime_adj(X509_getm_notAfter(out.cert), 365 * 86400);
    X509_set_pubkey(out.cert, out.key);
    X509_set_subject_name(out.cert, stolenSubject);  // copies
    X509_set_issuer_name(out.cert, stolenSubject);   // self-signed

    X509_EXTENSION* bc = X509V3_EXT_conf_nid(nullptr, nullptr,
                                             NID_basic_constraints,
                                             "critical,CA:TRUE");
    if (bc) { X509_add_ext(out.cert, bc, -1); X509_EXTENSION_free(bc); }

    if (X509_sign(out.cert, out.key, EVP_sha256()) == 0) {
        ERR_clear_error();
        return false;
    }
    return true;
}

// An OCSP response ABOUT @p target whose CertID names @p certIdIssuer, signed by
// @p respKey / @p respCert. Decoupling the CertID issuer from the responder is
// what lets the rogue case be shaped the way a real attack would be: the rogue
// signs the response AND is named as the CertID issuer, so OpenSSL's
// "is the responder the CA that issued the target" test is satisfied INTERNALLY
// and the only thing left standing between the forgery and a GOOD status is
// whether the rogue is in the trust store — i.e. whether it got into the pool.
std::vector<uint8_t> buildOcspDer(X509* target, X509* certIdIssuer,
                                  X509* respCert, EVP_PKEY* respKey, int status)
{
    std::vector<uint8_t> result;
    const time_t now = time(nullptr);
    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    OCSP_CERTID* cid = OCSP_cert_to_id(EVP_sha1(), target, certIdIssuer);
    ASN1_TIME* t1 = ASN1_TIME_set(nullptr, now - 3600);
    ASN1_TIME* t2 = ASN1_TIME_set(nullptr, now + 7 * 86400);
    if (basic && cid && t1 && t2) {
        OCSP_basic_add1_status(basic, cid, status, 0, nullptr, t1, t2);
        if (OCSP_basic_sign(basic, respCert, respKey, EVP_sha256(), nullptr, 0)
            != 1) {
            ERR_clear_error();
        } else if (OCSP_RESPONSE* resp = OCSP_response_create(
                       OCSP_RESPONSE_STATUS_SUCCESSFUL, basic)) {
            unsigned char* der = nullptr;
            const int len = i2d_OCSP_RESPONSE(resp, &der);
            OCSP_RESPONSE_free(resp);
            if (len > 0 && der) { result.assign(der, der + len); OPENSSL_free(der); }
        }
    }
    if (cid) OCSP_CERTID_free(cid);
    if (t1) ASN1_TIME_free(t1);
    if (t2) ASN1_TIME_free(t2);
    if (basic) OCSP_BASICRESP_free(basic);
    return result;
}

// The committed two-level corpus: root -> inter_crlsign -> leaf_crlsign.
struct Corpus {
    X509* root = nullptr;
    X509* inter = nullptr;
    X509* leaf = nullptr;
    EVP_PKEY* interKey = nullptr;
    std::vector<uint8_t> rootDer, interDer, leafDer;
    ~Corpus()
    {
        if (root) X509_free(root);
        if (inter) X509_free(inter);
        if (leaf) X509_free(leaf);
        if (interKey) EVP_PKEY_free(interKey);
    }
};

bool loadCorpus(Corpus& c)
{
    c.root = cc::loadDerCert(cc::chainDir() / "root.der");
    c.inter = cc::loadDerCert(cc::chainDir() / "inter_crlsign.der");
    c.leaf = cc::loadDerCert(cc::chainDir() / "leaf_crlsign.der");
    c.interKey = cc::loadPemKey(cc::chainDir() / "inter_crlsign_key.pem");
    c.rootDer = cc::readAllBytes(cc::chainDir() / "root.der");
    c.interDer = cc::readAllBytes(cc::chainDir() / "inter_crlsign.der");
    c.leafDer = cc::readAllBytes(cc::chainDir() / "leaf_crlsign.der");
    return c.root && c.inter && c.leaf && c.interKey && !c.rootDer.empty()
           && !c.interDer.empty() && !c.leafDer.empty();
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// F54, control 1 — the factory's admission rule.
//
// Four candidates are offered against the leaf, and exactly ONE of them issued
// it. The other three are the three ways a candidate can be wrong:
//
//   root  — a real, trusted CA that simply is not this leaf's issuer (wrong DN).
//   rogue — a self-signed certificate wearing the intermediate's EXACT subject
//           DN. It passes the cheap DN gate and fails the signature check. This
//           is the /DSS-planted certificate from the phase-42 bypass, and it is
//           the only candidate whose rejection needs cryptography.
//   leaf  — the signer offered as its own issuer, which verifyWith() really does
//           push into the candidate list (a genuinely self-signed signer is
//           supposed to survive; this one is not self-signed and must not).
//
// If the certIssuedSigner() guard is removed from
// SignerIssuerPool::fromCandidates(), all four are admitted and this fails.
// ═══════════════════════════════════════════════════════════════════════
TEST(F54SignerIssuerPool, PoolAdmitsOnlyProvenIssuersOfTheSigner)
{
    Corpus c;
    ASSERT_TRUE(loadCorpus(c))
        << "tests/fixtures/crl_chain/ is committed; a missing member is a lost "
           "fixture, not an optional environment feature";

    SynthCert rogue;
    ASSERT_TRUE(mintRogueWearing(rogue, X509_get_subject_name(c.inter)));
    const auto rogueDer = rogue.der();
    ASSERT_FALSE(rogueDer.empty());

    // Fixture self-check: the rogue really does wear the intermediate's DN, so
    // it really does reach the signature check rather than being dismissed on
    // the name. Without this the test could pass for the wrong reason.
    ASSERT_EQ(X509_NAME_cmp(X509_get_subject_name(rogue.cert),
                            X509_get_subject_name(c.inter)), 0)
        << "the rogue no longer impersonates the intermediate's DN, so it is "
           "refused by the cheap name gate and proves nothing about the "
           "signature check";

    const std::vector<std::vector<uint8_t>> candidates = {
        c.rootDer,   // 0 — real CA, wrong DN for this leaf
        c.interDer,  // 1 — the genuine issuer
        rogueDer,    // 2 — right DN, wrong key
        c.leafDer,   // 3 — the signer itself, not self-signed
    };

    const std::vector<int> admitted =
        pdfe::signatures::detail::signerIssuerPoolAdmittedIndices(candidates,
                                                                  c.leafDer);

    EXPECT_EQ(admitted, (std::vector<int>{1}))
        << "SignerIssuerPool admitted something other than exactly the proven "
           "issuer. Every member of this pool becomes a TRUST ANCHOR inside "
           "ocspResponseIsAuthorized() (X509_V_FLAG_PARTIAL_CHAIN), so an "
           "admitted rogue is a complete OCSP revocation bypass.";
}

// ═══════════════════════════════════════════════════════════════════════
// F54, control 2 — the same rule observed at the point of use.
//
// Control 1 could in principle pass while the pool were bypassed somewhere
// downstream, so this drives the real ocspResponseIsAuthorized() through the
// detail seam.
//
// THE POSITIVE CONTROL IS LOAD-BEARING. "Never authorise anything" would pass
// the rogue half on its own; the genuine half is what makes the discrimination,
// rather than the refusal, the thing being pinned.
//
// THE SCENARIO IS THE ONE PHASE-42 CLOSED, and its shape is forced by how OpenSSL
// builds a CertID — this was MEASURED against the mutated build, not assumed:
//
//   * OCSP_cert_to_id(md, target, issuer) takes the issuer NAME hash from the
//     TARGET's issuer field and only the KEY hash from `issuer`. A rogue with its
//     own distinct DN therefore can NEVER satisfy ocsp_check_issuer(), so such a
//     rogue is refused by OpenSSL itself whether or not it is in the trust store.
//     A test built on one is decoration: it stays green under the mutation.
//   * The rogue must therefore STEAL the intermediate's subject DN — which is
//     exactly what the phase-42 attack does, since /DSS /Certs is copied verbatim
//     out of the PDF.
//   * And it must REPLACE the genuine intermediate in /DSS rather than sit
//     alongside it. With both present the store holds two certificates with the
//     same subject and OpenSSL's chain builder picks the genuine one as the
//     rogue's issuer, failing the signature check — again refusing for its own
//     reasons rather than PDFE's. Replacing the issuer cert is also the more
//     realistic attack: the attacker controls the whole /DSS.
//
// With that shape, the ONLY thing standing between the forged GOOD status and
// acceptance is whether the rogue reached the trust store — i.e. whether
// SignerIssuerPool admitted it. Under the mutation it does, and the forgery is
// honoured.
// ═══════════════════════════════════════════════════════════════════════
TEST(F54SignerIssuerPool, RogueDssCertCannotAuthoriseAnOcspResponse)
{
    Corpus c;
    ASSERT_TRUE(loadCorpus(c));

    SynthCert rogue;
    ASSERT_TRUE(mintRogueWearing(rogue, X509_get_subject_name(c.inter)));
    const auto rogueDer = rogue.der();
    ASSERT_FALSE(rogueDer.empty());
    ASSERT_EQ(X509_NAME_cmp(X509_get_subject_name(rogue.cert),
                            X509_get_subject_name(c.inter)), 0)
        << "the rogue must wear the intermediate's DN or its forged CertID "
           "cannot match, and the refusal below would be OpenSSL's rather than "
           "SignerIssuerPool's";

    // Positive control: the genuine /DSS — the real intermediate answers for its
    // own leaf.
    const std::vector<std::vector<uint8_t>> honestDss = {
        c.rootDer, c.interDer, c.leafDer};
    const auto genuineOcsp = buildOcspDer(c.leaf, c.inter, c.inter, c.interKey,
                                          V_OCSP_CERTSTATUS_GOOD);
    ASSERT_FALSE(genuineOcsp.empty());
    EXPECT_TRUE(pdfe::signatures::detail::ocspAuthorizedForSigner(
        genuineOcsp, honestDss, c.leafDer))
        << "a response signed by the leaf's real issuer must authenticate — if "
           "this is false the negative assertion below is vacuous, because a "
           "function that always answers false would satisfy it";

    // The attack: /DSS carries the rogue IN PLACE OF the genuine intermediate,
    // and the rogue both signs the response and supplies the CertID key hash.
    const std::vector<std::vector<uint8_t>> forgedDss = {
        c.rootDer, rogueDer, c.leafDer};
    const auto rogueOcsp = buildOcspDer(c.leaf, rogue.cert, rogue.cert,
                                        rogue.key, V_OCSP_CERTSTATUS_GOOD);
    ASSERT_FALSE(rogueOcsp.empty());
    EXPECT_FALSE(pdfe::signatures::detail::ocspAuthorizedForSigner(
        rogueOcsp, forgedDss, c.leafDer))
        << "a document-supplied certificate that never proved it issued the "
           "signer authorised an OCSP response. With "
           "X509_V_FLAG_PARTIAL_CHAIN that certificate is a trust anchor, so "
           "the document can forge any revocation status it likes.";
}

// ═══════════════════════════════════════════════════════════════════════
// F60(a) — ENCODED ENTRY ORDER IS LOAD-BEARING; A SORT DESTROYS ATTRIBUTION.
//
// RFC 5280 §5.3.3: an entry without a certificateIssuer extension inherits the
// issuer of the nearest PRECEDING entry that has one (and, before any such
// entry, the CRL's own issuer). crlEntryRevokesCert() therefore walks
// X509_CRL_get_REVOKED() in encoded order. X509_CRL_get0_by_serial() — and
// anything else that reaches for OpenSSL's sorted lookup — calls
// sk_X509_REVOKED_sort() on that very stack, IN PLACE.
//
// THE FIXTURE IS BUILT SO THAT THE SORT CHANGES THE ANSWER, which is the only
// way a test can witness this:
//
//   encoded order:  [ serial 0x99, certificateIssuer = "unrelated CA" ]
//                   [ serial 0x02, no certificateIssuer               ]
//
//   0x02 is the INTERMEDIATE's serial. In encoded order it inherits "unrelated
//   CA" from the entry in front of it, is therefore not attributed to the
//   intermediate's real issuer, and does NOT revoke it. Sorted ascending by
//   serial the bare 0x02 entry moves to the FRONT, where there is no preceding
//   certificateIssuer, so it falls back to the CRL's own issuer — the root,
//   which IS the intermediate's issuer — and the intermediate is reported
//   REVOKED. A forged attribution, invented purely by the sort.
//
// The second CRL is the positive control: identical shape, except the leading
// entry names the ROOT, so carry-forward legitimately attributes the bare entry
// to the intermediate's real issuer and the revocation IS honoured. Without it,
// "always answer false" would pass the discriminating half.
//
// REVERT-AND-OBSERVE (F60a). Insert a sorting lookup at the top of
// crlEntryRevokesCert(), e.g.
//     X509_REVOKED* d = nullptr;
//     X509_CRL_get0_by_serial(crl, &d, const_cast<ASN1_INTEGER*>(targetSerial));
// before X509_CRL_get_REVOKED(), and the first assertion below goes red while
// the positive control stays green. See the phase-51 report for the verbatim
// output.
// ═══════════════════════════════════════════════════════════════════════
TEST(F60EncodedEntryOrder, CarryForwardAttributionSurvivesOnlyInEncodedOrder)
{
    SynthCert root, inter, leaf;
    ASSERT_TRUE(mintCa(root, "PDFE phase-51 F60 root", nullptr, 0x6001));
    // Serial 2 deliberately: it must sort BEFORE the decoy entry's serial so a
    // sort demonstrably moves it to the front of the list.
    ASSERT_TRUE(mintCa(inter, "PDFE phase-51 F60 intermediate", &root, 0x02));
    ASSERT_TRUE(mintCa(leaf, "PDFE phase-51 F60 leaf", &inter, 0x6003));

    const std::vector<std::vector<uint8_t>> pathDer = {
        leaf.der(), inter.der(), root.der()};
    for (const auto& d : pathDer) ASSERT_FALSE(d.empty());

    // The decoy entry's serial. Higher than the intermediate's, which is what
    // makes the sort reorder the pair.
    OwnedAsn1Integer decoy;
    ASSERT_NE(decoy.p, nullptr);
    ASN1_INTEGER* const decoySerial = decoy.p;
    ASSERT_EQ(ASN1_INTEGER_set(decoySerial, 0x99), 1);

    const ASN1_INTEGER* interSerial = X509_get0_serialNumber(inter.cert);
    ASSERT_NE(interSerial, nullptr);

    // FIXTURE SELF-CHECK. X509_REVOKED_cmp orders entries by ASN1_STRING_cmp of
    // the serial, so this is exactly the comparison a sort would apply. If the
    // intermediate's serial did NOT sort first, the sort would leave the encoded
    // order alone and the mutation below could not red anything — the test would
    // be decoration.
    ASSERT_LT(ASN1_STRING_cmp(reinterpret_cast<const ASN1_STRING*>(interSerial),
                              reinterpret_cast<const ASN1_STRING*>(decoySerial)),
              0)
        << "the fixture is no longer discriminating: a sort would not move the "
           "bare entry to the front, so encoded-order and sorted-order walks "
           "would agree and this test could not witness F60(a)";

    // An issuer name belonging to nobody in the chain.
    OwnedX509Name unrelatedName;
    ASSERT_NE(unrelatedName.p, nullptr);
    X509_NAME* const unrelated = unrelatedName.p;
    ASSERT_EQ(X509_NAME_add_entry_by_txt(
                  unrelated, "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char*>(
                      "PDFE phase-51 unrelated CA"),
                  -1, -1, 0),
              1);

    // ---- The discriminator: bare entry inherits the UNRELATED issuer.
    cc::CrlSpec misattributed;
    misattributed.sortEntries = false;  // encoded order is the whole point
    misattributed.entries = {
        {decoySerial, unrelated},   // says: what follows belongs to someone else
        {interSerial, nullptr},     // bare — inherits "unrelated CA"
    };
    const auto crlMisattributed = cc::mintCrl(root.key, root.cert, misattributed);
    ASSERT_FALSE(crlMisattributed.empty());

    // ---- The positive control: bare entry inherits the ROOT (the real issuer).
    cc::CrlSpec attributed;
    attributed.sortEntries = false;
    attributed.entries = {
        {decoySerial, X509_get_subject_name(root.cert)},
        {interSerial, nullptr},     // bare — inherits the root
    };
    const auto crlAttributed = cc::mintCrl(root.key, root.cert, attributed);
    ASSERT_FALSE(crlAttributed.empty());

    using pdfe::signatures::detail::walkPathForRevokedCert;

    EXPECT_TRUE(walkPathForRevokedCert(pathDer, {crlAttributed}, nullptr))
        << "POSITIVE CONTROL. A bare entry preceded by a certificateIssuer "
           "naming the intermediate's real issuer must revoke it (RFC 5280 "
           "§5.3.3 carry-forward). If this is false the assertion below is "
           "vacuous — 'never revoke anything' would satisfy it.";

    EXPECT_FALSE(walkPathForRevokedCert(pathDer, {crlMisattributed}, nullptr))
        << "the bare entry was attributed to the CRL's own issuer instead of to "
           "the 'unrelated CA' named by the entry in front of it. That is what "
           "happens when the revoked stack is SORTED before the walk "
           "(X509_CRL_get0_by_serial sorts in place), and it manufactures a "
           "revocation the CRL never asserted.";

    // No manual frees: `decoy` and `unrelatedName` release on every exit path,
    // including an ASSERT_* above returning early.
}
