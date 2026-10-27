// -*- coding: utf-8 -*-
#pragma once

// Meta-audit 2026-07 follow-up, phase F21 — gaps PDF-36 and PDF-37.
//
// PDF-37: nothing in PDFE ever asked whether a signature's /ByteRange actually
// COVERS the file. `verifySignature()` asks MuPDF whether the PKCS#7 digest
// over the stated ranges checks out — and it always does, because the attacker
// chooses the ranges. A signature that vouches for 1 298 of a file's 11 538
// bytes verifies exactly as cleanly as one that vouches for all of them.
//
// PDF-36: the incremental-update shadow attack and signature wrapping are the
// two ways that gap is exploited. Both leave the digest VALID:
//
//   * shadow attack — append an incremental update after the signed range. The
//     signed bytes are untouched, so the digest verifies, but the newest
//     revision redefines the page content and the rendered document is not the
//     document that was signed.
//   * signature wrapping — state a /ByteRange whose second range begins after
//     an injected block, so a region in the MIDDLE of the file is outside
//     every signed range. Re-signing over the manipulated ranges makes the
//     digest valid too.
//
// This class answers the question that separates those from a real signature:
// given the file's actual bytes, what does /ByteRange leave out?
//
// ISO 32000-1 §12.8.1: /ByteRange is "an array of pairs of integers ...
// describing the exact byte range for the digest calculation. A byte range
// shall be specified to include the entire file, including the signature
// dictionary but excluding the signature value itself (the /Contents entry)."
// That sentence is the whole specification of what a correct value looks like,
// and it is what `CoverageVerdict::CoversWholeFile` means here.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::signatures {

/// What the /ByteRange leaves uncovered. Ordered by severity of the finding,
/// not by likelihood: `analyze()` reports the FIRST condition that holds in
/// the order Malformed -> ContentsHoleMismatch -> GapInSignedRange ->
/// ContentAppendedAfterSignedRange -> CoversWholeFile.
enum class CoverageVerdict {
    /// No signature value dictionary, or it carries no /ByteRange.
    NoByteRange,
    /// Not four integers, not starting at 0, negative, out of order, or
    /// running past the end of the file.
    Malformed,
    /// The gap between the two ranges does not begin with the `<` of a
    /// /Contents hex string, or that string's `>` is not where the second
    /// range's start implies it should be.
    ContentsHoleMismatch,
    /// Bytes in the MIDDLE of the file are outside every signed range and are
    /// not the /Contents hole — signature wrapping.
    GapInSignedRange,
    /// The signed ranges stop before the end of the file — an incremental
    /// update was appended after signing (the shadow-attack shape).
    ContentAppendedAfterSignedRange,
    /// /ByteRange covers the entire file except exactly the /Contents hex
    /// string. This is the only value that means "the signature vouches for
    /// what you are looking at".
    CoversWholeFile,
};

const char* coverageVerdictToString(CoverageVerdict v);

/// One signature's coverage of the file it lives in.
struct SignatureCoverage {
    /// Object number of the signature VALUE dictionary.
    int signatureObjectNumber = 0;
    /// Fully-qualified field name, when it can be determined.
    std::string fieldName;

    /// The /ByteRange as written in the file, verbatim.
    std::vector<int64_t> byteRange;

    int64_t fileSize = 0;
    /// Sum of the range lengths — the bytes the signature actually vouches for.
    int64_t signedBytes = 0;
    /// Bytes after the end of the last signed range. Non-zero == something was
    /// appended after signing.
    int64_t unsignedBytesAfterRange = 0;
    /// Bytes between the signed ranges that are NOT the /Contents hex string.
    int64_t unsignedBytesInGaps = 0;

    /// Offsets of the /Contents hex string as located in the raw bytes:
    /// `contentsHoleStart` is the offset of `<`, `contentsHoleEnd` one past
    /// `>`. Both -1 when it could not be located.
    int64_t contentsHoleStart = -1;
    int64_t contentsHoleEnd = -1;

    /// Number of `/ByteRange` arrays present anywhere in the raw file. More
    /// than one per signature means the file carries a byte-range array that
    /// no live signature owns — the classic wrapping tell.
    int byteRangeArraysInFile = 0;

    CoverageVerdict verdict = CoverageVerdict::NoByteRange;
    std::string message;

    /// True only for CoversWholeFile.
    bool coversWholeFile() const {
        return verdict == CoverageVerdict::CoversWholeFile;
    }
};

/// Whole-file summary.
struct DocumentCoverage {
    bool readable = false;
    int64_t fileSize = 0;
    int signatureCount = 0;
    std::vector<SignatureCoverage> signatures;
    /// True iff there is at least one signature and EVERY one of them covers
    /// the whole file. A caller deciding whether to show "signed document,
    /// intact" reads this, not the digest alone.
    bool allSignaturesCoverWholeFile = false;
    std::string message;
};

/// Reads a PDF's raw bytes plus its signature dictionaries and reports what
/// the signatures do and do not vouch for.
///
/// Deliberately file-based rather than Document-based: the question is about
/// bytes on disk, and an in-memory Document with unsaved edits cannot answer
/// it. Callers holding a Document should pass `doc->filePath()`.
class SignatureCoverageAnalyzer {
public:
    explicit SignatureCoverageAnalyzer(
        std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~SignatureCoverageAnalyzer();

    SignatureCoverageAnalyzer(const SignatureCoverageAnalyzer&) = delete;
    SignatureCoverageAnalyzer& operator=(const SignatureCoverageAnalyzer&) = delete;

    /// Never throws for an unreadable or non-PDF file — it reports
    /// `readable=false` instead.
    DocumentCoverage analyze(const std::filesystem::path& path) const;

private:
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace pdfe::signatures
