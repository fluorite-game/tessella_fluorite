# Materials

One `.mat` per shader family, compiled to `.filamat` with Filament's `matc` and loaded by
`FilamentRenderer` from a directory the caller names. The file stem is the family: `familyOf` in
`filament_renderer.cc` maps `circle.filamat` to `TSL_BUILTIN_CIRCLE_SHADER`, and a family with no
package is reported as `missing_family_<n>` rather than drawn wrong.

Compile them all with:

    for m in materials/*.mat; do
      matc -a vulkan -p all -o "$out/$(basename "${m%.mat}").filamat" "$m"
    done

`-a vulkan -p all` because the consumer runs a Vulkan backend and the packages are loaded at
runtime rather than linked, so every platform variant has to be in the file.

The families here are the ones a real basemap asks for. Each is a port of the matching pair in
maplibre-native's `shaders/`, and the comment at the top of each says which.
