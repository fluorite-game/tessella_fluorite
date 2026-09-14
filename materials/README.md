# Materials

One `.mat` per shader family, compiled to `.filamat` with `matc` and loaded by the renderer from
a directory the caller names. The file stem is the family: `familyOf` maps `circle.filamat` to
`TSL_BUILTIN_CIRCLE_SHADER`, and a family with no package is reported as `missing_family_<n>`
rather than drawn wrong.

A family may ship three variants. The bare stem draws on a plane; `_globe` bends the placement
onto a sphere directly; `_globe_anchored` replaces that placement with a quadratic expansion
about the tile center, which is what a large tile needs once the direct form's `f32` trig costs
more than a pixel. The loader strips either suffix before looking the family up, so a variant is
added by dropping a file in.

Compile them all with:

    for m in materials/*.mat; do
      matc -a vulkan -p all -o "$out/$(basename "${m%.mat}").filamat" "$m"
    done

`-a vulkan -p all` because the consumer runs a Vulkan backend and the packages are loaded at
runtime rather than linked, so every platform variant has to be in the file.

The families here are the ones a real basemap asks for. The comment at the top of each file says
what it draws and where its arithmetic comes from.
