#include "CrlParser.h"

#include <openssl/x509.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace pdfe::signatures {

namespace {

// CRLs can be larger than OCSP responses (revocation lists for major CAs run
// to tens of MiB). 64 MiB is a generous upper bound that still rejects
// pathological / accidentally-truncated huge files cleanly.
constexpr std::size_t kMaxCrlBytes = 64u * 1024u * 1024u;

bool readFileBinary(const std::filesystem::path& path,
                    std::vector<uint8_t>& out, std::string& err)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        err = "File not found";
        return false;
    }
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        err = "Cannot stat file";
        return false;
    }
    if (sz == 0) {
        err = "Empty file";
        return false;
    }
    if (sz > kMaxCrlBytes) {
        err = "File too large for CRL";
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "Cannot open file";
        return false;
    }
    out.resize(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    if (!f) {
        err = "Short read";
        out.clear();
        return false;
    }
    return true;
}

int64_t asn1TimeToEpoch(const ASN1_TIME* t)
{
    if (!t) return 0;
    struct tm tmv{};
    if (!ASN1_TIME_to_tm(t, &tmv)) return 0;
#if defined(_WIN32)
    time_t tt = _mkgmtime(&tmv);
#else
    time_t tt = timegm(&tmv);
#endif
    if (tt == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(tt);
}

// Format an X509_NAME in RFC 2253 form. Returns empty string on failure.
std::string nameToRfc2253(X509_NAME* name)
{
    if (!name) return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return {};
    std::string result;
    if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) >= 0) {
        char* data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        if (data && len > 0) {
            result.assign(data, static_cast<std::size_t>(len));
        }
    }
    BIO_free(bio);
    return result;
}

} // namespace

bool CrlParser::parse(const std::filesystem::path& path, CrlInfo& out)
{
    out = {};
    std::vector<uint8_t> bytes;
    std::string err;
    if (!readFileBinary(path, bytes, err)) {
        out.statusMessage = err;
        return false;
    }
    return parseBytes(bytes.data(), bytes.size(), out);
}

bool CrlParser::parseBytes(const uint8_t* der, std::size_t len, CrlInfo& out)
{
    out = {};
    if (!der || len == 0) {
        out.statusMessage = "Empty input";
        return false;
    }
    if (len > kMaxCrlBytes) {
        out.statusMessage = "Buffer too large for CRL";
        return false;
    }

    const unsigned char* p = der;
    X509_CRL* crl = d2i_X509_CRL(nullptr, &p, static_cast<long>(len));
    if (!crl) {
        ERR_clear_error();
        out.statusMessage = "Malformed CRL (d2i_X509_CRL failed)";
        return false;
    }

    out.derBytes.assign(der, der + len);

    X509_NAME* issuer = X509_CRL_get_issuer(crl);
    out.issuerDN = nameToRfc2253(issuer);

    // get0_lastUpdate / get0_nextUpdate are non-owning views; the returned
    // pointers are valid for the lifetime of @c crl.
    out.thisUpdate = asn1TimeToEpoch(X509_CRL_get0_lastUpdate(crl));
    out.nextUpdate = asn1TimeToEpoch(X509_CRL_get0_nextUpdate(crl));

    if (out.issuerDN.empty()) {
        out.statusMessage = "Parsed CRL has empty issuer DN";
    } else {
        out.statusMessage = "ok";
    }

    X509_CRL_free(crl);
    return true;
}

} // namespace pdfe::signatures
