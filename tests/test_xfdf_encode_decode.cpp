// PHASE3_XFDF_ENCODE_DECODE: symbol-level XFDF round-trip tests.
//
// test_form_submit.cpp asserts XFDF output via substring search —
// that's a Pattern-D weak assertion (any matching substring elsewhere
// in the document, even inside a comment, would pass). This file
// adds element-level checks: the export must produce valid XML, the
// expected field elements must be present at the expected nesting,
// and re-import must produce byte-identical field values.
//
// Includes the AC-required "at least one Unicode field name/value
// case" — Arabic / CJK / emoji round-trips through XFDF without
// mojibake.

#include <gtest/gtest.h>

#include "forms/FormManager.h"
#include "forms/FormDataIO.h"
#include "forms/FormFieldData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <QXmlStreamReader>
#include <QString>
#include <QSet>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::forms::FormDataIO;
using pdfe::forms::FormManager;
using pdfe::forms::FormFieldType;

namespace {

class XfdfEncodeDecodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<FormManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_xfdf" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    int firstTextFieldIndex() {
        auto page = document_->page(0);
        auto fields = manager_->getFormFields(page.get());
        for (const auto& f : fields) {
            if (f.type == FormFieldType::Text && !f.isReadOnly) {
                return f.widgetIndex;
            }
        }
        return -1;
    }

    // Parse an XFDF string and return the set of (field name, value)
    // pairs found at any nesting depth. Used to assert structural
    // contracts beyond substring matching.
    QSet<QPair<QString, QString>> parseFieldValues(const std::string& xfdf) {
        QSet<QPair<QString, QString>> out;
        QXmlStreamReader reader(QString::fromStdString(xfdf));
        QString currentName;
        bool inField = false;
        bool inValue = false;
        while (!reader.atEnd()) {
            auto t = reader.readNext();
            if (t == QXmlStreamReader::StartElement) {
                if (reader.name() == QLatin1String("field")) {
                    inField = true;
                    currentName = reader.attributes()
                        .value("name").toString();
                } else if (reader.name() == QLatin1String("value")) {
                    inValue = true;
                }
            } else if (t == QXmlStreamReader::Characters) {
                if (inField && inValue) {
                    out.insert({currentName, reader.text().toString()});
                }
            } else if (t == QXmlStreamReader::EndElement) {
                if (reader.name() == QLatin1String("value")) {
                    inValue = false;
                } else if (reader.name() == QLatin1String("field")) {
                    inField = false;
                    currentName.clear();
                }
            }
        }
        return out;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<FormManager> manager_;
};

// ─── Well-formed XML ─────────────────────────────────────────────────

TEST_F(XfdfEncodeDecodeTest, ExportProducesParseableXml) {
    int idx = firstTextFieldIndex();
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(document_->page(0).get(), idx, "value_xml_test");

    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());
    QXmlStreamReader reader(QString::fromStdString(xfdf));
    while (!reader.atEnd()) reader.readNext();
    EXPECT_FALSE(reader.hasError())
        << "XFDF export must be well-formed XML; parse error: "
        << reader.errorString().toStdString();
}

// ─── Field present as <field> element ────────────────────────────────

TEST_F(XfdfEncodeDecodeTest, SetValueAppearsAsFieldElement) {
    int idx = firstTextFieldIndex();
    ASSERT_GE(idx, 0);
    auto fields = manager_->getFormFields(document_->page(0).get());
    QString fieldName =
        QString::fromStdString(fields[idx].fieldName);

    manager_->setFieldValue(document_->page(0).get(), idx,
                            "structured_check");
    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());

    auto pairs = parseFieldValues(xfdf);
    bool found = false;
    for (const auto& p : pairs) {
        if (p.first == fieldName && p.second == "structured_check") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "Field value must be present as <value> child of "
           "<field name=\"" << fieldName.toStdString() << "\"> "
           "(structural check, not substring)";
}

// ─── Round-trip equality ─────────────────────────────────────────────

TEST_F(XfdfEncodeDecodeTest, ExportImportPreservesValueAtSymbolLevel) {
    int idx = firstTextFieldIndex();
    ASSERT_GE(idx, 0);

    const std::string original = "lvl_symbol_test";
    manager_->setFieldValue(document_->page(0).get(), idx, original);

    FormDataIO io(ctx_);
    std::string exported = io.exportToXfdf(document_.get(), manager_.get());

    // Clear the field, save+reopen, then import.
    manager_->setFieldValue(document_->page(0).get(), idx, "");
    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(manager_->getFormField(reopened->page(0).get(), idx).value, "");

    io.importFromXfdf(reopened.get(), manager_.get(), exported);
    auto reField = manager_->getFormField(reopened->page(0).get(), idx);
    EXPECT_EQ(reField.value, original)
        << "XFDF import must restore exact byte-equal value";
}

// ─── Unicode round-trip ─────────────────────────────────────────────

TEST_F(XfdfEncodeDecodeTest, UnicodeFieldValueRoundTripsWithoutMojibake) {
    int idx = firstTextFieldIndex();
    ASSERT_GE(idx, 0);

    // Mixed-script value: Arabic, CJK (Chinese + Japanese), emoji.
    // The exact codepoints lock the contract — anything dropped by
    // a UTF-8 ↔ UTF-16 transit failure shows up as inequality.
    const std::string unicode_value =
        "مرحبا 你好 こんにちは 👋";
    manager_->setFieldValue(document_->page(0).get(), idx, unicode_value);

    FormDataIO io(ctx_);
    std::string exported = io.exportToXfdf(document_.get(), manager_.get());

    // Confirm the export at least contains the codepoints (a sanity
    // check independent of round-trip below).
    EXPECT_NE(exported.find("مرحبا"), std::string::npos)
        << "Arabic must survive XFDF export";
    EXPECT_NE(exported.find("你好"), std::string::npos)
        << "CJK must survive XFDF export";
    EXPECT_NE(exported.find("👋"), std::string::npos)
        << "Emoji must survive XFDF export";

    // Round-trip through clear -> import -> read.
    manager_->setFieldValue(document_->page(0).get(), idx, "");
    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    io.importFromXfdf(reopened.get(), manager_.get(), exported);
    auto reField = manager_->getFormField(reopened->page(0).get(), idx);
    EXPECT_EQ(reField.value, unicode_value)
        << "Unicode field value must survive XFDF export+import "
           "without mojibake";
}

// ─── Empty-value handling ────────────────────────────────────────────

TEST_F(XfdfEncodeDecodeTest, EmptyExportImportIsSafe) {
    // Don't mutate any field. Export should still produce parseable
    // XML, and re-importing it should not corrupt any in-memory state.
    FormDataIO io(ctx_);
    std::string xfdf = io.exportToXfdf(document_.get(), manager_.get());
    EXPECT_FALSE(xfdf.empty())
        << "Empty form export must still produce a valid XFDF document, "
           "not an empty string";
    QXmlStreamReader r(QString::fromStdString(xfdf));
    while (!r.atEnd()) r.readNext();
    EXPECT_FALSE(r.hasError())
        << "Empty-form XFDF must still be well-formed XML";

    // Re-import is a no-op on an unchanged form.
    EXPECT_NO_THROW({
        io.importFromXfdf(document_.get(), manager_.get(), xfdf);
    });
    // Confirm fields are still queryable post-import.
    auto fields = manager_->getFormFields(document_->page(0).get());
    EXPECT_GE(fields.size(), 1u)
        << "Form fields must remain queryable after no-op XFDF import";
}

} // namespace
