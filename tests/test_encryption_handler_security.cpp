// -*- coding: utf-8 -*-
//
// Encryption / permission-handler security area.
//
// This file is the test pointer named by the 'Encryption and permission
// handler' area in docs/security-policy.md, and it closes the six
// encryption-handler gap ids that meta-audit follow-up phase F3 owns:
// PDF-24, PDF-25, PDF-26, PDF-27, PDF-29 and PDF-30.
//
// The two AREA tests are:
//   * CryptoDowngrade  — re-saving an AES-256 R6 document must not emit a
//                        weaker handler (no silent drop to RC4 or AES-128).
//   * EmptyUserPassword — a document whose owner password is set and whose
//                        user password is empty opens with full read access in
//                        most viewers; the product must DETECT and SURFACE it.
//
// Every refusal/report assertion in this file is paired with a CONTROL
// assertion that must still SUCCEED, so none of these tests can be made to
// pass by breaking the feature for everybody.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::encryption;

namespace {

std::filesystem::path corpusFixture(const char* rel) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "flaw_corpus"
           / "encryption" / rel;
}

// Raw bytes of a produced file. The handler-identity assertions below read the
// /Encrypt dictionary out of the file itself rather than trusting the same
// reader that produced the in-memory EncryptionInfo — a downgrade that fooled
// the reader would otherwise fool the oracle too.
std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool listHas(const std::vector<std::string>& v, const char* name) {
    return std::find(v.begin(), v.end(), std::string(name)) != v.end();
}

class EncryptionHandlerSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<EncryptionManager>(ctx_);

        plainFixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(plainFixture_))
            << "test_single.pdf fixture missing at " << plainFixture_.string();

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_enc_handler_security" / info->name();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tempDir_, ec);
    }

    std::filesystem::path out(const std::string& stem) const {
        return tempDir_ / (stem + ".pdf");
    }

    std::unique_ptr<pdfe::core::Document> openPlain() {
        return pdfe::core::Document::open(ctx_, plainFixture_);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<EncryptionManager> manager_;
    std::filesystem::path plainFixture_;
    std::filesystem::path tempDir_;
};

// ===========================================================================
// AREA TEST 1 — crypto downgrade
// ===========================================================================

// An AES-256 (V5/R6) document that is opened and re-saved through the
// re-encryption pipeline (MuPDF decrypt-to-memory -> QPDF re-encrypt) must come
// back out at V5/R6. A silent downgrade to RC4 or AES-128 would leave the user
// believing the document is still AES-256: the UI label, the file extension and
// the password all stay the same.
TEST_F(EncryptionHandlerSecurityTest, Aes256R6ResaveDoesNotEmitAWeakerHandler) {
    EncryptionSettings aes256;
    aes256.algorithm = EncryptionAlgorithm::AES_256;
    aes256.userPassword = "u_r6";
    aes256.ownerPassword = "o_r6";
    aes256.permissions = PermissionFlags::all();

    // Round 1 — produce the AES-256 R6 fixture.
    auto first = out("r6_first");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), first, aes256);
    }
    ASSERT_TRUE(std::filesystem::exists(first));

    auto firstInfo = [&] {
        auto d = pdfe::core::Document::open(ctx_, first, "u_r6");
        EXPECT_NE(d, nullptr);
        return manager_->getEncryptionInfo(d.get());
    }();
    ASSERT_EQ(firstInfo.version, 5);
    ASSERT_EQ(firstInfo.revision, 6);

    // Round 2 — the re-save under test.
    auto second = out("r6_resaved");
    {
        auto doc = pdfe::core::Document::open(ctx_, first, "o_r6");
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), second, aes256);
    }
    ASSERT_TRUE(std::filesystem::exists(second));

    auto reopened = pdfe::core::Document::open(ctx_, second, "u_r6");
    ASSERT_NE(reopened, nullptr);
    auto info = manager_->getEncryptionInfo(reopened.get());

    EXPECT_TRUE(info.isEncrypted);
    EXPECT_EQ(info.version, 5)  << "re-save must stay at /V 5";
    EXPECT_EQ(info.revision, 6) << "re-save must stay at /R 6";
    EXPECT_EQ(info.keyLength, 256);
    EXPECT_EQ(info.method, std::string("AES-256"));

    // The explicit no-downgrade assertions the area asks for.
    EXPECT_NE(info.method, std::string("RC4"))
        << "AES-256 must never be re-saved as RC4";
    EXPECT_NE(info.method, std::string("AES-128"))
        << "AES-256 must never be re-saved as AES-128";
    // MuPDF collapses AESV2 and AESV3 to the single label "AES"
    // (pdf-crypt.c crypt_method), so the AES-128-vs-AES-256 discrimination is
    // carried by keyLength above and by the byte oracle below.
    EXPECT_EQ(info.streamCryptMethod, std::string("AES"))
        << "stream crypt filter must remain an AES filter, got '"
        << info.streamCryptMethod << "'";

    // Byte-level oracle, independent of the reader.
    const std::string bytes = readAllBytes(second);
    EXPECT_TRUE(contains(bytes, "/R 6")) << "written file must declare /R 6";
    EXPECT_TRUE(contains(bytes, "/AESV3"))
        << "written file must declare the AESV3 crypt filter";
    EXPECT_FALSE(contains(bytes, "/AESV2"))
        << "written file must not contain an AES-128 crypt filter";

    // CONTROL — the oracle DISCRIMINATES. Ask for a genuinely weaker handler
    // and the same assertions must report it, otherwise the test above would
    // pass no matter what was written.
    EncryptionSettings weak;
    weak.algorithm = EncryptionAlgorithm::AES_128;
    weak.userPassword = "u_r4";
    weak.ownerPassword = "o_r4";
    weak.permissions = PermissionFlags::all();

    auto weakPath = out("r4_control");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), weakPath, weak);
    }
    auto weakDoc = pdfe::core::Document::open(ctx_, weakPath, "u_r4");
    ASSERT_NE(weakDoc, nullptr) << "CONTROL: an AES-128 save must still work";
    auto weakInfo = manager_->getEncryptionInfo(weakDoc.get());
    EXPECT_EQ(weakInfo.method, std::string("AES-128"))
        << "CONTROL: a real AES-128 file must be reported as AES-128";
    EXPECT_TRUE(contains(readAllBytes(weakPath), "/AESV2"))
        << "CONTROL: the /AESV2 byte oracle must fire on a real AES-128 file";
}

// ===========================================================================
// AREA TEST 2 — empty user password
// ===========================================================================

// The classic "protected" PDF: an owner password restricts permissions, the
// user password is empty, and the document therefore opens with full read
// access and no prompt at all. Permission bits are advisory in most viewers, so
// the practical protection is close to none.
//
// WHAT THE PRODUCT DOES (recorded in
// docs/audit/meta-audit-2026-07/followup/phase-3-dispositions.md): it DETECTS
// the condition (EncryptionInfo::emptyUserPassword) and SURFACES it as an
// explicit warning in the Document Permissions dialog
// (MainWindow::onDocumentPermissions). It does NOT refuse to open such a
// document and does not refuse to produce one — both would be wrong; empty
// user passwords are legal, common and often intended.
TEST_F(EncryptionHandlerSecurityTest, EmptyUserPasswordIsDetectedAndSurfaced) {
    EncryptionSettings ownerOnly;
    ownerOnly.algorithm = EncryptionAlgorithm::AES_256;
    ownerOnly.userPassword = "";              // <-- the condition under test
    ownerOnly.ownerPassword = "owner_only_pw";
    ownerOnly.permissions = PermissionFlags::readOnly();

    auto path = out("empty_user_pw");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), path, ownerOnly);
    }
    ASSERT_TRUE(std::filesystem::exists(path));

    // It really does open with no password — that is the bypass.
    auto opened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(opened, nullptr)
        << "an empty-user-password document opens without any password";

    auto info = manager_->getEncryptionInfo(opened.get());
    EXPECT_TRUE(info.isEncrypted);
    EXPECT_TRUE(info.emptyUserPassword)
        << "the empty-user-password condition must be DETECTED and reported "
           "on EncryptionInfo, not left for the caller to infer";
    EXPECT_FALSE(manager_->needsPassword(opened.get()))
        << "consistency: the document does not need a password";
    // The restriction the owner asked for is present but, per the above, only
    // advisory once the content is readable without a password.
    EXPECT_FALSE(info.permissions.copy)
        << "the owner's copy restriction is still recorded in /P";

    // CONTROL — a document WITH a user password must NOT be flagged, otherwise
    // the flag is a constant and detects nothing.
    EncryptionSettings withUserPw = ownerOnly;
    withUserPw.userPassword = "real_user_pw";
    auto guarded = out("real_user_pw");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), guarded, withUserPw);
    }
    auto guardedDoc = pdfe::core::Document::open(ctx_, guarded, "real_user_pw");
    ASSERT_NE(guardedDoc, nullptr)
        << "CONTROL: a user-password document must still open with its password";
    auto guardedInfo = manager_->getEncryptionInfo(guardedDoc.get());
    EXPECT_TRUE(guardedInfo.isEncrypted);
    EXPECT_FALSE(guardedInfo.emptyUserPassword)
        << "CONTROL: a document with a real user password must NOT be flagged";
    EXPECT_TRUE(manager_->needsPassword(guardedDoc.get()))
        << "CONTROL: that document does need a password";
}

// ===========================================================================
// PDF-25 — /StmF /Identity: encrypted wrapper, plaintext streams
// ===========================================================================

// A document whose stream crypt filter is /Identity carries a real /Encrypt
// dictionary — so isEncrypted() is true and every "is this protected?" check
// says yes — while every stream in the body is stored in plaintext. The product
// must REPORT the condition rather than silently presenting the document as
// encrypted.
//
// Fixture: tests/fixtures/flaw_corpus/encryption/identity_stmf.pdf, built by
// tests/fixtures/gen_identity_stmf.py (/V 4 /R 4, /CF /StdCF /CFM /AESV2,
// /StmF /Identity, /StrF /StdCF, empty user password, owner password "owner").
TEST_F(EncryptionHandlerSecurityTest, IdentityStreamFilterReportedAsUnprotected) {
    auto path = corpusFixture("identity_stmf.pdf");
    ASSERT_TRUE(std::filesystem::exists(path))
        << "fixture missing: " << path.string()
        << " (regenerate with tests/fixtures/gen_identity_stmf.py)";

    auto doc = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr)
        << "the /StmF /Identity fixture must open (its user password is empty)";

    auto info = manager_->getEncryptionInfo(doc.get());

    // The trap: this IS an encrypted document by every conventional check.
    EXPECT_TRUE(info.isEncrypted)
        << "the fixture carries a real /Encrypt dictionary";
    EXPECT_TRUE(manager_->isEncrypted(doc.get()));

    // And yet its streams are plaintext. That is what must be reported.
    EXPECT_EQ(info.streamCryptMethod, std::string("None"))
        << "an /Identity stream filter must be reported as method 'None', got '"
        << info.streamCryptMethod << "'";
    EXPECT_TRUE(info.streamsUnprotected)
        << "the product must REPORT that stream content is unprotected rather "
           "than silently treating the document as encrypted";
    // The string filter is a real AESV2 filter, so the report is specific to
    // streams and not a blanket "nothing is encrypted".
    EXPECT_FALSE(info.stringsUnprotected)
        << "the fixture's /StrF is a real /StdCF AESV2 filter";

    // Ground truth, independent of the reader: the sentinel really is sitting
    // in the file in plaintext.
    const std::string bytes = readAllBytes(path);
    EXPECT_TRUE(contains(bytes, "/StmF/Identity"))
        << "fixture must declare /StmF /Identity";
    EXPECT_TRUE(contains(bytes, "IDENTITY-STMF-PLAINTEXT-SENTINEL"))
        << "the page content stream really is stored in plaintext";

    // CONTROL — a genuinely stream-encrypted document of the SAME handler
    // family (/V 4 /R 4 AESV2) must NOT be flagged.
    auto controlPath = corpusFixture("aes128.pdf");
    ASSERT_TRUE(std::filesystem::exists(controlPath))
        << "control fixture missing: " << controlPath.string();
    auto controlDoc = pdfe::core::Document::open(ctx_, controlPath, "user");
    ASSERT_NE(controlDoc, nullptr)
        << "CONTROL: the real AES-128 fixture must still open";
    auto controlInfo = manager_->getEncryptionInfo(controlDoc.get());
    EXPECT_TRUE(controlInfo.isEncrypted);
    EXPECT_EQ(controlInfo.streamCryptMethod, std::string("AES"))
        << "CONTROL: a real AESV2 stream filter must be reported as an AES "
           "filter, not as 'None'";
    EXPECT_FALSE(controlInfo.streamsUnprotected)
        << "CONTROL: a genuinely encrypted document must NOT be flagged as "
           "having unprotected streams";
}

// ===========================================================================
// PDF-26 — AES-256 R5 must not be mislabelled
// ===========================================================================

// /V 5 /R 5 is the deprecated Adobe Extension Level 3 handler. It is AES-256.
// getMethodString() previously matched only (version==5 && revision==6), so
// every R5 document fell through to the `version >= 2` branch and was reported
// as "RC4" — a mislabel in the direction that understates the protection.
//
// Fixture: tests/fixtures/flaw_corpus/encryption/aes256_r5.pdf (user password
// "user"), the itext7 upstream R5 file already in the corpus.
TEST_F(EncryptionHandlerSecurityTest, Aes256R5IsReportedAsAes256NotRc4) {
    auto path = corpusFixture("aes256_r5.pdf");
    ASSERT_TRUE(std::filesystem::exists(path))
        << "fixture missing: " << path.string();

    auto doc = pdfe::core::Document::open(ctx_, path, "user");
    ASSERT_NE(doc, nullptr) << "the R5 fixture must open with password 'user'";

    auto info = manager_->getEncryptionInfo(doc.get());
    ASSERT_TRUE(info.isEncrypted);
    ASSERT_EQ(info.version, 5)  << "fixture must be /V 5";
    ASSERT_EQ(info.revision, 5) << "fixture must be /R 5 (not R6)";

    EXPECT_EQ(info.method, std::string("AES-256"))
        << "V5/R5 is AES-256; got '" << info.method << "'";
    EXPECT_NE(info.method, std::string("RC4"))
        << "V5/R5 must never be reported as RC4";
    EXPECT_EQ(info.streamCryptMethod, std::string("AES"))
        << "the R5 fixture's crypt filter really is AESV3 (MuPDF labels both "
           "AES variants \"AES\")";

    // CONTROL 1 — a real RC4 document must still be reported as RC4, so the fix
    // is not "call everything AES-256".
    auto rc4Path = corpusFixture("standard40.pdf");
    ASSERT_TRUE(std::filesystem::exists(rc4Path));
    auto rc4Doc = pdfe::core::Document::open(ctx_, rc4Path, "user");
    ASSERT_NE(rc4Doc, nullptr) << "CONTROL: the RC4-40 fixture must still open";
    auto rc4Info = manager_->getEncryptionInfo(rc4Doc.get());
    EXPECT_EQ(rc4Info.method, std::string("RC4"))
        << "CONTROL: a genuine /V 1 /R 2 document must still be labelled RC4";

    // CONTROL 2 — R6 keeps its existing label.
    EncryptionSettings r6;
    r6.algorithm = EncryptionAlgorithm::AES_256;
    r6.userPassword = "u";
    r6.ownerPassword = "o";
    auto r6Path = out("r6_label_control");
    {
        auto plain = openPlain();
        ASSERT_NE(plain, nullptr);
        manager_->saveEncrypted(plain.get(), r6Path, r6);
    }
    auto r6Doc = pdfe::core::Document::open(ctx_, r6Path, "u");
    ASSERT_NE(r6Doc, nullptr);
    auto r6Info = manager_->getEncryptionInfo(r6Doc.get());
    EXPECT_EQ(r6Info.revision, 6);
    EXPECT_EQ(r6Info.method, std::string("AES-256"))
        << "CONTROL: R6 must still be AES-256";
}

// ===========================================================================
// PDF-27 — the lossy R2 permission mapping must be REPORTED
// ===========================================================================

// RC4-40 (/V 1 /R 2) defines only four permission bits: print, modify, copy and
// annotate. QPDF's setR2EncryptionParametersInsecure takes exactly those four.
// A caller who restricts accessibility, formFill, assemble or printHighQuality
// at R2 therefore gets a file that permits all of them — silently, before this
// change.
TEST_F(EncryptionHandlerSecurityTest, Rc4_40LossyPermissionMappingIsReported) {
    EncryptionSettings r2;
    r2.algorithm = EncryptionAlgorithm::RC4_40;
    r2.userPassword = "u_r2";
    r2.ownerPassword = "o_r2";
    PermissionFlags pf;
    pf.print = true;
    pf.modify = false;
    pf.copy = false;
    pf.annotate = false;
    // The four R2 cannot express — all of them RESTRICTED by the caller:
    pf.accessibility = false;
    pf.formFill = false;
    pf.assemble = false;
    pf.printHighQuality = false;
    r2.permissions = pf;

    // The preview path reports the same thing without writing anything.
    auto preview = EncryptionManager::previewPermissionMapping(r2);
    EXPECT_TRUE(preview.lossyPermissionMapping);

    EncryptionSaveReport report;
    auto path = out("r2_lossy");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), path, r2, &report);
    }
    ASSERT_TRUE(std::filesystem::exists(path));

    EXPECT_TRUE(report.lossyPermissionMapping)
        << "an R2 save that drops requested restrictions must say so";
    EXPECT_EQ(report.handlerWritten, std::string("RC4-40 (V1/R2)"));
    EXPECT_EQ(report.unrepresentablePermissions.size(), 4u);
    for (const char* name : {"accessibility", "formFill", "assemble",
                             "printHighQuality"}) {
        EXPECT_TRUE(listHas(report.unrepresentablePermissions, name))
            << name << " must be listed as unrepresentable at R2";
        EXPECT_TRUE(listHas(report.droppedRestrictions, name))
            << name << " was requested restricted and must be listed as dropped";
    }
    EXPECT_FALSE(report.summary.empty())
        << "a lossy mapping must carry a human-readable summary";

    // The report is truthful about the file: the dropped restrictions really
    // are not honoured by the written document.
    auto reopened = pdfe::core::Document::open(ctx_, path, "u_r2");
    ASSERT_NE(reopened, nullptr);
    auto restored = manager_->getPermissions(reopened.get());
    EXPECT_TRUE(restored.accessibility)
        << "R2 cannot deny accessibility — this is the loss being reported";
    EXPECT_FALSE(restored.copy)
        << "the bits R2 CAN express must still be honoured";

    // CONTROL 1 — a handler that can express all eight bits reports no loss.
    EncryptionSettings r6 = r2;
    r6.algorithm = EncryptionAlgorithm::AES_256;
    EncryptionSaveReport r6Report;
    auto r6Path = out("r6_lossless");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), r6Path, r6, &r6Report);
    }
    ASSERT_TRUE(std::filesystem::exists(r6Path))
        << "CONTROL: the AES-256 save must still produce a file";
    EXPECT_FALSE(r6Report.lossyPermissionMapping)
        << "CONTROL: AES-256 expresses all eight bits — nothing is lost";
    EXPECT_TRUE(r6Report.droppedRestrictions.empty());
    EXPECT_TRUE(r6Report.unrepresentablePermissions.empty());
    EXPECT_EQ(r6Report.handlerWritten, std::string("AES-256 (V5/R6)"));

    // CONTROL 2 — R2 with the four unexpressible bits left ALLOWED loses
    // nothing, so the flag tracks the request and is not simply "algorithm ==
    // RC4_40".
    EncryptionSettings r2Allowed = r2;
    r2Allowed.permissions.accessibility = true;
    r2Allowed.permissions.formFill = true;
    r2Allowed.permissions.assemble = true;
    r2Allowed.permissions.printHighQuality = true;
    auto allowedPreview = EncryptionManager::previewPermissionMapping(r2Allowed);
    EXPECT_FALSE(allowedPreview.lossyPermissionMapping)
        << "CONTROL: nothing was restricted, so nothing was dropped";
    EXPECT_EQ(allowedPreview.unrepresentablePermissions.size(), 4u)
        << "CONTROL: the bits are still unrepresentable, just not restricted";
}

// ===========================================================================
// PDF-29 — cross-engine password preparation (SASLprep / stringprep)
// ===========================================================================

// PDFE writes encryption with QPDF and reads it with MuPDF. ISO 32000-2 requires
// R6 passwords to be SASLprep-prepared and truncated to 127 UTF-8 bytes. If the
// two engines disagree on that preparation, a non-ASCII password set by PDFE
// cannot re-open the file PDFE just wrote.
TEST_F(EncryptionHandlerSecurityTest, NonAsciiPasswordRoundTripsAcrossEngines) {
    // UTF-8 literal, written as explicit bytes so the test does not depend on
    // the compiler's source/execution charset (MSVC without /utf-8 would
    // otherwise re-encode it).
    const std::string userPw =
        "p\xC3\xA4ssw\xC3\xB6rd\xC2\xB7\xCE\xA9";     // "pässwörd·Ω"
    const std::string ownerPw =
        "\xC3\x96wner\xE2\x82\xAC\xE4\xB8\xAD";        // "Öwner€中"

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = userPw;
    settings.ownerPassword = ownerPw;
    settings.permissions = PermissionFlags::all();

    auto path = out("non_ascii_pw");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), path, settings);
    }
    ASSERT_TRUE(std::filesystem::exists(path));

    // The document must NOT open without the password...
    std::unique_ptr<pdfe::core::Document> noPw;
    bool threw = false;
    try {
        noPw = pdfe::core::Document::open(ctx_, path);
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw || noPw == nullptr)
        << "a user-password document must not open without the password";
    noPw.reset();

    // ...and must open with the exact non-ASCII user password QPDF was given.
    std::unique_ptr<pdfe::core::Document> reopened;
    ASSERT_NO_THROW({ reopened = pdfe::core::Document::open(ctx_, path, userPw); })
        << "MuPDF must accept the non-ASCII user password QPDF wrote";
    ASSERT_NE(reopened, nullptr)
        << "cross-engine divergence: QPDF wrote a password MuPDF cannot verify";
    // NOTE on the return convention: EncryptionManager::authenticatePassword
    // forwards MuPDF's RAW fz_authenticate_password bitfield (0=fail, 2=user,
    // 4=owner, 6=both) -- it is Document::authLevel() that normalises to
    // 1=user / 2=owner. Assert on both, using each one's own convention.
    EXPECT_NE(manager_->authenticatePassword(reopened.get(), userPw), 0)
        << "the non-ASCII user password must authenticate";
    EXPECT_EQ(reopened->authLevel() & 1, 1)
        << "the non-ASCII user password must authenticate at USER level";
    EXPECT_FALSE(reopened->isOwnerAuthenticated())
        << "the user password must not confer owner authority";

    // The owner password must round-trip too, and confer owner authority.
    std::unique_ptr<pdfe::core::Document> asOwner;
    ASSERT_NO_THROW({ asOwner = pdfe::core::Document::open(ctx_, path, ownerPw); });
    ASSERT_NE(asOwner, nullptr);
    EXPECT_TRUE(asOwner->isOwnerAuthenticated())
        << "the non-ASCII owner password must confer owner authority";

    // CONTROL — the same pipeline with an ASCII password, proving the failure
    // mode above would be attributable to the non-ASCII preparation and not to
    // a broken save path.
    EncryptionSettings ascii = settings;
    ascii.userPassword = "plain_user";
    ascii.ownerPassword = "plain_owner";
    auto asciiPath = out("ascii_pw");
    {
        auto doc = openPlain();
        ASSERT_NE(doc, nullptr);
        manager_->saveEncrypted(doc.get(), asciiPath, ascii);
    }
    auto asciiDoc = pdfe::core::Document::open(ctx_, asciiPath, "plain_user");
    ASSERT_NE(asciiDoc, nullptr)
        << "CONTROL: an ASCII password must still round-trip";
    EXPECT_TRUE(manager_->isEncrypted(asciiDoc.get()));

    // CONTROL — a WRONG non-ASCII password must still be refused, so the test
    // above cannot pass because the reader accepts anything.
    std::unique_ptr<pdfe::core::Document> wrong;
    bool wrongRejected = false;
    try {
        wrong = pdfe::core::Document::open(ctx_, path, "p\xC3\xA4ssw\xC3\xB6rd");
    } catch (const std::exception&) {
        wrongRejected = true;
    }
    EXPECT_TRUE(wrongRejected || wrong == nullptr)
        << "CONTROL: a near-miss non-ASCII password must still be refused";
}

// ===========================================================================
// PDF-24 — public-key / certificate encryption
// ===========================================================================

// PDFE does not implement the public-key security handler (/Filter
// /Adobe.PubSec). That is a FEATURE gap, recorded as a wont-fix in
// docs/audit/meta-audit-2026-07/followup/phase-3-dispositions.md. What must be
// guaranteed today is that such a document is refused cleanly and is never
// presented as a readable document PDFE cannot actually decrypt.
TEST_F(EncryptionHandlerSecurityTest, CertificateEncryptedDocumentIsRefusedCleanly) {
    auto path = corpusFixture("aes256_certificate.pdf");
    ASSERT_TRUE(std::filesystem::exists(path))
        << "fixture missing: " << path.string();

    // The fixture really is a public-key handler document.
    const std::string bytes = readAllBytes(path);
    ASSERT_TRUE(contains(bytes, "/Adobe.PubSec"))
        << "fixture must use the public-key security handler";

    std::unique_ptr<pdfe::core::Document> doc;
    bool threw = false;
    try {
        doc = pdfe::core::Document::open(ctx_, path);
    } catch (const std::exception&) {
        threw = true;   // a clean, typed refusal is the accepted outcome
    }
    EXPECT_TRUE(threw || doc == nullptr)
        << "a certificate-encrypted document must be refused cleanly, not "
           "returned as a usable document PDFE cannot decrypt";
    doc.reset();

    // A password is not a way in either — there is no password to supply.
    std::unique_ptr<pdfe::core::Document> withPw;
    bool threwWithPw = false;
    try {
        withPw = pdfe::core::Document::open(ctx_, path, "anything");
    } catch (const std::exception&) {
        threwWithPw = true;
    }
    EXPECT_TRUE(threwWithPw || withPw == nullptr)
        << "supplying a password must not bypass the unsupported handler";
    withPw.reset();

    // CONTROL — the refusal is specific to the public-key handler. A standard
    // security handler document of the SAME cipher strength must still open and
    // be fully usable, so this test cannot pass by refusing encrypted PDFs.
    auto controlPath = corpusFixture("aes256_r5.pdf");
    ASSERT_TRUE(std::filesystem::exists(controlPath));
    auto control = pdfe::core::Document::open(ctx_, controlPath, "user");
    ASSERT_NE(control, nullptr)
        << "CONTROL: a standard-handler AES-256 document must still open";
    EXPECT_EQ(control->pageCount(), 1)
        << "CONTROL: and must still be readable";
    EXPECT_TRUE(manager_->isEncrypted(control.get()));
}

// ===========================================================================
// PDF-30 — intermediate plaintext buffer handling on the failure path
// ===========================================================================

// EncryptionManager::saveEncrypted decrypts the whole document into a heap
// buffer before QPDF re-encrypts it. PDF-30 asked whether that plaintext is
// wiped. The RAII IntermediateWiper (src/encryption/EncryptionManager.cpp)
// zeroes it with SecureZeroMemory on EVERY scope exit, and the pre-guard catch
// block wipes the partial buffer before rethrowing.
//
// Proving the wipe happened requires a memory-inspection harness this project
// does not have (see the disposition file). What IS assertable — and is the
// observable half of the same concern — is that the failure path leaves no
// plaintext ON DISK: the intermediate never touches the filesystem, and a
// failed encryption writes no output file.
TEST_F(EncryptionHandlerSecurityTest, FailedEncryptionLeavesNoPlaintextOnDisk) {
    auto badPath = tempDir_ / "no_such_directory" / "out.pdf";
    ASSERT_FALSE(std::filesystem::exists(badPath.parent_path()));

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "u";
    settings.ownerPassword = "o";

    auto doc = openPlain();
    ASSERT_NE(doc, nullptr);
    EXPECT_THROW(manager_->saveEncrypted(doc.get(), badPath, settings),
                 std::exception)
        << "an unwritable destination must fail cleanly";

    EXPECT_FALSE(std::filesystem::exists(badPath))
        << "no output file may be left behind by a failed encryption";

    // No plaintext intermediate was spilled next to the destination either --
    // the pipeline keeps it in memory by construction.
    std::size_t strays = 0;
    for (const auto& entry : std::filesystem::directory_iterator(tempDir_)) {
        ++strays;
        ADD_FAILURE() << "unexpected file left in the output directory: "
                      << entry.path().string();
    }
    EXPECT_EQ(strays, 0u);

    // CONTROL — the same settings against a writable destination must succeed
    // and produce a real encrypted file, so the test above cannot pass because
    // encryption is broken outright.
    auto goodPath = out("wiper_control");
    ASSERT_NO_THROW(manager_->saveEncrypted(doc.get(), goodPath, settings));
    ASSERT_TRUE(std::filesystem::exists(goodPath));
    EXPECT_GT(std::filesystem::file_size(goodPath), 0u);
    auto reopened = pdfe::core::Document::open(ctx_, goodPath, "u");
    ASSERT_NE(reopened, nullptr)
        << "CONTROL: the successful path must still produce a usable file";
    EXPECT_TRUE(manager_->isEncrypted(reopened.get()));
}

}  // namespace
