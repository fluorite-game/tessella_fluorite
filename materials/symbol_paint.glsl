// SPDX-License-Identifier: Apache-2.0
//
// The symbol family's paint, in the part both halves share. See `fill_paint.glsl` for why each
// family gets its own file, and `paint.glsl` for the encoding.
//
// Ten style properties and five attributes: `text-color` and `icon-color` are the same
// `idSymbolColorVertexAttribute`, and which of them a drawable means is decided by which half it
// draws. The producer describes only its own half -- see `binder::symbol_layout` -- so by the time
// the bytes are here there is one set of five and no ambiguity left.
//
// Opacity is the one both shaders declare. The other four are the SDF's alone, in
// `symbol_sdf_paint.glsl`, because a constant a material does not declare is not an unused
// symbol -- it is an undeclared identifier, and the icon halves would not compile.

#include "paint.glsl"

/// Takes the attribute rather than naming it: the SDF reads `custom5` and the icon `custom3`,
/// because the icon shader declares only this one of the five.
float resolveSymbolOpacity(const vec4 packed) {
    return materialConstants_opacityFromAttribute
               ? unpackMixFloat(packed.xy, materialParams.opacityT)
               : materialParams.opacity;
}
