// -*- coding: utf-8 -*-
//
// Accessibility-tree bypass of the copy/extract permission.
//
// The previous phase gated WordExporter, ExcelExporter and ImageExporter on
// Document::copyAllowed(), closing the "copy-denied content walks out through
// the exporters" hole. A live bypass survived it: PdfPageAccessible (and the
// whole-canvas PdfCanvasAccessible text interface) are QAccessibleTextInterface
// implementations backed by the file-local helper pageText() in
// src/ui/PdfCanvasAccessible.cpp, which called Page::extractPlainText() with NO
// permission check. Any process walking the UIA / QAccessible tree —
// pywinauto, WinAppDriver, FlaUI, i.e. exactly this project's own UI test
// stack — could read the complete text of a copy-restricted encrypted PDF.
//
// The gate permits the read when EITHER ISO 32000-1 bit is set:
//   bit 10 extract-for-accessibility (extractForAccessibilityAllowed()) — the
//          very purpose the a11y tree serves — OR
//   bit  5 general copy/extract (copyAllowed()) — content that may be copied
//          wholesale may certainly be read by assistive technology.
// Only a document denying BOTH is silenced (empty QString, never a throw).
//
// The controls below are what make this a real discrimination rather than an
// a11y off-switch: an implementation that simply returned "" would fail
// UnencryptedDocumentStillReachableThroughA11yTree,
// CopyDeniedButAccessibilityAllowedStillYieldsText and
// EncryptedR6CopyDeniedStillReachable. Blanking the a11y tree for every
// protected document would be its own defect — an accessibility regression.
//
// Revision note: QPDF refuses to CLEAR bit 10 for R > 3 (PDF 2.0 deprecates
// it), asserted by PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3.
// So the denied-both fixture must be written at R3 (RC4-128); at R4/R6 the
// accessibility bit is always on and the gate always permits.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "ui/PdfCanvas.h"
#include "ui/PdfCanvasAccessible.h"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::encryption;
using namespace pdfe::ui;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Distinctive per-page text baked into tests/fixtures/test_3page.pdf.
const char* kPage1Marker = "Page 1 - Title Page";

/// Encrypt test_3page.pdf with the given algorithm/permissions. Empty user
/// password so the reopen observes the RESTRICTED (user-level) view — opening
/// with the owner password would grant everything and make the gate vacuous.
std::filesystem::path writeEncrypted(const std::filesystem::path& dir,
                                     const std::string& name,
                                     EncryptionAlgorithm algorithm,
                                     const PermissionFlags& perms) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto src = Document::open(ctx, fixturesDir() / "test_3page.pdf");
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

} // namespace

class AccessibilityPermissionGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        QAccessible::installFactory(pdfCanvasAccessibleFactory);
        ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_3page.pdf"))
            << "test_3page.pdf fixture is required";
        ASSERT_TRUE(tmp_.isValid());
    }

    std::filesystem::path dir() const {
        return std::filesystem::path(tmp_.path().toStdString());
    }

    // Everything the accessibility tree can hand out for `doc`, as one string:
    // the whole-canvas text interface plus every per-page child's text. If the
    // gate holds, ALL of it must be empty.
    struct A11yReadout {
        QString canvasText;
        QString childText;   // all children concatenated
        int canvasCharCount = 0;
        int childCount = 0;
    };

    A11yReadout readThroughA11yTree(Document* doc) {
        A11yReadout out;
        canvas_ = std::make_unique<PdfCanvas>();
        canvas_->setDocument(doc, nullptr, nullptr);

        QAccessibleInterface* iface =
            QAccessible::queryAccessibleInterface(canvas_.get());
        EXPECT_NE(iface, nullptr);
        if (!iface) return out;

        out.childCount = iface->childCount();

        if (QAccessibleTextInterface* ti = iface->textInterface()) {
            out.canvasCharCount = ti->characterCount();
            out.canvasText = ti->text(0, ti->characterCount());
        }

        for (int i = 0; i < out.childCount; ++i) {
            QAccessibleInterface* child = iface->child(i);
            if (!child) continue;
            // Both the Value/Description text and the text interface are
            // reachable by a UIA client; check both doors.
            out.childText += child->text(QAccessible::Value);
            out.childText += child->text(QAccessible::Description);
            if (QAccessibleTextInterface* ct = child->textInterface()) {
                out.childText += ct->text(0, ct->characterCount());
            }
        }
        return out;
    }

    void TearDown() override {
        if (canvas_) {
            canvas_->setDocument(nullptr, nullptr, nullptr);
            canvas_.reset();
        }
    }

    QTemporaryDir tmp_;
    std::unique_ptr<PdfCanvas> canvas_;
};

// ===========================================================================
// The bypass itself: copy DENIED and accessibility DENIED -> nothing readable.
// ===========================================================================

TEST_F(AccessibilityPermissionGateTest, CopyAndAccessibilityDeniedYieldsNoText) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;
    perms.accessibility = false;
    // R3 (RC4-128) is the newest revision whose writer honours bit 10.
    auto path = writeEncrypted(dir(), "deny_both_r3.pdf",
                               EncryptionAlgorithm::RC4_128, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed())
        << "fixture must actually deny copy";
    ASSERT_FALSE(doc->extractForAccessibilityAllowed())
        << "fixture must actually deny accessibility extraction (R3)";
    // The document really does still have text to leak — otherwise an empty
    // readout below would prove nothing.
    ASSERT_FALSE(doc->page(0)->extractPlainText().empty());

    const A11yReadout r = readThroughA11yTree(doc.get());

    EXPECT_FALSE(r.canvasText.contains(kPage1Marker))
        << "copy-denied + a11y-denied page text leaked through the whole-canvas "
           "QAccessibleTextInterface";
    // Not a character-count of 0: allPagesText() joins the per-page results
    // with '\n', so N silenced pages still yield N-1 separators. Those are
    // STRUCTURE, not content — the oracle is that not one non-whitespace
    // character of the document survives.
    EXPECT_EQ(r.canvasText.simplified().trimmed().length(), 0)
        << "no document text may be exposed at all, got: "
        << r.canvasText.left(200).toStdString();
    EXPECT_LT(r.canvasCharCount, doc->pageCount())
        << "the whole-canvas text interface must hold nothing but the "
           "page-join separators";

    EXPECT_FALSE(r.childText.contains(kPage1Marker))
        << "copy-denied + a11y-denied page text leaked through a per-page "
           "PdfPageAccessible child (the UIA-tree bypass)";
    EXPECT_EQ(r.childText.simplified().trimmed().length(), 0)
        << "no page text may be exposed at all, got: "
        << r.childText.left(200).toStdString();

    // The tree STRUCTURE is unchanged — a screen reader still sees the pages,
    // it just cannot read restricted content out of them. Silencing the text
    // must not silently remove the document from the a11y tree.
    EXPECT_EQ(r.childCount, doc->pageCount());
}

// ===========================================================================
// Control 1 — an UNENCRYPTED document must still be fully readable.
// Without this, "return empty always" passes the test above.
// ===========================================================================

TEST_F(AccessibilityPermissionGateTest, UnencryptedDocumentStillReachableThroughA11yTree) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->copyAllowed());
    ASSERT_TRUE(doc->extractForAccessibilityAllowed());

    const A11yReadout r = readThroughA11yTree(doc.get());

    EXPECT_GT(r.canvasCharCount, 0)
        << "the gate must not break accessibility for unrestricted documents";
    EXPECT_TRUE(r.canvasText.contains(kPage1Marker))
        << "unencrypted page text must remain reachable through the a11y tree";
    EXPECT_TRUE(r.childText.contains(kPage1Marker))
        << "unencrypted page text must remain reachable through the per-page "
           "children";
    EXPECT_EQ(r.childCount, doc->pageCount());
}

// ===========================================================================
// Control 2 — copy DENIED but accessibility ALLOWED must still yield text.
// This is the spec-correct behaviour for assistive technology (ISO 32000-1
// bit 10) and the case that makes the gate a discrimination between the two
// bits rather than a blanket off-switch keyed on copyAllowed() alone.
// ===========================================================================

TEST_F(AccessibilityPermissionGateTest, CopyDeniedButAccessibilityAllowedStillYieldsText) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;               // general extraction denied ...
    perms.accessibility = true;       // ... but assistive tech explicitly allowed
    auto path = writeEncrypted(dir(), "deny_copy_allow_a11y_r3.pdf",
                               EncryptionAlgorithm::RC4_128, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed())
        << "fixture must actually deny copy — otherwise this control is vacuous";
    ASSERT_TRUE(doc->extractForAccessibilityAllowed());

    const A11yReadout r = readThroughA11yTree(doc.get());

    EXPECT_GT(r.canvasCharCount, 0)
        << "bit 10 grants assistive-technology extraction even when bit 5 "
           "denies general copying";
    EXPECT_TRUE(r.canvasText.contains(kPage1Marker));
    EXPECT_TRUE(r.childText.contains(kPage1Marker));
}

// ===========================================================================
// Control 3 — the modern-encryption reality check. QPDF cannot clear bit 10
// above R3, so an R6 copy-denied document still grants accessibility and its
// text must remain readable by assistive technology. This is what proves the
// gate does not over-restrict the encrypted documents users actually produce.
// If QPDF ever gains the ability to clear bit 10 at R6 this test goes RED
// alongside PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3.
// ===========================================================================

TEST_F(AccessibilityPermissionGateTest, EncryptedR6CopyDeniedStillReachable) {
    PermissionFlags perms = PermissionFlags::all();
    perms.copy = false;
    perms.accessibility = false;   // requested, but not expressible at R6
    auto path = writeEncrypted(dir(), "deny_both_r6.pdf",
                               EncryptionAlgorithm::AES_256, perms);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->copyAllowed());
    ASSERT_TRUE(doc->extractForAccessibilityAllowed())
        << "R6 is expected to force bit 10 on (PDF 2.0 deprecates clearing it)";

    const A11yReadout r = readThroughA11yTree(doc.get());

    EXPECT_GT(r.canvasCharCount, 0)
        << "an R4/R6 copy-denied document still grants accessibility "
           "extraction; silencing it would be an a11y regression";
    EXPECT_TRUE(r.canvasText.contains(kPage1Marker));
}

// ===========================================================================
// A null document must not crash the gate, and must expose nothing.
// ===========================================================================

TEST_F(AccessibilityPermissionGateTest, NullDocumentExposesNothingAndDoesNotThrow) {
    A11yReadout r;
    EXPECT_NO_THROW({ r = readThroughA11yTree(nullptr); });
    EXPECT_EQ(r.childCount, 0);
    EXPECT_TRUE(r.canvasText.isEmpty());
    EXPECT_TRUE(r.childText.isEmpty());
}
