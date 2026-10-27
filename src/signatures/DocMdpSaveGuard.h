// -*- coding: utf-8 -*-
#pragma once

// Meta-audit 2026-07 follow-up, phase F24 — the CONSUMER for DocMdpEnforcer.
//
// F2 built the read side of /Perms /DocMDP and F21 made it correct on
// third-party files (the ISO 32000-1 §12.8.2.2 layout Acrobat and pyHanko
// emit, which used to fall through to the /P default of 2). Neither wired it
// to anything: `grep -rn "checkDocMdp|DocMdpEnforcer" src/` returned the class
// and nothing else, so a P1 ("no changes permitted") certified document could
// still be edited and saved by PDFE without a word. Detection with no consumer
// is not enforcement.
//
// This file is the consumer. It installs a pdfe::core::Document::SaveGuard —
// the seam declared in Document.h — that evaluates the certification state
// immediately before a save writes anything.
//
// THE PRODUCT DECISION, and why
// -----------------------------
// The guard REFUSES an in-place save and WARNS on a save to a different path.
//
// PDFE is a desktop editor, and a blanket refusal at save time is the one
// outcome that can destroy a user's work: by the time save() is called the
// edit has already been made, and refusing every route to disk strands it.
// Equally, silently overwriting somebody's certified original with a version
// that breaks the certification destroys an artifact whose whole purpose is to
// be tamper-evident, and it does so at the exact moment the evidence (the
// original revision chain) is flattened away by a full-rewrite save.
//
// Splitting on the target resolves both:
//
//   * IN PLACE (Save, or an incremental append — the certified original is the
//     target): REFUSED, with a message naming the level and the first
//     disallowed change. The certified file is never damaged.
//   * ELSEWHERE (Save As to a new path): ALLOWED, with the same message
//     recorded in Document::lastSaveGuardWarning() for the UI to show. The
//     user keeps their work, and the copy is honestly a NEW document that no
//     longer satisfies the certification it carries.
//
// So the user is never cornered — "Save As" is always available — and the
// certified original is never quietly rewritten.
//
// The guard is /P-AWARE, not a blanket block on certified documents: a change
// a certified document's own /P permits (form filling at /P 2, annotations at
// /P 3) passes through untouched. That discrimination is what makes it a
// control rather than an obstruction, and it is what the phase's CONTROL test
// asserts.

#include <memory>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::signatures {

/// Install the production DocMDP save guard into pdfe::core::Document.
/// Reference-counted and paired with uninstallDocMdpSaveGuard(); the last
/// installation supplies the context. Called from MainWindow's constructor.
void installDocMdpSaveGuard(std::shared_ptr<pdfe::core::MuPdfContext> context);

/// Drop one installation claim; the guard is removed when the last one goes.
/// Exists so a session (or a test) can restore the process-wide seam to its
/// pristine state and so a host embedding PDFE can opt out deliberately.
void uninstallDocMdpSaveGuard();

} // namespace pdfe::signatures
