// SPDX-License-Identifier: Apache-2.0
//
// The circle family's paint. See `fill_paint.glsl` for why each family gets its own file, and
// `paint.glsl` for the encoding.
//
// Every attribute a circle has but its position is paint -- seven properties, which is the widest
// permutation space in the style spec and the strongest argument for specializing one package
// rather than shipping the combinations.

#include "paint.glsl"

vec4 resolveCircleColor() {
    return materialConstants_colorFromAttribute
               ? unpackMixColor(getCustom0(), materialParams.colorT)
               : materialParams.color;
}

float resolveCircleRadius() {
    return materialConstants_radiusFromAttribute
               ? unpackMixFloat(getCustom1().xy, materialParams.radiusT)
               : materialParams.radius;
}

float resolveCircleBlur() {
    return materialConstants_blurFromAttribute
               ? unpackMixFloat(getCustom2().xy, materialParams.blurT)
               : materialParams.blur;
}

float resolveCircleOpacity() {
    return materialConstants_opacityFromAttribute
               ? unpackMixFloat(getCustom3().xy, materialParams.opacityT)
               : materialParams.opacity;
}

vec4 resolveCircleStrokeColor() {
    return materialConstants_strokeColorFromAttribute
               ? unpackMixColor(getCustom4(), materialParams.strokeColorT)
               : materialParams.strokeColor;
}

float resolveCircleStrokeWidth() {
    return materialConstants_strokeWidthFromAttribute
               ? unpackMixFloat(getCustom5().xy, materialParams.strokeWidthT)
               : materialParams.strokeWidth;
}

float resolveCircleStrokeOpacity() {
    return materialConstants_strokeOpacityFromAttribute
               ? unpackMixFloat(getCustom6().xy, materialParams.strokeOpacityT)
               : materialParams.strokeOpacity;
}
