// SPDX-License-Identifier: Apache-2.0
//
// A `tsf::Renderer` that puts tessella's batches on a Filament scene.

#ifndef TSF_FILAMENT_RENDERER_H
#define TSF_FILAMENT_RENDERER_H

#include <tsf/host.h>

#include <filament/Engine.h>
#include <filament/BufferObject.h>
#include <filament/Camera.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <utils/Entity.h>

#include <array>
#include <cstdint>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tsf {

/// One allocation behind a vertex buffer. Defined in the implementation.
struct Slab;
class Slabs;

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
    /// `layer` is the Filament layer every renderable this builds is put on.
    ///
    /// Fluorite gives all its platform views one shared scene and differs them
    /// by camera, so without a layer four maps would each draw in all four
    /// panes. One bit per view and `View::setVisibleLayers` on the other side
    /// keeps them apart. Defaults to 0x01, which is Filament's default
    /// renderable layer and what a view shows unless told otherwise -- so a
    /// single-view caller passes nothing and sees what it saw before.
    FilamentRenderer(filament::Engine* engine,
                     filament::Scene* scene,
                     const std::string& materialDir,
                     std::uint32_t width,
                     std::uint32_t height,
                     std::uint8_t layer = 0x01);
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
    static void configureCamera(filament::Camera& camera, bool flipY = true);

    /// Whether this renderer's target has its first row at the bottom.
    ///
    /// The same answer `configureCamera` takes, and it has to be the same one: the scissor boxes
    /// are computed in the producer's clip space and carried into the target's, so a camera
    /// flipped one way and a scissor the other clips every tile to where its own reflection
    /// overlaps it -- a band across the middle of the map.
    void setFlipY(bool flipY) noexcept { flipY_ = flipY; }

    /// Material packages loaded, and packages found but rejected by Filament.
    ///
    /// Zero loaded means nothing can draw, and the reason is almost always that
    /// the packages were compiled for the wrong shader model: Filament resolves
    /// Vulkan on this class of GPU as *mobile*, and a `matc -p desktop` package
    /// is refused with "not built for mobile" on stderr and a null material
    /// here. The frame then comes out black, which a caller comparing two of
    /// its own renders will happily report as agreement.
    [[nodiscard]] std::pair<std::size_t, std::size_t> materialsLoaded() const noexcept {
        return {materials_.size(), materialsRejected_};
    }

    /// The view's new size. Only the scissor rectangles and `unitsToPixels` read
    /// it -- both are computed per frame from these, so a resize is these two
    /// numbers and nothing to rebuild.
    void setViewportSize(std::uint32_t width, std::uint32_t height) noexcept {
        width_ = width;
        height_ = height;
    }

    void beginFrame(std::uint64_t frameNo) override;
    void endFrame(std::uint64_t frameNo) override;
    void onGeometry(const DrawableAdd& add) override;
    void onRetire(std::uint64_t id) override;
    void onCamera(const tsl_camera_update& camera) override;
    void onBatch(const Batch& batch) override;

    /// Builds one batch into the scene. Called from `endFrame`, in painter order.
    void issue(const Batch& batch);
    void onUniforms(const UboUpdate& update) override;
    void onTexture(const TextureUpdate& update) override;
    void onStencilTiles(const StencilTiles& tiles) override;
    void onViewTarget(const tsl_view_target& target) override;

    /// One offscreen pass this map needs: what to render into it, and where it lands.
    ///
    /// The caller owns the Filament `View` because the caller owns the one it draws the map
    /// with. This says what that view must be configured as, which is the whole of what the
    /// consumer can derive and the caller cannot.
    struct OffscreenPass {
        /// The producer's view id, which is derived from the map's view and the layer.
        std::uint32_t view = 0;
        /// The target to attach, already sized and formatted.
        filament::RenderTarget* target = nullptr;
        /// The layer bit its drawables carry, for `View::setVisibleLayers`.
        std::uint8_t layer = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    /// The offscreen passes this map has been told about, in declaration order.
    [[nodiscard]] const std::vector<OffscreenPass>& offscreenPasses() const noexcept {
        return offscreen_;
    }

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

    /// Symbol drawables whose `texsize` did not match the atlas bound for them.
    ///
    /// The shader divides by it, so any mismatch is every glyph drawn at the ratio between the
    /// two -- which reads as text made of fragments rather than as text at the wrong size.
    [[nodiscard]] std::uint64_t atlasMismatched() const noexcept { return atlasMismatched_; }

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

    /// Drawables that took the anchored bend rather than the direct one.
    [[nodiscard]] std::uint64_t anchoredDrawn() const noexcept { return anchoredDrawn_; }

    /// Buckets whose indices had to be widened to `u32` because the producer split them.
    ///
    /// Nonzero means some layer passed 65,535 vertices in one tile, which is the regime the
    /// segment offsets exist for. Zero at street zoom is expected.
    [[nodiscard]] std::uint64_t rebased() const noexcept { return rebased_; }

    /// Label quads placement kept, and label quads it hid.
    ///
    /// The producer shapes every label a tile holds and hides the ones that lost their space, so
    /// these are "drawn" against "offered minus drawn". A map with too few labels is one or the
    /// other and they want different fixes: nothing shaped is a layout question, everything
    /// shaped and hidden is a collision one.
    [[nodiscard]] std::uint64_t glyphsDrawn() const noexcept { return glyphsDrawn_; }
    [[nodiscard]] std::uint64_t glyphsHidden() const noexcept { return glyphsHidden_; }

    /// How many drawables were clipped to their own tile.
    [[nodiscard]] std::uint64_t scissored() const noexcept { return scissored_; }

    /// How many wall triangles were built from instances.
    [[nodiscard]] std::uint64_t walls() const noexcept { return walls_; }

    /// Textures held, and how many pixel uploads they have taken.
    [[nodiscard]] std::size_t textures() const noexcept { return textures_.size(); }
    [[nodiscard]] std::uint64_t textureUploads() const noexcept { return textureUploads_; }
    /// Uploads refused because the format has no Filament equivalent here.
    [[nodiscard]] std::uint64_t textureSkipped() const noexcept { return textureSkipped_; }

    /// Label batches skipped because they lay out in the map's plane, not the viewport's.
    [[nodiscard]] std::uint64_t pitchedLabels() const noexcept { return pitchedLabels_; }

    /// Symbol batches skipped because their atlas had not arrived.
    [[nodiscard]] std::uint64_t missingAtlas() const noexcept { return missingAtlas_; }

    /// How many materials were loaded.
    [[nodiscard]] std::size_t materials() const noexcept { return materials_.size(); }
    /// How many bent families have an anchored package beside the direct one.
    [[nodiscard]] std::size_t anchoredMaterials() const noexcept {
        return anchoredMaterials_.size();
    }

private:
    /// The material for a family, surface and paint permutation, built on first use.
    ///
    /// A mask of zero is the material the loader already built, returned as it is. Anything else
    /// specializes the family's package, which is why the loader keeps the bytes.
    filament::Material* materialFor(std::int32_t family, std::uint32_t surface,
                                    std::uint32_t mask);

    /// Fills a vertex buffer's slabs, and records the ones it made.
    ///
    /// A slab with no bytes is the shared zero buffer, which is not among the `owned` and is not
    /// destroyed with the mesh. Answers false when an allocation failed, which leaves the caller
    /// to tear down what it had built.
    bool uploadSlabs(const Slabs& slabs, std::uint32_t vertices,
                     filament::VertexBuffer& into, std::vector<filament::BufferObject*>& owned);

    /// The shared zero buffer, grown to cover `vertices` at the widest paint attribute.
    ///
    /// Growing replaces it and retires the old one to `retiredZeroPaint_`, because meshes built
    /// before the growth still point at it.
    filament::BufferObject* zeroPaint(std::size_t vertices);

    /// A geometry's GPU buffers, kept until it retires.
    struct Mesh {
        filament::VertexBuffer* vertices = nullptr;
        filament::IndexBuffer* indices = nullptr;
        std::uint32_t indexCount = 0;
        std::int32_t layerIndex = -1;
        std::uint8_t zoom = 0;
        std::uint8_t overscaledZoom = 0;
        TileID tile{};
        /// The atlas this drawable samples, or zero. Held with the mesh because the reference
        /// arrives with the geometry and is needed when the batch that draws it is issued.
        std::uint64_t texture = 0;
        /// The second picture, for a raster tile fading from its parent.
        std::uint64_t texture1 = 0;
        /// How slot zero is sampled: 0 linear, 1 nearest. See `filterFor`.
        ///
        /// After the textures, because both mesh records above are built with positional
        /// initialisers and a field between them would silently take the next one's value.
        std::uint32_t filter = 0;
        /// Whether this drawable writes colour.
        ///
        /// `DrawFlags::ENABLE_COLOR`, cleared for an extrusion's depth-only pass. Ignoring it drew
        /// that pass *as* the colour pass, which is what a building looked like before: a flat
        /// footprint in the roof's shade, with no walls and no depth between them.
        bool colour = true;
        /// Whether the producer asked for this drawable to be clipped to its tile.
        ///
        /// `DrawFlags::ENABLE_STENCIL`, carried on the geometry because that is where it
        /// arrives and the batch that draws it does not repeat it. A fill or a line sets it; a
        /// symbol and a circle do not, and clipping one of those to its tile cuts a label in
        /// half at the tile edge it crosses.
        bool clipped = false;
        /// Which of this drawable's paint properties came per feature rather than per layer.
        ///
        /// One bit per data-driven-capable property of the family, in the order
        /// `paintSlots` lists them, which is the order the material's constants are specialized
        /// in. mbgl's permutation, derived here from the attributes that actually arrived rather
        /// than read off the wire: the mesh is what declares the slots, so the mesh is what has
        /// to agree with the material about which of them the shader may read.
        std::uint32_t paintMask = 0;
        /// Buffer objects this mesh made, which it also destroys.
        ///
        /// The shared zero buffer is deliberately not among them -- it outlives every mesh that
        /// points at it.
        std::vector<filament::BufferObject*> ownedBuffers = {};
    };

    /// One layer's uniform blocks, by slot.
    using Blocks = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;

    /// Destroys every cached material instance.
    ///
    /// For a texture being replaced: an instance holds the samplers set on it until it is set
    /// again, so one that named the old texture would draw with a freed handle. See the call
    /// site for why this is safe only while no renderable exists.
    void dropInstances();

    void clearScene();

    /// Turns a wall drawable's instances into ordinary geometry. See the definition for why.
    bool expandWalls(const DrawableAdd& add);

    /// Builds an extrusion roof, keying attributes by id and filling in the ones a style left
    /// constant. See the definition for why that is not the generic path.
    bool buildRoof(const DrawableAdd& add);

    /// Builds a symbol drawable, keyed by attribute id, folding the fade into the float channel.
    bool buildSymbol(const DrawableAdd& add);

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
    /// The bent mask, and the subdivided quads it runs on.
    ///
    /// A globe's mask cannot be the flat one's four corners: bent, four corners are a sheet
    /// through the inside of the planet, and a stencil cut from that clips the wrong region. So a
    /// grid, and one per cell count rather than one overall -- the grid has to match the one the
    /// producer split that tile's *fills* against, or the mask cuts a sliver off every tile edge
    /// or leaves one. Keyed by cell count, which is a small set: one to forty-one.
    filament::Material* maskGlobeMaterial_ = nullptr;
    /// The same mask on the anchored expansion, for tiles drawn that way.
    filament::Material* maskAnchoredMaterial_ = nullptr;
    /// This frame's bend coefficients, by tile, harvested from the queued batches before the
    /// masks are written. A mask's own record carries only a placement matrix.
    std::map<TileID, std::array<filament::math::float4, 6>> tileBends_;
    struct MaskGrid {
        filament::VertexBuffer* vertices = nullptr;
        filament::IndexBuffer* indices = nullptr;
        std::uint32_t index_count = 0;
    };
    std::unordered_map<std::uint32_t, MaskGrid> maskGrids_;

    /// The grid for `cells` a side, made once and kept.
    MaskGrid maskGrid(std::uint32_t cells);

    /// Uploads a drawable's indices, rebasing a segmented bucket onto its whole vertex buffer.
    filament::IndexBuffer* uploadIndices(const DrawableAdd& add);

    /// The globe's depth shell -- an opaque sphere just beneath the surface, which is what stops
    /// the far side of the planet drawing through the near one. See `globe_shell.mat`.
    filament::Material* shellMaterial_ = nullptr;
    filament::MaterialInstance* shellInstance_ = nullptr;
    filament::VertexBuffer* shellVertices_ = nullptr;
    filament::IndexBuffer* shellIndices_ = nullptr;
    std::uint32_t shellIndexCount_ = 0;
    /// The colour the shell is painted, taken from the last background drawable seen. A tile that
    /// has not arrived then reads as ocean rather than as a hole through the planet.
    filament::math::float4 shellColor_{0.0f, 0.0f, 0.0f, 1.0f};

    /// Adds the shell to the scene for this frame, building it on first use.
    void writeShell();
    std::uint64_t rebased_ = 0;
    std::uint64_t masked_ = 0;
    std::uint64_t glyphsDrawn_ = 0;
    std::uint64_t glyphsHidden_ = 0;
    std::uint64_t unmasked_ = 0;

    filament::Engine* engine_ = nullptr;
    filament::Scene* scene_ = nullptr;

    std::unordered_map<std::int32_t, filament::Material*> materials_;
    /// The package bytes behind every material above, kept so a permutation can be built later.
    ///
    /// Keyed by family and surface, because `Material::Builder::constant` needs the package and
    /// resolves at build: one package is every permutation of a family, but each permutation is
    /// its own `Material`. Enumerating them offline is what does not scale -- the line family has
    /// six data-driven properties, so sixty-four packages, times three surfaces -- and
    /// `native/test/permutation_probe.cc` is where that was settled.
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> packages_;
    /// Permutations built on demand, keyed by family, surface and paint mask.
    std::unordered_map<std::uint64_t, filament::Material*> permuted_;
    /// The unread paint slot of every constant-paint drawable in the frame.
    ///
    /// Filament bakes `requires` into the package and refuses a primitive whose vertex buffer
    /// does not declare what its material requires, so a drawable whose colour is the layer's
    /// still has to declare the attribute it will never read. One buffer serves all of them:
    /// declared, never sampled because the specialization compiled the branch out, and grown to
    /// the largest vertex count seen rather than allocated per drawable.
    filament::BufferObject* zeroPaint_ = nullptr;
    /// How many vertices `zeroPaint_` covers at the widest declared paint attribute.
    std::size_t zeroPaintVertices_ = 0;
    /// Zero buffers a growth replaced, destroyed when the renderer is.
    ///
    /// Not destroyed at the growth: meshes built before it still point at the old buffer, and
    /// Filament does not reference-count a `BufferObject` against the vertex buffers holding it
    /// -- destroying one in use is undefined. Bounded by the doubling, so a handful over a
    /// process rather than one per frame.
    std::vector<filament::BufferObject*> retiredZeroPaint_;
    /// The bent families again, expanded about the tile rather than bent by trig. Selected per
    /// drawable above `kAnchoredFromZoom`; see `fill_globe_anchored.mat`.
    std::unordered_map<std::int32_t, filament::Material*> anchoredMaterials_;
    /// How many drawables took the anchored path this frame, for the counters.
    std::uint64_t anchoredDrawn_ = 0;
    /// The same families bent onto a sphere, loaded from `<stem>_globe.filamat`.
    ///
    /// A second map rather than a variant inside the first: the two differ in what a vertex *is*
    /// -- a Mercator drawable's matrix reaches clip space and a globe's reaches normalized
    /// Mercator -- so they are different materials with different parameters, not one material in
    /// two moods. A family with no globe package is skipped under a globe rather than drawn flat,
    /// because a flat layer over a bent one is a worse picture than a missing layer and a much
    /// harder one to diagnose.
    std::unordered_map<std::int32_t, filament::Material*> globeMaterials_;
    /// The projection the current frame's batches draw under.
    std::int32_t projection_ = TSL_PROJECTION_MODE_MERCATOR;
    /// Unit sphere to clip, meaningful only under `TSL_PROJECTION_MODE_GLOBE`.
    filament::math::mat4f globeMatrix_;
    /// Families asked for under a globe that have no globe package. Named once each, like
    /// `missingFamilies_`, so "this style has no globe materials" reads differently from "this
    /// layer kind has none yet".
    std::vector<std::int32_t> missingGlobeFamilies_;
    std::size_t materialsRejected_ = 0;
    std::unordered_map<std::uint64_t, Mesh> meshes_;
    /// Uniform blocks, keyed by *view and* layer.
    ///
    /// The view is not decoration. A heatmap writes two blocks at layer 2 slot 5 -- the kernels'
    /// evaluated properties in its offscreen view, and the second pass's props in the map's --
    /// and keyed on the layer alone the second overwrote the first. The kernels then read an
    /// eighty-byte block as a sixteen-byte one, took a matrix out of the wrong bytes, and
    /// rasterized nothing at all.
    ///
    /// The oracle's own dump had the identical bug and was fixed the identical way: mbgl
    /// hardcodes a render target's group to index 0, so two heatmap layers both claimed it.
    std::unordered_map<std::uint64_t, Blocks> uniforms_;

    /// The key `uniforms_` is held under. Frame-wide blocks arrive with layer `-1`.
    static std::uint64_t uniformKey(std::uint32_t view, std::int32_t layer) noexcept {
        return (static_cast<std::uint64_t>(view) << 32)
               | static_cast<std::uint32_t>(layer);
    }

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
    /// The bool is whether this drawable took the anchored bend: the two forms are different
    /// materials declaring different parameters, so an instance cached under one must never be
    /// handed to the other when a tile crosses the threshold mid-zoom.
    ///
    /// The last field is the paint permutation, for the same reason: a data-driven fill and a
    /// constant one are different programs, and an instance made against one declares parameters
    /// the other does not have.
    std::map<std::tuple<std::uint32_t, std::int32_t, std::uint32_t, bool, std::uint32_t>,
             filament::MaterialInstance*>
        instances_;

    /// The layer every renderable goes on. See the constructor.
    std::uint8_t layer_ = 0x01;
    bool flipY_ = true;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    std::uint64_t missing_ = 0;
    std::vector<std::int32_t> missingFamilies_;
    std::map<std::uint8_t, std::uint64_t> zooms_;
    std::map<std::uint8_t, std::uint64_t> overZooms_;
    std::uint64_t ordered_ = 0;
    std::uint64_t unplaced_ = 0;
    std::uint64_t atlasMismatched_ = 0;
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
    std::uint64_t missingAtlas_ = 0;
    std::uint64_t pitchedLabels_ = 0;

    /// Atlases by id, kept until the engine goes. A glyph or sprite atlas outlives any one tile
    /// and is re-uploaded in rects as it fills, so it is owned here rather than with a drawable.
    std::unordered_map<std::uint64_t, filament::Texture*> textures_;

    /// The offscreen passes, keyed for lookup and kept in order for the caller.
    std::vector<OffscreenPass> offscreen_;
    std::unordered_map<std::uint32_t, std::size_t> offscreenByView_;
    /// The next layer bit an offscreen pass takes.
    ///
    /// The mask is eight bits and the caller has already spent one per pane -- four for the
    /// quad. So this counts down from the top and a style with more heatmap layers than bits
    /// left gets no pass rather than a wrong one: drawing its kernels onto the map would be a
    /// picture, and drawing nothing is a missing layer, which is the failure worth having.
    std::uint8_t nextOffscreenLayer_ = 0x80;
    std::uint64_t textureUploads_ = 0;
    std::uint64_t textureSkipped_ = 0;
};

} // namespace tsf

#endif // TSF_FILAMENT_RENDERER_H
