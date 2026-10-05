# tessella_fluorite

Draws a [tessella](https://github.com/jwinarske/tessella) map into a running
[fluorite](https://github.com/jwinarske/fluorite) scene.

A Dart package whose native half is a code asset. A map is addressed by name rather than by ECS
entity, and the renderer is fluorite's — taken from it rather than stood up a second time
alongside it.

## Written against the stream

tessella does not hand over a scene to mirror. It emits a capture stream: geometry announcements,
per-view uses, uniform blocks, textures, and a draw order. This package consumes that directly,
and the shape of it follows from what the stream already guarantees.

- **Geometry is announced once and used per view.** Four views over one tile send one
  announcement and four uses, so a consumer holds one set of GPU buffers and four draw lists.
- **Painter order arrives as a flat array, already ordered**, with the uniform index each entry
  binds by. There is nothing to sort.
- **Bytes live in a slab region with a lifetime protocol.** The producer will not reuse a slab
  until the consumer acknowledges having uploaded past the announcement that named it, so
  buffers upload from the producer's own memory and nothing is copied to make it safe.
- **Traffic is proportional to change.** A still map sends nothing, so there is no per-frame
  diffing for a consumer to do.

Drawables are batched by layer, shader permutation and texture set, and issued in the order the
producer computed. Nothing re-derives an ordering the stream carried.

## Draw order beside ECS content

A map and the Flutter app's own ECS content share one `filament::Scene`, and
Filament's `Renderable` priority is what orders them. There are eight bands and
the map holds seven:

| band | what |
| --- | --- |
| 0 | the globe's depth shell, "the ground beneath the ground" |
| 1-4 | the stencil clip masks, by zoom, coarsest first -- every mask has to be written before any geometry tests against it |
| 5 | the map's opaque pass |
| 6 | the map's blended pass |
| 7 | **free** |

So **blended ECS content that must appear over the map takes `priority: 7`.** At
Fluorite's default of 4 it draws before the map's blended pass and the map paints
over it -- which is not a bug in either side, just the band order.

Two things that follow, and neither is obvious from the table:

- **Band 7 is the only one left, so priority cannot order ECS content against
  *itself* there.** Several pieces of blended content in band 7 are sorted
  back-to-front by distance, which is right until two of them are coplanar or
  want a fixed stacking -- a HUD over a marker over a route ribbon. That is what
  `Renderable.blendOrder` is for; see its doc comment for why the local mode only
  breaks ties at one distance and what `globalBlendOrder` costs.
- **Fluorite's default priority of 4 is inside the map's mask range.** ECS content
  left at the default therefore interleaves with stencil mask writes. It is
  harmless today -- the masks are on the map's own layer and ECS geometry does not
  test against them -- but it means the default sits in map infrastructure rather
  than beside the map's geometry, so do not read band 4 as "below the map".

Band 7 is free here because the platform-view seam leaves `SkyboxSystem` out:
Filament gives a skybox the lowest priority by convention, and Fluorite omits the
system because its default opaque white would fill the transparent background this
seam composites over. A view that did add a skybox would share band 7 with it.

## Status

Draws background, fill, fill-extrusion, line, circle, symbol, raster and heatmap layers, over
vector, raster and GeoJSON sources. Every family but the heatmap also draws on a globe.

Visual parity is measured in gross pixels — per-channel difference over 48 — by the harness in
tessella's `tools/parity`. One layer of every family over Berlin holds at **24, 45, 5, 52 and 30
differing pixels** across five cameras — z14 and z16 at pitch 0 and 60 in 1024x768, and z9 in
2400x900. A heatmap over the same city holds at **0**.

Four maps run on one engine, each in its own view, sharing one tile store. The quad runs on
desktop and on a Raspberry Pi 4 and 5.

## License

Apache-2.0. See [LICENSE](LICENSE).
