#include <gtest/gtest.h>
#include "forms/FormFieldType.h"
#include "forms/FormFieldData.h"

using namespace pdfe::forms;

TEST(FormFieldTypeTest, ConversionRoundTrip) {
    FormFieldType types[] = {
        FormFieldType::Text,
        FormFieldType::Checkbox,
        FormFieldType::RadioButton,
        FormFieldType::ComboBox,
        FormFieldType::ListBox,
        FormFieldType::PushButton,
        FormFieldType::Signature,
        FormFieldType::Unknown
    };

    for (auto type : types) {
        int pdfType = toPdfWidgetType(type);
        FormFieldType roundTripped = fromPdfWidgetType(pdfType);
        EXPECT_EQ(roundTripped, type) << "Round-trip failed for " << formFieldTypeName(type);
    }
}

TEST(FormFieldTypeTest, AllTypesHaveNames) {
    FormFieldType types[] = {
        FormFieldType::Text,
        FormFieldType::Checkbox,
        FormFieldType::RadioButton,
        FormFieldType::ComboBox,
        FormFieldType::ListBox,
        FormFieldType::PushButton,
        FormFieldType::Signature,
        FormFieldType::Unknown
    };

    for (auto type : types) {
        std::string name = formFieldTypeName(type);
        EXPECT_FALSE(name.empty()) << "Empty name for type " << static_cast<int>(type);
    }
}

TEST(FormFieldDataTest, DefaultConstruction) {
    FormFieldData data;
    EXPECT_EQ(data.type, FormFieldType::Unknown);
    EXPECT_TRUE(data.rect.isNull());
    EXPECT_TRUE(data.fieldName.empty());
    EXPECT_TRUE(data.value.empty());
    EXPECT_EQ(data.fieldFlags, 0);
    EXPECT_TRUE(data.options.empty());
    EXPECT_TRUE(data.exportValues.empty());
    EXPECT_TRUE(data.selectedIndices.empty());
    EXPECT_FALSE(data.isMultiSelect);
    EXPECT_FALSE(data.isReadOnly);
    EXPECT_FALSE(data.isRequired);
    EXPECT_FALSE(data.isMultiline);
    EXPECT_FALSE(data.isPassword);
    EXPECT_EQ(data.pageIndex, -1);
    EXPECT_EQ(data.widgetIndex, -1);
}
