// SPDX-License-Identifier: Apache-2.0

#include <tsf/filament_renderer.h>

#include <tessella_capture_abi.h>

#include <filament/Camera.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
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
#include <limits>

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
        case TSL_BUILTIN_RASTER_SHADER:
            // A matrix and nothing else, where a fill's block is 96. Falling through to the
            // fill's stride read every raster drawable after the first at the wrong offset.
            return sizeof(tsl_raster_drawable_ubo);
        case TSL_BUILTIN_SYMBOL_SDFSHADER:
        case TSL_BUILTIN_SYMBOL_ICON_SHADER:
        case TSL_BUILTIN_SYMBOL_TEXT_AND_ICON_SHADER:
            // 272: three matrices before anything else. Nothing else in the header is this size,
            // so falling through to the fill's 96 put every symbol drawable after the first in a
            // layer on another one's matrices.
            return sizeof(tsl_symbol_drawable_ubo);
        case TSL_BUILTIN_CIRCLE_SHADER:
            // 112, where a fill's is 96. As for the extrusions, the header emits no union
            // constant for this family because it has one drawable block, so the block's own
            // size is the stride.
            return sizeof(tsl_circle_drawable_ubo);
        case TSL_BUILTIN_FILL_EXTRUSION_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_PATTERN_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_PATTERN_INSTANCED_SHADER:
            // 112, where a fill's is 96. The header emits no union constant for this family
            // because every extrusion variant shares one drawable block, so the block's own size
            // *is* the stride here -- which is why `sizeof` is right for once and worth saying so
            // rather than leaving the next reader to wonder. Falling through to the fill's stride
            // read every drawable after the first at the wrong offset, which is a layer whose
            // buildings wear each other's matrices.
            return sizeof(tsl_fill_extrusion_drawable_ubo);
        default:
            return TSL_STRIDE_FILL_DRAWABLE_UNION_UBO;
    }
}
/// And which carries the layer's evaluated paint.
constexpr std::uint32_t kPropsSlot = 5;

/// A symbol's per-drawable pass flags, which are their own block rather than part of the paint.
constexpr std::uint32_t kSymbolTilePropsSlot = 3;

/// And a patterned fill's, which names the sprite rectangles for this tile.
///
/// Four, not three. Three is where a *symbol* keeps its tile props, and copying that constant
/// left every patterned fill reading a slot the producer never writes: `patternFrom` and
/// `patternTo` stayed zero, the sprite rectangle had no area, and the layer drew nothing at all
/// -- not even the `fill-color` beside it, because the drawable is the pattern permutation and
/// there is no fallback in it. `idFillTilePropsUBO` is 4 in the generated tables, which are
/// mbgl's own.
constexpr std::uint32_t kFillPatternTilePropsSlot = 4;

/// Whether a family resolves against the depth buffer.
///
/// The extrusions, and only them. A building is a volume and has to know which of its own faces is
/// in front; every other family lies flat on the map and painter order settles it.
///
/// Not read from `ENABLE_DEPTH`, which a background carries too. A background is a viewport quad
/// at a clip z of zero, and zero is the *near* plane in the producer's convention, so a background
/// taking part in depth stands in front of every building in the frame -- mbgl draws it
/// `DepthMaskType::ReadOnly` for exactly that reason, a distinction the single wire bit cannot
/// carry. `native/test/depth_probe.cc` shows it: a background quad at z zero that writes depth
/// hides both test quads, and the same quad with the write off does not.
bool resolvesInDepth(std::int32_t family) {
    return family == TSL_BUILTIN_FILL_EXTRUSION_SHADER
           || family == TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER
           || family == TSL_BUILTIN_FILL_EXTRUSION_PATTERN_SHADER
           || family == TSL_BUILTIN_FILL_EXTRUSION_PATTERN_INSTANCED_SHADER;
}

/// Whether a family carries its own matrix rather than taking the renderable's transform.
bool patternPlaces(std::int32_t family) {
    return family == TSL_BUILTIN_FILL_PATTERN_SHADER
           || family == TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER
           // A circle's quad is extruded in *clip* space, by a radius in pixels scaled by the
           // projected w -- so the matrix has to be inside the shader, and the renderable carries
           // the identity like a line's does.
           || family == TSL_BUILTIN_CIRCLE_SHADER;
}

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
        // The pattern variants share the fill block; only their tile props differ.
        case TSL_BUILTIN_FILL_PATTERN_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER:
            return offsetof(tsl_fill_evaluated_props_ubo, opacity);
        case TSL_BUILTIN_LINE_SHADER:
            return offsetof(tsl_line_evaluated_props_ubo, opacity);
        case TSL_BUILTIN_CIRCLE_SHADER:
            return offsetof(tsl_circle_evaluated_props_ubo, opacity);
        // Raster is deliberately absent: its block does not open with a colour, and it sets
        // `opacity` itself from `tsl_raster_evaluated_props_ubo` alongside the rest of its paint.
        case TSL_BUILTIN_BACKGROUND_SHADER:
            return offsetof(tsl_background_props_ubo, opacity);
        // Both extrusion families, which share one props block. Missing here, the default below
        // left `opacity` at one and a translucent building was drawn opaque: the roofs came out
        // at the lit colour instead of nine parts lit to one part what was behind them, which is
        // 3 of 255 on a roof and 14 on a wall, over a third of the frame. It is also why the
        // layer matched the oracle exactly at an opacity of one -- the only value at which not
        // blending is right.
        case TSL_BUILTIN_FILL_EXTRUSION_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER:
            return offsetof(tsl_fill_extrusion_props_ubo, opacity);
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
                                   std::uint32_t height,
                                   std::uint8_t layer)
    : engine_(engine), scene_(scene), layer_(layer), width_(width), height_(height) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(materialDir, ec)) {
        if (entry.path().extension() != ".filamat") {
            continue;
        }
        // `<stem>_globe` is the same family bent onto a sphere. Stripped before the lookup so
        // one table names the families, and filed separately so the two never shadow each other.
        std::string stem = entry.path().stem().string();
        const std::string suffix = "_globe";
        const bool bent = stem.size() > suffix.size()
                          && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (bent) {
            stem.erase(stem.size() - suffix.size());
        }
        const std::int32_t family = familyOf(stem);
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
            (bent ? globeMaterials_ : materials_)[family] = material;
        } else {
            // Filament has already said why on stderr. Counted so a caller can
            // tell "this directory has no materials" from "these materials were
            // refused", which are different mistakes with the same black frame.
            materialsRejected_++;
        }
    }

    // The bent mask, for the same reason the flat one is not a family. Loaded before the flat one
    // so a directory carrying only the flat mask still leaves a globe unmasked rather than masked
    // by a quad that reaches the wrong space.
    const auto globeMaskPath = std::filesystem::path(materialDir) / "mask_globe.filamat";
    if (std::filesystem::exists(globeMaskPath, ec)) {
        std::ifstream file(globeMaskPath, std::ios::binary);
        const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
        if (!package.empty()) {
            maskGlobeMaterial_ = filament::Material::Builder()
                                     .package(package.data(), package.size())
                                     .build(*engine_);
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

void FilamentRenderer::onTexture(const TextureUpdate& update) {
    // An atlas arrives whole the first time and in rects afterwards, as glyphs and sprites are
    // added to it. Both are the same call: a whole upload is the rect covering everything, and
    // the producer sends no rects for it.
    if (update.width == 0 || update.height == 0 || update.pixels.data == nullptr) {
        return;
    }
    filament::Texture::InternalFormat internal{};
    filament::Texture::Format format{};
    switch (update.format) {
        case TSL_TEXTURE_PIXEL_TYPE_RGBA:
            internal = filament::Texture::InternalFormat::RGBA8;
            format = filament::Texture::Format::RGBA;
            break;
        case TSL_TEXTURE_PIXEL_TYPE_ALPHA:
        case TSL_TEXTURE_PIXEL_TYPE_LUMINANCE:
            // A glyph atlas is one channel: the SDF distance. Filament has no ALPHA8, so it goes
            // in R8 and the shader reads `.r` -- which is what mbgl's own GL backend does once
            // ALPHA textures stop existing in core profiles.
            internal = filament::Texture::InternalFormat::R8;
            format = filament::Texture::Format::R;
            break;
        default:
            // Depth and stencil are not something a layer samples, and guessing a format here
            // would upload whatever bytes happened to arrive as colour.
            textureSkipped_++;
            return;
    }

    auto found = textures_.find(update.id);
    // An atlas is announced before it has anything in it -- a 1x1 placeholder, so that a drawable
    // naming the slot samples something defined rather than whatever was last bound -- and the
    // real pixels arrive later at the real size. A Filament texture cannot be resized, so the
    // placeholder is replaced rather than written into. Both of those uploads are whole-texture,
    // so nothing is lost; a *rect* update that disagreed with the held size would be, and there is
    // no sensible way to honour one, since the bytes for the rest of the atlas never arrive twice.
    //
    // A resize is honoured whether or not it carries rects, and the rects are then ignored. The
    // payload of every texture update is the *whole* image -- the rects say which parts of it
    // changed, not which parts were sent -- so a resize loses nothing by uploading all of it,
    // and every texel is new anyway.
    //
    // This used to refuse a resize that carried rects, on the reasoning that the bytes for the
    // rest of the atlas never arrive twice. They arrive every time. What the refusal actually
    // did was keep the *old* atlas, at the old size, while the producer went on addressing the
    // new one -- and the glyph atlas always carries a rect, so this fired every time a fetch
    // found a new script and grew it. Every label in the pane then drew as fragments.
    bool resized = false;
    if (found != textures_.end() && (found->second->getWidth() != update.width ||
                                     found->second->getHeight() != update.height)) {
        // Except when the payload cannot cover the new size, which is the one case where bytes
        // really would be missing. Keeping what is there beats replacing it with a hole.
        const std::size_t needed = static_cast<std::size_t>(update.width) * update.height *
                                   update.pixelSize();
        if (update.pixelSize() == 0 || needed > update.pixels.size) {
            textureSkipped_++;
            return;
        }
        // Every cached material instance goes with it. An instance is kept per (layer, shader,
        // ubo slot) across frames and a sampler set on it in an earlier frame is never cleared,
        // so an instance that named this texture goes on naming it after the destroy. It is only
        // reissued -- and so rebound -- if the same key comes round again; a full re-announce
        // reshuffles which drawable lands on which key, and the first instance reused for a
        // drawable that binds no texture draws with a freed handle. That is
        // "Handle (Texture) is being used after it has been freed", and it is what stopped
        // `Map::set_fonts` re-announcing against a grown atlas.
        //
        // Safe here and nowhere later: `beginFrame` has already destroyed every renderable of the
        // previous frame, and batches are only issued in `endFrame`, so at this point no instance
        // is attached to anything. They are a cache; `issue` rebuilds and rebinds what it needs.
        dropInstances();
        engine_->destroy(found->second);
        textures_.erase(found);
        found = textures_.end();
        resized = true;
    }
    if (found == textures_.end()) {
        auto* built = filament::Texture::Builder()
                          .width(update.width)
                          .height(update.height)
                          .levels(1)
                          .sampler(filament::Texture::Sampler::SAMPLER_2D)
                          .format(internal)
                          .build(*engine_);
        if (built == nullptr) {
            textureSkipped_++;
            return;
        }
        found = textures_.emplace(update.id, built).first;
    }

    const std::uint32_t pixel = update.pixelSize();
    if (pixel == 0) {
        textureSkipped_++;
        return;
    }

    // Copied onto the heap and freed by the descriptor's callback, for the reason the vertex
    // buffers are: the driver reads these after this call returns.
    const auto upload = [&](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                            const std::uint8_t* from, std::size_t bytes) {
        auto* owned = static_cast<std::uint8_t*>(std::malloc(bytes));
        if (owned == nullptr) {
            return;
        }
        std::memcpy(owned, from, bytes);
        found->second->setImage(
            *engine_, 0, x, y, w, h,
            filament::Texture::PixelBufferDescriptor(
                owned, bytes, format, filament::Texture::Type::UBYTE,
                [](void* buffer, std::size_t, void*) { std::free(buffer); }));
        textureUploads_++;
    };

    if (update.rects.empty() || resized) {
        const std::size_t bytes =
            static_cast<std::size_t>(update.width) * update.height * pixel;
        if (bytes <= update.pixels.size) {
            upload(0, 0, update.width, update.height, update.pixels.data, bytes);
        }
        return;
    }
    // The pixels are the *whole* texture and a rect names which part of it changed -- not a
    // tightly packed run of just that region. Reading them as packed uploads the atlas's top rows
    // into wherever the rect happens to point, which put every glyph at the wrong address: the
    // shader then sampled the right coordinates and found nothing there, and about one label in
    // nine survived by landing on a glyph anyway.
    const std::size_t rowBytes = static_cast<std::size_t>(update.width) * pixel;
    if (rowBytes * update.height > update.pixels.size) {
        textureSkipped_++;
        return;
    }
    std::vector<std::uint8_t> region;
    for (const tsl_rect& rect : update.rects) {
        if (rect.w == 0 || rect.h == 0 || rect.x + rect.w > update.width ||
            rect.y + rect.h > update.height) {
            continue;
        }
        // Cut the rectangle out at the texture's own row stride.
        const std::size_t bytes = static_cast<std::size_t>(rect.w) * rect.h * pixel;
        region.resize(bytes);
        for (std::uint32_t row = 0; row < rect.h; row++) {
            std::memcpy(region.data() + static_cast<std::size_t>(row) * rect.w * pixel,
                        update.pixels.data + (static_cast<std::size_t>(rect.y) + row) * rowBytes +
                            static_cast<std::size_t>(rect.x) * pixel,
                        static_cast<std::size_t>(rect.w) * pixel);
        }
        upload(rect.x, rect.y, rect.w, rect.h, region.data(), bytes);
    }
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

/// One subdivided unit quad, for a mask bent onto a sphere.
///
/// `cells` a side, in the tile's own 0..8192 units, wound as `mask.mat`'s four corners are. Made
/// once per count and kept: the counts a cover asks for are a handful, and a grid rebuilt per
/// frame would be the largest allocation the mask pass makes.
FilamentRenderer::MaskGrid FilamentRenderer::maskGrid(const std::uint32_t cells) {
    if (const auto found = maskGrids_.find(cells); found != maskGrids_.end()) {
        return found->second;
    }
    const std::uint32_t side = cells + 1;
    std::vector<std::int16_t> points;
    points.reserve(static_cast<std::size_t>(side) * side * 2);
    for (std::uint32_t row = 0; row < side; ++row) {
        for (std::uint32_t column = 0; column < side; ++column) {
            // From the exact fraction, so the last row and column land on the tile's edge rather
            // than short of it. A gap there is a seam the neighbouring mask does not cover.
            const auto at = [&](std::uint32_t n) {
                return static_cast<std::int16_t>((static_cast<std::int64_t>(n) * 8192) / cells);
            };
            points.push_back(at(column));
            points.push_back(at(row));
        }
    }
    std::vector<std::uint16_t> indices;
    indices.reserve(static_cast<std::size_t>(cells) * cells * 6);
    for (std::uint32_t row = 0; row < cells; ++row) {
        for (std::uint32_t column = 0; column < cells; ++column) {
            const auto corner = static_cast<std::uint16_t>(row * side + column);
            const auto below = static_cast<std::uint16_t>(corner + side);
            for (const std::uint16_t index :
                 {corner, static_cast<std::uint16_t>(corner + 1), below,
                  static_cast<std::uint16_t>(corner + 1), below,
                  static_cast<std::uint16_t>(below + 1)}) {
                indices.push_back(index);
            }
        }
    }

    MaskGrid grid{};
    grid.index_count = static_cast<std::uint32_t>(indices.size());
    grid.vertices = filament::VertexBuffer::Builder()
                        .vertexCount(static_cast<std::uint32_t>(points.size() / 2))
                        .bufferCount(1)
                        .attribute(filament::VertexAttribute::POSITION, 0,
                                   filament::VertexBuffer::AttributeType::SHORT2, 0,
                                   sizeof(std::int16_t) * 2)
                        .build(*engine_);
    const std::size_t vertexBytes = points.size() * sizeof(std::int16_t);
    auto* vertexCopy = new std::int16_t[points.size()];
    std::memcpy(vertexCopy, points.data(), vertexBytes);
    grid.vertices->setBufferAt(
        *engine_, 0,
        filament::VertexBuffer::BufferDescriptor(
            vertexCopy, vertexBytes,
            [](void* buffer, std::size_t, void*) { delete[] static_cast<std::int16_t*>(buffer); }));

    grid.indices = filament::IndexBuffer::Builder()
                       .indexCount(grid.index_count)
                       .bufferType(filament::IndexBuffer::IndexType::USHORT)
                       .build(*engine_);
    const std::size_t indexBytes = indices.size() * sizeof(std::uint16_t);
    auto* indexCopy = new std::uint16_t[indices.size()];
    std::memcpy(indexCopy, indices.data(), indexBytes);
    grid.indices->setBuffer(
        *engine_, filament::IndexBuffer::BufferDescriptor(
                      indexCopy, indexBytes, [](void* buffer, std::size_t, void*) {
                          delete[] static_cast<std::uint16_t*>(buffer);
                      }));

    maskGrids_.emplace(cells, grid);
    return grid;
}

void FilamentRenderer::writeMasks() {
    references_.clear();
    if (maskMaterial_ == nullptr || masks_.empty()) {
        return;
    }
    // A globe masks with its own material over its own grid. Four corners bent onto a sphere is a
    // flat sheet through the inside of it, and a stencil cut from that clips the wrong region --
    // which is why this used to bail out here and leave a globe unmasked.
    //
    // Unmasked was worse than it sounded. MVT geometry runs past its tile's edge into the buffer
    // that hides seams, and with neither this nor the bounding-box scissor (meaningless on a
    // curved patch) nothing stopped the overhang painting into the neighbor: wedges of water
    // lying across the map. Not a bend artifact -- switching the stencil and scissor off on a
    // *flat* map draws the same wedges at the same camera, which is what identified it.
    const bool bent = projection_ == TSL_PROJECTION_MODE_GLOBE;
    if (bent && maskGlobeMaterial_ == nullptr) {
        return;
    }
    // One grid for every globe mask, at least as fine as the producer's finest -- forty-one cells
    // a side at z0, from `subdivide::step_for_level`. Finer is safe and coarser is not: the mask
    // has to follow the surface at least as closely as the fills it clips, or it cuts a sliver off
    // every tile edge. Not read from the tile's zoom, because duplicating that arithmetic here is
    // how the two drift; over-subdividing costs a stencil-only pass some triangles.
    constexpr std::uint32_t kGlobeMaskCells = 48;
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

        auto* instance = (bent ? maskGlobeMaterial_ : maskMaterial_)->createInstance();
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

        // The bend, from the same two matrices the geometry it clips is drawn through.
        const MaskGrid grid = bent ? maskGrid(kGlobeMaskCells) : MaskGrid{};
        if (bent) {
            instance->setParameter("matrix", matrix);
            instance->setParameter("globeMatrix", globeMatrix_);
        }

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .layerMask(0xFF, layer_)
            .culling(false)
            .priority(band)
            .material(0, instance)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      bent ? grid.vertices : maskVertices_, bent ? grid.indices : maskIndices_, 0,
                      bent ? grid.index_count : 6);
        utils::Entity entity = utils::EntityManager::get().create();
        // A mask that failed to build is an entity with no renderable on it: adding it to the
        // scene draws nothing and leaves a destroy to do at teardown. Returned instead, and the
        // tile simply goes unclipped -- visible, which is what §13.2 asks for, rather than a
        // parent painted over its children.
        if (builder.build(*engine_, entity) != filament::RenderableManager::Builder::Success) {
            utils::EntityManager::get().destroy(entity);
            continue;
        }
        auto& transforms = engine_->getTransformManager();
        // A bent mask places itself through the material, as every bent drawable does: its matrix
        // reaches normalized Mercator, which Filament's transform could not take it on from.
        transforms.setTransform(transforms.getInstance(entity),
                                bent ? filament::math::mat4f() : matrix);
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
    // The globe's tables, which the flat path never touches and which were leaking a material per
    // family and a buffer pair per grid for the life of the process.
    for (auto& [family, material] : globeMaterials_) {
        engine_->destroy(material);
    }
    globeMaterials_.clear();
    if (maskGlobeMaterial_ != nullptr) {
        engine_->destroy(maskGlobeMaterial_);
        maskGlobeMaterial_ = nullptr;
    }
    for (auto& [cells, grid] : maskGrids_) {
        engine_->destroy(grid.vertices);
        engine_->destroy(grid.indices);
    }
    maskGrids_.clear();
    // Before the engine, like everything else it made.
    for (auto& [id, texture] : textures_) {
        engine_->destroy(texture);
    }
    textures_.clear();
}

void FilamentRenderer::dropInstances() {
    for (auto& [key, instance] : instances_) {
        engine_->destroy(instance);
    }
    instances_.clear();
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

void FilamentRenderer::configureCamera(filament::Camera& camera, bool flipY) {
    // Identity, because `clip = viewProj * worldPosition` has to reduce to
    // `clip = matrix * position` -- the producer's matrix already reaches clip space.
    //
    // The Y sign is the target's, not the producer's. An offscreen target read back with
    // `readPixels` comes out bottom-up, and every probe here writes it top-down again, so the
    // two flips cancel and the camera carries one. A swapchain presented straight to a
    // compositor is read by nobody: its first row is the top of the image, and flipping here
    // puts the map on its head -- which is exactly what a platform view showed.
    const double y = flipY ? -1.0 : 1.0;
    camera.setCustomProjection(filament::math::mat4{filament::math::float4{1.0, 0.0, 0.0, 0.0},
                                                    filament::math::float4{0.0, y, 0.0, 0.0},
                                                    filament::math::float4{0.0, 0.0, 1.0, 0.0},
                                                    filament::math::float4{0.0, 0.0, 0.0, 1.0}},
                               -1.0, 1.0);
    camera.setModelMatrix(filament::math::mat4f());
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
    glyphsDrawn_ = 0;
    glyphsHidden_ = 0;
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

namespace {

/// The atlas a drawable samples at slot zero, or zero if it names none.
///
/// Slot zero is the image texture for every family that has one -- a glyph atlas for text, a
/// sprite atlas for a pattern -- and a drawable that samples nothing simply lists no reference.
std::uint64_t textureFor(const DrawableAdd& add, std::uint32_t want = 0) {
    for (const auto& ref : add.textureRefs) {
        if (ref.slot == want) {
            return ref.texture;
        }
    }
    return 0;
}

/// How the drawable asks for slot zero to be sampled.
///
/// Nearest for an icon drawn at its own size, which is what keeps its texels one to a pixel;
/// linear for everything else. The producer decides, because the test reads the style -- see
/// `SymbolLayout::icons_need_linear`.
std::uint32_t filterFor(const DrawableAdd& add, std::uint32_t want = 0) {
    for (const auto& ref : add.textureRefs) {
        if (ref.slot == want) {
            return ref.filter;
        }
    }
    return 0;
}

/// The two components of a packed instance, read at a byte offset the wire names.
struct Outline {
    float x = 0.0f;
    float y = 0.0f;
    bool discarded = false;
};

/// Unpacks one wall instance: the footprint point it stands on, and whether it closes a ring.
///
/// `decimals_ed.x` holds seven bits of fraction per axis above a low bit that marks a ring's
/// closing point, which has no edge leaving it and so raises no wall. The fraction is zero for
/// integer tile units but a simplification pass produces fractional positions, and dropping it
/// would part the walls from the roof they meet.
Outline unpackOutline(const std::uint8_t* pos, const std::uint8_t* dec) {
    std::int16_t ix = 0, iy = 0;
    std::uint16_t packed = 0;
    std::memcpy(&ix, pos, sizeof ix);
    std::memcpy(&iy, pos + sizeof ix, sizeof iy);
    std::memcpy(&packed, dec, sizeof packed);

    const std::uint16_t fraction = static_cast<std::uint16_t>(packed / 2);
    const float high = static_cast<float>(fraction / 256);
    const float low = static_cast<float>(fraction % 256);
    return Outline{static_cast<float>(ix) + high / 128.0f,
                   static_cast<float>(iy) + low / 128.0f,
                   (packed % 2) != 0};
}

} // namespace

bool FilamentRenderer::expandWalls(const DrawableAdd& add) {
    // Filament has no per-instance vertex attributes -- `instances()` plus `getInstanceIndex()`
    // is the whole of its instancing, and per-instance data would have to ride in a uniform array
    // whose size is capped well below what a building-dense tile needs. So the instances are
    // expanded once here, on upload, into the geometry mbgl's own non-instanced branch builds:
    // four vertices per wall rather than one instance. Same pixels, same arithmetic, and the cost
    // is paid per tile rather than per frame.
    const Attribute* positions = nullptr;
    const Attribute* decimals = nullptr;
    // The building's own base and height, per instance. Data-driven in every real style, so a
    // wall that took them from the paint block instead got the fallback -- zero -- and stood no
    // height at all. Absent for a constant-paint layer, where the paint block is the right
    // source and the fallback below is what reads it.
    const Attribute* base = nullptr;
    const Attribute* height = nullptr;
    for (const Attribute& attribute : add.instanceAttrs) {
        if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_OUTLINE_POS_ATTRIBUTE) {
            positions = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_DECIMALS_ED_ATTRIBUTE) {
            decimals = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_BASE_VERTEX_ATTRIBUTE) {
            base = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_HEIGHT_VERTEX_ATTRIBUTE) {
            height = &attribute;
        }
    }
    if (positions == nullptr || decimals == nullptr || add.attrs.empty()) {
        return false;
    }
    const std::size_t instanceCount = std::min(positions->count(), decimals->count());
    if (instanceCount < 2) {
        return false;
    }

    // The template is the unit quad the producer sends: x picks which end of the edge this vertex
    // sits at, y picks the base ring or the roof ring. Read from the wire rather than assumed, so
    // the winding stays the producer's.
    const Attribute& templateAttr = add.attrs.front();
    const std::size_t templateCount = add.vertexCount;
    const std::uint16_t* templateIndexes = reinterpret_cast<const std::uint16_t*>(add.indexes.data);
    const std::size_t templateIndexCount = add.indexes.size / sizeof(std::uint16_t);
    if (templateCount == 0 || templateIndexCount == 0) {
        return false;
    }

    // Position as x, y and the base/roof selector; the wall's facing beside it.
    std::vector<float> vertices;
    std::vector<float> normals;
    // Base and height, replicated onto each of the quad's four corners. A vertex attribute is
    // the only per-instance channel Filament has here, which is the same reason the instances
    // are expanded at all.
    std::vector<float> extents;
    std::vector<std::uint16_t> indexes;
    vertices.reserve(instanceCount * templateCount * 3);
    normals.reserve(instanceCount * templateCount * 2);
    extents.reserve(instanceCount * templateCount * 2);
    indexes.reserve(instanceCount * templateIndexCount);

    for (std::size_t i = 0; i + 1 < instanceCount; i++) {
        // Each attribute's own offset, not just its stride. The two share one buffer -- position
        // at 0, the packed decimals-and-flag at 4 -- so dropping the offset read the position's
        // bytes as the flag, and the closing point of a ring was marked by the parity of its x.
        // Half the rings therefore raised a wall from their last point to the *next ring's*
        // first, which is a quad six hundred tile units long. Invisible while every wall stood
        // zero metres tall, and the moment the walls got their height it was a cross-hatch of
        // lines over the whole tile.
        const Outline p1 = unpackOutline(
            positions->data.data + i * positions->desc.stride + positions->desc.offset,
            decimals->data.data + i * decimals->desc.stride + decimals->desc.offset);
        // A closing point raises no wall, which is what the flag is for.
        if (p1.discarded) {
            continue;
        }
        const Outline p2 = unpackOutline(
            positions->data.data + (i + 1) * positions->desc.stride + positions->desc.offset,
            decimals->data.data + (i + 1) * decimals->desc.stride + decimals->desc.offset);

        const float dx = p2.x - p1.x;
        const float dy = p2.y - p1.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        // A zero-length edge has no facing to normalize, and would put a NaN through the lighting.
        if (!(length > 0.0f)) {
            continue;
        }
        const float nx = -dy / length;
        const float ny = dx / length;

        // This instance's own base and height. The binder writes them as floats at its own
        // stride, one entry per outline point, which is the same indexing the position above
        // uses. Falls back to the paint block's values, which is what a constant-paint layer
        // wants and what the material reads when the pair is absent.
        const auto readFloat = [&](const Attribute* attribute, std::size_t index) {
            float value = 0.0f;
            // Absent, or short of this instance. The binder writes one entry per roof vertex and
            // the outline is walked per instance; a buffer that does not reach is answered NaN
            // rather than read past, which the material takes as "use the paint block". Reading
            // past it flung whole walls across the tile -- the picture is long thin quads
            // radiating from a few points, which is what a garbage height looks like.
            if (attribute == nullptr || index >= attribute->count()) {
                return std::numeric_limits<float>::quiet_NaN();
            }
            std::memcpy(&value,
                        attribute->data.data + index * attribute->desc.stride
                            + attribute->desc.offset,
                        sizeof value);
            return value;
        };
        const float instanceBase = readFloat(base, i);
        const float instanceHeight = readFloat(height, i);

        const auto corner = static_cast<std::uint16_t>(vertices.size() / 3);
        if (vertices.size() / 3 + templateCount > std::numeric_limits<std::uint16_t>::max()) {
            break;
        }
        for (std::size_t k = 0; k < templateCount; k++) {
            std::int16_t sx = 0, sy = 0;
            const std::uint8_t* at = templateAttr.data.data + k * templateAttr.desc.stride;
            std::memcpy(&sx, at, sizeof sx);
            std::memcpy(&sy, at + sizeof sx, sizeof sy);
            vertices.push_back(sx == 0 ? p1.x : p2.x);
            vertices.push_back(sx == 0 ? p1.y : p2.y);
            vertices.push_back(static_cast<float>(sy));
            normals.push_back(nx);
            normals.push_back(ny);
            extents.push_back(instanceBase);
            extents.push_back(instanceHeight);
        }
        for (std::size_t k = 0; k < templateIndexCount; k++) {
            indexes.push_back(static_cast<std::uint16_t>(corner + templateIndexes[k]));
        }
    }
    if (indexes.empty()) {
        return false;
    }

    auto* built = filament::VertexBuffer::Builder()
                      .vertexCount(static_cast<std::uint32_t>(vertices.size() / 3))
                      .bufferCount(3)
                      .attribute(filament::VertexAttribute::POSITION, 0,
                                 filament::VertexBuffer::AttributeType::FLOAT3, 0, 12)
                      .attribute(filament::VertexAttribute::CUSTOM0, 1,
                                 filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                      .attribute(filament::VertexAttribute::CUSTOM1, 2,
                                 filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                      .build(*engine_);
    if (built == nullptr) {
        return false;
    }
    const auto upload = [&](std::uint8_t slot, const std::vector<float>& from) {
        const std::size_t bytes = from.size() * sizeof(float);
        auto* owned = static_cast<std::uint8_t*>(std::malloc(bytes));
        if (owned == nullptr) {
            return;
        }
        std::memcpy(owned, from.data(), bytes);
        built->setBufferAt(*engine_, slot,
                           filament::VertexBuffer::BufferDescriptor(
                               owned, bytes,
                               [](void* buffer, std::size_t, void*) { std::free(buffer); }));
    };
    upload(0, vertices);
    upload(1, normals);
    upload(2, extents);

    const auto indexCount = static_cast<std::uint32_t>(indexes.size());
    auto* built_indexes = filament::IndexBuffer::Builder()
                              .indexCount(indexCount)
                              .bufferType(filament::IndexBuffer::IndexType::USHORT)
                              .build(*engine_);
    if (built_indexes == nullptr) {
        engine_->destroy(built);
        return false;
    }
    const std::size_t indexBytes = indexes.size() * sizeof(std::uint16_t);
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(indexBytes));
    if (ownedIndexes == nullptr) {
        engine_->destroy(built_indexes);
        engine_->destroy(built);
        return false;
    }
    std::memcpy(ownedIndexes, indexes.data(), indexBytes);
    built_indexes->setBuffer(*engine_,
                             filament::IndexBuffer::BufferDescriptor(
                                 ownedIndexes, indexBytes,
                                 [](void* buffer, std::size_t, void*) { std::free(buffer); }));

    onRetire(add.id);
    meshes_[add.id] = Mesh{built, built_indexes, indexCount, add.layerIndex,
                           add.tileID ? add.tileID->z : std::uint8_t{0},
                           add.tileID ? add.tileID->overscaled_z : std::uint8_t{0},
                           add.tileID ? *add.tileID : TileID{}};
    meshes_[add.id].clipped = add.enableStencil;
    meshes_[add.id].colour = add.enableColor;
    walls_ += indexCount / 3;
    return true;
}

/// Builds an extrusion roof, keying its attributes by id rather than by wire order.
///
/// The generic path assigns Filament's custom slots in the order the wire lists attributes, which
/// is fine where a family always sends the same ones. An extrusion does not: `base`, `height` and
/// `color` are data-driven, so they are present when the style computes them per feature and
/// absent when it does not, and a slot assigned by position would mean something different in the
/// two cases.
///
/// A missing one is synthesised as a constant so the material stays single. mbgl compiles a
/// permutation per case and branches on `HAS_UNIFORM_u_height`; a Filament material is one
/// compiled thing, so the cheaper trade is to hand it the value it would have read anyway. It
/// costs eight bytes a vertex, and only for the properties a style did *not* make data-driven.
bool FilamentRenderer::buildRoof(const DrawableAdd& add) {
    const Attribute* position = nullptr;
    const Attribute* decimals = nullptr;
    const Attribute* base = nullptr;
    const Attribute* height = nullptr;
    for (const Attribute& attribute : add.attrs) {
        switch (attribute.desc.attr_id) {
            case TSL_UBO_ID_FILL_EXTRUSION_POS_VERTEX_ATTRIBUTE: position = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_DECIMALS_ED_ATTRIBUTE: decimals = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_BASE_VERTEX_ATTRIBUTE: base = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_HEIGHT_VERTEX_ATTRIBUTE: height = &attribute; break;
            default: break;
        }
    }
    if (position == nullptr || decimals == nullptr) {
        return false;
    }
    const auto count = static_cast<std::uint32_t>(add.vertexCount);
    if (count == 0) {
        return false;
    }

    // A zoom-interpolated property arrives as the pair the zoom mixes between; one that only
    // varies by feature writes the first and leaves the factor at zero, so reading both is right
    // either way.
    const std::size_t fillBytes = static_cast<std::size_t>(count) * sizeof(float);
    std::vector<std::uint8_t> baseFill;
    std::vector<std::uint8_t> heightFill;
    const auto constantPair = [&](std::vector<std::uint8_t>& into, float value) {
        into.resize(fillBytes);
        auto* as_floats = reinterpret_cast<float*>(into.data());
        for (std::uint32_t i = 0; i < count; i++) {
            as_floats[i] = value;
        }
    };
    // The layer's own evaluated value, which is what the shader would have read as a uniform.
    float constantBase = 0.0f;
    float constantHeight = 0.0f;
    if (const auto layer = uniforms_.find(add.layerIndex); layer != uniforms_.end()) {
        if (const auto props = layer->second.find(kPropsSlot);
            props != layer->second.end() && props->second.size() >= sizeof(tsl_fill_extrusion_props_ubo)) {
            tsl_fill_extrusion_props_ubo paint{};
            std::memcpy(&paint, props->second.data(), sizeof paint);
            constantBase = paint.base;
            constantHeight = paint.height;
        }
    }
    if (base == nullptr) {
        constantPair(baseFill, constantBase);
    }
    if (height == nullptr) {
        constantPair(heightFill, constantHeight);
    }

    auto* vertices = filament::VertexBuffer::Builder()
                         .vertexCount(count)
                         .bufferCount(4)
                         .attribute(filament::VertexAttribute::POSITION, 0,
                                    filament::VertexBuffer::AttributeType::SHORT2,
                                    position->desc.offset, position->desc.stride)
                         .attribute(filament::VertexAttribute::CUSTOM0, 1,
                                    filament::VertexBuffer::AttributeType::USHORT2,
                                    decimals->desc.offset, decimals->desc.stride)
                         // One float each, not a pair. The binder interleaves the layer's
                         // data-driven properties into a single buffer -- base at offset 0 and
                         // height at offset 4 of an 8-byte stride for this style -- so reading
                         // either as a pair runs into the other property, and then into the next
                         // vertex. The wire says which: `FLOAT` is one component, and a
                         // zoom-interpolated property would arrive as `FLOAT2` with the pair mbgl
                         // mixes between.
                         .attribute(filament::VertexAttribute::CUSTOM1, 2,
                                    filament::VertexBuffer::AttributeType::FLOAT,
                                    base ? base->desc.offset : 0,
                                    base ? base->desc.stride : 4)
                         .attribute(filament::VertexAttribute::CUSTOM2, 3,
                                    filament::VertexBuffer::AttributeType::FLOAT,
                                    height ? height->desc.offset : 0,
                                    height ? height->desc.stride : 4)
                         .build(*engine_);
    if (vertices == nullptr) {
        return false;
    }
    const auto upload = [&](std::uint8_t slot, const std::uint8_t* from, std::size_t bytes) {
        auto* owned = static_cast<std::uint8_t*>(std::malloc(bytes));
        if (owned == nullptr) {
            return;
        }
        std::memcpy(owned, from, bytes);
        vertices->setBufferAt(*engine_, slot,
                              filament::VertexBuffer::BufferDescriptor(
                                  owned, bytes,
                                  [](void* buffer, std::size_t, void*) { std::free(buffer); }));
    };
    upload(0, position->data.data, position->data.size);
    upload(1, decimals->data.data, decimals->data.size);
    upload(2, base ? base->data.data : baseFill.data(),
           base ? base->data.size : baseFill.size());
    upload(3, height ? height->data.data : heightFill.data(),
           height ? height->data.size : heightFill.size());

    const auto indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    auto* indices = filament::IndexBuffer::Builder()
                        .indexCount(indexCount)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine_);
    if (indices == nullptr) {
        engine_->destroy(vertices);
        return false;
    }
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
    if (ownedIndexes == nullptr) {
        engine_->destroy(indices);
        engine_->destroy(vertices);
        return false;
    }
    std::memcpy(ownedIndexes, add.indexes.data, add.indexes.size);
    indices->setBuffer(*engine_,
                       filament::IndexBuffer::BufferDescriptor(
                           ownedIndexes, add.indexes.size,
                           [](void* buffer, std::size_t, void*) { std::free(buffer); }));

    onRetire(add.id);
    meshes_[add.id] = Mesh{vertices, indices, indexCount, add.layerIndex,
                           add.tileID ? add.tileID->z : std::uint8_t{0},
                           add.tileID ? add.tileID->overscaled_z : std::uint8_t{0},
                           add.tileID ? *add.tileID : TileID{}};
    meshes_[add.id].clipped = add.enableStencil;
    meshes_[add.id].colour = add.enableColor;
    return true;
}

/// Builds a symbol drawable, keyed by attribute id, with the fade folded into the float channel.
///
/// A symbol carries five attributes, which is one more custom slot than the generic path gets a
/// working binding for here: the fifth arrived as zero however the wire described it, and a fade
/// opacity of zero multiplies every glyph away. Rather than leave labels dependent on that, the
/// projected position and the fade travel together in one `FLOAT4` -- the position needs three
/// components and the fade one, and they are both per-vertex floats, so the pair costs nothing
/// over sending them apart.
bool FilamentRenderer::buildSymbol(const DrawableAdd& add) {
    const Attribute* posOffset = nullptr;
    const Attribute* data = nullptr;
    const Attribute* pixelOffset = nullptr;
    const Attribute* projected = nullptr;
    const Attribute* fade = nullptr;
    for (const Attribute& a : add.attrs) {
        switch (a.desc.attr_id) {
            case TSL_UBO_ID_SYMBOL_POS_OFFSET_VERTEX_ATTRIBUTE: posOffset = &a; break;
            case TSL_UBO_ID_SYMBOL_DATA_VERTEX_ATTRIBUTE: data = &a; break;
            case TSL_UBO_ID_SYMBOL_PIXEL_OFFSET_VERTEX_ATTRIBUTE: pixelOffset = &a; break;
            case TSL_UBO_ID_SYMBOL_PROJECTED_POS_VERTEX_ATTRIBUTE: projected = &a; break;
            case TSL_UBO_ID_SYMBOL_FADE_OPACITY_VERTEX_ATTRIBUTE: fade = &a; break;
            default: break;
        }
    }
    if (posOffset == nullptr || data == nullptr || pixelOffset == nullptr || projected == nullptr) {
        return false;
    }
    const auto count = static_cast<std::uint32_t>(add.vertexCount);
    if (count == 0) {
        return false;
    }

    // Position and fade, interleaved. A label the producer has not placed carries no fade, and
    // full opacity is the right reading of that: the alternative is a map that draws no labels.
    std::vector<float> placed(static_cast<std::size_t>(count) * 4, 0.0f);
    for (std::uint32_t i = 0; i < count; i++) {
        float xyz[3] = {0, 0, 0};
        std::memcpy(xyz, projected->data.data + i * projected->desc.stride + projected->desc.offset,
                    sizeof xyz);
        float packed = 255.0f;
        if (fade != nullptr) {
            std::memcpy(&packed, fade->data.data + i * fade->desc.stride + fade->desc.offset,
                        sizeof packed);
        }
        placed[i * 4 + 0] = xyz[0];
        placed[i * 4 + 1] = xyz[1];
        placed[i * 4 + 2] = xyz[2];
        placed[i * 4 + 3] = packed;
        // What placement decided, counted per quad. The producer shapes every label a tile
        // holds and hides the ones that lost their space, so the difference between these two
        // is "offered" against "drawn" -- the number to look at when a map draws fewer labels
        // than it should, and the one thing that says whether they were never made or made and
        // rejected. Four vertices to a quad; the counts are divided at the accessor.
        if (i % 4 == 0) {
            // The opacity rides in the high bits with the fade direction in the low one, so a
            // value of zero or one is hidden either way it is moving.
            if (packed <= 1.0f) {
                glyphsHidden_++;
            } else {
                glyphsDrawn_++;
            }
        }
    }

    // The consumer half of `tessella_orchestrate::watch`: what actually arrived, against what
    // the producer says it sent. TSF_WATCH_FADE prints the first quad's packed opacity for every
    // symbol geometry received, decoded the same way the producer packs it -- opacity in the high
    // seven bits, whether it was placed in the low one.
    {
        static const bool watching = std::getenv("TSF_WATCH_FADE") != nullptr;
        if (watching && placed.size() >= 4) {
            // The range across the whole buffer, not the first vertex: a buffer whose labels are
            // at different points of their fades is the expected picture, and one that is
            // uniformly a single value is a buffer nobody rewrote.
            std::uint8_t lo = 255;
            std::uint8_t hi = 0;
            int distinct = 0;
            bool seen[128] = {false};
            for (std::size_t i = 3; i < placed.size(); i += 4) {
                const auto bits = static_cast<std::uint8_t>(placed[i]);
                const std::uint8_t level = bits >> 1;
                lo = level < lo ? level : lo;
                hi = level > hi ? level : hi;
                if (level < 128 && !seen[level]) {
                    seen[level] = true;
                    distinct++;
                }
            }
            // The reference this attribute names, beside what was read through it. Compared
            // against the producer's `sent` line for the same geometry, a disagreement in the
            // reference and a disagreement in the contents are different defects.
            std::fprintf(stderr,
                         "recv layer=%u id=%llu slab=%u offset=%u length=%u first=%.3f "
                         "vertices=%u opacity_min=%.3f opacity_max=%.3f levels=%d\n",
                         static_cast<unsigned>(layer_),
                         static_cast<unsigned long long>(add.id),
                         fade != nullptr ? fade->desc.source.slab : 0u,
                         fade != nullptr ? fade->desc.source.offset : 0u,
                         fade != nullptr ? fade->desc.source.length : 0u,
                         static_cast<double>(static_cast<std::uint8_t>(placed[3]) >> 1) / 127.0,
                         count, static_cast<double>(lo) / 127.0,
                         static_cast<double>(hi) / 127.0, distinct);
        }
    }

    auto* vertices = filament::VertexBuffer::Builder()
                         .vertexCount(count)
                         .bufferCount(4)
                         .attribute(filament::VertexAttribute::POSITION, 0,
                                    filament::VertexBuffer::AttributeType::SHORT4,
                                    posOffset->desc.offset, posOffset->desc.stride)
                         .attribute(filament::VertexAttribute::CUSTOM0, 1,
                                    filament::VertexBuffer::AttributeType::USHORT4,
                                    data->desc.offset, data->desc.stride)
                         .attribute(filament::VertexAttribute::CUSTOM1, 2,
                                    filament::VertexBuffer::AttributeType::SHORT4,
                                    pixelOffset->desc.offset, pixelOffset->desc.stride)
                         .attribute(filament::VertexAttribute::CUSTOM2, 3,
                                    filament::VertexBuffer::AttributeType::FLOAT4, 0, 16)
                         .build(*engine_);
    if (vertices == nullptr) {
        return false;
    }
    const auto upload = [&](std::uint8_t slot, const void* from, std::size_t bytes) {
        auto* owned = static_cast<std::uint8_t*>(std::malloc(bytes));
        if (owned == nullptr) {
            return;
        }
        std::memcpy(owned, from, bytes);
        vertices->setBufferAt(*engine_, slot,
                              filament::VertexBuffer::BufferDescriptor(
                                  owned, bytes,
                                  [](void* buffer, std::size_t, void*) { std::free(buffer); }));
    };
    upload(0, posOffset->data.data, posOffset->data.size);
    upload(1, data->data.data, data->data.size);
    upload(2, pixelOffset->data.data, pixelOffset->data.size);
    upload(3, placed.data(), placed.size() * sizeof(float));

    const auto indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    auto* indices = filament::IndexBuffer::Builder()
                        .indexCount(indexCount)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine_);
    if (indices == nullptr) {
        engine_->destroy(vertices);
        return false;
    }
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
    if (ownedIndexes == nullptr) {
        engine_->destroy(indices);
        engine_->destroy(vertices);
        return false;
    }
    std::memcpy(ownedIndexes, add.indexes.data, add.indexes.size);
    indices->setBuffer(*engine_,
                       filament::IndexBuffer::BufferDescriptor(
                           ownedIndexes, add.indexes.size,
                           [](void* buffer, std::size_t, void*) { std::free(buffer); }));

    onRetire(add.id);
    meshes_[add.id] = Mesh{vertices, indices, indexCount, add.layerIndex,
                           add.tileID ? add.tileID->z : std::uint8_t{0},
                           add.tileID ? add.tileID->overscaled_z : std::uint8_t{0},
                           add.tileID ? *add.tileID : TileID{},
                           textureFor(add)};
    meshes_[add.id].filter = filterFor(add);
    meshes_[add.id].clipped = add.enableStencil;
    meshes_[add.id].colour = add.enableColor;
    return true;
}

void FilamentRenderer::onGeometry(const DrawableAdd& add) {
    if (add.vertexCount == 0 || add.indexes.empty()) {
        // Silent until now, and the one place a delivered record can vanish without a trace: a
        // geometry whose index reference resolved to nothing looks exactly like one that was
        // never sent.
        static const bool tracing = std::getenv("TSF_WATCH_FADE") != nullptr;
        if (tracing) {
            std::fprintf(stderr, "drop id=%llu shader=%d vertices=%u indexes=%zu\n",
                         static_cast<unsigned long long>(add.id), add.builtinShader,
                         static_cast<unsigned>(add.vertexCount), add.indexes.size);
        }
        return;
    }

    // The walls arrive as instances over the roof's outline rather than as their own vertices.
    if (add.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER) {
        expandWalls(add);
        return;
    }
    if (add.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER) {
        buildRoof(add);
        return;
    }
    if (add.builtinShader == TSL_BUILTIN_SYMBOL_SDFSHADER
        || add.builtinShader == TSL_BUILTIN_SYMBOL_ICON_SHADER) {
        // Both halves carry the same five attributes and want the same packing; what differs is
        // only which atlas they sample and how the fragment resolves it.
        buildSymbol(add);
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
                           add.tileID ? *add.tileID : TileID{},
                           textureFor(add),
                           textureFor(add, TSL_UBO_ID_RASTER_IMAGE1_TEXTURE)};
    meshes_[add.id].clipped = add.enableStencil;
    meshes_[add.id].colour = add.enableColor;
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

/// The camera this frame's batches draw under.
///
/// Recorded rather than acted on: it arrives before the batches precisely so that each of them can
/// be issued against the right material. A projection this build does not know is refused back to
/// Mercator and counted, which draws a flat map -- the honest failure, since the alternative is
/// reading a matrix for a space nobody here has agreed on.
void FilamentRenderer::onCamera(const tsl_camera_update& camera) {
    projection_ = camera.projection == TSL_PROJECTION_MODE_GLOBE ? TSL_PROJECTION_MODE_GLOBE
                                                                 : TSL_PROJECTION_MODE_MERCATOR;
    // Element by element, not a memcpy: `globe_matrix` is sixteen doubles on the wire -- f64
    // because the sphere-to-clip step is composed from a camera distance and two rotations -- and
    // `mat4f` is sixteen floats. A memcpy would read half the matrix as garbage and still compile.
    float* out = &globeMatrix_[0][0];
    for (std::size_t index = 0; index < 16; ++index) {
        out[index] = static_cast<float>(camera.globe_matrix[index]);
    }
}

void FilamentRenderer::onBatch(const Batch& batch) {
    pending_.push_back(batch);
}

void FilamentRenderer::endFrame(std::uint64_t) {
    // The clip masks first, so every drawable issued below has a reference to test against.
    writeMasks();
    // Reversed: see `pending_`. The producer's order is front-to-back and this pass blends.
    // Reversed in place: `pending_` is cleared below either way, and a `Batch` owns two vectors,
    // so copying the frame's batches into a second sequence to reverse them would allocate once
    // per drawable for nothing.
    std::reverse(pending_.begin(), pending_.end());

    // ... except inside a layer that resolves in depth, where the reversal is wrong.
    //
    // The reversal exists because a translucent pass with no depth buffer has to blend
    // bottom-up. An extrusion is not that layer: it *has* a depth buffer, and the producer
    // already orders its drawables the way mbgl does -- the roof before the walls it belongs
    // to. Reversing that draws the walls first, and where the two meet at exactly equal depth
    // the comparison cannot separate them, so painter order decides and the roof edge is drawn
    // by whichever came last.
    //
    // This is also how the depth pass was caught. It was arriving *after* the colour pass, so
    // nothing ever read what it wrote -- which is why dropping it rendered pixel-identically,
    // and why a read-only colour pass with the prepass and one with no depth buffer at all lost
    // the *same* 6,857 wall pixels. The prepass is skipped outright now, in `issue`.
    //
    // Reversed at layer granularity, so the extrusion layer as a whole still sits where painter
    // order puts it and only its interior is restored.
    for (auto run = pending_.begin(); run != pending_.end();) {
        if (!resolvesInDepth(run->builtinShader)) {
            ++run;
            continue;
        }
        const auto layer = run->layerIndex;
        auto end = run;
        while (end != pending_.end() && resolvesInDepth(end->builtinShader)
               && end->layerIndex == layer) {
            ++end;
        }
        std::reverse(run, end);
        run = end;
    }

    for (const auto& batch : pending_) {
        issue(batch);
    }
    pending_.clear();
}

void FilamentRenderer::issue(const Batch& batch) {
    // Read once. These are diagnostic escape hatches and this is the per-drawable path, so a
    // getenv per drawable per frame is a syscall-shaped cost on the hot loop for a value that
    // cannot change while the process runs.
    static const bool noStencil = std::getenv("TSF_NO_STENCIL") != nullptr;
    static const bool impossibleRef = std::getenv("TSF_IMPOSSIBLE_REF") != nullptr;
    static const bool noScissor = std::getenv("TSF_NO_SCISSOR") != nullptr;
    static const bool traceScissor = std::getenv("TSF_SCISSOR_TRACE") != nullptr;
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

    // Which surface this frame draws on decides which material, because the two take a drawable's
    // matrix to mean different things: clip space on a plane, normalized Mercator on a globe.
    const bool bent = projection_ == TSL_PROJECTION_MODE_GLOBE;
    auto& table = bent ? globeMaterials_ : materials_;
    const auto material = table.find(batch.builtinShader);
    if (material == table.end()) {
        missing_++;
        // A family with no globe package is a different fault from one with no material at all,
        // and drawing it flat would be worse than not drawing it: a flat layer sitting across a
        // bent one looks like a geometry bug rather than a missing file.
        auto& named = bent ? missingGlobeFamilies_ : missingFamilies_;
        if (std::find(named.begin(), named.end(), batch.builtinShader) == named.end()) {
            named.push_back(batch.builtinShader);
        }
        return;
    }

    const auto layer = uniforms_.find(static_cast<std::int32_t>(batch.layerIndex));

    // One renderable per drawable, because a renderable carries one transform and each drawable
    // carries its own matrix -- different tiles do not share one. That gives up the multi-primitive
    // batching `DrawList` groups for; recovering it means splitting a batch by matrix, which is
    // worth doing once there is a picture to measure it against.
    // Bands four to seven: zero to three belong to the mask pass, which must have written every
    // clip before any geometry tests against it.
    //
    // Then by render pass, which is the coarse half of painter order: mbgl draws the opaque pass
    // and then the translucent one, and a layer whose mask names both -- a background does --
    // appears in each. This read `layerIndex / 32`, which put every layer of any style under
    // thirty-two layers in one band and left Filament to sort them as it saw fit. Two things
    // came of that: roads painted over the labels naming them, and the background's opaque-pass
    // drawable, which the producer emits after the rest, landed wherever it landed.
    const auto band = static_cast<std::uint8_t>(
        batch.pass == static_cast<std::uint8_t>(TSL_RENDER_PASS_OPAQUE) ? 4 : 5);

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

        // A bent drawable takes both halves of the bend. `transform` is the tile-local to
        // normalized Mercator placement the producer sent -- the same sixteen floats a Mercator
        // drawable would have used as its tile-to-clip matrix -- and `globeMatrix` is the frame's
        // sphere-to-clip. Every material in `globeMaterials_` declares both, which is why this is
        // unconditional here rather than per family.
        if (bent) {
            instance->setParameter("matrix", transform);
            instance->setParameter("globeMatrix", globeMatrix_);
        }

        if (const auto props = layer->second.find(kPropsSlot);
            props != layer->second.end() &&
            props->second.size() >= colourOffset(batch.builtinShader) + sizeof(float) * 4) {
            // A symbol has no single colour: it carries a fill and a halo, and which of them
            // applies is a property of the pass rather than of the layer. Its own block below
            // sets both, so the shared path would only be setting a uniform it does not declare.
            const bool patterned =
                batch.builtinShader == TSL_BUILTIN_FILL_PATTERN_SHADER
                || batch.builtinShader == TSL_BUILTIN_FILL_OUTLINE_PATTERN_SHADER;
            const bool sharedColour = batch.builtinShader != TSL_BUILTIN_SYMBOL_SDFSHADER
                                      && batch.builtinShader != TSL_BUILTIN_SYMBOL_ICON_SHADER
                                      && batch.builtinShader != TSL_BUILTIN_RASTER_SHADER
                                      && !patterned;
            if (sharedColour) {
                float colour[4] = {0, 0, 0, 0};
                std::memcpy(colour, props->second.data() + colourOffset(batch.builtinShader),
                            sizeof colour);
                coloured_++;
                instance->setParameter(
                    "color", filament::math::float4{colour[0], colour[1], colour[2], colour[3]});
            }

            // Opacity is not a property of having a shared colour, and nesting it inside that
            // test cost every family that sets its own colour its opacity: a raster tile at
            // `raster-opacity` 0.55 was composited at one, which is opaque imagery over the
            // vector layers it should be showing through to. Every material declares the
            // parameter, so this runs for all of them; a family whose block has no opacity field
            // answers `bytes` above and keeps the one.
            {
                float opacity = 1.0f;
                const std::size_t off = opacityOffset(batch.builtinShader, props->second.size());
                if (off + sizeof(float) <= props->second.size()) {
                    std::memcpy(&opacity, props->second.data() + off, sizeof opacity);
                }
                instance->setParameter("opacity", opacity);
            }

            // A circle takes its size and its stroke from the layer's paint and its extrude
            // scale from the drawable. `color` and `opacity` are already set above, which is why
            // only the rest is read here.
            if (batch.builtinShader == TSL_BUILTIN_CIRCLE_SHADER) {
                tsl_circle_evaluated_props_ubo paint{};
                paint.radius = 5.0f;
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                instance->setParameter("radius", paint.radius);
                instance->setParameter("blur", paint.blur);
                instance->setParameter("strokeWidth", paint.stroke_width);
                instance->setParameter("strokeOpacity", paint.stroke_opacity);
                instance->setParameter("strokeColor",
                                       filament::math::float4{paint.stroke_color[0],
                                                              paint.stroke_color[1],
                                                              paint.stroke_color[2],
                                                              paint.stroke_color[3]});
                instance->setParameter("scaleWithMap", paint.scale_with_map ? 1.0f : 0.0f);
                instance->setParameter("pitchWithMap", paint.pitch_with_map ? 1.0f : 0.0f);

                tsl_circle_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                filament::math::mat4f placement;
                std::memcpy(&placement, block.matrix, sizeof block.matrix);
                instance->setParameter("matrix", placement);
                instance->setParameter("extrudeScale",
                                       filament::math::float2{block.extrude_scale[0],
                                                              block.extrude_scale[1]});
                // The frame's own camera distance, which is what holds a circle at a constant
                // pixel size while the map is pitched. Read from the frame-wide block the same
                // way a symbol reads it.
                tsl_global_paint_params_ubo frame{};
                if (const auto global = uniforms_.find(-1); global != uniforms_.end()) {
                    if (const auto slot = global->second.find(TSL_UBO_ID_GLOBAL_PAINT_PARAMS_UBO);
                        slot != global->second.end() && slot->second.size() >= sizeof frame) {
                        std::memcpy(&frame, slot->second.data(), sizeof frame);
                    }
                }
                instance->setParameter("cameraToCenterDistance",
                                       frame.camera_to_center_distance);
                instance->setParameter("pixelRatio",
                                       frame.pixel_ratio > 0.0f ? frame.pixel_ratio : 1.0f);
            }

            // A line needs the widths from the layer's paint and the drawable's own ratio, which
            // is what keeps a road at a constant pixel width as the tile scales.
            if (batch.builtinShader == TSL_BUILTIN_LINE_SHADER) {
                // The whole block rather than one field: width, gap width, offset and blur all
                // feed the same edge arithmetic, and a shader given some of them from this frame
                // and the rest from a default draws a line of a width nothing asked for.
                tsl_line_evaluated_props_ubo paint{};
                paint.width = 1.0f;
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                instance->setParameter("width", paint.width);
                instance->setParameter("gapwidth", paint.gapwidth);
                instance->setParameter("offset", paint.offset);
                instance->setParameter("blur", paint.blur);

                float ratio = 1.0f;
                const std::size_t ratioAt = at + offsetof(tsl_line_drawable_ubo, ratio);
                if (ratioAt + sizeof(float) <= drawables->second.size()) {
                    std::memcpy(&ratio, drawables->second.data() + ratioAt, sizeof ratio);
                }
                instance->setParameter("ratio", ratio);
                // One device pixel per rendered pixel: the probe's swap chain is the view's own
                // size. A host on a HiDPI display passes its scale and the feather narrows to
                // match, which is the whole reason mbgl divides by this rather than fixing 1px.
                instance->setParameter("pixelRatio", 1.0f);
                // Clip space spans [-1, 1] across the viewport, so half the extent per unit. Only
                // read to measure how far perspective stretched an edge.
                instance->setParameter("unitsToPixels",
                                       filament::math::float2{static_cast<float>(width_) * 0.5f,
                                                              -static_cast<float>(height_) * 0.5f});
                instance->setParameter("matrix", transform);
            }

            // A patterned fill takes its sprite rectangles from the tile props, its world anchor
            // and scale from the drawable block, and the crossfade from the layer's paint.
            if (patterned) {
                tsl_fill_pattern_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                filament::math::mat4f placement;
                std::memcpy(&placement, block.matrix, sizeof block.matrix);
                instance->setParameter("matrix", placement);
                instance->setParameter("pixelCoordUpper",
                                       filament::math::float2{block.pixel_coord_upper[0],
                                                              block.pixel_coord_upper[1]});
                instance->setParameter("pixelCoordLower",
                                       filament::math::float2{block.pixel_coord_lower[0],
                                                              block.pixel_coord_lower[1]});
                instance->setParameter("tileRatio", block.tile_ratio);

                // Which sprite, and how big it is in the atlas. Per tile rather than per layer,
                // because the same layer resolves to different sprites in different tiles when
                // the pattern is data-driven.
                tsl_fill_pattern_tile_props_ubo tile{};
                if (const auto held = layer->second.find(kFillPatternTilePropsSlot);
                    held != layer->second.end()) {
                    const std::size_t tileAt =
                        static_cast<std::size_t>(batch.uboIndexes[i]) * sizeof tile;
                    if (tileAt + sizeof tile <= held->second.size()) {
                        std::memcpy(&tile, held->second.data() + tileAt, sizeof tile);
                    }
                }
                instance->setParameter("patternFrom",
                                       filament::math::float4{tile.pattern_from[0],
                                                              tile.pattern_from[1],
                                                              tile.pattern_from[2],
                                                              tile.pattern_from[3]});
                instance->setParameter("patternTo",
                                       filament::math::float4{tile.pattern_to[0],
                                                              tile.pattern_to[1],
                                                              tile.pattern_to[2],
                                                              tile.pattern_to[3]});
                // A zero atlas size would divide the sprite rectangles into infinity.
                instance->setParameter(
                    "texsize",
                    filament::math::float2{tile.texsize[0] > 0.0f ? tile.texsize[0] : 1.0f,
                                           tile.texsize[1] > 0.0f ? tile.texsize[1] : 1.0f});

                tsl_fill_evaluated_props_ubo paint{};
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                instance->setParameter("fromScale", paint.from_scale);
                instance->setParameter("toScale", paint.to_scale);
                instance->setParameter("fade", paint.fade);
                instance->setParameter("opacity", paint.opacity);

                tsl_global_paint_params_ubo frame{};
                if (const auto global = uniforms_.find(-1); global != uniforms_.end()) {
                    if (const auto slot = global->second.find(TSL_UBO_ID_GLOBAL_PAINT_PARAMS_UBO);
                        slot != global->second.end() && slot->second.size() >= sizeof frame) {
                        std::memcpy(&frame, slot->second.data(), sizeof frame);
                    }
                }
                instance->setParameter("pixelRatio",
                                       frame.pixel_ratio > 0.0f ? frame.pixel_ratio : 1.0f);

                const auto atlas = textures_.find(mesh->second.texture);
                if (atlas == textures_.end()) {
                    missingAtlas_++;
                    continue;
                }
                instance->setParameter("image0", atlas->second,
                                       filament::TextureSampler(
                                           filament::TextureSampler::MinFilter::LINEAR,
                                           filament::TextureSampler::MagFilter::LINEAR));
            }

            // A raster tile needs its own placement, the style's colour adjustments, and both
            // pictures: the tile's own and the parent it is fading from.
            if (batch.builtinShader == TSL_BUILTIN_RASTER_SHADER) {
                tsl_raster_evaluated_props_ubo paint{};
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                instance->setParameter(
                    "spinWeights",
                    filament::math::float4{paint.spin_weights[0], paint.spin_weights[1],
                                           paint.spin_weights[2], paint.spin_weights[3]});
                instance->setParameter(
                    "tlParent", filament::math::float2{paint.tl_parent[0], paint.tl_parent[1]});
                instance->setParameter("scaleParent", paint.scale_parent);
                // A zero buffer scale would divide the texture coordinates into infinity; the
                // producer sends one, and this is the guard rather than the assumption.
                instance->setParameter("bufferScale",
                                       paint.buffer_scale > 0.0f ? paint.buffer_scale : 1.0f);
                instance->setParameter("fadeT", paint.fade_t);
                instance->setParameter("opacity", paint.opacity);
                instance->setParameter("brightnessLow", paint.brightness_low);
                instance->setParameter("brightnessHigh", paint.brightness_high);
                instance->setParameter("saturationFactor", paint.saturation_factor);
                instance->setParameter("contrastFactor", paint.contrast_factor);

                tsl_raster_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                filament::math::mat4f placement;
                std::memcpy(&placement, block.matrix, sizeof block.matrix);
                instance->setParameter("matrix", placement);

                const auto first = textures_.find(mesh->second.texture);
                if (first == textures_.end()) {
                    missingAtlas_++;
                    continue;
                }
                // The second picture falls back to the first, which is what "no fade in
                // progress" means and what keeps the sampler from reading whatever was last
                // bound to it.
                const auto held = textures_.find(mesh->second.texture1);
                auto* second =
                    held == textures_.end() ? first->second : held->second;
                const filament::TextureSampler sampler(
                    filament::TextureSampler::MinFilter::LINEAR,
                    filament::TextureSampler::MagFilter::LINEAR);
                instance->setParameter("image0", first->second, sampler);
                instance->setParameter("image1", second, sampler);
            }

            // A symbol needs three matrices, the atlas it samples, the layer's text paint, and
            // the frame's camera distance -- the last because how far a label's anchor is from
            // the camera is what sets its size on screen.
            if (batch.builtinShader == TSL_BUILTIN_SYMBOL_SDFSHADER
                || batch.builtinShader == TSL_BUILTIN_SYMBOL_ICON_SHADER) {
                tsl_symbol_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                const auto asMatrix = [](const float (&from)[16]) {
                    filament::math::mat4f out;
                    std::memcpy(&out, from, sizeof from);
                    return out;
                };
                instance->setParameter("matrix", asMatrix(block.matrix));
                instance->setParameter("labelPlaneMatrix", asMatrix(block.label_plane_matrix));
                instance->setParameter("coordMatrix", asMatrix(block.coord_matrix));
                // The sheet this half samples, not the other's. The block carries both because
                // one shader can sample both atlases; a drawable that samples one still has to be
                // told which. Handing an icon the glyph atlas's dimensions scales every sprite
                // coordinate by the ratio between the two sheets, which lands the lookup in
                // whatever happens to be there -- transparent, most of the time, so a shield
                // drew nothing rather than drawing wrong.
                const bool isIcon = batch.builtinShader == TSL_BUILTIN_SYMBOL_ICON_SHADER;
                const float* sheet = isIcon ? block.texsize_icon : block.texsize;
                // The shader divides its atlas coordinates by this, so it has to be the size of
                // the texture actually bound. A drawable carrying one size against a texture of
                // another draws each glyph at the ratio between them -- a magnified corner of
                // itself, and its neighbour's corners around it.
                //
                // Taken from the texture rather than from the block. The two are the same thing
                // said twice, and the block's copy can be a frame behind: a fetch that finds a
                // new script hands the map a larger atlas, the upload carries the new size, and
                // a drawable whose uniforms were not re-sent still names the old one. Every
                // glyph then draws at the ratio between them -- a magnified corner of itself
                // with its neighbours' corners around it, which is what the CJK panes showed and
                // no flat, settled, Latin frame ever could.
                //
                // Counted as well as corrected: the staleness is a producer question, and a
                // consumer that quietly papers over it would make the question unaskable.
                float atlasWidth = sheet[0];
                float atlasHeight = sheet[1];
                if (const auto bound = textures_.find(mesh->second.texture);
                    bound != textures_.end()) {
                    const auto realWidth = static_cast<float>(bound->second->getWidth());
                    const auto realHeight = static_cast<float>(bound->second->getHeight());
                    // Only where the block made a claim. A drawable with no sheet of its own
                    // carries zeroes, which is an absence rather than a disagreement.
                    if (sheet[0] > 0.0f && sheet[1] > 0.0f &&
                        (realWidth != sheet[0] || realHeight != sheet[1])) {
                        atlasMismatched_++;
                    }
                    atlasWidth = realWidth;
                    atlasHeight = realHeight;
                }
                instance->setParameter(
                    "texsize",
                    filament::math::float2{atlasWidth > 0.0f ? atlasWidth : 1.0f,
                                           atlasHeight > 0.0f ? atlasHeight : 1.0f});
                instance->setParameter("isTextProp", block.is_text_prop ? 1.0f : 0.0f);
                instance->setParameter("rotateSymbol", block.rotate_symbol ? 1.0f : 0.0f);
                instance->setParameter("pitchWithMap", block.pitch_with_map ? 1.0f : 0.0f);
                instance->setParameter("isSizeZoomConstant",
                                       block.is_size_zoom_constant ? 1.0f : 0.0f);
                instance->setParameter("isSizeFeatureConstant",
                                       block.is_size_feature_constant ? 1.0f : 0.0f);
                instance->setParameter("isOffset", block.is_offset ? 1.0f : 0.0f);
                instance->setParameter("sizeT", block.size_t);
                instance->setParameter("size", block.size);

                // Text or icon decides which half of the paint block applies, and the tile props
                // say which of the two passes this drawable is.
                tsl_symbol_tile_props_ubo tile{};
                if (const auto props = layer->second.find(kSymbolTilePropsSlot);
                    props != layer->second.end()) {
                    const std::size_t tileAt =
                        static_cast<std::size_t>(batch.uboIndexes[i]) * sizeof tile;
                    if (tileAt + sizeof tile <= props->second.size()) {
                        std::memcpy(&tile, props->second.data() + tileAt, sizeof tile);
                    }
                }
                const bool sdf = batch.builtinShader == TSL_BUILTIN_SYMBOL_SDFSHADER;
                if (sdf) {
                    instance->setParameter("isHalo", tile.is_halo ? 1.0f : 0.0f);
                    instance->setParameter("tileGammaScale", tile.gamma_scale);
                }

                tsl_symbol_evaluated_props_ubo paint{};
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                const bool text = tile.is_text != 0;
                const float* fill = text ? paint.text_fill_color : paint.icon_fill_color;
                const float* halo = text ? paint.text_halo_color : paint.icon_halo_color;
                instance->setParameter(
                    "fillColor", filament::math::float4{fill[0], fill[1], fill[2], fill[3]});
                if (sdf) {
                    instance->setParameter(
                        "haloColor", filament::math::float4{halo[0], halo[1], halo[2], halo[3]});
                }
                instance->setParameter("opacity",
                                       text ? paint.text_opacity : paint.icon_opacity);
                if (sdf) {
                    instance->setParameter("haloWidth",
                                           text ? paint.text_halo_width : paint.icon_halo_width);
                    instance->setParameter("haloBlur",
                                           text ? paint.text_halo_blur : paint.icon_halo_blur);
                }

                tsl_global_paint_params_ubo frame{};
                if (const auto global = uniforms_.find(-1); global != uniforms_.end()) {
                    if (const auto slot = global->second.find(TSL_UBO_ID_GLOBAL_PAINT_PARAMS_UBO);
                        slot != global->second.end() && slot->second.size() >= sizeof frame) {
                        std::memcpy(&frame, slot->second.data(), sizeof frame);
                    }
                }
                instance->setParameter("cameraToCenterDistance",
                                       frame.camera_to_center_distance);
                instance->setParameter("aspectRatio", frame.aspect_ratio);
                instance->setParameter("symbolFadeChange", frame.symbol_fade_change);
                instance->setParameter("pixelRatio",
                                       frame.pixel_ratio > 0.0f ? frame.pixel_ratio : 1.0f);

                // A label pitched with the map lays out in the tile's own plane rather than the
                // viewport's: its plane matrix is the identity and its coord matrix carries the
                // tile's projection, so the offsets added between them are in tile units and not
                // pixels. That is a second arrangement of the same three matrices, and this
                // shader implements the viewport one; drawing an on-map label through it puts the
                // offsets in the wrong space. Counted and skipped until it is written.
                // Drawn, not skipped -- and the paragraph above was right the first time. A
                // line label under a pitched camera draws an opaque slab the size of its own
                // extent with the text over it: the quad is expanded in the wrong space, so it
                // reaches far enough past the glyph to sample the atlas either side of it.
                // Reproduced at Seattle z15, pitch 15, with the road layer alone -- point labels
                // in the same frame are clean, and `pitchedLabels_` is zero for them.
                //
                // Left drawing rather than skipped, because a pane with no road labels at all is
                // not obviously better than one with slabs, and because the count is what makes
                // the case: the arrangement wants writing, not a branch here.
                if (block.pitch_with_map) {
                    pitchedLabels_++;
                }

                // Without the atlas there is nothing to read a distance out of, so the batch is
                // skipped rather than drawn sampling whatever is bound.
                const auto atlas = textures_.find(mesh->second.texture);
                if (atlas == textures_.end()) {
                    missingAtlas_++;
                    continue;
                }
                // The producer chose it: an icon drawn at its own size is sampled nearest so
                // its texels land one to a pixel, and everything else linearly.
                const bool nearest = mesh->second.filter == 1;
                instance->setParameter("atlas", atlas->second,
                                       filament::TextureSampler(
                                           nearest ? filament::TextureSampler::MinFilter::NEAREST
                                                   : filament::TextureSampler::MinFilter::LINEAR,
                                           nearest ? filament::TextureSampler::MagFilter::NEAREST
                                                   : filament::TextureSampler::MagFilter::LINEAR));
            }

            // An extrusion needs its base and height, its light, and the height factor that turns
            // metres into the tile's own units -- the last from the drawable block, the rest from
            // the layer's paint.
            if (batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER ||
                batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER) {
                tsl_fill_extrusion_props_ubo paint{};
                if (props->second.size() >= sizeof paint) {
                    std::memcpy(&paint, props->second.data(), sizeof paint);
                }
                if (batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER) {
                    // Nothing: the roof reads base and height per vertex. The zoom-mix factors in
                    // the drawable block are for a height that interpolates across zooms, which
                    // arrives as a `FLOAT2` pair; this reads the single-component form and would
                    // need both to serve that case.
                } else {
                    // The walls still take theirs as uniforms: `encode_extrusion_walls` does not
                    // put the data-driven attributes on the wire, so there is nothing per instance
                    // to read and this is the layer's evaluated value or nothing.
                    instance->setParameter("base", paint.base);
                    instance->setParameter("height", paint.height);
                }
                instance->setParameter("lightIntensity", paint.light_intensity);
                instance->setParameter("verticalGradient", paint.vertical_gradient);
                instance->setParameter(
                    "lightColor", filament::math::float3{paint.light_color[0],
                                                         paint.light_color[1],
                                                         paint.light_color[2]});
                // The direction the light arrives from. Without it the directional term reduces
                // to its unlit floor and every roof takes the shading meant for a surface facing
                // away from the light.
                instance->setParameter(
                    "lightPosition", filament::math::float3{paint.light_position[0],
                                                           paint.light_position[1],
                                                           paint.light_position[2]});

                tsl_fill_extrusion_drawable_ubo block{};
                if (at + sizeof block <= drawables->second.size()) {
                    std::memcpy(&block, drawables->second.data() + at, sizeof block);
                }
                // The height factor is deliberately not passed: it belongs to the pattern
                // variants, which use it for texture coordinates rather than placement.
                instance->setParameter("matrix", transform);
            }
        }

        // The tile's own clip, as a stencil test. A parent's geometry passes only where the
        // parent's own mask survived -- that is, where no child overwrote it -- which is what
        // stops an ancestor compositing over the children that replaced it.
        //
        // Only where the producer asked for it. §11.7's clip obligation is per drawable and the
        // flag is how it is stated: a fill and a line carry it, a symbol and a circle do not,
        // because those are drawn from an anchor whose geometry legitimately overhangs the tile
        // that owns it. Clipping them anyway cut every label at every tile edge it crossed.
        // What the producer said this pass draws. An extrusion's depth pass clears `ENABLE_COLOR`
        // and exists only to fill the depth buffer; drawing it as though it wrote colour is a
        // building painted twice, once flat.
        instance->setColorWrite(mesh->second.colour);

        // And the depth buffer, which is what makes a building a volume rather than an outline.
        //
        // Read and written by both passes, where mbgl leaves its colour pass read-only.
        // `depth_probe` is why: two quads at the depths a z15 frame really produces, and the only
        // combination putting the near one in front in both draw orders is this projection with
        // Filament's default comparison and the write on. Reversing z -- the obvious reading,
        // since Filament is a reversed-Z renderer -- puts the *far* quad in front instead.
        // The depth-only pass, drawn as mbgl draws it.
        //
        // Depth alone cannot prevent a double blend: the test rejects a farther fragment that
        // arrives second, but nothing stops it arriving first. So the buffer is filled with the
        // whole layer before any colour is blended. On a stacked building an upper block's wall
        // projects over the lower block's roof, and without this both are blended, which reads as
        // a lighter rectangle let into the wall.
        //
        // The colour pass writes depth as well as reading it, where mbgl leaves it read-only.
        // Read-only measures worse here and is not taken on faithfulness alone.
        if (resolvesInDepth(batch.builtinShader)) {
            instance->setDepthCulling(true);
            instance->setDepthWrite(true);
        } else {
            // A flat layer is not occluded by a building.
            //
            // mbgl draws one under `depthModeForSublayer`, which is a depth *range*: the fragment's
            // depth is remapped into a narrow band a few `depthEpsilon` from the near plane, one
            // band per layer and sublayer. Two things follow. Flat layers resolve against each
            // other by the band they are in, and every one of them sits in front of anything
            // drawn through the whole range -- which is what a fill-extrusion uses.
            //
            // The band is reproduced here as a nudge to the projection's `[14]`, because a
            // consumer that binds a matrix has nowhere to put a depth range. A nudge translates
            // the depth; it does not compress it. So a flat layer kept the depth of the ground it
            // sits on, and once the extrusion started writing depth, a building's roof was nearer
            // than the circle beside its foot and the test threw the circle away. Half the POI
            // dots in the all-families scene went: 946 pixels of them against the oracle's 1,811,
            // and 1,794 with the buildings taken out of the style.
            //
            // Not tested at all rather than tested against a band this cannot express. Painter
            // order already puts these layers in the right sequence -- it is what the reversal in
            // `endFrame` is for -- and a flat layer in mbgl neither writes depth nor loses to
            // anything that does.
            instance->setDepthCulling(false);
        }

        // An extrusion is not clipped to its tile, in either pass.
        //
        // The producer marks the colour pass `ENABLE_STENCIL` and the depth pass not, which is
        // what mbgl does -- `setEnableStencil(doDepthPass)` on the colour builder, the depth
        // builder left at the default of false. There the asymmetry is harmless: mbgl's stencil
        // is what makes exactly one tile paint each pixel, and between them the tiles cover
        // everything.
        //
        // Here it was the anomaly. A building's geometry runs past its tile's edge by design, and
        // clipping the colour pass to the tile square slices the walls off there. Nothing paints
        // what is cut: the neighbouring tile does not carry its own copy of that building to
        // paint it with. So the clip removes wall faces and puts the background in their place.
        //
        // Measured against the oracle, clipping is what the visible error *is*. Unclipped the
        // frame has 8 gross pixels; clipped, 161 -- and in the worst region, 1 against 64. The
        // cost of dropping it is that two tiles' copies of an overlapping building both blend,
        // which is 2.1% of pixels differing by a shade nobody sees, against wall faces that are
        // simply missing.
        //
        // Both passes, not just one. A clip on the depth pass and not the colour pass is worse
        // than either: the depth pass writes for the whole building and the colour pass cannot
        // paint the part outside the tile, which leaves depth with no colour -- a hole rather
        // than a slice. That asymmetry is why the prepass looked broken when it was first drawn.
        const bool clipped = mesh->second.clipped && !resolvesInDepth(batch.builtinShader);
        const std::uint8_t reference = clipped ? referenceFor(mesh->second.tile) : 0;
        if (clipped && reference == 0) {
            unmasked_++;
        }
        if (reference != 0 && !noStencil) {
            instance->setStencilWrite(false);
            instance->setStencilReferenceValue(impossibleRef ? 200 : reference);
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
        //
        // A globe is not scissored. This box comes from pushing the tile's corners through the
        // drawable's matrix, which is exact only while that matrix reaches clip space and the tile
        // is a flat rectangle in it. Under a globe it reaches normalized Mercator and the tile is
        // a curved patch, so four corners neither land in screen space nor bound the bulge between
        // them. The comment above already says this device is the coarse one and the stencil is
        // the exact one; a globe simply has neither until the mask is bent.
        if (!bent) {
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            const float corners[4][2] = {{0, 0}, {8192, 0}, {8192, 8192}, {0, 8192}};
            // A corner behind the camera has a negative `w`, and dividing by it mirrors that
            // corner through the origin: the box then bounds somewhere the tile is not, and the
            // tile is scissored to a strip of it. A pitched camera puts corners behind itself
            // routinely -- the nearer half of a tile the camera is standing on -- so this is not
            // a degenerate case to guard against but the ordinary one at any pitch and zoom.
            //
            // There is no screen-space box for such a tile: it reaches to the horizon. So it is
            // not scissored at all, which is what the mask pass is for anyway.
            bool boundable = true;
            for (const auto& corner : corners) {
                const filament::math::float4 clip =
                    transform * filament::math::float4{corner[0], corner[1], 0.0f, 1.0f};
                if (clip.w <= 0.0f) {
                    boundable = false;
                    break;
                }
                minX = std::min(minX, clip.x / clip.w);
                maxX = std::max(maxX, clip.x / clip.w);
                // The same Y sign `configureCamera` was given, because this box is measured in
                // the producer's clip space and names a region of the screen: it has to be
                // carried across exactly as the geometry was. Carried the other way it is the
                // mirror of what it should bound, and a tile is clipped to where its own
                // reflection overlaps it -- a band across the middle of the map, which is what
                // a platform view showed when its camera stopped flipping and this did not.
                const float screenY = (flipY_ ? -clip.y : clip.y) / clip.w;
                minY = std::min(minY, screenY);
                maxY = std::max(maxY, screenY);
            }
            const auto toPixels = [](float ndc, std::uint32_t extent) {
                return (ndc * 0.5f + 0.5f) * static_cast<float>(extent);
            };
            const float l = std::max(0.0f, std::floor(toPixels(minX, width_)));
            const float b = std::max(0.0f, std::floor(toPixels(minY, height_)));
            const float r = std::min(static_cast<float>(width_), std::ceil(toPixels(maxX, width_)));
            const float t =
                std::min(static_cast<float>(height_), std::ceil(toPixels(maxY, height_)));
            if (clipped && traceScissor) {
                std::fprintf(stderr,
                             "scissor tile=%u/%u/%u boundable=%d box=%.0f,%.0f,%.0f,%.0f "
                             "view=%ux%u\n",
                             static_cast<unsigned>(mesh->second.tile.z),
                             static_cast<unsigned>(mesh->second.tile.x),
                             static_cast<unsigned>(mesh->second.tile.y), boundable ? 1 : 0, l, b,
                             r, t, width_, height_);
            }
            if (clipped && boundable && r > l && t > b && !noScissor) {
                instance->setScissor(
                    static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(b),
                    static_cast<std::uint32_t>(r - l), static_cast<std::uint32_t>(t - b));
                scissored_++;
            } else {
                // Explicitly, not by omission. An instance is cached per (layer, shader, tile)
                // and a scissor is state on it, so a frame that skips the call inherits the box
                // the previous frame set. A tile that reaches past the frustum has no box, and
                // keeping the one it had when it did is what emptied the lower two thirds of a
                // pitched pane mid-zoom.
                instance->unsetScissor();
            }
        } else {
            // Explicitly here too, and for the same reason one zoom further up: the projection is
            // a runtime toggle, so an instance cached while the map was flat still carries the box
            // that frame set. Switching to a globe would then draw the planet through a rectangle
            // cut for a tile on a plane.
            instance->unsetScissor();
        }

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .layerMask(0xFF, layer_)
            .culling(false)
            .priority(band)
            // Painter order within the pass, enforced rather than hoped for.
            //
            // `priority` is three bits and is spent on the pass above, so it cannot also carry
            // the order of a style's layers -- and left to itself Filament sorts blended
            // primitives within a band as it sees fit, which is how roads came to paint over
            // the labels naming them. `blendOrder` is fifteen bits and, made global, orders
            // blended primitives across the whole scene, with priority still taking precedence.
            // So: pass in the band, layer order here.
            //
            // `ordered_` counts this frame's renderables in the order the producer sent them,
            // which *is* painter order -- `DrawList` never reorders, by construction. Lower is
            // drawn first, so the count goes in as it stands.
            //
            // Counting it down was tried first, on a reading that the key sorts like a distance
            // and a larger value means farther and so earlier. It does not: what looked like an
            // inverted frame was the background's opaque-pass drawable, which the producer emits
            // after every translucent one and which the old band left free to land anywhere.
            //
            // Clamped rather than wrapped. Past 32,767 drawables in one frame the tail all sorts
            // together, which loses order within the tail; wrapping would move it to the other
            // end and paint the frame inside out.
            .blendOrder(0, static_cast<std::uint16_t>(
                               std::min<std::uint64_t>(ordered_, 0x7FFF)))
            .globalBlendOrderEnabled(0, true)
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
        // A bent drawable always places itself: its matrix reaches normalized Mercator, which is
        // not a space Filament's transform could take it the rest of the way from.
        const bool placesItself = bent || patternPlaces(batch.builtinShader) ||
                                  batch.builtinShader == TSL_BUILTIN_RASTER_SHADER ||
                                  batch.builtinShader == TSL_BUILTIN_SYMBOL_ICON_SHADER ||
                                  batch.builtinShader == TSL_BUILTIN_SYMBOL_SDFSHADER ||
                                  batch.builtinShader == TSL_BUILTIN_LINE_SHADER ||
                                  batch.builtinShader ==
                                      TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER ||
                                  batch.builtinShader == TSL_BUILTIN_FILL_EXTRUSION_SHADER;
        transforms.setTransform(transforms.getInstance(entity),
                                placesItself ? filament::math::mat4f() : transform);

        // The order actually issued, which is what settled this: the background's opaque-pass
        // drawable arriving *after* every translucent one is not a thing to deduce from a
        // picture. Alongside `TSF_NO_SCISSOR` and `TSF_NO_STENCIL` for the same reason.
        if (std::getenv("TSF_ORDER_LOG")) {
            std::fprintf(stderr,
                         "order %llu shader %d layer %u pass %u band %u geom %llu slot %u "
                         "colour %d idx %u tx %.4f ty %.4f tile %u/%u/%u\n",
                         (unsigned long long)ordered_, (int)batch.builtinShader,
                         (unsigned)batch.layerIndex, (unsigned)batch.pass, (unsigned)band,
                         (unsigned long long)batch.geometries[i], (unsigned)batch.uboIndexes[i],
                         (int)mesh->second.colour, (unsigned)mesh->second.indexCount,
                         (double)transform[3][0], (double)transform[3][1],
                         (unsigned)mesh->second.tile.z, (unsigned)mesh->second.tile.x,
                         (unsigned)mesh->second.tile.y);
        }
        scene_->addEntity(entity);
        entities_.push_back(entity);
        renderables_++;
        ordered_++;
        primitives_++;
    }
}

} // namespace tsf
