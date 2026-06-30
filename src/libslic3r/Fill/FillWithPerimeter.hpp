///|/ Copyright (c) Prusa Research 2016 - 2023 Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Vojtěch Král @vojtechkral
///|/ Copyright (c) SuperSlicer 2019 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FillWithPerimeter_hpp_
#define slic3r_FillWithPerimeter_hpp_

#include "FillBase.hpp"

#include <memory>

namespace Slic3r {

// Composite filler adding one or more contour loops before delegating the remaining area
// to another fill pattern.  It is used by the rectilinear-with-perimeter pattern and by
// support paths that need dense interface islands with explicit internal contours.
class FillWithPerimeter : public Fill
{
public:
    // Controls how much the outer contour centerline is pulled toward the source surface.
    // 0 keeps the historical inset, 1 places it at the nominal half-spacing inset.
    float external_perimeter_encroachment = 0.f;
    std::unique_ptr<Fill> infill{ nullptr };
    float ratio_fill_inside = 0.f;
    // Fraction of spacing overlapped between the innermost contour and the wrapped infill.
    // 0.5 preserves the historical half-spacing encroachment, 0 removes it.
    float perimeter_infill_encroachment = 0.5f;
    // Number of contour loops generated before the wrapped fill. Zero keeps only the wrapped fill.
    unsigned int perimeter_count = 1;

    FillWithPerimeter() : Fill() {}
    FillWithPerimeter(Fill *parent, unsigned int perimeter_count = 1) :
        Fill(),
        external_perimeter_encroachment(0.f),
        infill(parent),
        ratio_fill_inside(0.f),
        perimeter_infill_encroachment(0.5f),
        perimeter_count(perimeter_count)
    {}
    FillWithPerimeter(const FillWithPerimeter &o) :
        Fill(o),
        external_perimeter_encroachment(o.external_perimeter_encroachment),
        infill(o.infill.get() ? o.infill->clone() : nullptr),
        ratio_fill_inside(o.ratio_fill_inside),
        perimeter_infill_encroachment(o.perimeter_infill_encroachment),
        perimeter_count(o.perimeter_count)
    {}
    Fill *clone() const override { return new FillWithPerimeter(*this); }
    ~FillWithPerimeter() override = default;

    void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) const override;
};

} // namespace Slic3r

#endif // slic3r_FillWithPerimeter_hpp_
