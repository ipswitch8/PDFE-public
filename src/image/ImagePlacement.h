// -*- coding: utf-8 -*-
#pragma once

// Placement geometry shared by the image editor, enumeration and the image
// tool (docs/image-placement-model.md, GitHub #417).
//
// A placement maps the image's unit square through an affine matrix. Matrices
// here use PDF's ROW-VECTOR convention, the same as fz_matrix / `cm`:
//
//     x' = a*x + c*y + e
//     y' = b*x + d*y + f
//
// and concat(A, B) means "apply A, then B" (fz_concat). Row 0 (a, b) is where
// the image's own x axis (unit u) goes, row 1 (c, d) where its own y axis
// (unit v) goes.
//
// Two frames appear:
//   * PDF user space: Y-up, as written in content streams;
//   * the RENDERED page frame, fitz device space: Y-down, origin at the
//     rendered page's top-left, page /Rotate and box origin applied
//     (pdf_page_transform). This is the frame mouse input, PageImageInfo
//     boundingBox and the canvas test seams share (GitHub #373/#376).
// The decomposition below (rotation, flips, skew) is taken in the rendered
// frame with Y flipped back to Y-up, so a positive angle is visually
// counter-clockwise, as PageImageInfo::rotationRadians always was.

#include <QPointF>
#include <QRectF>
#include <array>
#include <cmath>

namespace pdfe::image::placement {

struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    [[nodiscard]] QPointF apply(double x, double y) const {
        return QPointF(a * x + c * y + e, b * x + d * y + f);
    }
    [[nodiscard]] QPointF apply(QPointF p) const { return apply(p.x(), p.y()); }
    [[nodiscard]] double det() const { return a * d - b * c; }
    [[nodiscard]] std::array<float, 6> toArray() const {
        return {float(a), float(b), float(c), float(d), float(e), float(f)};
    }
};

/// "Apply A, then B" (fz_concat semantics).
inline Affine concat(const Affine& A, const Affine& B) {
    Affine r;
    r.a = A.a * B.a + A.b * B.c;
    r.b = A.a * B.b + A.b * B.d;
    r.c = A.c * B.a + A.d * B.c;
    r.d = A.c * B.b + A.d * B.d;
    r.e = A.e * B.a + A.f * B.c + B.e;
    r.f = A.e * B.b + A.f * B.d + B.f;
    return r;
}

/// Inverse; returns false (and leaves `out` untouched) for a singular matrix.
inline bool invert(const Affine& m, Affine& out) {
    const double det = m.det();
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    Affine r;
    r.a = m.d / det;
    r.b = -m.b / det;
    r.c = -m.c / det;
    r.d = m.a / det;
    r.e = -(m.e * r.a + m.f * r.c);
    r.f = -(m.e * r.b + m.f * r.d);
    out = r;
    return true;
}

/// Crop in the image's unit square, [u0, v0, u1, v1] within [0, 1]^2.
/// v is the image's own y axis: v = 0 is the image's BOTTOM row as drawn.
struct UnitCrop {
    double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    [[nodiscard]] bool isFull() const {
        constexpr double eps = 1e-6;
        return u0 <= eps && v0 <= eps && u1 >= 1 - eps && v1 >= 1 - eps;
    }
    [[nodiscard]] bool isEmpty() const { return u1 - u0 <= 1e-6 || v1 - v0 <= 1e-6; }
    [[nodiscard]] UnitCrop intersected(const UnitCrop& o) const {
        UnitCrop r;
        r.u0 = std::max(u0, o.u0);
        r.v0 = std::max(v0, o.v0);
        r.u1 = std::min(u1, o.u1);
        r.v1 = std::min(v1, o.v1);
        return r;
    }
    [[nodiscard]] UnitCrop clamped() const {
        auto cl = [](double x) { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); };
        return UnitCrop{cl(u0), cl(v0), cl(u1), cl(v1)};
    }
};

/// The decomposition of a placement's linear part in the rendered frame.
///
///   rotation: angle of the image's own x axis, CCW-positive in the Y-up
///             rendered frame (= visually counter-clockwise).
///   flips:    a placement with a negative determinant is mirrored. The same
///             matrix is both "flip X then rotate by r" and "flip Y then rotate
///             by r + 180"; the decomposition picks whichever gives the
///             smaller |rotation|, so a plain mirror reads as flipX at 0 and
///             a plain vertical flip as flipY at 0 (neither reads as 180).
///             Past 90 degrees the reported pair switches over; the matrix,
///             and therefore what is drawn, is the same either way.
///   width/height: lengths of the image's own x and y axes (the FULL,
///             uncropped placement size).
///   skew:     the angle between the image's axes minus 90 degrees (0 for a
///             rectangle). The editor keeps it by working from the full
///             matrix; it is reported, never rebuilt from.
struct Decomposition {
    double rotation = 0;
    bool flipX = false;
    bool flipY = false;
    double width = 0;
    double height = 0;
    double skew = 0;
};

inline Decomposition decompose(const Affine& fitzMatrix) {
    // Y-up rendered frame: negate the y components (b, d).
    const double a = fitzMatrix.a, b = -fitzMatrix.b, c = fitzMatrix.c, d = -fitzMatrix.d;
    Decomposition r;
    r.width = std::hypot(a, b);
    r.height = std::hypot(c, d);
    const double det = a * d - b * c;
    if (det >= 0) {
        r.rotation = std::atan2(b, a);
    } else {
        const double viaX = std::atan2(-b, -a);  // flip the image's x axis first
        const double viaY = std::atan2(b, a);    // flip the image's y axis first
        if (std::abs(viaX) <= std::abs(viaY) + 1e-9) {
            r.flipX = true;
            r.rotation = viaX;
        } else {
            r.flipY = true;
            r.rotation = viaY;
        }
    }
    if (r.width > 0 && r.height > 0) {
        const double cosAxes = (a * c + b * d) / (r.width * r.height);
        const double between = std::acos(std::max(-1.0, std::min(1.0, cosAxes)));
        r.skew = 1.5707963267948966 - between;
    }
    return r;
}

/// The linear part of `fitzMatrix` re-oriented to rotation `newRotation`
/// (same convention as Decomposition::rotation) and re-scaled so the image's
/// own axes have lengths newWidth / newHeight. Flips and skew are kept: the
/// matrix is written K * R(newRotation) where K = L * R(-currentRotation) is
/// the placement's own-axes part (scale, flip, skew), scaled row-wise.
/// Translation is left at 0. Returns false for a degenerate matrix.
inline bool reorient(const Affine& fitzMatrix, double newRotation,
                     double newWidth, double newHeight, Affine& out) {
    const Decomposition cur = decompose(fitzMatrix);
    if (cur.width <= 1e-9 || cur.height <= 1e-9) return false;
    // Y-up linear part.
    const double a = fitzMatrix.a, b = -fitzMatrix.b, c = fitzMatrix.c, d = -fitzMatrix.d;
    // K = L * R(-cur): R(t) = [cos t, sin t; -sin t, cos t] (row-vector CCW).
    const double cs = std::cos(-cur.rotation), sn = std::sin(-cur.rotation);
    double ka = a * cs - b * sn, kb = a * sn + b * cs;
    double kc = c * cs - d * sn, kd = c * sn + d * cs;
    const double sx = newWidth / cur.width, sy = newHeight / cur.height;
    ka *= sx; kb *= sx;
    kc *= sy; kd *= sy;
    const double cn = std::cos(newRotation), sw = std::sin(newRotation);
    Affine r;
    r.a = ka * cn - kb * sw;
    r.b = ka * sw + kb * cn;
    r.c = kc * cn - kd * sw;
    r.d = kc * sw + kd * cn;
    // Back to the Y-down rendered frame.
    r.b = -r.b;
    r.d = -r.d;
    r.e = 0;
    r.f = 0;
    out = r;
    return true;
}

/// Lengths of the image's own axes under `m`.
inline double axisLengthX(const Affine& m) { return std::hypot(m.a, m.b); }
inline double axisLengthY(const Affine& m) { return std::hypot(m.c, m.d); }

/// Centre of the visible (cropped) region, in the matrix's output frame.
inline QPointF visibleCentre(const Affine& m, const UnitCrop& crop) {
    return m.apply((crop.u0 + crop.u1) / 2.0, (crop.v0 + crop.v1) / 2.0);
}

/// Rotate `p` about `centre` by `radians` (math convention of the frame the
/// points are in).
inline QPointF rotateAbout(QPointF p, QPointF centre, double radians) {
    const double cs = std::cos(radians), sn = std::sin(radians);
    const QPointF dlt = p - centre;
    return centre + QPointF(dlt.x() * cs - dlt.y() * sn, dlt.x() * sn + dlt.y() * cs);
}

} // namespace pdfe::image::placement
