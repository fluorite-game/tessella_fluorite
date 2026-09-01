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

    /// A run of drawables to issue as one renderable, in painter order.
    virtual void onBatch(const Batch& /*batch*/) {}

    /// Uniform bytes for a layer's consolidated buffer.
    virtual void onUniforms(const UboUpdate& /*update*/) {}

    /// Pixels for a texture, whole or in rects.
    virtual void onTexture(const TextureUpdate& /*update*/) {}

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

    /// Ticks the map and drives `renderer` with whatever it published.
    ///
    /// Returns the ring position the renderer has now been shown, to be handed back to `retire`
    /// once the backend is done with those bytes. Cheap when nothing changed: a settled map
    /// publishes nothing and this walks no records.
    std::uint64_t tick(Renderer& renderer);

    /// Releases everything the producer wrote below `upTo`.
    void retire(std::uint64_t upTo);

    /// How far along the map's sources are, and why if they failed.
    tessella_readiness readiness(std::string* reason = nullptr) const;

    /// The last status any call returned, for a caller that wants the producer's own word.
    [[nodiscard]] tessella_result lastResult() const noexcept { return last_; }

    /// How many records have been read since the map was created.
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }

private:
    explicit Host(tessella_map* map) noexcept : map_(map) {}

    tessella_map* map_ = nullptr;
    std::unique_ptr<Reader> reader_;
    DrawList drawlist_;
    tessella_result last_ = TESSELLA_OK;
    std::uint64_t records_ = 0;
};

} // namespace tsf

#endif // TSF_HOST_H
