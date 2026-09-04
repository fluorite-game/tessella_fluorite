// SPDX-License-Identifier: Apache-2.0

#include <tsf/reader.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tsf {
namespace {

/// Rounds up to an alignment, as the ABI's `TSL_PAYLOAD_ALIGN` and `TSL_RECORD_ALIGN` require.
constexpr std::uint64_t alignUp(std::uint64_t value, std::uint64_t to) noexcept {
    return (value + to - 1) & ~(to - 1);
}

/// Copies a trivially-copyable record out of the stream.
///
/// By value rather than by cast: a record's bytes are aligned to the *record* boundary and its
/// fields to their own, but a reinterpret_cast onto a stream is undefined behaviour whatever the
/// alignment, and the copy is a few dozen bytes against a frame's worth of geometry.
template <typename T>
bool read(const std::uint8_t* from, std::uint32_t available, T& into) noexcept {
    if (available < sizeof(T)) {
        return false;
    }
    std::memcpy(&into, from, sizeof(T));
    return true;
}

} // namespace

Bytes Reader::resolve(tsl_slab_ref ref) const noexcept {
    tsl_slab_region header{};
    if (slabs_.size < sizeof header) {
        return {};
    }
    std::memcpy(&header, slabs_.data, sizeof header);
    // A handle the table does not cover has no meaning. Refusing it turns a producer fault into
    // a diagnosis rather than a wild read.
    if (header.abi_rev != TSL_ABI_REV || ref.slab >= header.count) {
        return {};
    }

    const std::size_t at = sizeof(tsl_slab_region) + std::size_t{ref.slab} * sizeof(tsl_slab_entry);
    if (at + sizeof(tsl_slab_entry) > slabs_.size) {
        return {};
    }
    tsl_slab_entry entry{};
    std::memcpy(&entry, slabs_.data + at, sizeof entry);

    if (entry.offset > slabs_.size || entry.length > slabs_.size - entry.offset) {
        return {};
    }
    if (std::uint64_t{ref.offset} + ref.length > entry.length) {
        return {};
    }
    return Bytes{slabs_.data + entry.offset + ref.offset, ref.length};
}

template <typename T>
std::vector<T> Reader::span(const std::uint8_t* payload,
                            std::uint32_t payloadLen,
                            tsl_span at) const {
    std::vector<T> out;
    // The offset is in bytes and the count is in *elements*, which is the one place this ABI's
    // spans differ from each other. Validating `offset + count` would accept a list running most
    // of the way past the end of the payload.
    const std::uint64_t bytes = std::uint64_t{at.count} * sizeof(T);
    if (std::uint64_t{at.offset} + bytes > payloadLen) {
        return out;
    }
    out.resize(at.count);
    if (at.count != 0) {
        std::memcpy(out.data(), payload + at.offset, static_cast<std::size_t>(bytes));
    }
    return out;
}

std::size_t Reader::drain(FrameSink& sink) {
    tsl_ring_control control{};
    if (ring_.size < sizeof control) {
        return 0;
    }
    std::memcpy(&control, ring_.data, sizeof control);
    if (control.abi_rev != TSL_ABI_REV || control.capacity == 0) {
        return 0;
    }

    const std::uint8_t* data = ring_.data + sizeof(tsl_ring_control);
    const std::uint64_t capacity = control.capacity;
    // Acquiring, against the producer's release of the same word: everything it wrote before
    // publishing head is visible to us once we have read head.
    const std::uint64_t head =
        __atomic_load_n(reinterpret_cast<const std::uint64_t*>(ring_.data + offsetof(tsl_ring_control, head)),
                        __ATOMIC_ACQUIRE);

    std::size_t consumed = 0;
    while (cursor_ < head) {
        const std::uint64_t offset = cursor_ & (capacity - 1);
        tsl_record_header header{};
        if (capacity - offset < sizeof header) {
            break;
        }
        std::memcpy(&header, data + offset, sizeof header);
        if (header.total_len == 0 || header.total_len > capacity - offset) {
            break;
        }

        // Every record's bytes to a file, for diffing two runs. Off unless TSF_DUMP names a path.
        if (const char* path = ::getenv("TSF_DUMP")) {
            static std::FILE* dump = std::fopen(path, "wb");
            if (dump != nullptr) {
                std::fwrite(data + offset, 1, (std::size_t)header.total_len, dump);
                std::fflush(dump);
            }
        }
        // A wrap record is padding to the end of the buffer, not content.
        if ((header.flags & TSL_RECORD_FLAG_SKIP) == 0) {
            const std::uint64_t body = alignUp(header.record_len, TSL_PAYLOAD_ALIGN);
            if (sizeof header + body + header.payload_len <= header.total_len) {
                const std::uint8_t* fixed = data + offset + sizeof(tsl_record_header);
                dispatch(header, fixed, fixed + body, header.payload_len, sink);
                consumed++;
            }
        }
        cursor_ += header.total_len;
    }
    return consumed;
}

void Reader::dispatch(const tsl_record_header& header,
                      const std::uint8_t* fixed,
                      const std::uint8_t* payload,
                      std::uint32_t payloadLen,
                      FrameSink& sink) {
    // A frame opens at its first record and closes at its camera, which is the commit point:
    // the camera carries the frame number and names the order epoch it requires, and nothing is
    // emitted after it. So there is no end-of-frame marker to look for and none is needed.
    const auto openFrame = [&] {
        if (!inFrame_) {
            inFrame_ = true;
            sink.beginFrame(0);
        }
    };

    switch (header.kind) {
    case TSL_ENVELOPE_KIND_GEOMETRY_ADD: {
        tsl_geometry_add add{};
        if (!read(fixed, header.record_len, add)) {
            return;
        }
        openFrame();
        // Held rather than delivered: the mirror wants the joined record, and the view that
        // uses this geometry has not been named yet. The spans are read out *now*, because they
        // point into the ring and the producer reuses that as soon as the tail passes.
        Geometry held;
        held.record = add;
        // Just *past* this record, which is what the producer compares against.
        held.announcedAt = cursor_ + header.total_len;
        held.attrs = span<tsl_attribute_desc>(payload, payloadLen, add.attrs);
        held.instanceAttrs = span<tsl_attribute_desc>(payload, payloadLen, add.instance_attrs);
        held.segments = span<tsl_segment>(payload, payloadLen, add.segments);
        held.textureRefs = span<tsl_texture_ref>(payload, payloadLen, add.texture_refs);
        geometry_[add.geometry] = std::move(held);
        break;
    }
    case TSL_ENVELOPE_KIND_VIEW_USE: {
        tsl_view_use use{};
        if (!read(fixed, header.record_len, use)) {
            return;
        }
        const auto found = geometry_.find(use.geometry);
        if (found == geometry_.end()) {
            // A use naming an id nothing declared is a protocol fault, and the ABI says so. It
            // is dropped rather than guessed at: drawing whatever geometry happened to be at
            // that id would be worse than drawing nothing.
            return;
        }
        openFrame();
        // Joined here. Everything shared comes from the add, everything per-view from the use.
        DrawableAdd out;
        out.view = use.view;
        out.id = use.geometry;
        out.reason = found->second.record.reason;
        out.builtinShader = found->second.record.builtin_shader;
        out.permutationKey = found->second.record.permutation_key;
        out.vertexCount = found->second.record.vertex_count;
        out.vertexType = found->second.record.vertex_type;
        out.layerIndex = use.layer_index;
        out.subLayerIndex = use.sub_layer_index;
        out.renderPass = use.render_pass;
        if (use.has_tile != 0) {
            out.tileID = TileID{use.tile.z, use.tile.x, use.tile.y, use.tile.wrap,
                                use.tile.overscaled_z};
        }
        out.is3D = (use.draw_flags & TSL_DRAW_FLAG_IS_3D) != 0;
        out.enableStencil = (use.draw_flags & TSL_DRAW_FLAG_ENABLE_STENCIL) != 0;
        out.enableDepth = (use.draw_flags & TSL_DRAW_FLAG_ENABLE_DEPTH) != 0;
        out.enableColor = (use.draw_flags & TSL_DRAW_FLAG_ENABLE_COLOR) != 0;
        out.indexes = resolve(found->second.record.indexes);
        out.announcedAt = found->second.announcedAt;

        // The attribute descriptors live in the *add's* payload, which the ring has since
        // reused — so they were copied when the add arrived, and each one's buffer is resolved
        // here. A mirror cannot resolve a slab reference itself: it has the record and not the
        // region.
        const auto withData = [&](const std::vector<tsl_attribute_desc>& from) {
            std::vector<Attribute> out;
            out.reserve(from.size());
            for (const auto& desc : from) {
                out.push_back(Attribute{desc, resolve(desc.source)});
            }
            return out;
        };
        out.attrs = withData(found->second.attrs);
        out.instanceAttrs = withData(found->second.instanceAttrs);
        out.segments = found->second.segments;
        out.textureRefs.reserve(found->second.textureRefs.size());
        for (const auto& ref : found->second.textureRefs) {
            out.textureRefs.push_back(TextureBinding{ref.slot, ref.texture, ref.filter});
        }

        sink.onDrawableAdd(out);
        break;
    }
    case TSL_ENVELOPE_KIND_VIEW_RELEASE: {
        tsl_view_release release{};
        if (!read(fixed, header.record_len, release)) {
            return;
        }
        openFrame();
        DrawableRemove out;
        out.id = release.geometry;
        out.view = release.view;
        sink.onDrawableRemove(out);
        break;
    }
    case TSL_ENVELOPE_KIND_GEOMETRY_REMOVE: {
        tsl_geometry_remove gone{};
        if (!read(fixed, header.record_len, gone)) {
            return;
        }
        openFrame();
        geometry_.erase(gone.geometry);
        DrawableRemove out;
        out.id = gone.geometry;
        sink.onDrawableRemove(out);
        break;
    }
    case TSL_ENVELOPE_KIND_UBO_UPDATE: {
        tsl_ubo_update update{};
        if (!read(fixed, header.record_len, update)) {
            return;
        }
        // A ubo span's count is a *byte* count, which the struct says and which reading it as
        // elements would multiply by a stride that does not exist.
        if (std::uint64_t{update.data.offset} + update.data.count > payloadLen) {
            return;
        }
        openFrame();
        UboUpdate out;
        out.view = update.view;
        out.slot = update.slot;
        if (update.layer_index >= 0) {
            out.layerIndex = update.layer_index;
        }
        out.bytes = Bytes{payload + update.data.offset, update.data.count};
        sink.onUboUpdate(out);
        break;
    }
    case TSL_ENVELOPE_KIND_TEXTURE_UPDATE: {
        tsl_texture_update update{};
        if (!read(fixed, header.record_len, update)) {
            return;
        }
        if (update.rect_count > TSL_TEXTURE_RECT_CAP) {
            return;
        }
        if (std::uint64_t{update.pixels.offset} + update.pixels.count > payloadLen) {
            return;
        }
        openFrame();
        TextureUpdate out;
        out.id = update.texture;
        out.width = update.size.width;
        out.height = update.size.height;
        out.format = update.format;
        // Zero rectangles means the whole texture, and a consumer reading that as "no damage"
        // uploads nothing and samples a blank atlas -- a map with no labels and no error.
        out.rects.assign(update.rects, update.rects + update.rect_count);
        out.pixels = Bytes{payload + update.pixels.offset, update.pixels.count};
        sink.onTextureUpdate(out);
        break;
    }
    case TSL_ENVELOPE_KIND_STENCIL_TILES: {
        tsl_stencil_tiles stencil{};
        if (!read(fixed, header.record_len, stencil)) {
            return;
        }
        openFrame();
        StencilTiles out;
        out.view = stencil.view;
        out.layerIndex = stencil.layer_index;
        out.tiles = span<tsl_stencil_tile>(payload, payloadLen, stencil.tiles);
        sink.onStencilTiles(out);
        break;
    }
    case TSL_ENVELOPE_KIND_ORDER_UPDATE: {
        tsl_order_update update{};
        if (!read(fixed, header.record_len, update)) {
            return;
        }
        openFrame();
        // Held for the camera that names its epoch. §11.7's obligation is that a consumer holds
        // a camera until its order epoch is held, which is only expressible if the two arrive as
        // one thing at the sink.
        pending_ = FrameOrder{};
        pending_.view = update.view;
        pending_.orderEpoch = update.order_epoch;
        pending_.entries = span<tsl_order_entry>(payload, payloadLen, update.entries);
        havePending_ = true;
        break;
    }
    case TSL_ENVELOPE_KIND_CAMERA_UPDATE: {
        tsl_camera_update camera{};
        if (!read(fixed, header.record_len, camera)) {
            return;
        }
        openFrame();
        if (havePending_ && pending_.orderEpoch == camera.order_epoch) {
            pending_.camera = camera;
            sink.onFrameOrder(pending_);
            // Held, not consumed. An order is durable -- the producer sends one when the draw
            // list changes and not otherwise -- so a later frame that moves only the camera
            // carries a camera and no order, and names the epoch of the order already sent.
            // Clearing the flag here made the next such frame fall through to the branch below
            // and dispatch an order with no entries; the sink rebuilds its scene from the order
            // it is given, so that is a frame that draws nothing. On the quad's zoom sweep it
            // was 777 of 930 frames, and on screen a flicker between the map and black.
        } else {
            // A camera naming an epoch this reader has never seen an order for. The obligation
            // is to hold it rather than apply it against the wrong order -- which would draw
            // this frame's camera over the last frame's painter order, one frame of the wrong
            // thing on every restyle. What goes to the sink carries the camera and no entries,
            // and the sink must leave its scene alone rather than rebuild it from nothing.
            FrameOrder held;
            held.view = camera.view;
            held.orderEpoch = camera.order_epoch;
            held.camera = camera;
            sink.onFrameOrder(held);
        }
        // The camera commits the frame.
        inFrame_ = false;
        sink.endFrame(camera.frame_no);
        break;
    }
    case TSL_ENVELOPE_KIND_VIEW_DECLARE: {
        tsl_view_declare declare{};
        if (!read(fixed, header.record_len, declare)) {
            return;
        }
        sink.onViewDeclare(declare.view, declare.camera_mode);
        break;
    }
    case TSL_ENVELOPE_KIND_VIEW_UNDECLARE: {
        tsl_view_undeclare undeclare{};
        if (!read(fixed, header.record_len, undeclare)) {
            return;
        }
        sink.onViewUndeclare(undeclare.view);
        break;
    }
    case TSL_ENVELOPE_KIND_MESH_ADD: {
        tsl_mesh_add mesh{};
        if (!read(fixed, header.record_len, mesh)) {
            return;
        }
        openFrame();
        sink.onMeshAdd(mesh.mesh, mesh.format, resolve(mesh.bytes));
        break;
    }
    default:
        unknown_++;
        break;
    }
}

} // namespace tsf
