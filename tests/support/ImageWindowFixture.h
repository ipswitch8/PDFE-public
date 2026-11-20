// -*- coding: utf-8 -*-
#pragma once
//
// Shared window-level fixture for the image state tests: opens
// test_single.pdf in a real MainWindow whose insert-image dialog is replaced
// by a fixed 200x120 placement at (150, 200), then inserts a red 64x64 PNG.
//
// Single definition (previously duplicated in test_image_editor_state_probes.cpp
// and test_image_placement_state.cpp; see
// tools/gate/check_duplicate_test_definitions.py).

#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <optional>
#include <string>

#ifndef TEST_FIXTURES_DIR
#error "TEST_FIXTURES_DIR must be defined by the test target"
#endif

namespace pdfe::testsupport {

class PlacingMainWindow : public pdfe::ui::MainWindow {
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize, QSizeF) override {
        InsertImageChoice choice;
        choice.targetRect = QRectF(150.0, 200.0, 200.0, 120.0);
        choice.jpegQuality = 90;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

struct WindowFixture {
    QTemporaryDir tmp;
    PlacingMainWindow mw;
    pdfe::image::ImageTool* tool = nullptr;
    std::string name;

    static void settleEvents() {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }

    static std::string fixturePath(const char* fname) {
        for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
            const std::string c = std::string(prefix) + fname;
            if (std::filesystem::exists(c)) return c;
        }
        const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + fname;
        return std::filesystem::exists(d) ? d : std::string();
    }

    bool open() {
        if (!QApplication::instance()) {
            static int argc = 0;
            static char* argv[] = { nullptr };
            static QApplication app(argc, argv);
        }
        qRegisterMetaType<std::string>("std::string");
        const std::string pdf = fixturePath("test_single.pdf");
        const std::string png = tmp.filePath("red.png").toStdString();
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        if (pdf.empty() || !red.save(QString::fromStdString(png))) return false;
        mw.openFile(QString::fromStdString(pdf));
        settleEvents();
        if (!mw.documentForTesting()) return false;
        pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(png));
        auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
        if (!insert) return false;
        insert->trigger();
        settleEvents();
        auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
        tool = canvas ? canvas->imageTool() : nullptr;
        if (!tool || !tool->hasSelection()) return false;
        name = tool->selectedImage().xobjectName;
        return true;
    }
    pdfe::image::PageImageInfo onPage() {
        using namespace pdfe::core;
        using namespace pdfe::image;
        Document* doc = mw.documentForTesting();
        ImageManager mgr(doc->context());
        auto page = doc->page(0);
        for (const auto& i : mgr.enumerateImages(page.get(), doc))
            if (i.xobjectName == name) return i;
        ADD_FAILURE() << "image not on the page";
        return {};
    }
};

} // namespace pdfe::testsupport
