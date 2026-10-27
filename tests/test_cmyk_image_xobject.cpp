// -*- coding: utf-8 -*-
//
// A1-22 — adversarial hostile-class 5: CMYK / Separation raster IMAGE XObject.
//
// docs/audit/meta-audit-2026-07/status-register.md row A1-22 (from
// docs/audit/phase3-adversarial-corpus.md:27) records this class as DEFERRED /
// OPEN with the evidence "all cmyk fixtures are content-stream FILL fixtures;
// grep DeviceCMYK hits only gen_cmyk_identity.py (fill operator `k`), no CMYK
// image XObject fixture". So the whole IMAGE colour path — subtractive sample
// decode, per-plate conversion, Separation tint-transform evaluation — had no
// oracle whatsoever.
//
// Fixture: tests/fixtures/cmyk_image_xobject.pdf, built by
// tests/fixtures/gen_cmyk_image_xobject.py. That generator is the AUTHORED
// GROUND TRUTH: the component literals below are copied from it, and every
// expectation here is derived arithmetically from those literals — never from
// what PDFE currently renders.
//
// The fixture stores both images with NO /Filter, so the sample bytes are the
// authored bytes verbatim; there is no encoder in the loop that could launder a
// wrong expectation.
//
// MUTATIONS KILLED (per-test detail in the comment above each TEST_F):
//   * swap or rotate any pair of the C/M/Y planes on the image decode path
//   * drop the K plate (render CMY only)
//   * treat DeviceCMYK samples as DeviceRGB / DeviceGray (component-count bug)
//   * ignore a Separation colourspace's tint transform and render the single
//     component as grey, or evaluate it with C0/C1 swapped
//   * fail to draw the image at all (blank page / white patch)

#include <gtest/gtest.h>

#include "support/RasterOracle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using pdfe::testsupport::luminance;
using pdfe::testsupport::Raster;
using pdfe::testsupport::renderPageRaster;
using pdfe::testsupport::RgbD;
using pdfe::testsupport::sampleBlockMean;

namespace {

// ---------------------------------------------------------------------------
// AUTHORED ground truth — mirrors tests/fixtures/gen_cmyk_image_xobject.py.
// ---------------------------------------------------------------------------
constexpr int kPageW = 400;
constexpr int kPageH = 200;
constexpr int kImgX = 40;      // user-space x of both images
constexpr int kImgW = 320;     // user-space width of both images
constexpr int kImgAY = 110;    // user-space bottom edge of /ImA
constexpr int kImgBY = 30;     // user-space bottom edge of /ImB
constexpr int kImgDrawH = 60;  // user-space height of both images

struct Cmyk {
    int c, m, y, k;
};

// /ImA — six authored DeviceCMYK patches, left -> right.
constexpr std::array<Cmyk, 6> kPatches{{
    {255, 0, 0, 0},      // 0 pure cyan
    {0, 255, 0, 0},      // 1 pure magenta
    {0, 0, 255, 0},      // 2 pure yellow
    {26, 102, 179, 64},  // 3 mixed CMY *with* a K plate
    {26, 102, 179, 0},   // 4 the SAME CMY, K dropped
    {0, 0, 0, 255},      // 5 pure K
}};

// /ImB — three authored Separation tints over a type-2 transform whose
// C0=[0 0 0 0], C1=[0 1 1 0], N=1. So tint t maps to CMYK(0, t, t, 0).
constexpr std::array<int, 3> kTints{{0, 128, 255}};

// Naive subtractive conversion, 255*(1-ink)*(1-k) per plate. This is the
// AUTHORED expectation; real CMYK->sRGB is colour-managed, hence a tolerance
// rather than an exact compare.
RgbD naiveCmykToRgb(const Cmyk& v) {
    const double c = v.c / 255.0, m = v.m / 255.0;
    const double y = v.y / 255.0, k = v.k / 255.0;
    return RgbD{255.0 * (1.0 - c) * (1.0 - k), 255.0 * (1.0 - m) * (1.0 - k),
                255.0 * (1.0 - y) * (1.0 - k)};
}

// Per-channel tolerance for the ABSOLUTE naive-formula comparisons.
//
// Deliberately NOT applied to the three fully-saturated primaries (patches
// 0/1/2): a 100%-ink primary is far outside sRGB, so gamut mapping legitimately
// moves it by ~100 levels, and any tolerance wide enough to admit that would
// admit a channel swap too. Those patches get an exact-ordering oracle instead
// (ChannelOrderingMatchesAuthoredPrimaries), which is invariant under any
// monotone colour transform yet still inverts under a swap.
//
// 50 is wide enough for colour management on the in-gamut mixed/spot patches
// and far narrower than the >=57-level separation the generator authored
// between patch 3's plates, so every plane permutation still goes RED.
constexpr double kTolAbs = 50.0;

std::filesystem::path fixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "cmyk_image_xobject.pdf";
}

// Device-space centre of patch `i` of `count` evenly-spaced patches in an image
// placed at user-space x = kImgX with width kImgW.
double patchCentreX(int i, int count) {
    const double pw = static_cast<double>(kImgW) / count;
    return kImgX + pw * (i + 0.5);
}

// Device space is y-down from the top of the page; the fixture's MediaBox has
// its origin at (0,0), so devY = pageHeight - userY.
double imageCentreDevY(int imgUserY) {
    return kPageH - (imgUserY + kImgDrawH / 2.0);
}

::testing::AssertionResult NearRgb(const RgbD& got, const RgbD& want, double tol,
                                   const char* what) {
    if (std::fabs(got.r - want.r) <= tol && std::fabs(got.g - want.g) <= tol &&
        std::fabs(got.b - want.b) <= tol)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << what << ": rendered rgb(" << got.r << ", " << got.g << ", " << got.b
           << ") differs from the AUTHORED naive-CMYK expectation rgb(" << want.r
           << ", " << want.g << ", " << want.b << ") by more than " << tol
           << " on some channel";
}

class CmykImageXObjectTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(fixture()))
            << "fixture missing (regenerate with: python "
               "tests/fixtures/gen_cmyk_image_xobject.py): "
            << fixture();
        std::string err;
        raster_ = renderPageRaster(fixture(), 0, 1.0f, &err);
        ASSERT_TRUE(err.empty()) << "page 0 failed to render: " << err;
        ASSERT_TRUE(raster_.valid()) << "empty raster";
    }

    // Each /ImA patch is ~53 device px wide, so a 6x6 block stays well inside.
    RgbD patch(int i) const {
        return sampleBlockMean(raster_, patchCentreX(i, int(kPatches.size())),
                               imageCentreDevY(kImgAY), 3);
    }
    RgbD tint(int i) const {
        return sampleBlockMean(raster_, patchCentreX(i, int(kTints.size())),
                               imageCentreDevY(kImgBY), 3);
    }

    Raster raster_;
};

}  // namespace

// The page renders at the authored size and both image XObjects actually
// DECODE — i.e. each authored patch is distinguishable from every other.
//
// Kills: an image-decode path that throws and leaves the region blank; an /ImB
// whose Separation colourspace fails to resolve (MuPDF then draws nothing);
// any change that collapses the six authored patches into one flat colour
// (e.g. decoding only the first sample and smearing it).
TEST_F(CmykImageXObjectTest, PageRendersAndBothImagesDecode) {
    EXPECT_EQ(raster_.w, kPageW);
    EXPECT_EQ(raster_.h, kPageH);

    std::vector<RgbD> seen;
    for (int i = 0; i < static_cast<int>(kPatches.size()); ++i)
        seen.push_back(patch(i));

    // The six authored patches are pairwise distinct by construction; require
    // every pair to differ by a clear margin in at least one channel.
    for (size_t a = 0; a < seen.size(); ++a) {
        for (size_t b = a + 1; b < seen.size(); ++b) {
            const double d = std::max({std::fabs(seen[a].r - seen[b].r),
                                       std::fabs(seen[a].g - seen[b].g),
                                       std::fabs(seen[a].b - seen[b].b)});
            EXPECT_GT(d, 20.0)
                << "authored DeviceCMYK patches " << a << " and " << b
                << " render identically — the image did not decode per-sample";
        }
    }

    // /ImB must not be blank: the authored full-tint patch is saturated red.
    const RgbD t2 = tint(2);
    EXPECT_LT(std::min({t2.r, t2.g, t2.b}), 200.0)
        << "the Separation image region is blank — /ImB did not decode";
}

// The two IN-GAMUT authored CMYK patches must land on the naive subtractive
// prediction within kTolAbs.
//
// Kills: any permutation of the C/M/Y planes (patch 3's authored plates are
// >=57 levels apart, so every swap/rotation moves at least one channel by more
// than kTolAbs); interpreting 4-component samples as 3-component DeviceRGB (the
// patch then reads misaligned bytes and lands nowhere near); dropping K
// (patch 3 then renders as patch 4, ~57 levels off on R).
TEST_F(CmykImageXObjectTest, AuthoredMixedPatchesMatchNaiveCmykRgb) {
    EXPECT_TRUE(NearRgb(patch(3), naiveCmykToRgb(kPatches[3]), kTolAbs,
                        "patch 3 (C=26 M=102 Y=179 K=64)"));
    EXPECT_TRUE(NearRgb(patch(4), naiveCmykToRgb(kPatches[4]), kTolAbs,
                        "patch 4 (C=26 M=102 Y=179 K=0)"));

    // Pure K must render essentially black (authored naive rgb(0,0,0)).
    const RgbD k = patch(5);
    EXPECT_LT(std::max({k.r, k.g, k.b}), 90.0)
        << "authored pure-K patch rgb(" << k.r << ", " << k.g << ", " << k.b
        << ") is not black — the K plate is not reaching the raster";
}

// The three fully-saturated authored primaries pin the PLANE ORDER. For a 100%
// single-ink primary the corresponding additive channel must be the minimum by
// a wide margin. That holds under any monotone colour transform, so it is a
// colour-management-proof channel-swap detector.
//
// Kills: swapping/rotating any pair of C/M/Y on the image decode path (each
// permutation moves the minimum channel to the wrong index); rendering CMYK
// samples as DeviceGray (all three channels become equal, so no channel is a
// clear minimum).
TEST_F(CmykImageXObjectTest, ChannelOrderingMatchesAuthoredPrimaries) {
    const RgbD cyan = patch(0), magenta = patch(1), yellow = patch(2);
    constexpr double kSep = 80.0;  // authored separation is a full 255 of ink

    EXPECT_LT(cyan.r, cyan.g - kSep)
        << "authored 100% CYAN must suppress RED, got rgb(" << cyan.r << ", "
        << cyan.g << ", " << cyan.b << ")";
    EXPECT_LT(cyan.r, cyan.b - kSep) << "authored 100% CYAN must suppress RED";

    EXPECT_LT(magenta.g, magenta.r - kSep)
        << "authored 100% MAGENTA must suppress GREEN, got rgb(" << magenta.r
        << ", " << magenta.g << ", " << magenta.b << ")";
    EXPECT_LT(magenta.g, magenta.b - kSep)
        << "authored 100% MAGENTA must suppress GREEN";

    EXPECT_LT(yellow.b, yellow.r - kSep)
        << "authored 100% YELLOW must suppress BLUE, got rgb(" << yellow.r << ", "
        << yellow.g << ", " << yellow.b << ")";
    EXPECT_LT(yellow.b, yellow.g - kSep)
        << "authored 100% YELLOW must suppress BLUE";

    // Patch 3's authored plates are ordered C < M < Y, so the rendered channels
    // must come out ordered R > G > B with a real gap.
    const RgbD mix = patch(3);
    EXPECT_GT(mix.r, mix.g + 25.0)
        << "authored patch 3 ordering R > G lost: rgb(" << mix.r << ", " << mix.g
        << ", " << mix.b << ")";
    EXPECT_GT(mix.g, mix.b + 25.0)
        << "authored patch 3 ordering G > B lost: rgb(" << mix.r << ", " << mix.g
        << ", " << mix.b << ")";
}

// Patches 3 and 4 carry IDENTICAL authored C/M/Y and differ only in the K plate
// (64 vs 0). Under any monotone transform, extra K ink must darken.
//
// Kills: dropping the K component during image sample decode (the two patches
// then render identically, luminance delta 0); reading only 3 of the 4
// components per pixel.
TEST_F(CmykImageXObjectTest, BlackPlateIsNotDropped) {
    const RgbD withK = patch(3), withoutK = patch(4);
    const double dl = luminance(withoutK) - luminance(withK);
    EXPECT_GT(dl, 20.0)
        << "authored K=64 patch (luma " << luminance(withK)
        << ") is not meaningfully darker than the same CMY with K=0 (luma "
        << luminance(withoutK) << ") — the K plate is being ignored";

    // Every channel must be darkened, not merely the average.
    EXPECT_LT(withK.r, withoutK.r - 10.0) << "K plate did not darken RED";
    EXPECT_LT(withK.g, withoutK.g - 10.0) << "K plate did not darken GREEN";
    EXPECT_LT(withK.b, withoutK.b - 10.0) << "K plate did not darken BLUE";
}

// The /Separation image's single component must be pushed through the authored
// type-2 tint transform (tint t -> CMYK(0, t, t, 0)) before conversion.
//
// Kills: ignoring the tint transform and rendering the 1-component samples as
// DeviceGray (grey renders R == G == B, and tint 255 would come out BLACK, not
// red); evaluating the transform with C0/C1 swapped (tint 0 would render red
// and tint 255 white — the monotonicity assertions invert); clamping the
// exponential function to its domain endpoints (the mid tint then collapses
// onto one of the extremes).
TEST_F(CmykImageXObjectTest, SeparationTintTransformIsApplied) {
    const RgbD t0 = tint(0), t1 = tint(1), t2 = tint(2);

    // Authored expectations, derived from the tint transform in the generator.
    EXPECT_TRUE(NearRgb(t0, naiveCmykToRgb(Cmyk{0, 0, 0, 0}), kTolAbs,
                        "Separation tint 0 (authored -> white)"));
    EXPECT_TRUE(NearRgb(t1, naiveCmykToRgb(Cmyk{0, 128, 128, 0}), kTolAbs,
                        "Separation tint 128 (authored -> CMYK 0,.502,.502,0)"));
    EXPECT_TRUE(NearRgb(t2, naiveCmykToRgb(Cmyk{0, 255, 255, 0}), kTolAbs,
                        "Separation tint 255 (authored -> CMYK 0,1,1,0)"));

    // Colour-management-proof form of the same claim: the spot is RED, not grey.
    EXPECT_GT(t2.r, t2.g + 100.0)
        << "authored full-tint spot is not red — tint transform ignored? rgb("
        << t2.r << ", " << t2.g << ", " << t2.b << ")";
    EXPECT_GT(t2.r, t2.b + 100.0) << "authored full-tint spot is not red";
    EXPECT_LT(std::fabs(t2.g - t2.b), 60.0)
        << "the authored spot lays down equal M and Y ink, so GREEN and BLUE "
           "must track each other";

    // Monotone in tint: more colourant is darker.
    EXPECT_GT(luminance(t0), luminance(t1) + 20.0)
        << "authored tint 0 must render lighter than tint 128";
    EXPECT_GT(luminance(t1), luminance(t2) + 20.0)
        << "authored tint 128 must render lighter than tint 255";
}
