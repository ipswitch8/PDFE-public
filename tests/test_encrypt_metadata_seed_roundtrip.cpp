// -*- coding: utf-8 -*-
//
// GitHub #180 — the EncryptMetadata checkbox must SEED from the document, must
// HONOR a user override, and must ROUND-TRIP, so the encrypted-metadata state
// never silently flips.
//
// THE DEFECT
// ----------
// EncryptionDialog constructed its "Encrypt metadata" checkbox with a literal
// setChecked(true), and MainWindow::onSaveWithEncryption() showed the dialog
// without ever telling it anything about the open document. So a document
// deliberately saved with /EncryptMetadata false — the configuration you choose
// when an indexer or a DMS has to read the title of an otherwise-encrypted file
// — came back with metadata encryption ON the next time it was re-saved, with
// nothing in the UI saying so. The reverse flip is worse: metadata a user
// believed encrypted becoming readable. Either direction is a confidentiality
// change nobody asked for.
//
// WHY THREE SEPARATE PROPERTIES
// -----------------------------
// "Seeds, honors an override, and round-trips" is three behaviours, and a test
// that checks only the composition of them can pass with any one of them
// broken. They are pinned independently here:
//
//   SeedsFromAnEncryptedDocumentsExistingState
//       the seed alone — the dialog opens on the DOCUMENT's value, in both
//       directions. Reddens if the seed is ignored (the pre-#180 behaviour).
//   AnUntouchedSeededDialogHandsBackTheSeed
//       the seed's other half: open, change nothing, press OK, and the value
//       that comes out is still the document's.
//   UserOverrideBeatsTheSeed
//       the override alone — settings() must report what the USER left in the
//       box. Deliberately written to stay GREEN when the seed is broken: it
//       puts the value in the box itself rather than relying on the seed to
//       have put it there, and where it does touch the seed it asserts
//       relative to what the dialog REPORTS the seed to be, never to a value
//       this case predicts. If a seed mutant reddened this one too, "seeds"
//       and "honors an override" would be a single property wearing two names.
//   MetadataOffSurvivesASaveReopenSeedCycle / MetadataOnSurvives...
//       the round trip through real bytes — save with the box off, reopen the
//       written file, and the seed derived from the reopened document is off
//       again. This is the "never silently flips" property end to end, in both
//       directions.
//
// The round-trip arm goes through EncryptionManager and a real file rather than
// asserting one in-memory struct against another, because the flip this issue
// is about happens across a save.

#include <gtest/gtest.h>

#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "ui/EncryptionDialog.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::encryption::EncryptionAlgorithm;
using pdfe::encryption::EncryptionInfo;
using pdfe::encryption::EncryptionManager;
using pdfe::encryption::EncryptionSettings;
using pdfe::ui::EncryptionDialog;

namespace {

namespace fs = std::filesystem;

fs::path plainFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
}

fs::path scratchDir() {
    auto d = fs::temp_directory_path() / "pdfe_encrypt_metadata_seed";
    fs::create_directories(d);
    return d;
}

} // namespace

class EncryptMetadataSeedTest : public ::testing::Test {
protected:
    void SetUp() override {
        // The fixture is TRACKED (git ls-files tests/fixtures/test_single.pdf),
        // so its absence is a broken checkout, not an environment this test may
        // excuse itself from. A skip here would hide the defect, which is the
        // entire failure mode #180 is about.
        ASSERT_TRUE(fs::exists(plainFixture()))
            << plainFixture().string() << " is missing; it is a tracked fixture "
               "and this test must not be skipped for its absence";
        ASSERT_NE(qApp, nullptr)
            << "EncryptionDialog is a QWidget and tests/test_main.cpp creates a "
               "QApplication; if that has gone away this test cannot be run and "
               "must say so rather than pass";
        ctx_ = std::make_shared<MuPdfContext>();
        mgr_ = std::make_unique<EncryptionManager>(ctx_);
    }

    /// Write `plainFixture()` out encrypted with the given metadata setting and
    /// return the path. AES-256 because /EncryptMetadata is only expressible
    /// from R4 upward, and R6 is what PDFE writes by default.
    fs::path writeEncrypted(const char* name, bool encryptMetadata) {
        const fs::path out = scratchDir() / name;
        std::error_code ec;
        fs::remove(out, ec);

        auto doc = Document::open(ctx_, plainFixture());
        EXPECT_NE(doc, nullptr);
        if (!doc) return out;

        EncryptionSettings s;
        s.algorithm = EncryptionAlgorithm::AES_256;
        s.userPassword = "user-180";
        s.ownerPassword = "owner-180";
        s.encryptMetadata = encryptMetadata;
        mgr_->saveEncrypted(doc.get(), out, s);
        return out;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<EncryptionManager> mgr_;
};

// ---------------------------------------------------------------------------
// 1. SEED — the dialog opens on the document's value, not on a constant.
// ---------------------------------------------------------------------------

TEST_F(EncryptMetadataSeedTest, SeedsFromAnEncryptedDocumentsExistingState) {
    const fs::path off = writeEncrypted("seed_meta_off.pdf", false);
    ASSERT_TRUE(fs::exists(off));

    auto reopened = Document::open(ctx_, off, "user-180");
    ASSERT_NE(reopened, nullptr);
    const EncryptionInfo info = mgr_->getEncryptionInfo(reopened.get());
    ASSERT_TRUE(info.isEncrypted);
    ASSERT_FALSE(info.metadataEncrypted)
        << "precondition: the document written with encryptMetadata=false must "
           "report metadataEncrypted=false, otherwise this test is seeding from "
           "the wrong value and proves nothing";

    EncryptionDialog dialog(nullptr);
    auto* box = dialog.findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->isChecked())
        << "precondition: an unseeded dialog starts checked, which is exactly "
           "why the seed has to happen";

    dialog.seedFromExistingEncryption(info);

    EXPECT_FALSE(box->isChecked())
        << "SEED: the dialog opened with 'Encrypt metadata' CHECKED for a "
           "document whose /EncryptMetadata is false. Accepting the dialog "
           "unchanged would silently turn metadata encryption back ON — a "
           "confidentiality change the user never asked for.";
    EXPECT_FALSE(dialog.seededEncryptMetadata())
        << "SEED: the recorded seed does not match the document's "
           "metadataEncrypted, so 'was this overridden?' cannot be answered";
    EXPECT_FALSE(dialog.encryptMetadataOverridden())
        << "SEED: seeding alone must not read as a user override";

    // ...and the same seam, seeded the other way, must land the other way. A
    // seed that always produced 'unchecked' would pass the assertions above.
    const fs::path on = writeEncrypted("seed_meta_on.pdf", true);
    auto reopenedOn = Document::open(ctx_, on, "user-180");
    ASSERT_NE(reopenedOn, nullptr);
    const EncryptionInfo infoOn = mgr_->getEncryptionInfo(reopenedOn.get());
    ASSERT_TRUE(infoOn.metadataEncrypted);

    EncryptionDialog dialogOn(nullptr);
    auto* boxOn = dialogOn.findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(boxOn, nullptr);
    boxOn->setChecked(false);            // start from the OPPOSITE of the seed
    dialogOn.seedFromExistingEncryption(infoOn);
    EXPECT_TRUE(boxOn->isChecked())
        << "SEED: a document whose /EncryptMetadata is TRUE must seed the box "
           "checked; the seed is not following the document, it is following a "
           "constant";
    EXPECT_TRUE(dialogOn.seededEncryptMetadata());
}

// ---------------------------------------------------------------------------
// 2. OVERRIDE — what the user leaves in the box is what gets written.
// ---------------------------------------------------------------------------

// DELIBERATELY INDEPENDENT OF THE SEED. This case must stay GREEN when the
// seed is broken, or "seeds AND honors an override" collapses into one
// property and a fix for either would appear to fix both. It therefore never
// asserts that the seed put a particular value in the box: it puts the value
// in the box itself, exactly as a user's click does, and asserts on what comes
// back out. `encryptMetadataOverridden()` is exercised against a seed the case
// asserts nothing about beyond it being the value it was handed.
TEST_F(EncryptMetadataSeedTest, UserOverrideBeatsTheSeed) {
    auto prepare = [](EncryptionDialog& d) {
        auto* algo = d.findChild<QComboBox*>("algorithmCombo");
        EXPECT_NE(algo, nullptr);
        if (!algo) return;
        algo->setCurrentIndex(4);        // AES-256 — settings() only fills the
                                         // metadata flag when encrypting
        d.findChild<QLineEdit*>("userPasswordEdit")->setText("user-180");
        d.findChild<QLineEdit*>("ownerPasswordEdit")->setText("owner-180");
    };

    // The user leaves the box UNCHECKED: settings() must say off.
    EncryptionDialog off(nullptr);
    auto* offBox = off.findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(offBox, nullptr);
    prepare(off);
    offBox->setChecked(false);
    EXPECT_FALSE(off.settings().encryptMetadata)
        << "OVERRIDE: the user unchecked 'Encrypt metadata' and settings() "
           "still reports true — something is overwriting the user's choice, "
           "which is the same silent flip in the opposite direction";

    // The user leaves the box CHECKED: settings() must say on. Both directions,
    // because a settings() hardwired to either constant passes one of them.
    EncryptionDialog on(nullptr);
    auto* onBox = on.findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(onBox, nullptr);
    prepare(on);
    onBox->setChecked(true);
    EXPECT_TRUE(on.settings().encryptMetadata)
        << "OVERRIDE: the user checked 'Encrypt metadata' and settings() "
           "reports false";

    // And the dialog must be able to SAY that the user changed it, whatever the
    // seed happened to be. Asserted relative to the seed the dialog reports,
    // not to a value this case predicts, so a broken seed cannot redden it.
    EncryptionInfo anySeed;
    anySeed.isEncrypted = true;
    anySeed.metadataEncrypted = true;

    EncryptionDialog probe(nullptr);
    auto* probeBox = probe.findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(probeBox, nullptr);
    prepare(probe);   // settings() only fills encryptMetadata when encrypting
    probe.seedFromExistingEncryption(anySeed);
    probeBox->setChecked(probe.seededEncryptMetadata());
    EXPECT_FALSE(probe.encryptMetadataOverridden())
        << "OVERRIDE: the box agrees with the seed and the dialog calls that an "
           "override";
    probeBox->setChecked(!probe.seededEncryptMetadata());
    EXPECT_TRUE(probe.encryptMetadataOverridden())
        << "OVERRIDE: the box disagrees with the seed and the dialog does not "
           "call that an override, so 'the user asked for this' cannot be "
           "distinguished from 'the document said so'";
    EXPECT_EQ(probe.settings().encryptMetadata, probeBox->isChecked())
        << "OVERRIDE: settings() disagrees with the live checkbox";
}

// The seed's other half: an UNTOUCHED seeded dialog hands back the seed. This
// one does belong with the seed, not with the override, because it is entirely
// a statement about what seeding did.
TEST_F(EncryptMetadataSeedTest, AnUntouchedSeededDialogHandsBackTheSeed) {
    EncryptionInfo seedOff;
    seedOff.isEncrypted = true;
    seedOff.metadataEncrypted = false;

    EncryptionDialog dialog(nullptr);
    dialog.seedFromExistingEncryption(seedOff);
    auto* algo = dialog.findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algo, nullptr);
    algo->setCurrentIndex(4);
    dialog.findChild<QLineEdit*>("userPasswordEdit")->setText("user-180");
    dialog.findChild<QLineEdit*>("ownerPasswordEdit")->setText("owner-180");

    EXPECT_FALSE(dialog.encryptMetadataOverridden());
    EXPECT_FALSE(dialog.settings().encryptMetadata)
        << "SEED: an untouched dialog seeded OFF handed back ON; that is "
           "precisely the silent flip — the user opened the dialog, changed "
           "nothing, pressed OK, and the document's metadata became encrypted";
}

// ---------------------------------------------------------------------------
// 3. ROUND TRIP — through real bytes, twice, with no flip.
// ---------------------------------------------------------------------------

TEST_F(EncryptMetadataSeedTest, MetadataOffSurvivesASaveReopenSeedCycle) {
    // Pass 1: write with metadata encryption OFF.
    const fs::path pass1 = writeEncrypted("roundtrip_pass1.pdf", false);
    ASSERT_TRUE(fs::exists(pass1));

    auto doc1 = Document::open(ctx_, pass1, "user-180");
    ASSERT_NE(doc1, nullptr);
    const EncryptionInfo info1 = mgr_->getEncryptionInfo(doc1.get());
    ASSERT_TRUE(info1.isEncrypted);
    EXPECT_FALSE(info1.metadataEncrypted)
        << "ROUND TRIP: /EncryptMetadata false did not survive the first save";

    // Pass 2: the user re-saves with encryption, touching nothing. The dialog
    // seeds from the document, the user accepts, and the settings that come
    // back out are what gets written.
    EncryptionDialog dialog(nullptr);
    dialog.seedFromExistingEncryption(info1);
    auto* algo = dialog.findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algo, nullptr);
    algo->setCurrentIndex(4);            // AES-256
    dialog.findChild<QLineEdit*>("userPasswordEdit")->setText("user-180");
    dialog.findChild<QLineEdit*>("ownerPasswordEdit")->setText("owner-180");
    const EncryptionSettings resave = dialog.settings();
    EXPECT_FALSE(resave.encryptMetadata)
        << "ROUND TRIP: the untouched re-save dialog handed back "
           "encryptMetadata=true for a document that had it false";

    const fs::path pass2 = scratchDir() / "roundtrip_pass2.pdf";
    std::error_code ec;
    fs::remove(pass2, ec);
    mgr_->saveEncrypted(doc1.get(), pass2, resave);
    ASSERT_TRUE(fs::exists(pass2));

    auto doc2 = Document::open(ctx_, pass2, "user-180");
    ASSERT_NE(doc2, nullptr);
    const EncryptionInfo info2 = mgr_->getEncryptionInfo(doc2.get());
    ASSERT_TRUE(info2.isEncrypted);
    EXPECT_FALSE(info2.metadataEncrypted)
        << "ROUND TRIP: metadata encryption flipped from OFF to ON across a "
           "save/reopen cycle in which the user changed nothing. That is the "
           "silent confidentiality change GitHub #180 is about.";

    // And the dialog opened on the re-saved file agrees, so the cycle is stable
    // rather than merely correct once.
    EncryptionDialog dialog2(nullptr);
    dialog2.seedFromExistingEncryption(info2);
    EXPECT_FALSE(dialog2.seededEncryptMetadata())
        << "ROUND TRIP: the second-generation file seeds the dialog ON; the "
           "state is oscillating rather than round-tripping";
}

// The same cycle in the other direction: a document that IS metadata-encrypted
// must not quietly become readable.
TEST_F(EncryptMetadataSeedTest, MetadataOnSurvivesASaveReopenSeedCycle) {
    const fs::path pass1 = writeEncrypted("roundtrip_on_pass1.pdf", true);
    ASSERT_TRUE(fs::exists(pass1));

    auto doc1 = Document::open(ctx_, pass1, "user-180");
    ASSERT_NE(doc1, nullptr);
    const EncryptionInfo info1 = mgr_->getEncryptionInfo(doc1.get());
    ASSERT_TRUE(info1.metadataEncrypted);

    EncryptionDialog dialog(nullptr);
    dialog.seedFromExistingEncryption(info1);
    auto* algo = dialog.findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algo, nullptr);
    algo->setCurrentIndex(4);
    dialog.findChild<QLineEdit*>("userPasswordEdit")->setText("user-180");
    dialog.findChild<QLineEdit*>("ownerPasswordEdit")->setText("owner-180");
    const EncryptionSettings resave = dialog.settings();
    ASSERT_TRUE(resave.encryptMetadata);

    const fs::path pass2 = scratchDir() / "roundtrip_on_pass2.pdf";
    std::error_code ec;
    fs::remove(pass2, ec);
    mgr_->saveEncrypted(doc1.get(), pass2, resave);

    auto doc2 = Document::open(ctx_, pass2, "user-180");
    ASSERT_NE(doc2, nullptr);
    EXPECT_TRUE(mgr_->getEncryptionInfo(doc2.get()).metadataEncrypted)
        << "ROUND TRIP: metadata encryption flipped from ON to OFF across a "
           "save/reopen cycle the user did not ask to change — the metadata of "
           "an encrypted document became readable without a word";
}
