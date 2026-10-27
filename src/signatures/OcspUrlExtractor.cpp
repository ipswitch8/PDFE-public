// -*- coding: utf-8 -*-
#include "OcspUrlExtractor.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>
#include <openssl/objects.h>
#include <openssl/safestack.h>

#include <algorithm>
#include <cctype>

namespace pdfe::signatures {

namespace {

// Lower-case ASCII for http/https scheme matching. The URL's host portion
// is left untouched; only the scheme prefix is normalised for comparison.
bool startsWithCi(const std::string& s, const char* prefix)
{
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(s[i])));
        char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

bool isAllowedScheme(const std::string& url)
{
    return startsWithCi(url, "http://") || startsWithCi(url, "https://");
}

} // anonymous namespace

bool OcspUrlExtractor::parse(X509* cert, std::vector<std::string>& urls)
{
    if (!cert) return false;

    // X509_get_ext_d2i with NID_info_access yields STACK_OF(ACCESS_DESCRIPTION).
    // We must free with AUTHORITY_INFO_ACCESS_free.
    AUTHORITY_INFO_ACCESS* aia = static_cast<AUTHORITY_INFO_ACCESS*>(
        X509_get_ext_d2i(cert, NID_info_access, nullptr, nullptr));
    if (!aia) return false;

    bool any = false;
    int n = sk_ACCESS_DESCRIPTION_num(aia);
    for (int i = 0; i < n; ++i) {
        ACCESS_DESCRIPTION* ad = sk_ACCESS_DESCRIPTION_value(aia, i);
        if (!ad || !ad->method) continue;

        // Only id-ad-ocsp accessMethod entries.
        if (OBJ_obj2nid(ad->method) != NID_ad_OCSP) continue;
        if (!ad->location) continue;
        if (ad->location->type != GEN_URI) continue;

        const ASN1_IA5STRING* uri = ad->location->d.uniformResourceIdentifier;
        if (!uri || !uri->data || uri->length <= 0) continue;

        std::string url(reinterpret_cast<const char*>(uri->data),
                        static_cast<std::size_t>(uri->length));
        if (!isAllowedScheme(url)) continue;

        urls.push_back(std::move(url));
        any = true;
    }

    AUTHORITY_INFO_ACCESS_free(aia);
    // Return true if the AIA extension parsed successfully, even if no
    // OCSP entries were http/https. Callers can detect "no usable URLs"
    // via urls.empty().
    (void)any;
    return true;
}

bool OcspUrlExtractor::parseDer(const uint8_t* der, std::size_t derLen,
                                 std::vector<std::string>& urls)
{
    if (!der || derLen == 0) return false;
    if (derLen > static_cast<std::size_t>(0x7fffffff)) return false;

    const unsigned char* p = der;
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(derLen));
    if (!cert) return false;

    bool ok = parse(cert, urls);
    X509_free(cert);
    return ok;
}

} // namespace pdfe::signatures
