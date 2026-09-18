// SPDX-License-Identifier: Apache-2.0
//
// One map, ticked and drained into batched draws.

#ifndef TSF_HOST_H
#define TSF_HOST_H

#include <tsf/drawlist.h>
#include <tsf/reader.h>

#include <tessella.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tsf {

/// What a backend does with a frame.
///
/// Deliberately not Filament. The chain from a map to a batch is renderer-agnostic and testable
/// without a GPU, and keeping the join free of Filament is what lets it be tested at all -- the
/// Filament backend implements this, and so could any other.
///
/// The calls arrive in the order a frame is emitted: geometry appears, then the batches that draw
/// it, then retirements. Everything handed over is borrowed for the duration of the call.
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void beginFrame(std::uint64_t /*frameNo*/) {}
    virtual void endFrame(std::uint64_t /*frameNo*/) {}

    /// Geometry appeared, with its vertices and indices borrowed from the producer's arena.
    ///
    /// A backend that needs the bytes past this call copies them or holds the slab, and holding
    /// is what §11.7 asks for.
    virtual void onGeometry(const DrawableAdd& /*add*/) {}

    /// Geometry retired. Whatever was uploaded for it can go.
    virtual void onRetire(std::uint64_t /*id*/) {}

    /// The camera this frame's batches are drawn under.
    ///
    /// Arrives before the batches, because it decides which material each of them takes: under
    /// `TSL_PROJECTION_MODE_GLOBE` a drawable's matrix reaches normalized Mercator rather than clip
    /// space, and the bend from there is the vertex stage's. A backend that ignores this draws a
    /// flat map for a producer that asked for a round one, and the producer cannot tell.
    virtual void onCamera(const tsl_camera_update& /*camera*/) {}

    /// A run of drawables to issue as one renderable, in painter order.
    virtual void onBatch(const Batch& /*batch*/) {}

    /// Uniform bytes for a layer's consolidated buffer.
    virtual void onUniforms(const UboUpdate& /*update*/) {}

    /// Pixels for a texture, whole or in rects.
    virtual void onTexture(const TextureUpdate& /*update*/) {}

    /// A view that draws into a texture rather than onto the screen (DR-25).
    ///
    /// A heatmap's kernels go into one of these and its second pass reads what they summed. A
    /// backend that ignores this draws the kernels onto the map instead of through the ramp,
    /// which is a picture rather than an error -- so ignoring it is not a safe default, and a
    /// backend that cannot render to a texture is better off drawing neither pass.
    virtual void onViewTarget(const tsl_view_target& /*target*/) {}

    /// The tiles a layer group wants clipped to.
    ///
    /// §11.7's clip obligation. A parent tile is drawn to fill what its children have not covered
    /// yet, and this is the mask that confines it to that region -- without it the parent paints
    /// its whole extent, over the children that replaced it.
    virtual void onStencilTiles(const StencilTiles& /*tiles*/) {}
};

/// Owns a map and turns its ticks into batched draws.
///
/// # What it joins
///
/// A `tessella_map` publishes records to a ring; `Reader` turns those into drawables; `DrawList`
/// groups a frame's order into batches. Each of those is useful and tested on its own, and none
/// of them is a map on screen. This is the piece that runs them as one thing.
///
/// # What it does not do
///
/// It does not advance the ring's tail on its own. §11.7 requires slab references be released
/// only after the driver's copy completes, and only a backend knows when that is -- so `tick`
/// reports how far it read and `retire` is a separate call the backend makes when the bytes are
/// genuinely free. A host that never retires stalls the producer, which is the correct failure:
/// the alternative is the producer reusing bytes the GPU is still reading.
class Host {
public:
    /// Creates a map from a style document. Parses it, and does no network.
    ///
    /// Returns null and fills `error` when the style does not parse. Every other failure is a
    /// question for `readiness`, because it cannot be known yet -- see `tessella_create`.
    static std::unique_ptr<Host> create(const tessella_config& config,
                                        double latitude,
                                        double longitude,
                                        double zoom,
                                        std::string* error);

    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Moves the camera. Does not draw.
    bool setCamera(double latitude, double longitude, double zoom, double bearing, double pitch);

    /// Sets the surface the map's tiles are drawn on.
    ///
    /// A toggle rather than a mode the map was created in. The producer's whole part is two
    /// matrices and a flag; the bend from normalized Mercator onto the sphere belongs to the
    /// renderer's vertex stage, so a `Renderer` that ignores `onCamera` draws a flat map for a
    /// producer that asked for a round one, and neither side can tell.
    ///
    /// A globe almost always wants `TESSELLA_WORLD_COPIES_ONE` alongside it: every wrap of a tile
    /// bends to the same patch, so a repeated cover draws that patch twice and z-fights with
    /// itself. Two calls rather than one, because one setting silently moving another is worse.
    bool setProjection(tessella_projection projection);

    /// Sets how many copies of the world the cover asks for.
    bool setWorldCopies(tessella_world_copies copies);

    /// Replaces the map's annotations from a GeoJSON feature collection.
    ///
    /// An annotation is not a style layer and no stylesheet can produce one; the source and the
    /// layers it draws through are synthesized into the style the map renders. Geometry type
    /// picks the class -- point, line, polygon -- and a feature's `icon`, `opacity`, `width`,
    /// `color` and `outlineColor` are read.
    ///
    /// Before the first `tick`. The layers are synthesized during source resolution, which the
    /// first tick starts and which happens once.
    bool setAnnotations(std::string_view geojson);

    /// Replaces a GeoJSON source's data, by the id the style gives it.
    ///
    /// GL JS's `map.getSource(id).setData(...)`. The source's options stay the style's --
    /// clustering, its radius and its maximum zoom -- because they describe the source rather
    /// than the data, and every tile of that source is built again for the next frame.
    ///
    /// After the style has resolved, unlike `setAnnotations`: being callable on a running map is
    /// the point of it. Answers false with `TESSELLA_NOT_RESOLVED` before then.
    bool setGeojsonData(std::string_view source, std::string_view geojson);

    /// Adds an image the style's `icon-image` and `*-pattern` can name.
    ///
    /// GL JS's `map.addImage(id, image)`. The picture joins the style's own sheet, under a name
    /// any layer can ask for, and a style with no sprite at all can still have images this way.
    /// `sdf` says the picture is a signed distance field, which is what lets `icon-color`
    /// recolour it.
    ///
    /// Distinct from `addAnnotationImage`, which adds an image an *annotation* names. After the
    /// style has resolved; an icon is laid out against the sheet per frame, so an image that
    /// arrives late costs a relayout and no tile is rebuilt.
    bool addImage(std::string_view id, std::string_view image, double pixelRatio = 1.0,
                  bool sdf = false);

    /// Adds an encoded image a symbol annotation's `icon` can name.
    ///
    /// `default_marker` is the id an annotation with no icon asks for. Before the first `tick`,
    /// for the reason `setAnnotations` gives.
    bool addAnnotationImage(std::string_view id, std::string_view image, double pixelRatio = 1.0,
                            bool sdf = false);

    /// Tells the map its viewport changed. The cover, the projection and every
    /// screen-space placement follow from it, so this is what a resize is; the
    /// map keeps its tiles and its camera.
    bool setViewport(std::uint32_t width, std::uint32_t height);

    /// Ticks the map and drives `renderer` with whatever it published.
    ///
    /// Returns the ring position the renderer has now been shown, to be handed back to `retire`
    /// once the backend is done with those bytes. Cheap when nothing changed: a settled map
    /// publishes nothing and this walks no records.
    /// Passes the frame's elapsed time to the producer, for the label fades.
    void advance(double elapsed_millis);

    std::uint64_t tick(Renderer& renderer);

    /// Releases everything the producer wrote below `upTo`.
    void retire(std::uint64_t upTo);

    /// How far along the map's sources are, and why if they failed.
    tessella_readiness readiness(std::string* reason = nullptr) const;

    /// How much work is still in flight: tiles asked for and not yet answered, plus an unfinished
    /// glyph fetch. Zero means nothing further arrives without another tick.
    [[nodiscard]] std::uint64_t pending() const;

    /// How far the slab region extends, in bytes: the producer's bump cursor,
    /// which is the high-water mark of live geometry plus whatever compaction
    /// has not yet reclaimed. What a caller sizes `slab_capacity` from.
    [[nodiscard]] std::uint64_t slabUsed() const;

    /// The most bytes the ring has ever held unread, and its capacity.
    ///
    /// Taken inside `tick`, between the producer publishing a frame and this draining it, which
    /// is the only moment the figure means anything: after the drain the ring is empty. What a
    /// caller sizes `ring_capacity` from -- a ring is one frame's records, not a buffer, and
    /// nothing had measured which.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> ringPeak() const noexcept {
        return {ringPeak_, ringCapacity_};
    }

    /// Bytes the region's table still claims, and how many slabs claim them.
    ///
    /// Against `slabUsed` this separates the two ways a region fills: live geometry that is
    /// genuinely held, and dead space under a bump cursor that nothing has reclaimed.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> slabOccupancy() const;

    /// Frame orders dropped for want of a camera at their epoch.
    ///
    /// `beginFrame` has already cleared the scene by the time an order is refused, so each one of
    /// these is a frame that draws nothing.
    [[nodiscard]] std::uint64_t orphanedOrders() const noexcept { return orderCounts_[0]; }

    /// Entries in the last order that reached the draw list, and batches it made of them.
    /// Together with the renderable count these say where a blank frame lost its drawables:
    /// an empty order, an order that batched to nothing, or batches the renderer refused.
    /// What `lastOrderEntries` reports for a frame that carried no order record at all.
    static constexpr std::uint64_t kNoOrder = ~0ull;

    [[nodiscard]] std::uint64_t lastOrderEntries() const noexcept { return orderCounts_[1]; }
    [[nodiscard]] std::uint64_t lastBatches() const noexcept { return orderCounts_[2]; }

    /// The last status any call returned, for a caller that wants the producer's own word.
    [[nodiscard]] tessella_result lastResult() const noexcept { return last_; }

    /// How many records have been read since the map was created.
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }

    /// Nanoseconds the last `tick` spent producing and draining, split at the
    /// FFI boundary: `produceNs` is `tessella_tick`, which covers cover, layout
    /// and placement; `drainNs` is walking the ring into the renderer, which
    /// covers buffer uploads and scene edits. A tick that is slow says nothing
    /// about which half without these, and the two are tuned separately.
    ///
    /// Two clock reads per tick, against a tick measured in milliseconds.
    [[nodiscard]] std::uint64_t produceNs() const noexcept { return produceNs_; }
    [[nodiscard]] std::uint64_t drainNs() const noexcept { return drainNs_; }

private:
    explicit Host(tessella_map* map) noexcept : map_(map) {}

    tessella_map* map_ = nullptr;
    std::unique_ptr<Reader> reader_;
    DrawList drawlist_;
    tessella_result last_ = TESSELLA_OK;
    std::uint64_t records_ = 0;
    std::uint64_t produceNs_ = 0;
    std::uint64_t ringPeak_ = 0;
    std::uint64_t ringCapacity_ = 0;
    std::uint64_t drainNs_ = 0;
    std::uint64_t orderCounts_[3] = {0, 0, 0};
};

} // namespace tsf

#endif // TSF_HOST_H
