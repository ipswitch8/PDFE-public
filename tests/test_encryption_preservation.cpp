// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F2 — gaps PDF-22 and PDF-28.
//
// PDF-22: nothing in the suite asserted that an ORDINARY save preserves the
// encryption dictionary. It works today only incidentally — `PDF_ENCRYPT_KEEP`
// happens to be 0, and no `pdf_save_document` call site in `src/` sets
// `opts.do_encrypt`, so the zero-initialised `pdf_write_options` keeps the
// existing handler. One `opts.do_encrypt = PDF_ENCRYPT_NONE` away, every
// protected document a user edits and saves is silently decrypted, and no
// test turns red. This file is that missing regression guard.
//
// PDF-28: the same guarantee across `Document::saveIncremental` — the path
// form-fill and signing take on an encrypted document.
//
// The guard asserts three things per save path, because "still encrypted" on
// its own is too weak: the /Encrypt dictionary is present, its REVISION and
// version are unchanged (no silent downgrade to a weaker handler), and the
// permission bits survive. Plus the access proof: the saved file still
// requires the user password, and the wrong password is still rejected.
//
// Over-refusal control: an UNENCRYPTED document saved through the same two
// paths must stay unencrypted — a "preserve encryption" implementation that
// encrypted everything would fail that.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"

#include <QRectF>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::encryption;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path writeEncrypted(const std::filesystem::path& dir,
                                     const std::string& name,
                                     EncryptionAlgorithm algorithm,
                                     const std::string& userPw,
                                     const std::string& ownerPw,
                                     const PermissionFlags& perms) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / "test_single.pdf");
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = algorithm;
    settings.userPassword = userPw;
    settings.ownerPassword = ownerPw;
    settings.permissions = perms;

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

/// Make a real, saveable edit that does not depend on the permission matrix
/// under test: add a Text annotation to page 0.
void makeAnEdit(Document* doc) {
    auto ctx = doc->context();
    pdfe::annotations::AnnotationManager annots(ctx);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    annots.createAnnotation(page.get(),
                            pdfe::annotations::AnnotationType::Text,
                            QRectF(72, 72, 24, 24));
}

struct EncSnapshot {
    bool isEncrypted = false;
    int version = 0;
    int revision = 0;
    int keyLength = 0;
    std::string method;
    PermissionFlags permissions;
};

EncSnapshot snapshot(const std::filesystem::path& p, const std::string& password) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = password.empty() ? Document::open(ctx, p)
                                : Document::open(ctx, p, password);
    EXPECT_NE(doc, nullptr);
    EncryptionManager mgr(ctx);
    auto info = mgr.getEncryptionInfo(doc.get());
    EncSnapshot s;
    s.isEncrypted = info.isEncrypted;
    s.version = info.version;
    s.revision = info.revision;
    s.keyLength = info.keyLength;
    s.method = info.method;
    s.permissions = info.permissions;
    return s;
}

void expectSameHandler(const EncSnapshot& before, const EncSnapshot& after,
                       const char* what) {
    EXPECT_TRUE(after.isEncrypted)
        << what << ": the saved file lost its /Encrypt dictionary entirely";
    EXPECT_EQ(before.revision, after.revision)
        << what << ": encryption REVISION changed (silent handler downgrade)";
    EXPECT_EQ(before.version, after.version)
        << what << ": encryption version (/V) changed";
    EXPECT_EQ(before.method, after.method)
        << what << ": encryption method changed";
    EXPECT_EQ(before.keyLength, after.keyLength)
        << what << ": encryption key length changed";
    EXPECT_EQ(before.permissions, after.permissions)
        << what << ": permission bits did not survive the save";
}

/// Does opening WITHOUT the user password fail? (Document::open returns null
/// or throws when a password is required and not supplied.)
bool requiresPasswordToOpen(const std::filesystem::path& p) {
    auto ctx = std::make_shared<MuPdfContext>();
    try {
        auto doc = Document::open(ctx, p);
        return doc == nullptr;
    } catch (const std::exception&) {
        return true;
    }
}

bool opensWithPassword(const std::filesystem::path& p, const std::string& pw) {
    auto ctx = std::make_shared<MuPdfContext>();
    try {
        auto doc = Document::open(ctx, p, pw);
        return doc != nullptr;
    } catch (const std::exception&) {
        return false;
    }
}

PermissionFlags restrictedPerms() {
    PermissionFlags p = PermissionFlags::all();
    p.copy = false;
    p.modify = false;
    p.assemble = false;
    return p;
}

} // namespace

// ===========================================================================
// PDF-22 — Document::saveAs preserves the encryption dictionary and revision
// ===========================================================================

TEST(EncryptionPreservation, OrdinarySaveAsPreservesEncryptionDictAndRevisionR6) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto src = writeEncrypted(dir, "r6_src.pdf", EncryptionAlgorithm::AES_256,
                              "userpw", "ownerpw", restrictedPerms());
    const EncSnapshot before = snapshot(src, "userpw");
    ASSERT_TRUE(before.isEncrypted);
    ASSERT_EQ(before.revision, 6);

    const auto out = dir / "r6_saveas.pdf";
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, src, "userpw");
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveAs(out);
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    const EncSnapshot after = snapshot(out, "userpw");
    expectSameHandler(before, after, "saveAs @R6");

    // Access proof: the copy is genuinely protected, not merely carrying a
    // leftover dictionary.
    EXPECT_TRUE(requiresPasswordToOpen(out))
        << "saveAs @R6 produced a copy that opens with no password";
    EXPECT_TRUE(opensWithPassword(out, "userpw"));
    EXPECT_FALSE(opensWithPassword(out, "definitely-not-the-password"));
}

TEST(EncryptionPreservation, OrdinarySaveAsPreservesEncryptionDictAndRevisionR4) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto src = writeEncrypted(dir, "r4_src.pdf", EncryptionAlgorithm::AES_128,
                              "userpw", "ownerpw", restrictedPerms());
    const EncSnapshot before = snapshot(src, "userpw");
    ASSERT_TRUE(before.isEncrypted);
    ASSERT_EQ(before.revision, 4);

    const auto out = dir / "r4_saveas.pdf";
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, src, "userpw");
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveAs(out);
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    const EncSnapshot after = snapshot(out, "userpw");
    expectSameHandler(before, after, "saveAs @R4");
    EXPECT_TRUE(requiresPasswordToOpen(out));
    EXPECT_TRUE(opensWithPassword(out, "userpw"));
}

// ===========================================================================
// PDF-28 — Document::saveIncremental preserves it too
// ===========================================================================

TEST(EncryptionPreservation, IncrementalSaveOnEncryptedDocStaysEncrypted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto src = writeEncrypted(dir, "r6_inc.pdf", EncryptionAlgorithm::AES_256,
                              "userpw", "ownerpw", restrictedPerms());
    const EncSnapshot before = snapshot(src, "userpw");
    ASSERT_TRUE(before.isEncrypted);
    ASSERT_EQ(before.revision, 6);

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, src, "userpw");
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveIncremental();   // writes back to `src`
    }

    const EncSnapshot after = snapshot(src, "userpw");
    expectSameHandler(before, after, "saveIncremental @R6");
    EXPECT_TRUE(requiresPasswordToOpen(src))
        << "saveIncremental left the file openable with no password";
    EXPECT_TRUE(opensWithPassword(src, "userpw"));
    EXPECT_FALSE(opensWithPassword(src, "definitely-not-the-password"));
}

TEST(EncryptionPreservation, IncrementalSaveOnEncryptedDocKeepsRevisionR4) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());

    auto src = writeEncrypted(dir, "r4_inc.pdf", EncryptionAlgorithm::AES_128,
                              "userpw", "ownerpw", restrictedPerms());
    const EncSnapshot before = snapshot(src, "userpw");
    ASSERT_EQ(before.revision, 4);

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, src, "userpw");
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveIncremental();
    }

    const EncSnapshot after = snapshot(src, "userpw");
    expectSameHandler(before, after, "saveIncremental @R4");
}

// ===========================================================================
// Over-refusal controls — preservation must not become "always encrypt"
// ===========================================================================

TEST(EncryptionPreservation, UnencryptedSaveAsStaysUnencrypted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const auto out = dir / "plain_saveas.pdf";

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, fixturesDir() / "test_single.pdf");
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveAs(out);
    }

    const EncSnapshot after = snapshot(out, "");
    EXPECT_FALSE(after.isEncrypted)
        << "an unencrypted document must not acquire encryption on saveAs";
    EXPECT_FALSE(requiresPasswordToOpen(out));
}

TEST(EncryptionPreservation, UnencryptedIncrementalSaveStaysUnencrypted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const auto work = dir / "plain_inc.pdf";
    std::filesystem::copy_file(fixturesDir() / "test_single.pdf", work);

    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        makeAnEdit(doc.get());
        doc->saveIncremental();
    }

    const EncSnapshot after = snapshot(work, "");
    EXPECT_FALSE(after.isEncrypted);
    EXPECT_FALSE(requiresPasswordToOpen(work));
}
