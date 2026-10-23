# tessella_harbor

Six styles over three cities, six platform views, one engine, one tile store.

Every pane looks at the same place and reads the same source. What differs is the style, which
is the point: the producer, the tile store and the consumer are shared, and what reaches the
screen is decided by the style alone.

The wall flies between Hong Kong, Tokyo and San Francisco on a dwell timer, and nothing about a
pane changes when it does -- not its style, not its zoom, not its pitch, not its orbit. Only the
center moves, which is the other half of the same point: a style is not about a place.

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

Four servers. Two are `maplibre-frontend/tileserver`'s and two are here:

    ./serve.sh &                 # the city archives on 8080
    python3 assets.py &          # glyphs, sprites and generated raster tiles on 8081
    python3 tools/cities.py &    # one source over those archives, on 8086
    python3 tools/dem.py &       # terrarium, clamped and cached, on 8085

The archives are `hongkong_z15.pmtiles` (113.55,21.95 - 114.65,22.95),
`tokyo_z15.pmtiles` (139.30,35.30 - 140.30,36.00) and `sanfrancisco_z15.pmtiles`
(-122.60,37.50 - -121.90,38.00), zooms 0-15. No pane goes past fifteen: above it a map draws
overzoomed tiles, and the detail it appears to gain is the renderer's rather than the data's.

Cut another the way the tileserver README does, and name it in `tools/cities.py`:

    ./pmtiles extract https://build.protomaps.com/YYYYMMDD.pmtiles <name>_z15.pmtiles \
        --bbox=<w,s,e,n> --maxzoom=15

`tools/dem.py` wants Pillow and the network on a city's first visit; everything after that is
its cache directory, `HARBOR_DEM_CACHE` or `~/.cache/harbor-dem`.

Then bundle and run, per the quad example's notes:

    dart run bin/emb.dart bundle -a <this dir> -m debug -o <out> --build
    TESSELLA_STYLE_DIR=<this dir>/styles TESSELLA_MATERIALS=<filamat dir> TSF_SLAB_MIB=256 \
    LD_LIBRARY_PATH=<out>/lib homescreen -b <out> --backend=wayland-vulkan -w 1920 --height 1150

`TSF_SLAB_MIB` is not optional once the wall is flying. A pane of this wall is about 640x575, so
`map_view.cc` gives it the 64 MiB floor -- a number chosen for a map that stays where it is. A
camera that crosses the planet every dwell keeps one city's geometry in the slab while the next
is being built, and the day pane, which is the heaviest (z15, 3D buildings, bilingual labels),
saturates: 63.4 of 63.5 MiB, and it stays there.

What that looks like is worth knowing. The pane keeps rendering, the HUD keeps counting frames,
`pending` is 0 -- and the picture is the last city it managed to build, while the camera flies on
without it. The HUD's `state` says so now: `ready · region full`, readiness and the last tick's
result side by side, because they answer different questions and this is the gap between them.
It said only `ready` when this was first hit, which is how it went unnoticed for an afternoon.

At 256 MiB the same pane runs between 42 and 104 MiB live over a full round and never sticks.

`TESSELLA_DWELL_S` is how long the wall stays in one city and `TESSELLA_ORBIT_S` is how long
one orbit takes -- 45 and 25 seconds. A dwell of zero stays in the first city for ever, which is
what this did before cities existed and what a capture or a single-pane investigation wants.

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

## Three things the styles do on purpose

**Extrusions filter `kind == "building"`.** Protomaps carries `building_part` as well, and a part
subdivides a building and shares its footprint -- two coincident solids in the depth buffer. At
rest one wins consistently; under rotation the winner flips per pixel and every such base
shimmers. Measured by coloring the kinds differently and counting pixels that change class
between two frames 0.05 degrees apart: 5877 building-to-part flips, against 0 with parts
excluded.

**Every `fill` over `water` or `earth` filters `geometry-type`.** Protomaps puts linear and
areal features in one source-layer: of the 35 features in `water` on 14/13387/7152, thirty are
LineStrings -- Pokfulam Stream, the Aberdeen catchwaters, the nullahs off The Peak -- against
four polygons and a point. A fill layer closes each of those polylines into a ring and earcuts
it, and a sixty-point stream becomes a self-intersecting bowtie: blue spikes fanning down every
hillside. On `day.json` at z14 over The Peak that was 12.4% of the frame, and water pixels went
from 62,289 to 6,903 with the filter on, leaving the two Aberdeen reservoirs where they belong.
mbgl draws the same spikes from the same tile, so this is the style's error and not a renderer's.
`earth` mixes the same way -- its coastlines are LineStrings -- and is filtered for the same
reason, though it shows nothing here because `water` is painted over it.

**And `water` names the kinds it means.** Geometry type is not the whole of it: of 373 polygon
features in `water` across 99 z14 tiles here, 102 are swimming pools, 25 are fountains and 11
are reefs -- a third of them, painted the same color as Victoria Harbour. The pools are the ones
that read as a renderer fault rather than as data, because a pool is a rectangle and the
terrarium DSM puts Kowloon's podiums on hillsides that do not exist, so they sit on slopes.

An allow-list -- `ocean`, `water`, `lake`, `river`, `dock` -- rather than dropping the two kinds
that were noticed. A deny-list would have left `reef` painted as sea and would let in whatever
Protomaps adds next; the layer is a schema this tree does not control. `earth` needs none: it
carries one kind.

**Place labels are ranked and filtered.** Nothing ranked them, so which won a contested spot came
down to the offer order -- and that order rotates with the bearing, because `placement_tile_order`
rotates each tile's position. A `symbol-sort-key` makes the priority explicit, and the z13 pane
drops the ranks a real style would not draw there. It took the churn from 2.69 to 1.81 label
swaps a second; the residue is mbgl's own behavior under a camera that never stops turning.

## Terrain

`styles/terrain.json` drapes a `raster-dem` source over the harbor -- color-relief, hillshade,
the water and roads from the vector source, and place labels -- with `terrain.exaggeration` at
1.3. The DEM is AWS's open terrarium tiles, served locally like everything else.

Two things it needs, one defect it found, and one that was not a defect:

- **An overzoomed `raster-dem` draws. This bullet used to say it does not** -- that the near
  field came out blank with the source at `maxzoom` 12 and the pane at z14.6, that a 90-second
  budget with `image_stable 1` ruled out starvation, and that the camera had to stay at or below
  the DEM's own maxzoom. It does not reproduce, and the camera is back at z13.

  Measured over four declared maxzooms at the pane's own camera: 15, 14, 13 and 12 leave 5, 2, 0
  and 0 sky pixels of 1,024,000, every frame fully lit. At Berlin, where the renderer's own
  `terrain_cover_p` is held against a magenta background, all four read zero holes at z14 p0 and
  at z16 p45, and the `maxzoom` 12 frame is **0** gross pixels from the `maxzoom` 15 one at the
  48 threshold -- a smoothed version of the same ground, which is what a coarser DEM gives. The
  drawable count does fall, 652 / 424 / 260 / 194 for 15 / 14 / 13 / 12, and that is one coarse
  tile standing in for sixteen rather than anything missing.

  What the blank was, as far as it can be reconstructed: the measurement ran against a
  hand-built archive, before `tools/dem.py` was in the tree, and that archive held **only z13
  and z14 near the harbor** -- the next bullet says so about the same archive. At `maxzoom` 12
  every near-field request is for a z12 tile it did not have, so the near field had no DEM while
  the far field, where its z8-z12 did reach, was fine. That is the symptom exactly, and it is
  why native z13 and z14 tiles for the pane's area fixed it. `tools/dem.py` answers for anywhere
  it is asked, so the gap is not there to fall into.

  The spikes are a separate finding and stand: at z13 the mesh could not resolve a 67 m hill and
  pulled one vertex into a needle, which native tiles under the camera fixed.
- **The DEM extract ends before the horizon does.** The archive is z8-z14 over
  113.203..114.961, 21.943..23.241, and only z13 and z14 near the harbor. The pane orbits at
  pitch 68, which reaches past that: at bearing 195 the cover asks for three tiles south of
  21.943 that do not exist, and with no DEM there is no color-relief and no hillshade, so the
  background shows through the islands beyond Po Toi -- 4,220 pixels of it, against 0 at pitch
  62 and below. Serving a flat sea-level terrarium tile for the 94 misses takes it to 2. This
  style has no land fill under the relief, so any gap in the DEM reads as sky rather than as
  unshaded ground.
- **Terrarium carries bathymetry.** The sea floor runs to -208 m off Hong Kong, and left alone
  the shoreline is a cliff whose face the hillshade lights white. The tiles are clamped to zero
  below sea level.
- **The water and roads are vector layers again** as of the stencil fix. Before it, a
  `color-relief` or `hillshade` layer clipped every draped fill and line away -- a water fill
  painted 817 pixels of 336,000 with a relief layer present and 107,975 with the stencil
  disabled -- so this style drew its sea from the elevation ramp instead and left the roads out.
  Both are honest layers now, and the coastline is the water geometry rather than a threshold.

## What is not here

A seventh pane. Filament's Vulkan backend has a pool of **32 timer queries, process-wide** --
`utils::bitset32` in `VulkanQueryManager` -- and six platform views are six Filament renderers,
which sits just under it. Seven does not, so the set is six and `raster.json` sits out.

Past the pool `getNextQuery` returns an empty handle. `VulkanDriver::createTimerQueryS` took it
unchecked and the process died rather than losing a frame's timing; fluorite patch 0027 checks
it, and it is filed upstream as google/filament#10659. **A build from this tree does not have
that yet** -- the `fluorite` path dependency points at a checkout from before the patch, and the
Filament it links is a staged build from before it as well. Both have to be refreshed.

The cap itself is unchanged and is not configurable, so six is still the limit either way, and
the fix only means a frame loses its timing instead of taking the process down. Moving the
window between outputs re-runs the dma-buf negotiation, which is enough to push the outstanding
count past the ceiling. toyota-connected/ivi-homescreen#815.

An earlier note here blamed the shell's own timer handling -- `shell/timer.cc` has no such
bound, and the message does not come from the shell at all.

`styles/raster.json` is still here and still works -- raster under vector, with opacity,
saturation, contrast, hue-rotate and the brightness clamp -- it is simply not in the default set.
Point `TESSELLA_PANE_ONLY` at it, or swap it back into `kPanes`.
