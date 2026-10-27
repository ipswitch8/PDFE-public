// -*- coding: utf-8 -*-
#include "SignatureCoverageAnalyzer.h"

#include "core/Error.h"
#include "core/MuPdfContext.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pdfe::signatures {

const char* coverageVerdictToString(CoverageVerdict v) {
    switch (v) {
    case CoverageVerdict::NoByteRange:  return "no /ByteRange";
    case CoverageVerdict::Malformed:    return "malformed /ByteRange";
    case CoverageVerdict::ContentsHoleMismatch:
        return "/ByteRange gap is not the /Contents hex string";
    case CoverageVerdict::GapInSignedRange:
        return "unsigned gap inside the file (signature wrapping)";
    case CoverageVerdict::ContentAppendedAfterSignedRange:
        return "content appended after the signed range";
    case CoverageVerdict::CoversWholeFile:
        return "covers the whole file except the /Contents hole";
    }
    return "unknown";
}

namespace {

std::vector<unsigned char> readAll(const std::filesystem::path& p) {
    std::vector<unsigned char> out;
    std::ifstream in(p, std::ios::binary);
    if (!in) return out;
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    if (len <= 0) return out;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    in.read(reinterpret_cast<char*>(out.data()), len);
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

int countNeedle(const std::vector<unsigned char>& hay, const char* needle) {
    const size_t n = std::char_traits<char>::length(needle);
    if (hay.size() < n) return 0;
    int count = 0;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        if (std::memcmp(hay.data() + i, needle, n) == 0) ++count;
    }
    return count;
}

bool isHexDigit(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

/// One signature value dictionary as read out of the document.
struct RawSig {
    int num = 0;
    std::string name;
    std::vector<int64_t> byteRange;
};

/// Fully-qualified field name: /T joined from the root of the /Parent chain.
void qualifiedName(fz_context* ctx, pdf_obj* field, std::string& out) {
    pdf_obj* parent = pdf_dict_get(ctx, field, PDF_NAME(Parent));
    if (parent) {
        qualifiedName(ctx, parent, out);
    }
    const char* t = pdf_dict_get_text_string(ctx, field, PDF_NAME(T));
    if (t && *t) {
        if (!out.empty()) out.push_back('.');
        out.append(t);
    }
}

bool fieldIsSignature(fz_context* ctx, pdf_obj* field) {
    pdf_obj* node = field;
    for (int depth = 0; node && depth < 32; ++depth) {
        pdf_obj* ft = pdf_dict_get(ctx, node, PDF_NAME(FT));
        if (ft) return pdf_name_eq(ctx, ft, PDF_NAME(Sig));
        node = pdf_dict_get(ctx, node, PDF_NAME(Parent));
    }
    return false;
}

void collectSignatures(fz_context* ctx, pdf_obj* fieldsArray,
                       std::vector<RawSig>& out, int depth) {
    if (depth > 16 || !pdf_is_array(ctx, fieldsArray)) return;
    const int n = pdf_array_len(ctx, fieldsArray);
    for (int i = 0; i < n; ++i) {
        pdf_obj* field = pdf_array_get(ctx, fieldsArray, i);
        pdf_obj* resolved = pdf_resolve_indirect(ctx, field);
        if (!pdf_is_dict(ctx, resolved)) continue;
        if (fieldIsSignature(ctx, resolved)) {
            pdf_obj* vref = pdf_dict_get(ctx, resolved, PDF_NAME(V));
            pdf_obj* v = pdf_resolve_indirect(ctx, vref);
            if (pdf_is_dict(ctx, v)) {
                out.push_back(RawSig{});
                RawSig& rs = out.back();
                rs.num = pdf_to_num(ctx, vref);
                qualifiedName(ctx, resolved, rs.name);
                pdf_obj* br = pdf_dict_get(ctx, v, PDF_NAME(ByteRange));
                if (pdf_is_array(ctx, br)) {
                    const int bn = pdf_array_len(ctx, br);
                    for (int k = 0; k < bn; ++k) {
                        rs.byteRange.push_back(static_cast<int64_t>(
                            pdf_array_get_int(ctx, br, k)));
                    }
                }
            }
        }
        collectSignatures(ctx, pdf_dict_get(ctx, resolved, PDF_NAME(Kids)),
                          out, depth + 1);
    }
}

/// The actual coverage judgement. Pure function of the raw bytes and the
/// /ByteRange as written, so it is testable and mutatable in one place.
void judge(const std::vector<unsigned char>& bytes, SignatureCoverage& sc) {
    const int64_t size = static_cast<int64_t>(bytes.size());
    sc.fileSize = size;

    if (sc.byteRange.empty()) {
        sc.verdict = CoverageVerdict::NoByteRange;
        sc.message = "signature has no /ByteRange";
        return;
    }
    // ISO 32000-1 §12.8.1: pairs of integers. A single pair cannot leave the
    // /Contents hole out, and more than two pairs is not a shape any writer
    // produces — both are treated as malformed rather than silently accepted.
    if (sc.byteRange.size() != 4) {
        sc.verdict = CoverageVerdict::Malformed;
        sc.message = "/ByteRange has " + std::to_string(sc.byteRange.size())
                   + " entries, expected 4";
        return;
    }
    const int64_t r0 = sc.byteRange[0], r1 = sc.byteRange[1];
    const int64_t r2 = sc.byteRange[2], r3 = sc.byteRange[3];
    if (r0 != 0 || r1 < 0 || r3 < 0 || r2 < r0 + r1 || r2 + r3 > size) {
        sc.verdict = CoverageVerdict::Malformed;
        sc.message = "/ByteRange [" + std::to_string(r0) + " "
                   + std::to_string(r1) + " " + std::to_string(r2) + " "
                   + std::to_string(r3) + "] is not a valid cover of a "
                   + std::to_string(size) + "-byte file";
        return;
    }

    sc.signedBytes = r1 + r3;
    sc.unsignedBytesAfterRange = size - (r2 + r3);

    // Locate the /Contents hex string the first range stops in front of.
    const int64_t holeStart = r0 + r1;
    int64_t holeEnd = -1;
    if (holeStart < size && bytes[static_cast<size_t>(holeStart)] == '<') {
        for (int64_t i = holeStart + 1; i < r2; ++i) {
            const unsigned char c = bytes[static_cast<size_t>(i)];
            if (c == '>') { holeEnd = i + 1; break; }
            if (!isHexDigit(c) && !std::isspace(c)) break;
        }
    }
    sc.contentsHoleStart = (holeEnd > 0) ? holeStart : -1;
    sc.contentsHoleEnd = holeEnd;

    if (holeEnd < 0) {
        sc.verdict = CoverageVerdict::ContentsHoleMismatch;
        sc.message = "the region /ByteRange skips at offset "
                   + std::to_string(holeStart)
                   + " is not a /Contents hex string";
        return;
    }

    sc.unsignedBytesInGaps = r2 - holeEnd;

    if (sc.unsignedBytesInGaps > 0) {
        sc.verdict = CoverageVerdict::GapInSignedRange;
        sc.message = std::to_string(sc.unsignedBytesInGaps)
                   + " byte(s) between offset " + std::to_string(holeEnd)
                   + " and " + std::to_string(r2)
                   + " are inside the file but outside every signed range";
        return;
    }
    if (sc.unsignedBytesAfterRange > 0) {
        sc.verdict = CoverageVerdict::ContentAppendedAfterSignedRange;
        sc.message = std::to_string(sc.unsignedBytesAfterRange)
                   + " byte(s) were appended after the signed range ends at "
                   + std::to_string(r2 + r3)
                   + "; the signature does not vouch for them";
        return;
    }
    sc.verdict = CoverageVerdict::CoversWholeFile;
    sc.message = "signature covers all " + std::to_string(size)
               + " bytes except the " + std::to_string(holeEnd - holeStart)
               + "-byte /Contents hex string";
}

} // namespace

// ---------------------------------------------------------------------------

SignatureCoverageAnalyzer::SignatureCoverageAnalyzer(
    std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context)) {}

SignatureCoverageAnalyzer::~SignatureCoverageAnalyzer() = default;

DocumentCoverage SignatureCoverageAnalyzer::analyze(
    const std::filesystem::path& path) const {
    DocumentCoverage out;

    const std::vector<unsigned char> bytes = readAll(path);
    if (bytes.empty()) {
        out.message = "file is empty or unreadable: " + path.string();
        return out;
    }
    out.fileSize = static_cast<int64_t>(bytes.size());
    const int byteRangeArrays = countNeedle(bytes, "/ByteRange");

    // Containers constructed BEFORE any fz_try in this frame.
    std::vector<RawSig> sigs;
    const std::string pathStr = path.string();
    bool opened = false;

    try {
        context_->execute([&](fz_context* ctx) {
            fz_document* fzdoc = nullptr;
            pdf_document* doc = nullptr;
            fz_try(ctx) {
                fzdoc = fz_open_document(ctx, pathStr.c_str());
                doc = pdf_document_from_fz_document(ctx, fzdoc);
                if (doc) {
                    opened = true;
                    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc),
                                                 PDF_NAME(Root));
                    pdf_obj* acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
                    collectSignatures(ctx,
                                      pdf_dict_get(ctx, acro, PDF_NAME(Fields)),
                                      sigs, 0);
                }
            }
            fz_always(ctx) {
                fz_drop_document(ctx, fzdoc);
            }
            fz_catch(ctx) {
                fz_rethrow(ctx);
            }
        });
    } catch (const std::exception& e) {
        out.message = std::string("could not read signatures: ") + e.what();
        return out;
    }

    out.readable = opened;
    if (!opened) {
        out.message = "not a PDF document: " + path.string();
        return out;
    }

    out.signatureCount = static_cast<int>(sigs.size());
    for (const RawSig& rs : sigs) {
        SignatureCoverage sc;
        sc.signatureObjectNumber = rs.num;
        sc.fieldName = rs.name;
        sc.byteRange = rs.byteRange;
        sc.byteRangeArraysInFile = byteRangeArrays;
        judge(bytes, sc);
        out.signatures.push_back(sc);
    }

    out.allSignaturesCoverWholeFile =
        !out.signatures.empty()
        && std::all_of(out.signatures.begin(), out.signatures.end(),
                       [](const SignatureCoverage& s) {
                           return s.coversWholeFile();
                       });
    if (out.signatures.empty()) {
        out.message = "document carries no signature value dictionary";
    } else if (out.allSignaturesCoverWholeFile) {
        out.message = std::to_string(out.signatures.size())
                    + " signature(s), all covering the whole file";
    } else {
        out.message = out.signatures.front().message;
    }
    return out;
}

} // namespace pdfe::signatures
