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

/// Where the anchored bend's coefficients arrive -- `globe_ubo::ID_GLOBE_BEND_UBO`.
///
/// Eleven, past everything mbgl binds. Not five: that is `kPropsSlot`, the layer's evaluated paint,
/// and every family keeps it there.
constexpr std::uint32_t kGlobeBendSlot = 11;

/// Six `vec4` -- `globe_ubo::GlobeBendUbo::STRIDE`.
constexpr std::size_t kGlobeBendStride = 96;

/// Half a tile's extent, which is the offset the producer expanded the bend about.
constexpr float kHalfExtent = 8192.0f / 2.0f;

/// The zoom at or above which a bent tile takes the anchored path.
///
/// The two forms overlap between z9 and z11 -- checked against the exact chain, the expansion is
/// 0.005 px out at z9 and 0.0003 at z11, and the direct bend does not reach a hundredth of a pixel
/// until z11 -- so anywhere in there is a seamless place to switch. Ten is the middle of it. Below,
/// a tile subtends too much sphere for a quadratic; above, the direct bend's `f32` trig is worth
/// more than a pixel.
constexpr std::uint8_t kAnchoredFromZoom = 10;

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

/// Which vertex slot a family's attribute lands in, keyed by its id.
///
/// # Wire order is not slot order
///
/// The generic path assigns `CUSTOM0`, `CUSTOM1`, ... in the order the wire lists attributes.
/// That is fine while every drawable of a family carries the same ones, and wrong the moment paint
/// becomes data-driven: a fill whose colour is the layer's but whose opacity is the feature's
/// sends one paint attribute, and in wire order it would land in `CUSTOM0` -- the slot the
/// material reads as *colour*. `getCustom0()` has to mean one property for the life of the
/// package, so the slot comes from the attribute's id.
///
/// `slot` is -1 for the position, which takes Filament's own `POSITION`.
struct PaintSlot {
    std::uint32_t attrId;
    int slot;
    /// Bit in the mesh's paint mask, and the index of the material constant it specializes.
    /// -1 for the position, which is not paint and has no permutation.
    int bit;
    /// Width the *shader* declares, in bytes, which is not always what the buffer supplies.
    ///
    /// A property that varies with zoom carries both endpoints and a colour is four floats; one
    /// that varies only per feature carries one endpoint and two. The shader reads the wide form
    /// either way and mixes by a factor of zero, which is mbgl's arrangement -- so the last
    /// vertex reads past what the producer sent unless the slab is padded to this.
    std::size_t declaredBytes;
    /// The type to declare when nothing supplies this slot, and the shared zero buffer does.
    filament::VertexBuffer::AttributeType declared;
};

/// The fill family's slots. `fill-outline-color` is the outline shader's `CUSTOM0` because the
/// outline is a different builtin with its own id space position, not a second colour on the fill.
constexpr PaintSlot kFillSlots[] = {
    {TSL_UBO_ID_FILL_POS_VERTEX_ATTRIBUTE, -1, -1, 0,
     filament::VertexBuffer::AttributeType::SHORT2},
    {TSL_UBO_ID_FILL_COLOR_VERTEX_ATTRIBUTE, 0, 0, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_FILL_OPACITY_VERTEX_ATTRIBUTE, 1, 1, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};
constexpr PaintSlot kFillOutlineSlots[] = {
    {TSL_UBO_ID_FILL_POS_VERTEX_ATTRIBUTE, -1, -1, 0,
     filament::VertexBuffer::AttributeType::SHORT2},
    {TSL_UBO_ID_FILL_OUTLINE_COLOR_VERTEX_ATTRIBUTE, 0, 0, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_FILL_OPACITY_VERTEX_ATTRIBUTE, 1, 1, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};

/// The line family's slots. `data` is not paint -- it is the extrusion normal and the segment's
/// geometry -- so it takes a custom slot with no permutation bit.
constexpr PaintSlot kLineSlots[] = {
    {TSL_UBO_ID_LINE_POS_NORMAL_VERTEX_ATTRIBUTE, -1, -1, 0,
     filament::VertexBuffer::AttributeType::SHORT2},
    {TSL_UBO_ID_LINE_DATA_VERTEX_ATTRIBUTE, 0, -1, sizeof(std::uint8_t) * 4,
     filament::VertexBuffer::AttributeType::UBYTE4},
    {TSL_UBO_ID_LINE_COLOR_VERTEX_ATTRIBUTE, 1, 0, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_LINE_BLUR_VERTEX_ATTRIBUTE, 2, 1, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_LINE_OPACITY_VERTEX_ATTRIBUTE, 3, 2, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_LINE_GAP_WIDTH_VERTEX_ATTRIBUTE, 4, 3, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_LINE_OFFSET_VERTEX_ATTRIBUTE, 5, 4, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_LINE_WIDTH_VERTEX_ATTRIBUTE, 6, 5, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};

/// The circle family's. Every attribute but the position is paint, which is what makes a circle
/// the widest permutation space in the style spec -- seven properties, and mbgl compiles a shader
/// for each of the hundred and twenty-eight combinations it meets.
constexpr PaintSlot kCircleSlots[] = {
    {TSL_UBO_ID_CIRCLE_POS_VERTEX_ATTRIBUTE, -1, -1, 0,
     filament::VertexBuffer::AttributeType::SHORT2},
    {TSL_UBO_ID_CIRCLE_COLOR_VERTEX_ATTRIBUTE, 0, 0, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_CIRCLE_RADIUS_VERTEX_ATTRIBUTE, 1, 1, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_CIRCLE_BLUR_VERTEX_ATTRIBUTE, 2, 2, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_CIRCLE_OPACITY_VERTEX_ATTRIBUTE, 3, 3, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_CIRCLE_STROKE_COLOR_VERTEX_ATTRIBUTE, 4, 4, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_CIRCLE_STROKE_WIDTH_VERTEX_ATTRIBUTE, 5, 5, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_CIRCLE_STROKE_OPACITY_VERTEX_ATTRIBUTE, 6, 6, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};

/// The symbol family's *paint* slots, which is only part of its table.
///
/// A symbol's four fixed channels are not here: `buildSymbol` packs the projected position and the
/// fade into one `FLOAT4` of its own making, so there is no attribute to name for it. What these
/// describe is the tail, custom3 upwards, which is the same shape every other family's paint has.
///
/// The icon half has one: mbgl's icon shader declares `opacity` and nothing else of the five.
constexpr PaintSlot kSymbolSdfPaint[] = {
    {TSL_UBO_ID_SYMBOL_COLOR_VERTEX_ATTRIBUTE, 3, 0, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_SYMBOL_HALO_COLOR_VERTEX_ATTRIBUTE, 4, 1, sizeof(float) * 4,
     filament::VertexBuffer::AttributeType::FLOAT4},
    {TSL_UBO_ID_SYMBOL_OPACITY_VERTEX_ATTRIBUTE, 5, 2, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_SYMBOL_HALO_WIDTH_VERTEX_ATTRIBUTE, 6, 3, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
    {TSL_UBO_ID_SYMBOL_HALO_BLUR_VERTEX_ATTRIBUTE, 7, 4, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};
constexpr PaintSlot kSymbolIconPaint[] = {
    {TSL_UBO_ID_SYMBOL_OPACITY_VERTEX_ATTRIBUTE, 3, 0, sizeof(float) * 2,
     filament::VertexBuffer::AttributeType::FLOAT2},
};

/// Whether to print what arrived beside what was taken. See the slotted path's use of it.
const bool tracingPaint = std::getenv("TSF_PAINT_TRACE") != nullptr;

std::pair<const PaintSlot*, std::size_t> symbolPaintSlots(std::int32_t shader) {
    switch (shader) {
        case TSL_BUILTIN_SYMBOL_SDFSHADER:
            return {kSymbolSdfPaint, std::size(kSymbolSdfPaint)};
        case TSL_BUILTIN_SYMBOL_ICON_SHADER:
            return {kSymbolIconPaint, std::size(kSymbolIconPaint)};
        default:
            return {nullptr, 0};
    }
}

/// The slots a family declares, or an empty span for one still on the wire-order path.
///
/// A family joins the permutation mechanism by gaining a table here and constants in its
/// material; until it has both, its drawables keep the generic path and its data-driven paint
/// still reads as the property's spec default.
std::pair<const PaintSlot*, std::size_t> paintSlots(std::int32_t shader) {
    switch (shader) {
        case TSL_BUILTIN_FILL_SHADER:
            return {kFillSlots, std::size(kFillSlots)};
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
            return {kFillOutlineSlots, std::size(kFillOutlineSlots)};
        case TSL_BUILTIN_LINE_SHADER:
            return {kLineSlots, std::size(kLineSlots)};
        case TSL_BUILTIN_CIRCLE_SHADER:
            return {kCircleSlots, std::size(kCircleSlots)};
        default:
            return {nullptr, 0};
    }
}

/// The material constants a family's paint mask specializes, in bit order.
constexpr const char* kFillConstants[] = {"colorFromAttribute", "opacityFromAttribute"};
constexpr const char* kLineConstants[] = {"colorFromAttribute", "blurFromAttribute",
                                          "opacityFromAttribute", "gapWidthFromAttribute",
                                          "offsetFromAttribute", "widthFromAttribute"};
/// The extrusion family binds `base` and `height` unconditionally -- they shape the geometry and
/// the builder synthesises a constant fill where the style did not drive them -- so the colour is
/// the only property with a permutation, and the mask is one bit wide.
constexpr const char* kFillExtrusionConstants[] = {"colorFromAttribute"};
constexpr const char* kSymbolSdfConstants[] = {"colorFromAttribute", "haloColorFromAttribute",
                                               "opacityFromAttribute", "haloWidthFromAttribute",
                                               "haloBlurFromAttribute"};
constexpr const char* kSymbolIconConstants[] = {"opacityFromAttribute"};
constexpr const char* kCircleConstants[] = {
    "colorFromAttribute",       "radiusFromAttribute",      "blurFromAttribute",
    "opacityFromAttribute",     "strokeColorFromAttribute", "strokeWidthFromAttribute",
    "strokeOpacityFromAttribute"};

std::pair<const char* const*, std::size_t> paintConstants(std::int32_t shader) {
    switch (shader) {
        case TSL_BUILTIN_FILL_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
            return {kFillConstants, std::size(kFillConstants)};
        case TSL_BUILTIN_LINE_SHADER:
            return {kLineConstants, std::size(kLineConstants)};
        case TSL_BUILTIN_CIRCLE_SHADER:
            return {kCircleConstants, std::size(kCircleConstants)};
        case TSL_BUILTIN_FILL_EXTRUSION_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER:
            return {kFillExtrusionConstants, std::size(kFillExtrusionConstants)};
        case TSL_BUILTIN_SYMBOL_SDFSHADER:
            return {kSymbolSdfConstants, std::size(kSymbolSdfConstants)};
        case TSL_BUILTIN_SYMBOL_ICON_SHADER:
            return {kSymbolIconConstants, std::size(kSymbolIconConstants)};
        default:
            return {nullptr, 0};
    }
}

/// One zoom-mix factor: the material parameter it feeds, and where the drawable block holds it.
///
/// Named per family rather than derived from the slot order, because a block's layout is mbgl's
/// and does not follow the attribute ids: a circle's `extrude_scale` sits between the matrix and
/// the factors, so its first factor is at 72 where a line's is at 68.
struct MixFactor {
    const char* name;
    std::size_t offset;
};

constexpr MixFactor kFillFactors[] = {{"colorT", 64}, {"opacityT", 68}};
constexpr MixFactor kLineFactors[] = {{"colorT", 68},    {"blurT", 72},   {"opacityT", 76},
                                      {"gapWidthT", 80}, {"offsetT", 84}, {"widthT", 88}};
constexpr MixFactor kFillExtrusionFactors[] = {{"colorT", 96}};
// Behind the matrices, the two texture sizes and the size pair; `SymbolDrawableUBO` is 260 bytes
// and these are its last five.
constexpr MixFactor kSymbolSdfFactors[] = {{"colorT", 240},
                                           {"haloColorT", 244},
                                           {"opacityT", 248},
                                           {"haloWidthT", 252},
                                           {"haloBlurT", 256}};
constexpr MixFactor kSymbolIconFactors[] = {{"opacityT", 248}};
constexpr MixFactor kCircleFactors[] = {
    {"colorT", 72},       {"radiusT", 76},      {"blurT", 80},         {"opacityT", 84},
    {"strokeColorT", 88}, {"strokeWidthT", 92}, {"strokeOpacityT", 96}};

std::pair<const MixFactor*, std::size_t> mixFactors(std::int32_t shader) {
    switch (shader) {
        case TSL_BUILTIN_FILL_SHADER:
        case TSL_BUILTIN_FILL_OUTLINE_SHADER:
            return {kFillFactors, std::size(kFillFactors)};
        case TSL_BUILTIN_LINE_SHADER:
            return {kLineFactors, std::size(kLineFactors)};
        case TSL_BUILTIN_CIRCLE_SHADER:
            return {kCircleFactors, std::size(kCircleFactors)};
        case TSL_BUILTIN_FILL_EXTRUSION_SHADER:
        case TSL_BUILTIN_FILL_EXTRUSION_INSTANCED_SHADER:
            return {kFillExtrusionFactors, std::size(kFillExtrusionFactors)};
        case TSL_BUILTIN_SYMBOL_SDFSHADER:
            return {kSymbolSdfFactors, std::size(kSymbolSdfFactors)};
        case TSL_BUILTIN_SYMBOL_ICON_SHADER:
            return {kSymbolIconFactors, std::size(kSymbolIconFactors)};
        default:
            return {nullptr, 0};
    }
}

/// Which of the three surface tables a material came from.
///
/// Part of the key rather than three parallel maps: a permutation is a property of the paint and
/// a surface is a property of the geometry, and the pair is what names a program.
constexpr std::uint32_t kSurfaceFlat = 0;
constexpr std::uint32_t kSurfaceGlobe = 1;
constexpr std::uint32_t kSurfaceAnchored = 2;

std::uint32_t materialKey(std::int32_t family, std::uint32_t surface) {
    return (static_cast<std::uint32_t>(family) << 2) | surface;
}

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
        // One float, which is what a data-driven scalar supplies when it does not also vary with
        // zoom -- `line-width`, `circle-radius`, `fill-opacity`. Its absence here was silent and
        // total: `attributeType` answering false made the attribute *not present*, so the mask
        // bit was never set, the material was specialized to read the layer's uniform, and every
        // such property fell back to its spec default. Data-driven line widths drew at one pixel.
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT: out = AT::FLOAT; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT2: out = AT::FLOAT2; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT3: out = AT::FLOAT3; return true;
        case TSL_ATTRIBUTE_DATA_TYPE_FLOAT4: out = AT::FLOAT4; return true;
        default: return false;
    }
}

/// How many bytes one vertex of a wire attribute type occupies.
///
/// Only the types a paint attribute can arrive as, which is a run of floats: the walls copy an
/// instance's colour bytes verbatim rather than decoding them, so they need the width and not
/// just the type.
std::size_t attributeBytes(filament::VertexBuffer::AttributeType type) {
    using AT = filament::VertexBuffer::AttributeType;
    switch (type) {
        case AT::FLOAT: return sizeof(float);
        case AT::FLOAT2: return sizeof(float) * 2;
        case AT::FLOAT3: return sizeof(float) * 3;
        case AT::FLOAT4: return sizeof(float) * 4;
        default: return 0;
    }
}

} // namespace

/// Buffer indexes for a vertex buffer, one per distinct slab rather than one per attribute.
///
/// The producer interleaves a layer's data-driven paint into a single allocation: six of a line's
/// attributes share one pointer and one stride and differ only in offset. An index apiece copies
/// those bytes six times, and -- because `VertexBuffer::Builder` admits eight buffers, not the
/// sixteen `MAX_VERTEX_BUFFER_COUNT` suggests -- a symbol's nine attributes do not fit at all.
///
/// `padded` is what makes a slab wider than the producer sent it. A property that varies per
/// feature but not with zoom supplies the narrow form at the narrow stride while the shader reads
/// the wide one, so the last vertex's read runs off the end; the tail is zeroed rather than
/// absent. Two attributes on one slab take the widest padding either of them asks for.
struct Slab {
    /// The producer's bytes, or null for the shared zero buffer.
    const std::uint8_t* data;
    /// How many to copy, padded to what the widest attribute on it reads.
    std::size_t bytes;
};

class Slabs {
public:
    /// The index for a slab, allocated on first sight.
    std::uint8_t of(const std::uint8_t* data, std::size_t bytes, std::uint32_t vertices,
                    std::uint32_t stride, std::size_t declaredBytes) {
        const std::size_t span =
            vertices == 0 ? bytes
                          : static_cast<std::size_t>(vertices - 1) * stride
                                + std::max<std::size_t>(stride, declaredBytes);
        for (std::size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].data == data) {
                entries_[i].bytes = std::max(entries_[i].bytes, std::max(bytes, span));
                return static_cast<std::uint8_t>(i);
            }
        }
        entries_.push_back({data, std::max(bytes, span)});
        return static_cast<std::uint8_t>(entries_.size() - 1);
    }

    /// The index for the shared zero buffer, which carries no bytes of its own.
    std::uint8_t zero() {
        if (!zero_) {
            entries_.push_back({nullptr, 0});
            zero_ = true;
            zeroAt_ = static_cast<std::uint8_t>(entries_.size() - 1);
        }
        return zeroAt_;
    }

    [[nodiscard]] std::size_t count() const { return entries_.size(); }
    [[nodiscard]] const std::vector<Slab>& entries() const { return entries_; }

private:
    std::vector<Slab> entries_;
    bool zero_ = false;
    std::uint8_t zeroAt_ = 0;
};

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
        const auto endsWith = [](const std::string& text, const std::string& tail) {
            return text.size() > tail.size()
                   && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
        };
        // Longest first: `_globe_anchored` also ends in `_anchored`, and testing `_globe` first
        // would leave `fill_globe_anchored` filed as a family called `fill_globe_anchored`.
        const std::string anchoredSuffix = "_globe_anchored";
        const std::string globeSuffix = "_globe";
        const bool anchored = endsWith(stem, anchoredSuffix);
        const bool bent = anchored || endsWith(stem, globeSuffix);
        if (anchored) {
            stem.erase(stem.size() - anchoredSuffix.size());
        } else if (bent) {
            stem.erase(stem.size() - globeSuffix.size());
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
            if (anchored) {
                anchoredMaterials_[family] = material;
            } else {
                (bent ? globeMaterials_ : materials_)[family] = material;
            }
            // Kept whether or not the family has constants: which families have them changes,
            // and a package held is cheaper than one re-read from a directory that may not still
            // be there. The default build above is the all-uniform permutation, so a mask of
            // zero never needs the bytes again.
            packages_[materialKey(family, anchored ? kSurfaceAnchored
                                                   : (bent ? kSurfaceGlobe : kSurfaceFlat))] =
                package;
        } else {
            // Filament has already said why on stderr. Counted so a caller can
            // tell "this directory has no materials" from "these materials were
            // refused", which are different mistakes with the same black frame.
            materialsRejected_++;
        }
    }

    // The globe's depth shell.
    const auto shellPath = std::filesystem::path(materialDir) / "globe_shell.filamat";
    if (std::filesystem::exists(shellPath, ec)) {
        std::ifstream file(shellPath, std::ios::binary);
        const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
        if (!package.empty()) {
            shellMaterial_ =
                filament::Material::Builder().package(package.data(), package.size()).build(*engine_);
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

    // And the same mask on the expansion, for tiles whose geometry is drawn that way. A mask has
    // to trace the same curve as what it admits; see `mask_globe_anchored.mat`.
    const auto anchoredMaskPath = std::filesystem::path(materialDir) / "mask_globe_anchored.filamat";
    if (std::filesystem::exists(anchoredMaskPath, ec)) {
        std::ifstream file(anchoredMaskPath, std::ios::binary);
        const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
        if (!package.empty()) {
            maskAnchoredMaterial_ = filament::Material::Builder()
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

/// The globe's depth shell, added to the scene for this frame.
///
/// A lat/lon sphere at 96 by 48, which keeps its silhouette within a fraction of a pixel of the
/// tile geometry's at any zoom. That matters in one direction: the shell sits a thousandth of a
/// radius *inside* the surface, and a coarser one would poke through and occlude the very content
/// it exists to let past.
void FilamentRenderer::writeShell() {
    static const bool noShell = std::getenv("TSF_NO_SHELL") != nullptr;
    if (noShell) {
        return;
    }
    if (projection_ != TSL_PROJECTION_MODE_GLOBE || shellMaterial_ == nullptr) {
        return;
    }
    if (shellVertices_ == nullptr) {
        constexpr int kRings = 48;
        constexpr int kSegments = 96;
        std::vector<filament::math::float3> points;
        points.reserve(static_cast<std::size_t>(kRings + 1) * (kSegments + 1));
        for (int ring = 0; ring <= kRings; ++ring) {
            const double phi =
                M_PI * (static_cast<double>(ring) / kRings - 0.5);
            for (int seg = 0; seg <= kSegments; ++seg) {
                const double theta = 2.0 * M_PI * static_cast<double>(seg) / kSegments;
                // `globe::sphere_point`'s axes, y negated -- the same convention the bend uses, so
                // the shell and the surface are the same sphere rather than two that nearly agree.
                points.emplace_back(static_cast<float>(std::cos(phi) * std::sin(theta)),
                                    static_cast<float>(-std::sin(phi)),
                                    static_cast<float>(std::cos(phi) * std::cos(theta)));
            }
        }
        std::vector<std::uint16_t> indices;
        indices.reserve(static_cast<std::size_t>(kRings) * kSegments * 6);
        const auto at = [](int ring, int seg) {
            return static_cast<std::uint16_t>(ring * (kSegments + 1) + seg);
        };
        for (int ring = 0; ring < kRings; ++ring) {
            for (int seg = 0; seg < kSegments; ++seg) {
                for (const std::uint16_t index :
                     {at(ring, seg), at(ring + 1, seg), at(ring, seg + 1), at(ring, seg + 1),
                      at(ring + 1, seg), at(ring + 1, seg + 1)}) {
                    indices.push_back(index);
                }
            }
        }
        shellIndexCount_ = static_cast<std::uint32_t>(indices.size());

        shellVertices_ = filament::VertexBuffer::Builder()
                             .vertexCount(static_cast<std::uint32_t>(points.size()))
                             .bufferCount(1)
                             .attribute(filament::VertexAttribute::POSITION, 0,
                                        filament::VertexBuffer::AttributeType::FLOAT3, 0,
                                        sizeof(filament::math::float3))
                             .build(*engine_);
        const std::size_t vertexBytes = points.size() * sizeof(filament::math::float3);
        auto* vertexCopy = new filament::math::float3[points.size()];
        std::memcpy(vertexCopy, points.data(), vertexBytes);
        shellVertices_->setBufferAt(
            *engine_, 0,
            filament::VertexBuffer::BufferDescriptor(
                vertexCopy, vertexBytes, [](void* buffer, std::size_t, void*) {
                    delete[] static_cast<filament::math::float3*>(buffer);
                }));

        shellIndices_ = filament::IndexBuffer::Builder()
                            .indexCount(shellIndexCount_)
                            .bufferType(filament::IndexBuffer::IndexType::USHORT)
                            .build(*engine_);
        const std::size_t indexBytes = indices.size() * sizeof(std::uint16_t);
        auto* indexCopy = new std::uint16_t[indices.size()];
        std::memcpy(indexCopy, indices.data(), indexBytes);
        shellIndices_->setBuffer(
            *engine_, filament::IndexBuffer::BufferDescriptor(
                          indexCopy, indexBytes, [](void* buffer, std::size_t, void*) {
                              delete[] static_cast<std::uint16_t*>(buffer);
                          }));
        shellInstance_ = shellMaterial_->createInstance();
    }
    if (shellInstance_ == nullptr) {
        return;
    }
    shellInstance_->setParameter("globeMatrix", globeMatrix_);
    // Diagnostic: a shell nobody can mistake for the background. With TSF_SHELL_COLOR set, any
    // pixel of this colour inside the disc is sphere with no surface tile over it.
    static const char* const shellOverride = std::getenv("TSF_SHELL_COLOR");
    if (shellOverride != nullptr) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        std::sscanf(shellOverride, "%f,%f,%f", &r, &g, &b);
        shellInstance_->setParameter("color", filament::math::float4{r, g, b, 1.0f});
    } else {
        shellInstance_->setParameter("color", shellColor_);
    }

    filament::RenderableManager::Builder builder(1);
    builder.boundingBox({{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}})
        .layerMask(0xFF, layer_)
        .culling(false)
        // Before every band a map layer uses, which is what "the ground beneath the ground" means.
        .priority(0)
        .material(0, shellInstance_)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, shellVertices_,
                  shellIndices_, 0, shellIndexCount_);
    utils::Entity entity = utils::EntityManager::get().create();
    if (builder.build(*engine_, entity) != filament::RenderableManager::Builder::Success) {
        utils::EntityManager::get().destroy(entity);
        return;
    }
    // The shell places itself through `globeMatrix`, so the renderable carries the identity.
    auto& transforms = engine_->getTransformManager();
    transforms.setTransform(transforms.getInstance(entity), filament::math::mat4f());
    scene_->addEntity(entity);
    entities_.push_back(entity);
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

        // The expansion, if this tile's geometry is drawn through it. A mask on the other curve
        // is a sliver cut off every tile edge -- survivable for a fill, fatal for a three-pixel
        // road, which is what left a clean gap across Seattle's street grid along a tile row.
        //
        // Bounded by the crossover exactly as the drawables are, and for the same reason: the
        // expansion is about a tile's centre and a tile below it subtends too much sphere for a
        // quadratic. A mask on an invalid expansion cuts the fill it exists to admit -- at z1 it
        // took fourteen thousand pixels of ocean out of the planet in straight-edged wedges, which
        // reads as land. The drawables were bounded and this was not, so the two disagreed at
        // exactly the zooms where the mask is the only one of them that is wrong.
        const auto bendFor = tileBends_.find(tile);
        const bool anchoredMask = bent && maskAnchoredMaterial_ != nullptr
                                  && bendFor != tileBends_.end()
                                  && tile.z >= kAnchoredFromZoom;
        auto* instance = (anchoredMask ? maskAnchoredMaterial_
                                       : (bent ? maskGlobeMaterial_ : maskMaterial_))
                             ->createInstance();
        maskInstances_.push_back(instance);
        instance->setColorWrite(std::getenv("TSF_SHOW_MASKS") != nullptr);
        // Each mask painted by its own reference, so the stencil's layout can be looked at.
        instance->setParameter("color", filament::math::float4{
            static_cast<float>(reference) / 8.0f,
            static_cast<float>(tile.overscaled_z % 4) / 4.0f, 0.5f, 1.0f});
        instance->setParameter("opacity", 1.0f);
        instance->setDepthWrite(false);
        // Tested against the shell on a globe, for the reason the colour pass is: both hemispheres
        // project onto the same disc, so a far-side tile's mask lands on the same pixels as a
        // near-side one. Untested, REPLACE let whichever was drawn last own the pixel -- and in the
        // middle of the disc that was the far side, so every near-side fill failed its own
        // reference and the water vanished from the centre outward, leaving a ring of ocean at the
        // limb and background everywhere else. Never writes depth either way; the shell is the only
        // writer, and a mask that wrote would hide the fill it exists to admit.
        instance->setDepthCulling(bent);
        instance->setStencilWrite(true);
        instance->setStencilReferenceValue(reference);
        instance->setStencilCompareFunction(filament::MaterialInstance::StencilCompareFunc::A);
        instance->setStencilOpDepthStencilPass(filament::MaterialInstance::StencilOperation::REPLACE);

        // The bend, from the same data the geometry it clips is drawn through.
        const MaskGrid grid = bent ? maskGrid(kGlobeMaskCells) : MaskGrid{};
        if (anchoredMask) {
            const auto& rows = bendFor->second;
            instance->setParameter("bendCenter",
                                   filament::math::float2{kHalfExtent, kHalfExtent});
            instance->setParameter("bendAnchor", rows[0]);
            instance->setParameter("bendU", rows[1]);
            instance->setParameter("bendV", rows[2]);
            instance->setParameter("bendUU", rows[3]);
            instance->setParameter("bendVV", rows[4]);
            instance->setParameter("bendUV", rows[5]);
        } else if (bent) {
            instance->setParameter("matrix", matrix);
            instance->setParameter("globeMatrix", globeMatrix_);
        }

        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{0, 0, 0}, {8192, 8192, 8192}})
            .layerMask(0xFF, layer_)
            .culling(false)
            // One past the shell's, so the shell has written depth before any mask tests against
            // it. Both sat in band zero, and Filament orders within a band as it likes -- so on a
            // globe a mask could be written before there was any depth to reject it, which is the
            // whole of the depth test above.
            .priority(static_cast<std::uint8_t>(band + 1))
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
        for (auto* object : mesh.ownedBuffers) {
            engine_->destroy(object);
        }
    }
    // Before the materials the permutations were specialized from -- Filament refuses to destroy
    // a material an instance still points at, and an instance was made against the permutation.
    for (auto& [key, material] : permuted_) {
        if (material != nullptr) {
            engine_->destroy(material);
        }
    }
    permuted_.clear();
    if (zeroPaint_ != nullptr) {
        engine_->destroy(zeroPaint_);
        zeroPaint_ = nullptr;
    }
    for (auto* object : retiredZeroPaint_) {
        engine_->destroy(object);
    }
    retiredZeroPaint_.clear();
    for (auto& [family, material] : materials_) {
        engine_->destroy(material);
    }
    // The globe's tables, which the flat path never touches and which were leaking a material per
    // family and a buffer pair per grid for the life of the process.
    for (auto& [family, material] : anchoredMaterials_) {
        engine_->destroy(material);
    }
    anchoredMaterials_.clear();
    for (auto& [family, material] : globeMaterials_) {
        engine_->destroy(material);
    }
    globeMaterials_.clear();
    if (maskAnchoredMaterial_ != nullptr) {
        engine_->destroy(maskAnchoredMaterial_);
        maskAnchoredMaterial_ = nullptr;
    }
    if (maskGlobeMaterial_ != nullptr) {
        engine_->destroy(maskGlobeMaterial_);
        maskGlobeMaterial_ = nullptr;
    }
    for (auto& [cells, grid] : maskGrids_) {
        engine_->destroy(grid.vertices);
        engine_->destroy(grid.indices);
    }
    maskGrids_.clear();
    if (shellInstance_ != nullptr) {
        engine_->destroy(shellInstance_);
        shellInstance_ = nullptr;
    }
    if (shellVertices_ != nullptr) {
        engine_->destroy(shellVertices_);
        shellVertices_ = nullptr;
    }
    if (shellIndices_ != nullptr) {
        engine_->destroy(shellIndices_);
        shellIndices_ = nullptr;
    }
    if (shellMaterial_ != nullptr) {
        engine_->destroy(shellMaterial_);
        shellMaterial_ = nullptr;
    }
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
    const Attribute* colour = nullptr;
    for (const Attribute& attribute : add.instanceAttrs) {
        if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_OUTLINE_POS_ATTRIBUTE) {
            positions = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_DECIMALS_ED_ATTRIBUTE) {
            decimals = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_BASE_VERTEX_ATTRIBUTE) {
            base = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_HEIGHT_VERTEX_ATTRIBUTE) {
            height = &attribute;
        } else if (attribute.desc.attr_id == TSL_UBO_ID_FILL_EXTRUSION_COLOR_VERTEX_ATTRIBUTE) {
            colour = &attribute;
        }
    }
    // The width of one instance's colour, and zero when the layer's paint is uniform. A type the
    // wire names and this build cannot bind is treated as absent rather than as garbage.
    filament::VertexBuffer::AttributeType colourType =
        filament::VertexBuffer::AttributeType::FLOAT4;
    std::size_t colourWidth = 0;
    if (colour != nullptr && attributeType(colour->desc.data_type, colourType)) {
        colourWidth = attributeBytes(colourType);
    }
    if (colourWidth == 0) {
        colour = nullptr;
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
    // The colour, replicated the same way and copied rather than decoded: the bytes are mbgl's
    // packed pair and the shader unpacks them, so the wall never needs to know what is in them.
    std::vector<std::uint8_t> colours;
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
        // This instance's colour bytes, or none when the layer's paint is uniform.
        const std::uint8_t* instanceColour =
            colour != nullptr && i < colour->count()
                ? colour->data.data + i * colour->desc.stride + colour->desc.offset
                : nullptr;

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
            if (colourWidth != 0) {
                const std::size_t at = colours.size();
                colours.resize(at + colourWidth, 0);
                if (instanceColour != nullptr) {
                    std::memcpy(colours.data() + at, instanceColour, colourWidth);
                }
            }
        }
        for (std::size_t k = 0; k < templateIndexCount; k++) {
            indexes.push_back(static_cast<std::uint16_t>(corner + templateIndexes[k]));
        }
    }
    if (indexes.empty()) {
        return false;
    }

    const auto vertexCount = static_cast<std::uint32_t>(vertices.size() / 3);
    auto* shared = zeroPaint(vertexCount);
    if (shared == nullptr) {
        return false;
    }
    auto* built = filament::VertexBuffer::Builder()
                      .vertexCount(vertexCount)
                      .bufferCount(4)
                      .enableBufferObjects()
                      .attribute(filament::VertexAttribute::POSITION, 0,
                                 filament::VertexBuffer::AttributeType::FLOAT3, 0, 12)
                      .attribute(filament::VertexAttribute::CUSTOM0, 1,
                                 filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                      .attribute(filament::VertexAttribute::CUSTOM1, 2,
                                 filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                      // The colour, declared whether or not this layer drives it -- `requires` is
                      // baked into the package -- and fed by the shared zero buffer when it does
                      // not, which the specialization compiles away.
                      .attribute(filament::VertexAttribute::CUSTOM2, 3,
                                 colour != nullptr ? colourType
                                                   : filament::VertexBuffer::AttributeType::FLOAT4,
                                 0, static_cast<std::uint8_t>(colour != nullptr ? colourWidth : 16))
                      .build(*engine_);
    if (built == nullptr) {
        return false;
    }
    std::vector<filament::BufferObject*> owned;
    const auto uploadBytes = [&](std::uint8_t slot, const std::uint8_t* from, std::size_t bytes) {
        auto* object = filament::BufferObject::Builder()
                           .size(static_cast<std::uint32_t>(bytes))
                           .bindingType(filament::BufferObject::BindingType::VERTEX)
                           .build(*engine_);
        auto* copy = object == nullptr ? nullptr : static_cast<std::uint8_t*>(std::malloc(bytes));
        if (copy == nullptr) {
            if (object != nullptr) {
                engine_->destroy(object);
            }
            return;
        }
        std::memcpy(copy, from, bytes);
        object->setBuffer(*engine_, filament::BufferObject::BufferDescriptor(
                                        copy, bytes, [](void* buffer, std::size_t, void*) {
                                            std::free(buffer);
                                        }));
        built->setBufferObjectAt(*engine_, slot, object);
        owned.push_back(object);
    };
    const auto upload = [&](std::uint8_t slot, const std::vector<float>& from) {
        uploadBytes(slot, reinterpret_cast<const std::uint8_t*>(from.data()),
                    from.size() * sizeof(float));
    };
    upload(0, vertices);
    upload(1, normals);
    upload(2, extents);
    if (colour != nullptr) {
        uploadBytes(3, colours.data(), colours.size());
    } else {
        built->setBufferObjectAt(*engine_, 3, shared);
    }

    const auto indexCount = static_cast<std::uint32_t>(indexes.size());
    auto* built_indexes = filament::IndexBuffer::Builder()
                              .indexCount(indexCount)
                              .bufferType(filament::IndexBuffer::IndexType::USHORT)
                              .build(*engine_);
    if (built_indexes == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
        engine_->destroy(built);
        return false;
    }
    const std::size_t indexBytes = indexes.size() * sizeof(std::uint16_t);
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(indexBytes));
    if (ownedIndexes == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
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
    meshes_[add.id].paintMask = colour != nullptr ? 1u : 0u;
    meshes_[add.id].ownedBuffers = std::move(owned);
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
    const Attribute* colour = nullptr;
    for (const Attribute& attribute : add.attrs) {
        switch (attribute.desc.attr_id) {
            case TSL_UBO_ID_FILL_EXTRUSION_POS_VERTEX_ATTRIBUTE: position = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_DECIMALS_ED_ATTRIBUTE: decimals = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_BASE_VERTEX_ATTRIBUTE: base = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_HEIGHT_VERTEX_ATTRIBUTE: height = &attribute; break;
            case TSL_UBO_ID_FILL_EXTRUSION_COLOR_VERTEX_ATTRIBUTE: colour = &attribute; break;
            default: break;
        }
    }
    if (position == nullptr || decimals == nullptr) {
        return false;
    }
    filament::VertexBuffer::AttributeType colourType{};
    if (colour != nullptr && !attributeType(colour->desc.data_type, colourType)) {
        colour = nullptr;
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

    auto* shared = zeroPaint(count);
    if (shared == nullptr) {
        return false;
    }
    auto* vertices = filament::VertexBuffer::Builder()
                         .vertexCount(count)
                         .bufferCount(5)
                         .enableBufferObjects()
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
                         // The colour, when this layer's is the feature's. Declared either way,
                         // because `requires` is baked into the package -- see
                         // `native/test/permutation_probe.cc` -- and fed by the shared zero
                         // buffer when it is the layer's, which the constant compiles away.
                         .attribute(filament::VertexAttribute::CUSTOM3, 4,
                                    colour != nullptr ? colourType
                                                      : filament::VertexBuffer::AttributeType::FLOAT4,
                                    colour != nullptr ? colour->desc.offset : 0,
                                    colour != nullptr ? colour->desc.stride
                                                      : sizeof(float) * 4)
                         .build(*engine_);
    if (vertices == nullptr) {
        return false;
    }
    std::vector<filament::BufferObject*> owned;
    const auto upload = [&](std::uint8_t slot, const std::uint8_t* from, std::size_t bytes) {
        auto* object = filament::BufferObject::Builder()
                           .size(static_cast<std::uint32_t>(bytes))
                           .bindingType(filament::BufferObject::BindingType::VERTEX)
                           .build(*engine_);
        auto* copy = object == nullptr ? nullptr : static_cast<std::uint8_t*>(std::malloc(bytes));
        if (copy == nullptr) {
            if (object != nullptr) {
                engine_->destroy(object);
            }
            return;
        }
        std::memcpy(copy, from, bytes);
        object->setBuffer(*engine_, filament::BufferObject::BufferDescriptor(
                                        copy, bytes, [](void* buffer, std::size_t, void*) {
                                            std::free(buffer);
                                        }));
        vertices->setBufferObjectAt(*engine_, slot, object);
        owned.push_back(object);
    };
    upload(0, position->data.data, position->data.size);
    upload(1, decimals->data.data, decimals->data.size);
    upload(2, base ? base->data.data : baseFill.data(),
           base ? base->data.size : baseFill.size());
    upload(3, height ? height->data.data : heightFill.data(),
           height ? height->data.size : heightFill.size());
    if (colour != nullptr) {
        upload(4, colour->data.data, colour->data.size);
    } else {
        vertices->setBufferObjectAt(*engine_, 4, shared);
    }

    const auto indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    auto* indices = filament::IndexBuffer::Builder()
                        .indexCount(indexCount)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine_);
    if (indices == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
        engine_->destroy(vertices);
        return false;
    }
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
    if (ownedIndexes == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
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
    meshes_[add.id].paintMask = colour != nullptr ? 1u : 0u;
    meshes_[add.id].ownedBuffers = std::move(owned);
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

    // The paint tail, which is the same permutation every other family has. The four fixed
    // channels above are this family's own and have no bit.
    const auto [paintSlots, paintCount] = symbolPaintSlots(add.builtinShader);
    const auto findPaint = [&](std::uint32_t attrId) -> const Attribute* {
        for (const Attribute& a : add.attrs) {
            if (a.desc.attr_id == attrId && a.desc.binding >= 0 && !a.data.empty()) {
                return &a;
            }
        }
        return nullptr;
    };
    std::vector<const Attribute*> supplied(paintCount, nullptr);
    std::uint32_t paintMask = 0;
    auto* shared = zeroPaint(count);
    if (shared == nullptr) {
        return false;
    }

    // One index per slab: the three fixed channels share the symbol vertex struct, the packed
    // position-and-fade is its own, and every paint attribute shares the binder's one buffer.
    // Nine attributes over four indexes, where Filament admits eight.
    Slabs slabs;
    const auto fixed = slabs.of(posOffset->data.data, posOffset->data.size, count,
                                posOffset->desc.stride, 0);
    const auto placedAt =
        slabs.of(reinterpret_cast<const std::uint8_t*>(placed.data()),
                 placed.size() * sizeof(float), count, sizeof(float) * 4, sizeof(float) * 4);
    std::vector<std::uint8_t> at(paintCount, 0);
    for (std::size_t i = 0; i < paintCount; i++) {
        const PaintSlot& slot = paintSlots[i];
        const Attribute* attribute = findPaint(slot.attrId);
        filament::VertexBuffer::AttributeType type{};
        const bool present = attribute != nullptr && attributeType(attribute->desc.data_type, type);
        supplied[i] = present ? attribute : nullptr;
        if (present) {
            at[i] = slabs.of(attribute->data.data, attribute->data.size, count,
                             attribute->desc.stride, slot.declaredBytes);
            paintMask |= 1u << slot.bit;
        } else {
            at[i] = slabs.zero();
        }
    }

    filament::VertexBuffer::Builder builder;
    builder.vertexCount(count)
        .bufferCount(static_cast<std::uint8_t>(slabs.count()))
        .enableBufferObjects()
        .attribute(filament::VertexAttribute::POSITION, fixed,
                   filament::VertexBuffer::AttributeType::SHORT4, posOffset->desc.offset,
                   posOffset->desc.stride)
        .attribute(filament::VertexAttribute::CUSTOM0,
                   slabs.of(data->data.data, data->data.size, count, data->desc.stride, 0),
                   filament::VertexBuffer::AttributeType::USHORT4, data->desc.offset,
                   data->desc.stride)
        .attribute(filament::VertexAttribute::CUSTOM1,
                   slabs.of(pixelOffset->data.data, pixelOffset->data.size, count,
                            pixelOffset->desc.stride, 0),
                   filament::VertexBuffer::AttributeType::SHORT4, pixelOffset->desc.offset,
                   pixelOffset->desc.stride)
        .attribute(filament::VertexAttribute::CUSTOM2, placedAt,
                   filament::VertexBuffer::AttributeType::FLOAT4, 0, 16);
    for (std::size_t i = 0; i < paintCount; i++) {
        const PaintSlot& slot = paintSlots[i];
        const auto target = static_cast<filament::VertexAttribute>(
            filament::VertexAttribute::CUSTOM0 + slot.slot);
        if (supplied[i] != nullptr) {
            filament::VertexBuffer::AttributeType type{};
            attributeType(supplied[i]->desc.data_type, type);
            builder.attribute(target, at[i], type, supplied[i]->desc.offset,
                              supplied[i]->desc.stride);
        } else {
            builder.attribute(target, at[i], slot.declared, 0,
                              static_cast<std::uint8_t>(slot.declaredBytes));
        }
    }
    auto* vertices = builder.build(*engine_);
    if (vertices == nullptr) {
        return false;
    }
    std::vector<filament::BufferObject*> owned;
    if (!uploadSlabs(slabs, count, *vertices, owned)) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
        engine_->destroy(vertices);
        return false;
    }
    if (tracingPaint) {
        std::fprintf(stderr, "paint symbol shader=%d verts=%u mask=%u\n", add.builtinShader,
                     count, paintMask);
    }

    const auto indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    auto* indices = filament::IndexBuffer::Builder()
                        .indexCount(indexCount)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine_);
    if (indices == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
        engine_->destroy(vertices);
        return false;
    }
    auto* ownedIndexes = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
    if (ownedIndexes == nullptr) {
        for (auto* object : owned) {
            engine_->destroy(object);
        }
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
    meshes_[add.id].paintMask = paintMask;
    meshes_[add.id].ownedBuffers = std::move(owned);
    return true;
}

/// Uploads a drawable's indices, rebasing them onto the whole vertex buffer where the producer
/// split them into segments.
///
/// Indices are `u16`, so a bucket with more than 65,535 vertices cannot address itself with one
/// range: the producer splits it, and every segment's indices are relative to that segment's
/// `vertex_offset`. §12.4 says so and the ABI carries the offsets.
///
/// This consumer used to ignore them and draw the whole index buffer as one flat range. For a
/// single-segment bucket that is right, because the base is zero. For a split one it is not:
/// segment 1's indices were read as absolute, so its triangles were assembled from the *first*
/// vertices of the buffer instead of its own. Both halves of that are visible -- the polygons that
/// should have been drawn are missing, and triangles built from unrelated vertices appear
/// somewhere else as wedges across the map.
///
/// It only bites where a bucket passes 65,535 vertices, which is a dense layer at a low zoom: a
/// z9 landuse tile here carries 2,274 polygons and 66,972 vertices, 1,477 of them past the split.
/// At street zoom no bucket comes close, which is why every parity camera missed it.
///
/// Rebasing into `u32` rather than issuing one primitive per segment: Filament's geometry call
/// takes an index range but no base vertex, so a second primitive would need its own vertex
/// buffer. Widening the indices keeps one buffer, one primitive and one draw. A bucket that needs
/// no rebasing keeps its `u16` buffer, so nothing that was already correct changes size.
filament::Material* FilamentRenderer::materialFor(std::int32_t family, std::uint32_t surface,
                                                  std::uint32_t mask) {
    auto& table = surface == kSurfaceAnchored ? anchoredMaterials_
                  : surface == kSurfaceGlobe  ? globeMaterials_
                                              : materials_;
    const auto base = table.find(family);
    if (mask == 0) {
        return base == table.end() ? nullptr : base->second;
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(mask) << 32) | materialKey(family, surface);
    if (const auto found = permuted_.find(key); found != permuted_.end()) {
        return found->second;
    }
    const auto package = packages_.find(materialKey(family, surface));
    const auto [names, count] = paintConstants(family);
    if (package == packages_.end() || names == nullptr) {
        // A family with no constants has one program, and a drawable that thinks otherwise is a
        // table and a material that disagree. Drawn with the uniform form rather than not at all:
        // the wrong colour is a bug worth seeing, and a missing layer looks like a data problem.
        return base == table.end() ? nullptr : base->second;
    }
    filament::Material::Builder builder;
    builder.package(package->second.data(), package->second.size());
    for (std::size_t i = 0; i < count; i++) {
        builder.constant(names[i], (mask & (1u << i)) != 0);
    }
    auto* built = builder.build(*engine_);
    // Cached even when null, so a package that cannot be specialized is not retried once per
    // drawable per frame.
    permuted_.emplace(key, built);
    return built != nullptr ? built : (base == table.end() ? nullptr : base->second);
}

bool FilamentRenderer::uploadSlabs(const Slabs& slabs, std::uint32_t vertices,
                                   filament::VertexBuffer& into,
                                   std::vector<filament::BufferObject*>& owned) {
    filament::BufferObject* shared = nullptr;
    for (std::size_t i = 0; i < slabs.entries().size(); i++) {
        const Slab& slab = slabs.entries()[i];
        if (slab.data == nullptr) {
            if (shared == nullptr) {
                shared = zeroPaint(vertices);
                if (shared == nullptr) {
                    return false;
                }
            }
            into.setBufferObjectAt(*engine_, static_cast<std::uint8_t>(i), shared);
            continue;
        }
        auto* object = filament::BufferObject::Builder()
                           .size(static_cast<std::uint32_t>(slab.bytes))
                           .bindingType(filament::BufferObject::BindingType::VERTEX)
                           .build(*engine_);
        // Zeroed rather than merely allocated: `bytes` may exceed what the producer sent, and the
        // tail is what a wide read past the last vertex lands in.
        auto* copy =
            object == nullptr ? nullptr : static_cast<std::uint8_t*>(std::calloc(slab.bytes, 1));
        if (copy == nullptr) {
            if (object != nullptr) {
                engine_->destroy(object);
            }
            return false;
        }
        std::memcpy(copy, slab.data, slab.bytes);
        object->setBuffer(*engine_,
                          filament::BufferObject::BufferDescriptor(
                              copy, slab.bytes,
                              [](void* buffer, std::size_t, void*) { std::free(buffer); }));
        into.setBufferObjectAt(*engine_, static_cast<std::uint8_t>(i), object);
        owned.push_back(object);
    }
    return true;
}

filament::BufferObject* FilamentRenderer::zeroPaint(std::size_t vertices) {
    // The widest paint attribute any family declares is a zoom-varying colour, four floats.
    constexpr std::size_t kWidest = sizeof(float) * 4;
    if (zeroPaint_ != nullptr && vertices <= zeroPaintVertices_) {
        return zeroPaint_;
    }
    // Grown in steps rather than to the exact count, so a frame of slowly larger tiles does not
    // allocate a buffer per drawable.
    std::size_t want = zeroPaintVertices_ == 0 ? 4096 : zeroPaintVertices_;
    while (want < vertices) {
        want *= 2;
    }
    auto* grown = filament::BufferObject::Builder()
                      .size(static_cast<std::uint32_t>(want * kWidest))
                      .bindingType(filament::BufferObject::BindingType::VERTEX)
                      .build(*engine_);
    if (grown == nullptr) {
        return zeroPaint_;
    }
    auto* zeros = static_cast<std::uint8_t*>(std::calloc(want, kWidest));
    if (zeros == nullptr) {
        engine_->destroy(grown);
        return zeroPaint_;
    }
    grown->setBuffer(*engine_,
                     filament::BufferObject::BufferDescriptor(
                         zeros, want * kWidest,
                         [](void* buffer, std::size_t, void*) { std::free(buffer); }));
    // The old one is kept rather than destroyed: meshes built before this growth still point at
    // it, and Filament does not reference-count a buffer object against them. `retiredZeroPaint_`
    // is what frees it, at teardown, when nothing can be drawing from it.
    if (zeroPaint_ != nullptr) {
        retiredZeroPaint_.push_back(zeroPaint_);
    }
    zeroPaint_ = grown;
    zeroPaintVertices_ = want;
    return zeroPaint_;
}

filament::IndexBuffer* FilamentRenderer::uploadIndices(const DrawableAdd& add) {
    const auto count = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
    const auto* source = reinterpret_cast<const std::uint16_t*>(add.indexes.data);
    const bool split = std::any_of(add.segments.begin(), add.segments.end(),
                                   [](const tsl_segment& s) { return s.vertex_offset != 0; });
    if (!split) {
        auto* buffer = filament::IndexBuffer::Builder()
                           .indexCount(count)
                           .bufferType(filament::IndexBuffer::IndexType::USHORT)
                           .build(*engine_);
        if (buffer == nullptr) {
            return nullptr;
        }
        auto* owned = static_cast<std::uint8_t*>(std::malloc(add.indexes.size));
        if (owned == nullptr) {
            engine_->destroy(buffer);
            return nullptr;
        }
        std::memcpy(owned, add.indexes.data, add.indexes.size);
        buffer->setBuffer(*engine_, filament::IndexBuffer::BufferDescriptor(
                                        owned, add.indexes.size,
                                        [](void* b, std::size_t, void*) { std::free(b); }));
        return buffer;
    }

    auto* widened = static_cast<std::uint32_t*>(std::malloc(count * sizeof(std::uint32_t)));
    if (widened == nullptr) {
        return nullptr;
    }
    // Copied through unchanged first, so an index outside every segment's range keeps its value
    // rather than becoming zero. The producer covers the whole buffer, but a consumer that assumed
    // it did and was wrong would draw a fan from vertex zero, which is the artifact this fixes.
    for (std::uint32_t i = 0; i < count; ++i) {
        widened[i] = source[i];
    }
    for (const tsl_segment& segment : add.segments) {
        // In 64 bits: a malformed length must clamp, not wrap past the start of the buffer.
        const std::uint64_t last = static_cast<std::uint64_t>(segment.index_offset) +
                                   static_cast<std::uint64_t>(segment.index_length);
        const auto end = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, last));
        for (std::uint32_t i = segment.index_offset; i < end; ++i) {
            widened[i] = static_cast<std::uint32_t>(source[i]) + segment.vertex_offset;
        }
    }
    auto* buffer = filament::IndexBuffer::Builder()
                       .indexCount(count)
                       .bufferType(filament::IndexBuffer::IndexType::UINT)
                       .build(*engine_);
    if (buffer == nullptr) {
        std::free(widened);
        return nullptr;
    }
    buffer->setBuffer(*engine_,
                      filament::IndexBuffer::BufferDescriptor(
                          widened, count * sizeof(std::uint32_t),
                          [](void* b, std::size_t, void*) { std::free(b); }));
    rebased_++;
    return buffer;
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

    static const bool traceGeometry = std::getenv("TSF_GEOM_TRACE") != nullptr;
    if (traceGeometry) {
        std::uint32_t maxIndex = 0;
        const auto n = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
        const auto* raw = reinterpret_cast<const std::uint16_t*>(add.indexes.data);
        for (std::uint32_t i = 0; i < n; ++i) {
            maxIndex = std::max(maxIndex, static_cast<std::uint32_t>(raw[i]));
        }
        const TileID tile = add.tileID.value_or(TileID{});
        std::fprintf(stderr, "geom id=%llu shader=%d layer=%d tile=%u/%u/%u vertices=%u indexes=%u "
                             "segments=%zu maxIndex=%u\n",
                     static_cast<unsigned long long>(add.id), add.builtinShader, add.layerIndex,
                     static_cast<unsigned>(tile.z), static_cast<unsigned>(tile.x),
                     static_cast<unsigned>(tile.y), static_cast<unsigned>(add.vertexCount), n,
                     add.segments.size(), maxIndex);
        for (const tsl_segment& seg : add.segments) {
            std::fprintf(stderr, "  seg v=%u+%u i=%u+%u\n", seg.vertex_offset, seg.vertex_length,
                         seg.index_offset, seg.index_length);
        }
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

    // A family that has declared its slots takes the permutation path: fixed slots, a mask, and
    // the shared zero buffer under whatever this drawable did not send.
    if (const auto [slots, slotCount] = paintSlots(add.builtinShader); slots != nullptr) {
        const auto find = [&](std::uint32_t attrId) -> const Attribute* {
            for (const Attribute& attribute : add.attrs) {
                if (attribute.desc.attr_id == attrId && attribute.desc.binding >= 0
                    && !attribute.data.empty()) {
                    return &attribute;
                }
            }
            return nullptr;
        };
        const Attribute* position = find(slots[0].attrId);
        filament::VertexBuffer::AttributeType positionType{};
        if (position == nullptr || !attributeType(position->desc.data_type, positionType)) {
            return;
        }

        std::uint32_t paintMask = 0;
        Slabs slabs;
        std::vector<const Attribute*> supplied(slotCount, nullptr);
        std::vector<std::uint8_t> at(slotCount, 0);
        for (std::size_t i = 0; i < slotCount; i++) {
            const PaintSlot& slot = slots[i];
            const auto attribute = i == 0 ? position : find(slot.attrId);
            filament::VertexBuffer::AttributeType type{};
            const bool present =
                attribute != nullptr && attributeType(attribute->desc.data_type, type);
            supplied[i] = present ? attribute : nullptr;
            if (present) {
                at[i] = slabs.of(attribute->data.data, attribute->data.size, add.vertexCount,
                                 attribute->desc.stride, slot.declaredBytes);
                if (slot.bit >= 0) {
                    paintMask |= 1u << slot.bit;
                }
            } else {
                at[i] = slabs.zero();
            }
        }
        filament::VertexBuffer::Builder builder;
        builder.vertexCount(static_cast<std::uint32_t>(add.vertexCount))
            .bufferCount(static_cast<std::uint8_t>(slabs.count()))
            .enableBufferObjects();
        for (std::size_t i = 0; i < slotCount; i++) {
            const PaintSlot& slot = slots[i];
            const auto target = static_cast<filament::VertexAttribute>(
                slot.slot < 0 ? filament::VertexAttribute::POSITION
                              : filament::VertexAttribute::CUSTOM0 + slot.slot);
            if (supplied[i] != nullptr) {
                filament::VertexBuffer::AttributeType type{};
                attributeType(supplied[i]->desc.data_type, type);
                builder.attribute(target, at[i], type, supplied[i]->desc.offset,
                                  supplied[i]->desc.stride);
            } else {
                // Declared so the material's `requires` is satisfied, and read from the shared
                // zero buffer, which the specialization means the shader never samples.
                builder.attribute(target, at[i], slot.declared, 0,
                                  static_cast<std::uint8_t>(slot.declaredBytes));
            }
        }
        // What arrived against what was taken. The mask is the whole of the permutation, and a
        // property that is data-driven in the style but absent from the mask is the failure this
        // found: `attributeType` refusing a type makes the attribute *not present*, which is
        // indistinguishable from a style that never asked for it. Printing the two together is
        // what separates them.
        if (tracingPaint) {
            std::fprintf(stderr, "paint shader=%d verts=%u mask=%u |", add.builtinShader,
                         static_cast<unsigned>(add.vertexCount), paintMask);
            for (const Attribute& a : add.attrs) {
                std::fprintf(stderr, " id=%u bind=%d off=%u stride=%u bytes=%zu",
                             a.desc.attr_id, a.desc.binding, a.desc.offset, a.desc.stride,
                             a.data.size);
            }
            std::fprintf(stderr, "\n");
        }
        auto* vertices = builder.build(*engine_);
        if (vertices == nullptr) {
            return;
        }

        std::vector<filament::BufferObject*> owned;
        const bool ok = uploadSlabs(slabs, add.vertexCount, *vertices, owned);
        auto* indices = ok ? uploadIndices(add) : nullptr;
        if (indices == nullptr) {
            for (auto* object : owned) {
                engine_->destroy(object);
            }
            engine_->destroy(vertices);
            return;
        }
        onRetire(add.id);
        Mesh mesh{};
        mesh.vertices = vertices;
        mesh.indices = indices;
        mesh.indexCount = static_cast<std::uint32_t>(add.indexes.size / sizeof(std::uint16_t));
        mesh.layerIndex = add.layerIndex;
        mesh.zoom = add.tileID ? add.tileID->z : std::uint8_t{0};
        mesh.overscaledZoom = add.tileID ? add.tileID->overscaled_z : std::uint8_t{0};
        mesh.tile = add.tileID ? *add.tileID : TileID{};
        mesh.colour = add.enableColor;
        mesh.clipped = add.enableStencil;
        mesh.paintMask = paintMask;
        mesh.ownedBuffers = std::move(owned);
        meshes_[add.id] = std::move(mesh);
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
    auto* indices = uploadIndices(add);
    if (indices == nullptr) {
        engine_->destroy(vertices);
        return;
    }

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
    // After the vertex buffer, which holds a reference to each of them. The shared zero buffer is
    // not in this list and outlives the mesh.
    for (auto* object : found->second.ownedBuffers) {
        engine_->destroy(object);
    }
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
    // Which tiles are drawn through the expansion, and with what.
    //
    // The mask has to trace the same curve as the geometry it admits, and its own record carries
    // only a placement matrix. It does not need a new one: every batch of this frame is already
    // queued in `pending_`, so the coefficients are here -- they were simply being read after the
    // masks rather than before. Walked once, keyed by tile, and handed to `writeMasks` below.
    tileBends_.clear();
    for (const Batch& batch : pending_) {
        const auto layer = uniforms_.find(static_cast<std::int32_t>(batch.layerIndex));
        if (layer == uniforms_.end()) {
            continue;
        }
        const auto bend = layer->second.find(kGlobeBendSlot);
        if (bend == layer->second.end()) {
            continue;
        }
        for (std::size_t i = 0; i < batch.geometries.size() && i < batch.uboIndexes.size(); i++) {
            const auto mesh = meshes_.find(batch.geometries[i]);
            if (mesh == meshes_.end()) {
                continue;
            }
            const std::size_t at =
                static_cast<std::size_t>(batch.uboIndexes[i]) * kGlobeBendStride;
            if (at + kGlobeBendStride > bend->second.size()) {
                continue;
            }
            const auto* rows =
                reinterpret_cast<const filament::math::float4*>(bend->second.data() + at);
            std::array<filament::math::float4, 6> copy{};
            std::copy(rows, rows + 6, copy.begin());
            tileBends_[mesh->second.tile] = copy;
        }
    }

    // The clip masks first, so every drawable issued below has a reference to test against.
    writeShell();
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
    // A family may have only the anchored package and no direct one. `line_globe_anchored.mat` has
    // no `line_globe.mat` beside it, because a line's quad is extruded sideways in clip space and
    // the trig form has no linear part to extrude along. Bailing on the direct table alone dropped
    // every road on the planet before the selection below could run.
    const bool anchoredOnly =
        bent && material == table.end() && anchoredMaterials_.count(batch.builtinShader) != 0;
    if (material == table.end() && !anchoredOnly) {
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
    // Five and six rather than four and five: band zero is the globe's depth shell and one
    // through four are the masks, which have to be written before anything tests against them.
    const auto band = static_cast<std::uint8_t>(
        batch.pass == static_cast<std::uint8_t>(TSL_RENDER_PASS_OPAQUE) ? 5 : 6);

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
        // The anchored bend, where the tile is small enough for a quadratic and the direct bend's
        // `f32` trig has started to cost more than a pixel. See `kAnchoredFromZoom`.
        //
        // Chosen per drawable rather than per batch, because a batch can hold tiles of more than
        // one zoom -- an ancestor drawn under its children is the ordinary case -- and the choice
        // is a property of how much sphere the tile covers.
        const filament::math::float4* bendRows = nullptr;
        auto anchoredMaterial = anchoredMaterials_.end();
        static const bool noAnchored = std::getenv("TSF_NO_ANCHORED") != nullptr;
        // The threshold binds even for a family that has no direct package. A quadratic about a
        // tile's centre is only as good as the arc that tile subtends, and a z0 tile subtends the
        // whole sphere -- so the expansion there is not a worse approximation, it is a wrong one,
        // and geometry drawn through it lands anywhere. Removing `fill_outline_globe.filamat` to
        // see what its anchored twin did alone showed it: coastlines smeared across the entire
        // viewport, far outside the planet.
        //
        // So below the crossover an anchored-only family draws nothing. A road missing at z4 is a
        // road missing; a road drawn through an invalid expansion is a stripe across the map.
        if (bent && !noAnchored && mesh->second.tile.z >= kAnchoredFromZoom) {
            anchoredMaterial = anchoredMaterials_.find(batch.builtinShader);
            if (anchoredMaterial != anchoredMaterials_.end()) {
                if (const auto bend = layer->second.find(kGlobeBendSlot);
                    bend != layer->second.end()) {
                    const std::size_t bendAt =
                        static_cast<std::size_t>(batch.uboIndexes[i]) * kGlobeBendStride;
                    if (bendAt + kGlobeBendStride <= bend->second.size()) {
                        bendRows = reinterpret_cast<const filament::math::float4*>(
                            bend->second.data() + bendAt);
                    }
                }
            }
        }
        const bool useAnchored = bendRows != nullptr;

        // The material is part of the key: a tile crossing the threshold mid-zoom would otherwise
        // keep the instance it was cached with and be drawn by the other form's shader, which
        // declares different parameters and would read whatever was last left in them.
        // The permutation is part of the key for the same reason the material is: two drawables
        // of one layer can differ in whether their paint came per feature, and the two programs
        // declare different attributes. The mask is the mesh's rather than the batch's because
        // the mesh is what declared the slots.
        const std::uint32_t paintMask = mesh->second.paintMask;
        const auto key = std::make_tuple(batch.layerIndex, batch.builtinShader, batch.uboIndexes[i],
                                         useAnchored, paintMask);
        // A family that only has an anchored package and did not take it has nothing to draw
        // with. Skipped rather than dereferencing the end iterator.
        if (!useAnchored && material == table.end()) {
            continue;
        }
        auto found = instances_.find(key);
        if (found == instances_.end()) {
            const std::uint32_t surface =
                useAnchored ? kSurfaceAnchored : (bent ? kSurfaceGlobe : kSurfaceFlat);
            auto* chosen = materialFor(batch.builtinShader, surface, paintMask);
            if (chosen == nullptr) {
                continue;
            }
            found = instances_.emplace(key, chosen->createInstance()).first;
            made_++;
        }
        auto* instance = found->second;

        // The frame's zoom-mix factors, which sit behind the matrix in every drawable block that
        // has them. Set whichever permutation this is: the shader multiplies them into a value it
        // may not read, and a stale factor on a reused instance would then apply to the drawable
        // that does. Zero is the right answer for a property that varies per feature but not with
        // zoom, and is what the producer sends for it.
        if (const auto [factors, factorCount] = mixFactors(batch.builtinShader);
            factors != nullptr) {
            for (std::size_t f = 0; f < factorCount; f++) {
                float value = 0.0f;
                if (at + factors[f].offset + sizeof value <= drawables->second.size()) {
                    std::memcpy(&value, drawables->second.data() + at + factors[f].offset,
                                sizeof value);
                }
                instance->setParameter(factors[f].name, value);
            }
        }

        // A bent drawable takes both halves of the bend. `transform` is the tile-local to
        // normalized Mercator placement the producer sent -- the same sixteen floats a Mercator
        // drawable would have used as its tile-to-clip matrix -- and `globeMatrix` is the frame's
        // sphere-to-clip. Every material in `globeMaterials_` declares both, which is why this is
        // unconditional here rather than per family.
        if (useAnchored) {
            // Half the tile's extent, which is where the producer expanded about. The same 8192
              // the scissor's own sampling walks, and the extent `camera::EXTENT` scales every
              // tile to on the way out.
            instance->setParameter("bendCenter",
                                   filament::math::float2{kHalfExtent, kHalfExtent});
            instance->setParameter("bendAnchor", bendRows[0]);
            instance->setParameter("bendU", bendRows[1]);
            instance->setParameter("bendV", bendRows[2]);
            instance->setParameter("bendUU", bendRows[3]);
            instance->setParameter("bendVV", bendRows[4]);
            instance->setParameter("bendUV", bendRows[5]);
            anchoredDrawn_++;
        } else if (bent) {
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
                // The globe's shell is painted in this, so a tile that has not arrived reads as
                // ocean rather than as a hole through the planet. Taken from the background
                // family rather than configured: it is the style's own answer to "what is under
                // everything", which is exactly what the shell is.
                if (batch.builtinShader == TSL_BUILTIN_BACKGROUND_SHADER) {
                    shellColor_ =
                        filament::math::float4{colour[0], colour[1], colour[2], 1.0f};
                }
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
                // An anchored line has no placement matrix: its position and its sideways
                // extrusion both come out of the expansion's own coefficients, and Filament
                // panics on a uniform the material does not declare.
                if (!useAnchored) {
                    instance->setParameter("matrix", transform);
                }
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
                if (useAnchored) {
                    // The producer sends the identity as the label plane when a label is walked
                    // along a line, because the walk projects the road point by point and its
                    // output is already in that plane. Read back rather than flagged: the block
                    // carries the matrix and nothing else, and identity is what it means.
                    const auto& plane = block.label_plane_matrix;
                    bool isIdentity = true;
                    for (std::size_t row = 0; row < 4 && isIdentity; row++) {
                        for (std::size_t col = 0; col < 4; col++) {
                            const float want = row == col ? 1.0f : 0.0f;
                            if (std::abs(plane[row * 4 + col] - want) > 1e-6f) {
                                isIdentity = false;
                                break;
                            }
                        }
                    }
                    instance->setParameter("alongLine", isIdentity ? 1.0f : 0.0f);
                    // Clip space to screen pixels, which is `camera::label_plane_matrix` with an
                    // identity placement: `(x + 1) * w / 2` across and `(1 - y) * h / 2` down. A
                    // plane folds its projection into `labelPlaneMatrix` and reaches the label
                    // plane with one multiply; a globe cannot, because the bend is not a matrix,
                    // so the material projects through the expansion and converts with this.
                    const float halfWidth = static_cast<float>(width_) * 0.5f;
                    const float halfHeight = static_cast<float>(height_) * 0.5f;
                    instance->setParameter(
                        "screenFromClip",
                        filament::math::mat4f{
                            filament::math::float4{halfWidth, 0.0f, 0.0f, 0.0f},
                            filament::math::float4{0.0f, -halfHeight, 0.0f, 0.0f},
                            filament::math::float4{0.0f, 0.0f, 1.0f, 0.0f},
                            filament::math::float4{halfWidth, halfHeight, 0.0f, 1.0f}});
                } else {
                    instance->setParameter("matrix", asMatrix(block.matrix));
                    instance->setParameter("labelPlaneMatrix",
                                           asMatrix(block.label_plane_matrix));
                }
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
            // Except on a globe, where the far side exists and has to lose. The shell is the only
            // thing that writes depth there, a whole diameter behind the near surface, so a flat
            // layer testing against it passes on the hemisphere facing the viewer and fails behind
            // it. Never writing keeps the reason this is off for a plane intact: layers resolve
            // against each other by draw order, not by the depth nudge, which translates depth
            // rather than banding it.
            instance->setDepthCulling(bent);
            if (bent) {
                instance->setDepthWrite(false);
            }
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
        // A globe scissors too, and has to. This is the device that clips a tile's *overhang* --
        // the stencil above is for ancestors, and most drawables are not stencil-clipped at all,
        // so without a box the buffer that hides seams paints straight into the neighbour. That
        // was the wedges of water lying across the map.
        //
        // The box cannot come from four corners through the drawable's matrix, which under a globe
        // reaches normalized Mercator rather than clip space and describes a curved patch rather
        // than a rectangle. So it is sampled: the tile's own grid, bent the way the vertex stage
        // bends it, and the screen box that contains the samples. A bound rather than the bend --
        // this is the third copy of that arithmetic and the only one that does not have to be
        // exact, because a box too large clips nothing and a box too small cuts a tile's edge off.
        // Hence the margin, and hence sampling the interior as well as the border: a bent patch
        // bulges away from its corners.
        if (bent) {
            constexpr int kSamples = 4;
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            bool boundable = true;
            // The patch's own extent on the sphere, as a 3D box over the samples. A grid samples
            // the patch's *interior*; the extreme screen x of a bent patch is at the silhouette,
            // which no grid line passes through. For a small patch the gap is under the margin
            // below. For a large one it is not: at z0 one tile is the whole sphere, its silhouette
            // is a quarter turn from every grid line, and the box came out narrower than the disc
            // -- so the fill was scissored away at the limb while the background, which carries no
            // box, drew there. That is the strip of land colour down each side of the planet.
            filament::math::float3 lo{1e30f, 1e30f, 1e30f};
            filament::math::float3 hi{-1e30f, -1e30f, -1e30f};
            for (int row = 0; row <= kSamples && boundable; ++row) {
                for (int column = 0; column <= kSamples; ++column) {
                    const filament::math::float4 merc =
                        transform * filament::math::float4{
                                        8192.0f * static_cast<float>(column) / kSamples,
                                        8192.0f * static_cast<float>(row) / kSamples, 0.0f, 1.0f};
                    // `globe::sphere_point_from_mercator`, as `fill_globe.mat` runs it.
                    constexpr float kPi = 3.14159265358979323846f;
                    const float lon = merc.x * 360.0f - 180.0f;
                    const float fraction = std::clamp(merc.y, 0.0f, 1.0f);
                    const float lat =
                        std::atan(std::exp((180.0f - fraction * 360.0f) * kPi / 180.0f)) * 360.0f /
                            kPi -
                        90.0f;
                    const float latR = lat * kPi / 180.0f;
                    const float lonR = lon * kPi / 180.0f;
                    const filament::math::float3 sphere{std::cos(latR) * std::sin(lonR),
                                                       -std::sin(latR),
                                                       std::cos(latR) * std::cos(lonR)};
                    // Measured on the curve the geometry is actually drawn on, which for an
                    // anchored drawable is the expansion rather than the trig.
                    //
                    // This is worth no pixels today and is still right. The two curves differ by
                    // the direct form's own `f32` error, a pixel or two by z15, and the box
                    // already carries a pixel of margin -- so nothing moved when it was measured.
                    // What makes it worth keeping is that the *mask* has the same mismatch and is
                    // not yet fixed: with the mask still on the direct bend, Monterey z15 is
                    // 0.358% against the oracle and 0.003% with the stencil off. Once the mask
                    // follows the expansion too, a box left on the trig would be the next thing
                    // in the way.
                    filament::math::float4 clip;
                    if (bendRows != nullptr) {
                        const float du =
                            8192.0f * static_cast<float>(column) / kSamples - kHalfExtent;
                        const float dv = 8192.0f * static_cast<float>(row) / kSamples - kHalfExtent;
                        clip = bendRows[0] + bendRows[1] * du + bendRows[2] * dv
                               + 0.5f * (bendRows[3] * (du * du) + bendRows[4] * (dv * dv))
                               + bendRows[5] * (du * dv);
                    } else {
                        clip = globeMatrix_ * filament::math::float4{sphere, 1.0f};
                    }
                    if (clip.w <= 0.0f) {
                        boundable = false;
                        break;
                    }
                    lo.x = std::min(lo.x, sphere.x);
                    lo.y = std::min(lo.y, sphere.y);
                    lo.z = std::min(lo.z, sphere.z);
                    hi.x = std::max(hi.x, sphere.x);
                    hi.y = std::max(hi.y, sphere.y);
                    hi.z = std::max(hi.z, sphere.z);
                    minX = std::min(minX, clip.x / clip.w);
                    maxX = std::max(maxX, clip.x / clip.w);
                    // The same Y sign the flat box is carried across with, and for the same
                    // reason: this is the producer's clip space naming a region of the screen.
                    const float screenY = (flipY_ ? -clip.y : clip.y) / clip.w;
                    minY = std::min(minY, screenY);
                    maxY = std::max(maxY, screenY);
                }
            }
            // A chord of 0.26 is a patch about fifteen degrees across, which is a tile up to
            // about z3. Past that the silhouette cannot cross the patch far enough from a grid
            // line to matter, and the margin below covers it. Under it the box is not a bound at
            // all, and a box that is not a bound must not be used: too large clips nothing, too
            // small cuts the tile's own fill off, and only one of those is recoverable.
            constexpr float kUnboundableChord = 0.26f;
            if (boundable) {
                const filament::math::float3 span = hi - lo;
                boundable = std::sqrt(span.x * span.x + span.y * span.y + span.z * span.z)
                            <= kUnboundableChord;
            }
            const auto toPixels = [](float ndc, std::uint32_t extent) {
                return (ndc * 0.5f + 0.5f) * static_cast<float>(extent);
            };
            // A pixel each way, for the sampling and for the gap between this bound and the curve
            // the vertex stage actually draws. Too large clips nothing; too small cuts a tile's
            // own edge off, which is a seam.
            const float l = std::max(0.0f, std::floor(toPixels(minX, width_)) - 1.0f);
            const float b = std::max(0.0f, std::floor(toPixels(minY, height_)) - 1.0f);
            const float r =
                std::min(static_cast<float>(width_), std::ceil(toPixels(maxX, width_)) + 1.0f);
            const float t =
                std::min(static_cast<float>(height_), std::ceil(toPixels(maxY, height_)) + 1.0f);
            if (clipped && boundable && r > l && t > b && !noScissor) {
                instance->setScissor(
                    static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(b),
                    static_cast<std::uint32_t>(r - l), static_cast<std::uint32_t>(t - b));
                scissored_++;
            } else {
                instance->unsetScissor();
            }
        } else {
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
