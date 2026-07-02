///|/ Filled support overhang normalization is a post-process on object
///|/ perimeter extrusions.
///|/
///|/ Support generation itself only knows where filled support islands are.  The
///|/ perimeter generator later decides which perimeter centerlines are overhangs.
///|/ This file connects the two results: for each object layer, it finds the
///|/ support islands on the layer immediately below, clips overhang perimeter
///|/ paths by that mask, and converts only the supported pieces back to regular
///|/ perimeter roles and region flows.  The operation is intentionally limited to
///|/ filled supports so the legacy support styles keep their existing behavior.

#include "FilledSupportedOverhangs.hpp"

#include "../../ClipperUtils.hpp"
#include "../../ExtrusionEntityCollection.hpp"
#include "../../Layer.hpp"
#include "../../Print.hpp"
#include "../../ShortestPath.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>
#include <vector>

namespace Slic3r {

// Coordinates access to LayerRegion internals without exposing mutable perimeter storage as public API.
class FilledSupportedOverhangsAccessor
{
public:
    static void normalize(PrintObject &object, const std::function<void()> &throw_on_cancel);
};

namespace {

struct ClassifiedPolyline
{
    Polyline polyline;
    bool     supported = false;
};

// Finds the synchronized filled support layer below each object layer.
static std::vector<const SupportLayer *> build_support_layers_below_object_layers(const PrintObject &object);

// Recurses through nested collections, replacing leaf path entities when a split creates multiple pieces.
static void normalize_collection(
    ExtrusionEntityCollection  &collection,
    const LayerRegion          &layer_region,
    const SupportLayer         &support_layer);

// Rewrites a continuous path vector while preserving its original sequence.
static void normalize_paths(
    ExtrusionPaths      &paths,
    const LayerRegion  &layer_region,
    const SupportLayer &support_layer,
    bool                closed_sequence);

// Splits one overhang path into unsupported and supported pieces, or returns the original path unchanged.
static ExtrusionPaths split_overhang_path(
    const ExtrusionPath &path,
    const LayerRegion   &layer_region,
    const SupportLayer  &support_layer);

// Converts the overhang path attributes to the matching normal perimeter role and flow for this region.
static ExtrusionAttributes normal_perimeter_attributes(const ExtrusionPath &path, const LayerRegion &layer_region);

// Selects only support islands whose bounding boxes can intersect the path centerline.
static ExPolygons candidate_support_islands(const Polyline &source, const SupportLayer &support_layer);

// Clips a polyline against candidate support islands and stores pieces with their target state.
static std::vector<ClassifiedPolyline> classify_polyline_by_support(
    const Polyline   &source,
    const ExPolygons &candidate_support,
    Polylines        &&supported_polylines);

// Adds clipped pieces without trusting Clipper output order or orientation.
static void append_classified_polylines(
    std::vector<ClassifiedPolyline> &classified,
    Polylines                       &&polylines,
    bool                              supported);

// Builds extrusion paths from classified clipped polylines and the two possible attribute sets.
static ExtrusionPaths make_extrusion_paths(
    std::vector<ClassifiedPolyline> &&classified,
    const ExtrusionAttributes        &overhang_attributes,
    const ExtrusionAttributes        &normal_attributes);

// Reorders clipped paths through the common perimeter chaining helper and snaps epsilon-close junctions.
static void chain_and_stabilize_split_paths(const ExtrusionPath &source, ExtrusionPaths &paths);

// Reverses the whole chained sequence when the generic chainer picked the opposite source direction.
static void reverse_split_paths_if_needed(const ExtrusionPath &source, ExtrusionPaths &paths);

// Detects reversed closed-loop order by comparing the two source neighbors around the seam.
static bool closed_split_paths_need_reverse(const ExtrusionPath &source, const ExtrusionPaths &paths);

// Finds the first occurrence of a source point in the split path sequence.
static bool first_split_point_index(const ExtrusionPaths &paths, const Point &point, size_t &index);

// Reverses both the path order and each path direction.
static void reverse_split_path_sequence(ExtrusionPaths &paths);

// Makes the chained path sequence use exact shared junction coordinates when Clipper produced tiny differences.
static void snap_split_path_junctions(const ExtrusionPath &source, ExtrusionPaths &paths);

// Checks whether a split path sequence is safe to use instead of the original path.
static bool split_path_sequence_is_valid(const ExtrusionPath &source, const ExtrusionPaths &paths);

// Returns true when two points are identical or differ only by Clipper numeric noise.
static bool points_match_or_epsilon(const Point &lhs, const Point &rhs);

// Verifies that a single path split still covers the exact same ordered endpoints.
static void assert_transformed_path_sequence(const ExtrusionPath &source, const ExtrusionPaths &result);

// Verifies that a multipath or loop replacement preserves all original path junctions.
static void assert_transformed_path_sequence(
    const ExtrusionPaths &source,
    const ExtrusionPaths &result,
    bool                  closed_sequence);

// Verifies that a generated sequence is continuous and optionally closed.
static void assert_path_sequence_connected(const ExtrusionPaths &paths, bool closed_sequence);

// Tests whether a path junction from the original entity is still a junction after splitting.
static bool has_path_junction(const ExtrusionPaths &paths, const Point &point);

std::vector<const SupportLayer *> build_support_layers_below_object_layers(const PrintObject &object)
{
    std::vector<const SupportLayer *> support_layers(object.layers().size(), nullptr);
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id) {
        const Layer *lower_layer = object.layers()[layer_id]->lower_layer;
        if (lower_layer == nullptr)
            continue;

        for (const SupportLayer *support_layer : object.support_layers()) {
            if (std::abs(support_layer->print_z - lower_layer->print_z) > EPSILON)
                continue;

            support_layers[layer_id] = support_layer;
            break;
        }
    }
    return support_layers;
}

void normalize_collection(
    ExtrusionEntityCollection  &collection,
    const LayerRegion          &layer_region,
    const SupportLayer         &support_layer)
{
    ExtrusionEntitiesPtr &entities = collection.set_entities();
    for (ExtrusionEntity *&entity : entities) {
        if (ExtrusionEntityCollection *child_collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            normalize_collection(*child_collection, layer_region, support_layer);
        } else if (ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop *>(entity)) {
            normalize_paths(loop->paths, layer_region, support_layer, true);
        } else if (ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath *>(entity)) {
            normalize_paths(multipath->paths, layer_region, support_layer, false);
        } else if (ExtrusionPath *path = dynamic_cast<ExtrusionPath *>(entity)) {
            ExtrusionPaths split_paths = split_overhang_path(*path, layer_region, support_layer);
#ifndef NDEBUG
            assert_transformed_path_sequence(*path, split_paths);
#endif
            if (split_paths.size() == 1) {
                *path = std::move(split_paths.front());
            } else if (!split_paths.empty()) {
                ExtrusionMultiPath replacement(split_paths);
                replacement.set_can_reverse(false);
                delete entity;
                entity = replacement.clone_move();
            }
        }
    }
}

void normalize_paths(
    ExtrusionPaths      &paths,
    const LayerRegion  &layer_region,
    const SupportLayer &support_layer,
    bool                closed_sequence)
{
#ifndef NDEBUG
    const ExtrusionPaths original_paths = paths;
#endif
    ExtrusionPaths normalized;
    normalized.reserve(paths.size());
    for (const ExtrusionPath &path : paths) {
        ExtrusionPaths split_paths = split_overhang_path(path, layer_region, support_layer);
#ifndef NDEBUG
        assert_transformed_path_sequence(path, split_paths);
#endif
        normalized.insert(normalized.end(), split_paths.begin(), split_paths.end());
    }
#ifndef NDEBUG
    assert_transformed_path_sequence(original_paths, normalized, closed_sequence);
#endif
    paths.swap(normalized);
}

ExtrusionPaths split_overhang_path(
    const ExtrusionPath &path,
    const LayerRegion   &layer_region,
    const SupportLayer  &support_layer)
{
    if (!path.role().is_overhang() || path.empty() || support_layer.support_islands.empty())
        return ExtrusionPaths{ path };

    const coord_t arc_deviation = std::max(SCALED_EPSILON, scale_t(std::max(0.001f, path.width() / 10.f)));
    Polyline source = path.polyline.to_polyline(arc_deviation);
    ensure_valid(source, SCALED_EPSILON);
    if (!source.is_valid())
        return ExtrusionPaths{ path };

    ExPolygons candidate_support = candidate_support_islands(source, support_layer);
    if (candidate_support.empty())
        return ExtrusionPaths{ path };

    // The bounding-box test is only a cheap prefilter.  A path is really
    // supported only if its centerline intersects a candidate support island.
    Polylines supported_polylines = intersection_pl(source, candidate_support);
    ensure_valid(supported_polylines, SCALED_EPSILON);
    if (supported_polylines.empty())
        return ExtrusionPaths{ path };

    std::vector<ClassifiedPolyline> classified = classify_polyline_by_support(
        source, candidate_support, std::move(supported_polylines));

    bool has_supported = false;
    bool has_unsupported = false;
    for (const ClassifiedPolyline &piece : classified) {
        has_supported = has_supported || piece.supported;
        has_unsupported = has_unsupported || !piece.supported;
    }

    if (!has_supported)
        return ExtrusionPaths{ path };

    const ExtrusionAttributes normal_attributes = normal_perimeter_attributes(path, layer_region);
    if (!has_unsupported) {
        ExtrusionPath normal_path = path;
        normal_path.attributes_mutable() = normal_attributes;
        return ExtrusionPaths{ std::move(normal_path) };
    }

    ExtrusionPaths split_paths = make_extrusion_paths(std::move(classified), path.attributes(), normal_attributes);
    chain_and_stabilize_split_paths(path, split_paths);
    if (!split_path_sequence_is_valid(path, split_paths)) {
        assert(false);
        return ExtrusionPaths{ path };
    }
#ifndef NDEBUG
    assert_transformed_path_sequence(path, split_paths);
#endif
    return split_paths;
}

ExtrusionAttributes normal_perimeter_attributes(const ExtrusionPath &path, const LayerRegion &layer_region)
{
    const bool external = path.role().is_external();
    const FlowRole flow_role = external ? frExternalPerimeter : frPerimeter;
    const ExtrusionRole extrusion_role = external ? ExtrusionRole::ExternalPerimeter : ExtrusionRole::Perimeter;
    const double layer_height = path.height() > 0.f ? double(path.height()) : layer_region.layer()->height;
    ExtrusionAttributes attributes(extrusion_role, layer_region.flow(flow_role, layer_height));
    attributes.no_seam = path.attributes().no_seam;
    return attributes;
}

ExPolygons candidate_support_islands(const Polyline &source, const SupportLayer &support_layer)
{
    BoundingBox path_bbox = get_extents(source);
    path_bbox = path_bbox.inflated(SCALED_EPSILON);

    ExPolygons candidates;
    const size_t bbox_count = std::min(support_layer.support_islands.size(), support_layer.support_islands_bboxes.size());
    for (size_t island_id = 0; island_id < bbox_count; ++island_id) {
        if (support_layer.support_islands_bboxes[island_id].overlap(path_bbox))
            candidates.emplace_back(support_layer.support_islands[island_id]);
    }

    // Some hand-built tests or future generators may forget to fill the cache.
    // Falling back to real extents keeps the behavior correct instead of silently
    // skipping all support islands.
    for (size_t island_id = bbox_count; island_id < support_layer.support_islands.size(); ++island_id) {
        if (get_extents(support_layer.support_islands[island_id]).inflated(SCALED_EPSILON).overlap(path_bbox))
            candidates.emplace_back(support_layer.support_islands[island_id]);
    }

    // A tiny inflation makes centerline clipping stable at shared support
    // boundaries without turning nearby unsupported perimeters into supported ones.
    return candidates.empty() ? ExPolygons() : offset_ex(candidates, double(SCALED_EPSILON));
}

std::vector<ClassifiedPolyline> classify_polyline_by_support(
    const Polyline   &source,
    const ExPolygons &candidate_support,
    Polylines        &&supported_polylines)
{
    std::vector<ClassifiedPolyline> classified;
    append_classified_polylines(classified, std::move(supported_polylines), true);
    append_classified_polylines(classified, diff_pl(source, candidate_support), false);
    return classified;
}

void append_classified_polylines(
    std::vector<ClassifiedPolyline> &classified,
    Polylines                       &&polylines,
    bool                              supported)
{
    ensure_valid(polylines, SCALED_EPSILON);
    classified.reserve(classified.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        if (!polyline.is_valid())
            continue;

        ClassifiedPolyline piece;
        piece.polyline = std::move(polyline);
        piece.supported = supported;
        classified.emplace_back(std::move(piece));
    }
}

ExtrusionPaths make_extrusion_paths(
    std::vector<ClassifiedPolyline> &&classified,
    const ExtrusionAttributes        &overhang_attributes,
    const ExtrusionAttributes        &normal_attributes)
{
    ExtrusionPaths paths;
    paths.reserve(classified.size());
    for (ClassifiedPolyline &piece : classified) {
        const ExtrusionAttributes &attributes = piece.supported ? normal_attributes : overhang_attributes;
        paths.emplace_back(ArcPolyline{ std::move(piece.polyline) }, attributes, false);
    }
    return paths;
}

void chain_and_stabilize_split_paths(const ExtrusionPath &source, ExtrusionPaths &paths)
{
    if (paths.empty())
        return;

    Point start_point = source.first_point();
    chain_and_reorder_extrusion_paths(paths, &start_point);
    reverse_split_paths_if_needed(source, paths);
    snap_split_path_junctions(source, paths);
}

void reverse_split_paths_if_needed(const ExtrusionPath &source, ExtrusionPaths &paths)
{
    if (paths.empty())
        return;

    if (source.is_closed()) {
        if (closed_split_paths_need_reverse(source, paths))
            reverse_split_path_sequence(paths);
        return;
    }

    const bool starts_at_source_start = points_match_or_epsilon(paths.front().first_point(), source.first_point());
    const bool starts_at_source_end = points_match_or_epsilon(paths.front().first_point(), source.last_point());
    const bool ends_at_source_start = points_match_or_epsilon(paths.back().last_point(), source.first_point());
    if (starts_at_source_start || !starts_at_source_end || !ends_at_source_start)
        return;

    reverse_split_path_sequence(paths);
}

bool closed_split_paths_need_reverse(const ExtrusionPath &source, const ExtrusionPaths &paths)
{
    Point source_forward_point;
    Point source_backward_point;
    bool has_forward_point = false;
    bool has_backward_point = false;
    for (size_t point_id = 1; point_id < source.size(); ++point_id) {
        const Point &point = source.polyline.get_point(point_id);
        if (!points_match_or_epsilon(point, source.first_point())) {
            source_forward_point = point;
            has_forward_point = true;
            break;
        }
    }
    for (size_t point_id = source.size() - 1; point_id > 0; --point_id) {
        const Point &point = source.polyline.get_point(point_id - 1);
        if (!points_match_or_epsilon(point, source.first_point())) {
            source_backward_point = point;
            has_backward_point = true;
            break;
        }
    }
    if (!has_forward_point || !has_backward_point || points_match_or_epsilon(source_forward_point, source_backward_point))
        return false;

    size_t forward_index = 0;
    size_t backward_index = 0;
    if (!first_split_point_index(paths, source_forward_point, forward_index) ||
        !first_split_point_index(paths, source_backward_point, backward_index))
        return false;

    return backward_index < forward_index;
}

bool first_split_point_index(const ExtrusionPaths &paths, const Point &point, size_t &index)
{
    size_t global_index = 0;
    for (const ExtrusionPath &path : paths) {
        for (size_t point_id = 0; point_id < path.size(); ++point_id) {
            if (points_match_or_epsilon(path.polyline.get_point(point_id), point)) {
                index = global_index;
                return true;
            }
            ++global_index;
        }
    }
    return false;
}

void reverse_split_path_sequence(ExtrusionPaths &paths)
{
    std::reverse(paths.begin(), paths.end());
    for (ExtrusionPath &path : paths)
        path.reverse();
}

void snap_split_path_junctions(const ExtrusionPath &source, ExtrusionPaths &paths)
{
    if (paths.empty())
        return;

    if (points_match_or_epsilon(paths.front().first_point(), source.first_point()))
        paths.front().polyline.set_front(source.first_point());

    for (size_t path_id = 1; path_id < paths.size(); ++path_id) {
        if (paths[path_id - 1].last_point() == paths[path_id].first_point())
            continue;
        if (!points_match_or_epsilon(paths[path_id - 1].last_point(), paths[path_id].first_point()))
            continue;

        // Clipper may return the same split point rounded by a few integer
        // units on each side.  Use the midpoint so both neighboring paths share
        // one exact junction without favoring either rounded endpoint.
        const Point middle = (paths[path_id - 1].last_point() + paths[path_id].first_point()) / 2;
        paths[path_id - 1].polyline.set_back(middle);
        paths[path_id].polyline.set_front(middle);
    }

    if (points_match_or_epsilon(paths.back().last_point(), source.last_point()))
        paths.back().polyline.set_back(source.last_point());
}

bool split_path_sequence_is_valid(const ExtrusionPath &source, const ExtrusionPaths &paths)
{
    if (source.empty())
        return paths.size() == 1 && paths.front().empty();
    if (paths.empty())
        return false;
    if (paths.front().first_point() != source.first_point())
        return false;
    if (paths.back().last_point() != source.last_point())
        return false;

    for (size_t path_id = 1; path_id < paths.size(); ++path_id)
        if (paths[path_id - 1].last_point() != paths[path_id].first_point())
            return false;

    return !source.is_closed() || paths.back().last_point() == paths.front().first_point();
}

bool points_match_or_epsilon(const Point &lhs, const Point &rhs)
{
    return lhs == rhs || lhs.coincides_with_epsilon(rhs);
}

Points extrusion_path_points(const ExtrusionPath &path)
{
    Points points;
    points.reserve(path.polyline.size());
    for (size_t point_id = 0; point_id < path.polyline.size(); ++point_id)
        points.emplace_back(path.polyline.get_point(point_id));
    return points;
}

Points extrusion_paths_points(const ExtrusionPaths &paths)
{
    Points points;
    for (const ExtrusionPath &path : paths) {
        const Points path_points = extrusion_path_points(path);
        points.insert(points.end(), path_points.begin(), path_points.end());
    }
    return points;
}

void assert_points_preserved_in_order(const Points &source_points, const Points &result_points)
{
#ifndef NDEBUG
    size_t result_point_id = 0;
    for (size_t i = 0; i < source_points.size(); ++i) {
        const Point &source_point = source_points[i];
        while (result_point_id < result_points.size() && !result_points[result_point_id].coincides_with_epsilon(source_point))
            ++result_point_id;

        assert(result_point_id < result_points.size());
        ++result_point_id;
    }
#else
    (void)source_points;
    (void)result_points;
#endif
}

void assert_transformed_path_sequence(const ExtrusionPath &source, const ExtrusionPaths &result)
{
#ifndef NDEBUG
    if (source.empty()) {
        assert(result.size() == 1 && result.front().empty());
        return;
    }

    assert(!source.empty());
    assert(!result.empty());
    assert(result.front().first_point() == source.first_point());
    assert(result.back().last_point() == source.last_point());

    // A split path is still one ordered extrusion.  Adjacent pieces must meet
    // exactly; otherwise the post-process has introduced travels or reordered
    // legal perimeter geometry.
    assert_path_sequence_connected(result, source.is_closed());

    const Points source_points = extrusion_path_points(source);
    const Points result_points = extrusion_paths_points(result);
    assert_points_preserved_in_order(source_points, result_points);
#else
    (void)source;
    (void)result;
#endif
}

void assert_transformed_path_sequence(
    const ExtrusionPaths &source,
    const ExtrusionPaths &result,
    bool                  closed_sequence)
{
#ifndef NDEBUG
    if (source.empty() || result.empty()) {
        assert(source.empty() && result.empty());
        return;
    }

    assert(!source.empty());
    assert(!result.empty());
    assert(source.front().first_point() == result.front().first_point());
    assert(source.back().last_point() == result.back().last_point());
    assert_path_sequence_connected(result, closed_sequence);

    const Points source_points = extrusion_paths_points(source);
    const Points result_points = extrusion_paths_points(result);
    assert_points_preserved_in_order(source_points, result_points);

    // Multipaths and loops may already be split into semantic path chunks.  The
    // support-overhang pass may subdivide those chunks, but it must not erase
    // their original junction points.
    for (size_t path_id = 1; path_id < source.size(); ++path_id) {
        assert(source[path_id - 1].last_point() == source[path_id].first_point());
        assert(has_path_junction(result, source[path_id].first_point()));
    }

    if (closed_sequence)
        assert(result.back().last_point() == result.front().first_point());
#else
    (void)source;
    (void)result;
    (void)closed_sequence;
#endif
}

void assert_path_sequence_connected(const ExtrusionPaths &paths, bool closed_sequence)
{
#ifndef NDEBUG
    assert(!paths.empty());
    for (const ExtrusionPath &path : paths)
        assert(!path.empty());

    for (size_t path_id = 1; path_id < paths.size(); ++path_id)
        assert(paths[path_id - 1].last_point() == paths[path_id].first_point());

    if (closed_sequence)
        assert(paths.back().last_point() == paths.front().first_point());
#else
    (void)paths;
    (void)closed_sequence;
#endif
}

bool has_path_junction(const ExtrusionPaths &paths, const Point &point)
{
#ifndef NDEBUG
    for (size_t path_id = 1; path_id < paths.size(); ++path_id)
        if (paths[path_id - 1].last_point() == point && paths[path_id].first_point() == point)
            return true;
    return false;
#else
    (void)paths;
    (void)point;
    return true;
#endif
}

} // namespace

void FilledSupportedOverhangsAccessor::normalize(PrintObject &object, const std::function<void()> &throw_on_cancel)
{
    if (object.layers().empty() || object.support_layers().empty())
        return;

    const std::vector<const SupportLayer *> support_layers_by_layer = build_support_layers_below_object_layers(object);
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id) {
        throw_on_cancel();
        if (layer_id >= support_layers_by_layer.size() || support_layers_by_layer[layer_id] == nullptr)
            continue;

        Layer *layer = object.layers()[layer_id];
        for (LayerRegion *layer_region : layer->regions())
            normalize_collection(layer_region->m_perimeters, *layer_region, *support_layers_by_layer[layer_id]);
    }
}

void filled_support_normalize_supported_overhangs(PrintObject &object, std::function<void()> throw_on_cancel)
{
    FilledSupportedOverhangsAccessor::normalize(object, throw_on_cancel);
}

} // namespace Slic3r
