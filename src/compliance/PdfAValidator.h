#pragma once

#include "ComplianceTypes.h"
#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class PdfAValidator {
public:
    explicit PdfAValidator(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~PdfAValidator();
    PdfAValidator(const PdfAValidator&) = delete;
    PdfAValidator& operator=(const PdfAValidator&) = delete;

    ComplianceResult validate(pdfe::core::Document* doc, PdfALevel level);
    bool isCompliant(pdfe::core::Document* doc, PdfALevel level);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
