/**
 * @file test_signature_data.cpp
 * @brief Unit tests for SignatureData types.
 */

#include <gtest/gtest.h>
#include "signatures/SignatureData.h"

using namespace pdfe::signatures;

// ============================================================================
// SignerIdentity Tests
// ============================================================================

TEST(SignerIdentityTest, DefaultConstruction_IsEmpty) {
    SignerIdentity identity;

    EXPECT_TRUE(identity.commonName.empty());
    EXPECT_TRUE(identity.organization.empty());
    EXPECT_TRUE(identity.orgUnit.empty());
    EXPECT_TRUE(identity.email.empty());
    EXPECT_TRUE(identity.country.empty());
    EXPECT_TRUE(identity.formattedName.empty());
    EXPECT_TRUE(identity.isEmpty());
}

TEST(SignerIdentityTest, WithCommonName_NotEmpty) {
    SignerIdentity identity;
    identity.commonName = "Test Signer";

    EXPECT_FALSE(identity.isEmpty());
}

TEST(SignerIdentityTest, WithOrganization_NotEmpty) {
    SignerIdentity identity;
    identity.organization = "Test Corp";

    EXPECT_FALSE(identity.isEmpty());
}

TEST(SignerIdentityTest, WithEmail_NotEmpty) {
    SignerIdentity identity;
    identity.email = "test@example.com";

    EXPECT_FALSE(identity.isEmpty());
}

TEST(SignerIdentityTest, Equality) {
    SignerIdentity a;
    a.commonName = "Alice";
    a.organization = "ACME";

    SignerIdentity b;
    b.commonName = "Alice";
    b.organization = "ACME";

    EXPECT_EQ(a, b);

    b.commonName = "Bob";
    EXPECT_NE(a, b);
}

// ============================================================================
// SignatureError Tests
// ============================================================================

TEST(SignatureErrorTest, ErrorToString) {
    EXPECT_EQ("OK", signatureErrorToString(SignatureError::Okay));
    EXPECT_EQ("No signatures", signatureErrorToString(SignatureError::NoSignatures));
    EXPECT_EQ("No certificate", signatureErrorToString(SignatureError::NoCertificate));
    EXPECT_EQ("Digest failure", signatureErrorToString(SignatureError::DigestFailure));
    EXPECT_EQ("Self-signed certificate", signatureErrorToString(SignatureError::SelfSigned));
    EXPECT_EQ("Self-signed certificate in chain", signatureErrorToString(SignatureError::SelfSignedInChain));
    EXPECT_EQ("Certificate not trusted", signatureErrorToString(SignatureError::NotTrusted));
    EXPECT_EQ("Not signed", signatureErrorToString(SignatureError::NotSigned));
    EXPECT_EQ("Unknown error", signatureErrorToString(SignatureError::Unknown));
}

TEST(SignatureErrorTest, FromMuPdf_ValidCodes) {
    EXPECT_EQ(SignatureError::Okay, signatureErrorFromMuPdf(0));
    EXPECT_EQ(SignatureError::NoSignatures, signatureErrorFromMuPdf(1));
    EXPECT_EQ(SignatureError::NoCertificate, signatureErrorFromMuPdf(2));
    EXPECT_EQ(SignatureError::DigestFailure, signatureErrorFromMuPdf(3));
    EXPECT_EQ(SignatureError::SelfSigned, signatureErrorFromMuPdf(4));
    EXPECT_EQ(SignatureError::SelfSignedInChain, signatureErrorFromMuPdf(5));
    EXPECT_EQ(SignatureError::NotTrusted, signatureErrorFromMuPdf(6));
    EXPECT_EQ(SignatureError::NotSigned, signatureErrorFromMuPdf(7));
}

TEST(SignatureErrorTest, FromMuPdf_InvalidCode) {
    EXPECT_EQ(SignatureError::Unknown, signatureErrorFromMuPdf(99));
    EXPECT_EQ(SignatureError::Unknown, signatureErrorFromMuPdf(-1));
}

// ============================================================================
// SignatureVerifyResult Tests
// ============================================================================

TEST(SignatureVerifyResultTest, DefaultConstruction) {
    SignatureVerifyResult result;

    EXPECT_EQ(SignatureError::Unknown, result.digestStatus);
    EXPECT_EQ(SignatureError::Unknown, result.certificateStatus);
    EXPECT_FALSE(result.digestValid);
    EXPECT_FALSE(result.certificateValid);
    EXPECT_FALSE(result.modifiedAfterSigning);
    EXPECT_TRUE(result.digestMessage.empty());
    EXPECT_TRUE(result.certificateMessage.empty());
    EXPECT_FALSE(result.isFullyValid());
}

TEST(SignatureVerifyResultTest, FullyValid) {
    SignatureVerifyResult result;
    result.digestStatus = SignatureError::Okay;
    result.certificateStatus = SignatureError::Okay;
    result.digestValid = true;
    result.certificateValid = true;
    result.modifiedAfterSigning = false;

    EXPECT_TRUE(result.isFullyValid());
}

TEST(SignatureVerifyResultTest, DigestInvalid_NotFullyValid) {
    SignatureVerifyResult result;
    result.digestValid = false;
    result.certificateValid = true;
    result.modifiedAfterSigning = false;

    EXPECT_FALSE(result.isFullyValid());
}

TEST(SignatureVerifyResultTest, CertificateInvalid_NotFullyValid) {
    SignatureVerifyResult result;
    result.digestValid = true;
    result.certificateValid = false;
    result.modifiedAfterSigning = false;

    EXPECT_FALSE(result.isFullyValid());
}

TEST(SignatureVerifyResultTest, ModifiedAfterSigning_NotFullyValid) {
    SignatureVerifyResult result;
    result.digestValid = true;
    result.certificateValid = true;
    result.modifiedAfterSigning = true;

    EXPECT_FALSE(result.isFullyValid());
}

// ============================================================================
// SignatureData Tests
// ============================================================================

TEST(SignatureDataTest, DefaultConstruction) {
    SignatureData data;

    EXPECT_EQ(-1, data.pageIndex);
    EXPECT_EQ(-1, data.widgetIndex);
    EXPECT_TRUE(data.fieldName.empty());
    EXPECT_TRUE(data.rect.isNull());
    EXPECT_FALSE(data.isSigned);
    EXPECT_TRUE(data.signer.isEmpty());
    EXPECT_TRUE(data.reason.empty());
    EXPECT_TRUE(data.location.empty());
    EXPECT_TRUE(data.signingTimeStr.empty());
    EXPECT_EQ(0, data.signingTime);
}

TEST(SignatureDataTest, WithValues) {
    SignatureData data;
    data.pageIndex = 0;
    data.widgetIndex = 2;
    data.fieldName = "Signature1";
    data.rect = QRectF(100, 200, 150, 50);
    data.isSigned = true;
    data.signer.commonName = "John Doe";
    data.reason = "Approval";
    data.location = "New York";
    data.signingTime = 1700000000;

    EXPECT_EQ(0, data.pageIndex);
    EXPECT_EQ(2, data.widgetIndex);
    EXPECT_EQ("Signature1", data.fieldName);
    EXPECT_DOUBLE_EQ(100.0, data.rect.x());
    EXPECT_DOUBLE_EQ(200.0, data.rect.y());
    EXPECT_TRUE(data.isSigned);
    EXPECT_EQ("John Doe", data.signer.commonName);
    EXPECT_EQ("Approval", data.reason);
    EXPECT_EQ("New York", data.location);
    EXPECT_EQ(1700000000, data.signingTime);
}

// ============================================================================
// SigningOptions Tests
// ============================================================================

TEST(SigningOptionsTest, DefaultConstruction) {
    SigningOptions opts;

    EXPECT_TRUE(opts.pfxPath.empty());
    EXPECT_TRUE(opts.pfxPassword.empty());
    EXPECT_TRUE(opts.reason.empty());
    EXPECT_TRUE(opts.location.empty());
    EXPECT_EQ(0x3F, opts.appearanceFlags); // PDF_SIGNATURE_DEFAULT_APPEARANCE
}

TEST(SigningOptionsTest, WithValues) {
    SigningOptions opts;
    opts.pfxPath = "/path/to/cert.pfx";
    opts.pfxPassword = "secret";
    opts.reason = "Signed for testing";
    opts.location = "Test Lab";
    opts.appearanceFlags = 0x0F;

    EXPECT_EQ("/path/to/cert.pfx", opts.pfxPath);
    EXPECT_EQ("secret", opts.pfxPassword);
    EXPECT_EQ("Signed for testing", opts.reason);
    EXPECT_EQ("Test Lab", opts.location);
    EXPECT_EQ(0x0F, opts.appearanceFlags);
}

// ============================================================================
// SignatureSummary Tests
// ============================================================================

TEST(SignatureSummaryTest, DefaultConstruction) {
    SignatureSummary summary;

    EXPECT_EQ(0, summary.totalCount);
    EXPECT_EQ(0, summary.signedCount);
    EXPECT_EQ(0, summary.unsignedCount);
    EXPECT_EQ(0, summary.validCount);
    EXPECT_EQ(0, summary.invalidCount);
    EXPECT_TRUE(summary.signatures.empty());
}

TEST(SignatureSummaryTest, WithSignatures) {
    SignatureSummary summary;
    summary.totalCount = 3;
    summary.signedCount = 2;
    summary.unsignedCount = 1;
    summary.validCount = 1;
    summary.invalidCount = 1;

    SignatureData sig1;
    sig1.fieldName = "Sig1";
    sig1.isSigned = true;

    SignatureData sig2;
    sig2.fieldName = "Sig2";
    sig2.isSigned = true;

    SignatureData sig3;
    sig3.fieldName = "Sig3";
    sig3.isSigned = false;

    summary.signatures = {sig1, sig2, sig3};

    EXPECT_EQ(3, summary.totalCount);
    EXPECT_EQ(2, summary.signedCount);
    EXPECT_EQ(1, summary.unsignedCount);
    EXPECT_EQ(3u, summary.signatures.size());
    EXPECT_EQ("Sig1", summary.signatures[0].fieldName);
    EXPECT_TRUE(summary.signatures[0].isSigned);
    EXPECT_FALSE(summary.signatures[2].isSigned);
}
