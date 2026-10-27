// -*- coding: utf-8 -*-
//
// UnencryptedWrapperProbe.h — PDF-05 / GitHub issue #3.
//
// WHAT AN UNENCRYPTED WRAPPER IS, AND WHY IT MATTERS
// --------------------------------------------------
// ISO 32000-2 §7.6.7. A PDF 2.0 *unencrypted wrapper document* is an
// UNENCRYPTED PDF whose only real job is to carry an encrypted payload as an
// attachment. Its catalog declares a `/Collection`, and one of its associated
// files is a file specification with `/AFRelationship /EncryptedPayload` whose
// embedded-file stream carries an `/EP` dictionary naming the cryptographic
// handler needed to open it.
//
// The visible page content is a COVER SHEET — conventionally "this document is
// protected, open it with X". The hazard is precisely that a viewer which does
// not implement the named handler renders that cover sheet perfectly happily
// and reports nothing wrong. The user sees a document that opened, reads a page
// that is not the document, and has no way to tell the difference. The wrapper
// exists so that a conforming reader can say "there is a payload here you
// cannot see"; a reader that stays silent produces exactly the confusion the
// construct was invented to prevent.
//
// PDFE HAD NO NOTION OF THIS AT ALL. `UnencryptedWrapper` and `EncryptMetadata`
// appeared nowhere in src/ before this file. So issue #3 is a PRODUCT gap, not
// a coverage gap: no test could have been written against behaviour that did
// not exist. The row says "detected AND reported", and the reporting half is
// the point — a detector whose answer nobody surfaces meets the letter of the
// criterion and none of its intent. That is why `summary` lives here, in the
// tested core, rather than being assembled ad hoc in the dialog that shows it.
//
// WHAT IS DELIBERATELY *NOT* CLAIMED. This does not decrypt the payload, does
// not verify the named handler exists, and does not judge whether the cover
// sheet is honest. It answers one question — "is the thing you are looking at
// a wrapper around content you are not seeing?" — and reports it.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

extern "C" {
typedef struct fz_context fz_context;
typedef struct pdf_document pdf_document;
}

namespace pdfe::core {

class MuPdfContext;

struct UnencryptedWrapperInfo {
    /// True only when an encrypted-payload attachment was found. A `/Collection`
    /// on its own is an ordinary PDF portfolio and is NOT reported as a
    /// wrapper — flagging every portfolio would train the user to dismiss the
    /// warning, which is worse than not showing it.
    bool isWrapper = false;

    /// The catalog declares `/Collection`. Recorded separately because it is
    /// the conventional (but not sufficient) marker, and because a payload
    /// found WITHOUT one is worth telling the user about too.
    bool hasCollection = false;

    /// An `/AFRelationship /EncryptedPayload` file specification was found.
    /// This is the decisive signal.
    bool hasEncryptedPayload = false;

    /// `/UF` (preferred) or `/F` from the payload's file specification. May be
    /// empty: the spec does not require a name.
    std::string payloadFileName;

    /// `/EP /Name` — the cryptographic filter the payload needs, e.g. a vendor
    /// handler name. Empty when the payload carries no `/EP` name.
    std::string cryptoFilterName;

    /// `/EP /V`, the encrypted-payload version. 0 when absent.
    int payloadVersion = 0;

    /// The message to show the user. NON-EMPTY exactly when `isWrapper` is
    /// true, and empty otherwise, so a caller cannot accidentally warn about
    /// an ordinary document by forgetting to check the flag.
    std::string summary;
};

/// Probe an ALREADY-OPEN document. Must be called from inside a MuPdfContext
/// execute()/executeReturn() scope — it calls into MuPDF, which reports
/// failure with fz_throw.
UnencryptedWrapperInfo probeUnencryptedWrapper(fz_context* ctx,
                                              pdf_document* doc);

/// Probe a file by path.
///
/// It opens the file itself rather than taking a `Document`, deliberately:
/// `Document`'s MuPDF handle is private to its friends, and widening that
/// friendship would edit `Document.h` — a header included almost everywhere,
/// for a question that is answered from the catalog alone. MuPDF parses
/// lazily, so this touches the trailer and the catalog and does not read the
/// page tree or any content stream.
///
/// Never throws: any failure to open or parse answers "not a wrapper". A probe
/// that is advisory must not be able to turn a readable document into an error.
UnencryptedWrapperInfo probeUnencryptedWrapper(
    const std::shared_ptr<MuPdfContext>& ctx,
    const std::filesystem::path& file);

}  // namespace pdfe::core
