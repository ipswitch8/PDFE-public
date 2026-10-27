// -*- coding: utf-8 -*-
// GitHub #212 — PIN: PDFE deliberately does NOT trust the Windows certificate
// store for PDF signature validation.
//
// WHY THIS FILE EXISTS
// --------------------
// #212 observed that nothing in src/ opens the OS trust store, and that nothing
// recorded whether that was deliberate. The recorded decision is the DECISION
// block at the top of src/signatures/CertChainValidator.h: it is deliberate,
// and it stays that way for now. A decision that lives only in a comment is an
// opinion; this file is what makes it a PROPERTY OF THE BUILD. The moment PDFE
// starts honouring the OS store, RootStoreCertificateIsNotTrusted goes red and
// whoever made that change has to come here and say so on purpose.
//
// The decision was sharpened by a second finding made while recording it:
// ChainValidationRequest's opt-in flag was called `useSystemStore` and BOTH
// production callers set it true, so the source read as though the machine's
// trust configuration was already consulted. It sets
// X509_STORE_set_default_paths(), which on Windows reads OPENSSLDIR/certs and
// SSL_CERT_FILE / SSL_CERT_DIR and never touches the certificate store. The
// flag is now `useOpenSslDefaultPaths`. These tests deliberately turn it ON, so
// they pin the strongest form of the claim: even with the "system" opt-in set,
// a certificate Windows trusts is not trusted by PDFE.
//
// THE ORACLE, AND WHY IT IS NOT VACUOUS
// -------------------------------------
// The input is not a fixture we minted — it is a REAL, currently-valid,
// self-signed root pulled out of the live Windows ROOT store via
// CertOpenSystemStore. Three things protect the assertions:
//
//   1. The enumeration is asserted to have FOUND something. An empty ROOT store
//      (or a broken enumeration) fails the test rather than silently passing it
//      with nothing measured — the "empty collection" vacuous-oracle shape.
//   2. Every negative is paired with a POSITIVE CONTROL on the SAME bytes: the
//      identical certificate, validated with the identical request except that
//      it is present in the TrustAnchorSet, must come back Trusted. So a red
//      negative cannot be explained by "the certificate was junk" or "OpenSSL
//      could not parse it" — the only input that differs is the trust SOURCE.
//   3. The verification instant is pinned to the midpoint of the chosen
//      certificate's own validity window, so neither outcome can be produced by
//      the calendar. Nothing here races the clock.
//
// crypt32 is used ONLY here, in the test binary. That is the point: the test
// can see the OS store precisely so it can prove the product does not.

#include <gtest/gtest.h>

#include "signatures/CertChainValidator.h"
#include "signatures/TrustAnchorSet.h"

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
// <wincrypt.h> defines X509_NAME / OCSP_RESPONSE / PKCS7_SIGNER_INFO as macros
// that collide with OpenSSL's type names. OpenSSL is included FIRST above and
// the offending macros are undefined immediately after, which is the
// documented way to use both in one translation unit.
#include <windows.h>
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif

using namespace pdfe::signatures;

namespace {

#ifdef _WIN32

/// One certificate lifted out of a Windows system store, with a verification
/// instant guaranteed to sit inside its validity window.
struct StoreCert {
    std::vector<uint8_t> der;
    std::int64_t midValidityUnix = 0;
    std::string subject;
};

std::int64_t fileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns ticks since 1601-01-01; 11644473600 s to the Unix epoch.
    return static_cast<std::int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

/// Enumerate the machine/user ROOT store and return the first SELF-SIGNED
/// certificate that is currently within its validity window, together with the
/// count of certificates seen.
///
/// Self-signed is required because the test validates the certificate AS THE
/// LEAF with no intermediates: a root is the only thing that can be a complete
/// one-element path, which makes "trusted iff it is in the anchor set" the only
/// variable. `certsSeen` is reported so the caller can fail on an empty
/// enumeration instead of passing it.
bool findSelfSignedRootFromWindowsStore(StoreCert& out, int& certsSeen)
{
    certsSeen = 0;
    HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return false;

    bool found = false;
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = ::CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        ++certsSeen;
        if (found) continue;

        // Self-signed: subject DN == issuer DN.
        if (!::CertCompareCertificateName(X509_ASN_ENCODING,
                                          &ctx->pCertInfo->Subject,
                                          &ctx->pCertInfo->Issuer)) {
            continue;
        }

        const std::int64_t notBefore = fileTimeToUnix(ctx->pCertInfo->NotBefore);
        const std::int64_t notAfter = fileTimeToUnix(ctx->pCertInfo->NotAfter);
        if (notAfter <= notBefore) continue;

        // Must be valid NOW as well as at the pinned instant, so the chosen
        // certificate is one a user could plausibly be relying on today.
        const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
        if (now < notBefore || now > notAfter) continue;

        out.der.assign(ctx->pbCertEncoded,
                       ctx->pbCertEncoded + ctx->cbCertEncoded);
        out.midValidityUnix = notBefore + (notAfter - notBefore) / 2;

        char name[512] = {0};
        ::CertGetNameStringA(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                             name, static_cast<DWORD>(sizeof(name)));
        out.subject = name;
        found = true;
    }
    ::CertCloseStore(store, 0);
    return found;
}

/// The request both halves of each test share. Only the anchor set differs
/// between the negative and its control.
ChainValidationRequest requestFor(const StoreCert& cert)
{
    ChainValidationRequest req;
    req.leafDer = cert.der;
    // Deliberately ON. This is the flag production sets, and the claim being
    // pinned is that even ON it does not reach the Windows store.
    req.useOpenSslDefaultPaths = true;
    req.verificationTimeUnix = cert.midValidityUnix;
    return req;
}

#endif  // _WIN32

}  // namespace

#ifdef _WIN32

// ---------------------------------------------------------------------------
// The pin.
// ---------------------------------------------------------------------------

/// A certificate the OPERATING SYSTEM trusts right now is reported
/// UntrustedRoot by PDFE, because PDFE's trust comes from its own anchor set
/// and from nowhere else.
TEST(WindowsCertStoreTrustPolicy, RootStoreCertificateIsNotTrusted)
{
    StoreCert cert;
    int certsSeen = 0;
    const bool got = findSelfSignedRootFromWindowsStore(cert, certsSeen);

    // Guard that the measurement HAPPENED. A zero-certificate ROOT store means
    // the enumeration is broken, not that the product behaved; that must be red.
    ASSERT_GT(certsSeen, 0)
        << "Enumerated 0 certificates from the Windows ROOT store. The test "
           "measured nothing; this is a harness failure, not a product verdict.";
    ASSERT_TRUE(got)
        << "No currently-valid self-signed root among " << certsSeen
        << " certificates in the Windows ROOT store.";
    ASSERT_FALSE(cert.der.empty());

    ChainValidationRequest req = requestFor(cert);
    // No anchors: PDFE's only trust source is empty.
    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::UntrustedRoot)
        << "GitHub #212 — PDFE reported '"
        << chainVerdictToString(res.verdict).toStdString()
        << "' for '" << cert.subject
        << "', a root the Windows certificate store trusts today.\n"
           "If PDFE has deliberately adopted the Windows certificate store, "
           "this test is the pin that says it had not, and it must be "
           "REWRITTEN to state the new policy — never deleted. See the "
           "DECISION block in src/signatures/CertChainValidator.h.\n"
           "OpenSSL default cert dir: "
        << (X509_get_default_cert_dir() ? X509_get_default_cert_dir() : "(null)")
        << ", default cert file: "
        << (X509_get_default_cert_file() ? X509_get_default_cert_file() : "(null)");

    EXPECT_FALSE(res.trusted());
    // Nothing was trusted, so nothing may be handed out as a verified path.
    EXPECT_TRUE(res.pathDer.empty());
    EXPECT_EQ(res.anchorsLoaded, 0);
}

/// POSITIVE CONTROL for the test above, on the SAME certificate bytes.
///
/// Without this, a red "UntrustedRoot" would be satisfiable by a certificate
/// OpenSSL could not parse, an expired one, or a truncated DER read out of the
/// store — every one of which also produces "not trusted". Here the ONLY input
/// that changes is where the trust comes from, so the pair is attributable.
TEST(WindowsCertStoreTrustPolicy, SameCertificateIsTrustedWhenItIsAPdfeAnchor)
{
    StoreCert cert;
    int certsSeen = 0;
    const bool got = findSelfSignedRootFromWindowsStore(cert, certsSeen);
    ASSERT_GT(certsSeen, 0);
    ASSERT_TRUE(got);

    ChainValidationRequest req = requestFor(cert);
    TrustAnchorSet anchors;
    ASSERT_TRUE(anchors.addDer(cert.der))
        << "The certificate read from the Windows ROOT store did not parse as "
           "DER X.509. The negative test above would then be unattributable.";
    req.anchors = anchors;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "Control failed for '" << cert.subject << "': "
        << res.message.toStdString()
        << " (X509_V_ERR " << res.opensslError
        << " at depth " << res.errorDepth << ")";
    EXPECT_TRUE(res.trusted());
    EXPECT_EQ(res.anchorsLoaded, 1);
    // A trusted verdict must publish the path it verified.
    EXPECT_EQ(static_cast<int>(res.pathDer.size()), res.chainLength);
    ASSERT_FALSE(res.pathDer.empty());
    EXPECT_EQ(res.pathDer.front(), cert.der);
}

/// The opt-in flag contributes NOTHING on Windows — the pair of verdicts is
/// identical with it on and off. This is the narrow, direct statement of what
/// the rename in #212 was about: nobody should read `useOpenSslDefaultPaths`
/// (formerly `useSystemStore`) as adding machine trust on this platform.
TEST(WindowsCertStoreTrustPolicy, OpenSslDefaultPathsAddNoTrustOnWindows)
{
    StoreCert cert;
    int certsSeen = 0;
    ASSERT_TRUE(findSelfSignedRootFromWindowsStore(cert, certsSeen));
    ASSERT_GT(certsSeen, 0);

    ChainValidationRequest off = requestFor(cert);
    off.useOpenSslDefaultPaths = false;
    ChainValidationRequest on = requestFor(cert);
    on.useOpenSslDefaultPaths = true;

    const ChainValidationResult resOff = CertChainValidator::validate(off);
    const ChainValidationResult resOn = CertChainValidator::validate(on);

    EXPECT_EQ(resOff.verdict, resOn.verdict);
    EXPECT_EQ(resOff.verdict, ChainVerdict::UntrustedRoot);
    EXPECT_FALSE(resOn.trusted());
}

#endif  // _WIN32
