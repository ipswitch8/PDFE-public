// -*- coding: utf-8 -*-
// Phase-73 fuzz harness: RFC 3161 timestamp-token parse surface.
//
// Target: pdfe::signatures::TsaClient::verifyTimestampToken, which takes the
// DER TimeStampToken and walks it with OpenSSL's TS_* API.
//
// WHY THIS ENTRY POINT AND NOT parseTimeStampResp. The reverification named
// TsaClient's transport paths (libcurl at TsaClient.cpp:210-236, WinHttp at
// :278-312) as remote-bytes-in-scope, and they are: the response comes off the
// network from a URL the document names. But `parseTimeStampResp` is PRIVATE,
// and so is `httpPost`. Rather than widen the class's visibility to suit a test
// -- which changes the product to fit the harness -- this drives the public
// entry point that consumes the same attacker-controlled DER once the transport
// has handed it over.
//
// WHAT THAT LEAVES UNCOVERED, stated rather than implied: the HTTP layer itself
// (chunked-transfer handling, Content-Length parsing, header handling in
// libcurl/WinHttp) is not reached from here. That is a transport-level surface
// needing an injectable HTTP seam, and it is recorded as a named follow-up in
// docs/audit/meta-audit-2026-07/followup/phase-73-dispositions.md rather than
// silently counted as covered by this file.
#include "fuzz_common.h"

#include "signatures/TsaClient.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    if (size == 0 || size > (1u << 20)) return;

    // A fixed imprint: the token's messageImprint must be COMPARED against
    // something for the comparison path to run at all. Passing the fuzz bytes
    // as the imprint too would make every comparison trivially mismatch on
    // length and skip the interesting code.
    static const std::uint8_t kImprint[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};

    const std::vector<std::uint8_t> token(data, data + size);

    try {
        const auto v = pdfe::signatures::TsaClient::verifyTimestampToken(
            token, kImprint, sizeof(kImprint), /*tsaCert=*/nullptr,
            pdfe::signatures::TsaClient::TsaCertTrust::NotEstablished);

        // OUTPUT INVARIANT. No anchor was installed (tsaCert is null), so
        // nothing in this call can establish trust. A result that claims a
        // valid signature would mean the "verified" flag is reachable without
        // a trust anchor -- the exact defect class this project has hit twice
        // in this module (F52/F53: saying "Validated" about a signature nothing
        // trusts, and the phase-49 CRL widening reading revoked as VALIDATED).
        if (v.signatureValid) {
            pdfe::fuzz::fail(
                "verifyTimestampToken reported signatureValid with no trust anchor");
        }
        // An unparsed token cannot have matched an imprint.
        if (!v.parsed && v.imprintMatches) {
            pdfe::fuzz::fail("imprintMatches set on a token that did not parse");
        }
    } catch (...) {
        // Documented "never throws"; see fuzz_ocsp_response.cpp for why a
        // contract violation is not converted into a crash report here.
    }
}
