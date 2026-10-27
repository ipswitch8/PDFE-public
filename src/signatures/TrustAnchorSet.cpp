// -*- coding: utf-8 -*-
// Meta-audit follow-up F23 (gap PDF-39): TrustAnchorSet -- see header.
#include "TrustAnchorSet.h"

#include <openssl/x509.h>
#include <openssl/err.h>

namespace pdfe::signatures {

namespace {

// Parse DER -> X509, draining the OpenSSL error queue on failure so a
// rejected anchor cannot surface as a spurious error on the next operation.
X509* derToX509(const char* data, int len)
{
    if (!data || len <= 0) return nullptr;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(len));
    if (!cert) ERR_clear_error();
    return cert;
}

} // namespace

bool TrustAnchorSet::addDer(const QByteArray& der)
{
    if (der.isEmpty()) return false;
    // Validate by round-tripping through OpenSSL. We discard the parsed cert
    // and keep the DER: storing the bytes (not an X509*) keeps the set a
    // plain copyable value with no refcount semantics to get wrong.
    X509* probe = derToX509(der.constData(), static_cast<int>(der.size()));
    if (!probe) return false;
    X509_free(probe);
    anchorsDer_.append(der);
    return true;
}

bool TrustAnchorSet::addDer(const std::vector<uint8_t>& der)
{
    if (der.empty()) return false;
    return addDer(QByteArray(reinterpret_cast<const char*>(der.data()),
                             static_cast<int>(der.size())));
}

int TrustAnchorSet::loadIntoStore(X509_STORE* store) const
{
    if (!store) return 0;
    int added = 0;
    for (const auto& der : anchorsDer_) {
        X509* cert = derToX509(der.constData(), static_cast<int>(der.size()));
        if (!cert) continue;
        // X509_STORE_add_cert bumps the refcount, so the local handle is
        // always freed. A duplicate add returns 0 and pushes
        // X509_R_CERT_ALREADY_IN_HASH_TABLE -- drain it and count the anchor
        // as present, because it is.
        if (X509_STORE_add_cert(store, cert) == 1) {
            ++added;
        } else {
            ERR_clear_error();
        }
        X509_free(cert);
    }
    return added;
}

} // namespace pdfe::signatures
