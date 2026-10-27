// -*- coding: utf-8 -*-
#pragma once

// Meta-audit 2026-07 follow-up, phase F24 REMEDIATION — security-audit gate
// finding 1: the DocMDP save guard was bypassable, so it was decorative for
// four write paths.
//
// F24 installed DocMdpSaveGuard into pdfe::core::Document::save/saveAs/
// saveIncremental (and, since F58, saveIncrementalStaged — the staged form of
// the same append). That covers every write that goes through the document
// model. It covers NOTHING that does not, and four content-editing engines
// call pdf_save_document() directly on a raw pdf_document* they opened
// themselves:
//
//   src/batch/BatchEngine.cpp        (watermark)
//   src/headerfooter/HeaderFooterEngine.cpp (apply + remove)
//   src/optimize/OptimizeEngine.cpp  (rewrite / sanitize / linearize)
//   src/reflow/AlignmentInjector.cpp (apply + remove)
//
// `grep -ci 'docmdp|enforcer|certif|saveguard'` returned 0 in all four. A
// watermark or header-footer job pointed at a /P 1 certified document could
// overwrite it in place with no refusal and no warning — through the same
// product, on the same file, that Document::save() would have refused. A
// control with a documented bypass is not a control.
//
// THE POLICY IS THE SAME ONE, NOT A SECOND ONE
// --------------------------------------------
// This guard reuses DocMdpEnforcer's decode (readDocMdpLevel) and DocMdpEnforcer's
// permission matrix (changePermittedAt). It applies the identical product
// decision DocMdpSaveGuard.h argues for and phase-24-dispositions.md §1 records:
//
//   * IN PLACE (the certified original IS the output): REFUSED.
//   * ELSEWHERE (output is a different file): ALLOWED, with a warning
//     recorded in lastDirectWriteWarning().
//
// A divergent second policy would be a worse outcome than the gap it closes,
// so the only thing that differs is the plumbing: these engines have a
// pdf_document* and two paths, not a Document* and a save target.
//
// WHY THE CHANGE CLASS IS A PARAMETER
// -----------------------------------
// The Document-side guard diffs revisions to discover WHAT changed. These
// engines know statically what they are about to do — a page content-stream
// edit, or a full rewrite — so they state it, and the guard asks the same
// permission matrix whether that class is permitted at the level in force.
// That is what keeps this /P-AWARE rather than a blanket block on certified
// inputs: pass DocMdpChangeClass::Annotation and a /P 3 document is allowed
// through in place, exactly as ISO 32000-1 Table 254 says it should be.

// DELIBERATELY Qt-FREE. This header is included by pdfe_headerfooter,
// pdfe_optimize, pdfe_reflow and pdfe_batch — engines that are otherwise pure
// MuPDF. Including DocMdpEnforcer.h here would drag in SignatureData.h ->
// <QRectF> -> Qt's `emit` keyword MACRO, which silently mangles any translation
// unit with a local named `emit` (HeaderFooterEngine.cpp has one). The DocMDP
// vocabulary therefore lives in DocMdpTypes.h, which includes nothing at all.
#include "DocMdpTypes.h"

// Forward declarations for MuPDF C types (global scope, C linkage).
extern "C" {
struct fz_context;
struct pdf_document;
}

#include <cstddef>
#include <filesystem>
#include <string>

namespace pdfe::signatures {

/// Verdict for one direct pdf_save_document() write.
///
/// Trivially copyable members only where it matters: DECLARE THIS OUTSIDE the
/// fz_try frame that consumes it. The engines fz_throw() on refusal, and a
/// longjmp past a std::string living inside the fz_try would leak it. Every
/// call site in the product follows that shape.
struct DirectWriteGuardVerdict {
    /// False iff the write must not happen.
    bool allow = true;
    /// True iff the SOURCE document carries /Perms /DocMDP.
    bool certified = false;
    /// Level in force on the source; NotCertification when !certified.
    CertificationLevel level = CertificationLevel::NotCertification;
    /// True iff `output` resolves to the same file as `input`.
    bool inPlace = false;
    /// Empty iff there is nothing to say. Carries the refusal reason when
    /// !allow and the warn-and-proceed text when allow.
    std::string message;
};

/// Evaluate a direct write. @p doc must be the SOURCE document, already open,
/// and @p ctx must be its fz_context.
///
/// Safe to call from within an fz_try frame: it wraps its own MuPDF reads in
/// fz_try/fz_catch and never lets a MuPDF error escape.
///
/// FAIL-SAFE DIRECTION when the certification cannot be read at all (a MuPDF
/// error during the /Perms probe): an IN-PLACE write is REFUSED and a write
/// elsewhere is allowed. Undecidable plus irreversible is the one combination
/// worth refusing; undecidable plus "writes a new file" costs the user nothing
/// to allow.
///
/// Side effect: when the verdict is allow-but-broken, `message` is also stored
/// in lastDirectWriteWarning() so a caller with no return channel (the void
/// apply()/remove() engine entry points) still surfaces it.
DirectWriteGuardVerdict evaluateDirectWrite(fz_context* ctx,
                                            pdf_document* doc,
                                            const std::filesystem::path& input,
                                            const std::filesystem::path& output,
                                            DocMdpChangeClass changeClass);

/// THE FORM EVERY ENGINE CALL SITE USES.
///
/// Evaluates the write and, on refusal, copies the reason into @p refusalOut —
/// a CALLER STACK BUFFER, deliberately not a std::string. Returns true iff the
/// write must be refused.
///
/// Why a raw buffer: the engines run inside MuPdfContext::execute(), whose
/// fz_try frame lives in the CALLER. An fz_throw from the engine longjmps past
/// every local in the engine's lambda, so a std::string holding the refusal
/// text would leak. This function destroys the verdict (and its string) BEFORE
/// returning, leaving the message in storage that has no destructor to skip.
/// The call site is then simply:
///
///     char refusal[512] = {0};
///     if (directWriteRefused(ctx, doc, in, out, cls, refusal, sizeof refusal)) {
///         pdf_drop_document(ctx, doc);
///         fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
///     }
///
/// On the allow-but-broken path the warning is left in
/// lastDirectWriteWarning() exactly as evaluateDirectWrite() leaves it.
bool directWriteRefused(fz_context* ctx,
                        pdf_document* doc,
                        const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        DocMdpChangeClass changeClass,
                        char* refusalOut,
                        std::size_t refusalCap);

/// The most recent warn-and-proceed message recorded on THIS thread, or empty.
///
/// Thread-local: a batch worker's warning must not be readable as the UI
/// thread's. Mirrors pdfe::core::Document::lastSaveGuardWarning() so the two
/// halves of the same control are surfaced the same way.
const std::string& lastDirectWriteWarning();

/// Clear the thread's recorded warning. evaluateDirectWrite() does this on
/// entry, so a stale warning can never be read as a fresh one.
void clearLastDirectWriteWarning();

/// True iff @p input and @p output name the same file on disk.
///
/// Exposed because "is this in place" is the pivot the whole policy turns on,
/// and it is not simply `input == output`: "./a.pdf" and "a.pdf" are the same
/// file, and on Windows so are "A.PDF" and "a.pdf". Compares by
/// std::filesystem::equivalent() when both exist (which follows the filesystem's
/// own notion of identity, including hard links), falling back to
/// weakly_canonical comparison when the output does not exist yet.
bool resolvesToSameFile(const std::filesystem::path& input,
                        const std::filesystem::path& output);

} // namespace pdfe::signatures
