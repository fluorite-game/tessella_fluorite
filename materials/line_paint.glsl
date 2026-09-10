// SPDX-License-Identifier: Apache-2.0
//
// The line family's paint. See `fill_paint.glsl` for why each family gets its own file, and
// `paint.glsl` for the encoding.
//
// Six properties, and three of them shape the geometry rather than colour it: `width`, `gapwidth`
// and `offset` are read in the vertex stage and never reach the fragment. That is why the resolve
// is six small functions rather than one -- the two halves of the shader want different subsets,
// and a single struct would carry the colour through the vertex stage for nothing.

#include "paint.glsl"

vec4 resolveLineColor() {
    return materialConstants_colorFromAttribute
               ? unpackMixColor(getCustom1(), materialParams.colorT)
               : materialParams.color;
}

float resolveLineBlur() {
    return materialConstants_blurFromAttribute
               ? unpackMixFloat(getCustom2().xy, materialParams.blurT)
               : materialParams.blur;
}

float resolveLineOpacity() {
    return materialConstants_opacityFromAttribute
               ? unpackMixFloat(getCustom3().xy, materialParams.opacityT)
               : materialParams.opacity;
}

float resolveLineGapWidth() {
    return materialConstants_gapWidthFromAttribute
               ? unpackMixFloat(getCustom4().xy, materialParams.gapWidthT)
               : materialParams.gapwidth;
}

float resolveLineOffset() {
    return materialConstants_offsetFromAttribute
               ? unpackMixFloat(getCustom5().xy, materialParams.offsetT)
               : materialParams.offset;
}

float resolveLineWidth() {
    return materialConstants_widthFromAttribute
               ? unpackMixFloat(getCustom6().xy, materialParams.widthT)
               : materialParams.width;
}
