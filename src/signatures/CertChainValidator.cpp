// -*- coding: utf-8 -*-
// Meta-audit follow-up F23 (gap PDF-39): CertChainValidator -- see header.
#include "CertChainValidator.h"

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/err.h>

namespace pdfe::signatures {

namespace {

X509* derToX509(const std::vector<uint8_t>& der)
{
    if (der.empty()) return nullptr;
    const unsigned char* p = der.data();
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (!cert) ERR_clear_error();
    return cert;
}

X509_CRL* derToX509Crl(const std::vector<uint8_t>& der)
{
    if (der.empty()) return nullptr;
    const unsigned char* p = der.data();
    X509_CRL* crl = d2i_X509_CRL(nullptr, &p, static_cast<long>(der.size()));
    if (!crl) ERR_clear_error();
    return crl;
}

// Map OpenSSL's verify error to our verdict. The mapping is the whole point
// of the class: it is what lets a test assert "rejected BECAUSE the anchor
// set does not cover this chain" rather than "rejected".
ChainVerdict verdictForError(int err)
{
    switch (err) {
    case X509_V_OK:
        return ChainVerdict::Trusted;

    // The chain does not reach anything trusted. All four of these mean
    // "the anchor set is what rejected it", at different points.
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_CERT_UNTRUSTED:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        return ChainVerdict::UntrustedRoot;

    // Validity-window failures. A path to an anchor EXISTS; it is stale.
    case X509_V_ERR_CERT_HAS_EXPIRED:
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        return ChainVerdict::ExpiredCertificate;
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        return ChainVerdict::NotYetValid;

    case X509_V_ERR_CERT_REVOKED:
        return ChainVerdict::Revoked;

    // CRL checking was demanded and could not be satisfied. Surfacing this
    // as its own verdict stops "we could not check" from being reported the
    // same way as "we checked and it is fine".
    case X509_V_ERR_UNABLE_TO_GET_CRL:
    case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:
    case X509_V_ERR_CRL_HAS_EXPIRED:
    case X509_V_ERR_CRL_NOT_YET_VALID:
        return ChainVerdict::NoRevocationInformation;

    default:
        return ChainVerdict::Unknown;
    }
}

} // namespace

QString chainVerdictToString(ChainVerdict verdict)
{
    switch (verdict) {
    case ChainVerdict::Trusted:                 return QStringLiteral("Trusted");
    case ChainVerdict::UntrustedRoot:           return QStringLiteral("UntrustedRoot");
    case ChainVerdict::ExpiredCertificate:      return QStringLiteral("ExpiredCertificate");
    case ChainVerdict::NotYetValid:             return QStringLiteral("NotYetValid");
    case ChainVerdict::Revoked:                 return QStringLiteral("Revoked");
    case ChainVerdict::NoRevocationInformation: return QStringLiteral("NoRevocationInformation");
    case ChainVerdict::MalformedInput:          return QStringLiteral("MalformedInput");
    case ChainVerdict::Unknown:                 break;
    }
    return QStringLiteral("Unknown");
}

ChainValidationResult CertChainValidator::validate(
    const ChainValidationRequest& req)
{
    ChainValidationResult result;

    X509* leaf = derToX509(req.leafDer);
    if (!leaf) {
        result.verdict = ChainVerdict::MalformedInput;
        result.message = QStringLiteral(
            "Leaf certificate is absent or is not a valid DER X.509 certificate");
        return result;
    }

    X509_STORE* store = X509_STORE_new();
    if (!store) {
        X509_free(leaf);
        result.verdict = ChainVerdict::Unknown;
        result.message = QStringLiteral("OpenSSL store allocation failed");
        return result;
    }

    // ---- Trust sources. This is the seam: anchors first, and OpenSSL's
    // default cert paths ONLY when the caller opted in.
    //
    // GitHub #212: this second source is NOT the Windows certificate store and
    // on Windows contributes nothing on a stock machine. See the DECISION block
    // in CertChainValidator.h. Deliberate; pinned by
    // tests/test_cert_store_trust_policy.cpp.
    result.anchorsLoaded = req.anchors.loadIntoStore(store);
    if (req.useOpenSslDefaultPaths) {
        X509_STORE_set_default_paths(store);
    }

    // ---- CRLs. Supplying any turns on leaf revocation checking; supplying
    // none leaves it off entirely (callers doing OCSP handle it themselves).
    bool crlCheck = false;
    for (const auto& crlDer : req.crlsDer) {
        X509_CRL* crl = derToX509Crl(crlDer);
        if (!crl) continue;
        if (X509_STORE_add_crl(store, crl) != 1) ERR_clear_error();
        X509_CRL_free(crl);
        crlCheck = true;
    }

    // ---- Untrusted path-building material. These can complete a chain but
    // can never be the source of trust -- they are not in the store.
    STACK_OF(X509)* untrusted = sk_X509_new_null();
    for (const auto& der : req.untrustedDer) {
        X509* c = derToX509(der);
        if (c) sk_X509_push(untrusted, c);
    }

    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    if (!ctx) {
        if (untrusted) sk_X509_pop_free(untrusted, X509_free);
        X509_STORE_free(store);
        X509_free(leaf);
        result.verdict = ChainVerdict::Unknown;
        result.message = QStringLiteral("OpenSSL store context allocation failed");
        return result;
    }

    if (X509_STORE_CTX_init(ctx, store, leaf, untrusted) != 1) {
        ERR_clear_error();
        X509_STORE_CTX_free(ctx);
        if (untrusted) sk_X509_pop_free(untrusted, X509_free);
        X509_STORE_free(store);
        X509_free(leaf);
        result.verdict = ChainVerdict::Unknown;
        result.message = QStringLiteral("OpenSSL store context init failed");
        return result;
    }

    if (crlCheck) {
        X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(ctx);
        if (param) X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CRL_CHECK);
    }

    // Pinning the verification instant. X509_STORE_CTX_set_time sets
    // X509_V_FLAG_USE_CHECK_TIME internally, so the validity-window
    // comparison stops consulting the wall clock.
    if (req.verificationTimeUnix != 0) {
        X509_STORE_CTX_set_time(ctx, 0,
                                static_cast<time_t>(req.verificationTimeUnix));
    }

    const int rc = X509_verify_cert(ctx);
    const int err = X509_STORE_CTX_get_error(ctx);
    result.errorDepth = X509_STORE_CTX_get_error_depth(ctx);

    STACK_OF(X509)* built = X509_STORE_CTX_get0_chain(ctx);
    result.chainLength = built ? sk_X509_num(built) : 0;

    // Hand the caller the path itself, not just its length (F47 item F2,
    // phase-49 — see ChainValidationResult::pathDer). Re-encoding rather than
    // handing out X509* keeps this class's "never leaks OpenSSL types" shape
    // and keeps the result trivially copyable/ownable. A cert that fails to
    // re-encode is skipped, and the size mismatch that produces is caught by
    // the invariant assertion below.
    //
    // ONLY WHEN rc == 1 (phase-49 review finding M1). X509_STORE_CTX_get0_chain
    // returns a PARTIALLY BUILT path on failure, whose upper elements came out
    // of the caller's untrusted material — for LtvValidator that is the
    // document's own attacker-controlled /DSS — and may never have had a
    // signature checked at all. The documented contract on pathDer is "the
    // certificates whose signatures were VERIFIED", and LtvValidator's indirect
    // CRL rule rests on exactly those words: ancestorsAbove() hands these certs
    // to crlIsAuthorized with no re-verification. Populating it on the failure
    // path made the contract false and left the next caller one `if` away from
    // trusting unverified material. Gating it here makes that safe by
    // construction rather than by every caller remembering.
    if (rc == 1) {
        for (int i = 0; i < result.chainLength; ++i) {
            X509* c = sk_X509_value(built, i);
            if (!c) continue;
            unsigned char* der = nullptr;
            const int len = i2d_X509(c, &der);
            if (len > 0 && der) {
                result.pathDer.emplace_back(der, der + len);
            } else {
                ERR_clear_error();
            }
            if (der) OPENSSL_free(der);
        }
        // A partially re-encoded path would silently shorten the revocation
        // walk in LtvValidator, which fails OPEN. Refuse to report a path we
        // could not reproduce in full.
        if (static_cast<int>(result.pathDer.size()) != result.chainLength) {
            result.pathDer.clear();
        }
    }

    if (rc == 1) {
        result.verdict = ChainVerdict::Trusted;
        result.opensslError = X509_V_OK;
        result.errorDepth = -1;
        result.message = QStringLiteral("Chain terminates at a trusted anchor");
    } else {
        result.opensslError = err;
        result.verdict = verdictForError(err);
        result.message = QStringLiteral("%1 (X509_V_ERR %2: %3, depth %4)")
                             .arg(chainVerdictToString(result.verdict))
                             .arg(err)
                             .arg(QString::fromUtf8(
                                 X509_verify_cert_error_string(err)))
                             .arg(result.errorDepth);
    }

    X509_STORE_CTX_free(ctx);
    if (untrusted) sk_X509_pop_free(untrusted, X509_free);
    X509_STORE_free(store);
    X509_free(leaf);
    ERR_clear_error();
    return result;
}

} // namespace pdfe::signatures
