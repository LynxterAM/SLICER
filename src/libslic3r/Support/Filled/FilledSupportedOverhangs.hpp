///|/ Filled support overhang normalization rewrites object perimeters that
///|/ are actually backed by filled support.
///|/
///|/ Filled supports are generated before dynamic overhang processing finishes.
///|/ This helper runs after overhang roles are known: it clips overhang perimeter
///|/ centerlines by the support islands below the object layer.  Unsupported
///|/ parts keep their overhang role and flow, while supported parts become normal
///|/ region perimeters so they use regular perimeter speed and flow.

#ifndef slic3r_FilledSupportedOverhangs_hpp
#define slic3r_FilledSupportedOverhangs_hpp

#include <functional>

namespace Slic3r {

class PrintObject;

void filled_support_normalize_supported_overhangs(PrintObject &object, std::function<void()> throw_on_cancel = []{});

} // namespace Slic3r

#endif // slic3r_FilledSupportedOverhangs_hpp
