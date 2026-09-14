// SPDX-License-Identifier: Apache-2.0
//
// The heatmap family's paint: two properties, against a circle's seven.
//
// See `fill_paint.glsl` for why each family gets its own file, and `paint.glsl` for the
// encoding.

#include "paint.glsl"

float resolveHeatmapWeight() {
    return materialConstants_weightFromAttribute
               ? unpackMixFloat(getCustom0().xy, materialParams.weightT)
               : materialParams.weight;
}

float resolveHeatmapRadius() {
    return materialConstants_radiusFromAttribute
               ? unpackMixFloat(getCustom1().xy, materialParams.radiusT)
               : materialParams.radius;
}
