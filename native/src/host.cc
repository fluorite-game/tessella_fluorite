// SPDX-License-Identifier: Apache-2.0

#include <tsf/host.h>

#include <cstring>
#include <utility>
#include <ctime>

#include <tessella_capture_abi.h>

#include <cstddef>
#include <vector>

namespace tsf {
namespace {

/// Forwards what the reader delivers to a `Renderer`, batching the order on the way through.
class HostSink final : public FrameSink {
public:
    HostSink(Renderer& renderer, DrawList& drawlist, std::uint64_t* orphaned) noexcept
        : renderer_(renderer), drawlist_(drawlist), orphaned_(orphaned) {}

    void beginFrame(std::uint64_t frameNo) override { renderer_.beginFrame(frameNo); }
    void endFrame(std::uint64_t frameNo) override { renderer_.endFrame(frameNo); }

    void onDrawableAdd(const DrawableAdd& add) override {
        drawlist_.observe(add);
        renderer_.onGeometry(add);
    }

    void onDrawableRemove(const DrawableRemove& gone) override {
        // A release names a view and says only that this view has stopped drawing it; the
        // geometry is still there for the others. A remove is the geometry itself going away,
        // and only then is there anything for a backend to free.
        if (gone.view == 0) {
            drawlist_.forget(gone.id);
            renderer_.onRetire(gone.id);
        }
    }

    void onUboUpdate(const UboUpdate& update) override { renderer_.onUniforms(update); }
    void onStencilTiles(const StencilTiles& tiles) override { renderer_.onStencilTiles(tiles); }
    void onTextureUpdate(const TextureUpdate& update) override { renderer_.onTexture(update); }
    void onViewTarget(const tsl_view_target& target) override { renderer_.onViewTarget(target); }

    void onFrameOrder(const FrameOrder& order) override {
        // The camera is the commit point: an order without one names an epoch the producer has
        // not published a camera for, so drawing it would put this frame's geometry under the
        // last frame's camera. Held rather than drawn, and the next order supersedes it.
        if (!order.camera) {
            if (orphaned_ != nullptr) {
                (*orphaned_)++;
            }
            return;
        }
        // An order with no entries is a camera arriving for an order this reader has not seen,
        // not a frame that draws nothing. Rebuilding the scene from it would empty the screen.
        if (order.entries.empty()) {
            if (orphaned_ != nullptr) {
                orphaned_[1] = 0;
                orphaned_[2] = 0;
            }
            return;
        }
        // Before the batches: the projection decides which material each of them takes.
        renderer_.onCamera(*order.camera);
        const std::vector<Batch> batches = drawlist_.build(order);
        if (orphaned_ != nullptr) {
            orphaned_[1] = order.entries.size();
            orphaned_[2] = batches.size();
        }
        for (const Batch& batch : batches) {
            renderer_.onBatch(batch);
        }
    }

private:
    Renderer& renderer_;
    DrawList& drawlist_;
    std::uint64_t* orphaned_;
};

} // namespace

std::unique_ptr<Host> Host::create(const tessella_config& config,
                                   double latitude,
                                   double longitude,
                                   double zoom,
                                   std::string* error) {
    tessella_map* map = nullptr;
    const tessella_result result = tessella_create(&config, latitude, longitude, zoom, &map);
    if (result != TESSELLA_OK || map == nullptr) {
        if (error != nullptr) {
            *error = result == TESSELLA_BAD_STYLE ? "the style did not parse"
                                                  : "the map could not be created";
        }
        // A failed create hands back no handle, so there is nothing to destroy.
        return nullptr;
    }
    return std::unique_ptr<Host>(new Host(map));
}

Host::~Host() {
    if (map_ != nullptr) {
        tessella_destroy(map_);
    }
}

bool Host::setCamera(double latitude, double longitude, double zoom, double bearing, double pitch) {
    last_ = tessella_set_camera(map_, latitude, longitude, zoom, bearing, pitch);
    return last_ == TESSELLA_OK;
}

bool Host::setProjection(tessella_projection projection) {
    last_ = tessella_set_projection(map_, projection);
    return last_ == TESSELLA_OK;
}

bool Host::setWorldCopies(tessella_world_copies copies) {
    last_ = tessella_set_world_copies(map_, copies);
    return last_ == TESSELLA_OK;
}

bool Host::setAnnotations(std::string_view geojson) {
    last_ = tessella_set_annotations(
        map_, reinterpret_cast<const std::uint8_t*>(geojson.data()), geojson.size());
    return last_ == TESSELLA_OK;
}

bool Host::setGeojsonData(std::string_view source, std::string_view geojson) {
    last_ = tessella_set_geojson_data(
        map_, reinterpret_cast<const std::uint8_t*>(source.data()), source.size(),
        reinterpret_cast<const std::uint8_t*>(geojson.data()), geojson.size());
    return last_ == TESSELLA_OK;
}

bool Host::addImage(std::string_view id, std::string_view image, double pixelRatio, bool sdf) {
    last_ = tessella_add_image(map_, reinterpret_cast<const std::uint8_t*>(id.data()), id.size(),
                               reinterpret_cast<const std::uint8_t*>(image.data()), image.size(),
                               pixelRatio, sdf);
    return last_ == TESSELLA_OK;
}

bool Host::addAnnotationImage(std::string_view id, std::string_view image, double pixelRatio,
                              bool sdf) {
    last_ = tessella_add_annotation_image(
        map_, reinterpret_cast<const std::uint8_t*>(id.data()), id.size(),
        reinterpret_cast<const std::uint8_t*>(image.data()), image.size(), pixelRatio, sdf);
    return last_ == TESSELLA_OK;
}

bool Host::setViewport(std::uint32_t width, std::uint32_t height) {
    last_ = tessella_set_viewport(map_, width, height);
    return last_ == TESSELLA_OK;
}

namespace {

std::uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace

void Host::advance(double elapsed_millis) {
    tessella_advance(map_, elapsed_millis);
}

std::uint64_t Host::tick(Renderer& renderer) {
    // A sentinel, so "this frame carried no order" is distinguishable from "its order was
    // empty". Read back stale, the two look identical and only one of them is a bug.
    orderCounts_[1] = kNoOrder;
    orderCounts_[2] = kNoOrder;
    const std::uint64_t producing = now_ns();
    last_ = tessella_tick(map_);
    produceNs_ = now_ns() - producing;
    drainNs_ = 0;
    // TESSELLA_RING_FULL means nothing was emitted and nothing retired, so draining is still the
    // right thing to do -- it is what makes room. TESSELLA_REGION_FULL likewise emitted nothing,
    // but the frame that reported it ran the arena's compaction, so the retry has room this
    // attempt did not; draining is harmless and the cursor is what the caller retires against.
    // Any other failure leaves the ring untouched.
    if (last_ != TESSELLA_OK && last_ != TESSELLA_RING_FULL && last_ != TESSELLA_REGION_FULL) {
        return reader_ ? reader_->cursor() : 0;
    }

    tessella_map_regions regions{};
    if (tessella_regions(map_, &regions) != TESSELLA_OK || regions.ring == nullptr) {
        return reader_ ? reader_->cursor() : 0;
    }

    // Before the drain, which is the only point the ring holds a whole frame. Read directly
    // rather than atomically: the producer is this thread and the consumer is this thread.
    if (regions.ring_len >= sizeof(tsl_ring_control)) {
        tsl_ring_control control{};
        std::memcpy(&control, regions.ring, sizeof control);
        ringCapacity_ = control.capacity;
        const std::uint64_t held = control.head - control.tail;
        if (held > ringPeak_) {
            ringPeak_ = held;
        }
    }

    const Region ring{regions.ring, regions.ring_len};
    const Region slabs{regions.slabs, regions.slabs_len};
    if (!reader_) {
        reader_ = std::make_unique<Reader>(ring, slabs);
    } else {
        // Every tick, not once: the producer repacks its slab table after each frame that
        // allocates, so last frame's pointer is not this frame's.
        reader_->rebind(ring, slabs);
    }

    HostSink sink(renderer, drawlist_, orderCounts_);
    const std::uint64_t draining = now_ns();
    records_ += reader_->drain(sink);
    drainNs_ = now_ns() - draining;
    return reader_->cursor();
}

std::uint64_t Host::slabUsed() const {
    tessella_map_regions regions{};
    if (tessella_regions(map_, &regions) != TESSELLA_OK || regions.slabs == nullptr ||
        regions.slabs_len < sizeof(tsl_slab_region)) {
        return 0;
    }
    tsl_slab_region header{};
    std::memcpy(&header, regions.slabs, sizeof header);
    return header.total_len;
}

std::pair<std::uint64_t, std::uint64_t> Host::slabOccupancy() const {
    tessella_map_regions regions{};
    if (tessella_regions(map_, &regions) != TESSELLA_OK || regions.slabs == nullptr ||
        regions.slabs_len < sizeof(tsl_slab_region)) {
        return {0, 0};
    }
    tsl_slab_region header{};
    std::memcpy(&header, regions.slabs, sizeof header);
    const std::size_t table = sizeof(tsl_slab_region) + sizeof(tsl_slab_entry) * header.count;
    if (regions.slabs_len < table) {
        return {header.total_len, 0};
    }
    std::uint64_t live = 0;
    std::uint64_t slabs = 0;
    for (std::uint32_t i = 0; i < header.count; i++) {
        tsl_slab_entry entry{};
        std::memcpy(&entry, regions.slabs + sizeof(tsl_slab_region) + sizeof(tsl_slab_entry) * i,
                    sizeof entry);
        if (entry.length != 0) {
            live += entry.length;
            slabs++;
        }
    }
    return {live, slabs};
}

void Host::retire(std::uint64_t upTo) {
    tessella_map_regions regions{};
    if (tessella_regions(map_, &regions) != TESSELLA_OK || regions.ring == nullptr) {
        return;
    }
    if (regions.ring_len < sizeof(tsl_ring_control)) {
        return;
    }
    // Release ordering: everything this consumer read from the ring must be visible to have
    // happened before the producer sees the bytes as free.
    auto* tail = reinterpret_cast<std::uint64_t*>(
        const_cast<std::uint8_t*>(regions.ring) + offsetof(tsl_ring_control, tail));
    __atomic_store_n(tail, upTo, __ATOMIC_RELEASE);
}

std::uint64_t Host::pending() const {
    std::uint64_t value = 0;
    if (map_ == nullptr || tessella_pending(map_, &value) != TESSELLA_OK) {
        return 0;
    }
    return value;
}

tessella_readiness Host::readiness(std::string* reason) const {
    std::int32_t value = TESSELLA_IDLE;
    if (reason == nullptr) {
        tessella_status(map_, &value, nullptr, 0);
        return static_cast<tessella_readiness>(value);
    }
    std::vector<char> buffer(512, '\0');
    tessella_status(map_, &value, buffer.data(), buffer.size());
    *reason = buffer.data();
    return static_cast<tessella_readiness>(value);
}

} // namespace tsf
