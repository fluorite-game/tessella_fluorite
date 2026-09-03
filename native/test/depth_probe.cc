// SPDX-License-Identifier: Apache-2.0
//
// Which quad wins: two overlapping quads at known clip depths, and a sweep over the settings that
// decide which one survives.
//
// This exists because toggling depth flags on a real frame cannot tell "clipped away" from "failed
// the test" -- both are a blank layer, and two attempts at the extrusion depth pass were spent on
// that ambiguity. Here there are two quads and nothing else, so a blank frame means clipped and a
// wrong colour means the comparison.
//
// The convention it is looking for: the *near* quad wins, whichever order the two are drawn in.
//
//   depth_probe <depth_probe.filamat>

#include <tsf/filament_renderer.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t W = 64;
constexpr std::uint32_t H = 64;

// Clip depths in the producer's own convention: nearer is *smaller*, both inside [0, 1]. These are
// the real numbers a z15 frame produces -- ground and a hundred and fifty metres up.
constexpr float FAR_Z = 0.999981f;
constexpr float NEAR_Z = 0.999773f;

struct Setting {
    const char* name;
    filament::math::mat4 projection;
};

// The perspective divisors to try: one, and the value a z15 tile matrix really produces.
constexpr float DIVISORS[] = {1.0f, 1050.0f};

const char* funcName(filament::MaterialInstance::DepthFunc f) {
    switch (f) {
        case filament::MaterialInstance::DepthFunc::LE: return "LE";
        case filament::MaterialInstance::DepthFunc::GE: return "GE";
        case filament::MaterialInstance::DepthFunc::L: return "L ";
        case filament::MaterialInstance::DepthFunc::G: return "G ";
        case filament::MaterialInstance::DepthFunc::A: return "A ";
        default: return "??";
    }
}

std::string slurp(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return {};
    std::string out;
    char chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) out.append(chunk, got);
    std::fclose(file);
    return out;
}

} // namespace

namespace {

/// What a frame may carry besides the two quads, and which the first sweep left out.
struct Scene {
    const char* name;
    bool background;   //!< A full-screen quad drawn first, as a background layer is.
    bool backgroundWritesDepth;
    float backgroundZ; //!< Its clip depth. A background layer's quad need not sit at the ground.
    bool stencil;      //!< The stencil buffer the real view enables.
    bool blendOrder;   //!< The global blend order the real consumer sets.
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <depth_probe.filamat>\n", argv[0]);
        return 2;
    }
    const std::string package = slurp(argv[1]);
    if (package.empty()) {
        std::fprintf(stderr, "probe: cannot read %s\n", argv[1]);
        return 2;
    }

    auto* engine = filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
    // Readable, and with a stencil buffer, because the real view enables one and Filament
    // refuses to render a stencil-enabled view against a swap chain without it.
    auto* swapChain = engine->createSwapChain(
        W, H,
        filament::SwapChain::CONFIG_READABLE | filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
    auto* renderer = engine->createRenderer();
    auto* view = engine->createView();
    auto cameraEntity = utils::EntityManager::get().create();
    auto* camera = engine->createCamera(cameraEntity);
    view->setCamera(camera);
    view->setViewport({0, 0, W, H});
    view->setPostProcessingEnabled(false);
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    auto* material =
        filament::Material::Builder().package(package.data(), package.size()).build(*engine);

    // One quad covering the viewport, in clip x/y. Depth comes from the material parameter.
    const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    const std::uint16_t indices[] = {0, 1, 2, 1, 3, 2};
    auto* vertices = filament::VertexBuffer::Builder()
                         .vertexCount(4)
                         .bufferCount(1)
                         .attribute(filament::VertexAttribute::POSITION, 0,
                                    filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                         .build(*engine);
    vertices->setBufferAt(*engine, 0,
                          filament::VertexBuffer::BufferDescriptor(quad, sizeof quad, nullptr));
    auto* indexBuffer = filament::IndexBuffer::Builder()
                            .indexCount(6)
                            .bufferType(filament::IndexBuffer::IndexType::USHORT)
                            .build(*engine);
    indexBuffer->setBuffer(*engine,
                           filament::IndexBuffer::BufferDescriptor(indices, sizeof indices, nullptr));

    const filament::math::mat4 passthrough{filament::math::float4{1.0, 0.0, 0.0, 0.0},
                                           filament::math::float4{0.0, -1.0, 0.0, 0.0},
                                           filament::math::float4{0.0, 0.0, 1.0, 0.0},
                                           filament::math::float4{0.0, 0.0, 0.0, 1.0}};
    // z' = w - z, which turns "nearer is smaller" into Filament's reversed Z.
    const filament::math::mat4 reversed{filament::math::float4{1.0, 0.0, 0.0, 0.0},
                                        filament::math::float4{0.0, -1.0, 0.0, 0.0},
                                        filament::math::float4{0.0, 0.0, -1.0, 0.0},
                                        filament::math::float4{0.0, 0.0, 1.0, 1.0}};
    const Setting settings[] = {{"passthrough", passthrough}, {"reversed  ", reversed}};
    const filament::MaterialInstance::DepthFunc funcs[] = {
        filament::MaterialInstance::DepthFunc::LE, filament::MaterialInstance::DepthFunc::GE,
        filament::MaterialInstance::DepthFunc::L, filament::MaterialInstance::DepthFunc::G,
        filament::MaterialInstance::DepthFunc::A};

    std::printf("%-12s %-8s %-4s %-10s %-10s %s\n", "projection", "w", "func", "near-first",
                "far-first", "verdict");
    for (const Setting& setting : settings) {
      for (float divisor : DIVISORS) {
        for (auto func : funcs) {
            std::string seen[2];
            // Both draw orders. A depth test that works gives the same answer for each; one that
            // does not is just painter order, and the two disagree.
            for (int order = 0; order < 2; order++) {
                auto* scene = engine->createScene();
                view->setScene(scene);
                camera->setCustomProjection(setting.projection, -1.0, 1.0);
                camera->setModelMatrix(filament::math::mat4f());

                std::vector<utils::Entity> entities;
                std::vector<filament::MaterialInstance*> instances;
                // red = near, blue = far.
                const struct { float z; filament::math::float4 colour; } quads[2] = {
                    {NEAR_Z, {1.0f, 0.0f, 0.0f, 1.0f}}, {FAR_Z, {0.0f, 0.0f, 1.0f, 1.0f}}};
                for (int step = 0; step < 2; step++) {
                    const auto& q = quads[order == 0 ? step : 1 - step];
                    auto* instance = material->createInstance();
                    instance->setParameter("color", q.colour);
                    instance->setParameter("clipZ", q.z);
                    instance->setParameter("clipW", divisor);
                    instance->setDepthCulling(func != filament::MaterialInstance::DepthFunc::A);
                    instance->setDepthWrite(true);
                    instance->setDepthFunc(func);
                    utils::Entity entity = utils::EntityManager::get().create();
                    filament::RenderableManager::Builder(1)
                        .boundingBox({{-1, -1, -1}, {1, 1, 1}})
                        .culling(false)
                        .material(0, instance)
                        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                                  vertices, indexBuffer, 0, 6)
                        .build(*engine, entity);
                    scene->addEntity(entity);
                    entities.push_back(entity);
                    instances.push_back(instance);
                }

                std::vector<std::uint8_t> pixels(W * H * 4);
                filament::backend::PixelBufferDescriptor pb(
                    pixels.data(), pixels.size(), filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::UBYTE);
                for (int warm = 0; warm < 2; warm++) {
                    if (renderer->beginFrame(swapChain)) {
                        renderer->render(view);
                        renderer->endFrame();
                    }
                    engine->flushAndWait();
                }
                if (renderer->beginFrame(swapChain)) {
                    renderer->render(view);
                    renderer->readPixels(0, 0, W, H, std::move(pb));
                    renderer->endFrame();
                }
                engine->flushAndWait();

                const std::size_t centre = ((H / 2) * W + W / 2) * 4;
                const std::uint8_t r = pixels[centre], g = pixels[centre + 1],
                                   b = pixels[centre + 2];
                seen[order] = r > 128 && b < 128   ? "near(red)"
                              : b > 128 && r < 128 ? "far(blue)"
                              : (r < 40 && g < 40 && b < 40) ? "blank"
                                                             : "other";
                for (auto entity : entities) {
                    scene->remove(entity);
                    engine->getRenderableManager().destroy(entity);
                    utils::EntityManager::get().destroy(entity);
                }
                engine->destroy(scene);
                // Before the material, which panics if an instance outlives it.
                for (auto* held : instances) {
                    engine->destroy(held);
                }
                engine->flushAndWait();
            }
            const bool good = seen[0] == "near(red)" && seen[1] == "near(red)";
            std::printf("%-12s %-8.0f %-4s %-10s %-10s %s\n", setting.name, divisor,
                        funcName(func), seen[0].c_str(), seen[1].c_str(),
                        good ? "<== near wins both" : "");
        }
      }
    }

    // Phase two: hold the settings phase one settled on, and add what a real frame carries.
    //
    // The same settings draw nothing on a real frame, so the difference is the scene rather than
    // the depth setup. These are the things this probe left out.
    const Scene scenes[] = {
        {"bare            ", false, false, FAR_Z, false, false},
        {"+background     ", true, false, FAR_Z, false, false},
        {"+bg writes depth", true, true, FAR_Z, false, false},
        // A background quad at the *near* end of clip space rather than the ground's depth. This
        // is the suspect: a viewport-space quad is at z 0, and 0 is the near plane in the
        // producer's convention -- so a background that writes depth there is in front of every
        // building in the frame.
        {"+bg at z=0      ", true, true, 0.0f, false, false},
        {"+bg z=0 no write", true, false, 0.0f, false, false},
        {"+stencil        ", false, false, FAR_Z, true, false},
        {"+blend order    ", false, false, FAR_Z, false, true},
        {"all             ", true, true, FAR_Z, true, true},
    };
    std::printf("\n%-17s %-10s %-10s %s\n", "scene", "near-first", "far-first", "verdict");
    for (const Scene& scene : scenes) {
        view->setStencilBufferEnabled(scene.stencil);
        std::string seen[2];
        for (int order = 0; order < 2; order++) {
            auto* sceneObj = engine->createScene();
            view->setScene(sceneObj);
            camera->setCustomProjection(passthrough, -1.0, 1.0);
            camera->setModelMatrix(filament::math::mat4f());

            std::vector<utils::Entity> entities;
            std::vector<filament::MaterialInstance*> instances;
            std::uint16_t blend = 0;
            const auto add = [&](float z, filament::math::float4 colour, bool writes) {
                auto* instance = material->createInstance();
                instance->setParameter("color", colour);
                instance->setParameter("clipZ", z);
                instance->setParameter("clipW", 1050.0f);
                instance->setDepthCulling(true);
                instance->setDepthWrite(writes);
                utils::Entity entity = utils::EntityManager::get().create();
                filament::RenderableManager::Builder builder(1);
                builder.boundingBox({{-1, -1, -1}, {1, 1, 1}})
                    .culling(false)
                    .priority(5)
                    .material(0, instance)
                    .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vertices,
                              indexBuffer, 0, 6);
                if (scene.blendOrder) {
                    builder.blendOrder(0, blend).globalBlendOrderEnabled(0, true);
                }
                blend++;
                builder.build(*engine, entity);
                sceneObj->addEntity(entity);
                entities.push_back(entity);
                instances.push_back(instance);
            };

            // A background sits at the ground's depth, which is where a background layer's quad is.
            if (scene.background) {
                add(scene.backgroundZ, {0.0f, 1.0f, 0.0f, 1.0f}, scene.backgroundWritesDepth);
            }
            if (order == 0) {
                add(NEAR_Z, {1.0f, 0.0f, 0.0f, 1.0f}, true);
                add(FAR_Z, {0.0f, 0.0f, 1.0f, 1.0f}, true);
            } else {
                add(FAR_Z, {0.0f, 0.0f, 1.0f, 1.0f}, true);
                add(NEAR_Z, {1.0f, 0.0f, 0.0f, 1.0f}, true);
            }

            std::vector<std::uint8_t> pixels(W * H * 4);
            filament::backend::PixelBufferDescriptor pb(pixels.data(), pixels.size(),
                                                        filament::backend::PixelDataFormat::RGBA,
                                                        filament::backend::PixelDataType::UBYTE);
            for (int warm = 0; warm < 2; warm++) {
                if (renderer->beginFrame(swapChain)) {
                    renderer->render(view);
                    renderer->endFrame();
                }
                engine->flushAndWait();
            }
            if (renderer->beginFrame(swapChain)) {
                renderer->render(view);
                renderer->readPixels(0, 0, W, H, std::move(pb));
                renderer->endFrame();
            }
            engine->flushAndWait();

            const std::size_t centre = ((H / 2) * W + W / 2) * 4;
            const std::uint8_t r = pixels[centre], g = pixels[centre + 1], b = pixels[centre + 2];
            seen[order] = r > 128 && b < 128        ? "near(red)"
                          : b > 128 && r < 128      ? "far(blue)"
                          : g > 128                 ? "bg(green)"
                          : (r < 40 && g < 40 && b < 40) ? "blank"
                                                         : "other";
            for (auto entity : entities) {
                sceneObj->remove(entity);
                engine->getRenderableManager().destroy(entity);
                utils::EntityManager::get().destroy(entity);
            }
            engine->destroy(sceneObj);
            for (auto* held : instances) {
                engine->destroy(held);
            }
            engine->flushAndWait();
        }
        const bool good = seen[0] == "near(red)" && seen[1] == "near(red)";
        std::printf("%-17s %-10s %-10s %s\n", scene.name, seen[0].c_str(), seen[1].c_str(),
                    good ? "near wins both" : "<== CHANGED");
    }

    engine->destroy(indexBuffer);
    engine->destroy(vertices);
    engine->destroy(material);
    engine->destroy(view);
    engine->destroy(renderer);
    engine->destroyCameraComponent(cameraEntity);
    utils::EntityManager::get().destroy(cameraEntity);
    engine->destroy(swapChain);
    filament::Engine::destroy(&engine);
    return 0;
}

// Next, when this is picked up: add a full-screen quad standing in for the background layer,
// drawn first with the flags the wire gives it (`ENABLE_DEPTH | ENABLE_COLOR`, no stencil), and
// sweep depth *write* on it separately from the two test quads. The settings above are right in
// isolation -- `passthrough` with `GE` puts the near quad in front whichever order they are drawn
// in -- and the same settings on a real frame draw nothing, so what blanks it is something else
// the frame contains. A layer that writes depth before the buildings is the first suspect, and
// this probe is the cheap place to prove it rather than another rebuild of the whole consumer.
