// -*- coding: utf-8 -*-
// =============================================================================
// WIN-87 wider class, phase-60, acceptance criterion 3 — clipboard image paste.
// =============================================================================
//
// The hostile-filesystem case the phase asks for, driven through the real
// MainWindow::pasteImageFromClipboard() rather than through a re-creation of
// what it does. The distinction matters here more than usual: the whole defect
// class is "a hardened primitive sitting beside an unmigrated call site", so a
// test that exercises the primitive alone would have passed against the
// vulnerable code.
//
// WHAT MADE THIS THE WORST MEMBER OF THE FAMILY. The temp PNG is deliberately
// LEFT ON DISK — InsertImageCommand::redo() re-reads the source file by path,
// so deleting it would break redo after an undo. Combine that with the old
// name, "pdfe_clip_paste_<n>.png" where n came from a counter that restarts at
// 0 every process, and the attacker did not need to win a race at all: the
// first paste of every session was always pdfe_clip_paste_0.png, and they could
// swap it at leisure and wait for the user to press Ctrl+Z, Ctrl+Y. The image
// that then entered the document was theirs.
#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "core/TempFileSafety.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QImage>
#include <QString>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace pdfe::ui;

namespace {

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// Every "pdfe_clip_paste.*" currently in %TEMP%. The paste path names its file
/// with 128 CSPRNG bits, so the test cannot predict it — which is exactly the
/// property under test — and finds it by difference instead.
std::vector<fs::path> pasteArtifacts()
{
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("pdfe_clip_paste", 0) == 0) out.push_back(e.path());
    }
    return out;
}

/// Put `img` on the clipboard, retrying the WRITE.
///
/// Under RDP, rdpclip can swallow the clipboard write — this project has the
/// root cause on file: retry the COPY, never relax the read-side assertion.
bool putImageOnClipboard(const QImage& img)
{
    QClipboard* cb = QApplication::clipboard();
    if (!cb) return false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        cb->setImage(img);
        QApplication::processEvents();
        if (!cb->image().isNull()) return true;
    }
    return false;
}

QImage distinctiveImage()
{
    QImage img(24, 16, QImage::Format_RGB32);
    img.fill(qRgb(0x21, 0x8B, 0xE5));
    // A marker the attacker's substitute will not carry.
    img.setPixel(0, 0, qRgb(0xFF, 0x00, 0x7F));
    return img;
}

}  // namespace

TEST(ClipboardPasteSubstitutionPhase60, PastedImageCannotBeSubstitutedBeforeOrAfterRedo)
{
    const auto before = pasteArtifacts();

    MainWindow w;
    w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
    QApplication::processEvents();

    ASSERT_TRUE(putImageOnClipboard(distinctiveImage()))
        << "the clipboard would not hold an image, so this case establishes "
           "nothing — investigate the clipboard owner rather than relaxing this";

    ASSERT_TRUE(w.pasteImageFromClipboard())
        << "paste failed outright";

    // Exactly one new artifact, and its name is the hardened shape.
    auto after = pasteArtifacts();
    std::vector<fs::path> fresh;
    for (const auto& p : after) {
        bool seen = false;
        for (const auto& q : before) seen = seen || (p == q);
        if (!seen) fresh.push_back(p);
    }
    ASSERT_EQ(fresh.size(), 1u)
        << "expected exactly one new pdfe_clip_paste artifact; found "
        << fresh.size();
    const fs::path pasted = fresh.front();

    const std::string name = pasted.filename().string();
    EXPECT_EQ(name.rfind("pdfe_clip_paste.", 0), 0u) << name;
    EXPECT_EQ(name.size(), std::string("pdfe_clip_paste.").size() + 32u + 4u)
        << "the pasted image's name is not 32 hex digits + \".png\": " << name
        << " — a counter-derived name is guessable from the first paste of the "
           "session onward";

    const std::string genuine = readFile(pasted);
    ASSERT_FALSE(genuine.empty());

#if defined(_WIN32)
    // ---- The attacker, with the path in hand. ----
    // Everything below is what an attacker who has ALREADY learned the name
    // would do; the CSPRNG name means they should not get this far, but the pin
    // is what holds if they do.

    // 1. Rewrite the bytes in place.
    HANDLE h = ::CreateFileW(pasted.wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(h, INVALID_HANDLE_VALUE)
        << "the pasted image is writable — an attacker can replace it and wait "
           "for the user's next redo";
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);

    // 2. Delete it, so the next redo reads whatever they put back.
    EXPECT_EQ(::DeleteFileW(pasted.wstring().c_str()), 0)
        << "the pasted image was deletable";

    // 3. Rename over it — the substitution primitive the whole WIN-87 class
    //    turns on, and the one a same-directory attacker reaches for.
    std::error_code ec;
    const fs::path decoy = pasted.parent_path() / "pdfe_clip_paste_decoy.png";
    {
        QImage evil(24, 16, QImage::Format_RGB32);
        evil.fill(qRgb(0, 0, 0));
        ASSERT_TRUE(evil.save(QString::fromStdWString(decoy.wstring()), "PNG"));
    }
    EXPECT_EQ(::MoveFileExW(decoy.wstring().c_str(), pasted.wstring().c_str(),
                            MOVEFILE_REPLACE_EXISTING), 0)
        << "an attacker renamed their image over the pasted one";
    fs::remove(decoy, ec);
#endif

    // 4. And the bytes the undo stack will re-read are still ours — which is
    //    the property the user actually cares about, stated independently of
    //    which of the three primitives above was tried.
    EXPECT_EQ(readFile(pasted), genuine)
        << "the pasted image's bytes changed under an attacker's hands";

    // 5. THE VACUITY GUARD, and the one that matters most here. A pin that
    //    denied readers as well as writers would satisfy every assertion above
    //    while BREAKING the feature: redo() re-reads the file by path, and if
    //    the pin locked it out, undo-then-redo would fail and the user would
    //    lose the pasted image. Withholding only FILE_SHARE_WRITE and
    //    FILE_SHARE_DELETE is what keeps the re-read working, and this is where
    //    that is proved rather than assumed.
    auto* undoAction = w.findChild<QAction*>("undoAction");
    auto* redoAction = w.findChild<QAction*>("redoAction");
    ASSERT_NE(undoAction, nullptr);
    ASSERT_NE(redoAction, nullptr);
    undoAction->trigger();
    QApplication::processEvents();
    redoAction->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(fs::exists(pasted))
        << "the pinned image vanished across undo/redo";
    EXPECT_EQ(readFile(pasted), genuine)
        << "the image redo() re-read is not the one paste wrote";
}

TEST(ClipboardPasteSubstitutionPhase60, PastedArtifactsAreRemovedWhenTheWindowGoesAway)
{
    // The old path left every pasted image in %TEMP% forever, by design — it
    // had no owner that outlived the paste and no way to know when redo could
    // no longer reach the file. The pin gives it one, so the leak closes as a
    // side effect and is worth pinning so it does not quietly return.
    const auto before = pasteArtifacts();
    fs::path pasted;
    {
        MainWindow w;
        w.openFile(QStringLiteral(TEST_FIXTURES_DIR "/cropbox_offset.pdf"));
        QApplication::processEvents();
        ASSERT_TRUE(putImageOnClipboard(distinctiveImage()));
        ASSERT_TRUE(w.pasteImageFromClipboard());

        for (const auto& p : pasteArtifacts()) {
            bool seen = false;
            for (const auto& q : before) seen = seen || (p == q);
            if (!seen) pasted = p;
        }
        ASSERT_FALSE(pasted.empty());
        ASSERT_TRUE(fs::exists(pasted));
    }
    QApplication::processEvents();
    EXPECT_FALSE(fs::exists(pasted))
        << "the pasted image outlived the window that owned it: " << pasted;
}
