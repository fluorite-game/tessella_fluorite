// SPDX-License-Identifier: Apache-2.0
//
// The fill family's paint, resolved once for all six of its materials.
//
// Its own file rather than lines in `paint.glsl`: the helpers there are the encoding, which every
// family shares, and this is the fill family's parameter names, which no other family has. A line
// material including this would reference a `colorT` it never declared.
//
// mbgl's fill fragment is `out_color = color * opacity`, and this is that product formed at the
// vertex instead. The two agree because both factors are per *feature*: every vertex of a fill
// triangle comes from one feature's ring, so the interpolants are constant across it and the
// product of interpolants is the interpolant of the product.

#include "paint.glsl"

vec4 resolveFillPaint() {
    vec4 color = materialConstants_colorFromAttribute
                     ? unpackMixColor(getCustom0(), materialParams.colorT)
                     : materialParams.color;
    float opacity = materialConstants_opacityFromAttribute
                        ? unpackMixFloat(getCustom1().xy, materialParams.opacityT)
                        : materialParams.opacity;
    return color * opacity;
}
