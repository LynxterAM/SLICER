///|/ Filled supports are generated as dense soluble interfaces only.
///|/
///|/ The algorithm is deliberately direct:
///|/ 1. collect automatic overhangs, support modifiers and support columns,
///|/ 2. project each supported area straight down through lower object layers,
///|/ 3. add explicit support-column volume slices on their own object layers,
///|/ 4. trim support by object XY clearance and optional build-plate-only masks,
///|/ 5. create one synchronized SupportLayer per useful object layer and fill it
///|/    with SupportMaterialInterface extrusions at the configured interface spacing.
///|/ This file does not include or call the legacy SupportMaterial pipeline.

#include "FilledSupport.hpp"

#include "../../ClipperUtils.hpp"
#include "../../Exception.hpp"
#include "../../ExtrusionEntityCollection.hpp"
#include "../../Fill/FillBase.hpp"
#include "../../Fill/FillWithPerimeter.hpp"
#include "../../Flow.hpp"
#include "../../Geometry.hpp"
#include "../../Layer.hpp"
#include "../../Print.hpp"
#include "../../PrintConfig.hpp"
#include "../../Surface.hpp"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <memory>
#include <vector>

namespace Slic3r {
namespace {

struct FilledSupportAnnotations
{
    std::vector<Polygons> enforcers_layers;
    std::vector<Polygons> blockers_layers;
    std::vector<Polygons> support_column_volumes_layers;
};

// Rejects settings that would require legacy base/contact/raft behavior.
static void validate_filled_support_settings(const PrintObject &object);

// Copies the first object region config so Fill has access to normal region tuning.
static PrintRegionConfig filled_region_config(const PrintObject &object);

// Collects support modifier volumes and painted facets on object-layer grids.
static FilledSupportAnnotations collect_annotations(const PrintObject &object);

// Computes the configured XY clearance without depending on SupportParameters.
static coordf_t filled_support_xy_gap(const PrintObject &object);

// Builds per-layer masks used to keep support away from the model contour.
static std::vector<Polygons> build_object_clearance_masks(const PrintObject &object, coordf_t gap_xy, coord_t resolution);

// Builds prefix masks of model material below each layer for build-plate-only supports.
static std::vector<Polygons> build_model_below_masks(const PrintObject &object, coord_t resolution);

// Finds the overhang/enforcer areas on a single object layer that need support.
static Polygons detect_layer_support_seeds(
    const PrintObject               &object,
    size_t                           layer_id,
    const FilledSupportAnnotations  &annotations,
    coord_t                          resolution);

// Projects detected seed areas and adds explicit support-column volume slices.
static std::vector<Polygons> project_support_columns(
    const PrintObject               &object,
    const FilledSupportAnnotations  &annotations,
    const std::vector<Polygons>     &object_clearance_masks,
    const std::vector<Polygons>     &model_below_masks,
    coord_t                          resolution,
    const std::function<void()>     &throw_on_cancel);

// Converts projected support areas into SupportLayer objects and their paths.
static void emit_filled_support_layers(
    PrintObject                 &object,
    const std::vector<Polygons> &support_by_layer,
    const std::vector<Polygons> &object_clearance_masks,
    const PrintRegionConfig     &region_config,
    InfillPattern                pattern,
    coord_t                      resolution);

// Expands the first bed layer like a raft flange while keeping model clearance.
static ExPolygons expand_filled_first_layer_areas(
    ExPolygons          support_areas,
    const Polygons     &object_clearance_mask,
    const PrintObject  &object,
    double              layer_height,
    coord_t             resolution);

// Generates dense interface extrusions for one synchronized support layer.
static void generate_filled_layer_extrusions(
    SupportLayer            &support_layer,
    ExPolygons               support_areas,
    const PrintObject       &object,
    const PrintRegionConfig &region_config,
    InfillPattern            pattern,
    coord_t                  resolution);

void validate_filled_support_settings(const PrintObject &object)
{
    const PrintObjectConfig &config = object.config();
    if (config.support_material_contact_distance_type.value != zdNone)
        throw RuntimeError("Filled supports require support_material_contact_distance_type = none.");
    if (config.raft_layers.value > 0)
        throw RuntimeError("Filled supports do not support raft_layers > 0.");
}

PrintRegionConfig filled_region_config(const PrintObject &object)
{
    // Fill uses region settings for overlap, gap-fill limits and Arachne-capable
    // infill patterns.  Copying the effective default keeps the generator local
    // while preserving the same computed defaults as object infill.
    PrintRegionConfig region_config = object.default_region_config(object.print()->default_region_config());
    return region_config;
}

FilledSupportAnnotations collect_annotations(const PrintObject &object)
{
    const size_t layer_count = object.layers().size();
    FilledSupportAnnotations annotations;
    annotations.enforcers_layers.assign(layer_count, Polygons());
    annotations.blockers_layers.assign(layer_count, Polygons());
    annotations.support_column_volumes_layers.assign(layer_count, Polygons());

    std::vector<ExPolygons> enforcers = object.slice_support_enforcers();
    std::vector<Polygons> custom_enforcers = object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER);
    const size_t enforcer_layer_count = std::min(layer_count, std::max(enforcers.size(), custom_enforcers.size()));
    for (size_t layer_id = 0; layer_id < enforcer_layer_count; ++layer_id) {
        if (layer_id < enforcers.size())
            polygons_append(annotations.enforcers_layers[layer_id], to_polygons(enforcers[layer_id]));
        if (layer_id < custom_enforcers.size())
            polygons_append(annotations.enforcers_layers[layer_id], std::move(custom_enforcers[layer_id]));
    }

    std::vector<ExPolygons> blockers = object.slice_support_blockers();
    std::vector<Polygons> custom_blockers = object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER);
    const size_t blocker_layer_count = std::min(layer_count, std::max(blockers.size(), custom_blockers.size()));
    for (size_t layer_id = 0; layer_id < blocker_layer_count; ++layer_id) {
        if (layer_id < blockers.size())
            polygons_append(annotations.blockers_layers[layer_id], to_polygons(blockers[layer_id]));
        if (layer_id < custom_blockers.size())
            polygons_append(annotations.blockers_layers[layer_id], std::move(custom_blockers[layer_id]));
    }

    // Support columns are explicit support volumes.  Their per-layer slices are
    // added later without vertical projection or same-layer object intersection.
    std::vector<ExPolygons> support_columns = object.slice_support_volumes(ModelVolumeType::SUPPORT_COLUMN);
    const size_t support_column_layer_count = std::min(layer_count, support_columns.size());
    for (size_t layer_id = 0; layer_id < support_column_layer_count; ++layer_id)
        polygons_append(annotations.support_column_volumes_layers[layer_id], to_polygons(support_columns[layer_id]));

    return annotations;
}

coordf_t filled_support_xy_gap(const PrintObject &object)
{
    const SlicingParameters &slicing_params = object.slicing_parameters();
    coordf_t external_perimeter_width = 0.;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegion &region = object.printing_region(region_id);
        const Flow flow = region.flow(object, frExternalPerimeter, slicing_params.layer_height, 2);
        external_perimeter_width = std::max(external_perimeter_width, coordf_t(flow.width()));
    }
    if (external_perimeter_width <= 0.)
        external_perimeter_width = support_material_interface_flow(&object, float(slicing_params.layer_height)).width();
    return object.config().support_material_xy_spacing.get_abs_value(external_perimeter_width);
}

std::vector<Polygons> build_object_clearance_masks(const PrintObject &object, coordf_t gap_xy, coord_t resolution)
{
    const size_t layer_count = object.layers().size();
    std::vector<Polygons> masks(layer_count);
    const coord_t clearance = std::max(SCALED_EPSILON, scale_t(gap_xy));
    for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
        masks[layer_id] = union_(offset(object.layers()[layer_id]->lslices(), clearance));
        ensure_valid(masks[layer_id], resolution);
    }
    return masks;
}

std::vector<Polygons> build_model_below_masks(const PrintObject &object, coord_t resolution)
{
    const size_t layer_count = object.layers().size();
    std::vector<Polygons> masks(layer_count);
    for (size_t layer_id = 1; layer_id < layer_count; ++layer_id) {
        masks[layer_id] = masks[layer_id - 1];
        polygons_append(masks[layer_id], offset(object.layers()[layer_id - 1]->lslices(), scale_(0.01)));
        masks[layer_id] = union_(masks[layer_id]);
        ensure_valid(masks[layer_id], resolution);
    }
    return masks;
}

Polygons detect_layer_support_seeds(
    const PrintObject               &object,
    size_t                           layer_id,
    const FilledSupportAnnotations  &annotations,
    coord_t                          resolution)
{
    if (layer_id == 0)
        return Polygons();

    const PrintObjectConfig &object_config = object.config();
    const Layer             &layer = *object.layers()[layer_id];
    const Layer             &lower_layer = *object.layers()[layer_id - 1];
    const Polygons           lower_layer_polygons = to_polygons(lower_layer.lslices());
    const bool               support_auto = object_config.support_material.value && object_config.support_material_auto.value;
    const double             threshold_rad = object_config.support_material_threshold.value > 0 ?
        M_PI * double(object_config.support_material_threshold.value + 1) / 180. :
        0.;

    Polygons seeds;
    coord_t  max_flow_width = 0;
    for (LayerRegion *layer_region : layer.regions()) {
        const coord_t flow_width = layer_region->flow(frExternalPerimeter).scaled_width();
        max_flow_width = std::max(max_flow_width, flow_width);
        const float lower_layer_offset =
            layer_id < size_t(object_config.support_material_enforce_layers.value) ? 0.f :
            threshold_rad > 0. ? float(scale_(lower_layer.height / tan(threshold_rad))) :
            float(flow_width / 2);

        ExPolygons layer_region_expolygons = to_expolygons(layer_region->slices().surfaces);
        ExPolygons unsupported;
        if (lower_layer_offset == 0.f) {
            unsupported = diff_ex(layer_region_expolygons, lower_layer_polygons);
        } else if (support_auto) {
            unsupported = diff_ex(
                layer_region_expolygons,
                expand_ex(lower_layer_polygons, lower_layer_offset, ClipperLib::jtSquare, 0.));
            if (!unsupported.empty()) {
                unsupported = diff_ex(
                    intersection_ex(offset_ex(unsupported, lower_layer_offset, ClipperLib::jtSquare, 0.), layer_region_expolygons),
                    lower_layer_polygons);
            }
        }

        if (!unsupported.empty()) {
            ensure_valid(unsupported, resolution);
            polygons_append(seeds, to_polygons(std::move(unsupported)));
        }
    }

    if (!annotations.enforcers_layers[layer_id].empty()) {
        Polygons enforced = intersection(layer.lslices(), annotations.enforcers_layers[layer_id]);
        ensure_valid(enforced, resolution);
        polygons_append(seeds, std::move(enforced));
    }

    if (!annotations.blockers_layers[layer_id].empty() && !seeds.empty()) {
        const ExPolygons blockers = union_ex(annotations.blockers_layers[layer_id]);
        seeds = diff(seeds, offset_ex(blockers, float(1000. * SCALED_EPSILON)));
    }

    if (!seeds.empty() && max_flow_width > 0)
        seeds = closing(seeds, double(max_flow_width) * 0.1);
    ensure_valid(seeds, resolution);
    return seeds;
}

std::vector<Polygons> project_support_columns(
    const PrintObject               &object,
    const FilledSupportAnnotations  &annotations,
    const std::vector<Polygons>     &object_clearance_masks,
    const std::vector<Polygons>     &model_below_masks,
    coord_t                          resolution,
    const std::function<void()>     &throw_on_cancel)
{
    const size_t layer_count = object.layers().size();
    std::vector<Polygons> support_by_layer(layer_count);
    const bool buildplate_only = object.config().support_material_buildplate_only.value;

    for (size_t source_layer_id = 1; source_layer_id < layer_count; ++source_layer_id) {
        throw_on_cancel();
        Polygons projection = detect_layer_support_seeds(object, source_layer_id, annotations, resolution);
        for (int support_layer_id = int(source_layer_id) - 1; support_layer_id >= 0 && !projection.empty(); --support_layer_id) {
            projection = diff(projection, object_clearance_masks[size_t(support_layer_id)]);
            if (buildplate_only && !model_below_masks[size_t(support_layer_id)].empty())
                projection = diff(projection, model_below_masks[size_t(support_layer_id)]);
            if (!annotations.blockers_layers[size_t(support_layer_id)].empty()) {
                const ExPolygons blockers = union_ex(annotations.blockers_layers[size_t(support_layer_id)]);
                projection = diff(projection, offset_ex(blockers, float(1000. * SCALED_EPSILON)));
            }
            ensure_valid(projection, resolution);
            if (!projection.empty())
                polygons_append(support_by_layer[size_t(support_layer_id)], projection);
        }
    }

    for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
        throw_on_cancel();
        Polygons support_column_volumes = annotations.support_column_volumes_layers[layer_id];
        if (support_column_volumes.empty())
            continue;

        support_column_volumes = diff(support_column_volumes, object_clearance_masks[layer_id]);
        if (!annotations.blockers_layers[layer_id].empty()) {
            const ExPolygons blockers = union_ex(annotations.blockers_layers[layer_id]);
            support_column_volumes = diff(support_column_volumes, offset_ex(blockers, float(1000. * SCALED_EPSILON)));
        }
        ensure_valid(support_column_volumes, resolution);
        if (!support_column_volumes.empty())
            polygons_append(support_by_layer[layer_id], std::move(support_column_volumes));
    }

    for (Polygons &layer_polygons : support_by_layer) {
        if (!layer_polygons.empty()) {
            layer_polygons = union_(layer_polygons);
            ensure_valid(layer_polygons, resolution);
        }
    }
    return support_by_layer;
}

void emit_filled_support_layers(
    PrintObject                 &object,
    const std::vector<Polygons> &support_by_layer,
    const std::vector<Polygons> &object_clearance_masks,
    const PrintRegionConfig     &region_config,
    InfillPattern                pattern,
    coord_t                      resolution)
{
    object.clear_support_layers();
    size_t interface_id = 0;
    for (size_t object_layer_id = 0; object_layer_id < support_by_layer.size(); ++object_layer_id) {
        if (support_by_layer[object_layer_id].empty())
            continue;

        const Layer &object_layer = *object.layers()[object_layer_id];
        ExPolygons support_areas = union_ex(support_by_layer[object_layer_id]);
        ensure_valid(support_areas, resolution);
        if (support_areas.empty())
            continue;

        if (object_layer.bottom_z() <= EPSILON) {
            support_areas = expand_filled_first_layer_areas(
                std::move(support_areas), object_clearance_masks[object_layer_id], object, object_layer.height, resolution);
            if (support_areas.empty())
                continue;
        }

        object.add_support_layer(int(object.support_layer_count()), int(interface_id), object_layer.height, object_layer.print_z);
        SupportLayer &support_layer = *object.edit_support_layers().back();
        support_layer.support_islands = support_areas;
        support_layer.support_islands_bboxes.reserve(support_layer.support_islands.size());
        for (const ExPolygon &expolygon : support_layer.support_islands)
            support_layer.support_islands_bboxes.emplace_back(get_extents(expolygon).inflated(SCALED_EPSILON));

        generate_filled_layer_extrusions(support_layer, std::move(support_areas), object, region_config, pattern, resolution);
        ++interface_id;
    }
}

ExPolygons expand_filled_first_layer_areas(
    ExPolygons          support_areas,
    const Polygons     &object_clearance_mask,
    const PrintObject  &object,
    double              layer_height,
    coord_t             resolution)
{
    const double expansion = scale_(object.config().raft_first_layer_expansion.value);
    if (expansion <= double(SCALED_EPSILON))
        return support_areas;

    const Flow flow = support_material_interface_flow(&object, float(layer_height));
    const double step_reference = std::max<double>(double(SCALED_EPSILON), double(flow.scaled_width()));
    const int step_count = std::max(5, int(std::ceil(expansion / step_reference)));
    const double step = expansion / double(step_count);

    ExPolygons expanded = std::move(support_areas);
    for (int step_idx = 0; step_idx < step_count; ++step_idx) {
        Polygons expanded_polygons = expand(expanded, step, jtSquare, 0.);
        expanded = object_clearance_mask.empty() ?
            union_ex(expanded_polygons) :
            diff_ex(expanded_polygons, object_clearance_mask);
        ensure_valid(expanded, resolution);
        if (expanded.empty())
            break;
    }
    return expanded;
}

void generate_filled_layer_extrusions(
    SupportLayer            &support_layer,
    ExPolygons               support_areas,
    const PrintObject       &object,
    const PrintRegionConfig &region_config,
    InfillPattern            pattern,
    coord_t                  resolution)
{
    const Flow flow = support_material_interface_flow(&object, float(support_layer.height));
    std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
    // Filled supports use the top interface pattern for every support layer.
    const int configured_perimeters = object.config().support_material_interface_perimeters.value;
    if (configured_perimeters > 0)
        filler = std::make_unique<FillWithPerimeter>(filler.release(), static_cast<unsigned int>(configured_perimeters));
    if (FillWithPerimeter *fill_with_perimeter = dynamic_cast<FillWithPerimeter *>(filler.get())) {
        if (flow.spacing() > 0.) {
            const double overlap = region_config.infill_overlap.get_abs_value(flow.spacing());
            fill_with_perimeter->perimeter_infill_encroachment = float(std::max(0., std::min(0.5, overlap / flow.spacing())));
        }
    }

    // Fill expects a density, while the support option stores the free gap between
    // adjacent interface lines. The first bed layer keeps its dedicated raft density.
    const double interface_pitch = flow.spacing() + object.config().support_material_interface_spacing.value;
    const float interface_density = float(std::min(1., flow.spacing() / interface_pitch));

    FillParams fill_params;
    fill_params.density = support_layer.bottom_z() <= EPSILON ?
        float(object.config().raft_first_layer_density.get_abs_value(1.f)) :
        interface_density;
    fill_params.dont_adjust = true;
    fill_params.config = &region_config;
    fill_params.flow = flow;
    fill_params.role = ExtrusionRole::SupportMaterialInterface;
    fill_params.fill_resolution = resolution;
    fill_params.layer_height = float(support_layer.height);
    fill_params.add_gap_fill =
        object.config().support_material_interface_gap_fill.value && fill_params.density > 1.f - EPSILON;

    filler->set_bounding_box(object.bounding_box());
    filler->set_config(&object.print()->config(), &object.config());
    filler->layer_id = support_layer.id();
    filler->z = support_layer.print_z;
    filler->angle = Geometry::deg2rad(float(object.config().support_material_interface_angle.value)) +
        float(support_layer.interface_id()) * Geometry::deg2rad(float(object.config().support_material_interface_angle_increment.value));
    filler->init_spacing(flow.spacing(), fill_params);
    filler->link_max_length = scale_t(flow.spacing());

    // support_areas are just disjoint extrusion, you need an offset2 to join them
    distf_t offset_dist = scale_d(0.5 * flow.spacing());
    ExPolygons extrusion_envelopes = offset2_ex(
        support_areas, offset_dist, -offset_dist);
    ensure_valid(extrusion_envelopes, resolution);

    for (ExPolygon &expolygon : extrusion_envelopes) {
        if (expolygon.area() <= 0.)
            continue;
        Surface surface(stPosInternal | stDensSolid, std::move(expolygon));
        filler->fill_surface_extrusion(&surface, fill_params, support_layer.support_fills.set_entities());
    }
}

} // namespace

void filled_support_generate(PrintObject &object, std::function<void()> throw_on_cancel)
{
    validate_filled_support_settings(object);
    throw_on_cancel();

    const PrintRegionConfig region_config = filled_region_config(object);
    InfillPattern pattern = object.config().support_material_top_interface_pattern.value;
    if (pattern == ipAuto)
        pattern = ipRectilinear;
    if (pattern == ipRectiWithPerimeter)
        pattern = ipRectilinear;

    const coord_t resolution = std::max(SCALED_EPSILON, scale_t(object.print()->config().resolution_internal.value));
    const FilledSupportAnnotations annotations = collect_annotations(object);
    const coordf_t gap_xy = filled_support_xy_gap(object);
    const std::vector<Polygons> object_clearance_masks = build_object_clearance_masks(object, gap_xy, resolution);
    const std::vector<Polygons> model_below_masks = object.config().support_material_buildplate_only.value ?
        build_model_below_masks(object, resolution) :
        std::vector<Polygons>(object.layers().size());

    const std::vector<Polygons> support_by_layer = project_support_columns(
        object, annotations, object_clearance_masks, model_below_masks, resolution, throw_on_cancel);
    emit_filled_support_layers(object, support_by_layer, object_clearance_masks, region_config, pattern, resolution);
}

} // namespace Slic3r
