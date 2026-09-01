// SPDX-License-Identifier: Apache-2.0

#include <tsf/filament_renderer.h>

#include <tessella_capture_abi.h>

#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cstdio>
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
    for (filament::MaterialInstance* instance : instances_) {
        engine_->destroy(instance);
    }
    instances_.clear();
}

void FilamentRenderer::beginFrame(std::uint64_t) {
    // The scene is rebuilt from the order every frame. A drawable's *buffers* persist -- they are
    // what the producer works to avoid resending -- but which of them are drawn, in what sequence,
    // and against which uniforms is the frame's own answer.
    clearScene();
    renderables_ = 0;
    primitives_ = 0;
}

void FilamentRenderer::endFrame(std::uint64_t) {}

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

    auto builder = filament::VertexBuffer::Builder()
                       .vertexCount(static_cast<std::uint32_t>(add.vertexCount))
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
    meshes_[add.id] = Mesh{vertices, indices, indexCount, add.layerIndex};
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
    const auto material = materials_.find(batch.builtinShader);
    if (material == materials_.end()) {
        missing_++;
        if (std::find(missingFamilies_.begin(), missingFamilies_.end(), batch.builtinShader) ==
            missingFamilies_.end()) {
            missingFamilies_.push_back(batch.builtinShader);
        }
        return;
    }

    // The drawables this batch names that actually have buffers. A batch may name geometry that
    // arrived with nothing usable in it, and a renderable with zero primitives is refused by
    // Filament rather than drawn empty.
    std::vector<std::pair<const Mesh*, std::uint32_t>> parts;
    for (std::size_t i = 0; i < batch.geometries.size(); i++) {
        const auto mesh = meshes_.find(batch.geometries[i]);
        if (mesh != meshes_.end()) {
            parts.emplace_back(&mesh->second, batch.uboIndexes[i]);
        }
    }
    if (parts.empty()) {
        return;
    }

    const auto layer = uniforms_.find(static_cast<std::int32_t>(batch.layerIndex));

    auto builder = filament::RenderableManager::Builder(parts.size());
    // Tile-local coordinates, so the box is the tile. Culling is off because the matrix that
    // places this geometry lives in the material rather than in a transform Filament can see.
    //
    // Priority carries the painter order across. Filament draws by its own rules -- material,
    // then distance for blended objects -- and every drawable here sits at the same depth, so
    // without this the layer order the whole capture stream works to preserve is discarded at
    // the last step: liberty's water and roads drew *under* its background and the frame came
    // out a flat sheet of #f8f4f0.
    //
    // Eight levels for a hundred and eleven layers is lossy, and deliberately the coarse version
    // of the answer: it separates background from fill from line from symbol, which is what makes
    // a frame legible. Exact within-band order needs the depth buffer carrying the layer index,
    // which is a change to every material rather than to this.
    const auto band = static_cast<std::uint8_t>(
        batch.layerIndex == 0 ? 0 : 1 + std::min<std::uint32_t>(6, batch.layerIndex / 16));
    builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}}).culling(false).priority(band);

    for (std::size_t i = 0; i < parts.size(); i++) {
        auto* instance = material->second->createInstance();
        instances_.push_back(instance);

        if (layer != uniforms_.end()) {
            const auto drawables = layer->second.find(kDrawableSlot);
            if (drawables != layer->second.end()) {
                // The per-drawable block, indexed by the slot the order carries. mbgl packs one
                // of these per drawable into the layer's consolidated buffer, and `ubo_index` is
                // which one this drawable is.
                const std::size_t stride = sizeof(tsl_fill_drawable_ubo);
                const std::size_t at = static_cast<std::size_t>(parts[i].second) * stride;
                if (at + stride <= drawables->second.size()) {
                    tsl_fill_drawable_ubo block{};
                    std::memcpy(&block, drawables->second.data() + at, stride);
                    filament::math::mat4f matrix;
                    std::memcpy(&matrix, block.matrix, sizeof block.matrix);
                    instance->setParameter("matrix", matrix);
                }
            }
            const auto props = layer->second.find(kPropsSlot);
            if (props != layer->second.end() &&
                props->second.size() >= sizeof(tsl_fill_evaluated_props_ubo)) {
                tsl_fill_evaluated_props_ubo paint{};
                std::memcpy(&paint, props->second.data(), sizeof paint);
                instance->setParameter(
                    "color", filament::math::float4{paint.color[0], paint.color[1],
                                                    paint.color[2], paint.color[3]});
                instance->setParameter("opacity", paint.opacity);
            }
        }

        builder.material(i, instance)
            .geometry(i, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      parts[i].first->vertices, parts[i].first->indices, 0,
                      parts[i].first->indexCount);
    }

    utils::Entity entity = utils::EntityManager::get().create();
    builder.build(*engine_, entity);
    scene_->addEntity(entity);
    entities_.push_back(entity);
    renderables_++;
    primitives_ += parts.size();
}

} // namespace tsf
