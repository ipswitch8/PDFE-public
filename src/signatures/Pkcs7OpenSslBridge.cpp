#include "Pkcs7OpenSslBridge.h"
#include "CertChainExtractor.h"
#include "CertChainValidator.h"
#include "TsaClient.h"
#include "TrustAnchorManager.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <vector>
#include <cstring>
#include <algorithm>

namespace pdfe::signatures {

// ============================================================================
// Internal: OpenSSL signer that implements pdf_pkcs7_signer vtable
// ============================================================================

struct OpenSslSignerData {
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* chain = nullptr;
    int refcount = 1;
    std::string tsaUrl;
};

static OpenSslSignerData* getSignerData(pdf_pkcs7_signer* signer)
{
    auto** dataPtr = reinterpret_cast<OpenSslSignerData**>(
        reinterpret_cast<char*>(signer) + sizeof(pdf_pkcs7_signer));
    return *dataPtr;
}

static pdf_pkcs7_signer* openssl_keep_signer(fz_context* /*ctx*/, pdf_pkcs7_signer* signer)
{
    if (!signer) return nullptr;
    auto* data = getSignerData(signer);
    if (data) data->refcount++;
    return signer;
}

static void openssl_drop_signer(fz_context* ctx, pdf_pkcs7_signer* signer)
{
    if (!signer) return;
    auto* data = getSignerData(signer);
    if (data && --data->refcount <= 0) {
        if (data->chain) sk_X509_pop_free(data->chain, X509_free);
        if (data->cert) X509_free(data->cert);
        if (data->pkey) EVP_PKEY_free(data->pkey);
        delete data;
        fz_free(ctx, signer);
    }
}

static pdf_pkcs7_distinguished_name* openssl_get_signing_name(fz_context* ctx, pdf_pkcs7_signer* signer)
{
    if (!signer) return nullptr;
    auto* data = getSignerData(signer);
    if (!data || !data->cert) return nullptr;

    X509_NAME* subj = X509_get_subject_name(data->cert);
    if (!subj) return nullptr;

    auto* dn = static_cast<pdf_pkcs7_distinguished_name*>(
        fz_calloc(ctx, 1, sizeof(pdf_pkcs7_distinguished_name)));

    auto extractField = [&](int nid) -> char* {
        int idx = X509_NAME_get_index_by_NID(subj, nid, -1);
        if (idx < 0) return nullptr;
        X509_NAME_ENTRY* entry = X509_NAME_get_entry(subj, idx);
        if (!entry) return nullptr;
        ASN1_STRING* asn1 = X509_NAME_ENTRY_get_data(entry);
        if (!asn1) return nullptr;
        unsigned char* utf8 = nullptr;
        int len = ASN1_STRING_to_UTF8(&utf8, asn1);
        if (len <= 0 || !utf8) return nullptr;
        char* result = fz_strdup(ctx, reinterpret_cast<const char*>(utf8));
        OPENSSL_free(utf8);
        return result;
    };

    dn->cn = extractField(NID_commonName);
    dn->o = extractField(NID_organizationName);
    dn->ou = extractField(NID_organizationalUnitName);
    dn->email = extractField(NID_pkcs9_emailAddress);
    dn->c = extractField(NID_countryName);

    return dn;
}

static size_t openssl_max_digest_size(fz_context* /*ctx*/, pdf_pkcs7_signer* signer)
{
    auto* data = getSignerData(signer);
    // TSA tokens add 3-5KB; use 16KB when TSA is configured, 8KB otherwise
    if (data && !data->tsaUrl.empty())
        return 16384;
    return 8192;
}

// Embed a TSA timestamp token as an unauthenticated attribute in the PKCS7 SignerInfo.
// Returns true on success, false on failure (non-fatal — signature is still valid without it).
static bool embedTimestampToken(PKCS7* p7, const std::string& tsaUrl)
{
    if (tsaUrl.empty() || !p7) return false;

    STACK_OF(PKCS7_SIGNER_INFO)* signerInfos = PKCS7_get_signer_info(p7);
    if (!signerInfos || sk_PKCS7_SIGNER_INFO_num(signerInfos) == 0)
        return false;

    PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(signerInfos, 0);
    if (!si) return false;

    // Get the encrypted digest (signature value) to timestamp
    ASN1_OCTET_STRING* encDigest = si->enc_digest;
    if (!encDigest || !encDigest->data || encDigest->length <= 0)
        return false;

    // SHA-256 hash of the signature value (message imprint for TSA)
    unsigned char sha256[SHA256_DIGEST_LENGTH];
    SHA256(encDigest->data, encDigest->length, sha256);

    // Request timestamp from TSA
    auto tsaResult = pdfe::signatures::TsaClient::requestTimestamp(
        tsaUrl, sha256, SHA256_DIGEST_LENGTH);

    if (!tsaResult.success || tsaResult.token.empty())
        return false;

    // Parse the token back as ASN1_TYPE for attribute embedding
    const unsigned char* tokenPtr = tsaResult.token.data();
    PKCS7* tsToken = d2i_PKCS7(nullptr, &tokenPtr,
                                static_cast<long>(tsaResult.token.size()));
    if (!tsToken) return false;

    // Serialize as ASN1_TYPE (SEQUENCE)
    unsigned char* tokenDer = nullptr;
    int tokenDerLen = i2d_PKCS7(tsToken, &tokenDer);
    PKCS7_free(tsToken);

    if (tokenDerLen <= 0 || !tokenDer) return false;

    // OID for id-smime-aa-timeStampToken (1.2.840.113549.1.9.16.2.14)
    ASN1_OBJECT* tsOid = OBJ_txt2obj("1.2.840.113549.1.9.16.2.14", 1);
    if (!tsOid) {
        OPENSSL_free(tokenDer);
        return false;
    }

    // Build unauthenticated attribute directly (PKCS7_add_attribute adds
    // to authenticated attrs, which would break the signature digest)
    if (!si->unauth_attr) {
        si->unauth_attr = sk_X509_ATTRIBUTE_new_null();
    }

    X509_ATTRIBUTE* attr = X509_ATTRIBUTE_new();
    if (!attr) {
        ASN1_OBJECT_free(tsOid);
        OPENSSL_free(tokenDer);
        return false;
    }

    X509_ATTRIBUTE_set1_object(attr, tsOid);
    X509_ATTRIBUTE_set1_data(attr, V_ASN1_SEQUENCE, tokenDer, tokenDerLen);
    sk_X509_ATTRIBUTE_push(si->unauth_attr, attr);

    ASN1_OBJECT_free(tsOid);
    OPENSSL_free(tokenDer);
    return true;
}

static int openssl_create_digest(fz_context* ctx, pdf_pkcs7_signer* signer,
                                  fz_stream* in, unsigned char* digest, size_t digest_len)
{
    if (!signer || !in) return 0;
    auto* data = getSignerData(signer);
    if (!data || !data->pkey || !data->cert) return 0;

    // Read all data from the stream
    std::vector<unsigned char> inputData;
    {
        unsigned char buf[4096];
        while (true) {
            size_t n = fz_read(ctx, in, buf, sizeof(buf));
            if (n == 0) break;
            inputData.insert(inputData.end(), buf, buf + n);
        }
    }

    // Create BIO from input data
    BIO* bio_in = BIO_new_mem_buf(inputData.data(), static_cast<int>(inputData.size()));
    if (!bio_in) return 0;

    // Create PKCS7 signed data
    int flags = PKCS7_DETACHED | PKCS7_BINARY | PKCS7_NOSMIMECAP;
    PKCS7* p7 = PKCS7_sign(data->cert, data->pkey, data->chain, bio_in, flags);
    BIO_free(bio_in);

    if (!p7) return 0;

    // Embed TSA timestamp if configured (non-fatal on failure)
    if (!data->tsaUrl.empty()) {
        embedTimestampToken(p7, data->tsaUrl);
    }

    // Serialize the PKCS7 to DER
    unsigned char* der = nullptr;
    int der_len = i2d_PKCS7(p7, &der);
    PKCS7_free(p7);

    if (der_len <= 0 || !der) return 0;

    if (static_cast<size_t>(der_len) > digest_len) {
        OPENSSL_free(der);
        return 0;
    }

    std::memcpy(digest, der, der_len);
    if (static_cast<size_t>(der_len) < digest_len) {
        std::memset(digest + der_len, 0, digest_len - der_len);
    }

    OPENSSL_free(der);
    return der_len;
}

// ============================================================================
// Internal: OpenSSL verifier that implements pdf_pkcs7_verifier vtable
// ============================================================================

static void openssl_drop_verifier(fz_context* ctx, pdf_pkcs7_verifier* verifier)
{
    if (verifier) {
        fz_free(ctx, verifier);
    }
}

static pdf_signature_error openssl_check_certificate(
    fz_context* /*ctx*/, pdf_pkcs7_verifier* /*verifier*/,
    unsigned char* signature, size_t len)
{
    if (!signature || len == 0)
        return PDF_SIGNATURE_ERROR_UNKNOWN;

    // Parse the PKCS7 structure
    const unsigned char* p = signature;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(len));
    if (!p7) return PDF_SIGNATURE_ERROR_UNKNOWN;

    // Get the signers from the PKCS7
    STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, 0);
    if (!signers || sk_X509_num(signers) == 0) {
        if (signers) sk_X509_free(signers);
        PKCS7_free(p7);
        return PDF_SIGNATURE_ERROR_NO_CERTIFICATE;
    }

    X509* signer_cert = sk_X509_value(signers, 0);

    // Meta-audit follow-up F23 (gap PDF-39): the inline X509_STORE build and
    // X509_verify_cert call that used to live here now go through
    // CertChainValidator, so this path and LtvValidator share ONE chain
    // implementation and one verdict taxonomy. Trust sources are unchanged:
    //
    // R-12 (phase-4 SECURITY-ORACLE): OpenSSL's default certificate paths
    // (NOT the Windows certificate store, #212) PLUS the
    // user's explicit trust anchors (TrustAnchorManager), which is purely
    // ADDITIVE — nothing that verified before stops verifying. It only lets a
    // cert the user has EXPLICITLY chosen to trust (Acrobat-style "trusted
    // identities") chain successfully, making the trusted-vs-untrusted
    // distinction real and observable. When the anchor list is empty (the
    // default) behaviour is byte-for-byte unchanged.
    ChainValidationRequest chainReq;
    {
        unsigned char* leafDer = nullptr;
        const int leafLen = i2d_X509(signer_cert, &leafDer);
        if (leafLen > 0 && leafDer) {
            chainReq.leafDer.assign(leafDer, leafDer + leafLen);
        }
        if (leafDer) OPENSSL_free(leafDer);
    }
    chainReq.anchors = TrustAnchorManager::instance().snapshot();
    // GitHub #212: OpenSSL's default cert paths, NOT the Windows certificate
    // store. See the DECISION block in CertChainValidator.h.
    chainReq.useOpenSslDefaultPaths = true;

    const ChainValidationResult chainRes =
        CertChainValidator::validate(chainReq);

    pdf_signature_error result;
    if (chainRes.trusted()) {
        result = PDF_SIGNATURE_ERROR_OKAY;
    } else {
        switch (chainRes.opensslError) {
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
            result = PDF_SIGNATURE_ERROR_SELF_SIGNED;
            break;
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
            result = PDF_SIGNATURE_ERROR_SELF_SIGNED_IN_CHAIN;
            break;
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_CERT_UNTRUSTED:
            result = PDF_SIGNATURE_ERROR_NOT_TRUSTED;
            break;
        default:
            result = PDF_SIGNATURE_ERROR_UNKNOWN;
            break;
        }
    }

    sk_X509_free(signers);
    PKCS7_free(p7);

    return result;
}

static pdf_signature_error openssl_check_digest(
    fz_context* ctx, pdf_pkcs7_verifier* /*verifier*/,
    fz_stream* in, unsigned char* signature, size_t len)
{
    if (!in || !signature || len == 0)
        return PDF_SIGNATURE_ERROR_UNKNOWN;

    // Read all data from the stream
    std::vector<unsigned char> inputData;
    {
        unsigned char buf[4096];
        while (true) {
            size_t n = fz_read(ctx, in, buf, sizeof(buf));
            if (n == 0) break;
            inputData.insert(inputData.end(), buf, buf + n);
        }
    }

    // Parse the PKCS7 structure
    const unsigned char* p = signature;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(len));
    if (!p7) return PDF_SIGNATURE_ERROR_UNKNOWN;

    // Create BIO from the signed data
    BIO* bio_content = BIO_new_mem_buf(inputData.data(), static_cast<int>(inputData.size()));
    if (!bio_content) {
        PKCS7_free(p7);
        return PDF_SIGNATURE_ERROR_UNKNOWN;
    }

    // Create an empty certificate store (we check certs separately)
    X509_STORE* store = X509_STORE_new();

    // Verify the PKCS7 signature (digest only, skip cert chain check)
    int flags = PKCS7_NOVERIFY | PKCS7_BINARY;
    int ok = PKCS7_verify(p7, nullptr, store, bio_content, nullptr, flags);

    X509_STORE_free(store);
    BIO_free(bio_content);
    PKCS7_free(p7);

    return ok == 1 ? PDF_SIGNATURE_ERROR_OKAY : PDF_SIGNATURE_ERROR_DIGEST_FAILURE;
}

static pdf_pkcs7_distinguished_name* openssl_get_signatory(
    fz_context* ctx, pdf_pkcs7_verifier* /*verifier*/,
    unsigned char* signature, size_t len)
{
    if (!signature || len == 0) return nullptr;

    // Parse PKCS7
    const unsigned char* p = signature;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(len));
    if (!p7) return nullptr;

    STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, 0);
    if (!signers || sk_X509_num(signers) == 0) {
        if (signers) sk_X509_free(signers);
        PKCS7_free(p7);
        return nullptr;
    }

    X509* cert = sk_X509_value(signers, 0);
    X509_NAME* subj = X509_get_subject_name(cert);

    auto* dn = static_cast<pdf_pkcs7_distinguished_name*>(
        fz_calloc(ctx, 1, sizeof(pdf_pkcs7_distinguished_name)));

    auto extractField = [&](int nid) -> char* {
        int idx = X509_NAME_get_index_by_NID(subj, nid, -1);
        if (idx < 0) return nullptr;
        X509_NAME_ENTRY* entry = X509_NAME_get_entry(subj, idx);
        if (!entry) return nullptr;
        ASN1_STRING* asn1 = X509_NAME_ENTRY_get_data(entry);
        if (!asn1) return nullptr;
        unsigned char* utf8 = nullptr;
        int l = ASN1_STRING_to_UTF8(&utf8, asn1);
        if (l <= 0 || !utf8) return nullptr;
        char* result = fz_strdup(ctx, reinterpret_cast<const char*>(utf8));
        OPENSSL_free(utf8);
        return result;
    };

    dn->cn = extractField(NID_commonName);
    dn->o = extractField(NID_organizationName);
    dn->ou = extractField(NID_organizationalUnitName);
    dn->email = extractField(NID_pkcs9_emailAddress);
    dn->c = extractField(NID_countryName);

    sk_X509_free(signers);
    PKCS7_free(p7);

    return dn;
}

// ============================================================================
// Pkcs7OpenSslBridge Implementation
// ============================================================================

class Pkcs7OpenSslBridge::Impl {
public:
    bool available = true; // OpenSSL is always linked
};

Pkcs7OpenSslBridge::Pkcs7OpenSslBridge()
    : impl_(std::make_unique<Impl>())
{
}

Pkcs7OpenSslBridge::~Pkcs7OpenSslBridge() = default;

bool Pkcs7OpenSslBridge::isAvailable() const
{
    return impl_->available;
}

pdf_pkcs7_signer* Pkcs7OpenSslBridge::readPfx(fz_context* ctx,
                                                const char* pfxPath,
                                                const char* password)
{
    if (!ctx) return nullptr;
    if (!pfxPath) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Null PFX path");
    }

    // Read PFX file
    FILE* fp = fopen(pfxPath, "rb");
    if (!fp) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Cannot open PFX file: %s", pfxPath);
    }

    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Use C-style allocation -- no RAII objects on stack across fz_throw (longjmp)
    unsigned char* pfxData = static_cast<unsigned char*>(malloc(static_cast<size_t>(fileSize)));
    if (!pfxData) {
        fclose(fp);
        fz_throw(ctx, FZ_ERROR_SYSTEM, "Failed to allocate %ld bytes for PFX file", fileSize);
    }
    size_t readBytes = fread(pfxData, 1, fileSize, fp);
    fclose(fp);

    if (static_cast<long>(readBytes) != fileSize) {
        free(pfxData);
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Failed to read PFX file: %s", pfxPath);
    }

    // Parse PKCS12 (d2i_PKCS12 copies internally, so we can free pfxData after)
    const unsigned char* p = pfxData;
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, static_cast<long>(fileSize));
    free(pfxData);
    if (!p12) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Invalid PFX file: %s", pfxPath);
    }

    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* chain = nullptr;

    int ok = PKCS12_parse(p12, password ? password : "", &pkey, &cert, &chain);
    PKCS12_free(p12);

    if (!ok || !pkey || !cert) {
        if (pkey) EVP_PKEY_free(pkey);
        if (cert) X509_free(cert);
        if (chain) sk_X509_pop_free(chain, X509_free);
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Failed to parse PFX (wrong password?): %s", pfxPath);
    }

    // Allocate signer struct + data pointer
    size_t allocSize = sizeof(pdf_pkcs7_signer) + sizeof(OpenSslSignerData*);
    auto* signer = static_cast<pdf_pkcs7_signer*>(fz_calloc(ctx, 1, allocSize));

    // Fill vtable
    signer->keep = openssl_keep_signer;
    signer->drop = openssl_drop_signer;
    signer->get_signing_name = openssl_get_signing_name;
    signer->max_digest_size = openssl_max_digest_size;
    signer->create_digest = openssl_create_digest;

    // Store data pointer after the vtable struct
    auto* data = new OpenSslSignerData();
    data->pkey = pkey;
    data->cert = cert;
    data->chain = chain;
    *reinterpret_cast<OpenSslSignerData**>(reinterpret_cast<char*>(signer) + sizeof(pdf_pkcs7_signer)) = data;

    return signer;
}

pdf_pkcs7_verifier* Pkcs7OpenSslBridge::createVerifier(fz_context* ctx)
{
    if (!ctx) return nullptr;

    auto* verifier = static_cast<pdf_pkcs7_verifier*>(
        fz_calloc(ctx, 1, sizeof(pdf_pkcs7_verifier)));

    verifier->drop = openssl_drop_verifier;
    verifier->check_certificate = openssl_check_certificate;
    verifier->check_digest = openssl_check_digest;
    verifier->get_signatory = openssl_get_signatory;

    return verifier;
}

void Pkcs7OpenSslBridge::dropSigner(fz_context* ctx, pdf_pkcs7_signer* signer)
{
    if (signer && signer->drop) {
        signer->drop(ctx, signer);
    }
}

void Pkcs7OpenSslBridge::dropVerifier(fz_context* ctx, pdf_pkcs7_verifier* verifier)
{
    if (verifier && verifier->drop) {
        verifier->drop(ctx, verifier);
    }
}

void Pkcs7OpenSslBridge::configureTsa(pdf_pkcs7_signer* signer,
                                       const std::string& tsaUrl)
{
    if (!signer) return;
    auto* data = getSignerData(signer);
    if (data) {
        data->tsaUrl = tsaUrl;
    }
}

VerifyWithChainResult Pkcs7OpenSslBridge::verifyWithChain(
    const uint8_t* pkcs7Der, size_t pkcs7DerLen,
    const uint8_t* signedContent, size_t signedContentLen,
    const VerifyWithChainOptions& opts)
{
    VerifyWithChainResult result;
    if (!pkcs7Der || pkcs7DerLen == 0) {
        result.statusMessage = "Empty PKCS#7 input";
        return result;
    }

    // Parse the PKCS#7 once so we can do both chain extraction and verify.
    const unsigned char* p = pkcs7Der;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(pkcs7DerLen));
    if (!p7) {
        result.statusMessage = "Failed to parse PKCS#7";
        return result;
    }

    // Always pull the chain out, even when later steps fail — the LTV writer
    // wants whatever certs we can get our hands on.
    result.certChain = CertChainExtractor::extractChain(p7);

    // Meta-audit follow-up F26: THE SECOND TRUST DECISION, brought under the
    // same control as the first.
    //
    // This block used to build its own X509_STORE, seed it from the machine's
    // default certificate paths and run X509_verify_cert() inline —
    // a trust evaluation that consulted the machine's ambient store and the
    // user's explicit trust anchors NOT AT ALL. F23 (gap PDF-39) rewired
    // LtvValidator and openssl_check_certificate through CertChainValidator and
    // TrustAnchorSet; phase-23-dispositions.md reads as though both of this
    // file's trust sites were done, but only openssl_check_certificate was.
    // Nothing consulted this verdict, so nothing was WRONG today — but the next
    // caller to honour `chainTrusted` would have inherited an uncontrolled
    // decision. Routed here instead of deleted because the field is part of the
    // published result type and the LTV pipeline is exactly where a trust
    // verdict eventually belongs; see phase-26-dispositions.md.
    //
    // NOTE the untouched X509_STORE_new() in openssl_check_digest() above: that
    // one is deliberately EMPTY and paired with PKCS7_NOVERIFY|PKCS7_BINARY for
    // digest-only verification. It makes no trust decision and must stay as it
    // is. The one below, likewise, is the digest store, not a trust store.
    STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, 0);
    if (signers && sk_X509_num(signers) > 0) {
        X509* signerCert = sk_X509_value(signers, 0);

        ChainValidationRequest chainReq;
        {
            unsigned char* leafDer = nullptr;
            const int leafLen = i2d_X509(signerCert, &leafDer);
            if (leafLen > 0 && leafDer) {
                chainReq.leafDer.assign(leafDer, leafDer + leafLen);
            }
            if (leafDer) OPENSSL_free(leafDer);
        }

        // Everything else in the container is PATH-BUILDING material, never a
        // source of trust: an embedded self-signed cert must not be able to act
        // as its own anchor. CertChainValidator enforces that distinction —
        // untrustedDer builds, anchors trust.
        for (const auto& der : result.certChain) {
            if (der.empty() || der == chainReq.leafDer) continue;
            chainReq.untrustedDer.push_back(der);
        }

        chainReq.anchors = opts.anchors ? *opts.anchors
                                        : TrustAnchorManager::instance().snapshot();
        chainReq.useOpenSslDefaultPaths = opts.useOpenSslDefaultPaths;
        chainReq.verificationTimeUnix = opts.verificationTimeUnix;

        const ChainValidationResult chainRes =
            CertChainValidator::validate(chainReq);

        result.chainTrusted = chainRes.trusted();
        result.chainOpensslError = chainRes.opensslError;
        if (!result.chainTrusted && !chainRes.message.isEmpty()) {
            result.statusMessage = chainRes.message.toStdString();
        }
    }
    if (signers) sk_X509_free(signers);

    // Optional: digest verification when the caller supplied the signed bytes.
    if (signedContent && signedContentLen > 0) {
        BIO* bio = BIO_new_mem_buf(signedContent,
                                    static_cast<int>(signedContentLen));
        if (bio) {
            X509_STORE* emptyStore = X509_STORE_new();
            int flags = PKCS7_NOVERIFY | PKCS7_BINARY;
            int ok = PKCS7_verify(p7, nullptr, emptyStore, bio, nullptr, flags);
            result.digestValid = (ok == 1);
            X509_STORE_free(emptyStore);
            BIO_free(bio);
        }
    }

    if (result.statusMessage.empty()) {
        result.statusMessage = result.chainTrusted ? "Trusted" : "Untrusted (self-signed or unknown CA)";
    }

    PKCS7_free(p7);
    return result;
}

} // namespace pdfe::signatures
