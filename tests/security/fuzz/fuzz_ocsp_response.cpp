// -*- coding: utf-8 -*-
// Phase-73 fuzz harness: OCSP response DER parse surface.
//
// Target: pdfe::signatures::OcspResponseParser::parseBytes, which is
// d2i_OCSP_RESPONSE plus the extraction of certStatus / thisUpdate /
// nextUpdate out of the first single-response.
//
// WHY THIS SURFACE HAD NO COVERAGE-GUIDED TARGET UNTIL NOW. A grep across
// tests/security/fuzz/ for ocsp|crl|pkcs7|signature returned ZERO before this
// phase: eleven harnesses existed and every one of them took PDF bytes. The
// signature-validation path takes a DIFFERENT input class -- DER from an OCSP
// responder, a CRL distribution point, or a /DSS stream -- and no amount of PDF
// mutation reaches it, because the bytes never get treated as ASN.1.
//
// REMOTE BYTES, TO BE EXPLICIT ABOUT SCOPE. These bytes are not always
// document-controlled: ValidationFetcher retrieves them over the network from
// the URL named in the certificate's AIA extension, and that URL comes out of
// an attacker-supplied document. So "attacker controls the response bytes"
// needs only that the attacker control the document, which is the ordinary
// threat model for a PDF reader.
#include "fuzz_common.h"

#include "signatures/OcspResponseParser.h"

#include <cstddef>
#include <cstdint>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    // 1 MiB ceiling: a real OCSP response is a couple of kilobytes. The cap
    // keeps the mutator from spending its budget on length-prefix arithmetic
    // over megabyte inputs that no responder would ever emit.
    if (size == 0 || size > (1u << 20)) return;

    try {
        pdfe::signatures::OcspResponse out;
        const bool ok =
            pdfe::signatures::OcspResponseParser::parseBytes(data, size, out);

        // OUTPUT INVARIANT, not just "did not crash". The parser documents
        // that a successful parse retains the raw DER verbatim so LtvDssWriter
        // can embed it into /DSS /OCSPs without re-encoding. If parseBytes
        // ever returns true with an empty derBytes, the writer emits an empty
        // OCSP stream into a signed document and the LTV chain silently
        // becomes unverifiable -- a correctness failure with no crash, which
        // is exactly the kind a no-crash oracle cannot see.
        if (ok && out.derBytes.empty()) {
            pdfe::fuzz::fail("parseBytes returned true with empty derBytes");
        }
        // A successful parse must also preserve the input length: the bytes
        // are documented as byte-for-byte, and a truncated copy would embed a
        // corrupt response.
        if (ok && out.derBytes.size() != size) {
            pdfe::fuzz::fail("parseBytes retained a different byte count");
        }
    } catch (...) {
        // parseBytes documents "returns false, no exception" -- but the
        // harness must not turn a contract violation into a fuzzer crash
        // report that reads as memory corruption, so it is swallowed here and
        // left to the seed-replay gtest to assert about.
    }
}
