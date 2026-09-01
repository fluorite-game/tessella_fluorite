// SPDX-License-Identifier: Apache-2.0
//
// A `tsf::Renderer` that puts tessella's batches on a Filament scene.

#ifndef TSF_FILAMENT_RENDERER_H
#define TSF_FILAMENT_RENDERER_H

#include <tsf/host.h>

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Scene.h>
#include <utils/Entity.h>

#include <cstdint>
#include <string>
#include <unordered_map>
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
    FilamentRenderer(filament::Engine* engine,
                     filament::Scene* scene,
                     const std::string& materialDir);
    ~FilamentRenderer() override;

    FilamentRenderer(const FilamentRenderer&) = delete;
    FilamentRenderer& operator=(const FilamentRenderer&) = delete;

    void beginFrame(std::uint64_t frameNo) override;
    void endFrame(std::uint64_t frameNo) override;
    void onGeometry(const DrawableAdd& add) override;
    void onRetire(std::uint64_t id) override;
    void onBatch(const Batch& batch) override;

    /// Builds one batch into the scene. Called from `endFrame`, in painter order.
    void issue(const Batch& batch);
    void onUniforms(const UboUpdate& update) override;

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

    /// How many materials were loaded.
    [[nodiscard]] std::size_t materials() const noexcept { return materials_.size(); }

private:
    /// A geometry's GPU buffers, kept until it retires.
    struct Mesh {
        filament::VertexBuffer* vertices = nullptr;
        filament::IndexBuffer* indices = nullptr;
        std::uint32_t indexCount = 0;
        std::int32_t layerIndex = -1;
    };

    /// One layer's uniform blocks, by slot.
    using Blocks = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;

    void clearScene();

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

    /// Everything created for the frame being built, torn down at the next `beginFrame`.
    std::vector<utils::Entity> entities_;
    std::vector<filament::MaterialInstance*> instances_;

    std::uint64_t missing_ = 0;
    std::vector<std::int32_t> missingFamilies_;
    std::uint64_t ordered_ = 0;
    std::uint64_t made_ = 0;
    std::uint64_t coloured_ = 0;
    std::uint64_t renderables_ = 0;
    std::uint64_t primitives_ = 0;
};

} // namespace tsf

#endif // TSF_FILAMENT_RENDERER_H
