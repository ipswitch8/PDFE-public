#include "TsaClient.h"

#include <openssl/ts.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/asn1.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <cstring>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winhttp.h>
  #pragma comment(lib, "winhttp.lib")
#else
  #include <curl/curl.h>
#endif

#include <sstream>
#include <algorithm>

namespace pdfe::signatures {

static constexpr size_t MAX_TSA_RESPONSE_SIZE = 1024 * 1024; // 1 MB

static bool isAllowedTsaUrl(const std::string& url)
{
    // Require http:// or https:// scheme
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return false;

    // Extract host portion
    size_t hostStart = url.find("://") + 3;
    size_t hostEnd = url.find_first_of(":/", hostStart);
    if (hostEnd == std::string::npos) hostEnd = url.size();
    std::string host = url.substr(hostStart, hostEnd - hostStart);

    // Block loopback
    if (host == "localhost" || host == "127.0.0.1" || host == "::1")
        return false;

    // Block cloud metadata endpoint
    if (host == "169.254.169.254")
        return false;

    // Block link-local (169.254.x.x)
    if (host.rfind("169.254.", 0) == 0)
        return false;

    return true;
}

std::vector<uint8_t> TsaClient::buildTimeStampReq(const uint8_t* digest,
                                                    size_t digestLen)
{
    TS_REQ* req = TS_REQ_new();
    if (!req) return {};

    TS_REQ_set_version(req, 1);

    // Set message imprint (SHA-256 hash)
    TS_MSG_IMPRINT* imprint = TS_MSG_IMPRINT_new();
    if (!imprint) {
        TS_REQ_free(req);
        return {};
    }

    X509_ALGOR* algor = X509_ALGOR_new();
    if (algor) {
        X509_ALGOR_set0(algor, OBJ_nid2obj(NID_sha256),
                         V_ASN1_NULL, nullptr);
        TS_MSG_IMPRINT_set_algo(imprint, algor);
        X509_ALGOR_free(algor);
    }

    if (digestLen > static_cast<size_t>(INT_MAX)) {
        TS_MSG_IMPRINT_free(imprint);
        TS_REQ_free(req);
        return {};
    }
    TS_MSG_IMPRINT_set_msg(imprint,
                            const_cast<unsigned char*>(digest),
                            static_cast<int>(digestLen));

    TS_REQ_set_msg_imprint(req, imprint);
    TS_MSG_IMPRINT_free(imprint);

    // Random nonce for replay protection
    ASN1_INTEGER* nonce = ASN1_INTEGER_new();
    if (nonce) {
        unsigned char nonce_bytes[8];
        RAND_bytes(nonce_bytes, sizeof(nonce_bytes));
        BIGNUM* bn = BN_bin2bn(nonce_bytes, sizeof(nonce_bytes), nullptr);
        if (bn) {
            BN_to_ASN1_INTEGER(bn, nonce);
            TS_REQ_set_nonce(req, nonce);
            BN_free(bn);
        }
        ASN1_INTEGER_free(nonce);
    }

    // Request certificate in response
    TS_REQ_set_cert_req(req, 1);

    // Serialize to DER
    unsigned char* der = nullptr;
    int derLen = i2d_TS_REQ(req, &der);
    TS_REQ_free(req);

    if (derLen <= 0 || !der) return {};

    std::vector<uint8_t> result(der, der + derLen);
    OPENSSL_free(der);
    return result;
}

TsaClient::TsaResult TsaClient::parseTimeStampResp(
    const std::vector<uint8_t>& response)
{
    TsaResult result;

    const unsigned char* p = response.data();
    TS_RESP* resp = d2i_TS_RESP(nullptr, &p,
                                 static_cast<long>(response.size()));
    if (!resp) {
        result.errorMessage = "Failed to parse TSA response";
        return result;
    }

    // Check status
    TS_STATUS_INFO* statusInfo = TS_RESP_get_status_info(resp);
    if (!statusInfo) {
        TS_RESP_free(resp);
        result.errorMessage = "TSA response has no status info";
        return result;
    }

    const ASN1_INTEGER* statusValue = TS_STATUS_INFO_get0_status(statusInfo);
    long status = ASN1_INTEGER_get(statusValue);

    if (status != 0 && status != 1) {
        // 0 = granted, 1 = grantedWithMods, 2+ = rejection/error
        std::ostringstream oss;
        oss << "TSA rejected request (status " << status << ")";

        const STACK_OF(ASN1_UTF8STRING)* texts =
            TS_STATUS_INFO_get0_text(statusInfo);
        if (texts && sk_ASN1_UTF8STRING_num(texts) > 0) {
            const ASN1_UTF8STRING* msg = sk_ASN1_UTF8STRING_value(texts, 0);
            if (msg && msg->data) {
                oss << ": " << reinterpret_cast<const char*>(msg->data);
            }
        }

        TS_RESP_free(resp);
        result.errorMessage = oss.str();
        return result;
    }

    // Extract the TimeStampToken (ContentInfo DER)
    PKCS7* token = TS_RESP_get_token(resp);
    if (!token) {
        TS_RESP_free(resp);
        result.errorMessage = "TSA response has no token";
        return result;
    }

    unsigned char* tokenDer = nullptr;
    int tokenDerLen = i2d_PKCS7(token, &tokenDer);

    if (tokenDerLen <= 0 || !tokenDer) {
        TS_RESP_free(resp);
        result.errorMessage = "Failed to serialize timestamp token";
        return result;
    }

    result.token.assign(tokenDer, tokenDer + tokenDerLen);
    result.success = true;

    OPENSSL_free(tokenDer);
    TS_RESP_free(resp);
    return result;
}

#ifndef _WIN32
namespace {
// libcurl write callback — accumulates bytes into a vector<uint8_t>
size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    size_t bytes = size * nmemb;
    if (buf->size() + bytes > MAX_TSA_RESPONSE_SIZE) return 0;  // signal abort
    auto* p = static_cast<const uint8_t*>(ptr);
    buf->insert(buf->end(), p, p + bytes);
    return bytes;
}
} // namespace

std::vector<uint8_t> TsaClient::httpPost(const std::string& url,
                                           const std::vector<uint8_t>& body,
                                           const std::string& contentType,
                                           std::string& errorOut)
{
    std::vector<uint8_t> response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        errorOut = "curl_easy_init failed";
        return {};
    }
    struct curl_slist* headers = nullptr;
    std::string contentTypeHdr = "Content-Type: " + contentType;
    headers = curl_slist_append(headers, contentTypeHdr.c_str());
    headers = curl_slist_append(headers, "Accept: application/timestamp-reply");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::ostringstream oss;
        oss << "curl request failed: " << curl_easy_strerror(rc);
        errorOut = oss.str();
        return {};
    }
    if (httpCode != 200) {
        std::ostringstream oss;
        oss << "TSA HTTP error " << httpCode;
        errorOut = oss.str();
        return {};
    }
    return response;
}
#else
std::vector<uint8_t> TsaClient::httpPost(const std::string& url,
                                           const std::vector<uint8_t>& body,
                                           const std::string& contentType,
                                           std::string& errorOut)
{
    // Parse URL components (proper UTF-8 to UTF-16 conversion)
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(),
                                       static_cast<int>(url.size()), nullptr, 0);
    if (wideLen <= 0) {
        errorOut = "Failed to convert TSA URL to wide string";
        return {};
    }
    std::wstring wUrl(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(),
                         static_cast<int>(url.size()), wUrl.data(), wideLen);

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()),
                          0, &urlComp)) {
        errorOut = "Failed to parse TSA URL";
        return {};
    }

    bool isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"PDFE/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        errorOut = "WinHTTP session creation failed";
        return {};
    }

    // Set timeouts: 10s connect, 30s send/receive
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf,
                                         urlComp.nPort, 0);
    if (!hConnect) {
        errorOut = "WinHTTP connect failed";
        WinHttpCloseHandle(hSession);
        return {};
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", pathBuf, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        errorOut = "WinHTTP request creation failed";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Set Content-Type header
    std::wstring header = L"Content-Type: ";
    header += std::wstring(contentType.begin(), contentType.end());

    BOOL sent = WinHttpSendRequest(
        hRequest, header.c_str(), static_cast<DWORD>(header.size()),
        const_cast<uint8_t*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!sent) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "WinHTTP send failed (error " << err << ")";
        errorOut = oss.str();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "WinHTTP receive failed (error " << err << ")";
        errorOut = oss.str();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Check HTTP status
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        std::ostringstream oss;
        oss << "TSA HTTP error " << statusCode;
        errorOut = oss.str();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response body (bounded to prevent DoS from malicious TSA)
    std::vector<uint8_t> responseData;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) &&
           bytesAvailable > 0) {
        if (responseData.size() + bytesAvailable > MAX_TSA_RESPONSE_SIZE) {
            errorOut = "TSA response exceeds maximum size";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return {};
        }
        std::vector<uint8_t> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), bytesAvailable,
                            &bytesRead)) {
            responseData.insert(responseData.end(),
                               buf.begin(), buf.begin() + bytesRead);
        }
        bytesAvailable = 0;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return responseData;
}
#endif // _WIN32 — end of platform-specific httpPost

TsaClient::TsaResult TsaClient::requestTimestamp(
    const std::string& tsaUrl,
    const uint8_t* digest,
    size_t digestLen)
{
    TsaResult result;

    if (tsaUrl.empty()) {
        result.errorMessage = "TSA URL is empty";
        return result;
    }

    if (!isAllowedTsaUrl(tsaUrl)) {
        result.errorMessage = "TSA URL must use http:// or https:// and cannot target localhost or internal addresses";
        return result;
    }

    if (!digest || digestLen == 0) {
        result.errorMessage = "No digest provided";
        return result;
    }

    // Build RFC 3161 TimeStampReq
    auto reqDer = buildTimeStampReq(digest, digestLen);
    if (reqDer.empty()) {
        result.errorMessage = "Failed to build TimeStampReq";
        return result;
    }

    // Send HTTP POST
    std::string httpError;
    auto response = httpPost(tsaUrl, reqDer,
                              "application/timestamp-query", httpError);
    if (response.empty()) {
        result.errorMessage = httpError.empty()
            ? "Empty TSA response" : httpError;
        return result;
    }

    // Parse TimeStampResp
    return parseTimeStampResp(response);
}

TsaClient::TokenVerification TsaClient::verifyTimestampToken(
    const std::vector<uint8_t>& tokenDer,
    const uint8_t* expectedImprint,
    size_t imprintLen,
    X509* tsaCert,
    TsaCertTrust certTrust)
{
    TokenVerification out;
    if (tokenDer.empty() || !expectedImprint || imprintLen == 0)
        return out;

    // An RFC 3161 TimeStampToken is a PKCS#7 SignedData whose eContent is a
    // TSTInfo. Parse the outer PKCS#7 first.
    const unsigned char* p = tokenDer.data();
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(tokenDer.size()));
    if (!p7) { ERR_clear_error(); return out; }

    // Extract the TSTInfo. PKCS7_to_TS_TST_INFO returns null if this is not a
    // well-formed timestamp token.
    TS_TST_INFO* info = PKCS7_to_TS_TST_INFO(p7);
    if (!info) {
        ERR_clear_error();
        PKCS7_free(p7);
        return out;
    }
    out.parsed = true;

    // HARD-ASSERT the RFC 3161 binding: the token's messageImprint must equal
    // the digest we expect it to have timestamped (SHA-256 of the signature
    // value). A mismatch means the token attests to different bytes and does
    // NOT timestamp our signature.
    TS_MSG_IMPRINT* mi = TS_TST_INFO_get_msg_imprint(info);
    if (mi) {
        ASN1_OCTET_STRING* msg = TS_MSG_IMPRINT_get_msg(mi);
        if (msg && msg->data && msg->length >= 0 &&
            static_cast<size_t>(msg->length) == imprintLen &&
            std::memcmp(msg->data, expectedImprint, imprintLen) == 0) {
            out.imprintMatches = true;
        }
    }

    // Optionally verify the token's signature against the TSA certificate.
    // This authenticates WHO issued the timestamp (analogous to OCSP responder
    // authentication). Skipped when no TSA cert is supplied.
    // F61 (follow-up phase-67) — ENFORCE THE PRECONDITION THE COMMENT BELOW
    // RESTS ON. Everything after this point installs `tsaCert` as a trust
    // ANCHOR (X509_V_FLAG_PARTIAL_CHAIN on a one-certificate store). That is
    // sound only because the caller is asserting "this is the TSA I expect",
    // established independently of the token in hand. A caller that took the
    // certificate FROM the token would be handing the attacker their own
    // anchor, and every self-consistent forged token would verify.
    //
    // The precondition was documented and unenforced. It is now a required
    // parameter with no default, so it cannot be inherited silently.
    //
    // Refusal is reported in its own field rather than folded into
    // `signatureValid == false`: "no certificate supplied", "certificate
    // supplied and the signature failed" and "refused to look" are three
    // different answers, and a caller that cannot tell them apart will read
    // the third as the first.
    //
    // The early return frees what this function already owns. Found by the
    // security-audit gate: the first version returned here and skipped the
    // TS_TST_INFO_free/PKCS7_free below, leaking both on every refusal. Not a
    // trust bypass, and unreachable from the sole production caller (which
    // passes a null certificate) -- but a refusal path that leaks is a refusal
    // path nobody will want to take.
    if (tsaCert && certTrust != TsaCertTrust::EstablishedByCaller) {
        out.signatureRefusedUntrustedCert = true;
        TS_TST_INFO_free(info);
        PKCS7_free(p7);
        return out;
    }

    if (tsaCert) {
        TS_VERIFY_CTX* vctx = TS_VERIFY_CTX_new();
        if (vctx) {
            X509_STORE* store = X509_STORE_new();
            STACK_OF(X509)* certs = sk_X509_new_null();
            if (store && certs) {
                // Trust the supplied TSA cert as the root for this check, and
                // also offer it as an intermediate so the signer is found.
                X509_STORE_add_cert(store, tsaCert);
                X509_up_ref(tsaCert);
                sk_X509_push(certs, tsaCert);

                // X509_V_FLAG_PARTIAL_CHAIN — the same correction phase-48
                // made in ocspResponseIsAuthorized() (see LtvValidator.cpp),
                // applied here because this store has the identical shape and
                // therefore the identical latent bug. Found by the phase-48
                // security review (SEC-4) as the "one file over" sibling of
                // the OCSP fix.
                //
                // Without this flag, X509_verify_cert() insists the path
                // terminate at a SELF-SIGNED certificate. `store` holds
                // exactly ONE certificate — the caller-supplied TSA cert —
                // and a real-world TSA certificate is issued by a CA, not
                // self-signed. No path could ever terminate, so
                // TS_RESP_verify_token() would fail and out.signatureValid
                // would be stuck false for every genuine public TSA. It works
                // today only for a self-signed TSA, which is exactly (and
                // only) what the existing test fixture was.
                //
                // Why enabling it is correct rather than permissive: `store`
                // is not attacker-controlled and is not a general trust store.
                // It contains one anchor, supplied by the caller as "the TSA
                // I expect this token to be signed by", already established by
                // whatever means the caller used. The question this function
                // answers is narrow — "was this token signed by THAT cert (or
                // a delegate it issued)?" — and that certificate belongs at
                // the top of this path by construction. A token signed by any
                // other key still fails, because that key's certificate is not
                // in `store` and its path still terminates nowhere trusted.
                // TsaTimestampVerifyTest.WrongTsaCertFailsSignature and
                // .IntermediateIssuedTsaCertVerifies pin both directions.
                if (X509_VERIFY_PARAM* vp = X509_STORE_get0_param(store)) {
                    X509_VERIFY_PARAM_set_flags(vp, X509_V_FLAG_PARTIAL_CHAIN);
                }

                TS_VERIFY_CTX_set_flags(vctx, TS_VFY_SIGNATURE);
                // The ctx takes ownership of store + certs (freed by
                // TS_VERIFY_CTX_free below).
                TS_VERIFY_CTX_set_store(vctx, store);
                TS_VERIFY_CTX_set_certs(vctx, certs);
                store = nullptr;
                certs = nullptr;

                if (TS_RESP_verify_token(vctx, p7) == 1)
                    out.signatureValid = true;
                else
                    ERR_clear_error();
            }
            // Free anything not handed to the ctx.
            if (certs) sk_X509_free(certs);
            if (store) X509_STORE_free(store);
            TS_VERIFY_CTX_free(vctx);
        }
    }

    TS_TST_INFO_free(info);
    PKCS7_free(p7);
    return out;
}

} // namespace pdfe::signatures
