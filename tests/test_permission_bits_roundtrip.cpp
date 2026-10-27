// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F2 — gap PDF-23.
//
// Before this file, `grep -rn "Allowed()" src/core/Document.h
// src/encryption/EncryptionManager.h` returned exactly two accessors:
// printAllowed() and copyAllowed(). Five of the eight ISO 32000-1 Table 22
// user-access permissions — modify, annotate, fill-forms, extract-for-
// accessibility, assemble — plus print-high-resolution had NO accessor at all,
// so no code path in the product could even ask whether they were granted. A
// user who denied "modify" got a document that looked protected and was not.
//
// These tests drive all EIGHT accessors through a real write/read round trip
// at BOTH revision 4 (AES-128, /V 4 /R 4) and revision 6 (AES-256, /V 5 /R 6),
// asserting the writer's revision actually appears in the produced file so a
// silently-downgraded handler cannot pass the suite.
//
// The over-refusal control is structural rather than bolted on: every
// single-bit-denied case asserts the SEVEN other accessors still return true.
// An implementation that refused everything (or that wired every accessor to
// the same bit) fails on those seven, not just on the denied one. The
// all-permissions and unencrypted cases are additional whole-document
// positive controls.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"

#include <QTemporaryDir>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

using namespace pdfe::encryption;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// The eight standard permissions, in ISO 32000-1 Table 22 order plus the
// high-resolution print bit. `select` picks the PermissionFlags member so a
// test can deny exactly one; `read` picks the matching Document accessor so
// the two are compared name-for-name.
struct PermissionBit {
    const char* name;
    bool PermissionFlags::*flag;
    bool (Document::*accessor)() const;
};

const std::array<PermissionBit, 8>& allBits() {
    static const std::array<PermissionBit, 8> bits{{
        {"print",         &PermissionFlags::print,            &Document::printAllowed},
        {"modify",        &PermissionFlags::modify,           &Document::modifyAllowed},
        {"copy",          &PermissionFlags::copy,             &Document::copyAllowed},
        {"annotate",      &PermissionFlags::annotate,         &Document::annotateAllowed},
        {"formFill",      &PermissionFlags::formFill,         &Document::fillFormsAllowed},
        {"accessibility", &PermissionFlags::accessibility,
                                        &Document::extractForAccessibilityAllowed},
        {"assemble",      &PermissionFlags::assemble,         &Document::assembleAllowed},
        {"printHighQuality", &PermissionFlags::printHighQuality,
                                        &Document::printHighResAllowed},
    }};
    return bits;
}

// Encrypt tests/fixtures/test_single.pdf with the given algorithm and
// permission set. Empty user password so the reopen observes the RESTRICTED
// (user-level) view — opening with the owner password would grant everything
// and make the assertions vacuous.
std::filesystem::path writeEncrypted(const std::filesystem::path& dir,
                                     const std::string& name,
                                     EncryptionAlgorithm algorithm,
                                     const PermissionFlags& perms) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / "test_single.pdf");
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = algorithm;
    settings.userPassword = "";
    settings.ownerPassword = "ownerpw";
    settings.permissions = perms;

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

int revisionOf(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, p);
    EXPECT_NE(doc, nullptr);
    EncryptionManager mgr(ctx);
    return mgr.getEncryptionInfo(doc.get()).revision;
}

// Run the whole eight-bit matrix at one algorithm/revision.
void runMatrixAt(EncryptionAlgorithm algorithm, int expectedRevision,
                 const char* label) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    // --- Positive control: all permissions granted -> all eight accessors true.
    {
        auto all = writeEncrypted(dir, std::string("all_") + label + ".pdf",
                                  algorithm, PermissionFlags::all());
        ASSERT_EQ(revisionOf(all), expectedRevision)
            << label << ": writer must actually produce revision "
            << expectedRevision;

        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, all);
        ASSERT_NE(doc, nullptr);
        for (const auto& bit : allBits()) {
            EXPECT_TRUE((doc.get()->*bit.accessor)())
                << label << ": " << bit.name
                << " must be granted on an all-permissions document";
        }
    }

    // --- One bit denied at a time. Each case is its own over-refusal control:
    // the seven bits NOT denied must still read true.
    for (const auto& denied : allBits()) {
        PermissionFlags perms = PermissionFlags::all();
        perms.*(denied.flag) = false;

        if (denied.flag == &PermissionFlags::accessibility) {
            // The extract-for-accessibility bit (10) is deprecated by PDF 2.0
            // and QPDF refuses to clear it for R > 3, so at R4 and R6 the
            // WRITER cannot express "accessibility denied" — a limitation of
            // the write side, not of the accessor. It is asserted explicitly
            // in AccessibilityBitIsNotExpressibleAboveR3 below (so a future
            // QPDF that honours it makes that test fail and forces a revisit)
            // and the accessor's real discrimination is proved end-to-end at
            // R3 in AccessibilityAccessorDiscriminatesAtR3.
            continue;
        }

        auto path = writeEncrypted(
            dir, std::string("deny_") + denied.name + "_" + label + ".pdf",
            algorithm, perms);
        ASSERT_EQ(revisionOf(path), expectedRevision)
            << label << ": denying " << denied.name
            << " must not change the encryption revision";

        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr) << label << ": deny-" << denied.name
                               << " document must open with the empty user password";

        EXPECT_FALSE((doc.get()->*denied.accessor)())
            << label << ": " << denied.name
            << " was denied at save but the accessor still reports it granted";

        for (const auto& other : allBits()) {
            if (other.flag == denied.flag) continue;
            // Denying plain `print` necessarily denies high-resolution print
            // too (ISO 32000-1: bit 12 is meaningful only when bit 3 is set,
            // and QPDF maps print=false to qpdf_r3p_none, clearing both).
            // That is the format's semantics, not over-refusal.
            if (denied.flag == &PermissionFlags::print
                && other.flag == &PermissionFlags::printHighQuality) {
                continue;
            }
            EXPECT_TRUE((doc.get()->*other.accessor)())
                << label << ": denying " << denied.name
                << " must not also deny " << other.name
                << " (over-refusal / bits wired to the same flag)";
        }
    }
}

} // namespace

// ===========================================================================
// PDF-23 — all eight accessors round-trip at R4 (AES-128) and R6 (AES-256)
// ===========================================================================

TEST(PermissionBitsRoundTrip, AllEightBitsRoundTripAtR4) {
    runMatrixAt(EncryptionAlgorithm::AES_128, 4, "R4");
}

TEST(PermissionBitsRoundTrip, AllEightBitsRoundTripAtR6) {
    runMatrixAt(EncryptionAlgorithm::AES_256, 6, "R6");
}

// Both revisions must be reachable from one test run — a build that silently
// produced only one handler would otherwise pass the two tests above with the
// same file twice.
TEST(PermissionBitsRoundTrip, R4AndR6AreDistinctHandlersInTheProducedFiles) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto r4 = writeEncrypted(dir, "handler_r4.pdf", EncryptionAlgorithm::AES_128,
                             PermissionFlags::all());
    auto r6 = writeEncrypted(dir, "handler_r6.pdf", EncryptionAlgorithm::AES_256,
                             PermissionFlags::all());

    auto ctx = std::make_shared<MuPdfContext>();
    EncryptionManager mgr(ctx);

    auto d4 = Document::open(ctx, r4);
    ASSERT_NE(d4, nullptr);
    auto i4 = mgr.getEncryptionInfo(d4.get());
    EXPECT_TRUE(i4.isEncrypted);
    EXPECT_EQ(i4.revision, 4);
    EXPECT_EQ(i4.version, 4);
    EXPECT_EQ(i4.method, "AES-128");

    auto d6 = Document::open(ctx, r6);
    ASSERT_NE(d6, nullptr);
    auto i6 = mgr.getEncryptionInfo(d6.get());
    EXPECT_TRUE(i6.isEncrypted);
    EXPECT_EQ(i6.revision, 6);
    EXPECT_EQ(i6.version, 5);
    EXPECT_EQ(i6.method, "AES-256");
}

// ===========================================================================
// The extract-for-accessibility bit, which R4/R6 cannot express.
//
// This is a WRITE-side limitation found while closing PDF-23, recorded here as
// two assertions rather than as a silently skipped case:
//   * the accessor really does discriminate — proved end-to-end at R3, the
//     newest revision whose writer honours the bit;
//   * R4 and R6 really do force it on — so the day QPDF changes that, this
//     test fails and the matrix above can drop its special case.
// ===========================================================================

TEST(PermissionBitsRoundTrip, AccessibilityAccessorDiscriminatesAtR3) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    PermissionFlags denied = PermissionFlags::all();
    denied.accessibility = false;
    auto deniedPath = writeEncrypted(dir, "a11y_denied_r3.pdf",
                                     EncryptionAlgorithm::RC4_128, denied);
    auto grantedPath = writeEncrypted(dir, "a11y_granted_r3.pdf",
                                      EncryptionAlgorithm::RC4_128,
                                      PermissionFlags::all());

    auto ctx = std::make_shared<MuPdfContext>();

    auto d = Document::open(ctx, deniedPath);
    ASSERT_NE(d, nullptr);
    EXPECT_FALSE(d->extractForAccessibilityAllowed())
        << "R3 denies the accessibility bit; the accessor must report it";
    // Over-refusal control on the same document: the other seven still granted.
    EXPECT_TRUE(d->printAllowed());
    EXPECT_TRUE(d->copyAllowed());
    EXPECT_TRUE(d->modifyAllowed());
    EXPECT_TRUE(d->annotateAllowed());
    EXPECT_TRUE(d->fillFormsAllowed());
    EXPECT_TRUE(d->assembleAllowed());
    EXPECT_TRUE(d->printHighResAllowed());

    auto g = Document::open(ctx, grantedPath);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->extractForAccessibilityAllowed())
        << "same revision, bit granted — the accessor must report THAT too";
}

TEST(PermissionBitsRoundTrip, AccessibilityBitIsNotExpressibleAboveR3) {
    // Documents the write-side limitation as an assertion. If this test starts
    // FAILING, QPDF has begun honouring allow_accessibility for R > 3 and the
    // special case in runMatrixAt() should be removed.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    PermissionFlags denied = PermissionFlags::all();
    denied.accessibility = false;

    auto ctx = std::make_shared<MuPdfContext>();
    for (auto [alg, label] : {std::pair{EncryptionAlgorithm::AES_128, "R4"},
                              std::pair{EncryptionAlgorithm::AES_256, "R6"}}) {
        auto path = writeEncrypted(dir, std::string("a11y_") + label + ".pdf",
                                   alg, denied);
        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr);
        EXPECT_TRUE(doc->extractForAccessibilityAllowed())
            << label << ": QPDF is expected to force bit 10 on for R > 3 "
                        "(PDF 2.0 deprecates it). If this now reports DENIED, "
                        "the writer has gained the capability — remove the "
                        "special case in runMatrixAt().";
    }
}

// Whole-document over-refusal control: an unencrypted document has no
// restrictions to enforce, so every one of the eight accessors must grant.
// An implementation that returned false by default would fail here.
TEST(PermissionBitsRoundTrip, UnencryptedDocumentGrantsAllEight) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_single.pdf");
    ASSERT_NE(doc, nullptr);
    for (const auto& bit : allBits()) {
        EXPECT_TRUE((doc.get()->*bit.accessor)())
            << "unencrypted document must grant " << bit.name;
    }
}

// The EncryptionManager view and the Document accessors must agree — they are
// two faces of the same permission bitmask and a caller may use either.
TEST(PermissionBitsRoundTrip, EncryptionManagerFlagsAgreeWithDocumentAccessors) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    PermissionFlags perms = PermissionFlags::all();
    perms.modify = false;
    perms.assemble = false;
    perms.formFill = false;
    auto path = writeEncrypted(dir, "agree.pdf", EncryptionAlgorithm::AES_256, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    EncryptionManager mgr(ctx);
    const PermissionFlags read = mgr.getPermissions(doc.get());

    EXPECT_EQ(read.print,             doc->printAllowed());
    EXPECT_EQ(read.modify,            doc->modifyAllowed());
    EXPECT_EQ(read.copy,              doc->copyAllowed());
    EXPECT_EQ(read.annotate,          doc->annotateAllowed());
    EXPECT_EQ(read.formFill,          doc->fillFormsAllowed());
    EXPECT_EQ(read.accessibility,     doc->extractForAccessibilityAllowed());
    EXPECT_EQ(read.assemble,          doc->assembleAllowed());
    EXPECT_EQ(read.printHighQuality,  doc->printHighResAllowed());

    // And the three denied bits are actually denied, so the equality above is
    // not comparing eight `true`s to eight `true`s.
    EXPECT_FALSE(doc->modifyAllowed());
    EXPECT_FALSE(doc->assembleAllowed());
    EXPECT_FALSE(doc->fillFormsAllowed());
}
