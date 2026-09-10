// SPDX-License-Identifier: Apache-2.0
//
// Data-driven paint, decoded the way mbgl encodes it.
//
// A paint property is either the layer's, uniform for every feature in it, or the feature's, which
// puts it in the vertex buffer. mbgl compiles a shader per combination and branches on
// `HAS_UNIFORM_u_color`; Filament resolves the same branch from a specialization constant, so one
// package serves every permutation -- see `native/test/permutation_probe.cc` for why that is the
// only mechanism that scales here.
//
// # Two values per property, not one
//
// A property that also varies with *zoom* carries both endpoints in the vertex and one mix factor
// per frame in the drawable UBO, which is what keeps a zoom change from rebuilding the buffer.
// A property that varies only per feature carries one endpoint and a factor of zero. The shader
// cannot tell the two apart and does not need to: it always reads the wide form and always mixes,
// and a factor of zero returns the first endpoint.
//
// The wide read is why the consumer pads a paint slab to the declared width. The buffer supplies
// the narrow form at the narrow stride, so the last vertex's wide read runs past the end of what
// the producer sent.
//
// # Colours are two floats, not four
//
// `packUint8Pair(a, b) = a * 256 + b` over `255 * component`, so a colour is two floats and a
// zoom-varying one is four. `unpackFloat` is `unpack_float` from mbgl's `common.hpp`, integer
// division included: the halves are exact 8-bit values and float division would round the wrong
// one at the boundary.

vec2 unpackFloat(const float packed) {
    int value = int(packed);
    int high = value / 256;
    return vec2(high, value - high * 256);
}

vec4 decodeColor(const vec2 encoded) {
    return vec4(unpackFloat(encoded[0]) / 255.0, unpackFloat(encoded[1]) / 255.0);
}

/// mbgl's `unpack_mix_color`: two packed colours and the frame's mix factor.
vec4 unpackMixColor(const vec4 packed, const float t) {
    return mix(decodeColor(packed.xy), decodeColor(packed.zw), t);
}

/// mbgl's `unpack_mix_float`: two plain scalars and the frame's mix factor.
float unpackMixFloat(const vec2 packed, const float t) {
    return mix(packed[0], packed[1], t);
}
