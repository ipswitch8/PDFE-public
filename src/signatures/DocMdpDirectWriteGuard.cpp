// -*- coding: utf-8 -*-
#include "DocMdpDirectWriteGuard.h"

#include "DocMdpEnforcer.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstring>
#include <system_error>

namespace pdfe::signatures {

namespace {

thread_local std::string t_lastWarning;

/// SHORT level name. The refusal text travels out through fz_throw and then
/// through MuPdfContext::execute's `char errorMsg[256]`, so anything past ~250
/// bytes is SILENTLY TRUNCATED — and the first thing to fall off the end is the
/// actionable advice at the tail. (Caught by the F24 remediation test, which
/// asserts the advice is present rather than merely that a refusal happened.)
/// The warn-and-proceed text is a std::string and is not length-bound, so it
/// uses the long form below.
std::string shortLevelName(CertificationLevel level) {
    switch (level) {
        case CertificationLevel::NoChanges:          return "/P 1 (no changes)";
        case CertificationLevel::FormFillOnly:       return "/P 2 (form fill + signing)";
        case CertificationLevel::AnnotationsAllowed: return "/P 3 (+ annotations)";
        case CertificationLevel::NotCertification:   break;
    }
    return "not certified";
}

std::string levelName(CertificationLevel level) {
    switch (level) {
        case CertificationLevel::NoChanges:
            return "/P 1 (no changes permitted)";
        case CertificationLevel::FormFillOnly:
            return "/P 2 (form filling and signing permitted)";
        case CertificationLevel::AnnotationsAllowed:
            return "/P 3 (form filling, signing and annotations permitted)";
        case CertificationLevel::NotCertification:
            break;
    }
    return "not certified";
}

} // namespace

const std::string& lastDirectWriteWarning() { return t_lastWarning; }

void clearLastDirectWriteWarning() { t_lastWarning.clear(); }

bool resolvesToSameFile(const std::filesystem::path& input,
                        const std::filesystem::path& output) {
    std::error_code ec;
    if (std::filesystem::exists(input, ec) && !ec
        && std::filesystem::exists(output, ec) && !ec) {
        const bool same = std::filesystem::equivalent(input, output, ec);
        if (!ec) return same;
        ec.clear();
    }
    ec.clear();
    // The output does not exist yet (or identity could not be established):
    // compare lexically-resolved absolute paths. weakly_canonical tolerates a
    // non-existent tail, which is precisely the Save-As-to-a-new-name case.
    auto a = std::filesystem::weakly_canonical(input, ec);
    if (ec) { a = input; ec.clear(); }
    auto b = std::filesystem::weakly_canonical(output, ec);
    if (ec) { b = output; ec.clear(); }
#ifdef _WIN32
    // NTFS is case-insensitive; "A.PDF" and "a.pdf" are the same file and the
    // whole point of the in-place branch is that it must not be dodgeable by
    // spelling the target differently.
    auto lower = [](std::filesystem::path p) {
        auto s = p.native();
        for (auto& c : s) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return std::filesystem::path(s);
    };
    return lower(a) == lower(b);
#else
    return a == b;
#endif
}

bool directWriteRefused(fz_context* ctx,
                        pdf_document* doc,
                        const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        DocMdpChangeClass changeClass,
                        char* refusalOut,
                        std::size_t refusalCap) {
    if (refusalOut && refusalCap > 0) refusalOut[0] = '\0';
    // Scoped so the verdict — and the std::string inside it — is destroyed
    // before this function returns and the caller longjmps.
    {
        const DirectWriteGuardVerdict v =
            evaluateDirectWrite(ctx, doc, input, output, changeClass);
        if (v.allow) return false;
        if (refusalOut && refusalCap > 0) {
            const std::size_t n =
                v.message.size() < refusalCap - 1 ? v.message.size()
                                                  : refusalCap - 1;
            std::memcpy(refusalOut, v.message.data(), n);
            refusalOut[n] = '\0';
        }
    }
    return true;
}

DirectWriteGuardVerdict evaluateDirectWrite(fz_context* ctx,
                                            pdf_document* doc,
                                            const std::filesystem::path& input,
                                            const std::filesystem::path& output,
                                            DocMdpChangeClass changeClass) {
    clearLastDirectWriteWarning();

    DirectWriteGuardVerdict verdict;
    verdict.inPlace = resolvesToSameFile(input, output);

    // Read the certification WITHOUT letting a MuPDF error escape: this is
    // called from inside the engines' own fz_try frames, and an fz_throw
    // escaping here would be reported to the user as a corrupt-file error
    // rather than as what it is.
    DocMdpLevelReading reading;
    bool readFailed = false;
    fz_try(ctx) {
        reading = readDocMdpLevel(ctx, doc);
    }
    fz_catch(ctx) {
        readFailed = true;
    }

    if (readFailed) {
        // Undecidable. Refuse only the irreversible case — see the header.
        if (!verdict.inPlace) return verdict;
        verdict.allow = false;
        verdict.message =
            "Refusing to overwrite this file in place: its certification "
            "status (/Perms /DocMDP) could not be read, so this edit cannot be "
            "shown to be permitted. Write the result to a DIFFERENT file; the "
            "original stays intact.";
        return verdict;
    }

    verdict.certified = reading.certified;
    verdict.level = reading.level;

    // Not certified: nothing to enforce. The overwhelmingly common case, and
    // the control that proves this is scoped to certified documents at all.
    if (!reading.certified) return verdict;

    // /P-AWARE, not a blanket block. The SAME matrix DocMdpEnforcer uses for
    // the Document-side guard decides here, so the two cannot drift.
    if (DocMdpEnforcer::changePermittedAt(changeClass, reading.level)) {
        return verdict;
    }

    const std::string what =
        std::string("A ") + docMdpChangeClassToString(changeClass)
        + "-class change is not permitted by this document's certification ("
        + levelName(reading.level) + ", ISO 32000-1 Table 254).";

    if (verdict.inPlace) {
        verdict.allow = false;
        // Kept under ~250 bytes on purpose — see shortLevelName().
        verdict.message =
            "Refusing to overwrite a certified document in place: its "
            "certification " + shortLevelName(reading.level)
            + " forbids a " + docMdpChangeClassToString(changeClass)
            + " change (ISO 32000-1 Table 254). Write the result to a "
              "DIFFERENT file; the certified original stays intact.";
        return verdict;
    }

    verdict.allow = true;
    verdict.message =
        "The written copy does NOT satisfy the certification it carries.\n\n"
        + what + "\n\nCertification level: " + levelName(reading.level)
        + ".\nThe original file at its certified state is unchanged; \""
        + output.filename().string()
        + "\" will be reported as modified since certification by any "
          "validating reader.";
    t_lastWarning = verdict.message;
    return verdict;
}

} // namespace pdfe::signatures
