#pragma once

#include <cstdint>
#include <string>

namespace pdfe::attachments {

/// Metadata for a single embedded file entry from /Names /EmbeddedFiles.
struct AttachmentEntry {
    std::string name;          // display name (key in the /EmbeddedFiles name tree)
    std::string filename;      // /UF or /F from the filespec (user-visible filename)
    std::string mimeType;      // /Subtype
    std::int64_t size = -1;    // bytes; -1 if unknown
    std::int64_t created = -1; // Unix epoch; -1 if unknown
    std::int64_t modified = -1;
};

} // namespace pdfe::attachments
