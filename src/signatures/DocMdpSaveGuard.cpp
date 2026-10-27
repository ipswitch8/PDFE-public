// -*- coding: utf-8 -*-
#include "DocMdpSaveGuard.h"

#include "DocMdpEnforcer.h"
#include "core/Document.h"

#include <string>
#include <utility>

namespace pdfe::signatures {

namespace {

/// Reference count so nested/overlapping installers (two MainWindows in the
/// same process, a test wrapping a UI fixture) cannot uninstall each other's
/// guard. Not thread-safe by design: installation happens on the UI thread at
/// window construction, never concurrently.
int g_installCount = 0;

std::string levelName(CertificationLevel level) {
    switch (level) {
        case CertificationLevel::NoChanges:
            return "/P 1 (no changes permitted)";
        case CertificationLevel::FormFillOnly:
            return "/P 2 (form filling and signing permitted)";
        case CertificationLevel::AnnotationsAllowed:
            return "/P 3 (form filling, signing and annotations permitted)";
        case CertificationLevel::NotCertification:
            break;
    }
    return "not certified";
}

} // namespace

void installDocMdpSaveGuard(std::shared_ptr<pdfe::core::MuPdfContext> context) {
    ++g_installCount;
    pdfe::core::Document::setSaveGuard(
        [context = std::move(context)](
            const pdfe::core::Document* doc,
            const std::filesystem::path& target,
            bool inPlace) -> pdfe::core::Document::SaveGuardVerdict {
            pdfe::core::Document::SaveGuardVerdict verdict;
            if (!doc || !context) return verdict;

            DocMdpEnforcer enforcer(context);
            const DocMdpStatus status = enforcer.evaluate(doc);

            // Not certified, or certified and still intact at its own level:
            // nothing to say. This is the branch that keeps the guard a /P-AWARE
            // control instead of a blanket block on certified documents — a
            // /P 2 form fill and a /P 3 annotation land here.
            if (!status.certified) return verdict;
            if (!status.violated && !status.fieldLockViolated) return verdict;

            std::string what = status.message;
            if (what.empty()) {
                what = "This document's certification (" + levelName(status.level)
                     + ") is broken by a change made since it was certified.";
            }

            if (inPlace) {
                verdict.allow = false;
                verdict.message =
                    "Refusing to overwrite a certified document whose "
                    "certification is broken.\n\n" + what
                    + "\n\nCertification level: " + levelName(status.level)
                    + ".\nUse Save As to write these changes to a DIFFERENT "
                      "file; the certified original will be left intact.";
            } else {
                verdict.allow = true;
                verdict.message =
                    "The saved copy does NOT satisfy the certification it "
                    "carries.\n\n" + what
                    + "\n\nCertification level: " + levelName(status.level)
                    + ".\nThe original file at its certified state is "
                      "unchanged; \"" + target.filename().string()
                    + "\" will be reported as modified since certification by "
                      "any validating reader.";
            }
            return verdict;
        });
}

void uninstallDocMdpSaveGuard() {
    if (g_installCount > 0) --g_installCount;
    if (g_installCount == 0) {
        pdfe::core::Document::setSaveGuard(pdfe::core::Document::SaveGuard{});
    }
}

} // namespace pdfe::signatures
