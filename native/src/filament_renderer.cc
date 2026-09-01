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
/// And which carries the layer's evaluated paint.
constexpr std::uint32_t kPropsSlot = 5;

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
                                   const std::string& materialDir)
    : engine_(engine), scene_(scene) {
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
}

void FilamentRenderer::beginFrame(std::uint64_t) {
    // The scene is rebuilt from the order every frame. A drawable's *buffers* persist -- they are
    // what the producer works to avoid resending -- but which of them are drawn, in what sequence,
    // and against which uniforms is the frame's own answer.
    clearScene();
    pending_.clear();
    renderables_ = 0;
    primitives_ = 0;
    coloured_ = 0;
    ordered_ = 0;
    zooms_.clear();
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
                           add.tileID ? add.tileID->z : std::uint8_t{0}};
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
    // Reversed: see `pending_`. The producer's order is front-to-back and this pass blends.
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        issue(*it);
    }
    pending_.clear();
}

void FilamentRenderer::issue(const Batch& batch) {
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

    // The layer's paint, shared by every drawable in it. One instance per (layer, shader), kept
    // across frames: a layer's colour is a property of the layer, not of a frame.
    const auto key = std::make_pair(batch.layerIndex, batch.builtinShader);
    auto found = instances_.find(key);
    if (found == instances_.end()) {
        found = instances_.emplace(key, material->second->createInstance()).first;
        made_++;
    }
    auto* instance = found->second;

    if (layer != uniforms_.end()) {
        const auto props = layer->second.find(kPropsSlot);
        if (props != layer->second.end() && props->second.size() >= sizeof(float) * 4) {
            float colour[4] = {0, 0, 0, 0};
            std::memcpy(colour, props->second.data(), sizeof colour);
            coloured_++;
            instance->setParameter(
                "color", filament::math::float4{colour[0], colour[1], colour[2], colour[3]});

            float opacity = 1.0f;
            const std::size_t at = opacityOffset(batch.builtinShader, props->second.size());
            if (at + sizeof(float) <= props->second.size()) {
                std::memcpy(&opacity, props->second.data() + at, sizeof opacity);
            }
            instance->setParameter("opacity", opacity);
        }
    }

    // One renderable per drawable, because a renderable carries one transform and each drawable
    // carries its own matrix -- different tiles do not share one. That gives up the multi-primitive
    // batching `DrawList` groups for; recovering it means splitting a batch by matrix, which is
    // worth doing once there is a picture to measure it against.
    const auto band = static_cast<std::uint8_t>(
        batch.layerIndex == 0 ? 0 : 1 + std::min<std::uint32_t>(6, batch.layerIndex / 16));

    for (std::size_t i = 0; i < batch.geometries.size(); i++) {
        const auto mesh = meshes_.find(batch.geometries[i]);
        if (mesh == meshes_.end()) {
            continue;
        }

        zooms_[mesh->second.zoom]++;
        filament::math::mat4f transform(1.0f);
        if (layer != uniforms_.end()) {
            const auto drawables = layer->second.find(kDrawableSlot);
            if (drawables != layer->second.end()) {
                const std::size_t stride = sizeof(tsl_fill_drawable_ubo);
                const std::size_t at = static_cast<std::size_t>(batch.uboIndexes[i]) * stride;
                if (at + sizeof(float) * 16 <= drawables->second.size()) {
                    std::memcpy(&transform, drawables->second.data() + at, sizeof(float) * 16);

                }
            }
        }

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .culling(false)
            .priority(band)
            .material(0, instance)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      mesh->second.vertices, mesh->second.indices, 0, mesh->second.indexCount);

        utils::Entity entity = utils::EntityManager::get().create();
        builder.build(*engine_, entity);

        // The transform Filament applies itself. This is the whole reason the matrix left the
        // material: a worldPosition write in a vertex hook does not move anything.
        auto& transforms = engine_->getTransformManager();
        transforms.setTransform(transforms.getInstance(entity), transform);

        scene_->addEntity(entity);
        entities_.push_back(entity);
        renderables_++;
        ordered_++;
        primitives_++;
    }
}

} // namespace tsf
