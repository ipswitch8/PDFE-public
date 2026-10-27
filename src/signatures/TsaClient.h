#pragma once

#include <string>
#include <vector>
#include <cstdint>

// OpenSSL typedef-only header so X509* is a valid type in this public API
// without pulling in the full <openssl/x509.h>.
#include <openssl/types.h>

namespace pdfe::signatures {

/// RFC 3161 Time-Stamp Authority client.
/// Constructs TimeStampReq, sends via HTTP POST, parses TimeStampResp.
class TsaClient {
public:
    struct TsaResult {
        bool success = false;
        std::vector<uint8_t> token;   // DER-encoded TimeStampToken
        std::string errorMessage;
    };

    /// Request a timestamp token from a TSA server.
    /// @param tsaUrl   HTTP(S) URL of the TSA
    /// @param digest   SHA-256 hash of the signature value to timestamp
    /// @param digestLen Length of digest in bytes
    /// @return TsaResult with token on success, error message on failure
    static TsaResult requestTimestamp(const std::string& tsaUrl,
                                       const uint8_t* digest,
                                       size_t digestLen);

    /// Outcome of verifying an RFC 3161 TimeStampToken. Each field is an
    /// independent property so callers (and tests) can distinguish "not a
    /// token", "token but it timestamps different bytes" (the forged/
    /// mismatched case) and "token not signed by the expected TSA".
    struct TokenVerification {
        bool parsed = false;          ///< Well-formed RFC 3161 TimeStampToken.
        bool imprintMatches = false;  ///< messageImprint == expectedImprint.
        bool signatureValid = false;  ///< Token signed by a cert in tsaStore.
        /// True when a TSA certificate WAS supplied but signature verification
        /// was refused because its trust had not been established by the
        /// caller. Distinct from `signatureValid == false`, which also covers
        /// "no cert supplied" and "cert supplied and the signature did not
        /// verify" — three different situations that must not share a field.
        bool signatureRefusedUntrustedCert = false;
    };

    /// Where the caller's TSA certificate came from — F61 (follow-up phase-67).
    ///
    /// WHY THIS PARAMETER EXISTS. verifyTimestampToken() sets
    /// X509_V_FLAG_PARTIAL_CHAIN on a store containing exactly the caller's
    /// certificate, which makes that certificate a trust anchor for this check.
    /// The load-bearing comment at TsaClient.cpp says why that is sound, and
    /// its reasoning rests entirely on a precondition about the CALLER:
    ///
    ///     "`store` is not attacker-controlled and is not a general trust
    ///      store. It contains one anchor, supplied by the caller as 'the TSA
    ///      I expect this token to be signed by', ALREADY ESTABLISHED BY
    ///      WHATEVER MEANS THE CALLER USED."
    ///
    /// That precondition was documented and unenforced. A future caller that
    /// lifts the certificate out of the token it is about to verify satisfies
    /// the signature check trivially — PARTIAL_CHAIN would be trusting the
    /// attacker's own anchor, and every self-consistent forged token would
    /// report `signatureValid`.
    ///
    /// NOTE ON THE FINDING'S OWN FRAMING. The register describes the hazard as
    /// reaching PARTIAL_CHAIN via the `nullptr` the sole production caller
    /// passes. It does not: the whole block is inside `if (tsaCert)`, so a null
    /// certificate SKIPS signature verification (which is what
    /// LtvValidator.cpp wants — it reads only `parsed && imprintMatches`) and
    /// never reaches the flag. Guarding null would have reddened
    /// TsaTimestampVerifyTest.NullInputsSafe and broken the only caller. The
    /// unenforced precondition is about a NON-null certificate's provenance.
    ///
    /// There is deliberately NO DEFAULT. A default is how an unstated
    /// assumption gets inherited by the next caller without anyone deciding.
    enum class TsaCertTrust {
        /// The certificate's trust has NOT been established, or came from the
        /// token itself. Signature verification is refused;
        /// `signatureRefusedUntrustedCert` is set and the imprint binding is
        /// still checked.
        ///
        /// FIRST, so that it is the zero value. There is no default argument,
        /// so this cannot be reached by omission — but a future call site that
        /// writes `TsaCertTrust{}` value-initialises to enumerator 0, and that
        /// must land on the refusing branch rather than the permissive one.
        /// Raised by the security-audit gate as defence in depth; the ordering
        /// costs nothing and removes a way to be wrong.
        NotEstablished,
        /// The caller established this certificate's trust independently of
        /// the token being verified — from configuration, a trust store, or a
        /// chain it validated itself. Only this value permits the
        /// PARTIAL_CHAIN-backed signature check.
        EstablishedByCaller,
    };

    /// Verify an embedded RFC 3161 TimeStampToken (DER, a PKCS#7 as embedded by
    /// the signing path). This is the read-side counterpart to the timestamp
    /// embedding done at sign time: it HARD-ASSERTS the RFC 3161 binding —
    /// that the token's messageImprint equals @p expectedImprint (the SHA-256
    /// of the signature value that was timestamped). A token whose imprint does
    /// not match does not timestamp our signature and must be rejected.
    ///
    /// @param tokenDer        DER-encoded TimeStampToken.
    /// @param expectedImprint The digest the token must attest to (SHA-256 of
    ///                        the signature value). Must not be null.
    /// @param imprintLen      Length of @p expectedImprint in bytes.
    /// @param tsaCert         TSA certificate to verify the token signature
    ///                        against. If null, @c signatureValid is left false
    ///                        (imprint binding is still checked).
    /// @param certTrust       How @p tsaCert's trust was established. Required,
    ///                        with no default: this certificate becomes a trust
    ///                        anchor under X509_V_FLAG_PARTIAL_CHAIN, so the
    ///                        caller must state that it means to make it one.
    ///                        Ignored when @p tsaCert is null, because no
    ///                        anchor is installed in that case.
    /// @return per-property verification result. Never throws.
    static TokenVerification verifyTimestampToken(
        const std::vector<uint8_t>& tokenDer,
        const uint8_t* expectedImprint,
        size_t imprintLen,
        X509* tsaCert,
        TsaCertTrust certTrust);

private:
    static std::vector<uint8_t> buildTimeStampReq(const uint8_t* digest,
                                                    size_t digestLen);

    static TsaResult parseTimeStampResp(const std::vector<uint8_t>& response);

    static std::vector<uint8_t> httpPost(const std::string& url,
                                          const std::vector<uint8_t>& body,
                                          const std::string& contentType,
                                          std::string& errorOut);
};

} // namespace pdfe::signatures
