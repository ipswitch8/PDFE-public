#include "OcspResponseParser.h"

#include <openssl/ocsp.h>
#include <openssl/asn1.h>
#include <openssl/err.h>

#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace pdfe::signatures {

namespace {

// Read a file fully into a byte vector. Returns false if the file is missing,
// empty, or unreadable. Bounded at 16 MiB — OCSP responses are typically
// well under 10 KiB; anything larger is suspicious and we reject early to
// avoid feeding arbitrary blobs to OpenSSL.
constexpr std::size_t kMaxOcspBytes = 16u * 1024u * 1024u;

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
    if (sz > kMaxOcspBytes) {
        err = "File too large for OCSP response";
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

// Convert an ASN1_GENERALIZEDTIME (or anything that implicitly converts to
// ASN1_TIME — both are ASN1_STRING under the hood in OpenSSL 3.x) to Unix
// epoch seconds. Returns 0 on any failure (caller should treat 0 as
// "absent").
int64_t generalizedTimeToEpoch(const ASN1_GENERALIZEDTIME* t)
{
    if (!t) return 0;

    // OpenSSL ≥1.1.1 has ASN1_TIME_to_tm which handles both UTCTIME and
    // GENERALIZEDTIME. ASN1_GENERALIZEDTIME and ASN1_TIME are both typedefs
    // for ASN1_STRING in OpenSSL 3.x, so the cast is a no-op at the byte
    // level — just a type-system bridge.
    struct tm tmv{};
    if (!ASN1_TIME_to_tm(reinterpret_cast<const ASN1_TIME*>(t), &tmv))
        return 0;

    // ASN1_TIME_to_tm produces UTC; use timegm-equivalent. MSVC has _mkgmtime.
#if defined(_WIN32)
    time_t tt = _mkgmtime(&tmv);
#else
    time_t tt = timegm(&tmv);
#endif
    if (tt == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(tt);
}

} // namespace

bool OcspResponseParser::parse(const std::filesystem::path& path,
                                OcspResponse& out)
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

bool OcspResponseParser::parseBytes(const uint8_t* der, std::size_t len,
                                     OcspResponse& out)
{
    out = {};
    if (!der || len == 0) {
        out.statusMessage = "Empty input";
        return false;
    }
    if (len > kMaxOcspBytes) {
        out.statusMessage = "Buffer too large for OCSP response";
        return false;
    }

    // d2i_OCSP_RESPONSE advances the pointer; keep a stable copy for the
    // raw bytes in the result.
    const unsigned char* p = der;
    OCSP_RESPONSE* resp = d2i_OCSP_RESPONSE(nullptr, &p, static_cast<long>(len));
    if (!resp) {
        // Drain any OpenSSL errors so they don't leak into later operations.
        ERR_clear_error();
        out.statusMessage = "Malformed OCSP response (d2i_OCSP_RESPONSE failed)";
        return false;
    }

    // Keep the raw DER bytes. This is what gets embedded into /DSS.
    out.derBytes.assign(der, der + len);

    int respStatus = OCSP_response_status(resp);
    if (respStatus != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
        // Per RFC 6960, only "successful" responses carry a BasicOCSPResponse.
        // An unsuccessful but well-formed response is still a valid file we
        // can hand to /DSS — Adobe and many other PDF readers tolerate it.
        out.statusMessage = "OCSP response status not successful (code " +
                             std::to_string(respStatus) + ")";
        OCSP_RESPONSE_free(resp);
        // Return true with raw bytes, no extracted fields.
        return true;
    }

    OCSP_BASICRESP* basic = OCSP_response_get1_basic(resp);
    if (!basic) {
        ERR_clear_error();
        out.statusMessage = "Successful OCSP response without BasicOCSPResponse";
        OCSP_RESPONSE_free(resp);
        return true;
    }

    // Walk the single-responses. We extract status from index 0; thisUpdate /
    // nextUpdate from the same index. A typical OCSP response carries one
    // single-response (one cert). If callers need per-cert extraction we can
    // expose that later — this phase only needs "is the file structurally
    // valid and what's the headline status".
    int singleCount = OCSP_resp_count(basic);
    if (singleCount > 0) {
        OCSP_SINGLERESP* single = OCSP_resp_get0(basic, 0);
        if (single) {
            ASN1_GENERALIZEDTIME* thisUpd = nullptr;
            ASN1_GENERALIZEDTIME* nextUpd = nullptr;
            ASN1_GENERALIZEDTIME* revoked = nullptr;
            int reason = 0;
            int status = OCSP_single_get0_status(single, &reason, &revoked,
                                                  &thisUpd, &nextUpd);
            (void)reason;
            (void)revoked;
            out.certStatus = status;
            out.thisUpdate = generalizedTimeToEpoch(thisUpd);
            out.nextUpdate = generalizedTimeToEpoch(nextUpd);

            switch (status) {
                case V_OCSP_CERTSTATUS_GOOD:
                    out.statusMessage = "good";
                    break;
                case V_OCSP_CERTSTATUS_REVOKED:
                    out.statusMessage = "revoked";
                    break;
                case V_OCSP_CERTSTATUS_UNKNOWN:
                    out.statusMessage = "unknown";
                    break;
                default:
                    out.statusMessage = "unrecognized status code " +
                                         std::to_string(status);
                    break;
            }
        } else {
            out.statusMessage = "successful (single-response not retrievable)";
        }
    } else {
        out.statusMessage = "successful (no single-response)";
    }

    OCSP_BASICRESP_free(basic);
    OCSP_RESPONSE_free(resp);
    return true;
}

} // namespace pdfe::signatures
