// SPDX-License-Identifier: Apache-2.0
//
// How a hillshade lights its slopes: mbgl's five methods, over up to four lights.
//
// `hillshade-method` chooses one, and the numbering is mbgl's `HillshadeMethodType` rather than
// the spec's listing order: standard, combined, igor, multidirectional, basic. Only the
// multidirectional one reads more than the first light; the other four take a single azimuth and
// altitude and ignore the rest of the block.
//
// Written once here because two materials shade the same way -- `hillshade.mat` on the plane and
// `hillshade_terrain.mat` on the ground -- and the only difference between them is where the
// quad's corners are, which is the vertex stage's business and not this.
//
// The arithmetic is transcribed from `hillshade.fragment.glsl` rather than derived. Two parts of
// it look wrong and are not: the standard method reads the *exaggeration* as an intensity, which
// mbgl's own comment calls out and does anyway, and the multidirectional one negates its azimuth
// where the others add pi. Changing either would be a different picture from the oracle's, which
// is the one thing this cannot be.
//
// Reads `materialParams.exaggeration`, `.accent`, `.method`, `.numLights`, `.altitudes`,
// `.azimuths` and the eight named light colours. A material that includes this declares all of
// them.

    // Filament already defines `PI` in its own math header, so this one is named apart.
    const float kPi = 3.141592653589793;

    // mbgl's own numbering, from `HillshadeMethodType`.
    const int kStandard = 0;
    const int kCombined = 1;
    const int kIgor = 2;
    const int kMultidirectional = 3;
    const int kBasic = 4;

    // `atan(y, x)` is undefined at the origin, so a purely vertical derivative is answered
    // directly rather than asked for.
    float aspectOf(vec2 deriv) {
        return deriv.x != 0.0
            ? atan(deriv.y, -deriv.x)
            : kPi / 2.0 * (deriv.y > 0.0 ? 1.0 : -1.0);
    }

    // A light's own azimuth and altitude, by index. Written out rather than subscripted: a
    // vector indexed by a non-constant is not portable across the shader models this compiles
    // for, which is why mbgl's shader spells it the same way.
    float lightAt(vec4 four, int index) {
        return index == 0 ? four.x : index == 1 ? four.y : index == 2 ? four.z : four.w;
    }

    // And a light's own two colours, for the same reason and by the same shape.
    vec4 shadowAt(int index) {
        return index == 0 ? materialParams.shadow0
             : index == 1 ? materialParams.shadow1
             : index == 2 ? materialParams.shadow2
                          : materialParams.shadow3;
    }

    vec4 highlightAt(int index) {
        return index == 0 ? materialParams.highlight0
             : index == 1 ? materialParams.highlight1
             : index == 2 ? materialParams.highlight2
                          : materialParams.highlight3;
    }

    // mbgl's legacy algorithm, and the spec's default.
    //
    // The exaggeration is read as an intensity here, not as a multiplier on the slope: the
    // slope is bent exponentially by it, base under one steepening the response and over one
    // flattening it. At exactly a half the base is one and the expression is 0/0, which is why
    // mbgl tests for it rather than letting it divide.
    vec4 standardHillshade(vec2 deriv) {
        float azimuth = materialParams.azimuths.x + kPi;
        float slope = atan(0.625 * length(deriv));
        float aspect = aspectOf(deriv);
        float intensity = materialParams.exaggeration;

        float base = 1.875 - intensity * 1.75;
        float maxValue = 0.5 * kPi;
        float scaledSlope = abs(intensity - 0.5) > 1e-6
            ? ((pow(base, slope) - 1.0) / (pow(base, maxValue) - 1.0)) * maxValue
            : slope;

        float accent = cos(scaledSlope);
        vec4 accentColor = (1.0 - accent) * materialParams.accent
                         * clamp(intensity * 2.0, 0.0, 1.0);

        // Which way this face turns relative to the light, folded into [0, 1]: the `mod` by two
        // and the distance from one make the shading symmetric about the light's axis.
        float shade = abs(mod((aspect + azimuth) / kPi + 0.5, 2.0) - 1.0);
        vec4 shadeColor = mix(materialParams.shadow0, materialParams.highlight0, shade)
                        * sin(scaledSlope) * clamp(intensity * 2.0, 0.0, 1.0);

        return accentColor * (1.0 - shadeColor.a) + shadeColor;
    }

    // How much of the light a face turned this way catches, in [0, 1]. The four methods below
    // that take a single light all start here; only the sign of the azimuth differs, which is
    // what `turn` carries.
    float lambert(vec2 deriv, float azimuth, float altitude, float turn) {
        float cosAz = turn * cos(azimuth);
        float sinAz = turn * sin(azimuth);
        float cosAlt = cos(altitude);
        float sinAlt = sin(altitude);
        return (sinAlt - (deriv.y * cosAz * cosAlt - deriv.x * sinAz * cosAlt))
             / sqrt(1.0 + dot(deriv, deriv));
    }

    // One light, shading and highlighting from the middle of the range outwards.
    vec4 basicHillshade(vec2 deriv) {
        deriv = deriv * materialParams.exaggeration * 2.0;
        float shade = clamp(
            lambert(deriv, materialParams.azimuths.x + kPi, materialParams.altitudes.x, 1.0),
            0.0, 1.0);
        return shade > 0.5
            ? materialParams.highlight0 * (2.0 * shade - 1.0)
            : materialParams.shadow0 * (1.0 - 2.0 * shade);
    }

    // Up to four lights, each contributing its share of the whole.
    //
    // The azimuth is negated here where every other method adds pi to it. That is mbgl's, and
    // the two are not the same turn: `-cos(a)` is `cos(a + pi)` but `-sin(a)` is not
    // `sin(a + pi)`, so this light points somewhere the others do not.
    vec4 multidirectionalHillshade(vec2 deriv) {
        deriv = deriv * materialParams.exaggeration * 2.0;
        vec4 lit = vec4(0.0);
        for (int i = 0; i < 4; i++) {
            if (i >= materialParams.numLights) break;
            float shade = clamp(
                lambert(deriv,
                        lightAt(materialParams.azimuths, i),
                        lightAt(materialParams.altitudes, i),
                        -1.0),
                0.0, 1.0);
            float share = 1.0 / float(materialParams.numLights);
            lit += shade > 0.5
                ? highlightAt(i) * (2.0 * shade - 1.0) * share
                : shadowAt(i) * (1.0 - 2.0 * shade) * share;
        }
        return lit;
    }

    // One light, with the shadow and the highlight both present everywhere and weighted by the
    // angle between the light and the face.
    vec4 combinedHillshade(vec2 deriv) {
        deriv = deriv * materialParams.exaggeration * 2.0;
        float angle = clamp(
            acos(lambert(deriv, materialParams.azimuths.x + kPi, materialParams.altitudes.x, 1.0)),
            0.0, kPi / 2.0);
        float steepness = atan(length(deriv)) * 4.0 / kPi / kPi;
        return materialParams.shadow0 * (angle * steepness)
             + materialParams.highlight0 * ((kPi / 2.0 - angle) * steepness);
    }

    // Igor's: the shadow follows how far the face turns away from the light and the highlight
    // takes what is left, both scaled by how steep the face is. No altitude reaches it.
    vec4 igorHillshade(vec2 deriv) {
        deriv = deriv * materialParams.exaggeration * 2.0;
        float aspect = aspectOf(deriv);
        float azimuth = materialParams.azimuths.x + kPi;

        float slopeStrength = atan(length(deriv)) * 2.0 / kPi;
        float aspectStrength = 1.0 - abs(mod((aspect + azimuth) / kPi + 0.5, 2.0) - 1.0);

        return materialParams.shadow0 * (slopeStrength * aspectStrength)
             + materialParams.highlight0 * (slopeStrength * (1.0 - aspectStrength));
    }

    // The method the layer asked for, over the slope field this fragment read.
    vec4 hillshadeLit(vec2 deriv) {
        if (materialParams.method == kBasic) return basicHillshade(deriv);
        if (materialParams.method == kCombined) return combinedHillshade(deriv);
        if (materialParams.method == kIgor) return igorHillshade(deriv);
        if (materialParams.method == kMultidirectional) return multidirectionalHillshade(deriv);
        return standardHillshade(deriv);
    }
