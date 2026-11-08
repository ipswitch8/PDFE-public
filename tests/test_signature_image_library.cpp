// -*- coding: utf-8 -*-
//
// GitHub #362 — persisted signature-image library (save a browsed signature
// image once, reuse it across sessions).
//
// These tests mirror the ImageStampLibrary persistence test
// (test_image_stamp.cpp: ImageStampLibrary.SaveAndLoadFromSettingsRoundTrip),
// including its isolation discipline: SignatureImageLibrary follows the
// ambient QCoreApplication org/app identity (bare QSettings, see
// SignatureImageLibrary.cpp:72 / ImageStampLibrary.cpp:68), so a
// pdfe_test::SettingsScope pointed at a purge-on-exit PDFE-Test hive makes
// every write land in a test-owned store that is deleted when the process
// ends — NOT the real HKCU\Software\PDFE user hive, and never referencing a
// C:\Users or build-account path.
#include <gtest/gtest.h>

#include "annotations/SignatureImageLibrary.h"
#include "support/SettingsIsolation.h"

#include <QImage>
#include <QSettings>
#include <QString>
#include <QStringList>

using pdfe::annotations::SignatureImageLibrary;

namespace {

// A tiny opaque image whose fill colour we can read back to prove the PNG
// payload round-tripped, not just the name.
QImage makeImage(int w, int h, QRgb colour)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(colour);
    return img;
}

} // namespace

// An empty library lists zero entries and reports nothing present.
TEST(SignatureImageLibrary, ListsZeroEntriesWhenEmpty)
{
    SignatureImageLibrary lib;
    EXPECT_EQ(lib.count(), 0);
    EXPECT_TRUE(lib.signatureImageNames().isEmpty());
    EXPECT_FALSE(lib.hasSignatureImage(QStringLiteral("anything")));
    EXPECT_TRUE(lib.signatureImage(QStringLiteral("anything")).isNull());
}

// Re-saving under an existing name UPDATES the payload and does not create a
// duplicate entry.
TEST(SignatureImageLibrary, ReSaveSameNameOverwritesNoDuplicate)
{
    SignatureImageLibrary lib;
    ASSERT_TRUE(lib.addSignatureImage(QStringLiteral("jane"),
                                      makeImage(8, 8, qRgb(10, 20, 30))));
    EXPECT_EQ(lib.count(), 1);

    // Same name, different image: must replace in place.
    ASSERT_TRUE(lib.addSignatureImage(QStringLiteral("jane"),
                                      makeImage(16, 24, qRgb(200, 100, 50))));
    EXPECT_EQ(lib.count(), 1) << "Same name must overwrite, not append";
    EXPECT_EQ(lib.signatureImageNames(),
              QStringList{QStringLiteral("jane")});

    QImage got = lib.signatureImage(QStringLiteral("jane"));
    ASSERT_FALSE(got.isNull());
    EXPECT_EQ(got.width(), 16);
    EXPECT_EQ(got.height(), 24);
}

// Null image / empty name are rejected.
TEST(SignatureImageLibrary, RejectsEmptyNameOrNullImage)
{
    SignatureImageLibrary lib;
    EXPECT_FALSE(lib.addSignatureImage(QString(), makeImage(4, 4, qRgb(0, 0, 0))));
    EXPECT_FALSE(lib.addSignatureImage(QStringLiteral("x"), QImage()));
    EXPECT_EQ(lib.count(), 0);
}

// Remove behaves: removes once, is a no-op the second time.
TEST(SignatureImageLibrary, RemoveSignatureImage)
{
    SignatureImageLibrary lib;
    lib.addSignatureImage(QStringLiteral("a"), makeImage(4, 4, qRgb(1, 2, 3)));
    lib.addSignatureImage(QStringLiteral("b"), makeImage(4, 4, qRgb(4, 5, 6)));
    EXPECT_EQ(lib.count(), 2);
    EXPECT_TRUE(lib.removeSignatureImage(QStringLiteral("a")));
    EXPECT_FALSE(lib.removeSignatureImage(QStringLiteral("a")))
        << "second remove is a no-op";
    EXPECT_EQ(lib.count(), 1);
    EXPECT_FALSE(lib.hasSignatureImage(QStringLiteral("a")));
    EXPECT_TRUE(lib.hasSignatureImage(QStringLiteral("b")));
}

// THE across-sessions guarantee: an image saved via one library instance is
// loaded correctly by a FRESH instance constructed against the SAME settings
// store. This is the behaviour #362 asks for — a saved signature survives to a
// later session. The store is a test-owned, purge-on-exit QSettings scope, so
// the test is hermetic and never touches the real user hive.
TEST(SignatureImageLibrary, PersistsAcrossFreshInstanceSameSettingsStore)
{
    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE-Test"),
        QStringLiteral("SignatureImageLibraryTest"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    {
        QSettings cleanup;
        cleanup.clear();
    }

    const QRgb logoColour = qRgb(222, 111, 33);
    {
        SignatureImageLibrary writer;
        ASSERT_TRUE(writer.addSignatureImage(QStringLiteral("jane-signature"),
                                             makeImage(20, 10, logoColour)));
        ASSERT_TRUE(writer.addSignatureImage(QStringLiteral("initials"),
                                             makeImage(8, 8, qRgb(0, 0, 0))));
        writer.saveToSettings();
    }

    // A brand-new instance — the stand-in for "next session" — reads the same
    // store from scratch.
    SignatureImageLibrary reader;
    EXPECT_EQ(reader.count(), 0) << "reader starts empty before loading";
    reader.loadFromSettings();

    EXPECT_EQ(reader.count(), 2)
        << "both saved signature images must round-trip through QSettings";
    EXPECT_TRUE(reader.hasSignatureImage(QStringLiteral("jane-signature")));
    EXPECT_TRUE(reader.hasSignatureImage(QStringLiteral("initials")));

    // Insertion order is preserved.
    QStringList names = reader.signatureImageNames();
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], QStringLiteral("jane-signature"));
    EXPECT_EQ(names[1], QStringLiteral("initials"));

    // The payload itself round-trips: dimensions and a sampled pixel colour.
    QImage jane = reader.signatureImage(QStringLiteral("jane-signature"));
    ASSERT_FALSE(jane.isNull());
    EXPECT_EQ(jane.width(), 20);
    EXPECT_EQ(jane.height(), 10);
    EXPECT_EQ(jane.pixel(0, 0), logoColour)
        << "the saved signature pixels must survive the PNG/base64 round-trip";

    {
        QSettings cleanup;
        cleanup.clear();
    }
}

// Overwriting then re-loading a fresh instance yields the UPDATED payload and
// still a single entry — the overwrite semantics persist, not just hold
// in-memory.
TEST(SignatureImageLibrary, OverwritePersistsAsSingleUpdatedEntry)
{
    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE-Test"),
        QStringLiteral("SignatureImageLibraryOverwriteTest"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    {
        QSettings cleanup;
        cleanup.clear();
    }

    {
        SignatureImageLibrary writer;
        ASSERT_TRUE(writer.addSignatureImage(QStringLiteral("sig"),
                                             makeImage(4, 4, qRgb(1, 1, 1))));
        ASSERT_TRUE(writer.addSignatureImage(QStringLiteral("sig"),
                                             makeImage(12, 6, qRgb(9, 8, 7))));
        writer.saveToSettings();
    }

    SignatureImageLibrary reader;
    reader.loadFromSettings();
    EXPECT_EQ(reader.count(), 1) << "overwrite must not persist a duplicate";
    QImage sig = reader.signatureImage(QStringLiteral("sig"));
    ASSERT_FALSE(sig.isNull());
    EXPECT_EQ(sig.width(), 12);
    EXPECT_EQ(sig.height(), 6);

    {
        QSettings cleanup;
        cleanup.clear();
    }
}
