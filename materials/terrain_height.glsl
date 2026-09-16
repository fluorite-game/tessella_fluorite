// SPDX-License-Identifier: Apache-2.0
//
// The height of the ground under a tile-local position, for a family the terrain raises.
//
// Every raised variant reads the same number the same way, out of the same block, so it is
// written once here rather than copied into each of them. `terrain.mat` is the definition this
// follows: the encoding's dot product over channels scaled back to bytes, times the style's
// exaggeration.
//
// Reads `materialParams_elevation`, `materialParams.unpack` and `materialParams.params` -- the
// raise block the producer sends to every drawable it marked `ON_TERRAIN`, whatever family it
// is. A material that includes this declares all three.
//
// The one family that cannot use it is the color relief, whose own picture *is* the raw
// elevation and which therefore samples `image` instead; the arithmetic there is this
// arithmetic, spelled out against the other sampler.
//
// The skirt is not here. Only the ground has one -- a layer standing on the ground is a surface
// on a surface -- so `terrain.mat` subtracts it after this returns.

float terrainHeight(vec2 position) {
    // One multiply-add an axis: the producer folded the DEM's width, its border pixel, the half
    // texel that centers a cell, and the square of a coarser DEM this tile sits in, into these
    // three numbers. See `terrain_ubo`.
    vec2 uv = position * materialParams.params.x + materialParams.params.yz;
    vec3 channels = texture(materialParams_elevation, uv).rgb * 255.0;
    float meters = channels.r * materialParams.unpack.r
                 + channels.g * materialParams.unpack.g
                 + channels.b * materialParams.unpack.b
                 - materialParams.unpack.a;
    return meters * materialParams.params.w;
}
