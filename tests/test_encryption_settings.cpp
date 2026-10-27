#include <gtest/gtest.h>
#include "encryption/EncryptionSettings.h"

using namespace pdfe::encryption;

// ============================================================================
// PermissionFlags Tests
// ============================================================================

TEST(PermissionFlagsTest, AllPermissions_DefaultConstruction) {
    PermissionFlags flags;

    EXPECT_TRUE(flags.print);
    EXPECT_TRUE(flags.printHighQuality);
    EXPECT_TRUE(flags.modify);
    EXPECT_TRUE(flags.copy);
    EXPECT_TRUE(flags.annotate);
    EXPECT_TRUE(flags.formFill);
    EXPECT_TRUE(flags.assemble);
    EXPECT_TRUE(flags.accessibility);
}

TEST(PermissionFlagsTest, AllPermissions_AllFactory) {
    PermissionFlags flags = PermissionFlags::all();

    EXPECT_TRUE(flags.print);
    EXPECT_TRUE(flags.printHighQuality);
    EXPECT_TRUE(flags.modify);
    EXPECT_TRUE(flags.copy);
    EXPECT_TRUE(flags.annotate);
    EXPECT_TRUE(flags.formFill);
    EXPECT_TRUE(flags.assemble);
    EXPECT_TRUE(flags.accessibility);

    // Should match default construction
    PermissionFlags defaultFlags;
    EXPECT_EQ(flags, defaultFlags);
}

TEST(PermissionFlagsTest, ReadOnly_Factory) {
    PermissionFlags flags = PermissionFlags::readOnly();

    EXPECT_FALSE(flags.print);
    EXPECT_FALSE(flags.printHighQuality);
    EXPECT_FALSE(flags.modify);
    EXPECT_FALSE(flags.copy);
    EXPECT_FALSE(flags.annotate);
    EXPECT_FALSE(flags.formFill);
    EXPECT_FALSE(flags.assemble);
    EXPECT_TRUE(flags.accessibility);  // Only accessibility is true
}

TEST(PermissionFlagsTest, ToBitmask_AllPermissions) {
    PermissionFlags flags = PermissionFlags::all();
    int bitmask = flags.toBitmask();

    // Verify each permission bit is set
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_PRINT);        // bit 2 = 4
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_MODIFY);       // bit 3 = 8
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_COPY);         // bit 4 = 16
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_ANNOTATE);     // bit 5 = 32
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_FORM);         // bit 8 = 256
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_ACCESSIBILITY);// bit 9 = 512
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_ASSEMBLE);     // bit 10 = 1024
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_PRINT_HQ);     // bit 11 = 2048

    // Verify exact values
    EXPECT_EQ(4, PermissionFlags::PDF_PERM_PRINT);
    EXPECT_EQ(8, PermissionFlags::PDF_PERM_MODIFY);
    EXPECT_EQ(16, PermissionFlags::PDF_PERM_COPY);
    EXPECT_EQ(32, PermissionFlags::PDF_PERM_ANNOTATE);
    EXPECT_EQ(256, PermissionFlags::PDF_PERM_FORM);
    EXPECT_EQ(512, PermissionFlags::PDF_PERM_ACCESSIBILITY);
    EXPECT_EQ(1024, PermissionFlags::PDF_PERM_ASSEMBLE);
    EXPECT_EQ(2048, PermissionFlags::PDF_PERM_PRINT_HQ);

    // Expected bitmask value with all permissions
    int expectedMask = 4 + 8 + 16 + 32 + 256 + 512 + 1024 + 2048;
    EXPECT_EQ(expectedMask, bitmask);
}

TEST(PermissionFlagsTest, ToBitmask_ReadOnly) {
    PermissionFlags flags = PermissionFlags::readOnly();
    int bitmask = flags.toBitmask();

    // Should only have accessibility bit set
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_PRINT);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_MODIFY);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_COPY);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_ANNOTATE);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_FORM);
    EXPECT_NE(0, bitmask & PermissionFlags::PDF_PERM_ACCESSIBILITY);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_ASSEMBLE);
    EXPECT_EQ(0, bitmask & PermissionFlags::PDF_PERM_PRINT_HQ);

    EXPECT_EQ(512, bitmask);  // Only accessibility bit
}

TEST(PermissionFlagsTest, FromBitmask_AllBits) {
    int allBits = PermissionFlags::PDF_PERM_PRINT |
                  PermissionFlags::PDF_PERM_MODIFY |
                  PermissionFlags::PDF_PERM_COPY |
                  PermissionFlags::PDF_PERM_ANNOTATE |
                  PermissionFlags::PDF_PERM_FORM |
                  PermissionFlags::PDF_PERM_ACCESSIBILITY |
                  PermissionFlags::PDF_PERM_ASSEMBLE |
                  PermissionFlags::PDF_PERM_PRINT_HQ;

    PermissionFlags flags = PermissionFlags::fromBitmask(allBits);

    EXPECT_TRUE(flags.print);
    EXPECT_TRUE(flags.printHighQuality);
    EXPECT_TRUE(flags.modify);
    EXPECT_TRUE(flags.copy);
    EXPECT_TRUE(flags.annotate);
    EXPECT_TRUE(flags.formFill);
    EXPECT_TRUE(flags.assemble);
    EXPECT_TRUE(flags.accessibility);

    // Should match all()
    EXPECT_EQ(PermissionFlags::all(), flags);
}

TEST(PermissionFlagsTest, FromBitmask_NoBits) {
    PermissionFlags flags = PermissionFlags::fromBitmask(0);

    EXPECT_FALSE(flags.print);
    EXPECT_FALSE(flags.printHighQuality);
    EXPECT_FALSE(flags.modify);
    EXPECT_FALSE(flags.copy);
    EXPECT_FALSE(flags.annotate);
    EXPECT_FALSE(flags.formFill);
    EXPECT_FALSE(flags.assemble);
    EXPECT_FALSE(flags.accessibility);
}

TEST(PermissionFlagsTest, FromBitmask_Roundtrip) {
    // Test all permissions
    {
        PermissionFlags original = PermissionFlags::all();
        int bitmask1 = original.toBitmask();
        PermissionFlags converted = PermissionFlags::fromBitmask(bitmask1);
        int bitmask2 = converted.toBitmask();

        EXPECT_EQ(bitmask1, bitmask2);
        EXPECT_EQ(original, converted);
    }

    // Test read-only permissions
    {
        PermissionFlags original = PermissionFlags::readOnly();
        int bitmask1 = original.toBitmask();
        PermissionFlags converted = PermissionFlags::fromBitmask(bitmask1);
        int bitmask2 = converted.toBitmask();

        EXPECT_EQ(bitmask1, bitmask2);
        EXPECT_EQ(original, converted);
    }

    // Test arbitrary combination
    {
        PermissionFlags original;
        original.print = true;
        original.printHighQuality = false;
        original.modify = false;
        original.copy = true;
        original.annotate = false;
        original.formFill = true;
        original.assemble = false;
        original.accessibility = true;

        int bitmask1 = original.toBitmask();
        PermissionFlags converted = PermissionFlags::fromBitmask(bitmask1);
        int bitmask2 = converted.toBitmask();

        EXPECT_EQ(bitmask1, bitmask2);
        EXPECT_EQ(original, converted);
    }
}

TEST(PermissionFlagsTest, Equality_SameFlags) {
    PermissionFlags flags1 = PermissionFlags::all();
    PermissionFlags flags2 = PermissionFlags::all();

    EXPECT_EQ(flags1, flags2);

    PermissionFlags flags3 = PermissionFlags::readOnly();
    PermissionFlags flags4 = PermissionFlags::readOnly();

    EXPECT_EQ(flags3, flags4);
}

TEST(PermissionFlagsTest, Equality_DifferentFlags) {
    PermissionFlags allPerms = PermissionFlags::all();
    PermissionFlags readOnly = PermissionFlags::readOnly();

    EXPECT_NE(allPerms, readOnly);

    // Test different individual flags
    PermissionFlags flags1;
    PermissionFlags flags2;
    flags2.print = false;

    EXPECT_NE(flags1, flags2);
}

// ============================================================================
// EncryptionSettings Tests
// ============================================================================

TEST(EncryptionSettingsTest, Defaults) {
    EncryptionSettings settings;

    EXPECT_EQ(EncryptionAlgorithm::None, settings.algorithm);
    EXPECT_TRUE(settings.userPassword.empty());
    EXPECT_TRUE(settings.ownerPassword.empty());
    EXPECT_FALSE(settings.isEncrypted());
    EXPECT_FALSE(settings.hasUserPassword());
    EXPECT_FALSE(settings.hasOwnerPassword());
    EXPECT_TRUE(settings.encryptMetadata);

    // Permissions should default to all enabled
    EXPECT_EQ(PermissionFlags::all(), settings.permissions);
}

TEST(EncryptionSettingsTest, WithAes256) {
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner456";
    settings.permissions = PermissionFlags::readOnly();
    settings.encryptMetadata = false;

    EXPECT_EQ(EncryptionAlgorithm::AES_256, settings.algorithm);
    EXPECT_TRUE(settings.isEncrypted());
    EXPECT_TRUE(settings.hasUserPassword());
    EXPECT_TRUE(settings.hasOwnerPassword());
    EXPECT_EQ("user123", settings.userPassword);
    EXPECT_EQ("owner456", settings.ownerPassword);
    EXPECT_EQ(PermissionFlags::readOnly(), settings.permissions);
    EXPECT_FALSE(settings.encryptMetadata);
}

// ============================================================================
// EncryptionInfo Tests
// ============================================================================

TEST(EncryptionInfoTest, DefaultConstruction) {
    EncryptionInfo info;

    EXPECT_FALSE(info.isEncrypted);
    EXPECT_EQ(0, info.version);
    EXPECT_EQ(0, info.revision);
    EXPECT_EQ(0, info.keyLength);
    EXPECT_TRUE(info.method.empty());
    EXPECT_TRUE(info.metadataEncrypted);
    EXPECT_EQ(0, info.authLevel);

    // Permissions should default to all enabled
    EXPECT_EQ(PermissionFlags::all(), info.permissions);
}
