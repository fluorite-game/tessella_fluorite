// SPDX-License-Identifier: Apache-2.0

#include <tsf/filament_renderer.h>

#include <tessella_capture_abi.h>

#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace tsf {
namespace {

/// The file name a shader family's package is expected under.
///
/// Named rather than numbered so a directory listing says what it holds. The families are the
/// ones a real basemap asks for -- measured against OpenFreeMap's liberty, which needs nine.
std::int32_t familyOf(const std::string& stem) {
    if (stem == "background") return TSL_BUILTIN_BACKGROUND_SHADER;
    if (stem == "fill") return TSL_BUILTIN_FILL_SHADER;
    if (stem == "fill_outline") return TSL_BUILTIN_FILL_OUTLINE_SHADER;
    if (stem == "fill_pattern") return TSL_BUILTIN_FILL_PATTERN_SHADER;
    if (stem == "fill_outline_pattern") return TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER;
    if (stem == "fill_extrusion") return TSL_BUILTIN_FILL_EXTRUSION_SHADER;
    if (stem == "fill_extrusion_instanced") return TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER;
    if (stem == "line") return TSL_BUILTIN_LINE_SHADER;
    if (stem == "circle") return TSL_BUILTIN_CIRCLE_SHADER;
    if (stem == "symbol_sdf") return TSL_BUILTIN_SYMBOL_SDFSHADER;
    if (stem == "symbol_icon") return TSL_BUILTIN_SYMBOL_ICON_SHADER;
    if (stem == "raster") return TSL_BUILTIN_RASTER_SHADER;
    return TSL_BUILTIN_NONE;
}

/// Which uniform slot carries a family's per-drawable block.
///
/// The ids collide across families by design -- `TSL_UBO_ID_FILL_DRAWABLE_UBO` and
/// `TSL_UBO_ID_BACKGROUND_DRAWABLE_UBO` are both 2 -- because they are slots within a family's
/// own layout rather than a global numbering. A family is always known here, so the collision
/// costs nothing.
constexpr std::uint32_t kDrawableSlot = 2;

/// What separates consecutive drawable blocks in a layer's consolidated buffer.
///
/// The *union's* stride, not the block's. `ubo.rs` says why, and predicted this bug exactly: a
/// plain fill writes an 80-byte `FillDrawableUBO` into a 96-byte slot because the pattern variants
/// are larger and set the stride for everyone. Reading at `sizeof` puts every entry after the
/// first at the wrong offset -- a layer whose tiles are drawn with each other's matrices, which is
/// plausible-looking output no size check catches, and duly was not caught until the picture
/// showed one tile's streets where four tiles' should have been.
std::size_t drawableStride(std::int32_t family) {
    switch (family) {
        case TSL_BUILTIN_BACKGROUND_SHADER:
        case TSL_BUILTIN_BACKGROUND_PATTERN_SHADER:
            return TSL_STRIDE_BACKGROUND_DRAWABLE_UNION_UBO;
        case TSL_BUILTIN_LINE_SHADER:
        case TSL_BUILTIN_LINE_GRADIENT_SHADER:
        case TSL_BUILTIN_LINE_PATTERN_SHADER:
        case TSL_BUILTIN_LINE_SDFSHADER:
            return TSL_STRIDE_LINE_DRAWABLE_UNION_UBO;
        default:
            return TSL_STRIDE_FILL_DRAWABLE_UNION_UBO;
    }
}
/// And which carries the layer's evaluated paint.
constexpr std::uint32_t kPropsSlot = 5;

/// Which primitive a family's indices describe.
///
/// The ABI carries no topology, and it does not need to: the family settles it. A fill outline is
/// the same vertices as its fill with its own indices over them, and those indices are pairs --
/// mbgl draws them with `gfx::DrawMode::Lines`. Drawing them as triangles produces geometry that
/// is wrong in a way that still fills pixels, which is the kind of wrong worth naming.
filament::RenderableManager::PrimitiveType primitiveFor(std::int32_t family) {
    switch (family) {
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_TRIANGULATED_SHADER:
            return filament::RenderableManager::PrimitiveType::LINES;
        default:
            return filament::RenderableManager::PrimitiveType::TRIANGLES;
    }
}

/// Where a family's evaluated-paint block keeps its colour.
///
/// Every block opens with `color[4]`, except that an outline wants the *outline* colour, which the
/// fill block keeps immediately after it.
std::size_t colourOffset(std::int32_t family) {
    switch (family) {
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER:
            return offsetof(tsl_fill_evaluated_props_ubo, outline_color);
        default:
            return 0;
    }
}

/// Where a family's evaluated-paint block keeps its opacity.
///
/// Every one of them opens with `color[4]`, which is why the colour read is family-agnostic. What
/// follows differs: background is colour then opacity, fill puts `outline_color[4]` between them.
/// Reading fill's layout out of a background block overruns a 32-byte buffer, and the size check
/// that caught it was silently costing the background its colour -- twelve of seventy-seven
/// instances drew fully transparent.
std::size_t opacityOffset(std::int32_t family, std::size_t bytes) {
    switch (family) {
        case TSL_BUILTIN_FILL_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
            return offsetof(tsl_fill_evaluated_props_ubo, opacity);
        case TSL_BUILTIN_BACKGROUND_SHADER:
            return offsetof(tsl_background_props_ubo, opacity);
        default:
            // Unknown layouts still open with a colour; the opacity is left at one rather than
            // read from an offset nothing has checked.
            return bytes;
    }
}

/// Filament's attribute type for what the wire declares.
///
/// Only the types tessella actually publishes. A drawable arriving with anything else is left
/// alone rather than guessed at: reading i16 as f32 produces geometry that is wrong in a way
/// nothing downstream can detect.
bool attributeType(std::uint8_t wire, filament::VertexBuffer::AttributeType& out) {
    using AT = filament::VertexBuffer::AttributeType;
    switch (wire) {
        case TSL_ATTRIBUTE_DATA_TYPE_SHORT2: out = AT::SHORT2; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_SHORT3: out = AT::SHORT3; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_SHORT4: out = AT::SHORT4; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_USHORT2: out = AT::USHORT2; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_USHORT4: out = AT::USHORT4; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_UBYTE4: out = AT::UBYTE4; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_BYTE4: out = AT::BYTE4; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT2: out = AT::FLOAT2; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT3: out = AT::FLOAT3; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT4: out = AT::FLOAT4; return true;
        default: return false;
    }
}

} // namespace

FilamentRenderer::FilamentRenderer(filament::Engine* engine,
                                   filament::Scene* scene,
                                   const std::string& materialDir,
                                   std::uint32_t width,
                                   std::uint32_t height)
    : engine_(engine), scene_(scene), width_(width), height_(height) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(materialDir, ec)) {
        if (entry.path().extension() != ".filamat") {
            continue;
        }
        const std::int32_t family = familyOf(entry.path().stem().string());
        if (family == TSL_BUILTIN_NONE) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
        if (package.empty()) {
            continue;
        }
        auto* material =
            filament::Material::Builder().package(package.data(), package.size()).build(*engine_);
        if (material != nullptr) {
            materials_[family] = material;
        }
    }

    // The mask material is not a family: it draws the clip quads and writes only the stencil.
    const auto maskPath = std::filesystem::path(materialDir) / "mask.filamat";
    if (std::filesystem::exists(maskPath, ec)) {
        std::ifstream file(maskPath, std::ios::binary);
        const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
        if (!package.empty()) {
            maskMaterial_ =
                filament::Material::Builder().package(package.data(), package.size()).build(*engine_);
        }
    }

    // One unit quad, reused by every mask: the tile's matrix is what places it.
    const std::int16_t quad[8] = {0, 0, 8192, 0, 8192, 8192, 0, 8192};
    const std::uint16_t tris[6] = {0, 1, 2, 0, 2, 3};
    maskVertices_ = filament::VertexBuffer::Builder()
                        .vertexCount(4)
                        .bufferCount(1)
                        .attribute(filament::VertexAttribute::POSITION, 0,
                                   filament::VertexBuffer::AttributeType::SHORT2, 0,
                                   sizeof(std::int16_t) * 2)
                        .build(*engine_);
    auto* qv = static_cast<std::uint8_t*>(std::malloc(sizeof quad));
    std::memcpy(qv, quad, sizeof quad);
    maskVertices_->setBufferAt(*engine_, 0,
                               filament::VertexBuffer::BufferDescriptor(
                                   qv, sizeof quad,
                                   [](void* b, std::size_t, void*) { std::free(b); }));
    maskIndices_ = filament::IndexBuffer::Builder()
                       .indexCount(6)
                       .bufferType(filament::IndexBuffer::IndexType::USHORT)
                       .build(*engine_);
    auto* qi = static_cast<std::uint8_t*>(std::malloc(sizeof tris));
    std::memcpy(qi, tris, sizeof tris);
    maskIndices_->setBuffer(*engine_, filament::IndexBuffer::BufferDescriptor(
                                          qi, sizeof tris,
                                          [](void* b, std::size_t, void*) { std::free(b); }));
}

void FilamentRenderer::onStencilTiles(const StencilTiles& tiles) {
    // Kept for this frame only. The producer names the tile set a layer group wants clipped *now*;
    // holding onto earlier frames' tiles leaves stale masks overlapping the live ones, and since
    // the reference is assigned by walking the set, every tile's reference also shifts as the set
    // grows. Both are silent: the masks are all written and every drawable still gets a reference,
    // so nothing counts wrong -- the clip simply stops meaning anything.
    for (const tsl_stencil_tile& tile : tiles.tiles) {
        TileID id{tile.tile.z, tile.tile.x, tile.tile.y, tile.tile.wrap, tile.tile.overscaled_z};
        filament::math::mat4f matrix;
        std::memcpy(&matrix, tile.matrix, sizeof matrix);
        masks_[id] = matrix;
    }
}

std::uint8_t FilamentRenderer::referenceFor(const TileID& tile) const {
    const auto found = references_.find(tile);
    return found == references_.end() ? 0 : found->second;
}

void FilamentRenderer::writeMasks() {
    references_.clear();
    if (maskMaterial_ == nullptr || masks_.empty()) {
        return;
    }
    // The reference is the tile's *zoom*, not a serial number, and the masks are banded by zoom
    // so the coarse ones are drawn first. Iteration order alone is not enough: Filament orders
    // within a priority band as it likes, so nine masks all at priority zero land in an order
    // nobody chose -- and the whole scheme depends on a child's quad overwriting its parent's.
    //
    // With the zoom as the value, a pixel ends up holding the finest zoom that covers it, and a
    // drawable testing equal to its own zoom draws only where nothing finer replaced it. Tiles of
    // one zoom are disjoint, so sharing a value between them costs nothing.
    std::uint8_t coarsest = 255;
    for (const auto& [tile, matrix] : masks_) {
        coarsest = std::min(coarsest, tile.overscaled_z);
    }
    // A reference *per tile*, not per zoom. Keying it by zoom is vacuous whenever the cover is one
    // zoom -- every mask writes the same value, every drawable tests for it, and the test passes
    // everywhere. Per tile it does the work it exists for: a tile's geometry runs well past its
    // own edge into the buffer that hides seams, and the mask is what stops that overhang painting
    // over the neighbour it overlaps.
    std::uint8_t next = 1;
    for (const auto& [tile, matrix] : masks_) {
        if (next == 255) {
            break;
        }
        const std::uint8_t reference = next++;
        references_[tile] = reference;
        const auto band = static_cast<std::uint8_t>(
            std::min<int>(3, static_cast<int>(tile.overscaled_z) - coarsest));

        auto* instance = maskMaterial_->createInstance();
        maskInstances_.push_back(instance);
        instance->setColorWrite(std::getenv("TSF_SHOW_MASKS") != nullptr);
        // Each mask painted by its own reference, so the stencil's layout can be looked at.
        instance->setParameter("color", filament::math::float4{
            static_cast<float>(reference) / 8.0f,
            static_cast<float>(tile.overscaled_z % 4) / 4.0f, 0.5f, 1.0f});
        instance->setParameter("opacity", 1.0f);
        instance->setDepthWrite(false);
        instance->setStencilWrite(true);
        instance->setStencilReferenceValue(reference);
        instance->setStencilCompareFunction(filament::MaterialInstance::StencilCompareFunc::A);
        instance->setStencilOpDepthStencilPass(filament::MaterialInstance::StencilOperation::REPLACE);

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .culling(false)
            .priority(band)
            .material(0, instance)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, maskVertices_,
                      maskIndices_, 0, 6);
        utils::Entity entity = utils::EntityManager::get().create();
        const auto built = builder.build(*engine_, entity);
        auto& transforms = engine_->getTransformManager();
        transforms.setTransform(transforms.getInstance(entity), matrix);
        scene_->addEntity(entity);
        entities_.push_back(entity);
        masked_++;
    }
}

FilamentRenderer::~FilamentRenderer() {
    clearScene();
    for (auto& [key, instance] : instances_) {
        engine_->destroy(instance);
    }
    instances_.clear();
    for (auto& [id, mesh] : meshes_) {
        engine_->destroy(mesh.vertices);
        engine_->destroy(mesh.indices);
    }
    for (auto& [family, material] : materials_) {
        engine_->destroy(material);
    }
}

void FilamentRenderer::clearScene() {
    for (utils::Entity entity : entities_) {
        scene_->remove(entity);
        engine_->destroy(entity);
        utils::EntityManager::get().destroy(entity);
    }
    entities_.clear();
    for (filament::MaterialInstance* instance : maskInstances_) {
        engine_->destroy(instance);
    }
    maskInstances_.clear();
}

void FilamentRenderer::beginFrame(std::uint64_t) {
    // The scene is rebuilt from the order every frame. A drawable's *buffers* persist -- they are
    // what the producer works to avoid resending -- but which of them are drawn, in what sequence,
    // and against which uniforms is the frame's own answer.
    clearScene();
    masks_.clear();
    pending_.clear();
    renderables_ = 0;
    primitives_ = 0;
    coloured_ = 0;
    ordered_ = 0;
    unplaced_ = 0;
    scissored_ = 0;
    masked_ = 0;
    unmasked_ = 0;
    zooms_.clear();
    overZooms_.clear();
    slotsThisFrame_.clear();
    placements_.clear();
    scales_.clear();
    sharedSlots_ = 0;
    drawnThisFrame_.clear();
    passes_.clear();
    redrawn_ = 0;
}

void FilamentRenderer::onGeometry(const DrawableAdd& add) {

    if (add.vertexCount == 0 || add.indexes.empty()) {
        return;
    }

    // One buffer per attribute, which is what the wire describes: each names its own slab and
    // stride, and nothing promises they are interleaved in one allocation.
    std::vector<const Attribute*> usable;
    for (const Attribute& attribute : add.attrs) {
        filament::VertexBuffer::AttributeType type{};
        if (attribute.desc.binding >= 0 && !attribute.data.empty() &&
            attributeType(attribute.desc.data_type, type)) {
            usable.push_back(&attribute);
        }
    }
    if (usable.empty()) {
        return;
    }

    filament::VertexBuffer::Builder builder;
    builder.vertexCount(static_cast<std::uint32_t>(add.vertexCount))
        .bufferCount(static_cast<std::uint8_t>(usable.size()));
    for (std::size_t i = 0; i < usable.size(); i++) {
        filament::VertexBuffer::AttributeType type{};
        attributeType(usable[i]->desc.data_type, type);
        // The first attribute is the position; the rest are paint the shaders read as custom
        // channels. Filament names its slots, so they are assigned in the order the wire lists
        // them, which is the order the family's attribute ids already put them in.
        const auto slot = static_cast<filament::VertexAttribute>(
            i == 0 ? filament::VertexAttribute::POSITION
                   : filament::VertexAttribute::CUSTOM0 + static_cast<int>(i) - 1);
        builder.attribute(slot, static_cast<std::uint8_t>(i), type, usable[i]->desc.offset,
                          usable[i]->desc.stride);
    }
    auto* vertices = builder.build(*engine_);
    if (vertices == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < usable.size(); i++) {
        // Copied onto the heap and freed by the descriptor's callback, not held on the stack.
        // `BufferDescriptor` does not copy: it takes the pointer and calls back when the driver
        // is done with it, which is later than this loop. A stack buffer here is a use-after-free
        // that presents as a double free somewhere else entirely -- which is how it was found.
        //
        // This is also what §11.7 means by releasing a slab only after the driver's copy
        // completes: the producer's own bytes are never handed to the driver, so the host may
        // retire the ring position as soon as the tick returns.
        auto* owned = static_cast<std::uint8_t*>(std::malloc(usable[i]->data.size));
        if (owned == nullptr) {
            continue;
        }
        std::memcpy(owned, usable[i]->data.data, usable[i]->data.size);
        vertices->setBufferAt(*engine_, static_cast<std::uint8_t>(i),
                              filament::VertexBuffer::BufferDescriptor(
                                  owned, usable[i]->data.size,
                                  [](void* buffer, std::size_t, void*) { std::free(buffer); }));
    }

    const std::uint32_t indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    auto* indices = filament::IndexBuffer::Builder()
                        .indexCount(indexCount)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine_);
    if (indices == nullptr) {
        engine_->destroy(vertices);
        return;
    }
    auto* ownedIndices = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
    if (ownedIndices == nullptr) {
        engine_->destroy(indices);
        engine_->destroy(vertices);
        return;
    }
    std::memcpy(ownedIndices, add.indexes.data, add.indexes.size);
    indices->setBuffer(*engine_,
                       filament::IndexBuffer::BufferDescriptor(
                           ownedIndices, add.indexes.size,
                           [](void* buffer, std::size_t, void*) { std::free(buffer); }));

    onRetire(add.id);
    meshes_[add.id] = Mesh{vertices, indices, indexCount, add.layerIndex,
                           add.tileID ? add.tileID->z : std::uint8_t{0},
                           add.tileID ? add.tileID->overscaled_z : std::uint8_t{0},
                           add.tileID ? *add.tileID : TileID{}};
}

void FilamentRenderer::onRetire(std::uint64_t id) {
    const auto found = meshes_.find(id);
    if (found == meshes_.end()) {
        return;
    }
    engine_->destroy(found->second.vertices);
    engine_->destroy(found->second.indices);
    meshes_.erase(found);
}

void FilamentRenderer::onUniforms(const UboUpdate& update) {
    // A frame-wide block has no layer; it is kept under -1 so the lookup is uniform.
    const std::int32_t layer = update.layerIndex.value_or(-1);
    auto& blocks = uniforms_[layer];
    blocks[update.slot].assign(update.bytes.data, update.bytes.data + update.bytes.size);
}

void FilamentRenderer::onBatch(const Batch& batch) {
    pending_.push_back(batch);
}

void FilamentRenderer::endFrame(std::uint64_t) {
    // The clip masks first, so every drawable issued below has a reference to test against.
    writeMasks();
    // Reversed: see `pending_`. The producer's order is front-to-back and this pass blends.
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        issue(*it);
    }
    pending_.clear();
}

void FilamentRenderer::issue(const Batch& batch) {
    if (std::getenv("TSF_ONLY_MASKS")) {
        return;
    }
    // Diagnostics: draw one layer, or drop the background, so a frame can be compared against the
    // oracle rendering the same subset.
    if (const char* only = std::getenv("TSF_ONLY_LAYER")) {
        if (batch.layerIndex != static_cast<std::uint32_t>(std::atoi(only))) {
            return;
        }
    }
    if (std::getenv("TSF_SKIP_BACKGROUND") && batch.builtinShader == TSL_BUILTIN_BACKGROUND_SHADER) {
        return;
    }

    const auto material = materials_.find(batch.builtinShader);
    if (material == materials_.end()) {
        missing_++;
        if (std::find(missingFamilies_.begin(), missingFamilies_.end(), batch.builtinShader) ==
            missingFamilies_.end()) {
            missingFamilies_.push_back(batch.builtinShader);
        }
        return;
    }

    const auto layer = uniforms_.find(static_cast<std::int32_t>(batch.layerIndex));

    // One renderable per drawable, because a renderable carries one transform and each drawable
    // carries its own matrix -- different tiles do not share one. That gives up the multi-primitive
    // batching `DrawList` groups for; recovering it means splitting a batch by matrix, which is
    // worth doing once there is a picture to measure it against.
    // Bands one to seven: zero belongs to the mask pass alone, so every clip is written before
    // any geometry tests against it. Sharing a band with the background left the order between
    // them unspecified, which is not a thing to leave to chance when one writes what the other
    // reads.
    // Bands four to seven: zero to three belong to the mask pass, which must have written every
    // clip before any geometry tests against it.
    const auto band = static_cast<std::uint8_t>(
        4 + std::min<std::uint32_t>(3, batch.layerIndex / 32));

    for (std::size_t i = 0; i < batch.geometries.size(); i++) {
        const auto mesh = meshes_.find(batch.geometries[i]);
        if (mesh == meshes_.end()) {
            continue;
        }

        zooms_[mesh->second.zoom]++;
        overZooms_[mesh->second.overscaledZoom]++;
        // Two drawables of one layer sharing a matrix slot would be stacked on the same tile,
        // compositing there again and again.
        if (!slotsThisFrame_.insert({batch.layerIndex, batch.uboIndexes[i]}).second) {
            sharedSlots_++;
        }
        // Counted, not skipped. The same geometry legitimately appears many times in a frame:
        // the background is one quad shared by every tile of the cover and drawn once per tile,
        // with that tile's matrix. Deduplicating by geometry id collapses those into one and the
        // cover goes dark, which is how this was found.
        if (!drawnThisFrame_.insert(batch.geometries[i]).second) {
            redrawn_++;
        }
        passes_[batch.pass]++;
        // The matrix this drawable is placed by. Skipped rather than defaulted when it cannot be
        // read: identity is not a neutral choice here -- it puts tile-local coordinates straight
        // into clip space, where they cover the viewport and look like a bug somewhere else.
        if (layer == uniforms_.end()) {
            continue;
        }
        const auto drawables = layer->second.find(kDrawableSlot);
        if (drawables == layer->second.end()) {
            continue;
        }
        const std::size_t at =
            static_cast<std::size_t>(batch.uboIndexes[i]) * drawableStride(batch.builtinShader);
        if (at + sizeof(float) * 16 > drawables->second.size()) {
            unplaced_++;
            continue;
        }
        filament::math::mat4f transform;
        std::memcpy(&transform, drawables->second.data() + at, sizeof(float) * 16);

        placements_.insert({transform[3][0], transform[3][1], transform[0][0]});
        scales_[transform[0][0]]++;
        // One instance per (layer, shader, tile slot). Keyed by the tile because the scissor is a
        // property of the instance and the clip is a property of the tile; still bounded by the
        // cover rather than one per primitive per frame.
        const auto key =
            std::make_tuple(batch.layerIndex, batch.builtinShader, batch.uboIndexes[i]);
        auto found = instances_.find(key);
        if (found == instances_.end()) {
            found = instances_.emplace(key, material->second->createInstance()).first;
            made_++;
        }
        auto* instance = found->second;

        if (const auto props = layer->second.find(kPropsSlot);
            props != layer->second.end() &&
            props->second.size() >= colourOffset(batch.builtinShader) + sizeof(float) * 4) {
            float colour[4] = {0, 0, 0, 0};
            std::memcpy(colour, props->second.data() + colourOffset(batch.builtinShader),
                        sizeof colour);
            coloured_++;
            instance->setParameter(
                "color", filament::math::float4{colour[0], colour[1], colour[2], colour[3]});

            float opacity = 1.0f;
            const std::size_t off = opacityOffset(batch.builtinShader, props->second.size());
            if (off + sizeof(float) <= props->second.size()) {
                std::memcpy(&opacity, props->second.data() + off, sizeof opacity);
            }
            instance->setParameter("opacity", opacity);

            // A line also needs its width, from the layer's paint, and the drawable's own ratio,
            // which is what keeps a road at a constant pixel width as the tile scales.
            if (batch.builtinShader == TSL_BUILTIN_LINE_SHADER) {
                float lineWidth = 1.0f;
                if (offsetof(tsl_line_evaluated_props_ubo, width) + sizeof(float) <=
                    props->second.size()) {
                    std::memcpy(&lineWidth,
                                props->second.data() +
                                    offsetof(tsl_line_evaluated_props_ubo, width),
                                sizeof lineWidth);
                }
                instance->setParameter("width", lineWidth);

                float ratio = 1.0f;
                const std::size_t ratioAt = at + offsetof(tsl_line_drawable_ubo, ratio);
                if (ratioAt + sizeof(float) <= drawables->second.size()) {
                    std::memcpy(&ratio, drawables->second.data() + ratioAt, sizeof ratio);
                }
                instance->setParameter("ratio", ratio);
                instance->setParameter("matrix", transform);
            }

            // An extrusion needs its base and height, its light, and the height factor that turns
            // metres into the tile's own units -- the last from the drawable block, the rest from
            // the layer's paint.
            if (batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER) {
                tsl_fill_extrusion_props_ubo paint{};
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                instance->setParameter("base", paint.base);
                instance->setParameter("height", paint.height);
                instance->setParameter("lightIntensity", paint.light_intensity);
                instance->setParameter(
                    "lightColor", filament::math::float3{paint.light_color[0],
                                                         paint.light_color[1],
                                                         paint.light_color[2]});

                tsl_fill_extrusion_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                instance->setParameter("heightFactor", block.height_factor);
                instance->setParameter("matrix", transform);
            }
        }

        // The tile's own clip, as a stencil test. A parent's geometry passes only where the
        // parent's own mask survived -- that is, where no child overwrote it -- which is what
        // stops an ancestor compositing over the children that replaced it.
        const std::uint8_t reference = referenceFor(mesh->second.tile);
        if (reference == 0) {
            unmasked_++;
        }
        if (reference != 0 && !std::getenv("TSF_NO_STENCIL")) {
            instance->setStencilWrite(false);
            instance->setStencilReferenceValue(std::getenv("TSF_IMPOSSIBLE_REF") ? 200 : reference);
            instance->setStencilCompareFunction(filament::MaterialInstance::StencilCompareFunc::E);
        }

        // The bounding-box scissor below is a coarser thing than the mask and does not replace it:
        // an ancestor's box is its whole extent, so clipping to it clips nothing. Kept because it
        // costs nothing and bounds what the stencil then refines.
        // §11.7 asks a consumer to honour the stencil tiles the producer
        // sends; this is that obligation met with a scissor. MVT geometry runs past its tile's
        // edge by design, into the buffer that exists to hide seams, so without a clip a tile
        // paints into its neighbour and what is already there blends a second time. That was the
        // banding: 122,097 pixels of a building grey composited twice, a colour the oracle never
        // produces.
        //
        // The rectangle comes from the drawable's own matrix rather than from the stencil record,
        // because the matrix already says where the tile's 0..8192 box lands and a screen-space
        // box is what a scissor takes. Exact while the map is north-up; a rotated or pitched view
        // needs the stencil buffer proper, which is why the record exists.
        {
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            const float corners[4][2] = {{0, 0}, {8192, 0}, {8192, 8192}, {0, 8192}};
            for (const auto& corner : corners) {
                const filament::math::float4 clip =
                    transform * filament::math::float4{corner[0], corner[1], 0.0f, 1.0f};
                if (clip.w == 0.0f) {
                    continue;
                }
                minX = std::min(minX, clip.x / clip.w);
                maxX = std::max(maxX, clip.x / clip.w);
                minY = std::min(minY, clip.y / clip.w);
                maxY = std::max(maxY, clip.y / clip.w);
            }
            const auto toPixels = [](float ndc, std::uint32_t extent) {
                return (ndc * 0.5f + 0.5f) * static_cast<float>(extent);
            };
            const float l = std::max(0.0f, std::floor(toPixels(minX, width_)));
            const float b = std::max(0.0f, std::floor(toPixels(minY, height_)));
            const float r = std::min(static_cast<float>(width_), std::ceil(toPixels(maxX, width_)));
            const float t =
                std::min(static_cast<float>(height_), std::ceil(toPixels(maxY, height_)));
            if (r > l && t > b && !std::getenv("TSF_NO_SCISSOR")) {
                instance->setScissor(
                    static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(b),
                    static_cast<std::uint32_t>(r - l), static_cast<std::uint32_t>(t - b));
                scissored_++;
            }
        }

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .culling(false)
            .priority(band)
            .material(0, instance)
            .geometry(0, primitiveFor(batch.builtinShader), mesh->second.vertices,
                      mesh->second.indices, 0, mesh->second.indexCount);

        utils::Entity entity = utils::EntityManager::get().create();
        builder.build(*engine_, entity);

        // The transform Filament applies itself. This is the whole reason the matrix left the
        // material: a worldPosition write in a vertex hook does not move anything.
        auto& transforms = engine_->getTransformManager();
        // A line places itself: it must extrude in tile units before the tile-to-clip transform,
        // so it takes the matrix as a parameter and its renderable carries the identity. Everything
        // else lets Filament apply the transform, which is cheaper and needs no vertex hook.
        const bool placesItself = batch.builtinShader == TSL_BUILTIN_LINE_SHADER ||
                                  batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER;
        transforms.setTransform(transforms.getInstance(entity),
                                placesItself ? filament::math::mat4f() : transform);

        scene_->addEntity(entity);
        entities_.push_back(entity);
        renderables_++;
        ordered_++;
        primitives_++;
    }
}

} // namespace tsf
