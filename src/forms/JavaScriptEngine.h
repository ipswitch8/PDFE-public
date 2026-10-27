#pragma once

#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Page;
}

namespace pdfe::forms {

/// JavaScript engine wrapper for PDF form calculations and events.
///
/// This class wraps MuPDF's bundled MuJS JavaScript engine, providing
/// high-level methods to enable JavaScript support and fire form field
/// events (keystroke, validate, calculate, format, focus, blur) as well
/// as document-level events (willSave, didSave).
///
/// JavaScript must be explicitly enabled via enable() before any events
/// can be fired. Most methods require a valid Page pointer and widget index.
class JavaScriptEngine {
public:
    explicit JavaScriptEngine(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~JavaScriptEngine();

    // Non-copyable
    JavaScriptEngine(const JavaScriptEngine&) = delete;
    JavaScriptEngine& operator=(const JavaScriptEngine&) = delete;

    // Movable
    JavaScriptEngine(JavaScriptEngine&&) noexcept;
    JavaScriptEngine& operator=(JavaScriptEngine&&) noexcept;

    /// Enable JavaScript support for the PDF document associated with the page.
    /// @param page A valid page from the document to enable JavaScript for.
    void enable(const pdfe::core::Page* page);

    /// Check if JavaScript is currently enabled.
    /// @return true if JavaScript has been enabled via enable().
    bool isEnabled() const;

    /// Fire a keystroke event on a form field.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    /// @param value The current value of the field.
    /// @param change The text being inserted/changed.
    /// @param selStart The start of the selection range.
    /// @param selEnd The end of the selection range.
    /// @param willCommit Whether this keystroke will commit the value.
    /// @return Non-zero if the keystroke was accepted, zero if rejected.
    int fireKeystrokeEvent(const pdfe::core::Page* page, int widgetIndex,
                            const std::string& value, const std::string& change,
                            int selStart, int selEnd, bool willCommit);

    /// Commit a text field value THROUGH the field's Keystroke action.
    ///
    /// PDF-46 / GitHub #17. FormManager::setFieldValue deliberately passes
    /// ignore_trigger_events=1, so it writes /V unconditionally and no
    /// /AA /K script ever sees the input. That is right for a programmatic
    /// fill (FDF import, undo) and wrong for typed/pasted input, which the
    /// spec says a Keystroke action may reject or rewrite. This is the other
    /// path: MuPDF runs the per-keystroke pass, then the willCommit pass, and
    /// writes /V only if BOTH accept — so a rejected value leaves the field
    /// untouched and a rewritten one is stored as the script rewrote it.
    ///
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    /// @param value The value the user is trying to commit.
    /// @return true if the keystroke action accepted the value and it was
    ///         written to /V; false if it was rejected (field unchanged).
    bool commitTextFieldValue(const pdfe::core::Page* page, int widgetIndex,
                              const std::string& value);

    /// Fire a validation event on a form field.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    /// @param value The value to validate.
    /// @return true if the value is valid, false otherwise.
    bool fireValidateEvent(const pdfe::core::Page* page, int widgetIndex,
                           const std::string& value);

    /// Fire a calculate event on a form field.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    void fireCalculateEvent(const pdfe::core::Page* page, int widgetIndex);

    /// Fire a format event on a form field and retrieve the formatted value.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    /// @return The formatted value string.
    std::string fireFormatEvent(const pdfe::core::Page* page, int widgetIndex);

    /// Fire a focus event when a widget receives focus.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    void fireFocusEvent(const pdfe::core::Page* page, int widgetIndex);

    /// Fire a blur event when a widget loses focus.
    /// @param page The page containing the widget.
    /// @param widgetIndex The zero-based index of the widget on the page.
    void fireBlurEvent(const pdfe::core::Page* page, int widgetIndex);

    /// Calculate all form fields in the document (runs all calculate scripts).
    /// @param page Any page from the document.
    void calculateAll(const pdfe::core::Page* page);

    /// Fire a document-level "will save" event.
    /// @param page Any page from the document.
    void fireWillSave(const pdfe::core::Page* page);

    /// Fire a document-level "did save" event.
    /// @param page Any page from the document.
    void fireDidSave(const pdfe::core::Page* page);

    /// Fire a document-level "will print" event.
    void fireWillPrint(const pdfe::core::Page* page);

    /// Fire a document-level "did print" event.
    void fireDidPrint(const pdfe::core::Page* page);

    /// Fire a document-level "will close" event.
    void fireWillClose(const pdfe::core::Page* page);

    /// Generic dispatcher: fire a document-level event by name.
    /// Recognized names: "willSave", "didSave", "willPrint", "didPrint",
    /// "willClose". Unknown names are silently ignored (no throw).
    /// JavaScript must already be enabled on the document (via enable())
    /// for any script to actually execute; if not enabled, this still
    /// invokes MuPDF's event hooks but they become no-ops.
    void fireDocumentEvent(const std::string& eventName,
                           const pdfe::core::Page* page);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::forms
