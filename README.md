# tessella_fluorite

Draws a [tessella](https://github.com/jwinarske/tessella) map into a running
[fluorite](https://github.com/jwinarske/fluorite) Filament scene.

Patterned on `maplibre_fluorite`, which does the same job for maplibre-native. The pattern is
worth keeping — a Dart package whose native half is a code asset, a map addressed by name rather
than by ECS entity, Filament taken *from* fluorite rather than linked twice — and the consumer
inside it is not.

## Why this is not a port of `maplibre_fluorite`'s mirror

That mirror consumes mbgl's capture stream, and it is shaped like mbgl. Reading it:

- `entries_` is a `std::map<pair<uint32_t,uint64_t>, Entry>`, so placing a frame's drawables in
  painter order costs a red-black tree descent **per drawable per frame**.
- Every drawable becomes its own Filament `Entity` and `MaterialInstance`, so Filament's scene
  graph culls and sorts on top of a painter order the producer already computed exactly.
- Every uniform block is heap-copied into `layerUniforms_` on arrival and copied again on use.
- Nothing is batched, though the obligation to merge by (layer, shader permutation, texture set)
  is written down.

None of that is fixed by translating the types. It is the shape, and the shape came from mirroring
an engine rather than from consuming a stream. So this is written against the stream.

## What the stream affords that mbgl's did not

- **Geometry is announced once and used per view.** Four views over one tile send one
  announcement and four uses, so a consumer holds one set of GPU buffers and four draw lists.
- **Painter order arrives as a flat array, already ordered**, with the uniform index each entry
  binds by. There is nothing to sort.
- **Bytes live in a slab region with a lifetime protocol.** The producer will not reuse a slab
  until the consumer acknowledges having uploaded past the announcement that named it, so
  buffers upload from the producer's own memory and nothing is copied to make it safe.
- **Traffic is proportional to change.** A still map sends nothing, so there is no per-frame
  diffing for a consumer to do.

## Status

Scaffolding. Nothing draws yet.
