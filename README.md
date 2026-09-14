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
