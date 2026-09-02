// SPDX-License-Identifier: Apache-2.0
//
// A `tsf::Renderer` that puts tessella's batches on a Filament scene.

#ifndef TSF_FILAMENT_RENDERER_H
#define TSF_FILAMENT_RENDERER_H

#include <tsf/host.h>

#include <filament/Engine.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Scene.h>
#include <utils/Entity.h>

#include <cstdint>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tsf {

/// Turns batched drawables into Filament renderables.
///
/// # What it maps onto what
///
/// A `Batch` is a contiguous run of drawables sharing a shader, a permutation and a texture set,
/// so it becomes *one* renderable with one primitive per geometry. That is the shape Filament
/// wants and the reason `DrawList` groups at all: N primitives on one renderable is one draw
/// submission rather than N.
///
/// A geometry becomes a `VertexBuffer` plus an `IndexBuffer`, built from the bytes the producer
/// published and kept until the geometry retires. §11.7 asks a consumer to hold the slab until
/// the driver's copy completes; here the copy is synchronous inside `setBufferAt`, so the bytes
/// are safe the moment it returns and the host may retire the ring position afterwards.
///
/// A layer's uniforms arrive as blocks: `tsl_fill_drawable_ubo` per drawable, indexed by the
/// `ubo_index` the order carries, and `tsl_fill_evaluated_props_ubo` once per layer. They are
/// held by (layer, slot) and read when a batch names that layer, which is why uniforms must be
/// delivered before the order that uses them -- and they are, because that is the sequence a
/// frame is emitted in.
///
/// # What it does not do yet
///
/// Materials it has no package for are counted and skipped rather than guessed at. `missing()`
/// says how many, so a frame that draws less than it should says so instead of quietly looking
/// thinner than the oracle's.
class FilamentRenderer final : public Renderer {
public:
    /// Loads every `.filamat` in `materialDir`, named for the shader family it serves.
    /// `width` and `height` are the view's, needed to turn a tile's clip-space box into the
    /// scissor rectangle that keeps its geometry inside its own tile.
    FilamentRenderer(filament::Engine* engine,
                     filament::Scene* scene,
                     const std::string& materialDir,
                     std::uint32_t width,
                     std::uint32_t height);
    ~FilamentRenderer() override;

    FilamentRenderer(const FilamentRenderer&) = delete;
    FilamentRenderer& operator=(const FilamentRenderer&) = delete;

    /// Points a camera at what tessella emits.
    ///
    /// The capture stream's matrices carry tile-local coordinates all the way to clip space, so
    /// the camera must not project again: its view and projection are the identity apart from one
    /// correction. Filament's clip space has +Y downward -- measured, and the same on the Vulkan
    /// and OpenGL backends, so it is Filament's own convention rather than a backend's NDC -- and
    /// mbgl's matrices are written for OpenGL's +Y up. The projection is that flip, which is the
    /// whole of the difference between the two conventions.
    ///
    /// Without it every frame renders vertically mirrored: the map is upside down on screen, and
    /// a readback compared against the oracle looks merely translated, which is how it hid.
    static void configureCamera(filament::Camera& camera);

    void beginFrame(std::uint64_t frameNo) override;
    void endFrame(std::uint64_t frameNo) override;
    void onGeometry(const DrawableAdd& add) override;
    void onRetire(std::uint64_t id) override;
    void onBatch(const Batch& batch) override;

    /// Builds one batch into the scene. Called from `endFrame`, in painter order.
    void issue(const Batch& batch);
    void onUniforms(const UboUpdate& update) override;
    void onStencilTiles(const StencilTiles& tiles) override;

    /// How many batches were skipped for want of a material.
    [[nodiscard]] std::uint64_t missing() const noexcept { return missing_; }
    /// Which families were asked for and not found, for a caller that wants to author them.
    [[nodiscard]] const std::vector<std::int32_t>& missingFamilies() const noexcept {
        return missingFamilies_;
    }
    /// How many renderables this frame put in the scene.
    [[nodiscard]] std::uint64_t renderables() const noexcept { return renderables_; }
    /// How many primitives those renderables carry.
    [[nodiscard]] std::uint64_t primitives() const noexcept { return primitives_; }
    /// Instances made this frame, and how many of them got a paint colour.
    [[nodiscard]] std::uint64_t made() const noexcept { return made_; }
    [[nodiscard]] std::uint64_t coloured() const noexcept { return coloured_; }

    /// How many geometries arrived at each tile zoom, so a frame drawing coarse ancestors
    /// beside the tiles that replaced them is visible rather than inferred.
    [[nodiscard]] const std::map<std::uint8_t, std::uint64_t>& zooms() const noexcept {
        return zooms_;
    }

    /// Drawables skipped because their matrix slot was past the end of the layer's buffer.
    [[nodiscard]] std::uint64_t unplaced() const noexcept { return unplaced_; }

    /// Geometries issued more than once in a frame. Each extra draw blends again, so a
    /// translucent fill drawn twice is visibly darker than the same fill drawn once.
    [[nodiscard]] std::uint64_t redrawn() const noexcept { return redrawn_; }

    /// How many drawables were issued in each render pass.
    [[nodiscard]] const std::map<std::uint8_t, std::uint64_t>& passes() const noexcept {
        return passes_;
    }

    /// The overscaled zoom of what was drawn, beside the source zoom. They differ when a
    /// coarse tile stands in for a finer one, and a consumer that reads only one of them cannot
    /// tell a z10 tile serving z13 from a z13 tile.
    [[nodiscard]] const std::map<std::uint8_t, std::uint64_t>& overZooms() const noexcept {
        return overZooms_;
    }

    /// Drawables that reused a (layer, matrix slot) another drawable already used this frame.
    [[nodiscard]] std::uint64_t sharedSlots() const noexcept { return sharedSlots_; }

    /// Distinct tile placements drawn: translation and scale of each drawable's matrix. One
    /// per tile of the cover if the cover is what is being drawn.
    [[nodiscard]] std::size_t placements() const noexcept { return placements_.size(); }

    /// Drawables whose tile matched no mask, so nothing clipped them.
    [[nodiscard]] std::uint64_t unmasked() const noexcept { return unmasked_; }

    /// How many drawables were placed at each matrix scale. One scale means one zoom; several
    /// means drawables are carrying other tiles' matrices.
    [[nodiscard]] const std::map<float, std::uint64_t>& scales() const noexcept { return scales_; }

    /// How many clip masks were written this frame.
    [[nodiscard]] std::uint64_t masked() const noexcept { return masked_; }

    /// How many drawables were clipped to their own tile.
    [[nodiscard]] std::uint64_t scissored() const noexcept { return scissored_; }

    /// How many wall triangles were built from instances.
    [[nodiscard]] std::uint64_t walls() const noexcept { return walls_; }

    /// How many materials were loaded.
    [[nodiscard]] std::size_t materials() const noexcept { return materials_.size(); }

private:
    /// A geometry's GPU buffers, kept until it retires.
    struct Mesh {
        filament::VertexBuffer* vertices = nullptr;
        filament::IndexBuffer* indices = nullptr;
        std::uint32_t indexCount = 0;
        std::int32_t layerIndex = -1;
        std::uint8_t zoom = 0;
        std::uint8_t overscaledZoom = 0;
        TileID tile{};
    };

    /// One layer's uniform blocks, by slot.
    using Blocks = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;

    void clearScene();

    /// Turns a wall drawable's instances into ordinary geometry. See the definition for why.
    bool expandWalls(const DrawableAdd& add);

    /// Builds an extrusion roof, keying attributes by id and filling in the ones a style left
    /// constant. See the definition for why that is not the generic path.
    bool buildRoof(const DrawableAdd& add);

    /// Draws the clip masks for this frame and assigns each tile its stencil reference.
    ///
    /// Coarse first, so a child's mask overwrites its parent's where they overlap and the parent
    /// is left owning only what the child does not cover.
    void writeMasks();

    /// The stencil reference a tile's geometry tests against, or zero if it has no mask.
    [[nodiscard]] std::uint8_t referenceFor(const TileID& tile) const;

    /// The mask set the producer named, newest wins, keyed by tile.
    std::map<TileID, filament::math::mat4f> masks_;
    std::map<TileID, std::uint8_t> references_;
    filament::Material* maskMaterial_ = nullptr;
    std::vector<filament::MaterialInstance*> maskInstances_;
    filament::VertexBuffer* maskVertices_ = nullptr;
    filament::IndexBuffer* maskIndices_ = nullptr;
    std::uint64_t masked_ = 0;
    std::uint64_t unmasked_ = 0;

    filament::Engine* engine_ = nullptr;
    filament::Scene* scene_ = nullptr;

    std::unordered_map<std::int32_t, filament::Material*> materials_;
    std::unordered_map<std::uint64_t, Mesh> meshes_;
    std::unordered_map<std::int32_t, Blocks> uniforms_;

    /// This frame's batches, held until `endFrame`.
    ///
    /// The producer sends front-to-back -- topmost layer first, background last -- because that is
    /// what a depth-buffered renderer wants. Submitting in that sequence to a translucent pass
    /// paints the map inside out, so the batches are collected and issued in reverse: bottom layer
    /// first, which is the painter order blending needs.
    std::vector<Batch> pending_;

    /// Entities for the frame being built, torn down at the next `beginFrame`.
    std::vector<utils::Entity> entities_;

    /// One instance per (layer, shader, tile slot), kept across frames.
    ///
    /// Keyed by the tile as well as the layer because the scissor is a property of the instance
    /// and the clip is a property of the tile. Still bounded by the cover -- a handful of tiles
    /// times the layers that draw -- rather than one per primitive per frame.
    std::map<std::tuple<std::uint32_t, std::int32_t, std::uint32_t>, filament::MaterialInstance*>
        instances_;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    std::uint64_t missing_ = 0;
    std::vector<std::int32_t> missingFamilies_;
    std::map<std::uint8_t, std::uint64_t> zooms_;
    std::map<std::uint8_t, std::uint64_t> overZooms_;
    std::uint64_t ordered_ = 0;
    std::uint64_t unplaced_ = 0;
    std::uint64_t scissored_ = 0;
    std::set<std::pair<std::uint32_t, std::uint32_t>> slotsThisFrame_;
    std::uint64_t sharedSlots_ = 0;
    std::set<std::tuple<float, float, float>> placements_;
    std::map<float, std::uint64_t> scales_;
    std::unordered_set<std::uint64_t> drawnThisFrame_;
    std::uint64_t redrawn_ = 0;
    std::map<std::uint8_t, std::uint64_t> passes_;
    std::uint64_t made_ = 0;
    std::uint64_t coloured_ = 0;
    std::uint64_t renderables_ = 0;
    std::uint64_t primitives_ = 0;
    std::uint64_t walls_ = 0;
};

} // namespace tsf

#endif // TSF_FILAMENT_RENDERER_H
