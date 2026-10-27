#pragma once
// Meta-audit follow-up F15 / gap PDF-51.
//
// Annotation flags (/F, ISO 32000-1 table 165) used to travel through PDFE as
// a bare `int` with no named constant anywhere in src/ — so nothing in the
// product said which bit meant Hidden, nothing enforced Locked, and no test
// could name a flag to assert on it.  This header gives every flag a NAME and
// wraps the bitmask in a type, so an opaque `int` no longer appears on the
// annotation-flag path.
//
// Bit values are the spec masks; they are asserted equal to MuPDF's
// PDF_ANNOT_IS_* enumerators one-by-one in
// tests/test_annotation_flags.cpp::NamedConstantsMatchMuPdfEnumerators, so the
// two vocabularies can never silently drift.

#include <array>
#include <string>

namespace pdfe::annotations {

/// A single /F annotation flag.  The value is the spec bit MASK
/// (spec bit position N -> mask 1 << (N-1)).
enum class AnnotationFlag : int {
    Invisible      = 1 << 0,  // spec bit 1
    Hidden         = 1 << 1,  // spec bit 2
    Print          = 1 << 2,  // spec bit 3
    NoZoom         = 1 << 3,  // spec bit 4
    NoRotate       = 1 << 4,  // spec bit 5
    NoView         = 1 << 5,  // spec bit 6
    ReadOnly       = 1 << 6,  // spec bit 7
    Locked         = 1 << 7,  // spec bit 8
    ToggleNoView   = 1 << 8,  // spec bit 9
    LockedContents = 1 << 9,  // spec bit 10
};

/// Every flag this product knows about, in spec bit order.  Tests iterate
/// this to prove each flag round-trips INDIVIDUALLY.
inline constexpr std::array<AnnotationFlag, 10> kAllAnnotationFlags = {
    AnnotationFlag::Invisible,
    AnnotationFlag::Hidden,
    AnnotationFlag::Print,
    AnnotationFlag::NoZoom,
    AnnotationFlag::NoRotate,
    AnnotationFlag::NoView,
    AnnotationFlag::ReadOnly,
    AnnotationFlag::Locked,
    AnnotationFlag::ToggleNoView,
    AnnotationFlag::LockedContents,
};

/// Human-readable name of a flag (matches the spec key name).
inline const char* annotationFlagName(AnnotationFlag f) {
    switch (f) {
        case AnnotationFlag::Invisible:      return "Invisible";
        case AnnotationFlag::Hidden:         return "Hidden";
        case AnnotationFlag::Print:          return "Print";
        case AnnotationFlag::NoZoom:         return "NoZoom";
        case AnnotationFlag::NoRotate:       return "NoRotate";
        case AnnotationFlag::NoView:         return "NoView";
        case AnnotationFlag::ReadOnly:       return "ReadOnly";
        case AnnotationFlag::Locked:         return "Locked";
        case AnnotationFlag::ToggleNoView:   return "ToggleNoView";
        case AnnotationFlag::LockedContents: return "LockedContents";
    }
    return "Unknown";
}

/// Typed /F bitmask.  Construction from a raw int is deliberately explicit and
/// named (`fromRaw`) so the only places that speak raw bits are the MuPDF
/// boundary calls in AnnotationManager.
class AnnotationFlags {
public:
    constexpr AnnotationFlags() = default;
    constexpr AnnotationFlags(AnnotationFlag f)  // NOLINT(google-explicit-constructor)
        : bits_(static_cast<int>(f)) {}

    static constexpr AnnotationFlags fromRaw(int raw) {
        AnnotationFlags f;
        f.bits_ = raw;
        return f;
    }
    constexpr int toRaw() const { return bits_; }

    constexpr bool has(AnnotationFlag f) const {
        return (bits_ & static_cast<int>(f)) != 0;
    }
    constexpr AnnotationFlags& set(AnnotationFlag f, bool on = true) {
        if (on) {
            bits_ |= static_cast<int>(f);
        } else {
            bits_ &= ~static_cast<int>(f);
        }
        return *this;
    }
    constexpr AnnotationFlags& clear(AnnotationFlag f) { return set(f, false); }
    constexpr bool none() const { return bits_ == 0; }

    // Intent-revealing predicates used by the product's enforcement paths.
    constexpr bool isHidden() const { return has(AnnotationFlag::Hidden); }
    constexpr bool isNoView() const { return has(AnnotationFlag::NoView); }
    constexpr bool isPrintable() const { return has(AnnotationFlag::Print); }
    /// Geometry (position/size) must not be changed by the user.
    constexpr bool isGeometryLocked() const {
        return has(AnnotationFlag::Locked) || has(AnnotationFlag::ReadOnly);
    }
    /// /Contents (and other text) must not be changed by the user.
    constexpr bool isContentsLocked() const {
        return has(AnnotationFlag::LockedContents) || has(AnnotationFlag::ReadOnly);
    }

    friend constexpr bool operator==(AnnotationFlags a, AnnotationFlags b) {
        return a.bits_ == b.bits_;
    }
    friend constexpr bool operator!=(AnnotationFlags a, AnnotationFlags b) {
        return !(a == b);
    }

private:
    int bits_ = 0;
};

/// "Hidden|Locked" style rendering for diagnostics and test failure messages.
inline std::string describeAnnotationFlags(AnnotationFlags flags) {
    std::string out;
    for (AnnotationFlag f : kAllAnnotationFlags) {
        if (flags.has(f)) {
            if (!out.empty()) out += "|";
            out += annotationFlagName(f);
        }
    }
    return out.empty() ? std::string("(none)") : out;
}

} // namespace pdfe::annotations
