// -*- coding: utf-8 -*-
#pragma once

// Meta-audit 2026-07 follow-up, phase F2 — gaps PDF-31 and PDF-32.
//
// PDFE *writes* /Perms /DocMDP when a signature certifies a document
// (SignatureManager::signField) but, until this file existed, nothing ever
// *read* it back. A P1 ("no changes allowed") certified document could be
// edited and incrementally saved with no complaint from the product, because
// the only signals available were the PKCS#7 digest and MuPDF's
// pdf_incremental_change_since_signing_widget — neither of which discriminates
// on the /P value. An incremental update leaves the digest VALID by design, so
// "the digest still checks out" says nothing about whether the certification
// was honoured.
//
// This class is the missing read side. It answers, for an already-open
// document: which certification level is in force, what changed since the
// certifying signature was applied, and is any of it disallowed AT THAT LEVEL.
//
// ISO 32000-1 §12.8.2.2, Table 254 (/TransformParams /P):
//   1  no changes to the document are permitted;
//   2  filling in forms, instantiating page templates, and signing are
//      permitted;
//   3  the above, plus annotation creation, deletion and modification.
// Anything outside the permitted set — a page-content edit, a page insert or
// delete, a catalog restructure — breaks the certification at EVERY level.

#include "SignatureData.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations for MuPDF C types (must be at global scope, with C
// linkage). Needed by readDocMdpLevel(), which reads certification straight off
// an already-open pdf_document — the form the direct-write engines need, since
// they never construct a pdfe::core::Document.
extern "C" {
struct fz_context;
struct pdf_document;
}

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::signatures {

// DocMdpChangeClass now lives in the Qt-free DocMdpTypes.h (F24 remediation),
// reached via SignatureData.h above. Same definition, same name, no Qt.

const char* docMdpChangeClassToString(DocMdpChangeClass c);

/// Where the certification level was read from. Recorded because the two
/// layouts are not interchangeable and PDFE only ever understood one of them.
enum class DocMdpLayout {
    /// No certification present.
    None,
    /// PDFE's own writer: /Root /Perms /DocMDP is a standalone dict carrying
    /// /TransformParams directly.
    DirectTransformParams,
    /// ISO 32000-1 §12.8.2.2 (and what Acrobat and pyHanko emit): /Perms
    /// /DocMDP IS the signature value dictionary, and the level lives in
    /// /Reference [ << /TransformMethod /DocMDP /TransformParams << /P n >> >> ].
    /// Before F21 this layout was silently read as the /P default of 2, so a
    /// third-party P1 document was under-enforced.
    IsoSigRefReference,
};

const char* docMdpLayoutToString(DocMdpLayout l);

/// What /Root /Perms /DocMDP says, read straight off an open pdf_document.
/// Trivially destructible on purpose: it is returned into a caller sitting
/// inside an fz_try frame, where a longjmp would skip any destructor.
struct DocMdpLevelReading {
    bool certified = false;
    CertificationLevel level = CertificationLevel::NotCertification;
    DocMdpLayout layout = DocMdpLayout::None;
};

/// THE single place the two /Perms /DocMDP layouts are decoded. Both
/// DocMdpEnforcer::evaluate() (the Document-based path) and the direct-write
/// guard used by the engines that call pdf_save_document() themselves route
/// through this, so there is exactly one reading of the certification level in
/// the product and not two that can drift apart.
///
/// Call with @p ctx inside a MuPdfContext::execute scope. Performs only pdf_*
/// dictionary reads and keeps nothing with a destructor on the stack, so it is
/// safe to call from within an fz_try frame.
DocMdpLevelReading readDocMdpLevel(fz_context* ctx, pdf_document* doc);

/// One object revised after the certifying signature was applied.
struct DocMdpChange {
    int objectNumber = 0;
    DocMdpChangeClass changeClass = DocMdpChangeClass::Other;
    /// Version the object was written in, MuPDF numbering (0 = newest).
    int version = 0;
    /// FieldMDP: true iff this object belongs to a form field that a
    /// signature's /Lock dictionary locks. A locked field's value may not
    /// change after signing at ANY /P level — that is the whole point of a
    /// field lock, and it is what makes /P 2 (form fill permitted) still
    /// refuse a change to the one field the signer pinned.
    bool touchesLockedField = false;
    std::string description;

    bool operator==(const DocMdpChange&) const = default;
};

/// The result of consulting /Perms /DocMDP on read.
struct DocMdpStatus {
    /// True iff /Root /Perms /DocMDP is present, i.e. the document claims to
    /// be certified.
    bool certified = false;

    /// The level in force. NotCertification when `certified` is false.
    CertificationLevel level = CertificationLevel::NotCertification;

    /// True iff at least one object was revised after the certifying
    /// signature. Not itself an error — a P3 document may legitimately gain
    /// annotations.
    bool changedSinceCertification = false;

    /// THE ANSWER: true iff some change is not permitted at `level`.
    /// A caller asking "is this certification broken by what I just did"
    /// reads this field.
    bool violated = false;

    /// Every post-certification change found, classified.
    std::vector<DocMdpChange> changes;

    /// The subset of `changes` that `level` does not permit. Empty whenever
    /// `violated` is false.
    std::vector<DocMdpChange> disallowedChanges;

    /// Count of in-memory (unsaved) revisions at the time of evaluation.
    /// Evaluate a document as read from disk for a stable answer; a non-zero
    /// value here means the caller is looking at a document with pending
    /// edits and the version numbering below it is provisional.
    int unsavedVersions = 0;

    /// MuPDF version index (0 = newest revision) the certifying signature was
    /// written in, and the total number of revisions in the file. Exposed for
    /// diagnostics and because a caller reporting "certification broken" wants
    /// to say WHERE.
    int certificationVersion = -1;
    int totalVersions = 0;

    /// Which /Perms /DocMDP layout the level came from.
    DocMdpLayout layout = DocMdpLayout::None;

    // --- FieldMDP (PDF-33) ---

    /// Fully-qualified names listed in a signature's /Lock /Fields.
    std::vector<std::string> lockedFields;
    /// True for /Lock /Action /All — every field in the document is locked.
    bool allFieldsLocked = false;
    /// True for /Lock /Action /Exclude — everything EXCEPT `lockedFields`.
    bool fieldLockIsExclusion = false;
    /// The subset of `changes` whose objects belong to a locked field.
    std::vector<DocMdpChange> lockedFieldChanges;
    /// THE FieldMDP ANSWER: true iff a locked field was changed after signing.
    /// Independent of `level` — a /P 2 document permits form filling in
    /// general and still forbids it for a locked field.
    bool fieldLockViolated = false;

    /// Human-readable summary, suitable for a status bar or a report.
    std::string message;
};

/// Reads /Perms /DocMDP and enforces it — the missing counterpart to
/// SignatureManager's write-side certification stamp.
class DocMdpEnforcer {
public:
    explicit DocMdpEnforcer(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~DocMdpEnforcer();

    DocMdpEnforcer(const DocMdpEnforcer&) = delete;
    DocMdpEnforcer& operator=(const DocMdpEnforcer&) = delete;

    /// Evaluate the document's certification state. Never throws for an
    /// uncertified or non-PDF document — it simply reports `certified=false`,
    /// `violated=false`.
    DocMdpStatus evaluate(const pdfe::core::Document* doc) const;

    /// The permission matrix itself, exposed so a caller (or a test) can ask
    /// the question without a document. This is the single place the /P
    /// semantics live.
    static bool changePermittedAt(DocMdpChangeClass changeClass,
                                  CertificationLevel level);

private:
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace pdfe::signatures
