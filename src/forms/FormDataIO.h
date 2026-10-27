#pragma once
#include <map>
#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::forms {

class FormManager;

class FormDataIO {
public:
    explicit FormDataIO(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~FormDataIO();

    FormDataIO(const FormDataIO&) = delete;
    FormDataIO& operator=(const FormDataIO&) = delete;
    FormDataIO(FormDataIO&&) noexcept;
    FormDataIO& operator=(FormDataIO&&) noexcept;

    /// Export form data to XFDF format string.
    std::string exportToXfdf(pdfe::core::Document* doc, FormManager* manager);

    /// Import form data from XFDF string.
    void importFromXfdf(pdfe::core::Document* doc, FormManager* manager,
                        const std::string& xfdfContent);

    /// F15 / PDF-40: import form data from an FDF (Forms Data Format) file.
    ///
    /// FDF is PDF *syntax*: the catalog's /FDF dictionary holds a /Fields
    /// array of field dictionaries carrying /T (partial name), /V (value) and
    /// optional /Kids. Fully-qualified names are built by joining the /T chain
    /// with '.', matching the AcroForm naming rules.
    ///
    /// SECURITY: an FDF is untrusted input. This importer reads ONLY field
    /// names and values. /JavaScript, /F (target file specification), /ID and
    /// any embedded action dictionaries are ignored — nothing in an FDF can
    /// make PDFE execute script or touch another file.
    ///
    /// @throws core::PdfeException when the content is not an FDF, cannot be
    ///         parsed, or carries no /FDF /Fields array.
    void importFromFdf(pdfe::core::Document* doc, FormManager* manager,
                       const std::string& fdfContent);

private:
    /// Shared tail of both importers: push name->value pairs into the
    /// document's widgets via FormManager::setFieldValue.
    void applyFieldValues(pdfe::core::Document* doc, FormManager* manager,
                          const std::map<std::string, std::string>& fieldValues);

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::forms
