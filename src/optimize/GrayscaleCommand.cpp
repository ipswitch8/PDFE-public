#include "GrayscaleCommand.h"

#include "GrayscaleConverter.h"
#include "core/Document.h"

namespace pdfe::optimize {

GrayscaleCommand::GrayscaleCommand(pdfe::core::Document* doc)
    : doc_(doc)
{
}

GrayscaleCommand::~GrayscaleCommand()
{
    // Each holder carries its own pinned context, so this is safe even if the
    // owning Document has already been destroyed (undo stack outliving the
    // Document — see DeletePageCommand::~DeletePageCommand, defect B2).
    GrayscaleConverter::releaseSnapshot(snapshot_);
}

void GrayscaleCommand::execute()
{
    if (!doc_) return;

    // redo() re-enters here; drop any prior snapshot before recapturing so a
    // re-execution never leaks a standalone pdf_document.
    GrayscaleConverter::releaseSnapshot(snapshot_);

    // Snapshot the ORIGINAL colour content BEFORE converting, so undo can put
    // it back. (After an undo+redo this recaptures the restored colour content,
    // which is again correct.)
    snapshot_ = GrayscaleConverter::snapshotPageContent(doc_);

    const auto r = GrayscaleConverter::convertDocumentToGrayscale(doc_);
    pagesConverted_ = r.pagesConverted;
    pagesFailed_ = r.pagesFailed;
    ok_ = r.ok;
    errorMessage_ = r.errorMessage;
    executed_ = true;
}

void GrayscaleCommand::undo()
{
    if (!executed_ || !doc_) return;
    GrayscaleConverter::restorePageContent(doc_, snapshot_);
}

void GrayscaleCommand::redo()
{
    execute();
}

std::string GrayscaleCommand::description() const
{
    return "Convert to Grayscale";
}

} // namespace pdfe::optimize
