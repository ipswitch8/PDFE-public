#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "forms/JavaScriptEngine.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include <filesystem>
#include <memory>

class JavaScriptEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;

        // Try to load a form PDF with JavaScript support
        formPdf = fixturesDir / "test_form.pdf";
        if (std::filesystem::exists(formPdf)) {
            document = pdfe::core::Document::open(context, formPdf);
            engine = std::make_unique<pdfe::forms::JavaScriptEngine>(context);
            manager = std::make_unique<pdfe::forms::FormManager>(context);
        }
    }

    void TearDown() override {
        engine.reset();
        manager.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path formPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> engine;
    std::unique_ptr<pdfe::forms::FormManager> manager;
};

TEST_F(JavaScriptEngineTest, EnableJavaScript) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    EXPECT_FALSE(engine->isEnabled());

    engine->enable(page.get());
    EXPECT_TRUE(engine->isEnabled());
}

TEST_F(JavaScriptEngineTest, EnableJavaScript_NullPageThrows) {
    EXPECT_THROW(engine->enable(nullptr), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, CalculateAllFields) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);

    // Enable JavaScript first
    engine->enable(page.get());

    // Call calculateAll - should not crash
    EXPECT_NO_THROW(engine->calculateAll(page.get()));
}

TEST_F(JavaScriptEngineTest, CalculateAllFields_NullPageThrows) {
    EXPECT_THROW(engine->calculateAll(nullptr), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireFocusBlurEvents) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test_form.pdf";
    }

    // Enable JavaScript first
    engine->enable(page.get());

    // Fire focus event on first widget
    EXPECT_NO_THROW(engine->fireFocusEvent(page.get(), 0));

    // Fire blur event on first widget
    EXPECT_NO_THROW(engine->fireBlurEvent(page.get(), 0));
}

TEST_F(JavaScriptEngineTest, FireFocusEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireFocusEvent(nullptr, 0), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireBlurEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireBlurEvent(nullptr, 0), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireFocusEvent_InvalidWidgetThrows) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    engine->enable(page.get());

    EXPECT_THROW(engine->fireFocusEvent(page.get(), 9999), pdfe::core::PdfeException);
}

TEST_F(JavaScriptEngineTest, FireKeystrokeEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Enable JavaScript first
    engine->enable(page.get());

    // Fire keystroke event - should return non-negative value (accepted or rejected)
    int result = engine->fireKeystrokeEvent(
        page.get(), textIdx,
        "Hello", "W", 5, 5, false
    );

    EXPECT_GE(result, 0);
}

TEST_F(JavaScriptEngineTest, FireKeystrokeEvent_NullPageThrows) {
    EXPECT_THROW(
        engine->fireKeystrokeEvent(nullptr, 0, "", "", 0, 0, false),
        std::invalid_argument
    );
}

TEST_F(JavaScriptEngineTest, FireKeystrokeEvent_InvalidWidgetThrows) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    engine->enable(page.get());

    EXPECT_THROW(
        engine->fireKeystrokeEvent(page.get(), 9999, "", "", 0, 0, false),
        pdfe::core::PdfeException
    );
}

TEST_F(JavaScriptEngineTest, FireValidateEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Enable JavaScript first
    engine->enable(page.get());

    // Fire validate event - should not crash
    bool result = engine->fireValidateEvent(page.get(), textIdx, "TestValue");

    // A3-19/A3-41 (self-negating tautology): `EXPECT_TRUE(result || !result)`
    // holds for every bool. The property that actually matters for a JS event
    // engine is that firing the same event twice with the same input yields the
    // same verdict — i.e. no state leaks between events — and that the engine is
    // still usable afterwards.
    const bool again = engine->fireValidateEvent(page.get(), textIdx, "TestValue");
    EXPECT_EQ(result, again)
        << "fireValidateEvent is not deterministic for an unchanged field and "
           "value; JS state is leaking between events";
    EXPECT_NO_THROW(engine->fireValidateEvent(page.get(), textIdx, "Other"))
        << "the engine must remain usable after a validate event";
}

TEST_F(JavaScriptEngineTest, FireValidateEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireValidateEvent(nullptr, 0, ""), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireCalculateEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test_form.pdf";
    }

    // Enable JavaScript first
    engine->enable(page.get());

    // Fire calculate event on first field - should not crash
    EXPECT_NO_THROW(engine->fireCalculateEvent(page.get(), 0));
}

TEST_F(JavaScriptEngineTest, FireCalculateEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireCalculateEvent(nullptr, 0), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireFormatEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text) {
            textIdx = f.widgetIndex;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No text field in test_form.pdf";
    }

    // Enable JavaScript first
    engine->enable(page.get());

    // Fire format event - should return a string (possibly empty)
    std::string formatted = engine->fireFormatEvent(page.get(), textIdx);

    // A3-19/A3-41 (self-negating tautology): `x.empty() || !x.empty()` is true
    // for every string. The real property is determinism across repeated fires
    // of the same format event on an unchanged field.
    const std::string again = engine->fireFormatEvent(page.get(), textIdx);
    EXPECT_EQ(formatted, again)
        << "fireFormatEvent is not deterministic for an unchanged field; JS "
           "state is leaking between events";
}

TEST_F(JavaScriptEngineTest, FireFormatEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireFormatEvent(nullptr, 0), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireWillSaveEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    engine->enable(page.get());

    // Fire willSave event - should not crash
    EXPECT_NO_THROW(engine->fireWillSave(page.get()));
}

TEST_F(JavaScriptEngineTest, FireWillSaveEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireWillSave(nullptr), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, FireDidSaveEvent) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    engine->enable(page.get());

    // Fire didSave event - should not crash
    EXPECT_NO_THROW(engine->fireDidSave(page.get()));
}

TEST_F(JavaScriptEngineTest, FireDidSaveEvent_NullPageThrows) {
    EXPECT_THROW(engine->fireDidSave(nullptr), std::invalid_argument);
}

TEST_F(JavaScriptEngineTest, MoveConstructor) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);

    auto engine1 = std::make_unique<pdfe::forms::JavaScriptEngine>(context);
    engine1->enable(page.get());
    EXPECT_TRUE(engine1->isEnabled());

    // Move construct
    pdfe::forms::JavaScriptEngine engine2(std::move(*engine1));
    EXPECT_TRUE(engine2.isEnabled());
}

TEST_F(JavaScriptEngineTest, MoveAssignment) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);

    auto engine1 = std::make_unique<pdfe::forms::JavaScriptEngine>(context);
    engine1->enable(page.get());
    EXPECT_TRUE(engine1->isEnabled());

    pdfe::forms::JavaScriptEngine engine2(context);
    EXPECT_FALSE(engine2.isEnabled());

    // Move assign
    engine2 = std::move(*engine1);
    EXPECT_TRUE(engine2.isEnabled());
}
