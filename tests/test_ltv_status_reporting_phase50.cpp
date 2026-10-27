// -*- coding: utf-8 -*-
//
// Meta-audit follow-up phase-50 — F52, F53, F59.
//
// WHAT THESE THREE ITEMS SHARE. All of them are about what LtvValidator TELLS
// you, not about what it decides. Every rejected-evidence path in
// LtvValidator::verifyWith() already failed CLOSED before this phase: none could
// produce LtvValidated, none could produce a false Revoked. What was wrong was
// that the honest description of WHY was either overwritten (F52), collapsed
// into a value that means something else (F53), or — for F59 — simply wasted
// work whose verdict is identical either way, which is precisely why it needed
// a control that does not look at the verdict.
//
//   F52  verify() rewrote UntrustedChain -> NoDssData in the no-/DSS branch,
//        and ltvStatusToDisplayString(NoDssData) is "Validated (no revocation
//        data)". A self-signed, unanchored certificate with no /DSS therefore
//        rendered as **Validated**.
//   F53  a rogue-responder OCSP, a CRL from a CA that may not sign CRLs, an
//        expired CRL and an EMPTY /DSS all produced the identical NoDssData, so
//        actively forged evidence was indistinguishable from no evidence.
//   F59  the revoked-path-certificate walk did not stop at its first hit.
//
// EVERY TEST BELOW CARRIES ITS POSITIVE CONTROL. "Never say Validated" and
// "always say rejected" would each pass half of this file on their own; the
// controls are what make the discrimination, rather than the refusal, the thing
// being pinned.

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "ui/SignaturePanel.h"
#include "support/CrlChainFixture.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <QLabel>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::signatures::LtvStatus;
using pdfe::signatures::LtvValidator;
using pdfe::signatures::LtvVerifyResult;
using pdfe::signatures::ltvStatusToDisplayString;

namespace cc = pdfe_test::crlchain;

namespace {

constexpr const char* kScratch = "phase50";

std::string shown(LtvStatus s)
{
    return ltvStatusToDisplayString(s).toStdString();
}

// The display string EXACTLY as MainWindow::onVerifySignatures() computes it:
//   signaturePanel_->setLtvStatusText(ltvStatusToDisplayString(ltv.status), ...)
// The F52 criterion is about that string, so the test must go through the same
// function rather than asserting on the enum.
QString panelStringFor(LtvStatus s)
{
    return ltvStatusToDisplayString(s);
}

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// ---------------------------------------------------------------------------
// A synthetic certificate chain, built in memory, for the F59 path-walk
// control. It cannot come from tests/fixtures/crl_chain/, which is
// root -> intermediate -> leaf: the verified path is then 3 certificates long
// and the walk (which skips the leaf and the anchor) has exactly ONE link to
// visit. A loop that breaks on its first hit and a loop that does not are
// indistinguishable over one iteration, so the committed corpus CANNOT witness
// F59 in either direction. Four levels is the minimum that can.
struct SynthCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    ~SynthCert()
    {
        if (key) EVP_PKEY_free(key);
        if (cert) X509_free(cert);
    }
    SynthCert() = default;
    SynthCert(const SynthCert&) = delete;
    SynthCert& operator=(const SynthCert&) = delete;

    std::vector<uint8_t> der() const
    {
        std::vector<uint8_t> out;
        unsigned char* d = nullptr;
        const int l = i2d_X509(cert, &d);
        if (l > 0 && d) { out.assign(d, d + l); OPENSSL_free(d); }
        return out;
    }
};

// Mint a CA certificate named @p cn. Self-signed when @p issuer is null,
// otherwise issued (and really signed) by @p issuer.
//
// keyUsage is deliberately OMITTED. X509_get_key_usage() returns UINT32_MAX for
// an absent extension, which RFC 5280 §4.2.1.3 says leaves the certificate
// unconstrained — so these CAs may sign CRLs. That is the same rule
// crlIsAuthorized() implements, and it keeps this fixture focused on the loop
// guard rather than on keyUsage.
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

    // basicConstraints CA:TRUE so X509_check_ca() classifies these as CAs, the
    // shape a real path above the leaf has.
    X509_EXTENSION* bc = X509V3_EXT_conf_nid(nullptr, nullptr,
                                             NID_basic_constraints,
                                             "critical,CA:TRUE");
    if (bc) { X509_add_ext(out.cert, bc, -1); X509_EXTENSION_free(bc); }

    EVP_PKEY* signWith = issuer ? issuer->key : out.key;
    if (X509_sign(out.cert, signWith, EVP_sha256()) == 0) {
        ERR_clear_error();
        return false;
    }
    return true;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// F52 — a self-signed, untrusted certificate with no /DSS must not say
// "Validated".
//
// This is the backlog row's own acceptance criterion, verbatim: "a document
// signed by an untrusted self-signed certificate with no /DSS must not produce
// a panel string containing the word 'Validated'".
//
// REVERT-AND-OBSERVE. Restore the `r.status = LtvStatus::NoDssData;` rewrite in
// LtvValidator::verify()'s no-/DSS branch and this test goes RED: the status
// becomes NoDssData, whose display string is literally "Validated (no
// revocation data)". Observed 2026-08-09; see phase-50-dispositions.md.
// ═══════════════════════════════════════════════════════════════════════
TEST(LtvStatusReportingPhase50, SelfSignedNoDssDoesNotSayValidated)
{
    auto pfx = fixturesDir() / "test_signer.pfx";
    ASSERT_TRUE(std::filesystem::exists(pfx))
        << "tests/fixtures/test_signer.pfx is committed; a missing one is a "
           "lost fixture, not an optional environment feature";

    auto ctx = std::make_shared<MuPdfContext>();
    auto signedPath = cc::signPdfWith(ctx, pfx, kScratch,
                                      "f52_selfsigned_nodss.pdf", "F52Sig");
    ASSERT_FALSE(signedPath.empty()) << "could not sign with test_signer.pfx";

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);

    // No /DSS was ever written, so verify() takes the no-/DSS branch — the one
    // that used to perform the rewrite.
    const LtvVerifyResult r = LtvValidator::verify(doc.get());

    ASSERT_EQ(r.status, LtvStatus::UntrustedChain)
        << "a self-signed end-entity certificate reaches no trust anchor, so "
           "the honest status is UntrustedChain. Getting NoDssData here means "
           "the F52 rewrite is back; got " << shown(r.status);

    const QString panel = panelStringFor(r.status);
    EXPECT_FALSE(panel.contains("Validated", Qt::CaseInsensitive))
        << "the signature panel would show '" << panel.toStdString()
        << "' for a document signed by a self-minted certificate that nothing "
           "vouches for. The digest verifying says only that the bytes match "
           "the attacker's own signature.";
    EXPECT_EQ(panel.toStdString(),
              QString::fromUtf8("Signature intact \xE2\x80\x94 certificate "
                                "NOT trusted")
                  .toStdString());

    // The honest detail must exist and must reach the caller, not be written
    // and discarded as it was before this phase.
    EXPECT_TRUE(r.message.contains("not trusted"))
        << "message was: " << r.message.toStdString();
    EXPECT_TRUE(r.message.contains("/DSS"))
        << "message was: " << r.message.toStdString();
}

// POSITIVE CONTROL for the test above. Without this, deleting the word
// "Validated" from every display string would pass — and a panel that never
// says a signature is valid is not a fix, it is a different defect.
//
// Same code path (verify(), no /DSS), same signing machinery, the ONLY
// difference being that this chain terminates at a trust anchor.
TEST(LtvStatusReportingPhase50, TrustedChainNoDssStillSaysValidated)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto rootPem = cc::derToPemFile(cc::chainDir() / "root.der", kScratch,
                                    "f52_control_root.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto signedPath = cc::signPdfWith(ctx, cc::chainDir() / "leaf_crlsign.pfx",
                                      kScratch, "f52_trusted_nodss.pdf",
                                      "F52CtlSig");
    ASSERT_FALSE(signedPath.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);
    const LtvVerifyResult r = LtvValidator::verify(doc.get());

    ASSERT_EQ(r.status, LtvStatus::OnlineValidated)
        << "with the root trusted and no /DSS, the chain validates against the "
           "anchor set and this is the genuine 'validated, revocation unknown' "
           "case; got " << shown(r.status);
    EXPECT_TRUE(panelStringFor(r.status).contains("Validated"))
        << "a genuinely trusted signature must still be reported as validated; "
           "otherwise SelfSignedNoDssDoesNotSayValidated passes for the wrong "
           "reason (nothing is ever validated). Got "
        << panelStringFor(r.status).toStdString();
}

// ═══════════════════════════════════════════════════════════════════════
// F53 — evidence REFUSED must render differently from evidence ABSENT.
//
// The two inputs below differ in exactly one thing: whether a CRL is supplied.
// The CRL that IS supplied is refused by RFC 5280 §6.3.3(f) — its issuing CA's
// keyUsage does not assert cRLSign — which is one of the three cases the
// acceptance criterion names.
//
// REVERT-AND-OBSERVE. Collapse RevocationEvidenceRejected back into NoDssData
// (delete the `else if (rejection.any)` arm in verifyWith() step 5) and both
// halves produce the identical status AND the identical message, so both
// EXPECT_NE below go RED. Observed 2026-08-09.
// ═══════════════════════════════════════════════════════════════════════
namespace {

// Verify `leaf_nocrlsign` (whose issuing intermediate lacks cRLSign) against
// the CRLs given, with the corpus root trusted.
::testing::AssertionResult noCrlSignVerdict(
    const char* signedName,
    bool supplyCrl,
    LtvVerifyResult& out)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    auto rootPem = cc::derToPemFile(dir / "root.der", kScratch,
                                    "f53_root.pem");
    if (rootPem.empty())
        return ::testing::AssertionFailure() << "could not write root anchor";
    cc::ScopedTrustAnchor anchor(rootPem);
    if (!anchor.added)
        return ::testing::AssertionFailure() << "could not install root anchor";

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_nocrlsign.pfx", kScratch,
                                      signedName, "F53Sig");
    if (signedPath.empty())
        return ::testing::AssertionFailure() << "could not sign";

    auto leafDer = cc::readAllBytes(dir / "leaf_nocrlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_nocrlsign.der");
    if (leafDer.empty() || interDer.empty())
        return ::testing::AssertionFailure() << "missing DER material";

    std::vector<std::vector<uint8_t>> crls;
    if (supplyCrl) {
        X509* interCert = cc::loadDerCert(dir / "inter_nocrlsign.der");
        EVP_PKEY* interKey = cc::loadPemKey(dir / "inter_nocrlsign_key.pem");
        if (!interCert || !interKey) {
            if (interCert) X509_free(interCert);
            if (interKey) EVP_PKEY_free(interKey);
            return ::testing::AssertionFailure() << "could not load inter_nocrlsign";
        }
        auto crl = cc::mintCrl(interKey, interCert, cc::CrlSpec{});
        EVP_PKEY_free(interKey);
        X509_free(interCert);
        if (crl.empty())
            return ::testing::AssertionFailure() << "could not mint CRL";
        crls.push_back(std::move(crl));
    }

    auto doc = Document::open(ctx, signedPath);
    if (!doc)
        return ::testing::AssertionFailure() << "could not reopen signed PDF";
    out = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {}, crls,
                                   /*useTrustAnchors=*/true);
    return ::testing::AssertionSuccess();
}

} // namespace

TEST(LtvStatusReportingPhase50, RejectedCrlIsDistinguishableFromAbsentEvidence)
{
    LtvVerifyResult rejected;
    ASSERT_TRUE(noCrlSignVerdict("f53_rejected.pdf", /*supplyCrl=*/true,
                                 rejected));
    LtvVerifyResult absent;
    ASSERT_TRUE(noCrlSignVerdict("f53_absent.pdf", /*supplyCrl=*/false, absent));

    // Neither may become evidence — the fail-closed property F53 must preserve.
    EXPECT_NE(rejected.status, LtvStatus::LtvValidated);
    EXPECT_NE(rejected.status, LtvStatus::Revoked)
        << "a CRL refused on §6.3.3(f) must not be consulted in EITHER "
           "direction; reporting Revoked on an unauthorised CRL's say-so is a "
           "denial of service with a forged cause";

    EXPECT_EQ(rejected.status, LtvStatus::RevocationEvidenceRejected)
        << "a CRL signed by a CA whose keyUsage does not assert cRLSign was "
           "PRESENT and was REFUSED. Reporting the same NoDssData an empty "
           "/DSS produces discards the only signal a user has; got "
        << shown(rejected.status);
    EXPECT_EQ(absent.status, LtvStatus::NoDssData)
        << "with no CRL at all there is nothing to reject; got "
        << shown(absent.status);

    EXPECT_NE(rejected.status, absent.status)
        << "refused and absent evidence still render identically";
    EXPECT_NE(rejected.message, absent.message)
        << "the message must name the rule that refused the evidence, so a "
           "user can tell a forged artifact from a missing one";

    // The message must name the RULE, not merely restate the status.
    EXPECT_TRUE(rejected.message.contains("cRLSign"))
        << "message was: " << rejected.message.toStdString();
    EXPECT_TRUE(rejected.message.contains("6.3.3"))
        << "message was: " << rejected.message.toStdString();

    // Display strings must differ too — the panel shows the status, not the
    // enum.
    EXPECT_NE(panelStringFor(rejected.status).toStdString(),
              panelStringFor(absent.status).toStdString());
    EXPECT_FALSE(panelStringFor(rejected.status).contains("Validated",
                                                          Qt::CaseInsensitive))
        << "refused evidence must not be reported with the word 'Validated'";
}

// POSITIVE CONTROL. Same shape, the cRLSign-bearing chain: a genuine CRL must
// still be honoured. Without this, "call everything rejected" passes the test
// above.
TEST(LtvStatusReportingPhase50, GenuineCrlIsStillHonouredNotRejected)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    auto rootPem = cc::derToPemFile(dir / "root.der", kScratch,
                                    "f53_ctl_root.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx", kScratch,
                                      "f53_control.pdf", "F53CtlSig");
    ASSERT_FALSE(signedPath.empty());

    X509* interCert = cc::loadDerCert(dir / "inter_crlsign.der");
    EVP_PKEY* interKey = cc::loadPemKey(dir / "inter_crlsign_key.pem");
    ASSERT_NE(interCert, nullptr);
    ASSERT_NE(interKey, nullptr);
    auto crl = cc::mintCrl(interKey, interCert, cc::CrlSpec{});
    EVP_PKEY_free(interKey);
    X509_free(interCert);
    ASSERT_FALSE(crl.empty());

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    ASSERT_FALSE(leafDer.empty());
    ASSERT_FALSE(interDer.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);
    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {}, {crl},
                                      /*useTrustAnchors=*/true);

    EXPECT_EQ(r.status, LtvStatus::LtvValidated)
        << "a fresh CRL from the genuine cRLSign-bearing issuer is authorised "
           "evidence and must reach full LTV. If this is "
           "RevocationEvidenceRejected then the new status is being applied to "
           "evidence that was accepted, which would be a regression, not a "
           "fix; got " << shown(r.status);
}

// ═══════════════════════════════════════════════════════════════════════
// F59 — the revoked-path-certificate walk must stop at its first hit.
//
// The VERDICT is identical whether or not it stops: `revokedPathCert` is a bool
// that is already true. Only the WORK differs, so the control counts the calls
// the walk makes rather than asserting the answer.
//
// The path is leaf -> i1 -> i2 -> root, and i1 is revoked by a CRL i2 signed.
// With the fixed guard the walk visits link 1 (subject i1) and stops. Without
// it, it goes on to visit link 2 (subject i2), running a full certIssuedSigner()
// signature verification and a whole isCertRevokedByCrl() CRL sweep for an
// answer that cannot change.
//
// THE SECOND ASSERTION IS THE ONE THAT MATTERS: the same path with a CRL that
// revokes NOTHING visits BOTH links. Without it, "linkVisits == 1" would also
// be satisfied by a path that only ever had one link — which is exactly the
// shape tests/fixtures/crl_chain/ has, and why it cannot be used here.
//
// REVERT-AND-OBSERVE. Drop `&& !revoked` from the loop guard in
// detail::walkPathForRevokedCert() and linkVisits becomes 2 in the revoked
// case, reddening this test while every verdict in the suite stays identical.
// Observed 2026-08-09.
// ═══════════════════════════════════════════════════════════════════════
TEST(LtvStatusReportingPhase50, PathWalkStopsAtTheFirstRevokedCertificate)
{
    SynthCert root, i2, i1, leaf;
    ASSERT_TRUE(mintCa(root, "PDFE phase-50 synthetic root", nullptr, 0x5001));
    ASSERT_TRUE(mintCa(i2, "PDFE phase-50 synthetic inter 2", &root, 0x5002));
    ASSERT_TRUE(mintCa(i1, "PDFE phase-50 synthetic inter 1", &i2, 0x5003));
    ASSERT_TRUE(mintCa(leaf, "PDFE phase-50 synthetic leaf", &i1, 0x5004));

    // Sanity: the path really is four long, so the walk really does have two
    // links to visit. Without this the counts below prove nothing.
    const std::vector<std::vector<uint8_t>> pathDer = {
        leaf.der(), i1.der(), i2.der(), root.der()};
    for (const auto& d : pathDer) ASSERT_FALSE(d.empty());
    ASSERT_EQ(pathDer.size(), 4u);

    // A CRL issued by i2 (the parent of i1) listing i1's serial as revoked.
    cc::CrlSpec revokeI1;
    revokeI1.revokedSerials = {X509_get0_serialNumber(i1.cert)};
    auto crlRevokingI1 = cc::mintCrl(i2.key, i2.cert, revokeI1);
    ASSERT_FALSE(crlRevokingI1.empty());

    // The same CRL shape listing nothing — the control that establishes the
    // walk's full length.
    auto crlRevokingNothing = cc::mintCrl(i2.key, i2.cert, cc::CrlSpec{});
    ASSERT_FALSE(crlRevokingNothing.empty());

    using pdfe::signatures::detail::PathWalkStats;
    using pdfe::signatures::detail::walkPathForRevokedCert;

    PathWalkStats hit;
    const bool revoked =
        walkPathForRevokedCert(pathDer, {crlRevokingI1}, &hit);
    EXPECT_TRUE(revoked)
        << "the fixture is wrong if a CRL from i2 listing i1's serial does not "
           "register as a revoked path certificate — every count below would "
           "then be measuring the wrong thing";
    EXPECT_EQ(hit.linkVisits, 1)
        << "the walk kept going after finding a revoked CA. The verdict is "
           "unchanged, which is exactly why nothing else can catch this: it is "
           "a full signature verification plus a CRL sweep per remaining link, "
           "for an answer that cannot move.";
    EXPECT_EQ(hit.certIssuedSignerCalls, 1);
    EXPECT_EQ(hit.isCertRevokedByCrlCalls, 1);

    PathWalkStats miss;
    const bool notRevoked =
        walkPathForRevokedCert(pathDer, {crlRevokingNothing}, &miss);
    EXPECT_FALSE(notRevoked);
    EXPECT_EQ(miss.linkVisits, 2)
        << "this path has TWO walkable links (leaf and anchor are excluded). "
           "If this is 1, the revoked-case count of 1 above is measuring a "
           "short path rather than an early break, and F59 is unpinned.";
    EXPECT_EQ(miss.certIssuedSignerCalls, 2);
    EXPECT_EQ(miss.isCertRevokedByCrlCalls, 2);
}

// ═══════════════════════════════════════════════════════════════════════
// The UI half of F52/F53 (acceptance criterion 4).
//
// MainWindow::onVerifySignatures() used to call
//     signaturePanel_->setLtvStatusText(ltvStatusToDisplayString(ltv.status));
// and discard ltv.message. The message is the ONLY place the specific rule is
// stated — "Revocation evidence REJECTED" is the same six words whether a rogue
// responder was caught or a CRL had expired — so discarding it threw away the
// entire point of F53.
//
// These tests drive the panel with the exact (status, message) pair a real
// verify() produces, in the exact way onVerifySignatures() now does.
// ═══════════════════════════════════════════════════════════════════════
TEST(LtvStatusReportingPhase50, PanelSurfacesTheLtvMessageNotJustTheStatus)
{
    pdfe::ui::SignaturePanel panel;

    // The widget must be findable by objectName for the pywinauto/UIA layer.
    ASSERT_NE(panel.findChild<QLabel*>("ltvStatusDetailLabel"), nullptr)
        << "the detail row must carry a stable objectName or no UI automation "
           "can assert on it";

    const QString status = ltvStatusToDisplayString(
        LtvStatus::RevocationEvidenceRejected);
    const QString detail = QStringLiteral(
        "OCSP response failed responder authorisation "
        "(rogue responder: not a proven issuer of the signer)");
    panel.setLtvStatusText(status, detail);

    EXPECT_TRUE(panel.ltvStatusText().contains(status))
        << "status row lost the status; got "
        << panel.ltvStatusText().toStdString();
    EXPECT_EQ(panel.ltvStatusDetailText().toStdString(), detail.toStdString())
        << "the rule that refused the evidence never reached the UI — this is "
           "the discard F52/F53 are about";

    // A different rule must produce a different visible detail. If the panel
    // showed a canned string per status this would pass anyway, so assert the
    // change is driven by the message.
    const QString other = QStringLiteral(
        "CRL rejected as unacceptable: expired validity window");
    panel.setLtvStatusText(status, other);
    EXPECT_EQ(panel.ltvStatusDetailText().toStdString(), other.toStdString());

    // Clearing the panel clears both rows.
    panel.setLtvStatusText(QString(), QString());
    EXPECT_TRUE(panel.ltvStatusText().isEmpty());
    EXPECT_TRUE(panel.ltvStatusDetailText().isEmpty());
}

// End-to-end: a REAL verify() result, fed to the panel exactly as
// MainWindow::onVerifySignatures() feeds it, must show the untrusted-chain
// explanation rather than nothing.
TEST(LtvStatusReportingPhase50, UntrustedChainDetailReachesThePanel)
{
    auto pfx = fixturesDir() / "test_signer.pfx";
    ASSERT_TRUE(std::filesystem::exists(pfx));

    auto ctx = std::make_shared<MuPdfContext>();
    auto signedPath = cc::signPdfWith(ctx, pfx, kScratch,
                                      "f52_panel_e2e.pdf", "F52PanelSig");
    ASSERT_FALSE(signedPath.empty());
    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);

    const LtvVerifyResult ltv = LtvValidator::verify(doc.get());

    pdfe::ui::SignaturePanel panel;
    // VERBATIM the MainWindow::onVerifySignatures() call.
    panel.setLtvStatusText(ltvStatusToDisplayString(ltv.status), ltv.message);

    EXPECT_FALSE(panel.ltvStatusText().contains("Validated", Qt::CaseInsensitive))
        << "panel status row read: " << panel.ltvStatusText().toStdString();
    EXPECT_FALSE(panel.ltvStatusDetailText().isEmpty())
        << "the panel shows a bare status with no explanation — the honest "
           "text exists in ltv.message and is being thrown away";
    EXPECT_TRUE(panel.ltvStatusDetailText().contains("not trusted"))
        << "detail row read: " << panel.ltvStatusDetailText().toStdString();
}

// ═══════════════════════════════════════════════════════════════════════
// PHASE-50 REVIEW, FIX 1 — the F53 recorder accused honest documents.
//
// WHAT THE ORIGINAL TESTS COULD NOT SEE. The F53 positive control above
// (GenuineCrlIsStillHonouredNotRejected) supplies exactly ONE CRL, so no
// artifact in it is ever evaluated against a certificate it is not about.
// CrlScopeAndChainTest.UnrevokedIntermediateStillValidates supplies two, which
// is the shape that reproduces the bug, but asserts only on `status` — and the
// status was right. The damage was in the MESSAGE, which nothing looked at.
//
// The defect: crlAuthorityFor() returns CrlAuthority::None both for a CRL we
// REFUSED and for a CRL that simply is not about this certificate, and the
// recorder noted a rejection for both. Any multi-level document carrying
// long-term validation material embeds one CRL per chain link, so the root's
// CRL — genuine, correctly signed, and about the INTERMEDIATE — took the
// second path every time.
//
// These two tests pin the message and the status independently, because the
// two symptoms are separable: full coverage kept the right status and gained a
// false note; partial coverage lost the status outright.

// (a) FULL COVERAGE. Same input shape as UnrevokedIntermediateStillValidates —
//     a CRL from the root and a CRL from the intermediate, neither listing
//     anything, root trusted. Nothing was refused, so the message must not
//     carry a rejection note.
TEST(LtvStatusReportingPhase50, TwoLevelDssCarriesNoRejectionNote)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    auto rootPem = cc::derToPemFile(dir / "root.der", kScratch,
                                    "f53fix_root.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx", kScratch,
                                      "f53fix_full.pdf", "F53FixFullSig");
    ASSERT_FALSE(signedPath.empty());

    X509* rootCert = cc::loadDerCert(dir / "root.der");
    EVP_PKEY* rootKey = cc::loadPemKey(dir / "root_key.pem");
    X509* interCert = cc::loadDerCert(dir / "inter_crlsign.der");
    EVP_PKEY* interKey = cc::loadPemKey(dir / "inter_crlsign_key.pem");
    ASSERT_NE(rootCert, nullptr);
    ASSERT_NE(rootKey, nullptr);
    ASSERT_NE(interCert, nullptr);
    ASSERT_NE(interKey, nullptr);
    // The root's CRL covers the INTERMEDIATE. It is entirely genuine; it just
    // has nothing to say about the signer. This is the artifact that used to
    // be reported as an authorisation failure.
    auto rootCrl = cc::mintCrl(rootKey, rootCert, cc::CrlSpec{});
    auto interCrl = cc::mintCrl(interKey, interCert, cc::CrlSpec{});
    EVP_PKEY_free(rootKey);
    EVP_PKEY_free(interKey);
    X509_free(rootCert);
    X509_free(interCert);
    ASSERT_FALSE(rootCrl.empty());
    ASSERT_FALSE(interCrl.empty());

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    ASSERT_FALSE(leafDer.empty());
    ASSERT_FALSE(interDer.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);
    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {},
                                      {rootCrl, interCrl},
                                      /*useTrustAnchors=*/true);

    EXPECT_EQ(r.status, LtvStatus::LtvValidated)
        << "a fully covered, entirely valid two-level chain must LTV-validate; "
           "got " << shown(r.status);
    // THE ASSERTION THE ORIGINAL TESTS WERE MISSING.
    EXPECT_FALSE(r.message.contains("Note:"))
        << "nothing in this document was refused, so the message must carry no "
           "rejection note; message was: " << r.message.toStdString();
    EXPECT_FALSE(r.message.contains("issuer authorisation"))
        << "the root's CRL is genuine and is about the INTERMEDIATE, not the "
           "signer. Reporting it as an authorisation failure accuses an honest "
           "document; message was: " << r.message.toStdString();
}

// (b) PARTIAL COVERAGE. An honest document that embeds only the ROOT's CRL —
//     nothing covers the signer, so there is no revocation evidence. That is
//     "no evidence", not "evidence we refused": the status must stay NoDssData
//     and must NOT become RevocationEvidenceRejected, the string reserved for
//     something adversarial.
TEST(LtvStatusReportingPhase50, PartialCoverageIsAbsentEvidenceNotRejected)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    auto rootPem = cc::derToPemFile(dir / "root.der", kScratch,
                                    "f53fix_root_b.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx", kScratch,
                                      "f53fix_partial.pdf", "F53FixPartSig");
    ASSERT_FALSE(signedPath.empty());

    X509* rootCert = cc::loadDerCert(dir / "root.der");
    EVP_PKEY* rootKey = cc::loadPemKey(dir / "root_key.pem");
    ASSERT_NE(rootCert, nullptr);
    ASSERT_NE(rootKey, nullptr);
    auto rootCrl = cc::mintCrl(rootKey, rootCert, cc::CrlSpec{});
    EVP_PKEY_free(rootKey);
    X509_free(rootCert);
    ASSERT_FALSE(rootCrl.empty());

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    ASSERT_FALSE(leafDer.empty());
    ASSERT_FALSE(interDer.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);
    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {},
                                      {rootCrl}, /*useTrustAnchors=*/true);

    EXPECT_NE(r.status, LtvStatus::RevocationEvidenceRejected)
        << "the only CRL here is genuine and covers the intermediate. Nothing "
           "was refused, so PDFE must not tell the user its revocation "
           "evidence was REJECTED; got " << shown(r.status) << " with message: "
        << r.message.toStdString();
    EXPECT_EQ(r.status, LtvStatus::NoDssData)
        << "no artifact covers the signer, so there is no revocation evidence "
           "about it — which is exactly NoDssData; got " << shown(r.status);
    // FAIL-CLOSED CONTROL. NoDssData must not be reachable here by way of
    // "everything is fine": nothing established the signer's revocation
    // status, so full LTV must NOT be awarded either.
    EXPECT_NE(r.status, LtvStatus::LtvValidated)
        << "no CRL covers the signer, so LTV must not be awarded";
}

// ═══════════════════════════════════════════════════════════════════════
// PHASE-50 REVIEW, FIX 4 — the rejection was discarded when the chain was
// untrusted.
//
// verifyWith() returns UntrustedChain ABOVE the rejection arm, so a document
// carrying BOTH refused evidence and an untrusted chain reported only the chain
// problem and dropped the refusal entirely. Fail-closed, but incomplete: of the
// two facts, the refused evidence is the more alarming, and the user never saw
// it. The STATUS must not change — an untrusted chain is already the stronger
// finding — so this pins the message.
TEST(LtvStatusReportingPhase50, UntrustedChainStillReportsRefusedEvidence)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    // DELIBERATELY NO ScopedTrustAnchor: the chain must not validate.
    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx", kScratch,
                                      "f53fix_untrusted.pdf", "F53FixUntrSig");
    ASSERT_FALSE(signedPath.empty());

    X509* interCert = cc::loadDerCert(dir / "inter_crlsign.der");
    EVP_PKEY* rootKey = cc::loadPemKey(dir / "root_key.pem");
    ASSERT_NE(interCert, nullptr);
    ASSERT_NE(rootKey, nullptr);
    // A CRL carrying the signer's issuer's DN — so it CLAIMS to cover the
    // signer, and the FIX 1 relevance test lets it through — but signed with
    // the WRONG key. That is exactly the forgery shape F53 exists to name:
    // crlIsAuthorized() cannot verify it under any proven issuer of the signer.
    auto forgedCrl = cc::mintCrl(rootKey, interCert, cc::CrlSpec{});
    EVP_PKEY_free(rootKey);
    X509_free(interCert);
    ASSERT_FALSE(forgedCrl.empty());

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    ASSERT_FALSE(leafDer.empty());
    ASSERT_FALSE(interDer.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);
    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {},
                                      {forgedCrl}, /*useTrustAnchors=*/true);

    EXPECT_EQ(r.status, LtvStatus::UntrustedChain)
        << "the status must stay UntrustedChain — a chain that terminates "
           "nowhere trusted is the stronger of the two findings and a refusal "
           "must not displace it; got " << shown(r.status);
    EXPECT_TRUE(r.message.contains("trusted root"))
        << "the original chain message must survive; message was: "
        << r.message.toStdString();
    EXPECT_TRUE(r.message.contains("issuer authorisation"))
        << "a forged CRL was supplied and refused. Reporting only the chain "
           "problem throws away the single most alarming thing PDFE learned "
           "about this document; message was: " << r.message.toStdString();
}
