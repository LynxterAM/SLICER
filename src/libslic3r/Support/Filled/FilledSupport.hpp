///|/ Copyright (c) SuperSlicer 2026 Remi Durand @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
///|/ Filled supports are a simplified soluble-support pipeline.
///|/
///|/ The classic FFF support generator has separate base, contact and interface
///|/ layers and many synchronization modes.  This module intentionally keeps a
///|/ smaller model for future experiments: every generated support layer is
///|/ aligned with an object layer, every extrusion is an interface extrusion,
///|/ and the support volume is projected vertically until it reaches the bed.
///|/ The implementation lives outside SupportMaterial/SupportParameters so the
///|/ overhang, gap and projection rules can evolve without touching the legacy
///|/ grid/snug/organic flows.

#ifndef slic3r_FilledSupport_hpp
#define slic3r_FilledSupport_hpp

#include <functional>

namespace Slic3r {

class PrintObject;

void filled_support_generate(PrintObject &object, std::function<void()> throw_on_cancel = []{});

} // namespace Slic3r

#endif // slic3r_FilledSupport_hpp
