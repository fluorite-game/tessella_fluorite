// SPDX-License-Identifier: Apache-2.0

#include <tsf/host.h>

#include <tessella_capture_abi.h>

#include <cstddef>
#include <vector>

namespace tsf {
namespace {

/// Forwards what the reader delivers to a `Renderer`, batching the order on the way through.
class HostSink final : public FrameSink {
public:
    HostSink(Renderer& renderer, DrawList& drawlist) noexcept
        : renderer_(renderer), drawlist_(drawlist) {}

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
    void onTextureUpdate(const TextureUpdate& update) override { renderer_.onTexture(update); }

    void onFrameOrder(const FrameOrder& order) override {
        // The camera is the commit point: an order without one names an epoch the producer has
        // not published a camera for, so drawing it would put this frame's geometry under the
        // last frame's camera. Held rather than drawn, and the next order supersedes it.
        if (!order.camera) {
            return;
        }
        for (const Batch& batch : drawlist_.build(order)) {
            renderer_.onBatch(batch);
        }
    }

private:
    Renderer& renderer_;
    DrawList& drawlist_;
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

std::uint64_t Host::tick(Renderer& renderer) {
    last_ = tessella_tick(map_);
    // TESSELLA_RING_FULL means nothing was emitted and nothing retired, so draining is still the
    // right thing to do -- it is what makes room. Any other failure leaves the ring untouched.
    if (last_ != TESSELLA_OK && last_ != TESSELLA_RING_FULL) {
        return reader_ ? reader_->cursor() : 0;
    }

    tessella_map_regions regions{};
    if (tessella_regions(map_, &regions) != TESSELLA_OK || regions.ring == nullptr) {
        return reader_ ? reader_->cursor() : 0;
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

    HostSink sink(renderer, drawlist_);
    records_ += reader_->drain(sink);
    return reader_->cursor();
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
