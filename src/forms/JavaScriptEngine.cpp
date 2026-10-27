#include "JavaScriptEngine.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <stdexcept>
#include <cstring>

namespace pdfe::forms {

// Pimpl implementation
class JavaScriptEngine::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;
    bool enabled_ = false;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    /// Get the pdf_page from a Page object.
    pdf_page* getPdfPage(const pdfe::core::Page* page) {
        fz_page* fzPage = page->fzPage();
        return context->executeReturn<pdf_page*>([fzPage](fz_context* ctx) {
            return pdf_page_from_fz_page(ctx, fzPage);
        });
    }

    /// Helper to get the nth widget from a page.
    static pdf_annot* getNthWidget(fz_context* ctx, pdf_page* page, int index) {
        pdf_annot* widget = pdf_first_widget(ctx, page);
        for (int i = 0; i < index && widget; i++) {
            widget = pdf_next_widget(ctx, widget);
        }
        return widget;
    }
};

// JavaScriptEngine implementation

JavaScriptEngine::JavaScriptEngine(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx))) {}

JavaScriptEngine::~JavaScriptEngine() = default;

JavaScriptEngine::JavaScriptEngine(JavaScriptEngine&&) noexcept = default;
JavaScriptEngine& JavaScriptEngine::operator=(JavaScriptEngine&&) noexcept = default;

void JavaScriptEngine::enable(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([pdfPage](fz_context* ctx) {
        pdf_document* pdfDoc = pdfPage->doc;
        pdf_enable_js(ctx, pdfDoc);
    });

    pImpl->enabled_ = true;
}

bool JavaScriptEngine::isEnabled() const {
    return pImpl->enabled_;
}

int JavaScriptEngine::fireKeystrokeEvent(
    const pdfe::core::Page* page, int widgetIndex,
    const std::string& value, const std::string& change,
    int selStart, int selEnd, bool willCommit)
{
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<int>(
        [pdfPage, widgetIndex, &value, &change, selStart, selEnd, willCommit](fz_context* ctx) -> int {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_document* pdfDoc = pdfPage->doc;

            // Build the keystroke event structure
            pdf_keystroke_event evt;
            evt.value = value.c_str();
            evt.change = change.c_str();
            evt.selStart = selStart;
            evt.selEnd = selEnd;
            evt.willCommit = willCommit ? 1 : 0;
            evt.newChange = nullptr;
            evt.newValue = nullptr;

            int rc = pdf_annot_field_event_keystroke(ctx, pdfDoc, widget, &evt);

            // Free any allocated strings
            if (evt.newChange) {
                fz_free(ctx, evt.newChange);
            }
            if (evt.newValue) {
                fz_free(ctx, evt.newValue);
            }

            return rc;
        }
    );
}

bool JavaScriptEngine::commitTextFieldValue(
    const pdfe::core::Page* page, int widgetIndex,
    const std::string& value)
{
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<bool>(
        [pdfPage, widgetIndex, &value](fz_context* ctx) -> bool {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            // pdf_set_text_field_value runs /AA /K twice (the keystroke pass
            // with willCommit=0, then the commit pass with willCommit=1) and
            // only calls pdf_set_annot_field_value when both return rc != 0.
            // It catches internally and returns 0 on failure, so a rejected
            // or failed commit leaves /V exactly as it was.
            return pdf_set_text_field_value(ctx, widget, value.c_str()) != 0;
        }
    );
}

bool JavaScriptEngine::fireValidateEvent(
    const pdfe::core::Page* page, int widgetIndex,
    const std::string& value)
{
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<bool>(
        [pdfPage, widgetIndex, &value](fz_context* ctx) -> bool {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_document* pdfDoc = pdfPage->doc;
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);

            char* newValue = nullptr;
            int rc = pdf_field_event_validate(ctx, pdfDoc, fieldObj, value.c_str(), &newValue);

            // Free the allocated string if present
            if (newValue) {
                fz_free(ctx, newValue);
            }

            return rc != 0;
        }
    );
}

void JavaScriptEngine::fireCalculateEvent(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex](fz_context* ctx) {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_document* pdfDoc = pdfPage->doc;
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);

            pdf_field_event_calculate(ctx, pdfDoc, fieldObj);
        }
    );
}

std::string JavaScriptEngine::fireFormatEvent(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    return pImpl->context->executeReturn<std::string>(
        [pdfPage, widgetIndex](fz_context* ctx) -> std::string {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_document* pdfDoc = pdfPage->doc;
            pdf_obj* fieldObj = pdf_annot_obj(ctx, widget);

            char* result = pdf_field_event_format(ctx, pdfDoc, fieldObj);

            std::string formatted;
            if (result) {
                formatted = result;
                fz_free(ctx, result);
            }

            return formatted;
        }
    );
}

void JavaScriptEngine::fireFocusEvent(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex](fz_context* ctx) {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_annot_event_focus(ctx, widget);
        }
    );
}

void JavaScriptEngine::fireBlurEvent(const pdfe::core::Page* page, int widgetIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage, widgetIndex](fz_context* ctx) {
            pdf_annot* widget = Impl::getNthWidget(ctx, pdfPage, widgetIndex);
            if (!widget) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Widget index out of range: %d", widgetIndex);
            }

            pdf_annot_event_blur(ctx, widget);
        }
    );
}

void JavaScriptEngine::calculateAll(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_calculate_form(ctx, pdfDoc);
        }
    );
}

void JavaScriptEngine::fireWillSave(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_document_event_will_save(ctx, pdfDoc);
        }
    );
}

void JavaScriptEngine::fireDidSave(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute(
        [pdfPage](fz_context* ctx) {
            pdf_document* pdfDoc = pdfPage->doc;
            pdf_document_event_did_save(ctx, pdfDoc);
        }
    );
}

void JavaScriptEngine::fireWillPrint(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([pdfPage](fz_context* ctx) {
        pdf_document_event_will_print(ctx, pdfPage->doc);
    });
}

void JavaScriptEngine::fireDidPrint(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([pdfPage](fz_context* ctx) {
        pdf_document_event_did_print(ctx, pdfPage->doc);
    });
}

void JavaScriptEngine::fireWillClose(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    pdf_page* pdfPage = pImpl->getPdfPage(page);
    pImpl->context->execute([pdfPage](fz_context* ctx) {
        pdf_document_event_will_close(ctx, pdfPage->doc);
    });
}

void JavaScriptEngine::fireDocumentEvent(const std::string& eventName,
                                         const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }
    if (eventName == "willSave")       { fireWillSave(page);  return; }
    if (eventName == "didSave")        { fireDidSave(page);   return; }
    if (eventName == "willPrint")      { fireWillPrint(page); return; }
    if (eventName == "didPrint")       { fireDidPrint(page);  return; }
    if (eventName == "willClose")      { fireWillClose(page); return; }
    // Unknown event name: silently no-op (per AC #5(3) — must not crash).
}

} // namespace pdfe::forms
