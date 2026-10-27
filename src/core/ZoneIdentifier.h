// -*- coding: utf-8 -*-
//
// Mark-of-the-Web propagation (GitHub #107, WIN-96).
//
// Windows records a file's provenance in a `Zone.Identifier` NTFS alternate
// data stream. `ZoneId=3` means "came from the internet", and the rest of the
// platform acts on it: SmartScreen inspects the file, Office opens it in
// Protected View, and the shell warns before executing it.
//
// THE DEFECT THIS EXISTS TO FIX
//
// PDFE already WRITES a mark on files it extracts from a document
// (`AttachmentManager`'s markOfTheWeb, tested by
// ExtractedFilesCarryMarkOfTheWeb). What it did not do is PROPAGATE an existing
// mark on Save As: open a PDF that was downloaded from the internet, save it
// somewhere else, and the copy carried no Zone.Identifier at all. The user is
// then holding a file the platform believes is locally authored, produced from
// one it knew was not — a silent provenance downgrade performed by the editor.
//
// PROPAGATION, NOT MARKING. This deliberately does not stamp ZoneId=3 on
// everything. A PDF the user authored locally has no mark and must not acquire
// one, or every saved document would land in Protected View and the mark would
// stop meaning anything. The rule is: the destination ends up with whatever the
// source had, including nothing.
//
// BEST EFFORT, like the sibling in AttachmentManager. Alternate data streams
// do not exist on FAT32 or most network shares, and failing to copy a mark is
// not a reason to fail the user's save — the save has already succeeded by the
// time this runs. The return value distinguishes the cases so a caller (and a
// test) can tell "nothing to copy" from "tried and could not", which a bare
// bool would conflate.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pdfe::core {

/// Outcome of a propagation attempt. Reported rather than swallowed: a silent
/// failure here is a silent security downgrade, and this project's own record
/// says an effect that cannot be observed cannot be audited.
enum class ZonePropagation {
    NoSourceMark,   ///< source carried no Zone.Identifier — nothing to do
    Copied,         ///< the source's mark was written to the destination
    Failed          ///< the source had a mark and it could NOT be written
};

/// Read a file's `Zone.Identifier` stream, if it has one.
/// Returns nullopt when there is no such stream (the common case for a
/// locally-created file) or when the filesystem has no stream support.
std::optional<std::string> readZoneIdentifier(const std::filesystem::path& file);

/// Copy `from`'s Mark-of-the-Web to `to`, if `from` has one.
///
/// Never removes an existing mark on `to` when `from` has none — that would be
/// a downgrade in the other direction, and this function's contract is to
/// preserve provenance, not to normalise it.
ZonePropagation propagateZoneIdentifier(const std::filesystem::path& from,
                                        const std::filesystem::path& to);

}  // namespace pdfe::core
