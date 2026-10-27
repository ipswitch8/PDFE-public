// -*- coding: utf-8 -*-
#include "CrlUrlExtractor.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>
#include <openssl/safestack.h>

#include <algorithm>
#include <cctype>

namespace pdfe::signatures {

namespace {

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

// Walk one DistributionPoint entry's fullName GeneralNames stack and
// append URIs to @p out. fullName is the only DistributionPointName
// variant we surface; nameRelativeToCRLIssuer is rare and not directly
// fetchable via HTTP.
void collectFromDistPoint(DIST_POINT* dp, std::vector<std::string>& out)
{
    if (!dp || !dp->distpoint) return;
    if (dp->distpoint->type != 0) return; // type 0 = fullName

    GENERAL_NAMES* names = dp->distpoint->name.fullname;
    if (!names) return;

    int n = sk_GENERAL_NAME_num(names);
    for (int i = 0; i < n; ++i) {
        GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
        if (!gn || gn->type != GEN_URI) continue;

        const ASN1_IA5STRING* uri = gn->d.uniformResourceIdentifier;
        if (!uri || !uri->data || uri->length <= 0) continue;

        std::string url(reinterpret_cast<const char*>(uri->data),
                        static_cast<std::size_t>(uri->length));
        if (!isAllowedScheme(url)) continue;

        out.push_back(std::move(url));
    }
}

} // anonymous namespace

bool CrlUrlExtractor::parse(X509* cert, std::vector<std::string>& urls)
{
    if (!cert) return false;

    CRL_DIST_POINTS* dps = static_cast<CRL_DIST_POINTS*>(
        X509_get_ext_d2i(cert, NID_crl_distribution_points,
                         nullptr, nullptr));
    if (!dps) return false;

    int n = sk_DIST_POINT_num(dps);
    for (int i = 0; i < n; ++i) {
        DIST_POINT* dp = sk_DIST_POINT_value(dps, i);
        collectFromDistPoint(dp, urls);
    }

    CRL_DIST_POINTS_free(dps);
    return true;
}

bool CrlUrlExtractor::parseDer(const uint8_t* der, std::size_t derLen,
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
