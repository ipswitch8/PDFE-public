// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F2 — gaps PDF-31 and PDF-32.
//
// PDF-31: /Perms /DocMDP was WRITTEN by SignatureManager::signField and never
// read back. All nine `DocMDP` hits in `src/` were write-side. A P1
// ("no changes allowed") certified document could be edited and incrementally
// saved with nothing in the product objecting.
//
// PDF-32: the tests that looked like coverage were byte-flip PROXIES.
// `P1CertificationFlagsDisallowedContentChange` XOR-flips a byte and asserts
// the digest breaks — which is equally true for P2, for P3 and for an
// uncertified approval signature. It discriminates NOTHING about /P. An
// honest incremental update, which is what a real editing session produces,
// leaves the digest VALID and was never flagged at all.
//
// The tests below are built around that gap. Every "is it broken" assertion is
// paired with a DISCRIMINATING CONTROL: the same change, byte for byte, made
// against a document certified at a level that permits it, which must NOT be
// reported as broken. Without the control the assertion would be another
// proxy — which is the exact defect PDF-32 names.
//
// Enforcement point under test: src/signatures/DocMdpEnforcer.cpp
// (DocMdpEnforcer::evaluate), reached through
// SignatureManager::checkDocMdp().

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "signatures/DocMdpEnforcer.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"

#include "signatures/SignatureCoverageAnalyzer.h"

#include <QRectF>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::signatures;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

SigningOptions signingOptions(CertificationLevel level) {
    SigningOptions opts;
    opts.pfxPath = (fixturesDir() / "test_signer.pfx").string();
    opts.pfxPassword = "test123";
    opts.reason = "DocMDP enforcement test";
    opts.location = "Test Lab";
    opts.certification = level;
    return opts;
}

/// Produce a document certified at `level`, in `dir`, from `sourceFixture`.
/// The signature field is created and the file saved, then signed in a second
/// open so the signing incremental-save has a real base revision to append to.
std::filesystem::path makeCertified(const std::filesystem::path& dir,
                                    const std::string& name,
                                    const std::string& sourceFixture,
                                    CertificationLevel level) {
    const auto out = dir / name;
    auto ctx = std::make_shared<MuPdfContext>();
    {
        SignatureManager mgr(ctx);
        auto doc = Document::open(ctx, fixturesDir() / sourceFixture);
        EXPECT_NE(doc, nullptr);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "CertSig", QRectF(400, 700, 120, 40));
        doc->saveAs(out);
    }
    {
        SignatureManager mgr(ctx);
        auto doc = Document::open(ctx, out);
        EXPECT_NE(doc, nullptr);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, signingOptions(level));
    }
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

std::string describe(const DocMdpStatus& st) {
    std::string s = st.message
        + " [certified=" + (st.certified ? "yes" : "no")
        + " level=" + std::to_string(static_cast<int>(st.level))
        + " certVersion=" + std::to_string(st.certificationVersion)
        + " totalVersions=" + std::to_string(st.totalVersions)
        + " unsaved=" + std::to_string(st.unsavedVersions)
        + " changes=" + std::to_string(st.changes.size()) + "]";
    for (const auto& c : st.changes) {
        s += "\n    obj " + std::to_string(c.objectNumber)
             + " v" + std::to_string(c.version)
             + " -> " + docMdpChangeClassToString(c.changeClass);
    }
    return s;
}

DocMdpStatus checkOnDisk(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, p);
    EXPECT_NE(doc, nullptr);
    return mgr.checkDocMdp(doc.get());
}

// --- The three change kinds, each applied via the PRODUCT path + a real
// incremental save (which is what leaves the PKCS#7 digest valid and defeats
// the old byte-flip proxies). ---

void applyAnnotationAdd(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::annotations::AnnotationManager annots(ctx);
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    annots.createAnnotation(page.get(), pdfe::annotations::AnnotationType::Text,
                            QRectF(200, 200, 24, 24));
    doc->saveIncremental();
}

/// Structural page edit — permitted by NO certification level.
void applyPageRotate(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    doc->rotatePage(0, 90);
    doc->saveIncremental();
}

/// Fill the first text field. Returns false if the fixture has none.
bool applyFormFill(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::forms::FormManager forms(ctx);
    auto doc = Document::open(ctx, p);
    EXPECT_NE(doc, nullptr);
    auto page = doc->page(0);
    bool filled = false;
    for (const auto& f : forms.getFormFields(page.get())) {
        if (f.type == pdfe::forms::FormFieldType::Text) {
            forms.setFieldValue(page.get(), f.widgetIndex, "DocMDP fill");
            filled = true;
            break;
        }
    }
    if (!filled) return false;
    doc->saveIncremental();
    return true;
}

} // namespace

// ===========================================================================
// The level is read back from /Perms /DocMDP at all — the precondition for
// any /P discrimination.
// ===========================================================================

TEST(DocMdpEnforcement, CertificationLevelIsReadBackFromPerms) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto p1 = makeCertified(dir, "lvl_p1.pdf", "test_single.pdf",
                            CertificationLevel::NoChanges);
    auto p2 = makeCertified(dir, "lvl_p2.pdf", "test_single.pdf",
                            CertificationLevel::FormFillOnly);
    auto p3 = makeCertified(dir, "lvl_p3.pdf", "test_single.pdf",
                            CertificationLevel::AnnotationsAllowed);

    const auto s1 = checkOnDisk(p1);
    const auto s2 = checkOnDisk(p2);
    const auto s3 = checkOnDisk(p3);

    EXPECT_TRUE(s1.certified);
    EXPECT_TRUE(s2.certified);
    EXPECT_TRUE(s3.certified);
    EXPECT_EQ(s1.level, CertificationLevel::NoChanges);
    EXPECT_EQ(s2.level, CertificationLevel::FormFillOnly);
    EXPECT_EQ(s3.level, CertificationLevel::AnnotationsAllowed);
}

// Over-refusal control: freshly certified, nothing touched. If this reported a
// violation, every assertion below would be meaningless.
TEST(DocMdpEnforcement, UntouchedCertifiedDocumentIsNotReportedBroken) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    for (auto level : {CertificationLevel::NoChanges,
                       CertificationLevel::FormFillOnly,
                       CertificationLevel::AnnotationsAllowed}) {
        const std::string name =
            "clean_p" + std::to_string(static_cast<int>(level)) + ".pdf";
        auto path = makeCertified(dir, name, "test_single.pdf", level);
        const auto st = checkOnDisk(path);
        EXPECT_TRUE(st.certified);
        EXPECT_FALSE(st.violated)
            << "untouched P" << static_cast<int>(level)
            << " document reported broken: " << describe(st);
        EXPECT_TRUE(st.disallowedChanges.empty());
    }
}

// An UNCERTIFIED (approval-signed) document has no DocMDP to break — the same
// annotation add must be silent. This separates "certification enforcement"
// from "any change detector".
TEST(DocMdpEnforcement, UncertifiedDocumentIsNeverReportedBroken) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "approval.pdf", "test_single.pdf",
                              CertificationLevel::NotCertification);
    applyAnnotationAdd(path);

    const auto st = checkOnDisk(path);
    EXPECT_FALSE(st.certified)
        << "an approval signature must not stamp /Perms /DocMDP";
    EXPECT_FALSE(st.violated);
    EXPECT_EQ(st.level, CertificationLevel::NotCertification);
}

// ===========================================================================
// PDF-31 / PDF-32 — the headline pair.
//
// ONE change (annotation add). P1 forbids it, P3 permits it. The product must
// say so, and say DIFFERENT things. A byte-flip proxy cannot produce this
// pair, which is precisely why PDF-32 was raised.
// ===========================================================================

TEST(DocMdpEnforcement, P1AnnotationAddIsReportedAsBrokenCertification) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p1_annot.pdf", "test_single.pdf",
                              CertificationLevel::NoChanges);
    ASSERT_FALSE(checkOnDisk(path).violated) << "baseline must be clean";

    applyAnnotationAdd(path);

    const auto st = checkOnDisk(path);
    EXPECT_TRUE(st.certified);
    EXPECT_EQ(st.level, CertificationLevel::NoChanges);
    EXPECT_TRUE(st.changedSinceCertification);
    EXPECT_TRUE(st.violated)
        << "P1 permits NO changes; an annotation add must break the "
           "certification. Status: " << describe(st);
    ASSERT_FALSE(st.disallowedChanges.empty());
}

TEST(DocMdpEnforcement, P3AnnotationAddIsPermittedAndNotReportedBroken) {
    // THE DISCRIMINATING CONTROL for the test above: identical change,
    // identical code path, level that permits it -> must NOT be broken.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p3_annot.pdf", "test_single.pdf",
                              CertificationLevel::AnnotationsAllowed);
    ASSERT_FALSE(checkOnDisk(path).violated) << "baseline must be clean";

    applyAnnotationAdd(path);

    const auto st = checkOnDisk(path);
    EXPECT_TRUE(st.certified);
    EXPECT_EQ(st.level, CertificationLevel::AnnotationsAllowed);
    EXPECT_TRUE(st.changedSinceCertification)
        << "the change must still be SEEN — it is simply permitted";
    EXPECT_FALSE(st.violated)
        << "P3 permits annotation creation; this must not be reported broken. "
           "Status: " << describe(st);
    EXPECT_TRUE(st.disallowedChanges.empty());
}

TEST(DocMdpEnforcement, P2AnnotationAddIsReportedAsBrokenCertification) {
    // Third point on the same axis: P2 permits form fill and signing, NOT
    // annotations. Together with the P1/P3 pair this pins the /P semantics to
    // the actual level rather than to "certified vs not".
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p2_annot.pdf", "test_single.pdf",
                              CertificationLevel::FormFillOnly);
    ASSERT_FALSE(checkOnDisk(path).violated);

    applyAnnotationAdd(path);

    const auto st = checkOnDisk(path);
    EXPECT_EQ(st.level, CertificationLevel::FormFillOnly);
    EXPECT_TRUE(st.violated)
        << "P2 does not permit annotation creation. Status: " << describe(st);
}

// ===========================================================================
// A structural page edit is permitted at NO level — including P3, where the
// annotation add above was lawful. Same document, same save path; only the
// KIND of change differs.
// ===========================================================================

TEST(DocMdpEnforcement, PageRotateBreaksCertificationEvenAtP3) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p3_rotate.pdf", "test_single.pdf",
                              CertificationLevel::AnnotationsAllowed);
    ASSERT_FALSE(checkOnDisk(path).violated);

    applyPageRotate(path);

    const auto st = checkOnDisk(path);
    EXPECT_EQ(st.level, CertificationLevel::AnnotationsAllowed);
    EXPECT_TRUE(st.violated)
        << "no certification level permits a page-level structural edit. "
           "Status: " << describe(st);
}

// ===========================================================================
// PDF-32 explicitly: the incremental update leaves the DIGEST VALID. The old
// proxies only ever asserted on the digest, so they could not see this. The
// certification must be reported broken DESPITE the intact digest.
// ===========================================================================

TEST(DocMdpEnforcement, P1IncrementalChangeIsFlaggedDespiteValidDigest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p1_digest.pdf", "test_single.pdf",
                              CertificationLevel::NoChanges);
    applyAnnotationAdd(path);

    auto ctx = std::make_shared<MuPdfContext>();
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);

    const auto verify = mgr.verifySignature(doc.get(), page.get(), 0);
    // This is the whole point of the gap: the cryptographic digest is FINE.
    EXPECT_TRUE(verify.digestValid)
        << "an incremental update must leave the PKCS#7 digest valid — if it "
           "does not, this test is no longer exercising the PDF-32 scenario";

    const auto st = mgr.checkDocMdp(doc.get());
    EXPECT_TRUE(st.violated)
        << "the certification must be reported broken even though the digest "
           "verifies. Status: " << describe(st);
    EXPECT_FALSE(st.message.empty());
}

// ===========================================================================
// Form fill: forbidden at P1, permitted at P2. The second discriminating pair.
// ===========================================================================

TEST(DocMdpEnforcement, P1FormFillIsReportedAsBrokenCertification) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p1_fill.pdf", "test_form.pdf",
                              CertificationLevel::NoChanges);
    ASSERT_FALSE(checkOnDisk(path).violated);

    ASSERT_TRUE(applyFormFill(path))
        << "test_form.pdf must expose a text field for this test to mean "
           "anything";

    const auto st = checkOnDisk(path);
    EXPECT_EQ(st.level, CertificationLevel::NoChanges);
    EXPECT_TRUE(st.violated)
        << "P1 permits no changes at all, form fill included. Status: "
        << describe(st);
}

TEST(DocMdpEnforcement, P2FormFillIsPermittedAndNotReportedBroken) {
    // DISCRIMINATING CONTROL for the test above.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto path = makeCertified(dir, "p2_fill.pdf", "test_form.pdf",
                              CertificationLevel::FormFillOnly);
    ASSERT_FALSE(checkOnDisk(path).violated);

    ASSERT_TRUE(applyFormFill(path));

    const auto st = checkOnDisk(path);
    EXPECT_EQ(st.level, CertificationLevel::FormFillOnly);
    EXPECT_TRUE(st.changedSinceCertification);
    EXPECT_FALSE(st.violated)
        << "P2 exists precisely to permit form filling. Status: " << describe(st);
}

// ===========================================================================
// Guard for the appearance-RESOURCES classification (phase-15 karen gate).
//
// Making a permitted /P 2 form fill stop reporting BROKEN required teaching the
// classifier that an appearance stream's /Resources are part of the appearance.
// The danger of any such widening is that it also swallows a REAL structural
// change. An earlier attempt did exactly that: it suppressed the report for any
// object that appeared to have no referrer, which (a) rested on a referrer scan
// too shallow to see a 3-hop inline chain, so the object was never orphaned in
// the first place, and (b) would have let a genuine structural edit hide.
//
// These two tests pin the boundary: the widening must not have made page
// content or page resources editable under a certification.
// ===========================================================================

TEST(DocMdpEnforcement, StructuralEditStillBreaksP2AfterAppearanceResourceFix) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    // Same fixture and level as the permitted-fill control, so the ONLY
    // difference is what is edited.
    auto path = makeCertified(dir, "p2_content.pdf", "test_form.pdf",
                              CertificationLevel::FormFillOnly);
    ASSERT_FALSE(checkOnDisk(path).violated);

    // A PAGE-LEVEL structural edit on the very same form fixture whose fill is
    // permitted. This is the discriminating half: identical document, identical
    // certification level, different edit.
    applyPageRotate(path);

    const auto st = checkOnDisk(path);
    EXPECT_TRUE(st.changedSinceCertification);
    EXPECT_TRUE(st.violated)
        << "a page-level structural edit must still break a /P 2 "
           "certification. If this regressed, the appearance-resources "
           "widening has swallowed a real structural change. Status: "
        << describe(st);
}

// NOT-COVERED, stated rather than faked (phase-15 karen gate, second round).
//
// A test named AppearanceResourceWideningIsScopedToFormResources briefly lived
// here. It only called the static changePermittedAt() with hardcoded enum
// values — assertions already present verbatim in
// PermissionMatrixMatchesIso32000Table254 below. It never opened a document,
// never built ObjectRoles and never reached classifyObject, so NO mutation to
// the classification logic could make it fail, including the very regression
// its name claimed to guard. It was a hollow test wearing a scope-guard label,
// and it has been deleted rather than left to imply coverage it never had.
//
// What actually protects the scope today:
//   * classifyObject checks roles.pageResources (-> Other) BEFORE
//     roles.formResources (-> FormFieldValue), so a page resource cannot fall
//     through into the permitted branch. This is enforced by code ORDER.
//   * StructuralEditStillBreaksP2AfterAppearanceResourceFix above, which is a
//     real integration guard and is mutation-killed by an over-permissive
//     classifier.
//
// What is NOT covered: a document in which ONE object is shared between a
// page's /Resources and the form's appearance resources, proving the ordering
// empirically rather than by inspection. That needs a fixture authoring the
// shared object plus a raw incremental update revising it, and it is not
// written. Recorded as an open item in phase-15-dispositions.md.

// ===========================================================================
// The /P matrix itself, asserted directly. This is the single place the
// semantics live (DocMdpEnforcer::changePermittedAt) and it is what makes the
// discrimination above possible.
// ===========================================================================

TEST(DocMdpEnforcement, PermissionMatrixMatchesIso32000Table254) {
    using C = DocMdpChangeClass;
    using L = CertificationLevel;

    // P1: nothing is permitted.
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::FormFieldValue, L::NoChanges));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Annotation,     L::NoChanges));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Signature,      L::NoChanges));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Other,          L::NoChanges));

    // P2: form fill + signing.
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::FormFieldValue, L::FormFillOnly));
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::Signature,      L::FormFillOnly));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Annotation,     L::FormFillOnly));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Other,          L::FormFillOnly));

    // P3: + annotations.
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::FormFieldValue, L::AnnotationsAllowed));
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::Signature,      L::AnnotationsAllowed));
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::Annotation,     L::AnnotationsAllowed));
    EXPECT_FALSE(DocMdpEnforcer::changePermittedAt(C::Other,          L::AnnotationsAllowed));

    // No certification: nothing to violate.
    EXPECT_TRUE (DocMdpEnforcer::changePermittedAt(C::Other, L::NotCertification));
}

// ===========================================================================
// FieldMDP — meta-audit 2026-07 follow-up phase F21, gap PDF-33.
//
// Before F21, `grep -rn "FieldMDP" src/` returned ZERO. A signature could not
// lock a form field, and a document that HAD such a lock (written by Acrobat
// or pyHanko) was read as though it did not: at /P 2 every form fill was
// permitted, including a fill of the one field the signer had pinned.
//
// /P and /Lock answer different questions. /P says which CLASSES of change are
// permitted; /Lock says "not THIS field", inside a class that is otherwise
// permitted. So the discriminating pair below is deliberately BOTH certified
// at /P 2 and BOTH carrying the same /Lock: the only difference is which field
// the post-signature update touches. A detector that cannot read /Lock returns
// the same answer for both.
//
// Fixtures: tests/fixtures/sig_fieldmdp_{locked,unlocked}.pdf, authored by
// tests/fixtures/gen_sig_shadow_corpus.py.
// ===========================================================================

namespace {

DocMdpStatus fieldMdpStatus(const char* fixture) {
    return checkOnDisk(fixturesDir() / fixture);
}

} // namespace

TEST(FieldMdpEnforcement, LockIsReadBackFromTheSignatureField) {
    const auto st = fieldMdpStatus("sig_fieldmdp_locked.pdf");
    ASSERT_TRUE(st.certified) << describe(st);
    EXPECT_EQ(st.level, CertificationLevel::FormFillOnly) << describe(st);
    ASSERT_EQ(st.lockedFields.size(), 1u)
        << "the /Lock names exactly one field. " << describe(st);
    EXPECT_EQ(st.lockedFields.front(), "locked_field") << describe(st);
    EXPECT_FALSE(st.allFieldsLocked)
        << "/Action /Include must not be read as /All — that would lock every "
           "field and make the unlocked control pass for the wrong reason";
    EXPECT_FALSE(st.fieldLockIsExclusion) << describe(st);
}

// The register's proposed name, verbatim.
TEST(FieldMdpEnforcement, FieldMdpLockedFieldRejectsPostSignatureChange) {
    const auto st = fieldMdpStatus("sig_fieldmdp_locked.pdf");
    ASSERT_TRUE(st.certified) << describe(st);
    EXPECT_EQ(st.level, CertificationLevel::FormFillOnly)
        << "the level must be one that PERMITS form filling — otherwise the "
           "violation below could be attributed to /P rather than to /Lock. "
        << describe(st);
    EXPECT_TRUE(st.changedSinceCertification) << describe(st);

    EXPECT_TRUE(st.fieldLockViolated)
        << "a LOCKED field was changed after signing; /P 2 permits form "
           "filling in general and the /Lock is what forbids it here. "
        << describe(st);
    ASSERT_FALSE(st.lockedFieldChanges.empty()) << describe(st);
    EXPECT_TRUE(st.violated)
        << "a broken field lock breaks the certification. " << describe(st);
    EXPECT_TRUE(std::any_of(st.changes.begin(), st.changes.end(),
                            [](const DocMdpChange& c) {
                                return c.touchesLockedField;
                            }))
        << describe(st);
}

// THE DISCRIMINATING CONTROL. Same certification level, same /Lock, same save
// path, different field. Without this the test above would pass for any
// implementation that simply refused every post-signature form change.
TEST(FieldMdpEnforcement, UnlockedFieldChangeIsStillPermittedAtP2) {
    const auto st = fieldMdpStatus("sig_fieldmdp_unlocked.pdf");
    ASSERT_TRUE(st.certified) << describe(st);
    EXPECT_EQ(st.level, CertificationLevel::FormFillOnly) << describe(st);
    ASSERT_EQ(st.lockedFields.size(), 1u) << describe(st);
    EXPECT_EQ(st.lockedFields.front(), "locked_field") << describe(st);

    EXPECT_TRUE(st.changedSinceCertification)
        << "the change must still be SEEN — it is simply permitted. "
        << describe(st);
    EXPECT_FALSE(st.fieldLockViolated)
        << "free_field is not locked; filling it is exactly what /P 2 exists "
           "to permit. " << describe(st);
    EXPECT_TRUE(st.lockedFieldChanges.empty()) << describe(st);
    EXPECT_FALSE(st.violated)
        << "no lock broken, no level exceeded — nothing to report. "
        << describe(st);
}

// The two fixtures differ ONLY in which field the update touches. Asserted so
// that a future regeneration cannot quietly make them differ in some other way
// and turn the pair above into two unrelated tests.
TEST(FieldMdpEnforcement, TheDiscriminatingPairDiffersOnlyInTheChangedField) {
    const auto locked = fieldMdpStatus("sig_fieldmdp_locked.pdf");
    const auto unlocked = fieldMdpStatus("sig_fieldmdp_unlocked.pdf");

    EXPECT_EQ(locked.level, unlocked.level);
    EXPECT_EQ(locked.lockedFields, unlocked.lockedFields);
    EXPECT_EQ(locked.totalVersions, unlocked.totalVersions);
    EXPECT_EQ(locked.changes.size(), unlocked.changes.size())
        << "both updates revise the same number of objects";
    EXPECT_NE(locked.fieldLockViolated, unlocked.fieldLockViolated)
        << "...and the product must nevertheless reach OPPOSITE verdicts. "
           "If these agree, /Lock is not being read at all.\nlocked:   "
        << describe(locked) << "\nunlocked: " << describe(unlocked);
}

// ---------------------------------------------------------------------------
// Write side: PDFE can now PUT a /Lock on the field it signs, and it lands
// inside the signed byte range (a lock an attacker can edit is no lock).
// ---------------------------------------------------------------------------

TEST(FieldMdpEnforcement, SignFieldWritesTheLockInsideTheSignedByteRange) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const auto out = dir / "locked_by_product.pdf";

    // Name a real field of the form fixture so the lock is not vacuous.
    std::string targetField;
    auto ctx = std::make_shared<MuPdfContext>();
    {
        pdfe::forms::FormManager forms(ctx);
        auto doc = Document::open(ctx, fixturesDir() / "test_form.pdf");
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        for (const auto& f : forms.getFormFields(page.get())) {
            if (f.type == pdfe::forms::FormFieldType::Text) {
                targetField = f.fieldName;
                break;
            }
        }
        ASSERT_FALSE(targetField.empty())
            << "test_form.pdf must expose a text field";
        SignatureManager mgr(ctx);
        mgr.createSignatureField(page.get(), "LockSig", QRectF(400, 700, 120, 40));
        doc->saveAs(out);
    }
    {
        SignatureManager mgr(ctx);
        auto doc = Document::open(ctx, out);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        auto opts = signingOptions(CertificationLevel::FormFillOnly);
        opts.fieldLockAction = FieldLockAction::Include;
        opts.lockedFields = {targetField};
        mgr.signField(doc.get(), page.get(), 0, opts);
    }

    const auto st = checkOnDisk(out);
    ASSERT_TRUE(st.certified) << describe(st);
    ASSERT_EQ(st.lockedFields.size(), 1u)
        << "the product must write a /Lock the product can read back. "
        << describe(st);
    EXPECT_EQ(st.lockedFields.front(), targetField) << describe(st);
    EXPECT_FALSE(st.allFieldsLocked);
    EXPECT_FALSE(st.violated) << "nothing has been changed yet. " << describe(st);

    // The lock must lie inside the signed byte range. A /Lock written after
    // the signed prefix would be editable without breaking the digest, which
    // would make the whole mechanism decorative.
    SignatureCoverageAnalyzer analyzer(ctx);
    const auto cov = analyzer.analyze(out);
    ASSERT_EQ(cov.signatureCount, 1) << cov.message;
    const auto& sc = cov.signatures.front();
    ASSERT_EQ(sc.byteRange.size(), 4u);

    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    const size_t lockAt = bytes.find("/Lock");
    ASSERT_NE(lockAt, std::string::npos)
        << "no /Lock in the saved bytes at all";
    EXPECT_LT(static_cast<int64_t>(lockAt), sc.byteRange[1])
        << "/Lock at offset " << lockAt
        << " must fall inside the first signed range [0, " << sc.byteRange[1]
        << ")";
}

// Over-refusal control for the write side: signing WITHOUT a lock must leave
// no /Lock behind and must report no locked fields, so the assertions above
// cannot be satisfied by something that always writes one.
TEST(FieldMdpEnforcement, SigningWithoutALockWritesNoneAndReportsNone) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    auto path = makeCertified(dir, "nolock.pdf", "test_form.pdf",
                              CertificationLevel::FormFillOnly);

    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    std::string lockContext;
    for (size_t at = bytes.find("/Lock"); at != std::string::npos;
         at = bytes.find("/Lock", at + 1)) {
        lockContext += " @" + std::to_string(at) + ": "
                     + bytes.substr(at, 120);
    }

    const auto st = checkOnDisk(path);
    ASSERT_TRUE(st.certified) << describe(st);
    EXPECT_TRUE(st.lockedFields.empty()) << describe(st);
    EXPECT_FALSE(st.allFieldsLocked) << "/Lock sites:" << lockContext;
    EXPECT_FALSE(st.fieldLockViolated);
    EXPECT_EQ(bytes.find("/SigFieldLock"), std::string::npos)
        << "no /Lock was requested; none may be written";
}
