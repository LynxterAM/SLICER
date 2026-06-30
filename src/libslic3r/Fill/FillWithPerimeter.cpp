///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Tomáš Mészáros @tamasmeszaros, Lukáš Matěna @lukasmatena
///|/ Copyright (c) SuperSlicer 2018 - 2019 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

// FillWithPerimeter wraps another infill generator and emits contour loops before
// the wrapped fill.  The implementation is isolated from FillBase so callers that
// only need the generic Fill API do not depend on this composite pattern.

#include "FillWithPerimeter.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Surface.hpp"

#include <algorithm>
#include <cassert>

#include <boost/log/trivial.hpp>

namespace Slic3r {

struct FillWithPerimeterContext
{
    const FillWithPerimeter &fill;
    const Surface           *surface;
    const FillParams        &params;
    coord_t                  perimeter_regularization;
    float                    infill_encroachment;
    ExtrusionRole            role;
};

// Transfers ownership to dst if child contains extrusion entities; deletes it otherwise.
static bool fill_with_perimeter_append_non_empty_collection(
    ExtrusionEntityCollection *child,
    ExtrusionEntityCollection &dst);

// Adds one or more available shell areas.  Multiple areas are wrapped in a sortable island collection.
static bool fill_with_perimeter_add_shells(
    const FillWithPerimeterContext &context,
    ExPolygons                    &&shells,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &dst,
    bool                             wrap_single_shell);

// Adds one available shell area to a branch collection, dispatching to perimeter or infill generation.
static bool fill_with_perimeter_add_shell(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch);

// Adds one shell as a non-sortable branch, preserving perimeter-before-infill order inside the island.
static bool fill_with_perimeter_add_shell_branch(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &dst);

// Creates perimeter centerlines for a shell, then recurses on the remaining inner shell areas.
static bool fill_with_perimeter_add_perimeter(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch);

// Adds an already-computed perimeter centerline, then continues with its inner shell.
static bool fill_with_perimeter_add_perimeter_centerline(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&perimeter_expolygon,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch);

// Appends the contour and holes of one perimeter centerline to the current branch collection.
static bool fill_with_perimeter_append_perimeter_paths(
    const FillWithPerimeterContext &context,
    ExPolygon                      &perimeter_expolygon,
    ExtrusionEntityCollection       &branch);

// Appends the wrapped fill into a terminal shell as the final item of a branch.
static bool fill_with_perimeter_add_infill(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell,
    ExtrusionEntityCollection       &branch);

// Builds the centerline polygons for one contour level from an available shell area.
static ExPolygons fill_with_perimeter_perimeter_expolygons(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell);

// Builds infill surfaces from a terminal available shell area.
static ExPolygons fill_with_perimeter_infill_expolygons(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell);

void FillWithPerimeter::fill_surface_extrusion(const Surface *surface,
                                               const FillParams &params,
                                               ExtrusionEntitiesPtr &out) const {
    assert(infill != nullptr);
    if (infill == nullptr)
        return;

    // The wrapped fill must see the same layer, angle, spacing and config state as this composite fill.
    *infill = *this;

    if (perimeter_count == 0) {
        infill->fill_surface_extrusion(surface, params, out);
        return;
    }

    ExtrusionEntityCollection *eecroot = new ExtrusionEntityCollection();
    // You don't want to sort the extrusions: big infill first, small second.
    eecroot->set_can_sort_reverse(true, true);

    // Each island owns one non-sortable branch.  When offsets split an island,
    // only the collection between the resulting islands is sortable.
    const float external_encroachment = std::max(0.f, std::min(1.f, external_perimeter_encroachment));
    const float infill_encroachment = std::max(0.f, std::min(0.5f, perimeter_infill_encroachment));
    const coord_t perimeter_regularization = scale_d(this->get_spacing() * (1.f - external_encroachment) / 4.f);
    const FillWithPerimeterContext context{*this,
                                           surface,
                                           params,
                                           perimeter_regularization,
                                           infill_encroachment,
                                           getRoleFromSurfaceType(params, surface)};
    fill_with_perimeter_add_shells(context, ExPolygons{surface->expolygon}, perimeter_count, *eecroot, true);

    if (!eecroot->empty()) {
        double mult_flow = 1;
        if (!params.dont_adjust && params.full_infill() && !params.flow.bridge() && params.fill_exactly) {
            const double extruded_volume = ExtrusionVolume{}.get(*eecroot);
            const double polyline_volume = compute_unscaled_volume_to_fill(surface, params);
            if (extruded_volume != 0 && polyline_volume != 0)
                mult_flow = polyline_volume / extruded_volume;
            if (mult_flow > 1.3)
                mult_flow = 1.3;
            if (mult_flow < 0.8)
                mult_flow = 0.8;
            BOOST_LOG_TRIVIAL(info) << "rectilinear/monotonic Infill (with gapfil) process extrude "
                                    << extruded_volume << " mm3 for a volume of " << polyline_volume
                                    << " mm3 : we mult the flow by " << mult_flow;
#if _DEBUG
            this->debug_verify_flow_mult = mult_flow;
#endif
        }
        mult_flow *= params.flow_mult;
        if (mult_flow != 1)
            ExtrusionModifyFlow{mult_flow}.set(*eecroot);
    } else {
#if _DEBUG
        this->debug_verify_flow_mult = -1;
#endif
    }

    if (!eecroot->empty())
        out.push_back(eecroot);
    else
        delete eecroot;
}

static bool fill_with_perimeter_append_non_empty_collection(
    ExtrusionEntityCollection *child,
    ExtrusionEntityCollection &dst)
{
    if (!child->empty()) {
        dst.append(ExtrusionEntitiesPtr{child});
        return true;
    }

    delete child;
    return false;
}

static bool fill_with_perimeter_add_shells(
    const FillWithPerimeterContext &context,
    ExPolygons                    &&shells,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &dst,
    bool                             wrap_single_shell)
{
    if (shells.empty())
        return false;

    if (shells.size() == 1) {
        if (wrap_single_shell)
            return fill_with_perimeter_add_shell_branch(context, std::move(shells.front()), remaining_perimeters, dst);
        return fill_with_perimeter_add_shell(context, std::move(shells.front()), remaining_perimeters, dst);
    }

    ExtrusionEntityCollection *islands = new ExtrusionEntityCollection();
    islands->set_can_sort_reverse(true, true);
    for (ExPolygon &shell : shells)
        fill_with_perimeter_add_shell_branch(context, std::move(shell), remaining_perimeters, *islands);

    return fill_with_perimeter_append_non_empty_collection(islands, dst);
}

static bool fill_with_perimeter_add_shell(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch)
{
    shell.assert_valid();
    if (remaining_perimeters > 0)
        return fill_with_perimeter_add_perimeter(context, std::move(shell), remaining_perimeters, branch);
    return fill_with_perimeter_add_infill(context, shell, branch);
}

static bool fill_with_perimeter_add_shell_branch(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &dst)
{
    ExtrusionEntityCollection *branch = new ExtrusionEntityCollection();
    branch->set_can_sort_reverse(false, false);
    fill_with_perimeter_add_shell(context, std::move(shell), remaining_perimeters, *branch);
    return fill_with_perimeter_append_non_empty_collection(branch, dst);
}

static bool fill_with_perimeter_add_perimeter(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&shell,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch)
{
    ExPolygons perimeters = fill_with_perimeter_perimeter_expolygons(context, shell);
    if (perimeters.empty())
        return false;

    if (perimeters.size() == 1)
        return fill_with_perimeter_add_perimeter_centerline(context, std::move(perimeters.front()), remaining_perimeters, branch);

    ExtrusionEntityCollection *islands = new ExtrusionEntityCollection();
    islands->set_can_sort_reverse(true, true);
    for (ExPolygon &perimeter : perimeters) {
        ExtrusionEntityCollection *island_branch = new ExtrusionEntityCollection();
        island_branch->set_can_sort_reverse(false, false);
        fill_with_perimeter_add_perimeter_centerline(context, std::move(perimeter), remaining_perimeters, *island_branch);
        fill_with_perimeter_append_non_empty_collection(island_branch, *islands);
    }

    return fill_with_perimeter_append_non_empty_collection(islands, branch);
}

static bool fill_with_perimeter_add_perimeter_centerline(
    const FillWithPerimeterContext &context,
    ExPolygon                     &&perimeter_expolygon,
    unsigned int                    remaining_perimeters,
    ExtrusionEntityCollection       &branch)
{
    perimeter_expolygon.assert_valid();
    if (!fill_with_perimeter_append_perimeter_paths(context, perimeter_expolygon, branch))
        return false;

    // The next available shell starts half a spacing inside the current
    // centerline.  Single shells continue in the same branch; split shells get
    // their own sortable island collection.
    ExPolygons inner_shells = offset_ex(perimeter_expolygon, scale_d(-context.fill.get_spacing() / 2.));
    ensure_valid(inner_shells);
    fill_with_perimeter_add_shells(context, std::move(inner_shells), remaining_perimeters - 1, branch, false);
    return true;
}

static bool fill_with_perimeter_append_perimeter_paths(
    const FillWithPerimeterContext &context,
    ExPolygon                      &perimeter_expolygon,
    ExtrusionEntityCollection       &branch)
{
    perimeter_expolygon.contour.make_counter_clockwise();
    Polylines polylines_perimeter = {perimeter_expolygon.contour.split_at_index(0)};
    for (Polygon hole : perimeter_expolygon.holes) {
        hole.make_clockwise();
        polylines_perimeter.push_back(hole.split_at_index(0));
    }
    if (polylines_perimeter.empty())
        return false;

    ExtrusionEntityCollection *eec_perimeter = new ExtrusionEntityCollection();
    eec_perimeter->set_can_sort_reverse(!context.fill.no_sort(), !context.fill.no_sort());
    extrusion_entities_append_paths(*eec_perimeter, std::move(polylines_perimeter),
                                    ExtrusionAttributes{context.role,
                                                        ExtrusionFlow{context.params.flow.mm3_per_mm(),
                                                                      context.params.flow.width(),
                                                                      context.params.flow.height()}},
                                    !context.params.monotonic);

    return fill_with_perimeter_append_non_empty_collection(eec_perimeter, branch);
}

static bool fill_with_perimeter_add_infill(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell,
    ExtrusionEntityCollection       &branch)
{
    ExPolygons infill_expolygons = fill_with_perimeter_infill_expolygons(context, shell);
    bool added = false;
    for (ExPolygon &inner_expolygon : infill_expolygons) {
        inner_expolygon.assert_valid();
        Surface surf_inner(*context.surface, inner_expolygon);

        ExtrusionEntityCollection *eec_infill = new ExtrusionEntityCollection();
        eec_infill->set_can_sort_reverse(!context.fill.no_sort(), !context.fill.no_sort());

        // The wrapped fill owns the gap-fill implementation.  Its gap-fill path
        // intersects candidates with no_overlap_expolygons, so provide the
        // current terminal shell when the parent fill has no stricter mask.
        if (context.fill.no_overlap_expolygons.empty())
            context.fill.infill->no_overlap_expolygons.push_back(shell);
        else
            context.fill.infill->no_overlap_expolygons =
                intersection_ex(shell, context.fill.no_overlap_expolygons);
        context.fill.infill->fill_surface_extrusion(&surf_inner, context.params, eec_infill->set_entities());

        if (eec_infill->empty()) {
            delete eec_infill;
            continue;
        }
#ifdef _DEBUGINFO
        eec_infill->visit(LoopAssertVisitor());
#endif
        added |= fill_with_perimeter_append_non_empty_collection(eec_infill, branch);
    }
    return added;
}

static ExPolygons fill_with_perimeter_perimeter_expolygons(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell)
{
    // The shell is the remaining fillable area.  The generated ExPolygon
    // boundary is the perimeter centerline, regularized by a small in/out offset.
    ExPolygons path_perimeter = offset2_ex(ExPolygons{shell},
                                           scale_d(-context.fill.get_spacing() / 2.) - context.perimeter_regularization,
                                           context.perimeter_regularization,
                                           ClipperLib::jtMiter,
                                           scale_d(context.fill.get_spacing()) * 10);
    // Keep the centerline inside the nominal half-spacing inset.  This prevents
    // positive offset regularization from pushing a contour outside its shell.
    path_perimeter = intersection_ex(path_perimeter, offset_ex(shell, scale_d(-context.fill.get_spacing() / 2.)));
    ensure_valid(path_perimeter);
    return path_perimeter;
}

static ExPolygons fill_with_perimeter_infill_expolygons(
    const FillWithPerimeterContext &context,
    const ExPolygon                &shell)
{
    // The infill surface is derived from the terminal available shell.  A
    // half-spacing encroachment lets the wrapped fill come back near the last
    // perimeter centerline; zero keeps it one half-spacing deeper inside.
    const double infill_delta = -context.fill.get_spacing() *
                                (context.fill.ratio_fill_inside + 0.5 - context.infill_encroachment);
    ExPolygons infill_expolygons = offset2_ex(ExPolygons{shell},
                                              scale_d(infill_delta),
                                              scale_d(context.fill.get_spacing() / 2.));
    ensure_valid(infill_expolygons);
    return infill_expolygons;
}

} // namespace Slic3r
