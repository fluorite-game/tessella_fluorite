# tessella_harbor

Six styles over Victoria Harbour, six platform views, one engine, one tile store.

Every pane looks at the same water and reads the same archive. What differs is the style, which
is the point: the producer, the tile store and the consumer are shared, and what reaches the
screen is decided by the style alone.

| pane | style | what it exercises |
| --- | --- | --- |
| day | `styles/day.json` | fill, fill-outline, line, line-dash, fill-extrusion with real heights, circle, symbol on a line, rich text (bilingual) |
| night | `styles/night.json` | the same geometry dark: a blurred glow pass under a core pass, a height ramp from steel to gold, blurred circles pitch-aligned to the map |
| heatmap | `styles/heat.json` | heatmap over the `pois` layer, weighted by kind and ramped over density |
| terrain | `styles/terrain.json` | a `raster-dem` source, color-relief and hillshade over real elevation, labels placed on the surface |
| patterns | `styles/patterns.json` | background-pattern, fill-pattern, fill-extrusion-pattern, line-pattern, two dash arrays, sprite icons with offset labels |
| globe | `styles/globe.json` | the same families on a sphere, over the Pearl River Delta |

## A style per pane

`TessellaMaps.install` is idempotent; the configure under it is not. The extension reads the
configured style when a view *attaches*, so the app installs one style, waits for that pane to
attach, and installs the next. Six styles, one process, no second engine.

The gate is the previous pane being attached rather than a timer: two views that attached between
one configure and the next would both read the same style, and the pane that lost would be a
duplicate with nothing to say why.

## Running it

Two servers, both from `maplibre-frontend/tileserver`:

    ./serve.sh &            # hongkong_z15.pmtiles on 8080
    python3 assets.py &     # glyphs, sprites and generated raster tiles on 8081

The archive is `hongkong_z15.pmtiles`, 113.55,21.95 - 114.65,22.95, zooms 0-15. No pane goes past
fifteen: above it a map draws overzoomed tiles, and the detail it appears to gain is the
renderer's rather than the data's.

Then bundle and run, per the quad example's notes:

    dart run bin/emb.dart bundle -a <this dir> -m debug -o <out> --build
    TESSELLA_STYLE_DIR=<this dir>/styles TESSELLA_MATERIALS=<filamat dir> \
    LD_LIBRARY_PATH=<out>/lib homescreen -b <out> --backend=wayland-vulkan -w 1920 --height 1150

`TESSELLA_PANES` seeds the count before any view exists. It is not a runtime control here, unlike
the quad's: a slot is its view's creation order and the styles were installed against that order,
so a pane torn down and brought back would get whichever style was configured last.
`TESSELLA_PANE_ONLY=<n>` runs one pane, which is how a fault gets attributed -- the watch
instrumentation's frame counter is process-global, so with several maps the lines interleave and
one label carries a different id per map, which reads exactly like id instability and is nothing
of the kind.

Space pauses the orbit. It is a `HardwareKeyboard` handler rather than a focused widget: a
platform view takes focus the moment the window is clicked or moved, after which a focused
listener hears nothing and there is no way to tell that from a wedged app.

One orbit is twenty-five seconds. At ninety a pane turned about a third of a degree a second,
which is below the rate at which anything reads as motion -- the map looked stopped, and a real
pause was indistinguishable from it.

## Two things the styles do on purpose

**Extrusions filter `kind == "building"`.** Protomaps carries `building_part` as well, and a part
subdivides a building and shares its footprint -- two coincident solids in the depth buffer. At
rest one wins consistently; under rotation the winner flips per pixel and every such base
shimmers. Measured by coloring the kinds differently and counting pixels that change class
between two frames 0.05 degrees apart: 5877 building-to-part flips, against 0 with parts
excluded.

**Place labels are ranked and filtered.** Nothing ranked them, so which won a contested spot came
down to the offer order -- and that order rotates with the bearing, because `placement_tile_order`
rotates each tile's position. A `symbol-sort-key` makes the priority explicit, and the z13 pane
drops the ranks a real style would not draw there. It took the churn from 2.69 to 1.81 label
swaps a second; the residue is mbgl's own behavior under a camera that never stops turning.

## Terrain

`styles/terrain.json` drapes a `raster-dem` source over the harbor -- color-relief, hillshade,
the water and roads from the vector source, and place labels -- with `terrain.exaggeration` at
1.3. The DEM is AWS's open terrarium tiles, served locally like everything else.

Two things it needs, and one defect it found:

- **An overzoomed `raster-dem` does not draw.** With the source at `maxzoom` 12 and the pane at
  z14.6 the near field came out blank while the far field was fine, and a 90-second budget with
  `image_stable 1` ruled out starvation. Native z13 and z14 tiles for the pane's own area fixed
  it -- and fixed the spikes with it: at z13 the mesh could not resolve a 67 m hill and pulled
  one vertex into a needle. Keep the camera at or below the DEM's own maxzoom.
- **Terrarium carries bathymetry.** The sea floor runs to -208 m off Hong Kong, and left alone
  the shoreline is a cliff whose face the hillshade lights white. The tiles are clamped to zero
  below sea level.
- **The water and roads are vector layers again** as of the stencil fix. Before it, a
  `color-relief` or `hillshade` layer clipped every draped fill and line away -- a water fill
  painted 817 pixels of 336,000 with a relief layer present and 107,975 with the stencil
  disabled -- so this style drew its sea from the elevation ramp instead and left the roads out.
  Both are honest layers now, and the coastline is the water geometry rather than a threshold.

## What is not here

A seventh pane. The shell refuses past thirty-two timers and dies; seven platform views reach
that even with one polling timer for the whole app, so the set is six and `raster.json` sits
out. `styles/raster.json` is still here and still works -- raster under vector, with opacity,
saturation, contrast, hue-rotate and the brightness clamp -- it is simply not in the default set.
Point `TESSELLA_PANE_ONLY` at it, or swap it back into `kPanes`.
