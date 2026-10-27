#pragma once

#include "StructureTypes.h"
#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class StructureTreeBuilder {
public:
    explicit StructureTreeBuilder(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~StructureTreeBuilder();
    StructureTreeBuilder(const StructureTreeBuilder&) = delete;
    StructureTreeBuilder& operator=(const StructureTreeBuilder&) = delete;

    void buildTree(pdfe::core::Document* doc, const StructureNode& root);
    bool hasStructureTree(pdfe::core::Document* doc) const;
    void removeStructureTree(pdfe::core::Document* doc);
    void setDocumentLanguage(pdfe::core::Document* doc, const std::string& lang);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
