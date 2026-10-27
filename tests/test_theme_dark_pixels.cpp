// -*- coding: utf-8 -*-
//
// Phase F11 — gaps WIN-36 / WIN-37 / WIN-39: dark mode and high contrast, on
// RENDERED PIXELS.
//
// WHAT WAS WRONG BEFORE
// ---------------------
// Every dark-mode assertion in the tree stopped at the QPalette. F8 added
// tests/test_theme_os_scheme_follow.cpp, which proves the palette OBJECT flips
// when the OS colour scheme changes — necessary, but it cannot see a widget
// that ignores the palette and paints a hard-coded light background, which is
// exactly the defect a user reports as "dark mode is half broken". Nothing
// anywhere rasterised a widget and looked at the resulting pixels.
//
// WHAT THIS FILE ASSERTS
// ----------------------
// Real product dialogs are rasterised with QWidget::grab() under the Light and
// the Dark palette and the two IMAGES are compared:
//
//   * mean luminance must drop substantially from light to dark;
//   * the dark render must contain almost no near-white pixels, while the
//     light render is mostly near-white — a widget that keeps painting a white
//     backdrop under the dark palette REDs this even though its palette is
//     correct;
//   * the dominant background pixel of each dark render must be close to the
//     ThemeManager dark base colour, per dialog (WIN-39).
//
// High contrast (WIN-37) is asserted the same way: pixels painted through
// AccessibilityHelper::adaptColor must actually change once the high-contrast
// state is on, rather than the helper merely returning a different QColor that
// no painter consumes.
//
// The oracle is deliberately STRUCTURAL (luminance statistics and a colour
// distance), not a stored golden bitmap: the self-hosted runner's PrintWindow
// render varies run to run by more than 15%, so a pixel-exact baseline would
// be a flake generator. See tests/ui_automation/README.md.

#include <gtest/gtest.h>

#include "theme/ThemeManager.h"
#include "ui/AccessibilityHelper.h"
#include "ui/CustomizeToolbarsDialog.h"
#include "ui/ValidationRequestDialog.h"

// WIN-39 (phase-8): every remaining product dialog under src/ui/.
#include "ui/AboutDialog.h"
#include "ui/AltTextDialog.h"
#include "ui/InsertImageDialog.h"
#include "ui/BatchDialog.h"
#include "ui/CalculationOrderDialog.h"
#include "ui/CompareDialog.h"
#include "ui/ComplianceDialog.h"
#include "ui/EncryptionDialog.h"
#include "ui/HeaderFooterDialog.h"
#include "ui/MetadataDialog.h"
#include "ui/OptimizeDialog.h"
#include "ui/PageLabelsDialog.h"
#include "ui/PasswordDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/PreflightDialog.h"
#include "ui/PrintDialog.h"
#include "ui/PrintPreviewDialog.h"
#include "ui/SignDialog.h"
#include "ui/TabOrderDialog.h"
#include "ui/TrustAnchorDialog.h"
#include "ui/WatermarkDialog.h"

#include "annotations/AnnotationManager.h"
#include "app/AppSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"

#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QImage>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStringList>

#include <cmath>
#include <filesystem>
#include <set>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::theme;
using namespace pdfe::ui;

namespace {

double relativeLuminance(const QColor& c)
{
    auto lin = [](double v) {
        v /= 255.0;
        return (v <= 0.03928) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

struct PixelStats {
    double meanLuminance = 0.0;
    double nearWhiteFraction = 0.0;
    QColor dominant;
    int sampled = 0;
};

PixelStats analyse(const QImage& imgIn)
{
    PixelStats s;
    if (imgIn.isNull()) return s;
    const QImage img = imgIn.convertToFormat(QImage::Format_RGB32);
    std::map<QRgb, int> histogram;
    double sum = 0.0;
    int nearWhite = 0;
    int n = 0;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            const QColor c = img.pixelColor(x, y);
            const double lum = relativeLuminance(c);
            sum += lum;
            if (lum > 0.75) ++nearWhite;
            ++histogram[c.rgb()];
            ++n;
        }
    }
    if (n == 0) return s;
    s.sampled = n;
    s.meanLuminance = sum / n;
    s.nearWhiteFraction = static_cast<double>(nearWhite) / n;
    QRgb best = 0;
    int bestCount = -1;
    for (const auto& kv : histogram) {
        if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
    }
    s.dominant = QColor(best);
    return s;
}

double colourDistance(const QColor& a, const QColor& b)
{
    const double dr = a.red()   - b.red();
    const double dg = a.green() - b.green();
    const double db = a.blue()  - b.blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// Rasterise a freshly-constructed dialog under the given theme. The dialog is
// built AFTER the application palette is installed so it can never inherit a
// stale one.
QImage renderUnderTheme(Theme theme, const std::function<QDialog*()>& make)
{
    QApplication::setPalette(ThemeManager::paletteFor(theme));
    std::unique_ptr<QDialog> dlg(make());
    if (!dlg) return QImage();
    dlg->setPalette(ThemeManager::paletteFor(theme));
    dlg->resize(560, 400);
    if (dlg->layout()) dlg->layout()->activate();
    dlg->ensurePolished();
    return dlg->grab().toImage();
}

QDialog* makeValidationRequestDialog()
{
    return new ValidationRequestDialog(
        QStringList{QStringLiteral("http://ocsp.example.test/responder")},
        QStringList{QStringLiteral("http://crl.example.test/list.crl")},
        nullptr);
}

QDialog* makeCustomizeToolbarsDialog()
{
    return new CustomizeToolbarsDialog(std::vector<QAction*>{},
                                       std::vector<ToolbarLayout>{},
                                       nullptr);
}

// ThemeManager's hard-coded dark base (mirrors test_theme_os_scheme_follow.cpp).
const QColor kDarkWindow(53, 53, 53);

class ThemeDarkPixelsTest : public ::testing::Test {
protected:
    void SetUp() override { saved_ = QApplication::palette(); }
    void TearDown() override {
        AccessibilityHelper::clearHighContrastOverrideForTesting();
        QApplication::setPalette(saved_);   // palette is process-global
    }
    QPalette saved_;
};

} // namespace

// ─── WIN-36: rendered pixels get darker, and stop being near-white ─────────

TEST_F(ThemeDarkPixelsTest, DarkThemeRendersDarkerPixelsThanLight)
{
    ASSERT_NE(qApp, nullptr);

    const QImage light = renderUnderTheme(Theme::Light, makeValidationRequestDialog);
    const QImage dark  = renderUnderTheme(Theme::Dark,  makeValidationRequestDialog);
    ASSERT_FALSE(light.isNull());
    ASSERT_FALSE(dark.isNull());
    ASSERT_GT(light.width(), 100);
    ASSERT_GT(dark.width(), 100);

    const PixelStats ls = analyse(light);
    const PixelStats ds = analyse(dark);
    ASSERT_GT(ls.sampled, 1000);
    ASSERT_GT(ds.sampled, 1000);

    EXPECT_LT(ds.meanLuminance, ls.meanLuminance)
        << "dark render is not darker than light (dark " << ds.meanLuminance
        << " vs light " << ls.meanLuminance << ")";
    EXPECT_LT(ds.meanLuminance, 0.25)
        << "dark render mean luminance " << ds.meanLuminance
        << " is too bright to be a dark theme";
    EXPECT_GT(ls.meanLuminance - ds.meanLuminance, 0.2)
        << "the light/dark luminance gap is too small to be a real theme change";
}

TEST_F(ThemeDarkPixelsTest, NoNearWhiteRegionSurvivesInTheDarkRender)
{
    ASSERT_NE(qApp, nullptr);

    const PixelStats ls = analyse(renderUnderTheme(Theme::Light, makeValidationRequestDialog));
    const PixelStats ds = analyse(renderUnderTheme(Theme::Dark,  makeValidationRequestDialog));
    ASSERT_GT(ls.sampled, 1000);
    ASSERT_GT(ds.sampled, 1000);

    // A widget that ignores the palette and paints a fixed white backdrop keeps
    // a large near-white area under the dark theme. This is the assertion that
    // a palette-only test structurally cannot make.
    EXPECT_GT(ls.nearWhiteFraction, 0.30)
        << "light render is not predominantly near-white (" << ls.nearWhiteFraction << ")";
    EXPECT_LT(ds.nearWhiteFraction, 0.05)
        << "dark render still has " << (ds.nearWhiteFraction * 100.0)
        << "% near-white pixels — some surface ignores the dark palette";
}

// ─── WIN-39: each dialog's dark backdrop really is the dark base colour ────

TEST_F(ThemeDarkPixelsTest, EveryCheckedDialogBackgroundFollowsTheDarkPalette)
{
    ASSERT_NE(qApp, nullptr);

    const std::vector<std::pair<const char*, std::function<QDialog*()>>> dialogs = {
        {"ValidationRequestDialog",  makeValidationRequestDialog},
        {"CustomizeToolbarsDialog",  makeCustomizeToolbarsDialog},
    };

    for (const auto& d : dialogs) {
        const PixelStats ds = analyse(renderUnderTheme(Theme::Dark, d.second));
        ASSERT_GT(ds.sampled, 1000) << d.first << " produced no render";
        EXPECT_LT(colourDistance(ds.dominant, kDarkWindow), 60.0)
            << d.first << ": dominant dark-render colour is "
            << ds.dominant.name().toStdString() << ", expected near "
            << kDarkWindow.name().toStdString();
        EXPECT_LT(ds.meanLuminance, 0.30) << d.first << " is not rendering dark";
    }
}

// ─── WIN-37: high contrast reaches the PIXELS, not just the helper ────────

TEST_F(ThemeDarkPixelsTest, HighContrastChangesTheRenderedOverlayPixels)
{
    ASSERT_NE(qApp, nullptr);

    // A stand-in for the custom-painted surfaces (PdfCanvas selection /
    // search-highlight overlays) that call adaptColor at paint time.
    auto paintOverlay = [](const QColor& base) {
        QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.fillRect(img.rect(),
                   AccessibilityHelper::adaptColor(base, QPalette::Highlight));
        p.end();
        return img;
    };

    const QColor base(0, 120, 215, 200);

    QApplication::setPalette(ThemeManager::paletteFor(Theme::Dark));
    AccessibilityHelper::clearHighContrastOverrideForTesting();
    AccessibilityHelper::setHighContrastOverrideForTesting(false);
    const QImage normal = paintOverlay(base);

    AccessibilityHelper::setHighContrastOverrideForTesting(true);
    const QImage highContrast = paintOverlay(base);

    ASSERT_FALSE(normal.isNull());
    ASSERT_FALSE(highContrast.isNull());
    EXPECT_NE(normal, highContrast)
        << "high-contrast mode produced identical pixels — adaptColor's result "
           "never reaches the painted surface";

    const QColor n = normal.pixelColor(32, 32);
    const QColor h = highContrast.pixelColor(32, 32);
    EXPECT_GT(colourDistance(n, h), 10.0)
        << "high-contrast overlay colour barely moved: " << n.name().toStdString()
        << " -> " << h.name().toStdString();
    // Translucency of the overlay must survive the adaptation.
    EXPECT_EQ(h.alpha(), n.alpha());
}

// ════════════════════════════════════════════════════════════════════════════
// WIN-39 (phase-8) — EVERY product dialog, not the two that were convenient
// ════════════════════════════════════════════════════════════════════════════
//
// EveryCheckedDialogBackgroundFollowsTheDarkPalette above is a real test with
// a real oracle. Its weakness is in its name: "EveryChecked". It checked two
// dialogs — ValidationRequestDialog and CustomizeToolbarsDialog — out of the
// twenty-three that exist under src/ui/, and nothing said so. A reader
// encountering a passing "every dialog follows the dark palette" test has no
// way to learn it means "two of them".
//
// So this section does two things, and the second is the one that lasts:
//
//   1. Renders EVERY product dialog under the dark theme and asserts the same
//      property. Dialogs needing a Document, a Page, a FormManager or an
//      AppSettings get real ones, built once from tests/fixtures/test_3page.pdf
//      — a dialog constructed with null dependencies may take an early-out
//      path and render an empty shell, which would pass the colour assertion
//      while telling us nothing about the populated dialog a user sees.
//
//   2. Enumerates src/ui/*Dialog*.h from disk and requires every one to be
//      either covered above or listed in kDeliberatelyUncovered WITH A REASON.
//      A dialog added tomorrow is red until someone makes that choice
//      explicitly. This is the same standing-enumeration shape used for the
//      single-CSPRNG guard in phase-60: the failure mode being designed
//      against is not a wrong answer, it is silent partial coverage that reads
//      as total.

namespace {

// Dialogs that exist under src/ui/ and are deliberately NOT rendered here.
// Each needs a reason. An empty reason is a compile-time-visible admission
// that nobody decided.
struct UncoveredDialog { const char* header; const char* reason; };

const UncoveredDialog kDeliberatelyUncovered[] = {
    {"TestDialogSeam",
     "test seam, not a product dialog — it exists to let UI tests drive a "
     "modal without one of the real dialogs appearing"},
    {"AtlasComparisonDialog",
     "diagnostic-only TRE tool; constructing it requires two populated "
     "EnrichedAtlas objects and a rendered line image, which is a font-"
     "pipeline run rather than a dialog construction. It is not reachable "
     "from the shipping menus"},
};

bool isDeliberatelyUncovered(const std::string& stem)
{
    for (const auto& u : kDeliberatelyUncovered) {
        if (stem == u.header) return true;
    }
    return false;
}

// One document, one page, one settings object, shared by every factory that
// needs them. Built lazily so the file's other tests do not pay for it.
struct DialogDeps {
    std::shared_ptr<pdfe::core::MuPdfContext> ctx;
    std::unique_ptr<pdfe::core::Document> doc;
    std::unique_ptr<pdfe::core::Page> page;
    std::unique_ptr<pdfe::forms::FormManager> forms;
    std::unique_ptr<pdfe::annotations::AnnotationManager> annots;
    std::unique_ptr<pdfe::app::AppSettings> settings;
    bool ready = false;
};

// DELIBERATELY NOT A FUNCTION-LOCAL STATIC, and this cost a suite run.
//
// The first version cached these in a `static DialogDeps`, so the MuPdfContext,
// Document and Page were destroyed during STATIC DESTRUCTION — after main() has
// returned, after QApplication is gone, and after MuPDF's own teardown. The
// whole shard exited 0xC0000005 with every test passing, and `run_gtest.py`
// reported "rc=3221225477 -> ok" because the XML was complete. A crash that
// arrives after the last assertion is invisible to the thing that reads
// assertions.
//
// Built per-test instead. One document open is not worth a lifetime hazard.
void initDeps(DialogDeps& d)
{
    d.ctx = std::make_shared<pdfe::core::MuPdfContext>();
    const auto fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    if (std::filesystem::exists(fixture)) {
        d.doc = pdfe::core::Document::open(d.ctx, fixture);
        if (d.doc) d.page = d.doc->page(0);
    }
    d.forms  = std::make_unique<pdfe::forms::FormManager>(d.ctx);
    d.annots = std::make_unique<pdfe::annotations::AnnotationManager>(d.ctx);
    d.settings = std::make_unique<pdfe::app::AppSettings>();
    d.ready = true;
}

} // namespace

TEST_F(ThemeDarkPixelsTest, EveryProductDialogBackgroundFollowsTheDarkPalette)
{
    ASSERT_NE(qApp, nullptr);

    DialogDeps d;
    initDeps(d);
    ASSERT_NE(d.doc, nullptr)
        << "tests/fixtures/test_3page.pdf did not open; the document-backed "
           "dialogs would be constructed with null dependencies and could "
           "render an empty shell that passes for the wrong reason";
    ASSERT_NE(d.page, nullptr);

    const std::vector<std::pair<const char*, std::function<QDialog*()>>> dialogs = {
        // Already covered by the case above; repeated so this list is the
        // complete statement rather than a supplement to be cross-referenced.
        {"ValidationRequestDialog", makeValidationRequestDialog},
        {"CustomizeToolbarsDialog", makeCustomizeToolbarsDialog},

        // Parent-only construction.
        {"AboutDialog",        [] { return new AboutDialog(nullptr); }},
        {"EncryptionDialog",   [] { return new EncryptionDialog(nullptr); }},
        {"MetadataDialog",     [] { return new MetadataDialog(nullptr); }},
        {"PasswordDialog",     [] { return new PasswordDialog(nullptr); }},
        {"SignDialog",         [] { return new SignDialog(nullptr); }},
        {"TrustAnchorDialog",  [] { return new TrustAnchorDialog(nullptr); }},

        // Context-backed.
        {"BatchDialog",   [&d] { return new BatchDialog(d.ctx, nullptr); }},
        {"CompareDialog", [&d] { return new CompareDialog(d.ctx, nullptr); }},

        // Context + document.
        {"ComplianceDialog",
         [&d] { return new ComplianceDialog(d.ctx, d.doc.get(), nullptr); }},
        {"HeaderFooterDialog",
         [&d] { return new HeaderFooterDialog(d.ctx, d.doc.get(), nullptr); }},
        {"OptimizeDialog",
         [&d] { return new OptimizeDialog(d.ctx, d.doc.get(), nullptr); }},
        {"WatermarkDialog",
         [&d] { return new WatermarkDialog(d.ctx, d.doc.get(), nullptr); }},

        // Document-backed.
        {"PageLabelsDialog",
         [&d] { return new PageLabelsDialog(d.doc.get(), nullptr); }},
        {"PrintDialog",
         [&d] { return new PrintDialog(d.doc.get(), 0, nullptr); }},
        {"PrintPreviewDialog",
         [&d] { return new PrintPreviewDialog(d.doc.get(),
                                              pdfe::core::PrintOptions{},
                                              nullptr); }},
        {"PreflightDialog",
         [&d] { return new PreflightDialog(d.doc.get(), d.ctx, nullptr); }},

        // Form / annotation backed.
        {"CalculationOrderDialog",
         [&d] { return new CalculationOrderDialog(d.forms.get(), d.doc.get(),
                                                  nullptr); }},
        {"TabOrderDialog",
         [&d] { return new TabOrderDialog(d.forms.get(), d.page.get(),
                                          nullptr); }},
        {"AltTextDialog",
         [&d] { return new AltTextDialog(d.annots.get(), d.page.get(), 0,
                                         nullptr); }},

        // GitHub #242 — the Insert Image placement/options dialog. Rendered for
        // real rather than exempted: it takes only plain values (a path, the
        // image's pixel size, the page size in points), so there is no fixture
        // reason it cannot be shown here.
        {"InsertImageDialog",
         [] { return new InsertImageDialog(QStringLiteral("p94.png"),
                                           QSize(800, 600),
                                           QSizeF(612.0, 792.0), nullptr); }},

        // Settings-backed.
        {"PreferencesDialog",
         [&d] { return new PreferencesDialog(d.settings.get(), nullptr); }},
    };

    for (const auto& entry : dialogs) {
        SCOPED_TRACE(entry.first);
        const PixelStats ds = analyse(renderUnderTheme(Theme::Dark, entry.second));
        ASSERT_GT(ds.sampled, 1000)
            << entry.first << " produced no render";
        EXPECT_LT(colourDistance(ds.dominant, kDarkWindow), 60.0)
            << entry.first << ": dominant dark-render colour is "
            << ds.dominant.name().toStdString() << ", expected near "
            << kDarkWindow.name().toStdString()
            << " — this dialog paints a surface that ignores the palette";
        EXPECT_LT(ds.meanLuminance, 0.30)
            << entry.first << " is not rendering dark (mean luminance "
            << ds.meanLuminance << ")";
    }

    // The list must actually be a list. A refactor that emptied it would
    // otherwise leave a green test asserting nothing.
    EXPECT_GE(dialogs.size(), 20u)
        << "the dialog coverage list has shrunk to " << dialogs.size();
}

// The standing control. This is what stops WIN-39 from reopening silently.
TEST(ThemeDialogCoverageEnumeration, EveryDialogHeaderIsCoveredOrJustified)
{
    const auto uiDir = std::filesystem::path(PDFE_REPO_ROOT) / "src" / "ui";
    ASSERT_TRUE(std::filesystem::exists(uiDir)) << uiDir.string();

    // The names rendered by EveryProductDialogBackgroundFollowsTheDarkPalette.
    // Kept as a separate literal list deliberately: deriving it from the
    // factory table would make this test agree with that one by construction,
    // and it is meant to be an independent check on it.
    static const std::set<std::string> kCovered = {
        "AboutDialog", "AltTextDialog", "BatchDialog", "CalculationOrderDialog",
        "CompareDialog", "ComplianceDialog", "CustomizeToolbarsDialog",
        "EncryptionDialog", "HeaderFooterDialog", "MetadataDialog",
        "OptimizeDialog", "PageLabelsDialog", "PasswordDialog",
        "InsertImageDialog",
        "PreferencesDialog", "PreflightDialog", "PrintDialog",
        "PrintPreviewDialog", "SignDialog", "TabOrderDialog",
        "TrustAnchorDialog", "ValidationRequestDialog", "WatermarkDialog",
    };

    std::vector<std::string> unaccounted;
    int headers = 0;

    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(uiDir, ec)) {
        if (ec) break;
        const auto& p = de.path();
        if (p.extension() != ".h") continue;
        const std::string stem = p.stem().string();
        if (stem.find("Dialog") == std::string::npos) continue;
        ++headers;
        if (kCovered.count(stem) == 0 && !isDeliberatelyUncovered(stem)) {
            unaccounted.push_back(stem);
        }
    }

    EXPECT_GT(headers, 20)
        << "only " << headers << " dialog headers were found under "
        << uiDir.string() << "; this case cannot see the tree it audits";

    std::string joined;
    for (const auto& u : unaccounted) joined += "  " + u + "\n";
    EXPECT_TRUE(unaccounted.empty())
        << unaccounted.size() << " dialog(s) under src/ui/ are neither "
           "rendered by EveryProductDialogBackgroundFollowsTheDarkPalette nor "
           "listed in kDeliberatelyUncovered with a reason:\n"
        << joined
        << "\nAdd a factory to the coverage list, or add an entry to "
           "kDeliberatelyUncovered saying why the dialog cannot be rendered "
           "here. Do not delete this test.";

    // Every exclusion must carry a reason. An entry with an empty one is
    // someone deferring the decision rather than making it.
    for (const auto& u : kDeliberatelyUncovered) {
        EXPECT_GT(std::string(u.reason).size(), 20u)
            << u.header << " is excluded without a usable reason";
    }
}
