// -*- coding: utf-8 -*-
#pragma once

// The two DocMDP vocabulary enums, in a header that includes NOTHING.
//
// F24 REMEDIATION (security-audit gate, finding 1) split these out. They used
// to live in SignatureData.h (CertificationLevel) and DocMdpEnforcer.h
// (DocMdpChangeClass), both of which reach <QRectF> and therefore drag in Qt's
// `emit` keyword MACRO. The four content-editing engines that now consult the
// DocMDP direct-write guard — pdfe_batch, pdfe_headerfooter, pdfe_optimize,
// pdfe_reflow — are otherwise pure MuPDF, and one of them (HeaderFooterEngine)
// has a local named `emit` that Qt's macro silently mangles.
//
// The alternative was to mirror the enums in a Qt-free header and pin the
// mirror with static_asserts. A single definition beats a policed duplicate:
// there is nothing to drift. SignatureData.h and DocMdpEnforcer.h include this
// file, so every existing include path still resolves both names unchanged.

namespace pdfe::signatures {

/// Level of restriction a certification signature places on downstream
/// edits (maps to PDF /Perms /DocMDP /P). NotCertification means the
/// signature is an ordinary "approval" signature, not a certification
/// one.
///
/// Underlying type pinned explicitly: the value IS the /P integer, and
/// static_cast between the two is relied on in both directions.
enum class CertificationLevel : int {
    NotCertification = 0,  ///< Regular signature, no /DocMDP.
    NoChanges        = 1,  ///< No changes permitted after signing.
    FormFillOnly     = 2,  ///< Form filling + signing allowed.
    AnnotationsAllowed = 3,///< Form + annotations + signing allowed.
};

/// What kind of change an object revised after the certifying signature
/// represents. The class, not the object, is what the /P level permits or
/// forbids. See DocMdpEnforcer::changePermittedAt for the matrix
/// (ISO 32000-1 Table 254).
enum class DocMdpChangeClass : int {
    /// Filling in an existing form field: the field dictionary's /V, its
    /// widget, or the appearance stream regenerated for it. Permitted at
    /// P2 and P3.
    FormFieldValue,
    /// A non-widget annotation added, modified or removed (and the page
    /// /Annots array edit that accompanies it). Permitted at P3 only.
    Annotation,
    /// A new or updated signature: signing is explicitly permitted at P2
    /// and P3.
    Signature,
    /// Anything else — page content streams, the page tree, the catalog,
    /// resources, fonts. Never permitted at any certification level. This is
    /// what every direct-write CONTENT-EDITING engine declares.
    Other,
};

} // namespace pdfe::signatures
