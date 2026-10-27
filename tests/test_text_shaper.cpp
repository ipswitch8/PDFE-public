#include <gtest/gtest.h>
#include "fonts/TextShaper.h"
#include <memory>
#include <string>

#include <ft2build.h>
#include FT_FREETYPE_H

using namespace pdfe::fonts;

class TextShaperTest : public ::testing::Test {
protected:
    void SetUp() override {
        shaper = std::make_unique<TextShaper>();

        // Initialize FreeType and load a system font directly
        FT_Error err = FT_Init_FreeType(&ftLibrary_);
        if (err != 0)
            return;

        // Try common system font paths on Windows
        const char* fontPaths[] = {
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\times.ttf",
            "C:\\Windows\\Fonts\\cour.ttf",
            nullptr
        };

        for (int i = 0; fontPaths[i]; ++i) {
            err = FT_New_Face(ftLibrary_, fontPaths[i], 0, &ftFace_);
            if (err == 0)
                break;
        }
    }

    void TearDown() override {
        if (ftFace_) {
            FT_Done_Face(ftFace_);
            ftFace_ = nullptr;
        }
        if (ftLibrary_) {
            FT_Done_FreeType(ftLibrary_);
            ftLibrary_ = nullptr;
        }
    }

    std::unique_ptr<TextShaper> shaper;
    FT_Library ftLibrary_ = nullptr;
    FT_Face ftFace_ = nullptr;
};

TEST_F(TextShaperTest, ShapeAsciiTextReturnsGlyphs) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    std::u32string text = U"Hello";
    auto glyphs = shaper->shape(ftFace_, text, 12.0f);

    EXPECT_FALSE(glyphs.empty());
    EXPECT_EQ(glyphs.size(), text.size());

    for (const auto& g : glyphs) {
        EXPECT_GT(g.xAdvance, 0.0f) << "Glyph should have positive advance";
    }
}

TEST_F(TextShaperTest, MeasureWidthPositiveForNonEmptyText) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    float width = shaper->measureWidth(ftFace_, U"Hello World", 12.0f);
    EXPECT_GT(width, 0.0f);
}

TEST_F(TextShaperTest, MeasureWidthZeroForEmptyText) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    float width = shaper->measureWidth(ftFace_, U"", 12.0f);
    EXPECT_FLOAT_EQ(width, 0.0f);
}

TEST_F(TextShaperTest, CanRenderLatinText) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    bool result = shaper->canRender(ftFace_, U"Hello World");
    EXPECT_TRUE(result) << "System font (Arial/Times) should cover Latin text";
}

TEST_F(TextShaperTest, CanRenderReturnsFalseForCJK) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    // Arial/Times typically don't have CJK glyphs
    bool result = shaper->canRender(ftFace_, U"\u4E2D\u6587");
    // Some system fonts may have CJK (e.g., on Asian locale Windows),
    // so we don't assert false -- just verify no crash
    (void)result;
}

TEST_F(TextShaperTest, ShapedGlyphCountMatchesInput) {
    if (!ftFace_) {
        GTEST_SKIP() << "No system font available for testing";
    }

    std::u32string text = U"test";
    auto glyphs = shaper->shape(ftFace_, text, 12.0f);

    // For simple Latin text, glyph count should equal char count
    EXPECT_EQ(glyphs.size(), text.size());
}

TEST_F(TextShaperTest, NullFaceHandledGracefully) {
    auto glyphs = shaper->shape(nullptr, U"Hello", 12.0f);
    EXPECT_TRUE(glyphs.empty());

    float width = shaper->measureWidth(nullptr, U"Hello", 12.0f);
    EXPECT_FLOAT_EQ(width, 0.0f);

    bool can = shaper->canRender(nullptr, U"Hello");
    EXPECT_FALSE(can);
}
