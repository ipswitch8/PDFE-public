// -*- coding: utf-8 -*-
// Regression tests for the header/footer + watermark adjacent fixes
// (pipeline e5b9c231 Phase 2):
//   * MediaBox-origin offset (both engines placed relative to (0,0));
//   * HeaderFooter /F1 collision (engine hardcoded /F1, colliding with a page
//     body font already bound to /F1).
// Fixture: hf_adversarial.pdf -- MediaBox origin (100,100), /F1 = Courier.
#include <gtest/gtest.h>
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

using namespace pdfe::headerfooter;

namespace {

std::filesystem::path fixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "hf_adversarial.pdf";
}

// Device-space center-x of the first line containing `needle`; -1 if absent.
float centerXOf(const std::filesystem::path& pdf, const std::u32string& needle) {
    float cx = -1.0f;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                cx = (ln->bbox.x0 + ln->bbox.x1) / 2.0f;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return cx;
}

std::filesystem::path applyHF(const std::string& headerText) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    HeaderFooterEngine eng(ctx);
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::All;
    cfg.centerHeader = headerText;
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_hf_adv_out.pdf";
    eng.apply(fixture(), out, cfg);
    return out;
}

} // namespace

// Page device width is 612 (MediaBox 612 wide); its center is x=306.
static constexpr float kPageCenterX = 306.0f;

TEST(HFWatermarkAdversarial, HeaderCenteredDespiteNonZeroMediaBoxOrigin) {
    std::filesystem::path out = applyHF("HDRCENTER");
    float cx = centerXOf(out, U"HDRCENTER");
    ASSERT_GE(cx, 0.0f) << "header text not found";
    // Fix lands at ~313; the un-fixed offset is ~100 (the MediaBox origin), so
    // a 45 px window catches the regression with margin for render variance.
    EXPECT_NEAR(cx, kPageCenterX, 45.0f)
        << "header center x=" << cx << " is offset from page center "
        << kPageCenterX << " -- MediaBox origin not honoured";
}

TEST(HFWatermarkAdversarial, HeaderUsesFreshFontKeyNotCollidingF1) {
    std::filesystem::path out = applyHF("HDRFONT");

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    pdf_document* doc = nullptr;
    bool f1StillCourier = false, addedHelvetica = false;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, out.string().c_str());
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* res = pdf_dict_get(ctx, page, PDF_NAME(Resources));
        pdf_obj* fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
        int n = pdf_dict_len(ctx, fonts);
        for (int i = 0; i < n; ++i) {
            pdf_obj* key = pdf_dict_get_key(ctx, fonts, i);
            pdf_obj* val = pdf_dict_get_val(ctx, fonts, i);
            pdf_obj* base = pdf_dict_get(ctx, val, PDF_NAME(BaseFont));
            const char* keyName = pdf_to_name(ctx, key);
            const char* baseName = base ? pdf_to_name(ctx, base) : "";
            if (keyName && std::string(keyName) == "F1" &&
                baseName && std::string(baseName) == "Courier")
                f1StillCourier = true;
            if (keyName && std::string(keyName) != "F1" &&
                baseName && std::string(baseName) == "Helvetica")
                addedHelvetica = true;
        }
    }
    fz_always(ctx) { if (doc) pdf_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);

    EXPECT_TRUE(f1StillCourier)
        << "page's original /F1 Courier body font was overwritten by the H/F engine";
    EXPECT_TRUE(addedHelvetica)
        << "H/F Helvetica was not added under a fresh key -- engine collided with /F1";
}

TEST(HFWatermarkAdversarial, WatermarkCenteredDespiteNonZeroMediaBoxOrigin) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::batch::WatermarkConfig cfg;
    cfg.text = "WMCENTER";
    cfg.rotation = 0.0f;
    cfg.opacity = 1.0f;
    cfg.fontSize = 30.0f;
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_wm_adv_out.pdf";
    std::filesystem::remove(out);
    pdfe::batch::BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermark(fixture(), out, cfg));

    float cx = centerXOf(out, U"WMCENTER");
    ASSERT_GE(cx, 0.0f) << "watermark text not found";
    // Fix lands at ~318; un-fixed offset is ~100. 45 px window catches it.
    EXPECT_NEAR(cx, kPageCenterX, 45.0f)
        << "watermark center x=" << cx << " is offset from page center "
        << kPageCenterX << " -- MediaBox origin not honoured";
}
