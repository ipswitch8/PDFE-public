#pragma once

#include <memory>
#include <string>

namespace pdfe::core {
    class MuPdfContext;
    class Document;
}

namespace pdfe::metadata {

struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string subject;
    std::string keywords;
    std::string creator;
    std::string producer;
    std::string creationDate;  // PDF date format strings
    std::string modDate;       // PDF date format strings
};

class MetadataManager {
public:
    explicit MetadataManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~MetadataManager();

    // Non-copyable
    MetadataManager(const MetadataManager&) = delete;
    MetadataManager& operator=(const MetadataManager&) = delete;

    DocumentMetadata readMetadata(pdfe::core::Document* doc);
    void writeMetadata(pdfe::core::Document* doc, const DocumentMetadata& metadata);
    std::string readField(pdfe::core::Document* doc, const std::string& key);
    void writeField(pdfe::core::Document* doc, const std::string& key, const std::string& value);
    void clearMetadata(pdfe::core::Document* doc);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::metadata
