// SPDX-License-Identifier: Apache-2.0
//
// The four paint properties only the SDF half has: a sprite carries its own colour, so an icon
// has no fill, no halo and no blur to resolve.

#include "symbol_paint.glsl"

vec4 resolveSymbolFillColor() {
    return materialConstants_colorFromAttribute
               ? unpackMixColor(getCustom3(), materialParams.colorT)
               : materialParams.fillColor;
}

vec4 resolveSymbolHaloColor() {
    return materialConstants_haloColorFromAttribute
               ? unpackMixColor(getCustom4(), materialParams.haloColorT)
               : materialParams.haloColor;
}

float resolveSymbolHaloWidth() {
    return materialConstants_haloWidthFromAttribute
               ? unpackMixFloat(getCustom6().xy, materialParams.haloWidthT)
               : materialParams.haloWidth;
}

float resolveSymbolHaloBlur() {
    return materialConstants_haloBlurFromAttribute
               ? unpackMixFloat(getCustom7().xy, materialParams.haloBlurT)
               : materialParams.haloBlur;
}
