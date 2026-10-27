// -*- coding: utf-8 -*-
//
// Phase-16 (follow-up phase-69) — src/metadata, the last genuinely unaudited
// subsystem.
//
// Three defects, all of which share a shape: the SAFE-LOOKING path was the
// dangerous one.
//
//   1. The dialog's EDITABLE fields are QLineEdit and were never exposed to
//      markup injection, because QLineEdit has no rich-text mode. The
//      READ-ONLY labels are QLabel, whose default Qt::AutoText runs
//      Qt::mightBeRichText() and switches to HTML rendering if the string looks
//      like markup. Every value in those three labels comes from the opened
//      document's /Info dictionary, which is attacker-controlled.
//
//   2. MetadataManager::readField() used a fixed char[256] and IGNORED
//      fz_lookup_metadata's return value, which is the size actually needed.
//      Over-long values were silently truncated -- and, because the cut is at a
//      byte offset, a multi-byte UTF-8 sequence could be severed mid-codepoint,
//      leaving a string that is not valid UTF-8.
//
//   3. That truncation was not merely a display bug. EditMetadataCommand
//      snapshots the pre-edit state THROUGH THE SAME READER, so an over-long
//      value was truncated on the way in and the truncated version was written
//      back by undo. A read operation destroyed data.
//
//   4. writeField() did not call markRequiresFullRewrite(), while
//      writeMetadata() and clearMetadata() both did. Overwriting an /Info entry
//      orphans the old value rather than erasing it, so an incremental save
//      leaves the previous author/title/keywords recoverable in the file. The
//      single-field path had the WEAKER guarantee of the two; the asymmetry was
//      the defect.
//
// WHY THE UI CASE IS TWO-SIDED. Asserting `textFormat() == Qt::PlainText` alone
// would pass against a build where the string was never markup to begin with,
// proving nothing. The control arm renders the SAME string in a default
// (Qt::AutoText) QLabel and asserts the two disagree about layout -- which
// establishes that the payload really is interpretable markup, so the subject
// arm's plain rendering is a fact about the fix rather than about the payload.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "metadata/MetadataCommands.h"
#include "metadata/MetadataManager.h"
#include "ui/MetadataDialog.h"

#include <QApplication>
#include <QLabel>
#include <QPoint>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace fs = std::filesystem;
using namespace pdfe::core;
using namespace pdfe::metadata;
using namespace pdfe::ui;

namespace {

void ensureQApplication()
{
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = {nullptr};
        static QApplication app(argc, argv);
    }
}

/// Markup that Qt::mightBeRichText() recognises. Deliberately also the shape
/// that makes this a security issue rather than a cosmetic one: a QLabel
/// rendering rich text resolves <img src=...>.
const char* kMarkupPayload =
    "<b>BOLD</b><img src=\"x\">Acrobat &amp; Co";

fs::path fixture(const char* name)
{
    return fs::path(TEST_FIXTURES_DIR) / name;
}

/// A value that is longer than the old 256-byte buffer AND places a multi-byte
/// UTF-8 codepoint so that a byte-wise cut at 255 lands INSIDE it.
std::string overlongUtf8Value()
{
    // U+00E9 (e-acute) is two bytes: 0xC3 0xA9.
    std::string s(254, 'A');
    s += "\xC3\xA9";  // bytes 255-256 -- a 255-byte cut severs this
    s += std::string(120, 'B');
    return s;
}

bool isValidUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;                       // stray continuation / invalid
        if (i + extra >= s.size() && extra > 0) return false;  // truncated
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string readAllBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class MetadataHardeningTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();
        doc_ = Document::open(context_, fixture("test_single.pdf").string());
        manager_ = std::make_unique<MetadataManager>(context_);
        ASSERT_NE(doc_, nullptr);
    }

    void TearDown() override
    {
        doc_.reset();
        manager_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::shared_ptr<Document> doc_;
    std::unique_ptr<MetadataManager> manager_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. The read-only labels must render attacker /Info values as literal text.
TEST(MetadataDialogInjection, ReadOnlyLabelsRenderDocumentMarkupAsLiteralText)
{
    ensureQApplication();

    // CONTROL ARM FIRST: prove the payload really is interpretable markup in
    // this Qt build. Without this the subject arm proves nothing.
    QLabel autoTextControl;
    ASSERT_EQ(autoTextControl.textFormat(), Qt::AutoText)
        << "QLabel's default is no longer AutoText; this test's premise is stale";
    autoTextControl.setText(QString::fromUtf8(kMarkupPayload));

    QLabel plainControl;
    plainControl.setTextFormat(Qt::PlainText);
    plainControl.setText(QString::fromUtf8(kMarkupPayload));

    ASSERT_NE(autoTextControl.sizeHint().width(), plainControl.sizeHint().width())
        << "the payload renders identically under AutoText and PlainText, so "
           "this test cannot distinguish a fixed dialog from a broken one. "
           "Choose a payload Qt::mightBeRichText() actually recognises.";

    // SUBJECT ARM: the real dialog, fed the payload through /Info values.
    MetadataDialog dialog;
    DocumentMetadata md;
    md.producer = kMarkupPayload;
    md.creationDate = kMarkupPayload;
    md.modDate = kMarkupPayload;
    dialog.setMetadata(md);

    for (const char* objectName : {"metadataProducerLabel",
                                   "metadataCreationDateLabel",
                                   "metadataModDateLabel"}) {
        auto* label = dialog.findChild<QLabel*>(objectName);
        ASSERT_NE(label, nullptr) << objectName << " not found in the dialog";

        EXPECT_EQ(label->textFormat(), Qt::PlainText)
            << objectName << " renders attacker-controlled /Info with "
            << "Qt::AutoText. A crafted /Producer is then interpreted as HTML, "
            << "and a QLabel rendering rich text resolves <img src=...>.";

        // Behavioural, not just a property read: it must lay out the same as
        // the explicitly-plain control, and differently from the AutoText one.
        EXPECT_EQ(label->sizeHint().width(), plainControl.sizeHint().width())
            << objectName << " lays the payload out differently from a "
               "PlainText label, i.e. it is interpreting the markup.";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. The reader must not silently truncate, and must never cut a codepoint.
TEST_F(MetadataHardeningTest, ReaderReturnsOverlongValuesWithoutSeveringUtf8)
{
    const std::string value = overlongUtf8Value();
    ASSERT_GT(value.size(), 256u) << "the fixture value must exceed the old buffer";
    ASSERT_TRUE(isValidUtf8(value)) << "the fixture value is itself malformed";

    manager_->writeField(doc_.get(), "info:Title", value);
    const std::string round = manager_->readField(doc_.get(), "info:Title");

    EXPECT_EQ(round.size(), value.size())
        << "readField truncated a " << value.size() << "-byte value to "
        << round.size() << " bytes. fz_lookup_metadata returns the size needed; "
           "ignoring it and keeping a fixed 256-byte buffer loses data silently.";
    EXPECT_EQ(round, value);
    EXPECT_TRUE(isValidUtf8(round))
        << "the value came back as invalid UTF-8 -- a byte-wise cut severed a "
           "multi-byte codepoint. This is why a length check alone is not "
           "enough of an oracle.";
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Undo must not write back a truncated snapshot. This is the consequence
//    that makes (2) a data-loss bug rather than a display bug.
TEST_F(MetadataHardeningTest, UndoRestoresAnOverlongValueByteForByte)
{
    const std::string original = overlongUtf8Value();
    manager_->writeField(doc_.get(), "info:Title", original);
    ASSERT_EQ(manager_->readField(doc_.get(), "info:Title"), original);

    DocumentMetadata edited = manager_->readMetadata(doc_.get());
    edited.title = "replaced";

    EditMetadataCommand cmd(manager_.get(), doc_.get(), edited);
    cmd.execute();
    ASSERT_EQ(manager_->readField(doc_.get(), "info:Title"), "replaced");

    cmd.undo();
    const std::string restored = manager_->readField(doc_.get(), "info:Title");

    EXPECT_EQ(restored, original)
        << "undo restored " << restored.size() << " bytes of an original "
        << original.size() << ". EditMetadataCommand snapshots through "
           "readField, so a truncating reader makes undo LOSSY: the edit is "
           "reverted to a value the user never had.";
    EXPECT_TRUE(isValidUtf8(restored));
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. A single-field edit must force a full rewrite, like the batch paths.
TEST_F(MetadataHardeningTest, WriteFieldForcesFullRewriteLikeWriteMetadata)
{
    ASSERT_FALSE(doc_->sessionRequiresFullRewrite())
        << "the fixture already requires a full rewrite, so this test cannot "
           "attribute the flag to writeField()";

    manager_->writeField(doc_.get(), "info:Author", "single field edit");

    EXPECT_TRUE(doc_->sessionRequiresFullRewrite())
        << "writeField() did not mark the session for full rewrite, while "
           "writeMetadata() and clearMetadata() both do. Overwriting /Info "
           "orphans the previous value; saved incrementally it stays in the "
           "file and is recoverable.";
}

// ─────────────────────────────────────────────────────────────────────────────
// The end-to-end statement of (4): the superseded value must not survive in
// the saved bytes. This is the one that would still fail if
// markRequiresFullRewrite() stopped implying a real rewrite.
TEST_F(MetadataHardeningTest, ASavedSingleFieldEditLeavesNoRecoverablePriorValue)
{
    const std::string secret = "CONFIDENTIAL-PRIOR-AUTHOR-c0ffee";
    const std::string replacement = "public author";

    manager_->writeField(doc_.get(), "info:Author", secret);

    const fs::path out =
        fs::temp_directory_path() / "pdfe_p69_metadata_seeded.pdf";
    fs::remove(out);
    doc_->saveAs(out.string());
    doc_.reset();

    auto seeded = Document::open(context_, out.string());
    ASSERT_NE(seeded, nullptr);
    ASSERT_EQ(manager_->readField(seeded.get(), "info:Author"), secret)
        << "the seeded document does not carry the value under test";

    manager_->writeField(seeded.get(), "info:Author", replacement);

    // save(), NOT saveAs() -- and this distinction is the whole test.
    //
    // saveAs() to a fresh path is a full write by construction, so the old
    // value could not survive it however writeField() behaved. A first version
    // of this case used saveAs() and NO MUTATION COULD REDDEN IT: removing
    // markRequiresFullRewrite() left it green, which made it decoration rather
    // than a guard. The orphaned-value hazard lives on the INCREMENTAL path,
    // where a save appends the new /Info and leaves the superseded object in
    // place, so that is the path this must exercise.
    seeded->save(out.string());
    seeded.reset();

    const std::string bytes = readAllBytes(out);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(bytes.find(secret), std::string::npos)
        << "the superseded /Author is still present verbatim in the saved "
           "file. Replacing an /Info entry only orphans the old object; "
           "without a full rewrite an incremental save appends the new value "
           "and leaves the old one intact and recoverable.";

    // Two-sided: the replacement must actually be there, or an empty/broken
    // save would satisfy the assertion above for the wrong reason.
    EXPECT_NE(bytes.find(replacement), std::string::npos)
        << "the replacement value is absent from the saved file, so the "
           "absence of the old one proves nothing";

    fs::remove(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// SIBLINGS OF THE SAME DEFECT, found by the phase-69 security-audit gate.
//
// The metadata dialog was not the only place a QLabel is fed an
// attacker-controlled document string at Qt::AutoText. Two more were reachable
// by opening a malicious PDF and clicking:
//
//   * StickyNotePopup's title bar, filled by openForAnnotation() with the
//     annotation's /T author -- which AnnotationManager reads straight off the
//     annotation dictionary, deliberately bypassing pdf_annot_has_author's
//     allow-list so it works for every annotation type.
//   * FormFieldPropertiesPanel's name label, filled by setFormField() with the
//     field's /T name from the PDF.
//
// This is the third phase running in which a gate has found the SAME defect one
// component over from the one being repaired. The pattern is now guarded by
// name in all three places rather than left to the next sweep.

#include "annotations/AnnotationManager.h"
#include "forms/FormManager.h"
#include "ui/FormFieldPropertiesPanel.h"
#include "ui/StickyNotePopup.h"

namespace {

/// Shared oracle, SELF-CALIBRATING against the widget under test.
///
/// An earlier version compared the subject's sizeHint() against a freshly
/// constructed PlainText QLabel. That over-constrained: these widgets carry
/// their own contentsMargins and wordWrap, so their layout differs from a bare
/// control for reasons that have nothing to do with text format (measured:
/// 261 vs 259 for the sticky-note title, 201 vs 259 for the word-wrapped form
/// field name). It failed on correctly-fixed widgets -- a false positive, which
/// is the worse kind of wrong for a guard.
///
/// So the comparison is now the SAME widget under both formats. That holds
/// margins, wrapping, font and style constant and isolates the one variable
/// that matters, and it still cannot pass vacuously: if the payload renders
/// identically either way it is not markup, and the case says so.
void expectRendersMarkupLiterally(QLabel* label, const char* what)
{
    ASSERT_NE(label, nullptr) << what << " not found";

    const Qt::TextFormat shipped = label->textFormat();
    const int shippedWidth = label->sizeHint().width();

    label->setTextFormat(Qt::AutoText);
    label->setText(QString::fromUtf8(kMarkupPayload));
    const int autoWidth = label->sizeHint().width();

    label->setTextFormat(Qt::PlainText);
    label->setText(QString::fromUtf8(kMarkupPayload));
    const int plainWidth = label->sizeHint().width();

    // Restore, so the widget is left as the product configured it.
    label->setTextFormat(shipped);
    label->setText(QString::fromUtf8(kMarkupPayload));

    ASSERT_NE(autoWidth, plainWidth)
        << what << ": the payload lays out identically under AutoText and "
                   "PlainText, so this case cannot distinguish a fixed widget "
                   "from a broken one. Pick a payload Qt::mightBeRichText() "
                   "actually recognises.";

    EXPECT_EQ(shipped, Qt::PlainText)
        << what << " renders an attacker-controlled document string with "
                   "Qt::AutoText; a rich-text QLabel resolves <img src=...>";
    EXPECT_EQ(shippedWidth, plainWidth)
        << what << " laid the payload out as markup, not as literal text";
}

} // namespace

TEST(MetadataDialogInjection, StickyNoteTitleRendersAnnotationAuthorAsLiteralText)
{
    ensureQApplication();

    StickyNotePopup popup;
    pdfe::annotations::AnnotationData data;
    data.author = kMarkupPayload;
    data.contents = "body";
    popup.openForAnnotation(data, QPoint(0, 0));

    auto* title = popup.findChild<QLabel*>("stickyNotePopupTitle");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QString::fromUtf8(kMarkupPayload))
        << "the title bar does not actually carry the annotation author, so "
           "this test would pass without guarding anything";
    expectRendersMarkupLiterally(title, "StickyNotePopup::titleBar_");
}

TEST(MetadataDialogInjection, FormFieldNameRendersFieldNameAsLiteralText)
{
    ensureQApplication();

    FormFieldPropertiesPanel panel;
    pdfe::forms::FormFieldData data;
    data.fieldName = kMarkupPayload;
    panel.setFormField(data);

    auto* name = panel.findChild<QLabel*>("formFieldNameLabel");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), QString::fromUtf8(kMarkupPayload))
        << "the name label does not actually carry the field name, so this "
           "test would pass without guarding anything";
    expectRendersMarkupLiterally(name, "FormFieldPropertiesPanel::nameLabel_");
}
