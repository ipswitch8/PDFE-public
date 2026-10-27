// -*- coding: utf-8 -*-
#pragma once

/// Shared, low-level "where are the signatures?" primitives.
///
/// WHY THIS FILE EXISTS (meta-audit follow-up F26, security-audit remediation)
/// -------------------------------------------------------------------------
/// Three subsystems answered that question three different ways, and the
/// narrowest of them shipped in a SECURITY warning:
///
///   * `SignatureManager` (`src/signatures/SignatureManager.cpp`) — the
///     product's own signature listing/UI — enumerates with MuPDF's
///     `pdf_first_widget`/`pdf_next_widget`, i.e. **page annotations**.
///   * `DocMdpEnforcer` (`src/signatures/DocMdpEnforcer.cpp`) — F24's
///     direct-write guard — walks page `/Annots` directly AND the AcroForm
///     field tree, testing the inheritable `/FT == /Sig`. It is written that
///     way *precisely because* AcroForm linkage cannot be assumed.
///   * `EncryptionManager::removeEncryption`'s F26 rewrite-integrity probe
///     originally walked **only** `/Root /AcroForm /Fields`. A document with a
///     signature widget on a page but no (or a broken) AcroForm linkage was
///     therefore invisible to it, and `removeEncryption` destroyed the
///     signature with **no warning on either channel** — the security-relevant
///     direction, since under-warning silently loses a user's signatures while
///     over-warning is merely annoying.
///
/// The predicate below is the one `DocMdpEnforcer` used to keep privately; it
/// now has a single home that any subsystem can share without taking a
/// dependency on `pdfe_signatures` (which drags in OpenSSL, WinHTTP/libcurl and
/// Qt6::Network for what is, at these call sites, a `pdf_obj*` question).
/// `pdfe_core` is the deepest library both `pdfe_encryption` and
/// `pdfe_signatures` already link, so it is where a shared MuPDF-only
/// primitive belongs.

extern "C" {
typedef struct fz_context fz_context;
typedef struct pdf_obj pdf_obj;
typedef struct pdf_document pdf_document;
}

namespace pdfe::core {

/// Is `field` a signature field? `/FT` is INHERITABLE through `/Parent`, so a
/// merged field/widget that carries no `/FT` of its own is still a signature
/// field if an ancestor says so. Depth-capped against a cyclic `/Parent` chain.
///
/// Null-safe: a null or non-dictionary `field` answers false.
bool pdfFieldIsSignature(fz_context* ctx, pdf_obj* field);

/// Count the APPLIED signatures in `doc` — signature value dictionaries that
/// carry a non-empty `/Contents`, and therefore a `/ByteRange` that a full
/// rewrite invalidates.
///
/// The traversal is the UNION of two reachability paths, de-duplicated by the
/// object number of the `/V` signature value dictionary:
///
///   1. `/Root /AcroForm /Fields`, recursively through `/Kids`.
///   2. every page's `/Annots`, taking widget annotations.
///
/// Neither alone is sufficient. A field linked in the AcroForm tree may have no
/// widget on any page (nothing requires one), and a widget on a page may not be
/// reachable from the AcroForm tree (no `/AcroForm` at all, an orphaned or
/// unlinked widget, a broken `/Kids` chain) — the shape a malformed or
/// adversarially-crafted document takes, which is exactly the population this
/// probe exists to be safe on.
///
/// An EMPTY signature field is deliberately NOT counted: it has no
/// `/ByteRange`, so a rewrite breaks nothing and counting it would warn where
/// nothing was lost.
///
/// The applied test is made on the `/V` dictionary itself (dict, `/Contents` a
/// non-empty string) rather than gated on `pdfFieldIsSignature`. That is
/// deliberate — requiring `/FT == /Sig` would reintroduce the same under-warn
/// class on a document whose `/FT` is missing or unreachable.
///
/// Never throws: MuPDF errors are caught per traversal step, so one unparseable
/// page cannot suppress the count for the others. Safe to call from inside a
/// `MuPdfContext::execute` lambda.
int countAppliedSignatures(fz_context* ctx, pdf_document* doc);

} // namespace pdfe::core
