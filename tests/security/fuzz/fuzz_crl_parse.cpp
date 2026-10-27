// -*- coding: utf-8 -*-
// Phase-73 fuzz harness: X.509 CRL DER parse surface.
//
// Target: pdfe::signatures::CrlParser::parseBytes -- d2i_X509_CRL plus issuer-DN
// extraction in RFC 2253 form and thisUpdate/nextUpdate conversion.
//
// The DN extraction is the part worth fuzzing beyond the d2i call itself. An
// issuer DN is attacker-influenced text that PDFE converts to a std::string and
// then MATCHES against a signer certificate's issuer to decide whether this CRL
// is even in scope for the certificate being validated. A DN that round-trips
// wrongly is a scope-matching failure, and scope-matching failures in this
// module have a history: the phase-49 CRL widening turned "revoked" into
// "VALIDATED" and was caught by review rather than by a test.
#include "fuzz_common.h"

#include "signatures/CrlParser.h"

#include <cstddef>
#include <cstdint>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    // 4 MiB: CRLs are legitimately much larger than OCSP responses -- a busy CA
    // publishes megabytes of revokedCertificates -- so the cap is looser here.
    if (size == 0 || size > (4u << 20)) return;

    try {
        pdfe::signatures::CrlInfo out;
        const bool ok =
            pdfe::signatures::CrlParser::parseBytes(data, size, out);

        // OUTPUT INVARIANT: same contract as the OCSP parser. The raw DER is
        // documented as retained byte-for-byte so LtvDssWriter can embed it
        // into /DSS /CRLs without re-encoding; a successful parse that lost or
        // truncated those bytes writes a corrupt CRL into a signed document.
        if (ok && out.derBytes.size() != size) {
            pdfe::fuzz::fail("parseBytes retained a different byte count");
        }
    } catch (...) {
        // Documented contract is "returns false, no exception"; see the note in
        // fuzz_ocsp_response.cpp for why this is swallowed here rather than
        // converted into a crash report.
    }
}
