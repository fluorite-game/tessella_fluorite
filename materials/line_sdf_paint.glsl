// SPDX-License-Identifier: Apache-2.0
//
// The dashed line family's paint: the plain line's six properties and one more.
//
// `floorwidth` is `line-width` evaluated at the integer zoom, and mbgl carries it as a paint
// property of its own rather than deriving it in the shader -- `setLineWidth` assigns both, and
// the evaluator for this one floors the zoom. The fragment stage divides the distance field's
// gamma by it, so a dash pattern steps at each zoom level instead of stretching between them.

#include "line_paint.glsl"

float resolveLineFloorWidth() {
    return materialConstants_floorWidthFromAttribute
               ? unpackMixFloat(getCustom7().xy, materialParams.floorWidthT)
               : materialParams.floorwidth;
}
