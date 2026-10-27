// -*- coding: utf-8 -*-
// Phase-20 criterion 1: the STATIC-CRT LINK PROBE, and it deliberately does
// almost nothing at runtime.
//
// The criterion requires that pdfe_signatures's OpenSSL / libcurl /
// Qt6::Network dependencies actually LINK under the fuzz target's /MT
// x64-windows-static configuration BEFORE any harness code is written. That
// ordering is the whole point: writing three harnesses and discovering at the
// end that OpenSSL cannot be linked static-CRT would mean throwing them away.
//
// No existing fuzz target links pdfe_signatures, so this has never been
// demonstrated. This file exists to make the linker answer the question.
#include "fuzz_common.h"

#include "signatures/OcspResponseParser.h"

#include <cstdint>
#include <cstddef>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    // One real call into the library, so the linker must resolve the
    // symbol chain rather than dead-strip it.
    if (size == 0 || size > 1u << 20) return;
    try {
        pdfe::signatures::OcspResponse out;
        const bool ok =
            pdfe::signatures::OcspResponseParser::parseBytes(data, size, out);
        (void)ok;
    } catch (...) {
    }
}
