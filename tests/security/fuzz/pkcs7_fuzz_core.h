// -*- coding: utf-8 -*-
#pragma once

// Shared PKCS#7 verification-path fuzz core (phase-73).
//
// ONE BODY, TWO DRIVERS, AND THAT IS THE POINT.
//
// This body used to live inside tests/test_pkcs7_fuzz.cpp as a static helper,
// next to an `LLVMFuzzerTestOneInput` that its own header admitted was "not
// linked as a fuzzer target". Promoting that entry point into a real
// coverage-guided target (fuzz_pkcs7_verify) offered two ways to do it: copy
// the body into the harness, or lift it here and have both include it.
//
// Copying was rejected on the evidence of DCA 0036, where a fix applied to
// `pdfLiteralEscape` was not applied to `xmlEscape` three functions away in the
// same file. Two copies of a parse body are two things that can be hardened
// separately, and the seed-replay oracle would then be asserting about code the
// campaign no longer runs. The gtest and the fuzzer must exercise the SAME
// bytes of logic or the gtest is not evidence about the fuzzer.
//
// The core therefore carries no GoogleTest dependency: it RETURNS its
// invariants (see Pkcs7ProbeResult) and lets each driver decide what to do with
// them. The gtest asserts on them; the fuzzer aborts on them, so a violation is
// a finding libFuzzer records rather than a silently-ignored bool.

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <cstddef>
#include <cstdint>

namespace pdfe::fuzz::pkcs7 {

/// Outcome of one probe. `parsed == false` is a perfectly good result -- most
/// fuzzer-generated inputs are not PKCS#7 -- so it is reported, not asserted.
struct Pkcs7ProbeResult {
    /// d2i_PKCS7 accepted the input.
    bool parsed = false;

    /// INVARIANT, only meaningful when `parsed`: a successfully-decoded PKCS7
    /// carries a resolvable content-type OID (the ASN.1 CHOICE selector). d2i
    /// must never yield a typeless object on success -- downstream
    /// PKCS7_verify would dereference it.
    bool hasContentType = false;

    /// Number of signers PKCS7_get0_signers returned (0 when none/NULL).
    int signerCount = 0;
};

/// Drive `size` bytes through the same OpenSSL calls as
/// Pkcs7OpenSslBridge::openssl_check_digest / openssl_check_certificate:
/// d2i_PKCS7 -> PKCS7_get0_signers -> PKCS7_verify.
inline Pkcs7ProbeResult probe(const std::uint8_t* data, std::size_t size)
{
    Pkcs7ProbeResult r;
    if (data == nullptr || size == 0) return r;

    const unsigned char* p = data;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(size));
    if (!p7) {
        ERR_clear_error();
        return r;
    }
    r.parsed = true;
    r.hasContentType = (p7->type != nullptr);
    if (p7->type) {
        // A known ContentType keeps d2i honest; an unknown OID (nid == 0) is a
        // legitimate parse result, so we only require that it be resolvable.
        (void)OBJ_obj2nid(p7->type);
    }

    // Signer extraction (mirrors openssl_check_certificate).
    STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, 0);
    if (signers) {
        r.signerCount = sk_X509_num(signers);
        for (int i = 0; i < r.signerCount; ++i) {
            X509* cert = sk_X509_value(signers, i);
            (void)X509_get_subject_name(cert);
        }
        sk_X509_free(signers);
    }
    ERR_clear_error();

    // Digest verification with empty content (mirrors openssl_check_digest).
    // The verify will fail, but the SignedData ASN.1 walk is the code path
    // that matters here.
    BIO* bio = BIO_new_mem_buf("", 0);
    if (bio) {
        X509_STORE* store = X509_STORE_new();
        if (store) {
            const int flags = PKCS7_NOVERIFY | PKCS7_BINARY;
            PKCS7_verify(p7, nullptr, store, bio, nullptr, flags);
            ERR_clear_error();
            X509_STORE_free(store);
        }
        BIO_free(bio);
    }

    PKCS7_free(p7);
    return r;
}

} // namespace pdfe::fuzz::pkcs7
