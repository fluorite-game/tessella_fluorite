// SPDX-License-Identifier: Apache-2.0
//
// The capture stream as the mirror consumes it, typed against tessella's flat C ABI rather
// than mbgl's C++ one.
//
// # Why these types exist at all
//
// The mirror's sink took `mln::capture::DrawableAdd` and friends, which are mbgl types: they
// carry `std::shared_ptr<gfx::IndexVectorBase>`, `OverscaledTileID`, `util::SimpleIdentity`.
// Taking them means linking mbgl, which is the whole thing the port removes -- and which is
// also why this build currently cannot link its own test probes, since mbgl-core is built
// against libstdc++ and Filament requires libc++.
//
// So these mirror mbgl's shapes field for field, deliberately, so that the Filament half reads
// the same as it did. What changes is where a field comes from, not what it means.
//
// # The two joins the stream requires
//
// tessella splits mbgl's `DrawableAdd` in two, because the halves have different lifetimes
// (plan §5.1): `tsl_geometry_add` is the *shared* description -- shader, attributes, indices,
// segments -- and `tsl_view_use` is one view's claim on it, carrying the layer index, the tile
// and the draw flags. Four views over one tile send one add and four uses. The reader joins
// them back into the single record the mirror expects.
//
// And a buffer is a slab reference rather than a pointer: an offset into a shared region that
// resolves to borrowed bytes. The consumer holds the reference until its driver's copy
// completes (§11.7), which is the same contract mbgl's `shared_ptr` expressed.

#ifndef TSF_FRAME_H
#define TSF_FRAME_H

#include <tessella_capture_abi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tsf {

/// Borrowed bytes in the producer's shared region.
///
/// Valid until the consumer releases the slab it came from, which it must not do before the
/// driver has finished copying (§11.7). Nothing here owns them.
struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool empty() const noexcept { return data == nullptr || size == 0; }
};

/// A tile address, when a drawable has one.
///
/// Background and the world-scale layers do not: `tsl_view_use` carries `has_tile` beside the
/// address for exactly that reason, and this is `std::optional` for the same reason mbgl's was.
struct TileID {
    std::uint8_t z = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::int32_t wrap = 0;
    std::uint8_t overscaled_z = 0;

    /// Two addresses are the same tile when every field agrees, `wrap` included.
    ///
    /// A wrap is a different place on a repeating plane -- the same patch drawn either side of
    /// the antimeridian -- so a mask keyed without it would clip one copy with the other's
    /// stencil. (A globe view asks for one copy and never has two, but the mirror does not get
    /// to assume which kind of view it is drawing.)
    /// The same address at the canonical world copy.
    ///
    /// mbgl's `toUnwrapped`. Placement is per copy and the *scale* is not: a tile is the same
    /// size whichever copy it is drawn in, so the maths that only wants the zoom asks for this
    /// rather than carrying a wrap it would have to remember to ignore.
    [[nodiscard]] TileID unwrapped() const noexcept {
        return TileID{z, x, y, 0, overscaled_z};
    }

    /// Built from the address a `tsl_view_use` or a `tsl_stencil_tile` carries.
    static TileID from(const tsl_tile_id& id) noexcept {
        return TileID{id.z, id.x, id.y, id.wrap, id.overscaled_z};
    }

    friend bool operator==(const TileID& a, const TileID& b) noexcept {
        return a.z == b.z && a.x == b.x && a.y == b.y && a.wrap == b.wrap &&
               a.overscaled_z == b.overscaled_z;
    }
};

/// One vertex attribute: what the shader wants, and where the bytes are.
///
/// mbgl's `AttributeDesc` carried a `shared_ptr` to the vertex vector; tessella's carries a slab
/// reference, and resolving it needs the region — which the reader has and the mirror does not.
/// So it is resolved on the way through and the pair travels together.
struct Attribute {
    tsl_attribute_desc desc{};
    /// The buffer this attribute reads, borrowed.
    Bytes data;

    /// How many vertices the buffer holds at this attribute's stride.
    [[nodiscard]] std::size_t count() const noexcept {
        return desc.stride == 0 ? 0 : data.size / desc.stride;
    }
};

/// One drawable, joined from the shared geometry and one view's use of it.
///
/// Field names follow mbgl's `DrawableAdd` so the Filament half needs no re-reading.
struct DrawableAdd {
    std::uint32_t view = 0;
    std::uint64_t id = 0;
    std::uint8_t reason = 0;

    std::int32_t builtinShader = 0;
    std::uint64_t permutationKey = 0;

    std::optional<TileID> tileID;
    std::int32_t layerIndex = -1;
    std::int32_t subLayerIndex = 0;

    std::vector<Attribute> attrs;
    std::vector<Attribute> instanceAttrs;
    std::size_t vertexCount = 0;
    std::uint8_t vertexType = 0;

    /// The index buffer, borrowed. `uint16` elements, as every shader here declares.
    Bytes indexes;
    std::vector<tsl_segment> segments;

    /// Slot to texture id, for the slots this drawable's shader declares.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> textureRefs;

    bool is3D = false;
    bool enableStencil = false;
    bool enableDepth = false;
    bool enableColor = true;
    std::uint8_t renderPass = 0;

    /// Ring position just past the record that announced this geometry.
    ///
    /// §13.2's acknowledgement, from the consumer's end. The producer will not reuse the slab
    /// these bytes live in until the consumer has said it has uploaded past this point, which is
    /// what makes handing the borrowed pointer straight to the driver safe — and what makes
    /// copying it wasted work rather than prudence.
    std::uint64_t announcedAt = 0;

    /// How many indices the buffer holds, which is what a draw call wants.
    [[nodiscard]] std::size_t indexCount() const noexcept {
        return indexes.size / sizeof(std::uint16_t);
    }
};

/// A drawable retired, or one view's claim on it dropped.
///
/// The two are distinct in tessella and were one in mbgl: `tsl_view_release` drops a view's
/// hold while others may keep theirs, and `tsl_geometry_remove` retires the geometry once no
/// view holds it. `view` is set for the first and absent for the second.
struct DrawableRemove {
    std::uint64_t id = 0;
    std::optional<std::uint32_t> view;
};

/// New bytes for one uniform block.
struct UboUpdate {
    std::uint32_t view = 0;
    /// Absent for a frame-wide block, which is how the globals arrive.
    std::optional<std::int32_t> layerIndex;
    std::uint32_t slot = 0;
    Bytes bytes;
};

/// New pixels for a texture.
///
/// `rects` empty means the whole texture, which is what `rect_count` of zero says on the wire.
/// A consumer reading that as "no damage" uploads nothing and samples a blank atlas.
struct TextureUpdate {
    std::uint64_t id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t format = 0;
    std::vector<tsl_rect> rects;
    Bytes pixels;

    /// Bytes one pixel of this texture occupies, from the header's own table.
    [[nodiscard]] std::uint32_t pixelSize() const noexcept {
        return tsl_texture_pixel_size(static_cast<int>(format));
    }
};

/// The tiles a layer's stencil mask is built from.
struct StencilTiles {
    std::uint32_t view = 0;
    std::int32_t layerIndex = -1;
    std::vector<tsl_stencil_tile> tiles;
};

/// The painter order, and the camera that commits the frame.
///
/// One record in mbgl and two here, and they arrive that way: the order establishes an epoch and
/// the camera names the epoch it requires. §11.7's obligation is to hold a camera until its
/// epoch is held, which a consumer cannot honour if the two are one record.
struct FrameOrder {
    std::uint32_t view = 0;
    std::uint64_t orderEpoch = 0;
    std::vector<tsl_order_entry> entries;

    /// Set once the camera naming this epoch has arrived.
    std::optional<tsl_camera_update> camera;
};

/// What the reader drives. The mirror implements it.
///
/// `beginFrame` and `endFrame` bracket the records of one frame. The stream has no explicit
/// end-of-frame marker and needs none: a frame is emitted as state, geometry, uniforms, order,
/// then camera, and the camera is the commit point -- it carries the frame number and names the
/// order epoch it requires. So the camera *is* the boundary.
class FrameSink {
public:
    virtual ~FrameSink() = default;

    virtual void beginFrame(std::uint64_t /*frameNo*/) {}
    virtual void endFrame(std::uint64_t /*frameNo*/) {}

    virtual void onDrawableAdd(const DrawableAdd&) {}
    virtual void onDrawableRemove(const DrawableRemove&) {}
    virtual void onUboUpdate(const UboUpdate&) {}
    virtual void onTextureUpdate(const TextureUpdate&) {}
    virtual void onStencilTiles(const StencilTiles&) {}
    virtual void onFrameOrder(const FrameOrder&) {}

    /// A view appearing, with its configuration, and a view going away.
    virtual void onViewDeclare(std::uint32_t /*view*/, std::uint8_t /*cameraMode*/) {}
    virtual void onViewUndeclare(std::uint32_t /*view*/) {}

    /// An authored mesh: bytes and a format the consumer's own loader reads.
    ///
    /// A consumer meeting a format it does not know must skip the mesh rather than guess at the
    /// bytes; the id is in the same space as geometry's, so a later use, release or retirement
    /// of it arrives through the ordinary records.
    virtual void onMeshAdd(std::uint64_t /*id*/, std::uint8_t /*format*/, Bytes) {}
};

} // namespace tsf

namespace std {

/// So a tile address can key a map, which is how the stencil masks are held.
template <>
struct hash<tsf::TileID> {
    std::size_t operator()(const tsf::TileID& id) const noexcept {
        // The zoom bounds x and y, so the three pack without collision below z16 and mix
        // cheaply above it. Wrap is folded in last because it is almost always zero.
        std::size_t h = (static_cast<std::size_t>(id.z) << 58) ^
                        (static_cast<std::size_t>(id.x) << 29) ^ static_cast<std::size_t>(id.y);
        h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(id.wrap)) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<std::size_t>(id.overscaled_z) << 51;
        return h;
    }
};

} // namespace std

#endif // TSF_FRAME_H
