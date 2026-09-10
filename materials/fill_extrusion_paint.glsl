// SPDX-License-Identifier: Apache-2.0
//
// The fill-extrusion family's paint. See `fill_paint.glsl` for why each family gets its own file.
//
// One property, because `base` and `height` shape the building rather than colour it: both are
// bound unconditionally and the builder synthesises a constant fill where the style did not drive
// them, so neither has a permutation to select.

#include "paint.glsl"

/// Takes the attribute rather than naming it: the roof reads `custom3` and the wall `custom2`,
/// because a wall's own two channels are the facing and the extents it was expanded with.
vec4 resolveExtrusionColor(const vec4 packed) {
    return materialConstants_colorFromAttribute
               ? unpackMixColor(packed, materialParams.colorT)
               : materialParams.color;
}
